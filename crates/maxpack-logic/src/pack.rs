//! The console's test compression (`POST /api/pack`, a prototype that measures): a library folder game packed
//! into `<folder>.p5z` next to it by the MaxPack engine, as `maxpack pack --p5z` does on the PC. What needs no
//! PS5: the request and its names, the gate, the progress of the engine's phases, the steps of the memory probe,
//! and the lines the page copies (the `archive_id` and the SHA-256 the `.p5z` records for each volume).

use serde::Deserialize;
use serde_json::Value;
use std::collections::VecDeque;

/// The zstd levels of the packs the page offers.
pub const LEVELS: [i32; 3] = [3, 9, 19];
pub const DEFAULT_LEVEL: i32 = 9;
/// The packs' threads the page offers: the console reports 16 CPUs, but a payload runs on 5.
pub const THREADS: [u32; 2] = [4, 5];
pub const DEFAULT_THREADS: u32 = 5;
/// The loose files' threads: each long-window zstd context takes about 120 MB.
pub const LOOSE_THREADS: u32 = 1;
/// The `.p5z` is written as `<folder>.p5z.maxpack-pack` and renamed at the end: no scan takes that name for a
/// game (it is not a `.p5z` name), and a cut job leaves nothing that looks like one.
pub const PART_SUFFIX: &str = ".maxpack-pack";

/// What the page asks for.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Request {
    /// The library folder game's `path`, as `/api/state` lists it.
    pub path: String,
    pub level: i32,
    pub threads: u32,
}

/// The body of `POST /api/pack`: `{"path": "<folder>", "level": 3|9|19, "threads": 4|5}`; `level` and
/// `threads` may be left out (or null) for their defaults. Anything else in the object is ignored.
pub fn request(body: &[u8]) -> Result<Request, String> {
    let v: Value = serde_json::from_slice(body).map_err(|e| format!("not JSON: {e}"))?;
    let o = v.as_object().ok_or("not a JSON object")?;
    let path = o.get("path").and_then(Value::as_str).filter(|p| !p.is_empty()).ok_or("no path")?;
    // Compared as i64: a larger number must not wrap onto a level.
    let pick = |key: &str, choices: &[i64], default: i64| match o.get(key) {
        None | Some(Value::Null) => Ok(default),
        Some(x) => x.as_i64().filter(|n| choices.contains(n)).ok_or(format!("{key} must be one of {choices:?}")),
    };
    let level = pick("level", &LEVELS.map(i64::from), DEFAULT_LEVEL.into())?;
    let threads = pick("threads", &THREADS.map(i64::from), DEFAULT_THREADS.into())?;
    Ok(Request { path: path.to_string(), level: level as i32, threads: threads as u32 })
}

/// Where a folder's `.p5z` goes: beside it, on the same drive.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Dest {
    /// `<folder>.p5z`
    pub p5z: String,
    /// `<folder>.p5z.maxpack-pack`, the file being written.
    pub part: String,
}

/// None for a path that is not absolute, has no folder name (`/`, `/x/..`) or holds a NUL. A trailing `/`
/// is dropped.
pub fn dest(folder: &str) -> Option<Dest> {
    let f = folder.trim_end_matches('/');
    if !f.starts_with('/') || f.contains('\0') {
        return None;
    }
    let name = f.rsplit('/').next()?;
    if name.is_empty() || name == "." || name == ".." {
        return None;
    }
    let p5z = format!("{f}.p5z");
    Some(Dest { part: format!("{p5z}{PART_SUFFIX}"), p5z })
}

/// Why a test compression does not start.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Refusal {
    /// The body is not what `request` takes.
    BadRequest(String),
    /// No game of the library is listed at that path.
    NotListed,
    /// The game at that path is not a plain folder (its kind, and its packing for a packed folder).
    NotPlain(String),
    /// A test compression runs already: the engine's cancel flag and progress hook are one per process.
    PackRunning,
    /// The installer runs or queues a job: it shares them.
    InstallRunning,
    /// A game runs: it needs the memory and the processor (and the memory probe would take its share).
    GameRunning,
    /// `<folder>.p5z` is there already: it is never overwritten.
    Exists,
    /// The drive lacks this many bytes (the folder's size plus the 256 MiB margin, less what is free).
    NoSpace { missing: u64 },
}

impl Refusal {
    pub fn status(&self) -> u16 {
        match self {
            Refusal::BadRequest(_) => 400,
            Refusal::NotListed => 404,
            _ => 409,
        }
    }

