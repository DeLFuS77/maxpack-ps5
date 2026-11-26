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

/// Where a receive is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum State {
    Receiving,
    Verifying,
    Done,
    Failed,
    Cancelled,
}

impl State {
    pub fn name(self) -> &'static str {
        match self {
            State::Receiving => "receiving",
            State::Verifying => "verifying",
            State::Done => "done",
            State::Failed => "failed",
            State::Cancelled => "cancelled",
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
    /// Bytes short on the drive, the margin included.
    NoSpace(u64),
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
}

impl Refusal {
    pub fn status(&self) -> u16 {
        match self {
            Refusal::Off => 403,
            Refusal::BadCode => 401,
            Refusal::Locked(_) => 429,
            Refusal::BadRequest(_) | Refusal::BadName | Refusal::BadSize | Refusal::Fat32 | Refusal::PastSize => 400,
            Refusal::NoDrive | Refusal::UnknownId => 404,
            Refusal::Exists | Refusal::Busy(_) | Refusal::Offset(_) | Refusal::State(_) | Refusal::Incomplete(_) => 409,
            Refusal::NoSpace(_) => 507,
            Refusal::TooLarge => 413,
            Refusal::LengthRequired => 411,
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
            Refusal::NoSpace(_) => "no_space",
            Refusal::UnknownId => "unknown_id",
            Refusal::Offset(_) => "offset",
            Refusal::PastSize => "past_size",
            Refusal::TooLarge => "too_large",
            Refusal::LengthRequired => "length_required",
            Refusal::State(_) => "state",
            Refusal::Incomplete(_) => "incomplete",
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
            Refusal::NoSpace(n) => format!("not enough space on the drive: {n} more bytes needed (256 MiB are kept free)"),
            Refusal::UnknownId => "no receive with that id (the console restarted, or another receive took its place): start again".into(),
            Refusal::Offset(h) => format!("the data must go on at byte {h}"),
            Refusal::PastSize => "past the size given at start".into(),
            Refusal::TooLarge => format!("at most {} MiB per request", MAX_PUT >> 20),
            Refusal::LengthRequired => "a Content-Length is needed".into(),
            Refusal::State(s) => format!("the receive is {}", s.name()),
            Refusal::Incomplete(h) => format!("only {h} bytes arrived"),
        }
    }

