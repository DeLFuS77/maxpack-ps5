//! The console's test compression (`POST /api/pack`, a prototype that measures; DESIGN.md "Test compression"):
//! a plain folder game of the library packed into `<folder>.p5z` beside it by the MaxPack engine (ps5/pack.c:
//! mp_pack_run with its .p5z output, as `maxpack pack --p5z` on the PC), on a thread of its own. One job at a
//! time, never beside an installer job (the engine's progress hook and cancel flag are one per process), never
//! started while any game runs. Before the first job of a run, a probe of how much memory the payload gets (once
//! per run, never taking the system's available memory under PROBE_FLOOR); during it, the phases' MB/s, the
//! payload's peak memory and what a game started meanwhile does to the speed, all in the log; after it, the
//! `archive_id` and each volume's SHA-256 as the `.p5z` records them, for the page (maxpack_logic::pack::lines).
use crate::web::State;
use maxpack_logic::pack::{self as logic, Dest, ProbeLook, ProbeStop, Progress, Rate, Refusal, Request};
use std::collections::HashSet;
use std::ffi::{c_char, c_int, c_uint, c_void, CStr, CString};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

/// ps5/pack.c's counts (its own struct, not the engine's).
#[repr(C)]
#[derive(Default)]
struct Counts {
    loose_files: u64,
    loose_bytes: u64,
    packed_files: u64,
    packed_bytes: u64,
    stored_bytes: u64,
    volumes: u64,
}

type ProgressFn = extern "C" fn(*mut c_void, *const c_char, u64, u64);

extern "C" {
    // ps5/pack.c: 0, or -1 with the reason in err (the engine deleted what it wrote of out).
    fn maxpack_pack_p5z(src: *const c_char, out: *const c_char, level: c_int, threads: c_uint, loose_threads: c_uint,
        progress: Option<ProgressFn>, ctx: *mut c_void, counts: *mut Counts, err: *mut c_char, errlen: usize) -> c_int;
    fn maxpack_pack_cancel();
    fn maxpack_pack_cancel_reset();
    // malloc'd JSON (maxpack_free), NULL with the reason in err.
    fn maxpack_p5z_sums(path: *const c_char, err: *mut c_char, errlen: usize) -> *mut c_char;
    fn maxpack_free(p: *mut c_void);
}

/// The engine commit vendor/maxpack was copied from (tools/sync_maxpack.sh).
const ENGINE_COMMIT: &str = include_str!("../vendor/maxpack/COMMIT");
const ERR_LEN: usize = 512;
/// The monitor's look at memory, the speed and the games MaxPack started.
const TICK_MS: u64 = 250;
/// Every this many ticks (2 s) the monitor also walks the process list for any game.
const WALK_TICKS: u64 = 8;
/// A progress line in the log this often, and one while a game runs.
const EVERY_MS: u64 = 60_000;
const GAME_EVERY_MS: u64 = 30_000;
/// The longest a quit waits for a cancelled job to delete its file (web::take_port retries for 5 s).
const STOP_WAIT_MS: u64 = 3000;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Stage {
    Probe,
    Engine,
    Sums,
    Done,
    Failed,
    Cancelled,
}

impl Stage {
    fn running(self) -> bool {
        matches!(self, Stage::Probe | Stage::Engine | Stage::Sums)
    }

    fn name(self) -> &'static str {
        match self {
            Stage::Probe => "probe",
            Stage::Engine => "running",
            Stage::Sums => "sums",
            Stage::Done => "done",
            Stage::Failed => "failed",
            Stage::Cancelled => "cancelled",
        }
    }
}

/// The job of this run (the last one stays for the page until another starts).
struct Job {
    req: Request,
    dest: Dest,
    stage: Stage,
    cancel: bool,
    started_ms: u64,
    ended_ms: Option<u64>,
    /// Fed by the engine's progress hook (on_progress).
    progress: Progress,
    error: Option<String>,
    lines: Option<String>,
    size: Option<u64>,
    /// The `.maxpack-pack` file, when a failed or cancelled job left it (the engine normally deletes it).
    left: Option<String>,
    /// Why a failed job failed, for the page's own words: `sums` (the written file's sums did not read back).
    code: Option<&'static str>,
}

static JOB: Mutex<Option<Job>> = Mutex::new(None);
/// The memory probe of this run: it runs before the first job only, and the later ones say its result again.
static PROBED: Mutex<Option<Probe>> = Mutex::new(None);
/// Held while a job is started (installer idle checked, the job set) and while the web forwards an
/// installer request that starts a job (hold_off), so neither slips in beside the other.
static GATE: Mutex<()> = Mutex::new(());