    /// The page shows its own text for each code (ui.html `packFail_<code>`).
    pub fn code(&self) -> &'static str {
        match self {
            Refusal::BadRequest(_) => "bad_request",
            Refusal::NotListed => "not_found",
            Refusal::NotPlain(_) => "not_folder",
            Refusal::PackRunning => "pack_running",
            Refusal::InstallRunning => "install_running",
            Refusal::GameRunning => "game_running",
            Refusal::Exists => "exists",
            Refusal::NoSpace { .. } => "no_space",
        }
    }

    /// A short English reason, for the log and under the page's text.
    pub fn detail(&self) -> String {
        match self {
            Refusal::BadRequest(why) => format!("bad request: {why}"),
            Refusal::NotListed => "no game of the library is listed at that path".into(),
            Refusal::NotPlain(kind) => format!("not a plain folder game: {kind}"),
            Refusal::PackRunning => "a test compression is running".into(),
            Refusal::InstallRunning => "the installer is running or queuing a job".into(),
            Refusal::GameRunning => "a game is running".into(),
            Refusal::Exists => "the .p5z is there already: it is never overwritten".into(),
            Refusal::NoSpace { missing } => format!("{} more is needed on that drive", mb(*missing)),
        }
    }
}

/// What the gate looks at, all read when the request comes.
pub struct Facts<'a> {
    /// `/api/state`'s `kind` and `packing` of the library entry at the asked path (None: none listed there).
    pub entry: Option<(&'a str, Option<&'a str>)>,
    pub pack_running: bool,
    pub installer_busy: bool,
    pub game_running: bool,
    /// `<folder>.p5z` exists (or cannot be looked at: it counts as there).
    pub dest_exists: bool,
}

/// The checks before the space check, in this order: the entry, a compression or an installer job already
/// running, a game running, the destination.
pub fn gate(f: &Facts) -> Result<(), Refusal> {
    match f.entry {
        None => return Err(Refusal::NotListed),
        Some(("folder", Some("plain"))) => {}
        Some(("folder", Some(packing))) => return Err(Refusal::NotPlain(format!("folder ({packing})"))),
        Some((kind, _)) => return Err(Refusal::NotPlain(kind.to_string())),
    }
    if f.pack_running {
        return Err(Refusal::PackRunning);
    }
    if f.installer_busy {
        return Err(Refusal::InstallRunning);
    }
    if f.game_running {
        return Err(Refusal::GameRunning);
    }
    if f.dest_exists {
        return Err(Refusal::Exists);
    }
    Ok(())
}

/// The space check: the folder's `need` bytes plus `p5z::SPACE_MARGIN` must fit in `free` (a `.p5z` can be
/// as large as the folder: some games hardly compress).
pub fn space(free: u64, need: u64) -> Result<(), Refusal> {
    match crate::p5z::space_short(free, need) {
        Some(missing) => Err(Refusal::NoSpace { missing }),
        None => Ok(()),
    }
}

/// The installer requests that start a job (it shares the engine's cancel flag and progress hook): refused
/// while a test compression runs. `route` is the installer's own (`/api/install`, with any query).
pub fn blocks_installer(method: &str, route: &str) -> bool {
    method == "POST" && matches!(route.split('?').next(), Some("/api/install" | "/api/verify" | "/api/uninstall"))
}

/// The page's name of one of the engine's progress phases: `loose` ("Copying the loose files"), `packs`
/// ("Packing (zstd level N)"), `other` for anything else.
pub fn phase_key(engine_phase: &str) -> &'static str {
    if engine_phase == "Copying the loose files" {
        "loose"
    } else if engine_phase.starts_with("Packing (zstd level ") {
        "packs"
    } else {
        "other"
    }
}

/// One phase as its reports came (the engine reports at most every 100 ms, and always at the end).
#[derive(Debug, Clone, PartialEq)]
pub struct Phase {
    /// The engine's name.
    pub name: String,
    /// Its first report for the first phase (what comes before is the folder's list, read and checked);
    /// the last report of the phase before it for the others.
    pub begin_ms: u64,
    pub last_ms: u64,
    pub done: u64,
    pub total: u64,
}

impl Phase {
    pub fn ms(&self) -> u64 {
        self.last_ms.saturating_sub(self.begin_ms)
    }

    /// MB/s (10^6 bytes a second) over the phase so far; None before any time went by.
    pub fn mbps(&self) -> Option<f64> {
        let ms = self.ms();
        (ms > 0).then(|| self.done as f64 / 1e3 / ms as f64)
    }
}

/// The phases of one job.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Progress {
    /// When the engine was called.
    pub start_ms: u64,
    pub phases: Vec<Phase>,
}

impl Progress {
    pub fn new(start_ms: u64) -> Progress {
        Progress { start_ms, phases: Vec::new() }
    }

    /// One report of the engine's progress hook. The same name as the last phase, with no fewer bytes, goes
    /// on with it; anything else starts a phase.
    pub fn see(&mut self, name: &str, done: u64, total: u64, now_ms: u64) {
        if let Some(p) = self.phases.last_mut().filter(|p| p.name == name && p.done <= done) {
            p.last_ms = now_ms.max(p.last_ms);
            p.done = done;
            p.total = total;
            return;
        }
        let begin_ms = self.phases.last().map_or(now_ms, |p| p.last_ms);
        self.phases.push(Phase { name: name.to_string(), begin_ms, last_ms: now_ms.max(begin_ms), done, total });
    }

