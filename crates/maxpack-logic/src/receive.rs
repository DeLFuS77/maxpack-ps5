//! A `.p5z` received from MaxPack on the PC (src/receive.rs; DESIGN.md "Receive from the PC"): every decision of
//! the `/api/receive/*` routes that needs no console. The pairing code and the lockout of wrong codes, the file
//! name, the drive, where the bytes go and where a resume starts, the order of the refusals, and the answers.
use crate::job::Busy;
use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::{HashMap, VecDeque};
use std::net::IpAddr;

/// What a file being received is called until it checks out: `<name>.maxpack-recv` beside where it goes.
pub const PARTIAL_SUFFIX: &str = ".maxpack-recv";
/// The longest name taken, in bytes (with the suffix it stays within exFAT's 255 characters).
pub const MAX_NAME: usize = 200;
/// Free space kept on the drive beyond what is still to come.
pub const MARGIN: u64 = 256 << 20;
/// The most bytes one `PUT /api/receive/data` carries.
pub const MAX_PUT: u64 = 64 << 20;
/// FAT32 keeps no file of this size or more.
pub const FAT32_LIMIT: u64 = 4 << 30;
/// The smallest .p5z: its 64-byte header and 32-byte footer.
pub const MIN_SIZE: u64 = 96;
/// Wrong codes in a row from one address before it is locked out, and for how long.
pub const MAX_WRONG: u32 = 5;
pub const LOCK_MS: u64 = 60_000;
/// Wrong codes from all addresses within LOCK_MS before every address is locked out (many addresses guessing).
pub const GLOBAL_WRONG: usize = 20;
/// Addresses with wrong codes remembered at most; more at once than that locks every address out.
pub const MAX_ADDRESSES: usize = 256;
/// An address whose last wrong code is older than this (and that is not locked out) is forgotten.
pub const FORGET_MS: u64 = 30 * 60_000;
/// A receive with nothing from the PC this long pauses: it keeps its file and no longer holds the other jobs.
pub const IDLE_MS: u64 = 10 * 60_000;
/// The most of a refused body read and dropped, so the PC reads the answer rather than a reset.
pub const MAX_DRAIN: u64 = 4 << 20;

/// Where a receive is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum State {
    Receiving,
    Verifying,
    Done,
    Failed,
    Cancelled,
    /// Stopped with its file kept, and why. It keeps its token while the payload runs: its status says
    /// `paused` with the reason, data and finish answer 409 `paused`, and a start of the same file goes on.
    Paused(PauseReason),
}

/// Why a receive paused (the status's `reason`). The PC goes on by itself after `idle` and `write_error`, and
/// never after `stopped`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PauseReason {
    /// Nothing from the PC for IDLE_MS.
    Idle,
    /// A write or a flush of what arrived failed (a full drive, FAT32, the drive gone).
    WriteError,
    /// "Stop (keep what arrived)" on the console, or the payload quitting during the verify.
    Stopped,
    /// The PC stopped the send and kept what arrived, and said so (`POST /api/receive/leave`).
    Left,
}

impl PauseReason {
    pub fn name(self) -> &'static str {
        match self {
            PauseReason::Idle => "idle",
            PauseReason::WriteError => "write_error",
            PauseReason::Stopped => "stopped",
            PauseReason::Left => "left",
        }
    }
}

impl State {
    pub fn name(self) -> &'static str {
        match self {
            State::Receiving => "receiving",
            State::Verifying => "verifying",
            State::Done => "done",
            State::Failed => "failed",
            State::Cancelled => "cancelled",
            State::Paused(_) => "paused",
        }
    }

    /// Under way: it keeps every other job of the engine waiting (job::Busy::Receive).
    pub fn active(self) -> bool {
        matches!(self, State::Receiving | State::Verifying)
    }
}

/// Why a request of `/api/receive/*` is refused: its status, its code (the contract's) and its answer.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Refusal {
    /// The switch is off (Settings > "Receive games from the PC").
    Off,
    /// No `X-MaxPack-Code`, or not the code.
    BadCode,
    /// Too many wrong codes: for this many more milliseconds.
    Locked(u64),
    /// A body or query that does not read.
    BadRequest(String),
    BadName,
    BadSize,
    NoDrive,
    Fat32,
    Exists,
    Busy(Busy),
    /// Bytes short on the drive, the margin included (`need`), and the bytes of a `<name>.maxpack-recv` of that
    /// name already there (`have`, 0 if none), which the PC may offer to delete.
    NoSpace { need: u64, have: u64 },
    UnknownId,
    /// A PUT not at the current `have`.
    Offset(u64),
    PastSize,
    TooLarge,
    LengthRequired,
    /// The receive is not in a state that takes this request.
    State(State),
    /// Finish before every byte arrived: `have`.
    Incomplete(u64),
    /// Data or finish on a paused receive: why, and what it has.
    Paused { reason: PauseReason, have: u64 },
    /// A discard whose delete failed: the OS error.
    DeleteFailed(String),
    /// A discard of the file a receive is receiving now, which had data within DISCARD_QUIET_MS or has a data
    /// request in flight: busy for this many more seconds at least.
    Receiving { retry_after: u64 },
}

impl Refusal {
    pub fn status(&self) -> u16 {
        match self {
            Refusal::Off => 403,
            Refusal::BadCode => 401,
            Refusal::Locked(_) => 429,
            Refusal::BadRequest(_) | Refusal::BadName | Refusal::BadSize | Refusal::Fat32 | Refusal::PastSize => 400,
            Refusal::NoDrive | Refusal::UnknownId => 404,
            Refusal::Exists | Refusal::Busy(_) | Refusal::Offset(_) | Refusal::State(_) | Refusal::Incomplete(_)
            | Refusal::Paused { .. } => 409,
            Refusal::NoSpace { .. } => 507,
            Refusal::TooLarge => 413,
            Refusal::LengthRequired => 411,
            Refusal::DeleteFailed(_) => 500,
            Refusal::Receiving { .. } => 409,
        }
    }

    pub fn code(&self) -> &'static str {
        match self {
            Refusal::Off => "receive_off",
            Refusal::BadCode => "bad_code",
            Refusal::Locked(_) => "locked",
            Refusal::BadRequest(_) => "bad_request",
            Refusal::BadName => "bad_name",
            Refusal::BadSize => "bad_size",
            Refusal::NoDrive => "no_drive",
            Refusal::Fat32 => "fat32",
            Refusal::Exists => "exists",
            Refusal::Busy(_) => "busy",
            Refusal::NoSpace { .. } => "no_space",
            Refusal::UnknownId => "unknown_id",
            Refusal::Offset(_) => "offset",
            Refusal::PastSize => "past_size",
            Refusal::TooLarge => "too_large",
            Refusal::LengthRequired => "length_required",
            Refusal::State(_) => "state",
            Refusal::Incomplete(_) => "incomplete",
            Refusal::Paused { .. } => "paused",
            Refusal::DeleteFailed(_) => "delete_failed",
            Refusal::Receiving { .. } => "busy",
        }
    }

    pub fn detail(&self) -> String {
        match self {
            Refusal::Off => "receiving games from the PC is off on the console (Settings)".into(),
            Refusal::BadCode => "missing or wrong pairing code (X-MaxPack-Code)".into(),
            Refusal::Locked(ms) => format!("too many wrong codes: try again in {} s", ms.div_ceil(1000)),
            Refusal::BadRequest(why) => why.clone(),
            Refusal::BadName => format!(
                "not a plain .p5z file name: no /, \\, .., control characters or \"*:<>?|, no leading dot, at most {MAX_NAME} bytes"),
            Refusal::BadSize => format!("a .p5z is at least {MIN_SIZE} bytes"),
            Refusal::NoDrive => "no such drive on the console, or MaxPack does not list the games of its homebrew folder".into(),
            Refusal::Fat32 => "the drive is FAT32, which keeps no file of 4 GiB or more".into(),
            Refusal::Exists => "that .p5z is already there".into(),
            Refusal::Busy(b) => b.detail().into(),
            Refusal::NoSpace { need, have: 0 } => format!("not enough space on the drive: {need} more bytes needed (256 MiB are kept free)"),
            Refusal::NoSpace { need, have } => format!(
                "not enough space on the drive: {need} more bytes needed (256 MiB are kept free); {have} bytes of this file are there from an earlier send"),
            Refusal::UnknownId => "no receive with that id (the console restarted, or another receive took its place): start again".into(),
            Refusal::Offset(h) => format!("the data must go on at byte {h}"),
            Refusal::PastSize => "past the size given at start".into(),
            Refusal::TooLarge => format!("at most {} MiB per request", MAX_PUT >> 20),
            Refusal::LengthRequired => "a Content-Length is needed".into(),
            Refusal::State(s) => format!("the receive is {}", s.name()),
            Refusal::Incomplete(h) => format!("only {h} bytes arrived"),
            Refusal::Paused { reason, have } => format!("the receive is paused ({}) at {have} bytes: start it again to go on", reason.name()),
            Refusal::DeleteFailed(why) => why.clone(),
            Refusal::Receiving { retry_after } => format!(
                "a receive of this file is under way: it can be deleted once no data came from the PC for {} s (in {retry_after} s at the soonest)",
                DISCARD_QUIET_MS / 1000),
        }
    }

    /// The answer's body: `{"error":code,"detail":...}`, with `have`, `need` (and `missing`, the same, as before v3)
    /// or `retry_after` (seconds) when it has one.
    pub fn json(&self) -> Value {
        let mut v = json!({"error": self.code(), "detail": self.detail()});
        match self {
            Refusal::Offset(h) | Refusal::Incomplete(h) => v["have"] = (*h).into(),
            Refusal::Paused { reason, have } => {
                v["reason"] = reason.name().into();
                v["have"] = (*have).into();
            }
            Refusal::NoSpace { need, have } => {
                v["need"] = (*need).into();
                v["missing"] = (*need).into();
                v["have"] = (*have).into();
            }
            Refusal::Locked(ms) => v["retry_after"] = ms.div_ceil(1000).into(),
            Refusal::Receiving { retry_after } => v["retry_after"] = (*retry_after).into(),
            _ => {}
        }
        v
    }
}

/// What the page asks of Settings > "Receive games from the PC" and of the receive under way.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Pairing {
    On,
    Off,
    NewCode,
    /// Stop the receive, keeping what arrived.
    Stop,
    /// Delete what arrived.
    Delete,
}