fn job() -> MutexGuard<'static, Option<Job>> {
    JOB.lock().unwrap_or_else(|e| e.into_inner())
}

/// A test compression runs (its probe, the engine or the reading of its sums).
pub fn running() -> bool {
    job().as_ref().is_some_and(|j| j.stage.running())
}

/// For an installer request that starts a job (maxpack_logic::pack::blocks_installer): None while a test
/// compression runs; else a guard to hold while the request is forwarded.
pub fn hold_off() -> Option<MutexGuard<'static, ()>> {
    let g = GATE.lock().unwrap_or_else(|e| e.into_inner());
    (!running()).then_some(g)
}

/// A running job reads or writes at or under `path` (a folder or a drive's mount): delete and eject wait.
pub fn uses(path: &Path) -> bool {
    job().as_ref().is_some_and(|j| {
        j.stage.running() && (Path::new(&j.req.path).starts_with(path) || path.starts_with(&j.req.path))
    })
}

/// POST /api/pack: checks the request (maxpack_logic::pack::gate, then the space) and starts the job on its own
/// thread. Err with why it does not start.
pub fn start(state: &State, body: &[u8]) -> Result<(), Refusal> {
    let req = logic::request(body).map_err(Refusal::BadRequest)?;
    let dest = logic::dest(&req.path).ok_or_else(|| Refusal::BadRequest(format!("not a folder path: {:?}", req.path)))?;
    let entry = state.games.lock().unwrap().iter().find(|g| g.src.path == Path::new(&req.path)).map(|g| crate::scan::kind_names(g.src.kind));
    let _gate = GATE.lock().unwrap_or_else(|e| e.into_inner());
    // Anything but "not there" counts as there: it is never overwritten.
    let dest_exists = !matches!(std::fs::symlink_metadata(&dest.p5z), Err(e) if e.kind() == std::io::ErrorKind::NotFound);
    // Any game, whoever started it (`paused` only knows MaxPack's); a walk that failed refuses too.
    let games = crate::launch::games_running();
    let game_running = maxpack_logic::inst::game_blocks(state.paused(), games.as_ref().ok().map(|g| !g.is_empty()));
    let facts = logic::Facts {
        entry, pack_running: running(), installer_busy: !crate::installer::idle(), game_running, dest_exists,
    };
    if let Err(r) = logic::gate(&facts) {
        let why = match (&r, &games) {
            (Refusal::GameRunning, Ok(g)) if !g.is_empty() => format!(" ({})", g.join(", ")),
            (Refusal::GameRunning, Err(e)) => format!(" (the process list could not be read: {e})"),
            _ => String::new(),
        };
        log!("pack: {} refused: {}{why}", req.path, r.detail());
        return Err(r);
    }
    // A file of a job this payload no longer runs (it quit or crashed mid-way): MaxPack's own name for one.
    match std::fs::remove_file(&dest.part) {
        Ok(()) => log!("pack: removed {}, left by a job that did not end", dest.part),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => log!("pack: remove {}: {e} (the engine will refuse it)", dest.part),
    }
    let need = crate::cache::du(Path::new(&req.path));
    let parent = Path::new(&dest.p5z).parent().map_or("/".into(), |p| p.to_string_lossy().into_owned());
    let free = crate::storage::free(&parent).unwrap_or(0);
    // The internal drive (the user partition) keeps 2 GiB; a FAT32 drive takes no folder of 4 GiB or more
    // (maxpack_logic::pack::room), before anything is written.
    let fstype = crate::storage::fs_type(&parent).unwrap_or_default();
    let drive = logic::Drive { internal: crate::storage::drive_of(Path::new(&req.path)) == "/data", fat32: logic::is_fat32(&fstype) };
    if let Err(r) = logic::room(free, need, drive) {
        log!("pack: {} refused: {} (the folder is {}, {} free on {parent}, {fstype})", req.path, r.detail(), logic::mb(need),
            logic::mb(free));
        return Err(r);
    }
    let now = crate::sys::uptime_ms();
    *job() = Some(Job {
        req: req.clone(), dest, stage: Stage::Probe, cancel: false, started_ms: now, ended_ms: None,
        progress: Progress::new(now), error: None, lines: None, size: None, left: None, code: None,
    });
    let running = state.running.clone();
    log!("pack: {} ({} in its files, {} free there): zstd level {}, {} threads for the packs, {} for the loose files",
        req.path, logic::mb(need), logic::mb(free), req.level, req.threads, logic::LOOSE_THREADS);
    // A thread the system refuses ends the job as failed, never leaves it running.
    if let Err(e) = std::thread::Builder::new().name("pack".into()).spawn(move || run(running, need)) {
        log!("pack: could not start the job's thread: {e}");
        end(Stage::Failed, Some(format!("could not start the job's thread: {e}")));
    }
    Ok(())
}

