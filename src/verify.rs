//! "Verify fully" on the game sheet of a library `.p5z` (`POST /api/verify/<TID>`; DESIGN.md "Verify on the
//! console"): the MaxPack engine's own full verify (ps5/verify.c: mp_verify_p5z, what `maxpack verify` runs on the
//! PC), on a thread of its own with THREADS threads, its progress, speed and cancel in `/api/state` `verify`. One
//! job of the engine at a time (src/jobs.rs), never started while any game runs. What it found goes to
//! `/data/maxpack/verified.json` (maxpack_logic::verify::Store, by path): the sheet and the report show it while
//! the file is the one verified.
use crate::web::State;
use maxpack_logic::job::Rate;
use maxpack_logic::verify::{self as logic, Facts, Kind, Record, Refusal, Store};
use std::ffi::{c_char, c_int, c_uint, c_void, CString};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard};
use std::time::Duration;

/// ps5/verify.c's counts (its own struct, not the engine's).
#[repr(C)]
#[derive(Default)]
pub struct Counts {
    pub files_ok: u64,
    pub files_bad: u64,
    pub bytes_checked: u64,
}

pub type ProgressFn = extern "C" fn(*mut c_void, *const c_char, u64, u64);

extern "C" {
    // ps5/verify.c: 0 (counts filled), or -1 with the reason in err ("cancelled" after maxpack_verify_cancel).
    fn maxpack_verify_full(p5z: *const c_char, threads: c_uint, progress: Option<ProgressFn>, ctx: *mut c_void,
        counts: *mut Counts, err: *mut c_char, errlen: usize) -> c_int;
    fn maxpack_verify_cancel();
    fn maxpack_verify_cancel_reset();
}

/// The engine's threads for a verify: as the test compression found, 5 is what the payload gets of the console.
pub const THREADS: u32 = 5;
const STORE_PATH: &str = "/data/maxpack/verified.json";
const ERR_LEN: usize = 512;
/// The longest a quit waits for a cancelled verify to end (web::take_port retries for 5 s).
const STOP_WAIT_MS: u64 = 3000;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Stage {
    Running,
    Done,
    Failed,
    Cancelled,
}

impl Stage {
    fn name(self) -> &'static str {
        match self {
            Stage::Running => "running",
            Stage::Done => "done",
            Stage::Failed => "failed",
            Stage::Cancelled => "cancelled",
        }
    }
}

/// The verify of this run (the last one stays for the page until another starts).
struct Job {
    tid: String,
    path: PathBuf,
    kind: Kind,
    stage: Stage,
    cancel: bool,
    /// The engine's call runs: a cancel sets its flag only then.
    engine: bool,
    started_ms: u64,
    ended_ms: Option<u64>,
    /// Fed by the engine's progress hook: bytes of the files checked so far, of all.
    done: u64,
    total: u64,
    rate: Rate,
    error: Option<String>,
}

static JOB: Mutex<Option<Job>> = Mutex::new(None);
/// verified.json, read once.
static STORE: Mutex<Option<Store>> = Mutex::new(None);

fn job() -> MutexGuard<'static, Option<Job>> {
    JOB.lock().unwrap_or_else(|e| e.into_inner())
}

/// A verify runs now.
pub fn running() -> bool {
    job().as_ref().is_some_and(|j| j.stage == Stage::Running)
}

/// A running verify reads at or under `path` (the `.p5z`, or a drive's mount): delete and eject wait.
pub fn uses(path: &Path) -> bool {
    job().as_ref().is_some_and(|j| j.stage == Stage::Running && j.path.starts_with(path))
}

fn store<R>(f: impl FnOnce(&mut Store) -> R) -> R {
    let mut g = STORE.lock().unwrap_or_else(|e| e.into_inner());
    let s = g.get_or_insert_with(|| match std::fs::read_to_string(STORE_PATH) {
        Ok(t) => logic::parse(&t),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Store::new(),
        Err(e) => {
            log!("verify: read {STORE_PATH}: {e}; no earlier results");
            Store::new()
        }
    });
    f(s)
}

/// (size, mtime) of the file at `path` now.
fn identity(path: &Path) -> Option<(u64, i64)> {
    std::fs::metadata(path).ok().map(|m| (m.len(), m.mtime()))
}

/// The last verify of the `.p5z` at `path` while it is the file verified (maxpack_logic::verify::current).
pub fn last(path: &Path) -> Option<Record> {
    let (size, mtime) = identity(path)?;
    store(|s| logic::current(s, &path.to_string_lossy(), size, mtime).cloned())
}