/// Turning receiving on and making a new code only from the console's own browser (`loopback`): from any other
/// device on the network they would undo "off by default" or break a send. Turning it off, stopping and deleting
/// from anywhere, as the page's other actions.
pub fn pairing_allowed(p: Pairing, loopback: bool) -> bool {
    loopback || !matches!(p, Pairing::On | Pairing::NewCode)
}

/// A plain `.p5z` file name, as it will be in `<drive>/homebrew/`: 1 to MAX_NAME bytes ending in `.p5z` (any case)
/// after a name, no `/`, `\`, `..` or control character, none of `"*:<>?|` (which exFAT and FAT32 do not
/// store), and no leading `.` (the scan does not list a hidden file).
pub fn valid_name(name: &str) -> bool {
    !name.is_empty() && name.len() <= MAX_NAME && crate::p5z::is_p5z_name(name) && !name.starts_with('.')
        && !name.contains("..")
        && !name.chars().any(|c| c.is_control() || format_char(c) || matches!(c, '/' | '\\' | '"' | '*' | ':' | '<' | '>' | '?' | '|'))
}

/// A Unicode format character (category Cf): bidi overrides and isolates, zero-width ones, the soft hyphen, tags.
/// In a name they would make the banner or the library show a name that is not the file's.
fn format_char(c: char) -> bool {
    matches!(c as u32, 0xad | 0x600..=0x605 | 0x61c | 0x6dd | 0x70f | 0x890..=0x891 | 0x8e2 | 0x180e | 0x200b..=0x200f
        | 0x202a..=0x202e | 0x2060..=0x2064 | 0x2066..=0x206f | 0xfeff | 0xfff9..=0xfffb | 0x110bd | 0x110cd
        | 0x13430..=0x1343f | 0x1bca0..=0x1bca3 | 0x1d173..=0x1d17a | 0xe0001 | 0xe0020..=0xe007f)
}

/// The `.p5z` name of a file a receive left (`<name>.maxpack-recv`, `name` a plain one by `valid_name`), else None.
pub fn leftover_name(file_name: &str) -> Option<&str> {
    file_name.strip_suffix(PARTIAL_SUFFIX).filter(|n| valid_name(n))
}

/// The page's delete of a leftover file: only a path the list of leftovers has now, and never the file the receive
/// under way writes (`under_way`, in any ASCII case: the same file on exFAT and FAT32).
pub fn may_delete_leftover(path: &str, listed: &[String], under_way: Option<&str>) -> bool {
    listed.iter().any(|l| l == path) && !under_way.is_some_and(|u| u.eq_ignore_ascii_case(path))
}

/// The mount of a drive id: `internal` is `/data`, `usb0`..`usb7` and `ext0`..`ext1` are `/mnt/<id>`.
pub fn drive_mount(id: &str) -> Option<String> {
    if id == "internal" {
        return Some("/data".into());
    }
    let ok = match (id.strip_prefix("usb"), id.strip_prefix("ext")) {
        (Some(n), _) => matches!(n.as_bytes(), [b'0'..=b'7']),
        (_, Some(n)) => matches!(n.as_bytes(), [b'0'..=b'1']),
        _ => false,
    };
    ok.then(|| format!("/mnt/{id}"))
}

/// The id of a drive's mount (drive_mount's inverse).
pub fn drive_id(mount: &str) -> Option<String> {
    if mount == "/data" {
        return Some("internal".into());
    }
    let id = mount.strip_prefix("/mnt/")?;
    (drive_mount(id).as_deref() == Some(mount)).then(|| id.to_string())
}

/// A filesystem type (statfs `f_fstypename`) that is FAT32.
pub fn is_fat32(fstype: &str) -> bool {
    matches!(fstype.to_ascii_lowercase().as_str(), "msdosfs" | "msdos" | "vfat" | "fat32" | "fat")
}

/// A path without a trailing `/` (the root stays `/`).
fn trimmed(p: &str) -> &str {
    let t = p.trim_end_matches('/');
    if t.is_empty() { "/" } else { t }
}

/// The library lists what is put in `homebrew` (a drive's `<mount>/homebrew`): it is a scan root, or a folder
/// right under one that is not a drive's own root (scan::candidates' one level), and is not excluded.
pub fn scanned(homebrew: &str, roots: &[String], exclude: &[String]) -> bool {
    if crate::exclude::excluded(homebrew, exclude) {
        return false;
    }
    let hb = trimmed(homebrew);
    let parent = hb.rsplit_once('/').map(|(p, _)| if p.is_empty() { "/" } else { p });
    // `/mnt/<x>`: a drive's root, never descended.
    let drive_root = |r: &str| r.strip_prefix("/mnt/").is_some_and(|x| !x.is_empty() && !x.contains('/'));
    roots.iter().map(|r| trimmed(r)).any(|r| r == hb || (Some(r) == parent && !drive_root(r)))
}

/// `POST /api/receive/start`'s body.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct StartReq {
    pub name: String,
    pub size: u64,
    pub drive: String,
}

pub fn parse_start(body: &[u8]) -> Result<StartReq, Refusal> {
    serde_json::from_slice(body).map_err(|e| Refusal::BadRequest(format!("expected {{\"name\",\"size\",\"drive\"}}: {e}")))
}

/// A drive as the start sees it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DriveFacts {
    pub free: u64,
    pub fat32: bool,
    /// Its homebrew is listed by the scan (`scanned`) and is a folder or not there yet (never a link).
    pub usable: bool,
}

/// The receive of this run, as the start sees it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Active {
    pub state: State,
    /// The same name, drive and size as the request.
    pub same: bool,
    pub have: u64,
}

/// What `POST /api/receive/start` looks at.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StartFacts {
    pub req: StartReq,
    /// None: no such drive, or not mounted.
    pub drive: Option<DriveFacts>,
    /// Anything is at `<homebrew>/<name>` (a file, a folder, a link).
    pub dest_exists: bool,
    /// `<name>.maxpack-recv`: Some(its length) when a file is there, Some(u64::MAX) for anything else there.
    pub partial: Option<u64>,
    /// This run's last receive of that file failed its verify.
    pub failed_before: bool,
    pub active: Option<Active>,
    /// Another job of the engine: the installer's or a verify (job::busy without the receive).
    pub busy: Option<Busy>,
}

/// What a start does.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Start {
    /// The receive under way is this very file: its token again, and how much it has.
    Resume { have: u64 },
    /// A new receive, from `have` (the bytes of a `.maxpack-recv` left by an earlier one; 0 = from the start,
    /// truncating whatever is there).
    New { have: u64 },
    /// The same file, paused in this run: receiving again with its token, from `have`.
    Unpause { have: u64 },
}

/// Where a new receive starts from a file left on the disk: its length less the last MAX_PUT (a power cut during
/// a PUT may have left a length whose tail never reached the disk; at most that much is sent again), when it is no
/// longer than the size and did not fail its verify in this run (then it is whole and damaged: sent again from the
/// start); else 0.
pub fn resume_have(partial: Option<u64>, size: u64, failed_before: bool) -> u64 {
    match partial {
        Some(n) if n <= size && !failed_before => n.saturating_sub(MAX_PUT),
        _ => 0,
    }
}

/// A receive in `stage` whose last request came at `last_ms` pauses at `now_ms`: receiving, idle for IDLE_MS.
pub fn idle(stage: State, last_ms: u64, now_ms: u64) -> bool {
    stage == State::Receiving && now_ms.checked_sub(last_ms).is_some_and(|d| d >= IDLE_MS)
}

/// How much of a refused PUT body to read and drop: none for the gate's refusals and the size's (closing at once
/// costs a stranger nothing; the PC can send `Expect: 100-continue`) or when the PC waits for 100 Continue; else
/// the body, up to MAX_DRAIN.
pub fn drain_len(r: &Refusal, len: u64, expect_continue: bool) -> u64 {
    let closes = matches!(r, Refusal::Off | Refusal::BadCode | Refusal::Locked(_) | Refusal::TooLarge | Refusal::LengthRequired);
    if closes || expect_continue { 0 } else { len.min(MAX_DRAIN) }
}

/// Bytes short to receive the rest (size - have) and keep MARGIN; None when it fits.
pub fn missing(free: u64, size: u64, have: u64) -> Option<u64> {
    let need = size.saturating_sub(have).saturating_add(MARGIN);
    need.checked_sub(free).filter(|m| *m > 0)
}

/// The start's checks, in this order: the name, the size, the drive, FAT32, the `.p5z` already there, the
/// receive under way (the same file resumes it, another is busy), another job, a partial file that is not a
/// file, the space.
pub fn start_gate(f: &StartFacts) -> Result<Start, Refusal> {
    let r = &f.req;
    if !valid_name(&r.name) {
        return Err(Refusal::BadName);
    }
    if r.size < MIN_SIZE {
        return Err(Refusal::BadSize);
    }
    let Some(d) = f.drive.as_ref().filter(|d| d.usable) else { return Err(Refusal::NoDrive) };
    if d.fat32 && r.size >= FAT32_LIMIT {
        return Err(Refusal::Fat32);
    }
    if f.dest_exists {
        return Err(Refusal::Exists);
    }
    match &f.active {
        Some(a) if a.state == State::Receiving && a.same => return Ok(Start::Resume { have: a.have }),
        Some(a) if a.state.active() => return Err(Refusal::Busy(Busy::Receive)),
        _ => {}
    }
    // The same file paused in this run goes on with its token: exactly where it was when its file is as it left it
    // after flushed PUTs (idle, stopped); from the file, as one left on the disk, after a failed write or a change.
    let unpause = f.active.as_ref().and_then(|a| match a.state {
        State::Paused(r) if a.same => Some((r != PauseReason::WriteError && f.partial == Some(a.have)).then_some(a.have)),
        _ => None,
    });
    if let Some(b) = f.busy {
        return Err(Refusal::Busy(b));
    }
    if f.partial == Some(u64::MAX) {
        return Err(Refusal::Exists);
    }
    let have = match unpause {
        Some(Some(exact)) => exact,
        Some(None) => resume_have(f.partial, r.size, false),
        None => resume_have(f.partial, r.size, f.failed_before),
    };
    if let Some(m) = missing(d.free, r.size, have) {
        // With what of it is there already (the partial's length, not where it would resume): the PC may offer
        // to delete it (`discard`).
        return Err(Refusal::NoSpace { need: m, have: f.partial.unwrap_or(0) });
    }
    Ok(if unpause.is_some() { Start::Unpause { have } } else { Start::New { have } })
}

fn internal() -> String {
    "internal".into()
}