    /// The answer's body: `{"error":code,"detail":...}`, with `have`, `missing` or `retry_after` (seconds) when it has one.
    pub fn json(&self) -> Value {
        let mut v = json!({"error": self.code(), "detail": self.detail()});
        match self {
            Refusal::Offset(h) | Refusal::Incomplete(h) => v["have"] = (*h).into(),
            Refusal::NoSpace(m) => v["missing"] = (*m).into(),
            Refusal::Locked(ms) => v["retry_after"] = ms.div_ceil(1000).into(),
            _ => {}
        }
        v
    }
}

/// A plain `.p5z` file name, as it will be in `<drive>/homebrew/`: 1 to MAX_NAME bytes ending in `.p5z` (any case)
/// after a name, no `/`, `\`, `..` or control character, none of `"*:<>?|` (which exFAT and FAT32 do not
/// store), and no leading `.` (the scan does not list a hidden file).
pub fn valid_name(name: &str) -> bool {
    !name.is_empty() && name.len() <= MAX_NAME && crate::p5z::is_p5z_name(name) && !name.starts_with('.')
        && !name.contains("..")
        && !name.chars().any(|c| c.is_control() || matches!(c, '/' | '\\' | '"' | '*' | ':' | '<' | '>' | '?' | '|'))
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
}

/// Where a new receive starts: the partial file's length when it is no longer than the size and did not fail
/// its verify in this run (then it is whole and damaged: sent again from the start), else 0.
pub fn resume_have(partial: Option<u64>, size: u64, failed_before: bool) -> u64 {
    match partial {
        Some(n) if n <= size && !failed_before => n,
        _ => 0,
    }
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
    if let Some(b) = f.busy {
        return Err(Refusal::Busy(b));
    }
    if f.partial == Some(u64::MAX) {
        return Err(Refusal::Exists);
    }
    let have = resume_have(f.partial, r.size, f.failed_before);
    if let Some(m) = missing(d.free, r.size, have) {
        return Err(Refusal::NoSpace(m));
    }
    Ok(Start::New { have })
}

/// A `PUT /api/receive/data` of `len` bytes at `offset` into a receive in `state` with `have` of `size`.
pub fn data_check(state: State, have: u64, size: u64, offset: u64, len: u64) -> Result<(), Refusal> {
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
    if state != State::Receiving {
        return Err(Refusal::State(state));
    }
    if have != size {
        return Err(Refusal::Incomplete(have));
    }
    Ok(())
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
    recent: VecDeque<u64>,
    all_until_ms: u64,
}

impl Lockout {
    /// `presented`: the `X-MaxPack-Code` header (None: absent, which is refused but not counted). `code`: the
    /// console's code. While locked out the code is not even compared.
    pub fn check(&mut self, key: &str, now_ms: u64, presented: Option<&str>, code: &str) -> Result<(), Refusal> {
        if self.all_until_ms > now_ms {
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
        self.recent.push_back(now_ms);
        while self.recent.front().is_some_and(|&t| now_ms.saturating_sub(t) >= LOCK_MS) {
            self.recent.pop_front();
        }
        if !self.per.contains_key(key) && self.per.len() >= MAX_ADDRESSES {
            self.per.retain(|_, t| t.until_ms > now_ms || now_ms.saturating_sub(t.last_ms) < FORGET_MS);
            if self.per.len() >= MAX_ADDRESSES {
                self.all_until_ms = now_ms + LOCK_MS;
                return Err(Refusal::Locked(LOCK_MS));
            }
        }
        let t = self.per.entry(key.to_string()).or_default();
        t.wrong += 1;
        t.last_ms = now_ms;
        if t.wrong >= MAX_WRONG {
            (t.wrong, t.until_ms) = (0, now_ms + LOCK_MS);
        }
        if self.recent.len() >= GLOBAL_WRONG {
            self.all_until_ms = now_ms + LOCK_MS;
            self.recent.clear();
        }
        Err(Refusal::BadCode)
    }
}

/// The gate of every `/api/receive/*` route: the switch, then the lockout and the code.
pub fn gate(on: bool, lock: &mut Lockout, key: &str, now_ms: u64, presented: Option<&str>, code: &str) -> Result<(), Refusal> {
    if !on {
        return Err(Refusal::Off);
    }
    lock.check(key, now_ms, presented, code)
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

/// `GET /api/receive/status`'s answer: `{"have","size","state","verify":{bytes,total,nbad,bad:[...]}|null,"error"}`.
pub fn status_json(have: u64, size: u64, state: State, verify: Option<&VerifyProgress>, error: Option<&str>) -> Value {
    let verify = verify.map(|v| {
        let bad: Vec<Value> = v.bad.iter().map(|b| json!({
            "kind": b.kind, "volume": b.volume, "name": b.name, "expected": b.expected, "found": b.found, "why": b.why,
        })).collect();
        json!({"bytes": v.bytes, "total": v.total, "nbad": v.nbad, "bad": bad})
    });
    json!({"have": have, "size": size, "state": state.name(), "verify": verify, "error": error})
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn states() {
        let all = [State::Receiving, State::Verifying, State::Done, State::Failed, State::Cancelled];
        let names: Vec<&str> = all.iter().map(|s| s.name()).collect();
        assert_eq!(names, ["receiving", "verifying", "done", "failed", "cancelled"]);
        let active: Vec<bool> = all.iter().map(|s| s.active()).collect();
        assert_eq!(active, [true, true, false, false, false]);
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
            "a?.p5z", "a<b>.p5z", "a\"b.p5z", "a|b.p5z", "/data/x.p5z", too_long.as_str()] {
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
        // A .maxpack-recv of an earlier receive (a restart, a broken connection) resumes where it is.
        assert_eq!(start_gate(&StartFacts { partial: Some(3 << 30), ..facts() }), Ok(Start::New { have: 3 << 30 }));
        // A finished one whose verify failed in this run, or one longer than the size, starts over.
        assert_eq!(start_gate(&StartFacts { partial: Some(10 << 30), failed_before: true, ..facts() }), Ok(Start::New { have: 0 }));
        assert_eq!(start_gate(&StartFacts { partial: Some((10 << 30) + 1), ..facts() }), Ok(Start::New { have: 0 }));
        // An ended receive of this run is no longer under way.
        for state in [State::Done, State::Failed, State::Cancelled] {
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
        assert_eq!(start_gate(&StartFacts { drive: tight.clone(), ..f() }), Err(Refusal::NoSpace(1)));
        assert_eq!(start_gate(&StartFacts { drive: tight, partial: Some(1), ..f() }), Ok(Start::New { have: 1 }));
        let full = Some(DriveFacts { free: 0, fat32: false, usable: true });
        assert_eq!(start_gate(&StartFacts { drive: full, busy: Some(Busy::Install), ..f() }), Err(Refusal::Busy(Busy::Install)));
    }

    #[test]
    fn resume_and_space() {
        assert_eq!(resume_have(None, 100, false), 0);
        assert_eq!(resume_have(Some(40), 100, false), 40);
        assert_eq!(resume_have(Some(100), 100, false), 100);
        assert_eq!(resume_have(Some(101), 100, false), 0);
        assert_eq!(resume_have(Some(40), 100, true), 0);
        assert_eq!(resume_have(Some(u64::MAX), 100, false), 0);
        assert_eq!(missing(MARGIN + 100, 100, 0), None);
        assert_eq!(missing(MARGIN + 99, 100, 0), Some(1));
        assert_eq!(missing(MARGIN, 100, 100), None);
        assert_eq!(missing(0, u64::MAX, 0), Some(u64::MAX));
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
        for s in [State::Receiving, State::Verifying, State::Failed, State::Cancelled] {
            assert_eq!(cancel_check(s), Ok(()));
        }
        assert_eq!(cancel_check(State::Done), Err(Refusal::State(State::Done)));
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
        assert_eq!(gate(false, &mut l, "a", 0, Some(CODE), CODE), Err(Refusal::Off));
        assert_eq!(gate(true, &mut l, "a", 0, Some(CODE), CODE), Ok(()));
        assert_eq!(gate(true, &mut l, "a", 0, None, CODE), Err(Refusal::BadCode));
        // No code on the console (not made yet): nothing passes.
        assert_eq!(gate(true, &mut l, "a", 0, Some(""), ""), Err(Refusal::BadCode));
        // Off answers before the lockout looks.
        for i in 0..MAX_WRONG as u64 {
            let _ = gate(true, &mut l, "z", i, Some("000001"), CODE);
        }
        assert_eq!(gate(false, &mut l, "z", 10, Some(CODE), CODE), Err(Refusal::Off));
        assert!(matches!(gate(true, &mut l, "z", 10, Some(CODE), CODE), Err(Refusal::Locked(_))));
    }

    #[test]
    fn refusals_answer_as_the_contract_says() {
        let cases: Vec<(Refusal, u16, &str)> = vec![
            (Refusal::Off, 403, "receive_off"), (Refusal::BadCode, 401, "bad_code"), (Refusal::Locked(1500), 429, "locked"),
            (Refusal::BadRequest("x".into()), 400, "bad_request"), (Refusal::BadName, 400, "bad_name"),
            (Refusal::BadSize, 400, "bad_size"), (Refusal::NoDrive, 404, "no_drive"), (Refusal::Fat32, 400, "fat32"),
            (Refusal::Exists, 409, "exists"), (Refusal::Busy(Busy::Install), 409, "busy"), (Refusal::NoSpace(5), 507, "no_space"),
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
        assert_eq!(Refusal::NoSpace(5).json()["missing"], 5);
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
        assert_eq!(s["verify"], json!({"bytes": 10, "total": 10, "nbad": 1, "bad": [
            {"kind": "volume", "volume": 3, "name": "ampr_zstd-003.pak", "expected": "aa", "found": "bb", "why": "sha256"}]}));
        assert_eq!(s["error"], "1 of 12 volumes and files do not match");
        assert_eq!(s["state"], "failed");
    }
}