/// Records what a verify found, and writes verified.json (temp + rename); a failed write is logged, the record
/// stays for this run.
pub fn remember(path: &Path, rec: Record) {
    let text = store(|s| {
        logic::put(s, &path.to_string_lossy(), rec);
        logic::render(s)
    });
    let tmp = format!("{STORE_PATH}.tmp");
    if let Err(e) = std::fs::write(&tmp, text).and_then(|_| std::fs::rename(&tmp, STORE_PATH)) {
        log!("verify: write {STORE_PATH}: {e} (the result stays until MaxPack stops)");
    }
}

/// POST /api/verify/<TID>, body `full`: checks the request (maxpack_logic::verify::gate) and starts the verify on
/// its own thread. Err with why it does not start.
pub fn start(state: &State, tid: &str, body: &[u8]) -> Result<(), Refusal> {
    let kind = logic::kind(body).ok_or(Refusal::BadRequest)?;
    let src = crate::scan::valid_tid(tid).then(|| state.source(tid)).flatten();
    let _gate = crate::jobs::gate();
    // Any game, whoever started it (`paused` only knows MaxPack's); a walk that failed refuses too.
    let game = crate::jobs::game_running(state);
    let facts = Facts {
        p5z: src.as_ref().map(|s| s.kind == crate::scan::Kind::P5z), busy: crate::jobs::busy(), game_running: game.is_some(),
    };
    if let Err(r) = logic::gate(&facts) {
        let why = match (r, &game) {
            (Refusal::GameRunning, Some(g)) => format!(" ({g})"),
            _ => String::new(),
        };
        log!("{tid}: verify refused: {}{why}", r.detail());
        return Err(r);
    }
    let Some(src) = src else { return Err(Refusal::NotFound) };
    let now = crate::sys::uptime_ms();
    *job() = Some(Job {
        tid: tid.to_string(), path: src.path.clone(), kind, stage: Stage::Running, cancel: false, engine: false, started_ms: now,
        ended_ms: None, done: 0, total: 0, rate: Rate::default(), error: None,
    });
    log!("{tid}: verify ({}) of {} with {THREADS} threads", kind.name(), src.path.display());
    // A thread the system refuses ends the job as failed, never leaves it running.
    if let Err(e) = std::thread::Builder::new().name("verify".into()).spawn(run) {
        log!("{tid}: verify: could not start its thread: {e}");
        end(Stage::Failed, Some(format!("could not start the job's thread: {e}")));
    }
    Ok(())
}

/// POST /api/verify/cancel: false when no verify runs. The engine stops at its next check.
pub fn cancel() -> bool {
    let mut g = job();
    let Some(j) = g.as_mut().filter(|j| j.stage == Stage::Running) else { return false };
    if !j.cancel {
        j.cancel = true;
        log!("{}: verify: cancel requested", j.tid);
        // Under JOB and only while the engine runs, so the flag is never set after the job's reset (run).
        if j.engine {
            engine_cancel();
        }
    }
    true
}

/// Before the payload quits: a running verify is cancelled and waited for (STOP_WAIT_MS at most).
pub fn stop() {
    if !cancel() {
        return;
    }
    let deadline = crate::sys::uptime_ms() + STOP_WAIT_MS;
    while running() && crate::sys::uptime_ms() < deadline {
        std::thread::sleep(Duration::from_millis(50));
    }
}

/// `/api/state` `verify`: the verify of this run, or null.
pub fn json() -> serde_json::Value {
    let g = job();
    let Some(j) = g.as_ref() else { return serde_json::Value::Null };
    let now = crate::sys::uptime_ms();
    serde_json::json!({
        "state": j.stage.name(),
        "cancelling": j.cancel && j.stage == Stage::Running,
        "title_id": j.tid,
        "path": j.path.to_string_lossy(),
        "kind": j.kind.name(),
        "done": j.done,
        "total": j.total,
        "mbps": j.rate.mbps(10_000),
        "elapsed_ms": j.ended_ms.unwrap_or(now).saturating_sub(j.started_ms),
        "error": j.error,
    })
}

/// A record's JSON for the game sheet (`verified` in `/api/state`).
pub fn record_json(r: &Record) -> serde_json::Value {
    serde_json::json!({
        "kind": r.kind, "at": r.at, "ok": r.ok, "files_ok": r.files_ok, "files_bad": r.files_bad,
        "bad": r.bad.iter().map(|b| serde_json::json!({"what": b.what, "name": b.name})).collect::<Vec<_>>(),
    })
}