/// `POST /api/receive/discard`'s body: `drive` as in start, the internal drive when it is missing.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct DiscardReq {
    pub name: String,
    #[serde(default = "internal")]
    pub drive: String,
}

pub fn parse_discard(body: &[u8]) -> Result<DiscardReq, Refusal> {
    serde_json::from_slice(body).map_err(|e| Refusal::BadRequest(format!("expected {{\"name\",\"drive\"}}: {e}")))
}

/// What `POST /api/receive/discard` looks at.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiscardFacts {
    pub req: DiscardReq,
    /// The drive is mounted and the PC may send to it (DriveFacts::usable).
    pub usable: bool,
    /// `<name>.maxpack-recv`, as StartFacts::partial.
    pub partial: Option<u64>,
    /// This run's receive; `same`: of that very file (`same_file`).
    pub active: Option<Active>,
    /// A data request of that receive is in flight (a PUT holds the writer).
    pub writing: bool,
    /// Milliseconds since that receive's last data request (its start counts as one).
    pub quiet_ms: u64,
}

/// What a discard does.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Discard {
    /// The bytes of the file it deletes; None when there is no file a send left there.
    pub bytes: Option<u64>,
    /// This run's receive of that file ends: its token is forgotten.
    pub forget: bool,
    /// It was receiving, with no data for DISCARD_QUIET_MS: it pauses (idle) first.
    pub paused_first: bool,
}

/// The discard's checks, in this order: the name (start's rules), the drive, the receive of that file under way
/// (verifying: busy; receiving: busy while data came within DISCARD_QUIET_MS or a data request is in flight, with
/// `retry_after`, else it pauses first and goes on as for a paused one). Then it deletes `<name>.maxpack-recv` when a file is there, whatever its size and
/// with no space check, and never anything else (something there that is not a file is left as it is); a receive
/// of that file that is paused, failed or cancelled ends (one that is done is the library's `.p5z` now).
pub fn discard_gate(f: &DiscardFacts) -> Result<Discard, Refusal> {
    if !valid_name(&f.req.name) {
        return Err(Refusal::BadName);
    }
    if !f.usable {
        return Err(Refusal::NoDrive);
    }
    let same = f.active.as_ref().filter(|a| a.same);
    // Receiving, but the PC went without saying so (a crash, a lost leave): let go once no data came for
    // DISCARD_QUIET_MS and none is being written; until then busy, with when to try again.
    let paused_first = same.is_some_and(|a| a.state == State::Receiving);
    if paused_first && (f.writing || f.quiet_ms < DISCARD_QUIET_MS) {
        let left = DISCARD_QUIET_MS.saturating_sub(f.quiet_ms);
        return Err(Refusal::Receiving { retry_after: left.div_ceil(1000).max(1) });
    }
    if same.is_some_and(|a| a.state == State::Verifying) {
        return Err(Refusal::Busy(Busy::Receive));
    }
    let bytes = f.partial.filter(|n| *n != u64::MAX);
    Ok(Discard { bytes, forget: same.is_some_and(|a| a.state != State::Done), paused_first })
}

/// A receiving receive with no data request for this long (and none in flight) is let go by a discard of its file:
/// above the PC's longest wait between its retries (30 s), so a live send is never cut.
pub const DISCARD_QUIET_MS: u64 = 60_000;

/// The same file on the console's drives: the same drive id, and the same name without regard to ASCII case (exFAT
/// and FAT32 ignore it; the contract's v3.1 uses that rule on every drive).
pub fn same_file(a_name: &str, a_drive: &str, b_name: &str, b_drive: &str) -> bool {
    a_drive == b_drive && a_name.eq_ignore_ascii_case(b_name)
}

/// `POST /api/receive/leave`: the PC stopped the send and kept what arrived. Only a receive that is receiving pauses
/// (reason `left`); any other is left as it is (the answer is 200 either way).
pub fn leave_pauses(state: State) -> bool {
    state == State::Receiving
}

/// The discard's answer: `{"deleted":true,"bytes":N}`, or `{"deleted":false}` when nothing of it was there.
pub fn discard_json(bytes: Option<u64>) -> Value {
    match bytes {
        Some(n) => json!({"deleted": true, "bytes": n}),
        None => json!({"deleted": false}),
    }
}

/// A `PUT /api/receive/data` of `len` bytes at `offset` into a receive in `state` with `have` of `size`.
pub fn data_check(state: State, have: u64, size: u64, offset: u64, len: u64) -> Result<(), Refusal> {
    if let State::Paused(reason) = state {
        return Err(Refusal::Paused { reason, have });
    }
    if state != State::Receiving {
        return Err(Refusal::State(state));
    }
    if len > MAX_PUT {
        return Err(Refusal::TooLarge);
    }
    if offset != have {
        return Err(Refusal::Offset(have));
    }
    match offset.checked_add(len) {
        Some(end) if end <= size => Ok(()),
        _ => Err(Refusal::PastSize),
    }
}

/// `POST /api/receive/finish`: only a receive that has every byte.
pub fn finish_check(state: State, have: u64, size: u64) -> Result<(), Refusal> {
    if let State::Paused(reason) = state {
        return Err(Refusal::Paused { reason, have });
    }
    if state != State::Receiving {
        return Err(Refusal::State(state));
    }
    if have != size {
        return Err(Refusal::Incomplete(have));
    }
    Ok(())
}

/// Stop, keeping what arrived (the page's "Stop"): only a receive under way.
pub fn stop_check(state: State) -> Result<(), Refusal> {
    if state.active() { Ok(()) } else { Err(Refusal::State(state)) }
}

/// `POST /api/receive/cancel`: anything but a receive that is done (its `.p5z` is the library's now).
pub fn cancel_check(state: State) -> Result<(), Refusal> {
    match state {
        State::Done => Err(Refusal::State(state)),
        _ => Ok(()),
    }
}

/// The value of `key` in a query string (`a=1&b=2`); the first when it repeats.
pub fn query_param<'a>(query: &'a str, key: &str) -> Option<&'a str> {
    query.split('&').find_map(|kv| {
        let (k, v) = kv.split_once('=')?;
        (k == key).then_some(v)
    })
}

/// A byte offset: 1 to 20 ASCII digits that fit in a u64.
pub fn parse_offset(s: &str) -> Option<u64> {
    (!s.is_empty() && s.len() <= 20 && s.bytes().all(|b| b.is_ascii_digit())).then(|| s.parse().ok()).flatten()
}

/// A receive's token: 32 lowercase hex digits.
pub fn valid_token(s: &str) -> bool {
    s.len() == 32 && s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// A token from 16 random bytes.
pub fn token(bytes: &[u8; 16]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// A pairing code: six ASCII digits.
pub fn valid_code(s: &str) -> bool {
    s.len() == 6 && s.bytes().all(|b| b.is_ascii_digit())
}

/// A code from a random u32, evenly over 000000..999999: None for the few values above the last whole million
/// (draw again).
pub fn code_from(r: u32) -> Option<String> {
    (r < 4_294_000_000).then(|| format!("{:06}", r % 1_000_000))
}

/// The code presented is the code, compared in a time that does not depend on where they differ.
pub fn same_code(presented: &str, code: &str) -> bool {
    let (p, c) = (presented.as_bytes(), code.as_bytes());
    valid_code(code) && p.len() == c.len() && p.iter().zip(c).fold(0u8, |acc, (a, b)| acc | (a ^ b)) == 0
}

/// What the lockout keys an address by: an IPv4 address (an IPv4-mapped IPv6 one too), or an IPv6 /64.
pub fn addr_key(ip: IpAddr) -> String {
    match ip {
        IpAddr::V4(v4) => v4.to_string(),
        IpAddr::V6(v6) => match v6.to_ipv4_mapped() {
            Some(v4) => v4.to_string(),
            None => {
                let s = v6.segments();
                format!("{:x}:{:x}:{:x}:{:x}::/64", s[0], s[1], s[2], s[3])
            }
        },
    }
}

/// A lockout to tell the log about, once when it starts (Lockout::take_events): wrong codes alone are not logged.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LockEvent {
    /// This address was locked out after MAX_WRONG wrong codes in a row.
    Address(String),
    /// Every address was locked out: `wrong` codes from `addresses` addresses within LOCK_MS, or too many addresses.
    All { wrong: usize, addresses: usize },
}

#[derive(Debug, Clone, Copy, Default)]
struct Tries {
    wrong: u32,
    last_ms: u64,
    until_ms: u64,
}

/// The code check with its lockout: MAX_WRONG wrong codes in a row from one address lock it out for LOCK_MS;
/// GLOBAL_WRONG wrong codes from all addresses within LOCK_MS, or more than MAX_ADDRESSES addresses guessing
/// within FORGET_MS, lock every address out for LOCK_MS. A right code clears its address's count. Time is given (ms).
#[derive(Debug, Default)]
pub struct Lockout {
    per: HashMap<String, Tries>,
    /// The wrong codes within LOCK_MS: when, and from which address.
    recent: VecDeque<(u64, String)>,
    all_until_ms: u64,
    events: Vec<LockEvent>,
}

impl Lockout {
    /// `presented`: the `X-MaxPack-Code` header (None: absent, which is refused but not counted). `code`: the
    /// console's code. While locked out the code is not even compared.
    pub fn check(&mut self, key: &str, now_ms: u64, presented: Option<&str>, code: &str) -> Result<(), Refusal> {
        self.check_as(key, now_ms, presented, code, false)
    }

    /// `check`, for a request that carries the token of the receive under way (`carries_token`): the lock of
    /// every address does not stop it (128 random bits and the code: a stranger's flood must not stall a send), its
    /// own address's lock does.
    pub fn check_as(&mut self, key: &str, now_ms: u64, presented: Option<&str>, code: &str, carries_token: bool) -> Result<(), Refusal> {
        if self.all_until_ms > now_ms && !carries_token {
            return Err(Refusal::Locked(self.all_until_ms - now_ms));
        }
        if let Some(t) = self.per.get_mut(key) {
            if t.until_ms > now_ms {
                return Err(Refusal::Locked(t.until_ms - now_ms));
            }
            if t.until_ms != 0 {
                // The minute is over: MAX_WRONG more tries.
                *t = Tries::default();
            }
        }
        let Some(p) = presented else { return Err(Refusal::BadCode) };
        if same_code(p, code) {
            self.per.remove(key);
            return Ok(());
        }
        self.recent.push_back((now_ms, key.to_string()));
        while self.recent.front().is_some_and(|(t, _)| now_ms.saturating_sub(*t) >= LOCK_MS) {
            self.recent.pop_front();
        }
        if !self.per.contains_key(key) && self.per.len() >= MAX_ADDRESSES {
            self.per.retain(|_, t| t.until_ms > now_ms || now_ms.saturating_sub(t.last_ms) < FORGET_MS);
            if self.per.len() >= MAX_ADDRESSES {
                self.lock_all(now_ms, self.per.len());
                return Err(Refusal::Locked(LOCK_MS));
            }
        }
        let t = self.per.entry(key.to_string()).or_default();
        t.wrong += 1;
        t.last_ms = now_ms;
        if t.wrong >= MAX_WRONG {
            (t.wrong, t.until_ms) = (0, now_ms + LOCK_MS);
            self.events.push(LockEvent::Address(key.to_string()));
        }
        if self.recent.len() >= GLOBAL_WRONG {
            let mut keys: Vec<&str> = self.recent.iter().map(|(_, k)| k.as_str()).collect();
            keys.sort_unstable();
            keys.dedup();
            let addresses = keys.len();
            self.lock_all(now_ms, addresses);
        }
        Err(Refusal::BadCode)
    }

    /// Every address locked out for LOCK_MS, told once (when it was not already).
    fn lock_all(&mut self, now_ms: u64, addresses: usize) {
        if self.all_until_ms <= now_ms {
            self.events.push(LockEvent::All { wrong: self.recent.len(), addresses });
        }
        self.all_until_ms = now_ms + LOCK_MS;
        self.recent.clear();
    }

    /// The lockouts that started since the last call, to log once each.
    pub fn take_events(&mut self) -> Vec<LockEvent> {
        std::mem::take(&mut self.events)
    }
}

/// The gate of every `/api/receive/*` route: the switch, then the lockout and the code. `carries_token`: the
/// request names the token of the receive under way (Lockout::check_as).
pub fn gate(on: bool, lock: &mut Lockout, key: &str, now_ms: u64, presented: Option<&str>, code: &str, carries_token: bool) -> Result<(), Refusal> {
    if !on {
        return Err(Refusal::Off);
    }
    lock.check_as(key, now_ms, presented, code, carries_token)
}

/// One item the quick verify found wrong, as the status lists it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BadItem {
    /// "volume" or "file".
    pub kind: &'static str,
    pub volume: Option<u32>,
    pub name: String,
    pub expected: String,
    pub found: Option<String>,
    pub why: String,
}