/// POST /api/pack/cancel: false when no job runs. The engine stops at its next check and deletes its file.
pub fn cancel() -> bool {
    let mut g = job();
    let Some(j) = g.as_mut().filter(|j| j.stage.running()) else { return false };
    if !j.cancel {
        j.cancel = true;
        log!("pack: cancel requested ({})", j.stage.name());
        if j.stage == Stage::Engine {
            // SAFETY: sets the engine's atomic flag; under JOB, so never after the job's reset.
            unsafe { maxpack_pack_cancel() };
        }
    }
    true
}

/// Before the payload quits: a running job is cancelled and waited for (STOP_WAIT_MS at most) so it deletes its
/// file; one that has not ended by then leaves its `.maxpack-pack` file, which the next job there removes.
pub fn stop() {
    if !cancel() {
        return;
    }
    let deadline = crate::sys::uptime_ms() + STOP_WAIT_MS;
    while running() && crate::sys::uptime_ms() < deadline {
        std::thread::sleep(Duration::from_millis(50));
    }
    if running() {
        log!("pack: still stopping after {} ms; quitting anyway", STOP_WAIT_MS);
    }
}

/// `/api/state` `pack`: the job of this run, or null.
pub fn json() -> serde_json::Value {
    let g = job();
    let Some(j) = g.as_ref() else { return serde_json::Value::Null };
    let now = crate::sys::uptime_ms();
    let phase = j.progress.current();
    serde_json::json!({
        "state": j.stage.name(),
        "cancelling": j.cancel && j.stage.running(),
        "source": j.req.path,
        "dest": j.dest.p5z,
        "level": j.req.level,
        "threads": j.req.threads,
        "phase": phase.map(|p| logic::phase_key(&p.name)),
        "done": phase.map_or(0, |p| p.done),
        "total": phase.map_or(0, |p| p.total),
        "mbps": phase.and_then(|p| p.mbps()),
        "elapsed_ms": j.ended_ms.unwrap_or(now).saturating_sub(j.started_ms),
        "error": j.error,
        "lines": j.lines,
        "size": j.size,
        "left": j.left,
        // Why it failed, for the page's own words ("sums"), else null.
        "code": j.code,
    })
}

/// The engine's progress hook (from its threads, with its phase lock held): into the job's Progress.
extern "C" fn on_progress(_ctx: *mut c_void, phase: *const c_char, done: u64, total: u64) {
    if phase.is_null() {
        return;
    }
    // SAFETY: the engine passes a NUL-terminated phase name that lives for the call.
    let name = unsafe { CStr::from_ptr(phase) }.to_string_lossy();
    if let Some(j) = job().as_mut() {
        j.progress.see(&name, done, total, crate::sys::uptime_ms());
    }
}

fn cstr(s: &str) -> Option<CString> {
    CString::new(s).ok()
}

fn err_text(buf: &[c_char]) -> String {
    let bytes: Vec<u8> = buf.iter().take_while(|&&c| c != 0).map(|&c| c as u8).collect();
    let s = String::from_utf8_lossy(&bytes).into_owned();
    if s.is_empty() { "no reason given".into() } else { s }
}

fn end(stage: Stage, error: Option<String>) {
    if let Some(j) = job().as_mut() {
        j.stage = stage;
        j.error = error;
        j.ended_ms = Some(crate::sys::uptime_ms());
    }
}

/// Ends a job whose thread returns without having ended it (as failed), so it never stays "running".
struct Unfinished;

impl Drop for Unfinished {
    fn drop(&mut self) {
        if running() {
            end(Stage::Failed, Some("the job's thread ended without a result".into()));
        }
    }
}

/// A game runs now, whoever started it, or the process list cannot be read to tell.
fn game_now(running: &Mutex<HashSet<String>>) -> bool {
    !running.lock().unwrap_or_else(|e| e.into_inner()).is_empty()
        || !matches!(crate::launch::games_running(), Ok(g) if g.is_empty())
}