/// The engine's progress hook (from its threads, with its phase lock held): into the job.
extern "C" fn on_progress(_ctx: *mut c_void, _phase: *const c_char, done: u64, total: u64) {
    if let Some(j) = job().as_mut() {
        (j.done, j.total) = (done, total);
        j.rate.add(crate::sys::uptime_ms(), done);
    }
}

pub fn err_text(buf: &[c_char]) -> String {
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

/// Unix seconds by the console's clock.
pub fn now_secs() -> u64 {
    // SAFETY: a plain query.
    let t = unsafe { libc::time(std::ptr::null_mut()) };
    u64::try_from(t).unwrap_or(0)
}

/// The engine's full verify of `path` (ps5/verify.c), its progress to `progress`. Ok(counts) when it could check,
/// Err(reason) when not ("cancelled" after `engine_cancel`). The caller holds the console's job (src/jobs.rs), and
/// clears the engine's cancel flag after it (`reset_cancel`, under the lock its cancel takes), so a cancel that
/// comes after the call is not left set for the installer.
pub fn engine_full(path: &Path, progress: ProgressFn) -> Result<Counts, String> {
    let c = CString::new(path.as_os_str().as_bytes()).map_err(|_| "NUL in the path".to_string())?;
    let mut counts = Counts::default();
    let mut err = [0 as c_char; ERR_LEN];
    // SAFETY: a NUL-terminated path, our counts and err buffer, all outliving the call; the hook is thread-safe
    // and the engine drops it before it returns.
    let rc = unsafe {
        maxpack_verify_full(c.as_ptr(), THREADS, Some(progress), std::ptr::null_mut(), &mut counts, err.as_mut_ptr(), err.len())
    };
    if rc != 0 {
        return Err(err_text(&err));
    }
    Ok(counts)
}

/// Clears the engine's cancel flag: before an engine call (none runs then) and after it.
pub fn reset_cancel() {
    // SAFETY: clears the engine's atomic flag; nothing of the engine runs a job now (src/jobs.rs).
    unsafe { maxpack_verify_cancel_reset() };
}

/// Asks the running engine call to stop (its next check).
pub fn engine_cancel() {
    // SAFETY: sets the engine's atomic flag.
    unsafe { maxpack_verify_cancel() };
}

/// The job's thread: the engine, then the record.
fn run() {
    let _unfinished = Unfinished;
    let Some((tid, path, kind, t0)) = job().as_ref().map(|j| (j.tid.clone(), j.path.clone(), j.kind, j.started_ms)) else { return };
    // The engine starts only if no cancel came meanwhile; the flag is cleared first.
    {
        let mut g = job();
        let Some(j) = g.as_mut() else { return };
        if j.cancel {
            drop(g);
            end(Stage::Cancelled, None);
            log!("{tid}: verify cancelled before it started");
            return;
        }
        reset_cancel();
        j.engine = true;
    }
    let before = identity(&path);
    let r = engine_full(&path, on_progress);
    let cancelled = {
        // Under JOB: a cancel asked after this finds the engine done, so the flag is never left set.
        let mut g = job();
        reset_cancel();
        g.as_mut().is_some_and(|j| {
            j.engine = false;
            j.cancel
        })
    };
    let secs = (crate::sys::uptime_ms() - t0) / 1000;
    let counts = match r {
        Ok(c) => c,
        Err(why) => {
            let stage = if cancelled && why == "cancelled" { Stage::Cancelled } else { Stage::Failed };
            log!("{tid}: verify {} after {secs} s: {why}", if stage == Stage::Cancelled { "cancelled" } else { "failed" });
            end(stage, (stage == Stage::Failed).then_some(why));
            return;
        }
    };
    let ok = counts.files_bad == 0;
    log!("{tid}: verify ({}) of {}: {} in {secs} s: {} files OK, {} failed ({} MB checked)", kind.name(), path.display(),
        if ok { "OK" } else { "damaged" }, counts.files_ok, counts.files_bad, counts.bytes_checked / 1_000_000);
    // Recorded against the file as it was when the verify started: one that changed meanwhile shows none.
    match (before, identity(&path)) {
        (Some((size, mtime)), Some(now)) if now == (size, mtime) => {
            remember(&path, Record {
                size, mtime, kind: kind.name().into(), at: now_secs(), ok, files_ok: counts.files_ok,
                files_bad: counts.files_bad, bad: Vec::new(),
            });
        }
        _ => log!("{tid}: {} changed or went during the verify: its result is not kept", path.display()),
    }
    end(Stage::Done, None);
}