/// The verify of what arrived, as the status shows it.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct VerifyProgress {
    pub bytes: u64,
    pub total: u64,
    /// How many did not match (more than `bad` lists when there are many).
    pub nbad: u64,
    pub bad: Vec<BadItem>,
}

/// The status's `verify` once the quick verify (maxpack_logic::verify::Quick) has ended.
pub fn verify_progress(q: &crate::verify::Quick) -> VerifyProgress {
    let bad = q.bad.iter().map(|b| BadItem {
        kind: b.what, volume: b.volume, name: b.name.clone(), expected: b.expected.clone(), found: b.found.clone(), why: b.why.clone(),
    }).collect();
    VerifyProgress { bytes: q.bytes, total: q.total, nbad: q.nbad, bad }
}

/// The status's `error` (and the log's line) for a receive whose quick verify found damage: how many records of
/// all, and the first that failed.
pub fn failed_text(q: &crate::verify::Quick) -> String {
    let all = u64::from(q.volumes) + u64::from(q.files);
    let mut s = format!("{} of {all} volumes and files do not match", q.nbad);
    if let Some(b) = q.bad.first() {
        s += &format!(": {} {} ({})", b.what, b.name, b.why);
        if q.nbad > 1 {
            s += &format!(" and {} more", q.nbad - 1);
        }
    }
    s
}