/// The job's thread: the probe (the first job of a run only), the engine, the rename, the sums.
fn run(running: Arc<Mutex<HashSet<String>>>, need: u64) {
    let _unfinished = Unfinished;
    let Some((req, dest, t0)) = job().as_ref().map(|j| (j.req.clone(), j.dest.clone(), j.started_ms)) else { return };
    let (Some(src), Some(part)) = (cstr(&req.path), cstr(&dest.part)) else {
        end(Stage::Failed, Some("NUL in the path".into()));
        return;
    };
    let before = rss();
    let earlier = PROBED.lock().unwrap_or_else(|e| e.into_inner()).clone();
    let reused = earlier.is_some();
    let probe = match earlier {
        Some(p) => {
            log!("pack: memory probe: not run again (once per MaxPack start); the first one got {}, stopped by {}",
                logic::mib(p.got), p.stop.text());
            p
        }
        None => {
            let p = probe(&|| job().as_ref().is_some_and(|j| j.cancel), &|| game_now(&running));
            let after = rss();
            log!("pack: memory probe: {} in steps of {}, each touched, then released, in {} ms; stopped by {}; \
                  the system's available memory ({}) {} before, {} at its lowest; the payload's RSS {} before, {} after",
                logic::mib(p.got), logic::mib(logic::PROBE_STEP), p.ms, p.stop.text(),
                if p.inactive { "free + inactive" } else { "free" }, opt_mib(p.available_start), opt_mib(p.available_low),
                opt_mib(before), opt_mib(after));
            *PROBED.lock().unwrap_or_else(|e| e.into_inner()) = Some(p.clone());
            p
        }
    };
    // The engine starts only if no cancel came meanwhile; the flag is cleared first (no job of the engine runs).
    let start_ms = {
        let mut g = job();
        let Some(j) = g.as_mut() else { return };
        if j.cancel {
            drop(g);
            end(Stage::Cancelled, Some("cancelled".into()));
            log!("pack: cancelled before the engine started");
            return;
        }
        // SAFETY: clears the engine's atomic flag; nothing of the engine uses it now (installer jobs wait).
        unsafe { maxpack_pack_cancel_reset() };
        j.stage = Stage::Engine;
        let now = crate::sys::uptime_ms();
        j.progress = Progress::new(now);
        now
    };
    let stop = Arc::new(AtomicBool::new(false));
    // Without its thread the job goes on, with no peak memory and no game lines.
    let mon = {
        let stop = stop.clone();
        std::thread::Builder::new().name("pack-monitor".into()).spawn(move || monitor(running, stop))
            .map_err(|e| log!("pack: could not start the monitor's thread: {e}; no peak memory or game lines this job"))
            .ok()
    };
    let mut counts = Counts::default();
    let mut err = [0 as c_char; ERR_LEN];
    // SAFETY: NUL-terminated paths, our counts and err buffer, all outliving the call; on_progress is
    // thread-safe (it takes JOB) and the engine drops the hook before it returns.
    let rc = unsafe {
        maxpack_pack_p5z(src.as_ptr(), part.as_ptr(), req.level, req.threads, logic::LOOSE_THREADS, Some(on_progress),
            std::ptr::null_mut(), &mut counts, err.as_mut_ptr(), err.len())
    };
    let engine_end = crate::sys::uptime_ms();
    stop.store(true, Ordering::SeqCst);
    let watch = mon.and_then(|m| m.join().ok()).unwrap_or_default();
    let (cancelled, summary) = {
        let mut g = job();
        // SAFETY: under JOB, so a cancel asked after this finds the job no longer in Engine.
        unsafe { maxpack_pack_cancel_reset() };
        let Some(j) = g.as_mut() else { return };
        j.stage = Stage::Sums;
        (j.cancel, j.progress.summary(engine_end))
    };
    for line in &summary {
        log!("pack: {line}");
    }
    let memory = format!("the payload's RSS at its peak {}, {} before the job (sampled every {TICK_MS} ms)",
        opt_mib(watch.peak), opt_mib(before));
    log!("pack: {memory}");
    if watch.game_ms > 0 {
        log!("pack: a game ran during {} s of the job", watch.game_ms / 1000);
    }
    if rc != 0 {
        let why = err_text(&err);
        let stage = if cancelled && why == "cancelled" { Stage::Cancelled } else { Stage::Failed };
        // The engine's writer deletes a .p5z it did not finish; this only says whether it is gone (the page says
        // "nothing is left" only then).
        let left = Path::new(&dest.part).exists();
        let still = if left { format!("; {} is still there (the next job of this folder removes it)", dest.part) } else { String::new() };
        log!("pack: {} after {} s in the engine: {why}{still}", if stage == Stage::Cancelled { "cancelled" } else { "failed" },
            (engine_end - start_ms) / 1000);
        if let Some(j) = job().as_mut() {
            j.left = left.then(|| dest.part.clone());
        }
        end(stage, Some(if stage == Stage::Failed { format!("{why}{still}") } else { why }));
        return;
    }
    log!("pack: {} loose files ({}), {} packed files ({} -> {}) in {} volumes",
        counts.loose_files, logic::mb(counts.loose_bytes), counts.packed_files, logic::mb(counts.packed_bytes),
        logic::mb(counts.stored_bytes), counts.volumes);
    // Its sums read back before it gets its game name (maxpack_logic::pack::finish): a file the engine's reader
    // refuses never stays as <folder>.p5z, and is deleted; a Cancel during the sums is honoured the same way.
    let size = std::fs::metadata(&dest.part).map(|m| m.len()).ok();
    let finished = logic::finish(&mut Files { part: &dest.part, p5z: &dest.p5z });
    let end_ms = crate::sys::uptime_ms();
    let sums = match finished {
        Ok(s) => s,
        Err(f) => {
            // What the delete of the file did, for the log, and the file when it is still there.
            let deleted = |d: &Result<(), String>| match d {
                Ok(()) => (format!("deleted {}", dest.part), None),
                Err(e) => (format!("delete {}: {e} (the next job of this folder removes it)", dest.part), Some(dest.part.clone())),
            };
            let (code, why, left) = match f {
                logic::FinishFail::Sums { why, delete } => {
                    let (what, left) = deleted(&delete);
                    (Some("sums"), format!("the .p5z was written, but its sums could not be read: {why}; {what}"), left)
                }
                logic::FinishFail::Cancelled { delete } => {
                    let (what, left) = deleted(&delete);
                    log!("pack: cancelled while reading the sums; {what}");
                    if let Some(j) = job().as_mut() {
                        j.left = left;
                    }
                    end(Stage::Cancelled, Some("cancelled".into()));
                    return;
                }
                logic::FinishFail::Taken => {
                    let why = format!("the .p5z was written, but {} appeared meanwhile: it stays as {}", dest.p5z, dest.part);
                    (None, why, Some(dest.part.clone()))
                }
                logic::FinishFail::Rename(e) => {
                    let left = Path::new(&dest.part).exists().then(|| dest.part.clone());
                    (None, format!("the .p5z was written, but rename {} to {}: {e}", dest.part, dest.p5z), left)
                }
            };
            log!("pack: {why}");
            if let Some(j) = job().as_mut() {
                (j.left, j.code) = (left, code);
            }
            end(Stage::Failed, Some(why));
            return;
        }
    };
    let commit = ENGINE_COMMIT.trim();
    let mut notes = vec![
        format!("MaxPack {} on the console, engine {}; lines that start with # are notes", env!("CARGO_PKG_VERSION"),
            &commit[..commit.len().min(7)]),
        format!("zstd level {}, {} threads for the packs, {} for the loose files", req.level, req.threads, logic::LOOSE_THREADS),
    ];
    notes.extend(summary.iter().cloned());
    notes.push(format!("the .p5z {} of the folder's {}, in all {} s with the reading of these lines",
        size.map_or("?".into(), logic::mb), logic::mb(need), (end_ms - t0) / 1000));
    notes.push(format!("memory probe {} (stopped by {}{}); {memory}", logic::mib(probe.got), probe.stop.text(),
        if reused { ", run once at this MaxPack start's first job" } else { "" }));
    if watch.game_ms > 0 {
        notes.push(format!("a game ran during {} s of the job", watch.game_ms / 1000));
    }
    let lines = logic::lines(&sums, &notes);
    log!("pack: done: {} (archive_id {}, {} volumes)", dest.p5z, sums.archive_id, sums.volumes.len());
    for v in &sums.volumes {
        log!("pack: {} {} {}", v.name, v.size, v.sha256);
    }
    if let Some(j) = job().as_mut() {
        j.lines = Some(lines);
        j.size = size;
    }
    end(Stage::Done, None);
}