    pub fn current(&self) -> Option<&Phase> {
        self.phases.last()
    }

    /// The bytes every phase has done.
    pub fn bytes(&self) -> u64 {
        self.phases.iter().map(|p| p.done).sum()
    }

    /// The log's lines: one per phase (bytes, time, MB/s), then the time before the first phase, after the
    /// last and in all, until `end_ms`.
    pub fn summary(&self, end_ms: u64) -> Vec<String> {
        let all = secs(end_ms.saturating_sub(self.start_ms));
        let (Some(first), Some(last)) = (self.phases.first(), self.phases.last()) else {
            return vec![format!("no phase reported, in all {all}")];
        };
        let mut out: Vec<String> = self.phases.iter().map(|p| match p.mbps() {
            Some(rate) => format!("{}: {} in {}, {rate:.1} MB/s", p.name, mb(p.done), secs(p.ms())),
            None => format!("{}: {} in under 0.1 s", p.name, mb(p.done)),
        }).collect();
        out.push(format!("before the first phase (the folder's list read and checked) {}, after the last (the indexes) {}, in all {all}",
            secs(first.begin_ms.saturating_sub(self.start_ms)), secs(end_ms.saturating_sub(last.last_ms))));
        out
    }
}

/// The speed over the last seconds, from (time, bytes done in all) samples.
#[derive(Debug, Clone, Default)]
pub struct Rate {
    samples: VecDeque<(u64, u64)>,
}

/// Rate keeps this much: the longest span asked for.
const RATE_KEEP_MS: u64 = 60_000;

impl Rate {
    pub fn add(&mut self, ms: u64, bytes: u64) {
        self.samples.push_back((ms, bytes));
        while self.samples.front().is_some_and(|&(t, _)| ms.saturating_sub(t) > RATE_KEEP_MS) {
            self.samples.pop_front();
        }
    }

    /// MB/s over (at most) the last `over_ms`, from the oldest sample in that span to the newest; None with
    /// fewer than two samples there or no time between them.
    pub fn mbps(&self, over_ms: u64) -> Option<f64> {
        let &(t1, b1) = self.samples.back()?;
        let &(t0, b0) = self.samples.iter().find(|&&(t, _)| t1.saturating_sub(t) <= over_ms)?;
        (t1 > t0).then(|| b1.saturating_sub(b0) as f64 / 1e3 / (t1 - t0) as f64)
    }
}

/// The memory probe takes this much at most: the plan's target for the whole job is about 500 MB.
pub const PROBE_CAP: u64 = 1 << 30;
/// In steps of this much, each one mapped and touched before the next.
pub const PROBE_STEP: u64 = 64 << 20;
/// The system's available memory (its free pages, plus its inactive ones when the kernel says them) never goes
/// under this because of the probe: a step is mapped only when what is available just before it, less the step,
/// is still this much. Judged on that one reading before each step, whatever the probe already holds.
pub const PROBE_FLOOR: u64 = 1 << 30;
/// It stops after a step that took longer than this (memory short: the system is making room).
pub const PROBE_SLOW_MS: u64 = 2000;

/// Why the memory probe stopped.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProbeStop {
    /// It got PROBE_CAP.
    Cap,
    /// The next step's mmap failed (errno).
    MapFailed(i32),
    /// One more step would have left the system less than PROBE_FLOOR available: this much was.
    Floor(u64),
    /// The system's free memory could not be read: no step is taken without it.
    Unknown,
    /// The last step took this many ms.
    Slow(u64),
    /// A game started (or the process list could not be read to tell).
    Game,
    /// The job was cancelled meanwhile.
    Cancelled,
}

impl ProbeStop {
    pub fn text(&self) -> String {
        match self {
            ProbeStop::Cap => format!("its {} cap", mib(PROBE_CAP)),
            ProbeStop::MapFailed(errno) => format!("the next step's mmap failed (errno {errno})"),
            ProbeStop::Floor(available) => format!("the system had {} available: one more step would have left it under {}",
                mib(*available), mib(PROBE_FLOOR)),
            ProbeStop::Unknown => "the system's free memory could not be read (no step is taken without it)".into(),
            ProbeStop::Slow(ms) => format!("a step took {ms} ms"),
            ProbeStop::Game => "a game started (or the process list could not be read)".into(),
            ProbeStop::Cancelled => "the job was cancelled".into(),
        }
    }
}

/// What the probe looks at before each step.
#[derive(Debug, Clone, Copy)]
pub struct ProbeLook {
    /// The bytes it holds now.
    pub got: u64,
    /// The system's available memory read just now (free, plus inactive when known); None when it cannot be read.
    pub available: Option<u64>,
    /// The last step's time.
    pub last_step_ms: Option<u64>,
    pub cancelled: bool,
    /// A game runs, or the process list could not be read to tell.
    pub game: bool,
}