/// `GET /api/receive/status`'s answer: `{"have","size","state","verify":{bytes,total,nbad,bad:[...]}|null,"error"}`,
/// and `reason` when paused.
pub fn status_json(have: u64, size: u64, state: State, verify: Option<&VerifyProgress>, error: Option<&str>) -> Value {
    // Each item as the PC's `maxpack verify --quick --json` gives it: a volume by its number, a file by its path, and
    // `found` only when something whole could be hashed.
    let verify = verify.map(|v| {
        let bad: Vec<Value> = v.bad.iter().map(|b| {
            let mut o = json!({"why": b.why, "expected": b.expected});
            match b.volume {
                Some(n) if b.kind == "volume" => o["volume"] = n.into(),
                _ => o["path"] = b.name.clone().into(),
            }
            if let Some(found) = &b.found {
                o["found"] = found.clone().into();
            }
            o
        }).collect();
        json!({"bytes": v.bytes, "total": v.total, "nbad": v.nbad, "bad": bad})
    });
    let mut s = json!({"have": have, "size": size, "state": state.name(), "verify": verify, "error": error});
    if let State::Paused(reason) = state {
        s["reason"] = reason.name().into();
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn leftovers_are_partial_files_of_plain_names() {
        assert_eq!(leftover_name("Game [PPSA01234].p5z.maxpack-recv"), Some("Game [PPSA01234].p5z"));
        for no in ["Game.p5z", "Game.maxpack-recv", ".p5z.maxpack-recv", "x.pkg.maxpack-recv", "a..b.p5z.maxpack-recv",
            "Game.p5z.maxpack-recv.tmp", "Game.p5z.MAXPACK-RECV", ""] {
            assert_eq!(leftover_name(no), None, "{no:?}");
        }
        let listed = vec!["/mnt/usb0/homebrew/A.p5z.maxpack-recv".to_string(), "/data/homebrew/B.p5z.maxpack-recv".to_string()];
        assert!(may_delete_leftover("/data/homebrew/B.p5z.maxpack-recv", &listed, None));
        // Only what the list has, never the file a receive under way writes.
        assert!(!may_delete_leftover("/data/homebrew/C.p5z.maxpack-recv", &listed, None));
        assert!(!may_delete_leftover("/data/homebrew/B.p5z", &listed, None));
        assert!(!may_delete_leftover("/data/homebrew/B.p5z.maxpack-recv", &listed, Some("/data/homebrew/B.p5z.maxpack-recv")));
        assert!(may_delete_leftover("/data/homebrew/B.p5z.maxpack-recv", &listed, Some("/mnt/usb0/homebrew/A.p5z.maxpack-recv")));
    }

    #[test]
    fn only_the_console_turns_it_on() {
        // On and a new code only from the console's own browser; off, stop and delete from anywhere.
        assert!(pairing_allowed(Pairing::On, true));
        assert!(!pairing_allowed(Pairing::On, false));
        assert!(pairing_allowed(Pairing::NewCode, true));
        assert!(!pairing_allowed(Pairing::NewCode, false));
        for p in [Pairing::Off, Pairing::Stop, Pairing::Delete] {
            assert!(pairing_allowed(p, true) && pairing_allowed(p, false), "{p:?}");
        }
    }

    #[test]
    fn states() {
        let all = [State::Receiving, State::Verifying, State::Done, State::Failed, State::Cancelled, State::Paused(PauseReason::Idle)];
        let names: Vec<&str> = all.iter().map(|s| s.name()).collect();
        assert_eq!(names, ["receiving", "verifying", "done", "failed", "cancelled", "paused"]);
        let active: Vec<bool> = all.iter().map(|s| s.active()).collect();
        assert_eq!(active, [true, true, false, false, false, false]);
    }

    #[test]
    fn a_stalled_receive_pauses() {
        // Receiving with nothing from the PC for IDLE_MS: paused (no longer under way); a verify never pauses.
        assert!(!idle(State::Receiving, 1000, 1000 + IDLE_MS - 1));
        assert!(idle(State::Receiving, 1000, 1000 + IDLE_MS));
        assert!(!idle(State::Verifying, 0, 10 * IDLE_MS));
        for s in [State::Done, State::Failed, State::Cancelled, State::Paused(PauseReason::Idle)] {
            assert!(!idle(s, 0, 10 * IDLE_MS), "{s:?}");
        }
        // A clock that went back never pauses.
        assert!(!idle(State::Receiving, 5000, 1000));
    }

    #[test]
    fn a_paused_receive_goes_on_where_it_was() {
        // The same file, paused idle or stopped in this run, its file as long as it had: the same token again, from
        // there exactly (every PUT was flushed).
        for reason in [PauseReason::Idle, PauseReason::Stopped] {
            let paused = Some(Active { state: State::Paused(reason), same: true, have: 3 << 30 });
            assert_eq!(start_gate(&StartFacts { active: paused, partial: Some(3 << 30), ..facts() }),
                Ok(Start::Unpause { have: 3 << 30 }), "{reason:?}");
        }
        // Paused by a write that failed (its flush may not have happened), or its file changed meanwhile: from the
        // file, as one left on the disk.
        let failed_write = Some(Active { state: State::Paused(PauseReason::WriteError), same: true, have: 3 << 30 });
        assert_eq!(start_gate(&StartFacts { active: failed_write, partial: Some(3 << 30), ..facts() }),
            Ok(Start::Unpause { have: (3 << 30) - MAX_PUT }));
        let changed = Some(Active { state: State::Paused(PauseReason::Idle), same: true, have: 3 << 30 });
        assert_eq!(start_gate(&StartFacts { active: changed, partial: Some(2 << 30), ..facts() }),
            Ok(Start::Unpause { have: (2 << 30) - MAX_PUT }));
        // It is not under way: another job of the engine refuses it, and the space is looked at.
        let paused = Some(Active { state: State::Paused(PauseReason::Idle), same: true, have: 5 });
        assert_eq!(start_gate(&StartFacts { active: paused.clone(), partial: Some(5), busy: Some(Busy::Install), ..facts() }),
            Err(Refusal::Busy(Busy::Install)));
        let full = Some(DriveFacts { free: 0, fat32: false, usable: true });
        assert!(matches!(start_gate(&StartFacts { active: paused, partial: Some(5), drive: full, ..facts() }), Err(Refusal::NoSpace { .. })));
        // Another file paused: nothing under way, the start goes on as usual (a new receive).
        let other = Some(Active { state: State::Paused(PauseReason::Stopped), same: false, have: 5 });
        assert_eq!(start_gate(&StartFacts { active: other, ..facts() }), Ok(Start::New { have: 0 }));
    }

    #[test]
    fn a_paused_receive_answers_with_its_reason() {
        assert_eq!([PauseReason::Idle, PauseReason::WriteError, PauseReason::Stopped].map(|r| r.name()),
            ["idle", "write_error", "stopped"]);
        let paused = State::Paused(PauseReason::Stopped);
        assert_eq!(paused.name(), "paused");
        // PUT data and finish on it: 409 paused with the reason and have.
        let r = Refusal::Paused { reason: PauseReason::Stopped, have: 40 };
        assert_eq!(data_check(paused, 40, 100, 40, 10), Err(r.clone()));
        assert_eq!(finish_check(State::Paused(PauseReason::Stopped), 40, 100), Err(r.clone()));
        assert_eq!((r.status(), r.code()), (409, "paused"));
        let j = r.json();
        assert_eq!((j["error"].as_str(), j["reason"].as_str(), j["have"].as_u64()), (Some("paused"), Some("stopped"), Some(40)));
        // The status says it with its reason; any other state has none.
        let s = status_json(40, 100, State::Paused(PauseReason::Idle), None, None);
        assert_eq!((s["state"].as_str(), s["reason"].as_str()), (Some("paused"), Some("idle")));
        assert!(status_json(40, 100, State::Receiving, None, None).get("reason").is_none());
    }

    #[test]
    fn what_a_refused_body_drains() {
        // The gate's refusals close at once (the PC can send Expect: 100-continue); the others read at most
        // MAX_DRAIN of the body, and nothing when the PC waits for 100 Continue.
        for r in [Refusal::Off, Refusal::BadCode, Refusal::Locked(5), Refusal::TooLarge, Refusal::LengthRequired] {
            assert_eq!(drain_len(&r, 10, false), 0, "{r:?}");
        }
        assert_eq!(drain_len(&Refusal::Offset(5), 10, false), 10);
        assert_eq!(drain_len(&Refusal::Offset(5), MAX_PUT, false), MAX_DRAIN);
        assert_eq!(drain_len(&Refusal::UnknownId, MAX_PUT, true), 0);
        assert_eq!(drain_len(&Refusal::Paused { reason: PauseReason::Idle, have: 3 }, 7, false), 7);
    }

    #[test]
    fn names_are_plain_p5z_files() {
        for ok in ["Game [PPSA01234].p5z", "a.p5z", "A.P5Z", "Juego ñ 日本.p5z", "x y (1).p5z", "v1.0.p5z"] {
            assert!(valid_name(ok), "{ok:?}");
        }
        let long_ok = format!("{}.p5z", "a".repeat(MAX_NAME - 4));
        assert!(valid_name(&long_ok));
        let too_long = format!("{}.p5z", "a".repeat(MAX_NAME - 3));
        for bad in ["", ".p5z", "game.pkg", "game.p5z.txt", "game", "../game.p5z", "..\\game.p5z", "a/b.p5z", "a\\b.p5z",
            "a..b.p5z", "..p5z", ".hidden.p5z", "a\u{0}.p5z", "a\n.p5z", "a\u{7f}.p5z", "a\u{85}.p5z", "a:b.p5z", "a*.p5z",
            "a?.p5z", "a<b>.p5z", "a\"b.p5z", "a|b.p5z", "/data/x.p5z", too_long.as_str(),
            // Format characters: a bidi override or a zero-width one would show a name that is not the file's.
            "a\u{202e}zp5.p5z", "a\u{200b}.p5z", "a\u{200f}.p5z", "a\u{2066}.p5z", "a\u{feff}.p5z", "a\u{ad}.p5z",
            "a\u{61c}.p5z", "a\u{180e}.p5z", "a\u{2060}.p5z", "a\u{e0041}.p5z"] {
            assert!(!valid_name(bad), "{bad:?}");
        }
    }

    #[test]
    fn drives_by_id() {
        assert_eq!(drive_mount("internal").as_deref(), Some("/data"));
        assert_eq!(drive_mount("usb0").as_deref(), Some("/mnt/usb0"));
        assert_eq!(drive_mount("usb7").as_deref(), Some("/mnt/usb7"));
        assert_eq!(drive_mount("ext0").as_deref(), Some("/mnt/ext0"));
        assert_eq!(drive_mount("ext1").as_deref(), Some("/mnt/ext1"));
        for bad in ["", "usb8", "usb10", "usb", "ext2", "USB0", "data", "/data", "usb0/..", "internal ", "usb-1"] {
            assert_eq!(drive_mount(bad), None, "{bad:?}");
        }
        for id in ["internal", "usb0", "usb7", "ext0", "ext1"] {
            assert_eq!(drive_id(&drive_mount(id).unwrap()).as_deref(), Some(id));
        }
        assert_eq!(drive_id("/mnt/usb8"), None);
        assert_eq!(drive_id("/mnt"), None);
        assert_eq!(drive_id("/user"), None);
    }

    #[test]
    fn fat32_by_type() {
        for t in ["msdosfs", "MSDOSFS", "msdos", "vfat", "fat32", "fat"] {
            assert!(is_fat32(t), "{t}");
        }
        for t in ["exfatfs", "exfat", "ufs", "pfs", "", "fat64"] {
            assert!(!is_fat32(t), "{t}");
        }
    }

    #[test]
    fn homebrew_is_listed_by_the_scan() {
        let roots: Vec<String> = ["/data/homebrew", "/mnt/usb0/homebrew", "/mnt/usb1", "/data", "/mnt/usb2/games"]
            .iter().map(|s| s.to_string()).collect();
        assert!(scanned("/data/homebrew", &roots, &[]));
        assert!(scanned("/mnt/usb0/homebrew", &roots, &[]));
        // A drive's own root is never descended: its homebrew must be a root itself.
        assert!(!scanned("/mnt/usb1/homebrew", &roots, &[]));
        // One level under a root that is not a drive root.
        assert!(scanned("/mnt/ext0/homebrew", &["/mnt/ext0".into()], &[]) == false);
        assert!(scanned("/data/homebrew", &["/data".into()], &[]));
        assert!(!scanned("/mnt/usb2/homebrew", &roots, &[]));
        // With a trailing slash on the root, and an excluded homebrew.
        assert!(scanned("/mnt/usb3/homebrew", &["/mnt/usb3/homebrew/".into()], &[]));
        assert!(!scanned("/data/homebrew", &roots, &["/data/homebrew".into()]));
        assert!(!scanned("/data/homebrew", &roots, &["/data".into()]));
    }

    #[test]
    fn the_start_body() {
        assert_eq!(parse_start(br#"{"name":"A.p5z","size":1000,"drive":"usb0"}"#),
            Ok(StartReq { name: "A.p5z".into(), size: 1000, drive: "usb0".into() }));
        for bad in [&b""[..], b"{}", b"[]", br#"{"name":"A.p5z","size":-1,"drive":"usb0"}"#, br#"{"name":1,"size":1,"drive":"x"}"#,
            br#"{"name":"A.p5z","size":"1","drive":"usb0"}"#, b"\xff"] {
            assert!(matches!(parse_start(bad), Err(Refusal::BadRequest(_))), "{bad:?}");
        }
    }

    fn facts() -> StartFacts {
        StartFacts {
            req: StartReq { name: "Game [PPSA01234].p5z".into(), size: 10 << 30, drive: "usb0".into() },
            drive: Some(DriveFacts { free: 100 << 30, fat32: false, usable: true }),
            dest_exists: false, partial: None, failed_before: false, active: None, busy: None,
        }
    }

    #[test]
    fn a_new_receive_starts() {
        assert_eq!(start_gate(&facts()), Ok(Start::New { have: 0 }));
        // A .maxpack-recv of an earlier receive (a restart, a broken connection) resumes where it is, less the last
        // MAX_PUT: a power cut may have left a tail that never reached the disk.
        assert_eq!(start_gate(&StartFacts { partial: Some(3 << 30), ..facts() }), Ok(Start::New { have: (3 << 30) - MAX_PUT }));
        // A finished one whose verify failed in this run, or one longer than the size, starts over.
        assert_eq!(start_gate(&StartFacts { partial: Some(10 << 30), failed_before: true, ..facts() }), Ok(Start::New { have: 0 }));
        assert_eq!(start_gate(&StartFacts { partial: Some((10 << 30) + 1), ..facts() }), Ok(Start::New { have: 0 }));
        // An ended receive of this run is no longer under way.
        for state in [State::Done, State::Failed, State::Cancelled, State::Paused(PauseReason::Idle)] {
            let active = Some(Active { state, same: false, have: 5 });
            assert_eq!(start_gate(&StartFacts { active, ..facts() }), Ok(Start::New { have: 0 }));
        }
    }

    #[test]
    fn the_receive_under_way_resumes_or_is_busy() {
        let same = Some(Active { state: State::Receiving, same: true, have: 77 });
        assert_eq!(start_gate(&StartFacts { active: same, ..facts() }), Ok(Start::Resume { have: 77 }));
        let other = Some(Active { state: State::Receiving, same: false, have: 77 });
        assert_eq!(start_gate(&StartFacts { active: other, ..facts() }), Err(Refusal::Busy(Busy::Receive)));
        let checking = Some(Active { state: State::Verifying, same: true, have: 77 });
        assert_eq!(start_gate(&StartFacts { active: checking, ..facts() }), Err(Refusal::Busy(Busy::Receive)));
        assert_eq!(start_gate(&StartFacts { busy: Some(Busy::Install), ..facts() }), Err(Refusal::Busy(Busy::Install)));
        assert_eq!(start_gate(&StartFacts { busy: Some(Busy::Verify), ..facts() }), Err(Refusal::Busy(Busy::Verify)));
    }

    #[test]
    fn start_refusals_in_order() {
        let f = facts;
        // Each refusal, and that it comes before the ones after it.
        let mut bad_name = f();
        bad_name.req.name = "../x.p5z".into();
        bad_name.drive = None;
        bad_name.dest_exists = true;
        assert_eq!(start_gate(&bad_name), Err(Refusal::BadName));
        let mut small = f();
        small.req.size = MIN_SIZE - 1;
        small.drive = None;
        assert_eq!(start_gate(&small), Err(Refusal::BadSize));
        assert_eq!(start_gate(&StartFacts { drive: None, dest_exists: true, ..f() }), Err(Refusal::NoDrive));
        let unusable = Some(DriveFacts { free: 100 << 30, fat32: false, usable: false });
        assert_eq!(start_gate(&StartFacts { drive: unusable, ..f() }), Err(Refusal::NoDrive));
        let fat = Some(DriveFacts { free: 100 << 30, fat32: true, usable: true });
        assert_eq!(start_gate(&StartFacts { drive: fat.clone(), dest_exists: true, ..f() }), Err(Refusal::Fat32));
        // FAT32 takes a file of up to 4 GiB less one byte.
        let mut fits = f();
        fits.req.size = FAT32_LIMIT - 1;
        fits.drive = fat.clone();
        assert_eq!(start_gate(&fits), Ok(Start::New { have: 0 }));
        fits.req.size = FAT32_LIMIT;
        assert_eq!(start_gate(&fits), Err(Refusal::Fat32));
        let busy_too = Some(Active { state: State::Receiving, same: false, have: 0 });
        assert_eq!(start_gate(&StartFacts { dest_exists: true, active: busy_too, ..f() }), Err(Refusal::Exists));
        assert_eq!(start_gate(&StartFacts { partial: Some(u64::MAX), ..f() }), Err(Refusal::Exists));
        // The space: what is still to come and the margin.
        let tight = Some(DriveFacts { free: (10 << 30) + MARGIN - 1, fat32: false, usable: true });
        assert_eq!(start_gate(&StartFacts { drive: tight.clone(), ..f() }), Err(Refusal::NoSpace { need: 1, have: 0 }));
        assert_eq!(start_gate(&StartFacts { drive: tight, partial: Some(MAX_PUT + 1), ..f() }), Ok(Start::New { have: 1 }));
        let full = Some(DriveFacts { free: 0, fat32: false, usable: true });
        assert_eq!(start_gate(&StartFacts { drive: full, busy: Some(Busy::Install), ..f() }), Err(Refusal::Busy(Busy::Install)));
    }

    #[test]
    fn resume_and_space() {
        assert_eq!(resume_have(None, 100, false), 0);
        // Less the last MAX_PUT, never under 0.
        assert_eq!(resume_have(Some(40), 100, false), 0);
        assert_eq!(resume_have(Some(MAX_PUT + 40), MAX_PUT + 100, false), 40);
        assert_eq!(resume_have(Some(MAX_PUT + 100), MAX_PUT + 100, false), 100);
        assert_eq!(resume_have(Some(101), 100, false), 0);
        assert_eq!(resume_have(Some(40), 100, true), 0);
        assert_eq!(resume_have(Some(u64::MAX), 100, false), 0);
        assert_eq!(missing(MARGIN + 100, 100, 0), None);
        assert_eq!(missing(MARGIN + 99, 100, 0), Some(1));
        assert_eq!(missing(MARGIN, 100, 100), None);
        assert_eq!(missing(0, u64::MAX, 0), Some(u64::MAX));
    }

    #[test]
    fn no_space_says_what_is_there() {
        // Nothing of it on the drive: have 0, need what is short.
        let tight = Some(DriveFacts { free: (10 << 30) + MARGIN - 5, fat32: false, usable: true });
        assert_eq!(start_gate(&StartFacts { drive: tight, ..facts() }), Err(Refusal::NoSpace { need: 5, have: 0 }));
        // A .maxpack-recv of that name: have is its length (what the PC may offer to delete), need what is short once
        // it resumes from it.
        let full = Some(DriveFacts { free: 0, fat32: false, usable: true });
        let partial = (3 << 30) + MAX_PUT;
        assert_eq!(start_gate(&StartFacts { drive: full, partial: Some(partial), ..facts() }),
            Err(Refusal::NoSpace { need: (10 << 30) - (3 << 30) + MARGIN, have: partial }));
        let r = Refusal::NoSpace { need: 7, have: 9 };
        assert_eq!((r.status(), r.code()), (507, "no_space"));
        let j = r.json();
        assert_eq!((j["need"].as_u64(), j["have"].as_u64(), j["missing"].as_u64()), (Some(7), Some(9), Some(7)));
        assert_eq!(Refusal::NoSpace { need: 7, have: 0 }.json()["have"], 0);
    }

    #[test]
    fn the_discard_body() {
        assert_eq!(parse_discard(br#"{"name":"A.p5z","drive":"usb0"}"#),
            Ok(DiscardReq { name: "A.p5z".into(), drive: "usb0".into() }));
        // No drive: the internal one, as start.
        assert_eq!(parse_discard(br#"{"name":"A.p5z"}"#), Ok(DiscardReq { name: "A.p5z".into(), drive: "internal".into() }));
        for bad in [&b""[..], b"{}", b"[]", br#"{"name":1}"#, br#"{"name":"A.p5z","drive":5}"#, b"\xff"] {
            assert!(matches!(parse_discard(bad), Err(Refusal::BadRequest(_))), "{bad:?}");
        }
    }

    fn discard_facts() -> DiscardFacts {
        DiscardFacts {
            req: DiscardReq { name: "Game [PPSA01234].p5z".into(), drive: "usb0".into() },
            usable: true, partial: Some(3 << 30), active: None, writing: false, quiet_ms: 0,
        }
    }

    #[test]
    fn a_discard_deletes_what_a_send_left() {
        // The file is there, whatever its size: deleted, with no space check.
        assert_eq!(discard_gate(&discard_facts()), Ok(Discard { bytes: Some(3 << 30), forget: false, paused_first: false }));
        assert_eq!(discard_gate(&DiscardFacts { partial: Some(0), ..discard_facts() }), Ok(Discard { bytes: Some(0), forget: false, paused_first: false }));
        // Nothing there, or something that is not a file (never deleted): nothing to delete.
        for partial in [None, Some(u64::MAX)] {
            assert_eq!(discard_gate(&DiscardFacts { partial, ..discard_facts() }), Ok(Discard { bytes: None, forget: false, paused_first: false }), "{partial:?}");
        }
        // A paused receive of that file (or one that failed or was cancelled) is ended: its token is forgotten.
        for state in [State::Paused(PauseReason::Stopped), State::Paused(PauseReason::Idle), State::Failed, State::Cancelled] {
            let active = Some(Active { state, same: true, have: 5 });
            assert_eq!(discard_gate(&DiscardFacts { active: active.clone(), ..discard_facts() }),
                Ok(Discard { bytes: Some(3 << 30), forget: true, paused_first: false }), "{state:?}");
            assert_eq!(discard_gate(&DiscardFacts { active, partial: None, ..discard_facts() }),
                Ok(Discard { bytes: None, forget: true, paused_first: false }), "{state:?}");
        }
        // One that is done is the library's .p5z now: kept as it is.
        let done = Some(Active { state: State::Done, same: true, have: 5 });
        assert_eq!(discard_gate(&DiscardFacts { active: done, ..discard_facts() }), Ok(Discard { bytes: Some(3 << 30), forget: false, paused_first: false }));
        // Another file's receive under way does not stop it.
        let other = Some(Active { state: State::Receiving, same: false, have: 5 });
        assert_eq!(discard_gate(&DiscardFacts { active: other, ..discard_facts() }), Ok(Discard { bytes: Some(3 << 30), forget: false, paused_first: false }));
    }

    #[test]
    fn a_discard_refuses_in_order() {
        // The name rules of start, before anything else.
        let mut bad = discard_facts();
        bad.req.name = "../x.p5z".into();
        bad.usable = false;
        assert_eq!(discard_gate(&bad), Err(Refusal::BadName));
        bad.req.name = "x.p5z.maxpack-recv".into();
        assert_eq!(discard_gate(&bad), Err(Refusal::BadName));
        // A drive that is not there, or whose homebrew the PC may not send to.
        let under_way = Some(Active { state: State::Receiving, same: true, have: 5 });
        assert_eq!(discard_gate(&DiscardFacts { usable: false, active: under_way, ..discard_facts() }), Err(Refusal::NoDrive));
        // That very file being verified: busy, with no retry_after.
        let active = Some(Active { state: State::Verifying, same: true, have: 5 });
        assert_eq!(discard_gate(&DiscardFacts { active, quiet_ms: u64::MAX, ..discard_facts() }), Err(Refusal::Busy(Busy::Receive)));
        assert!(Refusal::Busy(Busy::Receive).json().get("retry_after").is_none());
    }

    #[test]
    fn a_discard_of_a_receive_with_no_data_for_60_s_pauses_it_first() {
        let receiving = || Some(Active { state: State::Receiving, same: true, have: 5 });
        // No data request in flight and none for DISCARD_QUIET_MS: paused (idle) first, then deleted as a paused one.
        assert_eq!(DISCARD_QUIET_MS, 60_000);
        for quiet_ms in [DISCARD_QUIET_MS, 10 * 60_000] {
            assert_eq!(discard_gate(&DiscardFacts { active: receiving(), quiet_ms, ..discard_facts() }),
                Ok(Discard { bytes: Some(3 << 30), forget: true, paused_first: true }), "{quiet_ms}");
        }
        // A data request in the last 60 s: busy, with the seconds until those 60 s are up (at least 1).
        let busy = |quiet_ms, writing| discard_gate(&DiscardFacts { active: receiving(), quiet_ms, writing, ..discard_facts() });
        assert_eq!(busy(0, false), Err(Refusal::Receiving { retry_after: 60 }));
        assert_eq!(busy(59_001, false), Err(Refusal::Receiving { retry_after: 1 }));
        assert_eq!(busy(30_500, false), Err(Refusal::Receiving { retry_after: 30 }));
        // A data request in flight: busy however long ago the last one began.
        assert_eq!(busy(0, true), Err(Refusal::Receiving { retry_after: 60 }));
        assert_eq!(busy(10 * 60_000, true), Err(Refusal::Receiving { retry_after: 1 }));
        let r = Refusal::Receiving { retry_after: 42 };
        assert_eq!((r.status(), r.code()), (409, "busy"));
        assert_eq!(r.json()["retry_after"], 42);
        // Another file's receive, quiet or not, is not this one.
        let other = Some(Active { state: State::Receiving, same: false, have: 5 });
        assert_eq!(discard_gate(&DiscardFacts { active: other, writing: true, ..discard_facts() }),
            Ok(Discard { bytes: Some(3 << 30), forget: false, paused_first: false }));
    }

    #[test]
    fn names_that_differ_only_in_case_are_the_same_file() {
        // exFAT and FAT32 ignore case: the same name and drive in any ASCII case is the same file.
        assert!(same_file("Game [PPSA01234].p5z", "usb0", "game [ppsa01234].P5Z", "usb0"));
        assert!(same_file("A.p5z", "internal", "A.p5z", "internal"));
        assert!(!same_file("A.p5z", "usb0", "A.p5z", "usb1"));
        assert!(!same_file("A.p5z", "usb0", "B.p5z", "usb0"));
        // ASCII only, as the contract says.
        assert!(!same_file("É.p5z", "usb0", "é.p5z", "usb0"));
        // A discard whose name differs only in case from the receive under way: busy, never its file deleted.
        let active = Some(Active { state: State::Receiving, same: same_file("Game [PPSA01234].p5z", "usb0", "GAME [ppsa01234].p5z", "usb0"), have: 5 });
        let mut f = DiscardFacts { active, ..discard_facts() };
        f.req.name = "GAME [ppsa01234].p5z".into();
        assert_eq!(discard_gate(&f), Err(Refusal::Receiving { retry_after: 60 }));
        // Settings' Delete never takes the file under way, whatever the case of its listed name.
        let listed = vec!["/mnt/usb0/homebrew/Game.p5z.maxpack-recv".to_string()];
        assert!(!may_delete_leftover("/mnt/usb0/homebrew/Game.p5z.maxpack-recv", &listed, Some("/mnt/usb0/homebrew/game.P5Z.maxpack-recv")));
        assert!(may_delete_leftover("/mnt/usb0/homebrew/Game.p5z.maxpack-recv", &listed, Some("/mnt/usb1/homebrew/game.p5z.maxpack-recv")));
    }

    #[test]
    fn the_pc_says_it_left() {
        // POST /api/receive/leave: a receive that is receiving pauses with reason left; any other is left as it is.
        assert_eq!(PauseReason::Left.name(), "left");
        assert!(leave_pauses(State::Receiving));
        for s in [State::Verifying, State::Done, State::Failed, State::Cancelled, State::Paused(PauseReason::Stopped),
                  State::Paused(PauseReason::Left)] {
            assert!(!leave_pauses(s), "{s:?}");
        }
        // As idle: data and finish answer 409 paused with its reason, and a start of the same file goes on exactly.
        let left = State::Paused(PauseReason::Left);
        assert_eq!(data_check(left, 40, 100, 40, 10), Err(Refusal::Paused { reason: PauseReason::Left, have: 40 }));
        assert_eq!(finish_check(left, 40, 100), Err(Refusal::Paused { reason: PauseReason::Left, have: 40 }));
        assert_eq!(status_json(40, 100, left, None, None)["reason"], "left");
        let paused = Some(Active { state: left, same: true, have: 3 << 30 });
        assert_eq!(start_gate(&StartFacts { active: paused, partial: Some(3 << 30), ..facts() }), Ok(Start::Unpause { have: 3 << 30 }));
        // A discard of it deletes at once.
        let paused = Some(Active { state: left, same: true, have: 5 });
        assert_eq!(discard_gate(&DiscardFacts { active: paused, ..discard_facts() }),
            Ok(Discard { bytes: Some(3 << 30), forget: true, paused_first: false }));
    }

    #[test]
    fn a_repeated_key_is_refused() {
        // The bodies are read by the same parse as what acts on them: a key given twice is no request.
        assert!(matches!(parse_start(br#"{"name":"A.p5z","name":"B.p5z","size":1000,"drive":"usb0"}"#), Err(Refusal::BadRequest(_))));
        assert!(matches!(parse_start(br#"{"name":"A.p5z","size":1000,"drive":"usb0","drive":"usb1"}"#), Err(Refusal::BadRequest(_))));
        assert!(matches!(parse_discard(br#"{"name":"A.p5z","name":"B.p5z"}"#), Err(Refusal::BadRequest(_))));
        assert!(matches!(parse_discard(br#"{"name":"A.p5z","drive":"usb0","drive":"usb1"}"#), Err(Refusal::BadRequest(_))));
    }

    #[test]
    fn what_a_discard_answers() {
        assert_eq!(discard_json(Some(42)), json!({"deleted": true, "bytes": 42}));
        assert_eq!(discard_json(None), json!({"deleted": false}));
        let r = Refusal::DeleteFailed("Permission denied (os error 13)".into());
        assert_eq!((r.status(), r.code()), (500, "delete_failed"));
        assert_eq!(r.json()["detail"], "Permission denied (os error 13)");
    }

    #[test]
    fn data_rules() {
        assert_eq!(data_check(State::Receiving, 0, 100, 0, 100), Ok(()));
        assert_eq!(data_check(State::Receiving, 40, 100, 40, 60), Ok(()));
        assert_eq!(data_check(State::Receiving, 40, 100, 40, 0), Ok(()));
        assert_eq!(data_check(State::Receiving, 40, 100, 0, 10), Err(Refusal::Offset(40)));
        assert_eq!(data_check(State::Receiving, 40, 100, 50, 10), Err(Refusal::Offset(40)));
        assert_eq!(data_check(State::Receiving, 40, 100, 40, 61), Err(Refusal::PastSize));
        assert_eq!(data_check(State::Receiving, 0, u64::MAX, 0, MAX_PUT + 1), Err(Refusal::TooLarge));
        assert_eq!(data_check(State::Receiving, u64::MAX - 1, u64::MAX, u64::MAX - 1, 5), Err(Refusal::PastSize));
        for s in [State::Verifying, State::Done, State::Failed, State::Cancelled] {
            assert_eq!(data_check(s, 0, 100, 0, 10), Err(Refusal::State(s)));
        }
    }

    #[test]
    fn finish_and_cancel_rules() {
        assert_eq!(finish_check(State::Receiving, 100, 100), Ok(()));
        assert_eq!(finish_check(State::Receiving, 99, 100), Err(Refusal::Incomplete(99)));
        for s in [State::Verifying, State::Done, State::Failed, State::Cancelled] {
            assert_eq!(finish_check(s, 100, 100), Err(Refusal::State(s)));
        }
        for s in [State::Receiving, State::Verifying, State::Failed, State::Cancelled, State::Paused(PauseReason::Idle)] {
            assert_eq!(cancel_check(s), Ok(()));
        }
        assert_eq!(cancel_check(State::Done), Err(Refusal::State(State::Done)));
        // Stop (keep what arrived): only a receive under way.
        assert_eq!(stop_check(State::Receiving), Ok(()));
        assert_eq!(stop_check(State::Verifying), Ok(()));
        for s in [State::Done, State::Failed, State::Cancelled, State::Paused(PauseReason::Idle)] {
            assert_eq!(stop_check(s), Err(Refusal::State(s)));
        }
    }

    #[test]
    fn queries_offsets_and_tokens() {
        assert_eq!(query_param("id=abc&offset=5", "id"), Some("abc"));
        assert_eq!(query_param("id=abc&offset=5", "offset"), Some("5"));
        assert_eq!(query_param("offset=5&id=a&id=b", "id"), Some("a"));
        assert_eq!(query_param("xid=1", "id"), None);
        assert_eq!(query_param("id", "id"), None);
        assert_eq!(query_param("", "id"), None);
        assert_eq!(parse_offset("0"), Some(0));
        assert_eq!(parse_offset("18446744073709551615"), Some(u64::MAX));
        for bad in ["", "-1", "+1", " 1", "1 ", "0x10", "18446744073709551616", "1e3", "000000000000000000001"] {
            assert_eq!(parse_offset(bad), None, "{bad:?}");
        }
        let t = token(&[0xab; 16]);
        assert_eq!(t, "ab".repeat(16));
        assert!(valid_token(&t));
        assert!(valid_token(&token(&[0; 16])));
        for bad in ["", "AB".repeat(16).as_str(), &"a".repeat(31), &"a".repeat(33), &"g".repeat(32)] {
            assert!(!valid_token(bad), "{bad:?}");
        }
    }

    #[test]
    fn codes() {
        assert!(valid_code("000000"));
        assert!(valid_code("123456"));
        for bad in ["", "12345", "1234567", "12345a", " 12345", "１２３４５６"] {
            assert!(!valid_code(bad), "{bad:?}");
        }
        assert_eq!(code_from(0).as_deref(), Some("000000"));
        assert_eq!(code_from(1_000_042).as_deref(), Some("000042"));
        assert_eq!(code_from(4_293_999_999).as_deref(), Some("999999"));
        assert_eq!(code_from(4_294_000_000), None);
        assert_eq!(code_from(u32::MAX), None);
        assert!(same_code("123456", "123456"));
        assert!(!same_code("123457", "123456"));
        assert!(!same_code("12345", "123456"));
        assert!(!same_code("", ""));
        assert!(!same_code("1234567", "123456"));
    }

    #[test]
    fn addresses() {
        assert_eq!(addr_key("192.168.1.5".parse().unwrap()), "192.168.1.5");
        assert_eq!(addr_key("::ffff:192.168.1.5".parse().unwrap()), "192.168.1.5");
        assert_eq!(addr_key("2001:db8:1:2:aaaa:bbbb:cccc:dddd".parse().unwrap()), "2001:db8:1:2::/64");
        assert_eq!(addr_key("2001:db8:1:2::9".parse().unwrap()), addr_key("2001:db8:1:2:ffff::1".parse().unwrap()));
    }

    const CODE: &str = "424242";

    #[test]
    fn five_wrong_codes_lock_an_address() {
        let mut l = Lockout::default();
        assert_eq!(l.check("a", 0, Some(CODE), CODE), Ok(()));
        // Four wrong, a right one clears the count, then five wrong lock it.
        for i in 0..4 {
            assert_eq!(l.check("a", i, Some("000000"), CODE), Err(Refusal::BadCode));
        }
        assert_eq!(l.check("a", 10, Some(CODE), CODE), Ok(()));
        for i in 0..MAX_WRONG as u64 {
            assert_eq!(l.check("a", 100 + i, Some("111111"), CODE), Err(Refusal::BadCode));
        }
        // Locked: even the right code, which is not even looked at; another address is not.
        assert_eq!(l.check("a", 1000, Some(CODE), CODE), Err(Refusal::Locked(LOCK_MS + 104 - 1000)));
        assert_eq!(l.check("b", 1000, Some(CODE), CODE), Ok(()));
        // After the minute: five more tries.
        assert_eq!(l.check("a", 104 + LOCK_MS, Some(CODE), CODE), Ok(()));
        // A missing header is refused but not counted.
        for i in 0..20 {
            assert_eq!(l.check("c", 2000 + i, None, CODE), Err(Refusal::BadCode));
        }
        assert_eq!(l.check("c", 3000, Some(CODE), CODE), Ok(()));
        // Malformed codes count as wrong.
        for i in 0..MAX_WRONG as u64 {
            assert_eq!(l.check("d", 4000 + i, Some("x"), CODE), Err(Refusal::BadCode));
        }
        assert!(matches!(l.check("d", 5000, Some(CODE), CODE), Err(Refusal::Locked(_))));
    }

    #[test]
    fn many_addresses_guessing_lock_every_address() {
        let mut l = Lockout::default();
        // Four wrong codes each from five addresses: none is locked, but twenty in a minute lock them all.
        let mut n = 0;
        for a in 0..5 {
            for _ in 0..4 {
                let r = l.check(&format!("10.0.0.{a}"), 1000 + n, Some("999999"), CODE);
                assert_eq!(r, Err(Refusal::BadCode));
                n += 1;
            }
        }
        assert!(matches!(l.check("10.0.0.99", 2000, Some(CODE), CODE), Err(Refusal::Locked(_))));
        assert_eq!(l.check("10.0.0.99", 1019 + LOCK_MS, Some(CODE), CODE), Ok(()));
        // Wrong codes spread over more than a minute do not add up.
        let mut l = Lockout::default();
        for i in 0..40u64 {
            let r = l.check(&format!("10.0.1.{}", i % 4), i * 5000, Some("999999"), CODE);
            assert!(!matches!(r, Err(Refusal::Locked(_))), "{i}");
            if i % 4 == 3 {
                // Each address gets a right one now and then, so none reaches five in a row.
                for a in 0..4 {
                    assert_eq!(l.check(&format!("10.0.1.{a}"), i * 5000 + 1, Some(CODE), CODE), Ok(()));
                }
            }
        }
        // More addresses than it keeps within FORGET_MS (under twenty a minute): every address is locked out.
        let mut l = Lockout::default();
        let step = 3200;
        for a in 0..MAX_ADDRESSES as u64 {
            assert_eq!(l.check(&format!("b{a}"), a * step, Some("999999"), CODE), Err(Refusal::BadCode), "{a}");
        }
        let t = MAX_ADDRESSES as u64 * step;
        assert!(matches!(l.check("b-new", t, Some("999999"), CODE), Err(Refusal::Locked(_))));
        assert!(matches!(l.check("b0", t + 1, Some(CODE), CODE), Err(Refusal::Locked(_))));
        // Addresses that guessed long ago are forgotten: no lock however many there were.
        let mut l = Lockout::default();
        for a in 0..(MAX_ADDRESSES as u64 * 2) {
            assert_eq!(l.check(&format!("c{a}"), a * (FORGET_MS / 100), Some("999999"), CODE), Err(Refusal::BadCode), "{a}");
        }
    }

    #[test]
    fn the_gate() {
        let mut l = Lockout::default();
        assert_eq!(gate(false, &mut l, "a", 0, Some(CODE), CODE, false), Err(Refusal::Off));
        assert_eq!(gate(true, &mut l, "a", 0, Some(CODE), CODE, false), Ok(()));
        assert_eq!(gate(true, &mut l, "a", 0, None, CODE, false), Err(Refusal::BadCode));
        // No code on the console (not made yet): nothing passes.
        assert_eq!(gate(true, &mut l, "a", 0, Some(""), "", false), Err(Refusal::BadCode));
        // Off answers before the lockout looks.
        for i in 0..MAX_WRONG as u64 {
            let _ = gate(true, &mut l, "z", i, Some("000001"), CODE, false);
        }
        assert_eq!(gate(false, &mut l, "z", 10, Some(CODE), CODE, false), Err(Refusal::Off));
        assert!(matches!(gate(true, &mut l, "z", 10, Some(CODE), CODE, false), Err(Refusal::Locked(_))));
        // Its own lock holds even with the receive's token.
        assert!(matches!(gate(true, &mut l, "z", 11, Some(CODE), CODE, true), Err(Refusal::Locked(_))));
    }

    #[test]
    fn the_receive_under_way_passes_the_lock_of_every_address() {
        let mut l = Lockout::default();
        // Twenty wrong codes from five addresses lock every address...
        for i in 0..GLOBAL_WRONG as u64 {
            let _ = l.check(&format!("10.0.0.{}", i % 5), i, Some("999999"), CODE);
        }
        assert!(matches!(gate(true, &mut l, "192.168.1.2", 100, Some(CODE), CODE, false), Err(Refusal::Locked(_))));
        // ...but a request with the token of the receive under way and the right code goes on; a wrong one does not.
        assert_eq!(gate(true, &mut l, "192.168.1.2", 101, Some(CODE), CODE, true), Ok(()));
        assert_eq!(gate(true, &mut l, "192.168.1.3", 102, Some("000000"), CODE, true), Err(Refusal::BadCode));
        assert_eq!(gate(true, &mut l, "192.168.1.3", 103, None, CODE, true), Err(Refusal::BadCode));
    }

    #[test]
    fn a_lockout_is_told_once() {
        let mut l = Lockout::default();
        for i in 0..MAX_WRONG as u64 - 1 {
            let _ = l.check("a", i, Some("000000"), CODE);
        }
        // Wrong codes alone are not told; the lock that comes of them is, once.
        assert!(l.take_events().is_empty());
        let _ = l.check("a", 10, Some("000000"), CODE);
        assert_eq!(l.take_events(), vec![LockEvent::Address("a".into())]);
        for i in 0..10 {
            let _ = l.check("a", 11 + i, Some("000000"), CODE);
        }
        assert!(l.take_events().is_empty());
        // Every address: told once, with how many wrong codes and from how many addresses.
        let mut l = Lockout::default();
        for i in 0..GLOBAL_WRONG as u64 {
            // Four from each of five addresses: no address reaches five in a row.
            let _ = l.check(&format!("10.0.0.{}", i % 5), i, Some("999999"), CODE);
        }
        assert_eq!(l.take_events(), vec![LockEvent::All { wrong: GLOBAL_WRONG, addresses: 5 }]);
        let _ = l.check("10.0.0.9", 50, Some("999999"), CODE);
        assert!(l.take_events().is_empty());
    }

    #[test]
    fn refusals_answer_as_the_contract_says() {
        let cases: Vec<(Refusal, u16, &str)> = vec![
            (Refusal::Off, 403, "receive_off"), (Refusal::BadCode, 401, "bad_code"), (Refusal::Locked(1500), 429, "locked"),
            (Refusal::BadRequest("x".into()), 400, "bad_request"), (Refusal::BadName, 400, "bad_name"),
            (Refusal::BadSize, 400, "bad_size"), (Refusal::NoDrive, 404, "no_drive"), (Refusal::Fat32, 400, "fat32"),
            (Refusal::Exists, 409, "exists"), (Refusal::Busy(Busy::Install), 409, "busy"),
            (Refusal::NoSpace { need: 5, have: 0 }, 507, "no_space"), (Refusal::DeleteFailed("x".into()), 500, "delete_failed"),
            (Refusal::UnknownId, 404, "unknown_id"), (Refusal::Offset(7), 409, "offset"), (Refusal::PastSize, 400, "past_size"),
            (Refusal::TooLarge, 413, "too_large"), (Refusal::LengthRequired, 411, "length_required"),
            (Refusal::State(State::Done), 409, "state"), (Refusal::Incomplete(3), 409, "incomplete"),
        ];
        for (r, status, code) in cases {
            assert_eq!((r.status(), r.code()), (status, code), "{r:?}");
            let j = r.json();
            assert_eq!(j["error"], code);
            assert!(!j["detail"].as_str().unwrap().is_empty(), "{r:?}");
        }
        assert_eq!(Refusal::Offset(7).json()["have"], 7);
        assert_eq!(Refusal::Incomplete(3).json()["have"], 3);
        assert_eq!(Refusal::NoSpace { need: 5, have: 0 }.json()["missing"], 5);
        assert_eq!(Refusal::Locked(1500).json()["retry_after"], 2);
        assert!(Refusal::Busy(Busy::Verify).detail().contains("verif"));
        assert!(Refusal::State(State::Verifying).detail().contains("verifying"));
    }

    #[test]
    fn what_the_quick_verify_found() {
        use crate::verify::{Quick, QuickBad};
        let q = Quick {
            ok: false, bytes: 9, total: 9, nbad: 70, version: 1, volumes: 3, files: 9,
            bad: vec![QuickBad { what: "volume", volume: Some(2), name: "ampr_zstd-002.pak".into(), expected: "aa".into(),
                found: None, why: "read".into() }],
        };
        let v = verify_progress(&q);
        assert_eq!(v, VerifyProgress { bytes: 9, total: 9, nbad: 70, bad: vec![BadItem { kind: "volume", volume: Some(2),
            name: "ampr_zstd-002.pak".into(), expected: "aa".into(), found: None, why: "read".into() }] });
        assert_eq!(failed_text(&q), "70 of 12 volumes and files do not match: volume ampr_zstd-002.pak (read) and 69 more");
        let one = Quick { nbad: 1, ..q.clone() };
        assert_eq!(failed_text(&one), "1 of 12 volumes and files do not match: volume ampr_zstd-002.pak (read)");
        let none_listed = Quick { nbad: 2, bad: vec![], ..q };
        assert_eq!(failed_text(&none_listed), "2 of 12 volumes and files do not match");
    }

    #[test]
    fn the_status() {
        let s = status_json(5, 10, State::Receiving, None, None);
        assert_eq!(s, json!({"have": 5, "size": 10, "state": "receiving", "verify": null, "error": null}));
        let v = VerifyProgress {
            bytes: 10, total: 10, nbad: 1,
            bad: vec![BadItem { kind: "volume", volume: Some(3), name: "ampr_zstd-003.pak".into(), expected: "aa".into(), found: Some("bb".into()), why: "sha256".into() }],
        };
        let s = status_json(10, 10, State::Failed, Some(&v), Some("1 of 12 volumes and files do not match"));
        // The shape of the PC's maxpack verify --quick --json: a volume by its number, a file by its path, no found
        // when nothing whole could be hashed.
        assert_eq!(s["verify"], json!({"bytes": 10, "total": 10, "nbad": 1, "bad": [
            {"volume": 3, "expected": "aa", "found": "bb", "why": "sha256"}]}));
        let v = VerifyProgress {
            bytes: 10, total: 10, nbad: 1,
            bad: vec![BadItem { kind: "file", volume: None, name: "sce_sys/param.json".into(), expected: "aa".into(), found: None, why: "data".into() }],
        };
        assert_eq!(status_json(10, 10, State::Failed, Some(&v), None)["verify"]["bad"],
            json!([{"path": "sce_sys/param.json", "expected": "aa", "why": "data"}]));
        assert_eq!(s["error"], "1 of 12 volumes and files do not match");
        assert_eq!(s["state"], "failed");
    }
}