/// The files at the end of a job (maxpack_logic::pack::finish): the `.maxpack-pack` file and its game name.
struct Files<'a> {
    part: &'a str,
    p5z: &'a str,
}

impl logic::Finish for Files<'_> {
    /// On the disk before it is read back and named: the engine closes it without a flush, and a power cut after
    /// the rename must not leave a .p5z whose index is there and some of whose volumes are not.
    fn flush_part(&mut self) {
        if let Err(e) = std::fs::File::open(self.part).and_then(|f| f.sync_all()) {
            log!("pack: fsync {}: {e} (going on)", self.part);
        }
    }

    fn sums(&mut self) -> Result<logic::Sums, String> {
        read_sums(self.part)
    }

    /// "Cancel the compression" during the sums (cancel() sets the flag in every running stage).
    fn cancelled(&mut self) -> bool {
        job().as_ref().is_some_and(|j| j.cancel)
    }

    fn delete_part(&mut self) -> Result<(), String> {
        match std::fs::remove_file(self.part) {
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(e.to_string()),
            _ => Ok(()),
        }
    }

    /// Renamed only onto nothing. ponytail: checked, then renamed; a file put there in between is replaced
    /// (rename(2) has no no-replace flag here, and exFAT has no link(2) to do it with).
    fn dest_taken(&mut self) -> bool {
        !matches!(std::fs::symlink_metadata(self.p5z), Err(e) if e.kind() == std::io::ErrorKind::NotFound)
    }

    fn rename(&mut self) -> Result<(), String> {
        std::fs::rename(self.part, self.p5z).map_err(|e| e.to_string())
    }

    fn flush_dir(&mut self) {
        if let Some(dir) = Path::new(self.p5z).parent() {
            if let Err(e) = std::fs::File::open(dir).and_then(|d| d.sync_all()) {
                log!("pack: fsync {}: {e} (the rename may not be on the disk yet)", dir.display());
            }
        }
    }
}