/// Whether the probe stops before its next step (then it unmaps everything at once): a cancel, a game, the cap, a
/// slow last step, unknown memory, or the floor, in that order.
pub fn probe_next(l: &ProbeLook) -> Option<ProbeStop> {
    if l.cancelled {
        return Some(ProbeStop::Cancelled);
    }
    if l.game {
        return Some(ProbeStop::Game);
    }
    if l.got >= PROBE_CAP {
        return Some(ProbeStop::Cap);
    }
    if let Some(ms) = l.last_step_ms.filter(|&ms| ms > PROBE_SLOW_MS) {
        return Some(ProbeStop::Slow(ms));
    }
    match l.available {
        None => Some(ProbeStop::Unknown),
        Some(a) if a < PROBE_FLOOR + PROBE_STEP => Some(ProbeStop::Floor(a)),
        Some(_) => None,
    }
}

/// MaxPack's own home-screen tile: its page open in the console's browser is not a game.
pub const TILE_ID: &str = "MXPK00001";

/// The title ids of the processes that count as a running game, for the test compression's gate and its log: a
/// game's title id (`titles::valid_tid`: 4 capitals and 5 digits, not a system app's `NPXS`), not MaxPack's tile;
/// sorted, each once.
pub fn running_games<'a>(titles: impl IntoIterator<Item = &'a str>) -> Vec<String> {
    let mut v: Vec<String> =
        titles.into_iter().filter(|t| crate::titles::valid_tid(t) && *t != TILE_ID).map(str::to_string).collect();
    v.sort();
    v.dedup();
    v
}

/// What the `.p5z`'s index records of its packs (ps5/pack.c reads it back from the file written): the
/// header's `archive_id` and level, and each volume's record.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct Sums {
    pub archive_id: String,
    pub level: i32,
    pub volumes: Vec<VolumeSum>,
}

/// One volume's record: its name, its size and the SHA-256 the index keeps for it (of its bytes from 64 to
/// its end: the 64-byte header has its own CRC and holds the `archive_id`).
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct VolumeSum {
    pub name: String,
    pub size: u64,
    pub sha256: String,
}

/// ps5/pack.c's JSON, checked: an `archive_id` of 32 lowercase hex digits, a level of 1 to 22, volumes named
/// `ampr_zstd-000.pak`, `ampr_zstd-001.pak`... in that order, each with 64 lowercase hex digits.
pub fn parse_sums(json: &str) -> Result<Sums, String> {
    let s: Sums = serde_json::from_str(json).map_err(|e| format!("bad sums: {e}"))?;
    let hex = |h: &str, n: usize| h.len() == n && h.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b));
    if !hex(&s.archive_id, 32) {
        return Err(format!("bad archive_id {:?}", s.archive_id));
    }
    if !(1..=22).contains(&s.level) {
        return Err(format!("bad level {}", s.level));
    }
    for (i, v) in s.volumes.iter().enumerate() {
        if v.name != format!("ampr_zstd-{i:03}.pak") {
            return Err(format!("volume {i} is named {:?}", v.name));
        }
        if !hex(&v.sha256, 64) {
            return Err(format!("{}: bad sha256 {:?}", v.name, v.sha256));
        }
    }
    Ok(s)
}

/// The lines the page shows and copies: `notes` first, each after `# ` (no compare looks at them), then
/// `level N`, `archive_id <hex>` and one `<volume> <size> <sha256>` line per volume. No path and no name:
/// a folder is often named after its game.
pub fn lines(s: &Sums, notes: &[String]) -> String {
    let mut out = String::new();
    for n in notes {
        out += &format!("# {}\n", n.replace(['\r', '\n'], " "));
    }
    out += &format!("level {}\narchive_id {}\n", s.level, s.archive_id);
    for v in &s.volumes {
        out += &format!("{} {} {}\n", v.name, v.size, v.sha256);
    }
    out
}

/// `n` bytes as MB (10^6 bytes) with one decimal.
pub fn mb(n: u64) -> String {
    // Whole tenths, cut (not rounded), so 7801.2499 MB never reads as 7801.3.
    format!("{}.{} MB", n / 1_000_000, n % 1_000_000 / 100_000)
}

/// `n` bytes as whole MiB (2^20 bytes), for memory.
pub fn mib(n: u64) -> String {
    format!("{} MiB", n >> 20)
}