fn read_sums(p5z: &str) -> Result<logic::Sums, String> {
    let c = cstr(p5z).ok_or("NUL in the path")?;
    let mut err = [0 as c_char; ERR_LEN];
    // SAFETY: a NUL-terminated path and an err buffer that outlive the call.
    let json = unsafe { maxpack_p5z_sums(c.as_ptr(), err.as_mut_ptr(), err.len()) };
    if json.is_null() {
        return Err(err_text(&err));
    }
    // SAFETY: NUL-terminated JSON the engine allocated; freed right after the copy.
    let text = unsafe { CStr::from_ptr(json) }.to_string_lossy().into_owned();
    unsafe { maxpack_free(json.cast()) };
    logic::parse_sums(&text)
}

fn opt_mib(n: Option<u64>) -> String {
    n.map_or("unknown".into(), logic::mib)
}

/// What the memory probe found.
#[derive(Clone)]
struct Probe {
    got: u64,
    stop: ProbeStop,
    ms: u64,
    /// The system's available memory before the first step and at its lowest before a step.
    available_start: Option<u64>,
    available_low: Option<u64>,
    /// It counted the inactive pages too (the kernel said them).
    inactive: bool,
}

/// How much memory the payload gets, up to logic::PROBE_CAP: anonymous mappings of PROBE_STEP each, every page
/// written to (so the memory is really there). Before each step it reads the system's available memory (free
/// pages, plus inactive ones when readable) and maps the step only when that less the step stays at least
/// PROBE_FLOOR (maxpack_logic::pack::probe_next): judged on that reading alone, so whatever the kernel takes the
/// pages from, the probe never takes it under the floor; memory it cannot read means no step at all. It also stops
/// at the cap, at the first mapping that fails, after a step slower than PROBE_SLOW_MS, at a cancel, and when a
/// game runs (`game`: MaxPack's launches and the process list, a walk that fails counting as a game); then every
/// mapping is unmapped at once. mmap, not the allocator: a failure here is a clean ENOMEM and leaves the heap as
/// it was. The caller runs it once per payload run.
fn probe(cancelled: &dyn Fn() -> bool, game: &dyn Fn() -> bool) -> Probe {
    let t0 = crate::sys::uptime_ms();
    let step = logic::PROBE_STEP as usize;
    // Room for every mapping up front.
    let mut maps: Vec<*mut c_void> = Vec::with_capacity((logic::PROBE_CAP / logic::PROBE_STEP) as usize + 1);
    let page = 4096;
    let first = available_memory();
    let (available_start, inactive) = (first.map(|a| a.0), first.is_some_and(|a| a.1));
    let mut available_low = available_start;
    let mut last = None;
    let stop = loop {
        let available = available_memory().map(|a| a.0);
        available_low = match (available_low, available) {
            (Some(a), Some(b)) => Some(a.min(b)),
            (a, b) => a.or(b),
        };
        let look = ProbeLook {
            got: maps.len() as u64 * logic::PROBE_STEP, available, last_step_ms: last, cancelled: cancelled(), game: game(),
        };
        if let Some(stop) = logic::probe_next(&look) {
            break stop;
        }
        let t = crate::sys::uptime_ms();
        // SAFETY: a fresh private anonymous mapping, unmapped below.
        let p = unsafe {
            libc::mmap(std::ptr::null_mut(), step, libc::PROT_READ | libc::PROT_WRITE, libc::MAP_ANON | libc::MAP_PRIVATE, -1, 0)
        };
        if p == libc::MAP_FAILED {
            break ProbeStop::MapFailed(std::io::Error::last_os_error().raw_os_error().unwrap_or(0));
        }
        maps.push(p);
        for off in (0..step).step_by(page) {
            // SAFETY: inside the mapping just made, which is writable.
            unsafe { std::ptr::write_volatile(p.cast::<u8>().add(off), 1) };
        }
        last = Some(crate::sys::uptime_ms() - t);
    };
    let got = maps.len() as u64 * logic::PROBE_STEP;
    for p in maps {
        // SAFETY: each one a mapping of `step` bytes made above.
        unsafe { libc::munmap(p, step) };
    }
    Probe { got, stop, ms: crate::sys::uptime_ms() - t0, available_start, available_low, inactive }
}

/// The system's available memory: its free pages (`vm.stats.vm.v_free_count`), plus its inactive ones
/// (`v_inactive_count`, the cache the kernel can hand out) when that can be read (then true). None when the free
/// pages cannot be read.
fn available_memory() -> Option<(u64, bool)> {
    let page = page_size()?;
    let free = sysctl_u64(b"vm.stats.vm.v_free_count")?;
    let inactive = sysctl_u64(b"vm.stats.vm.v_inactive_count");
    Some(((free + inactive.unwrap_or(0)) * page, inactive.is_some()))
}

fn page_size() -> Option<u64> {
    // SAFETY: a plain query.
    let n = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    (n > 0).then_some(n as u64)
}

/// A numeric sysctl by name, through sysctl.name2oid ({0, 3}) and sysctl(2) only: sysctlbyname may not be in
/// the SDK's libc. A 4- or 8-byte value.
fn sysctl_u64(name: &[u8]) -> Option<u64> {
    let mut oid = [0 as c_int; 24];
    let mut len = std::mem::size_of_val(&oid);
    let q = [0 as c_int, 3];
    // SAFETY: the buffers and their lengths are ours; name is given with its length (no NUL needed).
    let rc = unsafe {
        libc::sysctl(q.as_ptr(), 2, oid.as_mut_ptr().cast(), &mut len, name.as_ptr().cast(), name.len())
    };
    if rc != 0 || len == 0 || len % 4 != 0 {
        return None;
    }
    let mut v = [0u8; 8];
    let mut vlen = v.len();
    // SAFETY: as above.
    let rc = unsafe {
        libc::sysctl(oid.as_ptr(), (len / 4) as c_uint, v.as_mut_ptr().cast(), &mut vlen, std::ptr::null(), 0)
    };
    match (rc, vlen) {
        (0, 4) => Some(u32::from_ne_bytes([v[0], v[1], v[2], v[3]]) as u64),
        (0, 8) => Some(u64::from_ne_bytes(v)),
        _ => None,
    }
}

/// `ki_rssize` (pages) of this process's kinfo_proc: FreeBSD 11's layout, which the PS5 keeps (the pid at 72 and
/// the command at 447 that launch.rs reads are of the same layout; libc puts ki_rssize at 264 in it).
const KINFO_RSSIZE_OFFSET: usize = std::mem::offset_of!(libc::kinfo_proc, ki_rssize);

/// The payload's resident memory now (sysctl KERN_PROC_PID), None when it cannot be read.
fn rss() -> Option<u64> {
    // SAFETY: a plain query.
    let pid = unsafe { libc::getpid() };
    let mib = [libc::CTL_KERN, libc::KERN_PROC, libc::KERN_PROC_PID, pid];
    let mut len = 0usize;
    // SAFETY: a size query, then our buffer and its length.
    if unsafe { libc::sysctl(mib.as_ptr(), 4, std::ptr::null_mut(), &mut len, std::ptr::null(), 0) } != 0 {
        return None;
    }
    let mut buf = vec![0u8; len.max(KINFO_RSSIZE_OFFSET + 8)];
    len = buf.len();
    let rc = unsafe { libc::sysctl(mib.as_ptr(), 4, buf.as_mut_ptr().cast(), &mut len, std::ptr::null(), 0) };
    if rc != 0 || len < KINFO_RSSIZE_OFFSET + 8 {
        return None;
    }
    let size = i32::from_ne_bytes(buf[..4].try_into().ok()?) as usize;
    if size < KINFO_RSSIZE_OFFSET + 8 || size > len {
        return None;
    }
    let pages = i64::from_ne_bytes(buf[KINFO_RSSIZE_OFFSET..KINFO_RSSIZE_OFFSET + 8].try_into().ok()?);
    Some(u64::try_from(pages).ok()? * page_size()?)
}