/// `ms` as seconds with one decimal.
fn secs(ms: u64) -> String {
    format!("{}.{} s", ms / 1000, ms % 1000 / 100)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_request_with_its_defaults() {
        let r = request(br#"{"path":"/data/homebrew/GAME"}"#).unwrap();
        assert_eq!(r, Request { path: "/data/homebrew/GAME".into(), level: DEFAULT_LEVEL, threads: DEFAULT_THREADS });
        assert_eq!((DEFAULT_LEVEL, DEFAULT_THREADS), (9, 5));
        let r = request(br#"{"path":"/mnt/usb0/homebrew/A B","level":null,"threads":null,"other":1}"#).unwrap();
        assert_eq!((r.path.as_str(), r.level, r.threads), ("/mnt/usb0/homebrew/A B", 9, 5));
        for (level, threads) in [(3, 4), (9, 5), (19, 4), (19, 5)] {
            let body = format!(r#"{{"path":"/data/homebrew/G","level":{level},"threads":{threads}}}"#);
            let r = request(body.as_bytes()).unwrap();
            assert_eq!((r.level, r.threads), (level, threads), "{body}");
        }
    }

    #[test]
    fn a_request_out_of_its_choices_is_refused() {
        for body in [
            &br#"{"level":9}"#[..], br#"{"path":1}"#, br#"{"path":""}"#, br#"{"path":"/data/homebrew/G","level":22}"#,
            br#"{"path":"/data/homebrew/G","level":0}"#, br#"{"path":"/data/homebrew/G","level":"9"}"#,
            br#"{"path":"/data/homebrew/G","level":9.5}"#, br#"{"path":"/data/homebrew/G","threads":16}"#,
            br#"{"path":"/data/homebrew/G","threads":1}"#, br#"{"path":"/data/homebrew/G","threads":-5}"#,
            br#"{"path":"/data/homebrew/G","level":4294967305}"#, br#"["/data/homebrew/G"]"#, b"", b"path=/x",
        ] {
            assert!(request(body).is_err(), "{}", String::from_utf8_lossy(body));
        }
    }

    #[test]
    fn the_p5z_goes_beside_the_folder() {
        let d = dest("/data/homebrew/GAME").unwrap();
        assert_eq!(d.p5z, "/data/homebrew/GAME.p5z");
        assert_eq!(d.part, "/data/homebrew/GAME.p5z.maxpack-pack");
        assert_eq!(dest("/mnt/usb0/etaHEN/games/A Game [X]/").unwrap().p5z, "/mnt/usb0/etaHEN/games/A Game [X].p5z");
        // The file being written is no game name for the scan; the finished one is.
        assert!(!crate::p5z::is_p5z_name("GAME.p5z.maxpack-pack"));
        assert!(crate::images::smp_image_kind("GAME.p5z.maxpack-pack").is_none());
        assert!(crate::p5z::is_p5z_name("GAME.p5z"));
        for bad in ["", "/", "//", "relative/GAME", "GAME", "/data/..", "/data/homebrew/.", "/data/x\0y"] {
            assert_eq!(dest(bad), None, "{bad:?}");
        }
    }

    fn facts<'a>(entry: Option<(&'a str, Option<&'a str>)>) -> Facts<'a> {
        Facts { entry, pack_running: false, installer_busy: false, game_running: false, dest_exists: false }
    }

    #[test]
    fn only_a_plain_folder_with_nothing_else_running() {
        let plain = Some(("folder", Some("plain")));
        assert_eq!(gate(&facts(plain)), Ok(()));
        assert_eq!(gate(&facts(None)), Err(Refusal::NotListed));
        assert_eq!(gate(&facts(Some(("p5z", None)))), Err(Refusal::NotPlain("p5z".into())));
        assert_eq!(gate(&facts(Some(("pkg", None)))), Err(Refusal::NotPlain("pkg".into())));
        assert_eq!(gate(&facts(Some(("folder", Some("zstd"))))), Err(Refusal::NotPlain("folder (zstd)".into())));
        assert_eq!(gate(&facts(Some(("folder", Some("lz4"))))), Err(Refusal::NotPlain("folder (lz4)".into())));
        assert_eq!(gate(&Facts { pack_running: true, ..facts(plain) }), Err(Refusal::PackRunning));
        assert_eq!(gate(&Facts { installer_busy: true, ..facts(plain) }), Err(Refusal::InstallRunning));
        assert_eq!(gate(&Facts { game_running: true, ..facts(plain) }), Err(Refusal::GameRunning));
        assert_eq!(gate(&Facts { dest_exists: true, ..facts(plain) }), Err(Refusal::Exists));
    }

    #[test]
    fn the_first_reason_wins() {
        let all = |entry| Facts { entry, pack_running: true, installer_busy: true, game_running: true, dest_exists: true };
        assert_eq!(gate(&all(None)), Err(Refusal::NotListed));
        assert_eq!(gate(&all(Some(("pkg", None)))), Err(Refusal::NotPlain("pkg".into())));
        let plain = Some(("folder", Some("plain")));
        assert_eq!(gate(&all(plain)), Err(Refusal::PackRunning));
        assert_eq!(gate(&Facts { pack_running: false, ..all(plain) }), Err(Refusal::InstallRunning));
        assert_eq!(gate(&Facts { pack_running: false, installer_busy: false, ..all(plain) }), Err(Refusal::GameRunning));
    }

    #[test]
    fn the_folder_and_the_margin_must_fit() {
        let margin = crate::p5z::SPACE_MARGIN;
        assert_eq!(space(10 << 30, 1 << 30), Ok(()));
        assert_eq!(space((1 << 30) + margin, 1 << 30), Ok(()));
        assert_eq!(space((1 << 30) + margin - 1, 1 << 30), Err(Refusal::NoSpace { missing: 1 }));
        assert_eq!(space(0, 5 << 30), Err(Refusal::NoSpace { missing: (5 << 30) + margin }));
        assert_eq!(space(u64::MAX, u64::MAX), Ok(()));
    }

    #[test]
    fn refusals_have_a_status_a_code_and_a_detail() {
        let all = [
            (Refusal::BadRequest("no path".into()), 400, "bad_request"),
            (Refusal::NotListed, 404, "not_found"),
            (Refusal::NotPlain("pkg".into()), 409, "not_folder"),
            (Refusal::PackRunning, 409, "pack_running"),
            (Refusal::InstallRunning, 409, "install_running"),
            (Refusal::GameRunning, 409, "game_running"),
            (Refusal::Exists, 409, "exists"),
            (Refusal::NoSpace { missing: 3 << 30 }, 409, "no_space"),
        ];
        for (r, status, code) in all {
            assert_eq!((r.status(), r.code()), (status, code), "{r:?}");
            assert!(!r.detail().is_empty(), "{r:?}");
        }
        assert!(Refusal::BadRequest("no path".into()).detail().contains("no path"));
        assert!(Refusal::NotPlain("folder (zstd)".into()).detail().contains("folder (zstd)"));
        assert!(Refusal::NoSpace { missing: 3 << 30 }.detail().contains("3221.2 MB"));
    }

    #[test]
    fn installer_jobs_wait_for_the_compression() {
        for r in ["/api/install", "/api/verify", "/api/uninstall", "/api/install?x=1"] {
            assert!(blocks_installer("POST", r), "{r}");
        }
        for (m, r) in [("GET", "/api/state"), ("GET", "/api/icon?p5z=x"), ("POST", "/api/scan"), ("POST", "/api/probe"),
            ("POST", "/api/pause"), ("POST", "/api/discard"), ("GET", "/api/install"), ("POST", "/api/installx")] {
            assert!(!blocks_installer(m, r), "{m} {r}");
        }
    }

    #[test]
    fn the_engine_phases_by_name() {
        assert_eq!(phase_key("Copying the loose files"), "loose");
        for l in [1, 3, 9, 19, 22] {
            assert_eq!(phase_key(&format!("Packing (zstd level {l})")), "packs");
        }
        assert_eq!(phase_key("comprobando"), "other");
        assert_eq!(phase_key(""), "other");
    }

    #[test]
    fn phases_from_the_reports() {
        let mut p = Progress::new(1_000);
        assert_eq!(p.current(), None);
        assert_eq!(p.bytes(), 0);
        // The first phase starts at its first report: before it, the folder's list was read and checked.
        p.see("Copying the loose files", 4 << 20, 100_000_000, 3_000);
        p.see("Copying the loose files", 50_000_000, 100_000_000, 3_500);
        p.see("Copying the loose files", 100_000_000, 100_000_000, 4_000);
        let loose = p.current().unwrap().clone();
        assert_eq!((loose.begin_ms, loose.last_ms, loose.done, loose.total), (3_000, 4_000, 100_000_000, 100_000_000));
        assert_eq!(loose.ms(), 1_000);
        assert_eq!(loose.mbps(), Some(100.0));
        // The next one starts where the last report of the one before it was.
        p.see("Packing (zstd level 9)", 1_000_000, 1_000_000_000, 6_000);
        p.see("Packing (zstd level 9)", 1_000_000_000, 1_000_000_000, 14_000);
        assert_eq!(p.phases.len(), 2);
        let packs = &p.phases[1];
        assert_eq!((packs.begin_ms, packs.ms()), (4_000, 10_000));
        assert_eq!(packs.mbps(), Some(100.0));
        assert_eq!(p.bytes(), 1_100_000_000);
        // One report that ended the phase at once: no time, no speed.
        let mut q = Progress::new(0);
        q.see("Copying the loose files", 10, 10, 50);
        assert_eq!(q.current().unwrap().ms(), 0);
        assert_eq!(q.current().unwrap().mbps(), None);
    }

    #[test]
    fn a_name_again_or_fewer_bytes_start_a_phase() {
        let mut p = Progress::new(0);
        p.see("Packing (zstd level 9)", 500, 1000, 10);
        p.see("Packing (zstd level 9)", 400, 1000, 20);
        assert_eq!(p.phases.len(), 2);
        p.see("Copying the loose files", 5, 5, 30);
        p.see("Packing (zstd level 9)", 1000, 1000, 40);
        assert_eq!(p.phases.len(), 4);
        assert_eq!(p.phases[3].begin_ms, 30);
    }

    #[test]
    fn the_summary_for_the_log() {
        let mut p = Progress::new(1_000);
        p.see("Copying the loose files", 4_000_000, 100_000_000, 3_000);
        p.see("Copying the loose files", 100_000_000, 100_000_000, 4_000);
        p.see("Packing (zstd level 9)", 2_000_000_000, 2_000_000_000, 24_000);
        let s = p.summary(25_500);
        assert_eq!(s, vec![
            "Copying the loose files: 100.0 MB in 1.0 s, 100.0 MB/s".to_string(),
            "Packing (zstd level 9): 2000.0 MB in 20.0 s, 100.0 MB/s".to_string(),
            "before the first phase (the folder's list read and checked) 2.0 s, after the last (the indexes) 1.5 s, in all 24.5 s".to_string(),
        ]);
        // A phase too short to time says so; no phase at all says that.
        let mut q = Progress::new(0);
        q.see("Copying the loose files", 10, 10, 50);
        assert_eq!(q.summary(60)[0], "Copying the loose files: 0.0 MB in under 0.1 s");
        assert_eq!(Progress::new(0).summary(2_000), vec!["no phase reported, in all 2.0 s".to_string()]);
    }

    #[test]
    fn the_speed_over_the_last_seconds() {
        let mut r = Rate::default();
        assert_eq!(r.mbps(10_000), None);
        r.add(0, 0);
        assert_eq!(r.mbps(10_000), None);
        r.add(1_000, 50_000_000);
        assert_eq!(r.mbps(10_000), Some(50.0));
        r.add(11_000, 150_000_000);
        // Over the last 10 s: from the sample at 1 s to the one at 11 s.
        assert_eq!(r.mbps(10_000), Some(10.0));
        assert_eq!(r.mbps(60_000), Some(150.0 / 11.0));
        // Samples older than a minute go.
        r.add(100_000, 150_000_000);
        assert_eq!(r.mbps(60_000), None);
        r.add(101_000, 160_000_000);
        assert_eq!(r.mbps(60_000), Some(10.0));
    }

    #[test]
    fn the_probe_maps_a_step_only_above_the_floor() {
        assert_eq!((PROBE_CAP, PROBE_STEP, PROBE_FLOOR), (1 << 30, 64 << 20, 1 << 30));
        let look = |got, available| ProbeLook { got, available, last_step_ms: Some(30), cancelled: false, game: false };
        assert_eq!(probe_next(&look(0, Some(PROBE_FLOOR + PROBE_STEP))), None);
        let short = PROBE_FLOOR + PROBE_STEP - 1;
        assert_eq!(probe_next(&look(0, Some(short))), Some(ProbeStop::Floor(short)));
        // Each step on its own reading, however much the probe holds: late in the probe as at its start.
        assert_eq!(probe_next(&look(PROBE_CAP - PROBE_STEP, Some(PROBE_FLOOR + PROBE_STEP))), None);
        assert_eq!(probe_next(&look(PROBE_CAP - PROBE_STEP, Some(50 << 20))), Some(ProbeStop::Floor(50 << 20)));
        assert_eq!(probe_next(&look(PROBE_STEP, Some(0))), Some(ProbeStop::Floor(0)));
        // Memory that cannot be read: no step at all, first or later.
        assert_eq!(probe_next(&look(0, None)), Some(ProbeStop::Unknown));
        assert_eq!(probe_next(&look(256 << 20, None)), Some(ProbeStop::Unknown));
    }

    #[test]
    fn the_probe_stops_at_its_cap_a_slow_step_a_game_or_a_cancel() {
        let look = |got, last_step_ms, cancelled, game| ProbeLook { got, available: Some(64 << 30), last_step_ms, cancelled, game };
        assert_eq!(probe_next(&look(0, None, false, false)), None);
        assert_eq!(probe_next(&look(PROBE_CAP - PROBE_STEP, Some(30), false, false)), None);
        assert_eq!(probe_next(&look(PROBE_CAP, Some(30), false, false)), Some(ProbeStop::Cap));
        assert_eq!(probe_next(&look(128 << 20, Some(PROBE_SLOW_MS + 1), false, false)), Some(ProbeStop::Slow(PROBE_SLOW_MS + 1)));
        assert_eq!(probe_next(&look(128 << 20, Some(PROBE_SLOW_MS), false, false)), None);
        assert_eq!(probe_next(&look(128 << 20, Some(30), false, true)), Some(ProbeStop::Game));
        assert_eq!(probe_next(&look(128 << 20, Some(30), true, false)), Some(ProbeStop::Cancelled));
        // A cancel, then a game, win over everything else, unknown memory included.
        let all = ProbeLook { got: PROBE_CAP, available: None, last_step_ms: Some(9999), cancelled: true, game: true };
        assert_eq!(probe_next(&all), Some(ProbeStop::Cancelled));
        assert_eq!(probe_next(&ProbeLook { cancelled: false, ..all }), Some(ProbeStop::Game));
        assert_eq!(probe_next(&ProbeLook { cancelled: false, game: false, ..all }), Some(ProbeStop::Cap));
    }

    #[test]
    fn probe_stops_say_why() {
        assert_eq!(ProbeStop::Cap.text(), "its 1024 MiB cap");
        assert_eq!(ProbeStop::MapFailed(12).text(), "the next step's mmap failed (errno 12)");
        assert_eq!(ProbeStop::Floor(1100 << 20).text(),
            "the system had 1100 MiB available: one more step would have left it under 1024 MiB");
        assert_eq!(ProbeStop::Unknown.text(), "the system's free memory could not be read (no step is taken without it)");
        assert_eq!(ProbeStop::Slow(2500).text(), "a step took 2500 ms");
        assert_eq!(ProbeStop::Game.text(), "a game started (or the process list could not be read)");
        assert_eq!(ProbeStop::Cancelled.text(), "the job was cancelled");
    }

    #[test]
    fn which_processes_count_as_a_running_game() {
        let found = ["NPXS40140", "PPSA01234", "MXPK00001", "CUSA00001", "PPSA01234", "", "ppsa01234", "PPSA0123", "PPSA012345"];
        assert_eq!(running_games(found), vec!["CUSA00001".to_string(), "PPSA01234".to_string()]);
        assert!(running_games([]).is_empty());
        // The system's apps and MaxPack's own page are not games.
        assert!(running_games(["NPXS20001", "NPXS40028", TILE_ID]).is_empty());
    }

    const AID: &str = "2388a49fde20d12c2322289c96ecdc5d";
    const SHA0: &str = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const SHA1: &str = "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";

    fn sums_json(aid: &str, level: i32, vols: &[(&str, u64, &str)]) -> String {
        let v: Vec<Value> = vols.iter().map(|(n, s, h)| serde_json::json!({"name": n, "size": s, "sha256": h})).collect();
        serde_json::json!({"archive_id": aid, "level": level, "volumes": v}).to_string()
    }

    #[test]
    fn sums_as_the_p5z_records_them() {
        let s = parse_sums(&sums_json(AID, 9, &[("ampr_zstd-000.pak", 562490, SHA0), ("ampr_zstd-001.pak", 338202, SHA1)])).unwrap();
        assert_eq!(s.archive_id, AID);
        assert_eq!(s.level, 9);
        assert_eq!(s.volumes.len(), 2);
        assert_eq!(s.volumes[1], VolumeSum { name: "ampr_zstd-001.pak".into(), size: 338202, sha256: SHA1.into() });
        // A game with nothing packed: no volumes, and the header's zeros.
        let none = parse_sums(&sums_json("00000000000000000000000000000000", 19, &[])).unwrap();
        assert!(none.volumes.is_empty());
    }

    #[test]
    fn sums_that_do_not_hold_together_are_refused() {
        let v0 = ("ampr_zstd-000.pak", 1u64, SHA0);
        for bad in [
            sums_json(&AID.to_uppercase(), 9, &[v0]),
            sums_json(&AID[..30], 9, &[v0]),
            sums_json(AID, 0, &[v0]),
            sums_json(AID, 23, &[v0]),
            sums_json(AID, 9, &[("ampr_zstd-001.pak", 1, SHA0)]),
            sums_json(AID, 9, &[v0, ("ampr_zstd-000.pak", 1, SHA1)]),
            sums_json(AID, 9, &[("ampr_zstd-0.pak", 1, SHA0)]),
            sums_json(AID, 9, &[("../x", 1, SHA0)]),
            sums_json(AID, 9, &[("ampr_zstd-000.pak", 1, &SHA0[..63])]),
            sums_json(AID, 9, &[("ampr_zstd-000.pak", 1, &SHA0.to_uppercase())]),
            r#"{"archive_id":"x"}"#.to_string(),
            "not json".to_string(),
        ] {
            assert!(parse_sums(&bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn the_lines_to_compare() {
        let s = parse_sums(&sums_json(AID, 9, &[("ampr_zstd-000.pak", 562490, SHA0), ("ampr_zstd-001.pak", 338202, SHA1)])).unwrap();
        let text = lines(&s, &["MaxPack 1.0.2 on the console, engine b3b526f".into(), "packs: 127.2 MB/s".into()]);
        assert_eq!(text, format!(
            "# MaxPack 1.0.2 on the console, engine b3b526f\n# packs: 127.2 MB/s\nlevel 9\narchive_id {AID}\n\
             ampr_zstd-000.pak 562490 {SHA0}\nampr_zstd-001.pak 338202 {SHA1}\n"));
        // A note never runs into a line of its own.
        let t = lines(&s, &["two\nlines".into()]);
        assert!(t.starts_with("# two lines\nlevel 9\n"), "{t}");
    }

    #[test]
    fn sizes_for_the_log() {
        assert_eq!(mb(0), "0.0 MB");
        assert_eq!(mb(152_340_000), "152.3 MB");
        assert_eq!(mb(7_801_249_999), "7801.2 MB");
        assert_eq!(mib(0), "0 MiB");
        assert_eq!(mib(412 << 20), "412 MiB");
        assert_eq!(mib((412 << 20) + 1), "412 MiB");
    }
}