/// What the monitor saw while the engine ran.
#[derive(Default)]
struct Watch {
    peak: Option<u64>,
    game_ms: u64,
}

/// Beside the engine: the payload's RSS every TICK_MS (its peak), the speed, and a game that starts or exits
/// (the job goes on: this prototype measures what it does), seen in MaxPack's own launches every tick and in the
/// process list every WALK_TICKS (any game, whoever started it; a walk that fails keeps the last one, logged once);
/// a progress line every EVERY_MS, and every GAME_EVERY_MS while a game runs.
fn monitor(running: Arc<Mutex<HashSet<String>>>, stop: Arc<AtomicBool>) -> Watch {
    let mut w = Watch::default();
    let mut rate = Rate::default();
    let mut games: Vec<String> = Vec::new();
    let (mut game_since, mut game_bytes) = (0u64, 0u64);
    let mut next_line = crate::sys::uptime_ms() + EVERY_MS;
    let mut next_game_line = 0;
    let (mut walked, mut walk_failed, mut tick) = (Vec::<String>::new(), false, 0u64);
    while !stop.load(Ordering::SeqCst) {
        std::thread::sleep(Duration::from_millis(TICK_MS));
        if tick % WALK_TICKS == 0 {
            match crate::launch::games_running() {
                Ok(g) => (walked, walk_failed) = (g, false),
                Err(e) if !walk_failed => {
                    log!("pack: the process list could not be read ({e}); the last one stands until it can");
                    walk_failed = true;
                }
                Err(_) => {}
            }
        }
        tick += 1;
        let now = crate::sys::uptime_ms();
        let rss = rss();
        w.peak = w.peak.max(rss);
        let (bytes, at) = match job().as_ref() {
            Some(j) => (j.progress.bytes(), where_at(&j.progress)),
            None => (0, String::new()),
        };
        rate.add(now, bytes);
        let speed = |over: u64| rate.mbps(over).map_or("?".into(), |r| format!("{r:.1}"));
        let mut now_games: Vec<String> = running.lock().unwrap_or_else(|e| e.into_inner()).iter().cloned().collect();
        now_games.extend(walked.iter().cloned());
        now_games.sort();
        now_games.dedup();
        if now_games != games {
            if games.is_empty() {
                log!("pack: a game started while compressing ({}): {at}, {} MB/s over the 10 s before; the job goes on",
                    now_games.join(", "), speed(10_000));
                (game_since, game_bytes) = (now, bytes);
                next_game_line = now + GAME_EVERY_MS;
            } else if now_games.is_empty() {
                let ms = now.saturating_sub(game_since);
                w.game_ms += ms;
                let during = if ms > 0 { format!("{:.1}", bytes.saturating_sub(game_bytes) as f64 / 1e3 / ms as f64) } else { "?".into() };
                log!("pack: the game exited ({}): {at}, {during} MB/s while it ran ({} s)", games.join(", "), ms / 1000);
            } else {
                log!("pack: the games running are now {}: {at}", now_games.join(", "));
            }
            games = now_games;
        }
        if !games.is_empty() && now >= next_game_line {
            log!("pack: a game runs ({}): {at}, {} MB/s over the last 30 s, RSS {}", games.join(", "), speed(30_000), opt_mib(rss));
            next_game_line = now + GAME_EVERY_MS;
        }
        if now >= next_line {
            log!("pack: {at}, {} MB/s over the last minute, RSS {}", speed(60_000), opt_mib(rss));
            next_line = now + EVERY_MS;
        }
    }
    if !games.is_empty() {
        w.game_ms += crate::sys::uptime_ms().saturating_sub(game_since);
    }
    w
}

/// "<phase> NN % (done of total)" for the log.
fn where_at(p: &Progress) -> String {
    match p.current() {
        Some(ph) => {
            let pct = if ph.total > 0 { ph.done as f64 * 100.0 / ph.total as f64 } else { 100.0 };
            format!("{} {pct:.0} % ({} of {})", ph.name, logic::mb(ph.done), logic::mb(ph.total))
        }
        None => "before the first phase".into(),
    }
}
