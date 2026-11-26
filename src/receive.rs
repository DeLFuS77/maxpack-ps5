//! Receive a `.p5z` from MaxPack on the PC (`maxpack send`; DESIGN.md "Receive from the PC", the shared contract):
//! the `/api/receive/*` routes of the page's server, each behind the switch (Settings > "Receive games from the
//! PC", `receive_from_pc`), the pairing code (`X-MaxPack-Code`, `receive_code`) and its lockout
//! (maxpack_logic::receive::Lockout), besides the page's own Host and Origin checks. The bytes go only to
//! `<drive>/homebrew/<name>.maxpack-recv` (a plain `.p5z` name, a drive the scan lists, never through a link),
//! pwrite'd at the offset the receive has and flushed before each answer; `finish` runs the engine's quick verify
//! on it (src/verify.rs `engine_quick`) on a thread of its own, and only a file that checks out is renamed to
//! `<name>` for the library's next pass. One receive at a time, never beside another job of the engine
//! (src/jobs.rs). Nothing else is written.
use crate::web::State;
use maxpack_logic::inst::header;
use maxpack_logic::job::Rate;
use maxpack_logic::receive::{self as logic, Active, DriveFacts, Refusal, Start, StartFacts, State as Stage, VerifyProgress};
use maxpack_logic::verify::QuickOutcome;
use serde_json::{json, Value};
use std::ffi::c_void;
use std::fs::{self, File, OpenOptions};
use std::io::{ErrorKind, Read, Write};
use std::net::{IpAddr, TcpStream};
use std::os::unix::fs::{FileExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, OnceLock};
use std::time::Duration;

/// An answer of `/api/receive/*`: a status and a JSON body.
pub struct Answer {
    pub status: u16,
    pub body: Value,
}

impl From<Refusal> for Answer {
    fn from(r: Refusal) -> Answer {
        Answer { status: r.status(), body: r.json() }
    }
}

fn ok(status: u16, body: Value) -> Answer {
    Answer { status, body }
}

/// The receive of this run (the last one stays until another starts or the payload quits).
struct Recv {
    /// The token the PC goes on with.
    id: String,
    /// For the page: which receive this is (the token is never shown).
    seq: u64,
    name: String,
    /// Its drive's id and mount.
    drive: String,
    mount: String,
    partial: PathBuf,
    dest: PathBuf,
    size: u64,
    have: u64,
    /// Where this receive started: more than 0 when it resumed a `.maxpack-recv`.
    from: u64,
    stage: Stage,
    verify: Option<VerifyProgress>,
    error: Option<String>,
    /// A stop of its verify: Some(true) deletes the file (a cancel), Some(false) keeps it (Stop, or the payload
    /// quits: the receive pauses).
    stop: Option<bool>,
    started_ms: u64,
    /// The last request that carried its token (or its start): idle for IDLE_MS, it pauses (`sweep`).
    last_ms: u64,
    ended_ms: Option<u64>,
    rate: Rate,
}

static RECV: Mutex<Option<Recv>> = Mutex::new(None);
/// Held by the PUT that writes (one at a time) and by finish and cancel while they take the file.
static WRITER: Mutex<()> = Mutex::new(());
static LOCK: Mutex<Option<logic::Lockout>> = Mutex::new(None);
/// The `.maxpack-recv` files whose verify failed in this run: a start of one again begins from 0.
static FAILED: Mutex<Vec<PathBuf>> = Mutex::new(Vec::new());
static SEQ: AtomicU64 = AtomicU64::new(0);
/// Mounts where a write failed with EFBIG in this run: FAT32 that the type check did not tell (start refuses it).
static FAT32_SEEN: Mutex<Vec<String>> = Mutex::new(Vec::new());
/// To wake the scan loop once a `.p5z` is in place.
static STATE: OnceLock<Arc<State>> = OnceLock::new();
/// How long a PUT, a finish or a cancel waits for a PUT whose connection is going (its reads time out in 5 s).
const WRITER_WAIT_MS: u64 = 10_000;
/// The longest a cancel or a quit waits for the verify to stop.
const STOP_WAIT_MS: u64 = 3000;
const PIECE: usize = 1 << 20;

fn recv() -> MutexGuard<'static, Option<Recv>> {
    RECV.lock().unwrap_or_else(|e| e.into_inner())
}

pub fn init(state: Arc<State>) {
    let _ = STATE.set(state);
}

/// RECV, with a receive that has been idle for IDLE_MS paused first (maxpack_logic::receive::idle): a PC that went
/// away never holds the other jobs for long, and its file stays for a send to go on with.
fn swept() -> MutexGuard<'static, Option<Recv>> {
    let mut g = recv();
    if let Some(r) = g.as_mut().filter(|r| logic::idle(r.stage, r.last_ms, crate::sys::uptime_ms())) {
        pause(r, None, &format!("nothing from the PC for {} min", logic::IDLE_MS / 60_000));
    }
    g
}

/// Ends a receive as paused: its file stays, its token is over (as after a restart), and a start of the same file
/// goes on where it was. `error`: what stopped it, for the page (a full drive...); None for idle or Stop.
fn pause(r: &mut Recv, error: Option<String>, why: &str) {
    (r.stage, r.error, r.ended_ms) = (Stage::Paused, error, Some(crate::sys::uptime_ms()));
    log!("receive: {}: paused at {} of {} bytes ({why}); {} stays: a send of it again goes on where it was", r.name, r.have,
        r.size, r.partial.display());
}

/// The receive whose token is `id`, while it is not paused (a paused receive's token is over, as after a restart:
/// 404 `unknown_id`, and the PC starts again); the request counts as the PC being there.
fn by_id<'a>(g: &'a mut Option<Recv>, id: &str) -> Option<&'a mut Recv> {
    let r = g.as_mut().filter(|r| r.id == id && r.stage != Stage::Paused)?;
    r.last_ms = crate::sys::uptime_ms();
    Some(r)
}

/// A receive is under way (receiving or verifying): every other job of the engine waits (src/jobs.rs).
pub fn active() -> bool {
    swept().as_ref().is_some_and(|r| r.stage.active())
}

/// A receive under way writes at or under `path` (a drive's mount): eject waits.
pub fn uses(path: &Path) -> bool {
    swept().as_ref().is_some_and(|r| r.stage.active() && r.partial.starts_with(path))
}

/// `n` random bytes: the system's /dev/urandom, else std's keyed SipHash (a secret per run) over a counter and
/// the clocks.
fn random(buf: &mut [u8]) {
    if File::open("/dev/urandom").and_then(|mut f| f.read_exact(buf)).is_ok() {
        return;
    }
    use std::hash::{BuildHasher, Hasher};
    static COUNT: AtomicU64 = AtomicU64::new(0);
    for chunk in buf.chunks_mut(8) {
        let mut h = std::collections::hash_map::RandomState::new().build_hasher();
        h.write_u64(COUNT.fetch_add(1, Ordering::Relaxed));
        h.write_u64(crate::sys::uptime_ms());
        h.write_u64(crate::verify::now_secs());
        chunk.copy_from_slice(&h.finish().to_le_bytes()[..chunk.len()]);
    }
}

/// A new pairing code (maxpack_logic::receive::code_from: even over the million codes).
pub fn new_code() -> String {
    loop {
        let mut b = [0u8; 4];
        random(&mut b);
        if let Some(c) = logic::code_from(u32::from_le_bytes(b)) {
            return c;
        }
    }
}

/// The gate of every `/api/receive/*` route: the switch, then the lockout and the code (maxpack_logic::receive::gate).
/// A wrong code is logged with its address, never with what was sent.
/// A request whose query names the token of the receive under way passes the lock of every address (its own
/// address's lock still holds): a stranger's flood of wrong codes must not stall a send. Lockouts are logged once
/// each when they start, with the address only; single wrong codes are not logged.
pub fn gate(state: &State, head: &str, query: &str, peer: IpAddr) -> Result<(), Refusal> {
    let (on, code) = {
        let s = state.settings.lock().unwrap_or_else(|e| e.into_inner());
        (s.receive_from_pc, s.receive_code.clone())
    };
    let presented = header(head, "x-maxpack-code");
    let key = logic::addr_key(peer);
    let carries_token = logic::query_param(query, "id")
        .is_some_and(|id| recv().as_ref().is_some_and(|r| r.id == id && r.stage.active()));
    let (r, events) = {
        let mut l = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let l = l.get_or_insert_with(logic::Lockout::default);
        (logic::gate(on, l, &key, crate::sys::uptime_ms(), presented, &code, carries_token), l.take_events())
    };
    for e in events {
        match e {
            logic::LockEvent::Address(k) => log!("receive: {k} locked out for {} s after {} wrong pairing codes in a row",
                logic::LOCK_MS / 1000, logic::MAX_WRONG),
            logic::LockEvent::All { wrong, addresses } => log!("receive: {wrong} wrong pairing codes from {addresses} \
                address(es) within a minute: every address locked out for {} s (a send under way goes on)", logic::LOCK_MS / 1000),
        }
    }
    r
}

/// The scan roots and exclusions of the settings, as the scan takes them.
fn roots(state: &State) -> (Vec<String>, Vec<String>) {
    let s = state.settings.lock().unwrap_or_else(|e| e.into_inner());
    let roots = if s.scan_paths.is_empty() { crate::storage::default_scan_roots() } else { s.scan_paths.clone() };
    (roots, s.exclude.clone())
}

/// A drive's homebrew can take the file: a folder, or not there yet (made at start); never a link or a file.
fn homebrew_ok(p: &Path) -> bool {
    match fs::symlink_metadata(p) {
        Ok(m) => m.is_dir(),
        Err(e) => e.kind() == ErrorKind::NotFound,
    }
}

/// The drives the PC may send to: each mounted drive whose homebrew the scan lists (DriveFacts::usable).
fn usable_drives(state: &State) -> Vec<(String, crate::storage::Drive)> {
    let (roots, exclude) = roots(state);
    crate::storage::drives().into_iter().filter_map(|d| {
        let id = logic::drive_id(&d.mount)?;
        let hb = format!("{}/homebrew", d.mount);
        (logic::scanned(&hb, &roots, &exclude) && homebrew_ok(Path::new(&hb))).then_some((id, d))
    }).collect()
}

/// GET /api/receive/drives.
pub fn drives(state: &State) -> Answer {
    let list: Vec<Value> = usable_drives(state).into_iter()
        .map(|(id, d)| json!({"id": id, "free": d.free, "total": d.total, "fs": d.fs}))
        .collect();
    ok(200, json!({"drives": list}))
}

/// POST /api/receive/start, body `{"name","size","drive"}` (maxpack_logic::receive::start_gate).
pub fn start(state: &State, body: &[u8]) -> Answer {
    let req = match logic::parse_start(body) {
        Ok(r) => r,
        Err(r) => return r.into(),
    };
    let mount = logic::drive_mount(&req.drive);
    let drive = mount.as_ref().and_then(|m| usable_or_not(state, m));
    let homebrew = mount.as_ref().map(|m| PathBuf::from(m).join("homebrew"));
    // Paths are made of the name only once it is known to be plain (start_gate refuses it otherwise).
    let (dest, partial) = match (&homebrew, logic::valid_name(&req.name)) {
        (Some(h), true) => (Some(h.join(&req.name)), Some(h.join(format!("{}{}", req.name, logic::PARTIAL_SUFFIX)))),
        _ => (None, None),
    };
    let dest_exists = dest.as_ref().is_some_and(|d| !matches!(fs::symlink_metadata(d), Err(e) if e.kind() == ErrorKind::NotFound));
    let partial_len = partial.as_ref().and_then(|p| match fs::symlink_metadata(p) {
        Ok(m) if m.is_file() => Some(m.len()),
        Err(e) if e.kind() == ErrorKind::NotFound => None,
        _ => Some(u64::MAX),
    });
    let failed_before = partial.as_ref().is_some_and(|p| FAILED.lock().unwrap_or_else(|e| e.into_inner()).contains(p));
    let _gate = crate::jobs::gate();
    let mut g = swept();
    let active = g.as_ref().map(|r| Active {
        state: r.stage, same: r.name == req.name && r.drive == req.drive && r.size == req.size, have: r.have,
    });
    // The switch again, under the gate: turning it off (set_on, which holds the gate) cannot race this start.
    if !state.settings.lock().unwrap_or_else(|e| e.into_inner()).receive_from_pc {
        return Refusal::Off.into();
    }
    let busy = maxpack_logic::job::busy(!crate::installer::idle(), crate::verify::running(), false);
    let facts = StartFacts { req: req.clone(), drive, dest_exists, partial: partial_len, failed_before, active, busy };
    let have = match logic::start_gate(&facts) {
        Ok(Start::Resume { have }) => {
            let id = g.as_mut().map(|r| {
                r.last_ms = crate::sys::uptime_ms();
                r.id.clone()
            }).unwrap_or_default();
            log!("receive: {} to {}: the PC goes on at {have} of {} bytes", req.name, req.drive, req.size);
            return ok(200, json!({"id": id, "have": have}));
        }
        Ok(Start::New { have }) => have,
        Err(r) => {
            log!("receive: {:?} ({} bytes) to {:?} refused: {}", req.name, req.size, req.drive, r.detail());
            return r.into();
        }
    };
    let (Some(homebrew), Some(dest), Some(partial), Some(mount)) = (homebrew, dest, partial, mount) else {
        return Refusal::NoDrive.into();
    };
    if let Err(e) = prepare(&homebrew, &partial, have) {
        log!("receive: {}: {e}", partial.display());
        return ok(500, json!({"error": "write", "detail": e.to_string()}));
    }
    FAILED.lock().unwrap_or_else(|e| e.into_inner()).retain(|p| p != &partial);
    let mut t = [0u8; 16];
    random(&mut t);
    let id = logic::token(&t);
    let now = crate::sys::uptime_ms();
    log!("receive: {} ({} bytes) to {}: {}", req.name, req.size, partial.display(),
        if have > 0 { format!("resuming the file there at {have} bytes") } else { "from the start".into() });
    *g = Some(Recv {
        id: id.clone(), seq: SEQ.fetch_add(1, Ordering::Relaxed) + 1, name: req.name, drive: req.drive, mount, partial, dest,
        size: req.size, have, from: have, stage: Stage::Receiving, verify: None, error: None, stop: None, started_ms: now,
        last_ms: now, ended_ms: None, rate: Rate::default(),
    });
    ok(200, json!({"id": id, "have": have}))
}

/// The start's view of the drive at `mount`: None when it is not mounted.
fn usable_or_not(state: &State, mount: &str) -> Option<DriveFacts> {
    let d = crate::storage::drives().into_iter().find(|d| d.mount == mount)?;
    let usable = usable_drives(state).iter().any(|(_, u)| u.mount == mount);
    let seen = FAT32_SEEN.lock().unwrap_or_else(|e| e.into_inner()).iter().any(|m| m == mount);
    Some(DriveFacts { free: d.free, fat32: logic::is_fat32(&d.fs) || seen, usable })
}

/// The homebrew folder made when missing, and the `.maxpack-recv` file there at `have` bytes (created, never
/// through a link).
fn prepare(homebrew: &Path, partial: &Path, have: u64) -> std::io::Result<()> {
    match fs::create_dir(homebrew) {
        Err(e) if e.kind() != ErrorKind::AlreadyExists => return Err(e),
        _ => {}
    }
    let f = OpenOptions::new().write(true).create(true).custom_flags(libc::O_NOFOLLOW).mode(0o666).open(partial)?;
    // A longer file (another of the same name, or a failed one) starts over; a resumed one is as long as `have`.
    if f.metadata()?.len() != have {
        f.set_len(have)?;
    }
    f.sync_all()
}

/// The id and the offset of a query.
fn id_of(query: &str) -> Result<&str, Refusal> {
    logic::query_param(query, "id").filter(|i| logic::valid_token(i)).ok_or_else(|| Refusal::BadRequest("expected ?id=<token>".into()))
}

/// WRITER, waited for up to WRITER_WAIT_MS.
fn writer() -> Option<MutexGuard<'static, ()>> {
    let deadline = crate::sys::uptime_ms() + WRITER_WAIT_MS;
    loop {
        match WRITER.try_lock() {
            Ok(g) => return Some(g),
            Err(std::sync::TryLockError::Poisoned(e)) => return Some(e.into_inner()),
            Err(std::sync::TryLockError::WouldBlock) if crate::sys::uptime_ms() < deadline => {
                std::thread::sleep(Duration::from_millis(50));
            }
            Err(_) => return None,
        }
    }
}

/// Reads and drops what is left of a refused body, `len` bytes of it at most (maxpack_logic::receive::drain_len),
/// so the client reads the answer rather than a reset connection. Never with WRITER held.
fn drain(conn: &mut TcpStream, pre: usize, len: u64) {
    let mut left = len.saturating_sub(pre as u64);
    let mut buf = vec![0u8; 64 << 10];
    while left > 0 {
        let want = buf.len().min(left as usize);
        match conn.read(&mut buf[..want]) {
            Ok(0) | Err(_) => return,
            Ok(n) => left -= n as u64,
        }
    }
}

/// PUT /api/receive/data?id=<token>&offset=<n>: the body (Content-Length `len`, of which `pre` came with the head)
/// written at `offset` of the `.maxpack-recv` file and flushed before the answer. `Expect: 100-continue` is
/// answered once the request passes. A refused body is read and dropped first only up to MAX_DRAIN and never for
/// the gate's refusals (maxpack_logic::receive::drain_len), with WRITER let go. A write that fails pauses the
/// receive (its file stays): a full drive answers 507 with what is missing, a file FAT32 cannot grow 400 `fat32`.
pub fn put(state: &State, head: &str, query: &str, pre: &[u8], len: Option<u64>, conn: &mut TcpStream, peer: IpAddr) -> Answer {
    let expect = header(head, "expect").is_some_and(|v| v.eq_ignore_ascii_case("100-continue"));
    let refuse = |conn: &mut TcpStream, r: Refusal| -> Answer {
        let n = logic::drain_len(&r, len.unwrap_or(0), expect);
        if n > 0 {
            drain(conn, pre.len(), n);
        }
        r.into()
    };
    if let Err(r) = gate(state, head, query, peer) {
        return refuse(conn, r);
    }
    let Some(len) = len else { return Refusal::LengthRequired.into() };
    let (id, offset) = match (id_of(query), logic::query_param(query, "offset").and_then(logic::parse_offset)) {
        (Ok(id), Some(o)) => (id.to_string(), o),
        _ => return refuse(conn, Refusal::BadRequest("expected ?id=<token>&offset=<n>".into())),
    };
    let Some(w) = writer() else {
        let have = recv().as_ref().filter(|r| r.id == id).map_or(0, |r| r.have);
        return refuse(conn, Refusal::Offset(have));
    };
    let checked = {
        let mut g = swept();
        match by_id(&mut g, &id) {
            None => Err(Refusal::UnknownId),
            Some(r) => logic::data_check(r.stage, r.have, r.size, offset, len)
                .map(|()| (r.partial.clone(), r.mount.clone(), r.size)),
        }
    };
    let (path, mount, size) = match checked {
        Ok(c) => c,
        Err(e) => {
            drop(w);
            return refuse(conn, e);
        }
    };
    if expect && conn.write_all(b"HTTP/1.1 100 Continue\r\n\r\n").is_err() {
        return Refusal::BadRequest("the connection went".into()).into();
    }
    let f = match OpenOptions::new().write(true).custom_flags(libc::O_NOFOLLOW).open(&path) {
        Ok(f) => f,
        Err(e) => {
            log!("receive: open {}: {e}", path.display());
            return paused_by(&id, format!("open {}: {e}", path.display()), ok(500, json!({"error": "write", "detail": e.to_string()})));
        }
    };
    let mut buf = vec![0u8; PIECE];
    let mut written = 0u64;
    let mut pre = &pre[..pre.len().min(len as usize)];
    // (the answer, and the error that pauses the receive when there is one)
    let mut failure: Option<(Answer, Option<String>)> = None;
    while written < len {
        let n = if !pre.is_empty() {
            let n = pre.len().min(PIECE);
            buf[..n].copy_from_slice(&pre[..n]);
            pre = &pre[n..];
            n
        } else {
            let want = buf.len().min((len - written) as usize);
            match conn.read(&mut buf[..want]) {
                Ok(0) | Err(_) => break,
                Ok(n) => n,
            }
        };
        if let Err(e) = f.write_all_at(&buf[..n], offset + written) {
            log!("receive: write {} at {}: {e}", path.display(), offset + written);
            let at = offset + written;
            failure = Some(match e.raw_os_error() {
                Some(libc::ENOSPC) => {
                    let free = crate::storage::free(&mount).unwrap_or(0);
                    let missing = logic::missing(free, size, at).unwrap_or(1);
                    (Refusal::NoSpace(missing).into(), Some(format!("the drive is full: {missing} more bytes needed")))
                }
                Some(libc::EFBIG) => {
                    FAT32_SEEN.lock().unwrap_or_else(|e| e.into_inner()).push(mount.clone());
                    (Refusal::Fat32.into(), Some("the drive keeps no file of 4 GiB or more (FAT32)".into()))
                }
                _ => (ok(500, json!({"error": "write", "detail": e.to_string()})), Some(format!("write: {e}"))),
            });
            break;
        }
        written += n as u64;
        let mut g = recv();
        match g.as_mut().filter(|r| r.id == id) {
            Some(r) if r.stage == Stage::Receiving => {
                r.have = offset + written;
                r.last_ms = crate::sys::uptime_ms();
                r.rate.add(r.last_ms, r.have - r.from);
            }
            // Cancelled or stopped meanwhile: a cancel deletes the file once this lets WRITER go.
            Some(r) => {
                failure = Some((Refusal::State(r.stage).into(), None));
                break;
            }
            None => {
                failure = Some((Refusal::UnknownId.into(), None));
                break;
            }
        }
    }
    // What arrived is on the disk before the answer (or before the PC asks again after a cut).
    if let Err(e) = f.sync_data() {
        log!("receive: flush {}: {e}", path.display());
        return paused_by(&id, format!("flush: {e}"), ok(500, json!({"error": "write", "detail": format!("flush: {e}")})));
    }
    match failure {
        Some((a, Some(why))) => return paused_by(&id, why, a),
        Some((a, None)) => return a,
        None => {}
    }
    if written < len {
        // The connection was cut: the bytes that came stay, and the PC goes on from `have`.
        return Refusal::BadRequest(format!("the body ended after {written} of {len} bytes")).into();
    }
    ok(200, json!({"have": offset + len}))
}

/// The receive `id` paused by a failed write (`why`, shown on the page), answered with `a`.
fn paused_by(id: &str, why: String, a: Answer) -> Answer {
    if let Some(r) = recv().as_mut().filter(|r| r.id == id && r.stage == Stage::Receiving) {
        pause(r, Some(why.clone()), &why);
    }
    a
}

fn status_of(r: &Recv) -> Value {
    logic::status_json(r.have, r.size, r.stage, r.verify.as_ref(), r.error.as_deref())
}

/// GET /api/receive/status?id=<token>.
pub fn status(query: &str) -> Answer {
    let id = match id_of(query) {
        Ok(i) => i,
        Err(r) => return r.into(),
    };
    match by_id(&mut swept(), id) {
        Some(r) => ok(200, status_of(r)),
        None => Refusal::UnknownId.into(),
    }
}

/// POST /api/receive/finish?id=<token>: every byte there (finish_check), the verify starts on its own thread: 202.
pub fn finish(query: &str) -> Answer {
    let id = match id_of(query) {
        Ok(i) => i.to_string(),
        Err(r) => return r.into(),
    };
    let Some(_w) = writer() else { return Refusal::State(Stage::Receiving).into() };
    let mut g = swept();
    let Some(r) = by_id(&mut g, &id) else { return Refusal::UnknownId.into() };
    if let Err(e) = logic::finish_check(r.stage, r.have, r.size) {
        return e.into();
    }
    r.stage = Stage::Verifying;
    r.verify = Some(VerifyProgress::default());
    r.rate = Rate::default();
    log!("receive: {}: every byte arrived ({} bytes, {} of them in this receive); verifying it", r.name, r.size, r.size - r.from);
    let body = status_of(r);
    drop(g);
    if let Err(e) = std::thread::Builder::new().name("receive-verify".into()).spawn(move || verify_received(&id)) {
        log!("receive: could not start the verify's thread: {e}");
        if let Some(r) = recv().as_mut() {
            (r.stage, r.error, r.ended_ms) = (Stage::Failed, Some(format!("could not start the verify: {e}")), Some(crate::sys::uptime_ms()));
        }
    }
    ok(202, body)
}

extern "C" fn on_verify(_ctx: *mut c_void, done: u64, total: u64) {
    if let Some(r) = recv().as_mut().filter(|r| r.stage == Stage::Verifying) {
        if let Some(v) = r.verify.as_mut() {
            (v.bytes, v.total) = (done, total);
        }
        r.rate.add(crate::sys::uptime_ms(), done);
    }
}

extern "C" fn verify_stopped(_ctx: *mut c_void) -> std::ffi::c_int {
    std::ffi::c_int::from(recv().as_ref().is_none_or(|r| r.stop.is_some()))
}

fn delete(partial: &Path) -> Result<(), String> {
    match fs::remove_file(partial) {
        Err(e) if e.kind() != ErrorKind::NotFound => Err(format!("delete {}: {e}", partial.display())),
        _ => Ok(()),
    }
}

/// The verify's thread: the file flushed, the engine's quick verify, then the rename (OK) or the failure.
fn verify_received(id: &str) {
    let Some((partial, dest, name, t0)) = recv().as_ref().filter(|r| r.id == id).map(|r| (r.partial.clone(), r.dest.clone(), r.name.clone(), r.started_ms))
    else { return };
    // On the disk before it is read back, and before it gets its name.
    if let Err(e) = File::open(&partial).and_then(|f| f.sync_all()) {
        log!("receive: fsync {}: {e}", partial.display());
    }
    let before = fs::metadata(&partial).ok();
    let outcome = crate::verify::engine_quick(&partial, on_verify, verify_stopped, std::ptr::null_mut());
    let mut g = recv();
    let Some(r) = g.as_mut().filter(|r| r.id == id) else { return };
    let now = crate::sys::uptime_ms();
    r.ended_ms = Some(now);
    let secs = (now - t0) / 1000;
    match outcome {
        Ok(QuickOutcome::Checked(q)) if q.ok && q.nbad == 0 && r.stop.is_none() => {
            r.verify = Some(logic::verify_progress(&q));
            // Renamed only onto nothing. ponytail: checked, then renamed; a file put there in between is replaced
            // (rename(2) has no no-replace flag here, and exFAT has no link(2) to do it with).
            if fs::symlink_metadata(&dest).is_ok() {
                r.stage = Stage::Failed;
                r.error = Some(format!("{} appeared meanwhile: what arrived stays as {}", dest.display(), partial.display()));
                log!("receive: {name}: {}", r.error.as_deref().unwrap_or(""));
                return;
            }
            if let Err(e) = fs::rename(&partial, &dest) {
                r.stage = Stage::Failed;
                r.error = Some(format!("rename {} to {}: {e}", partial.display(), dest.display()));
                log!("receive: {name}: {}", r.error.as_deref().unwrap_or(""));
                return;
            }
            if let Some(dir) = dest.parent() {
                if let Err(e) = File::open(dir).and_then(|d| d.sync_all()) {
                    log!("receive: fsync {}: {e} (the rename may not be on the disk yet)", dir.display());
                }
            }
            r.stage = Stage::Done;
            log!("receive: {name}: checks out ({} volumes and {} files, {} MB read) in {secs} s in all: now {}", q.volumes,
                q.files, q.bytes / 1_000_000, dest.display());
            drop(g);
            // The sheet shows it as verified quickly, as a "Verify quickly" there would.
            if let Some(m) = before {
                use std::os::unix::fs::MetadataExt;
                crate::verify::remember(&dest, maxpack_logic::verify::quick_record(&q, m.len(), m.mtime(), crate::verify::now_secs()));
            }
            if let Some(s) = STATE.get() {
                s.wake();
            }
        }
        Ok(QuickOutcome::Checked(q)) if r.stop.is_none() => {
            r.verify = Some(logic::verify_progress(&q));
            r.stage = Stage::Failed;
            r.error = Some(logic::failed_text(&q));
            FAILED.lock().unwrap_or_else(|e| e.into_inner()).push(partial.clone());
            log!("receive: {name}: damaged: {}; it stays as {}", r.error.as_deref().unwrap_or(""), partial.display());
        }
        Ok(QuickOutcome::Error(why)) | Err(why) if r.stop.is_none() => {
            r.stage = Stage::Failed;
            r.error = Some(why);
            FAILED.lock().unwrap_or_else(|e| e.into_inner()).push(partial.clone());
            log!("receive: {name}: could not be verified: {}; it stays as {}", r.error.as_deref().unwrap_or(""), partial.display());
        }
        // Stopped: Delete removes what arrived; Stop or a quit keeps it (paused: a send again verifies it again).
        _ if r.stop == Some(true) => {
            r.stage = Stage::Cancelled;
            if let Err(e) = delete(&partial) {
                log!("receive: {name}: {e}");
                r.error = Some(e);
            }
            log!("receive: {name}: cancelled while verifying; deleted {}", partial.display());
        }
        _ => pause(r, None, "stopped while verifying"),
    }
}

/// POST /api/receive/cancel?id=<token> (the PC) and POST /api/pairing/cancel (the page's "Delete what arrived", `id`
/// None: the receive of this run, a paused or failed one too): the file deleted, state `cancelled`
/// (maxpack_logic::receive::cancel_check: never a receive that is done). Only that receive's file: the receive is
/// told by its `seq`, so a start that comes while the cancel waits for a PUT to end keeps its file.
pub fn cancel(id: Option<&str>) -> Answer {
    let (stage, seq) = {
        let mut g = swept();
        let found = match id {
            Some(i) => by_id(&mut g, i),
            None => g.as_mut(),
        };
        let Some(r) = found else { return Refusal::UnknownId.into() };
        if let Err(e) = logic::cancel_check(r.stage) {
            return e.into();
        }
        let stage = match r.stage {
            Stage::Receiving | Stage::Failed | Stage::Paused => {
                // A PUT under way stops at its next piece (it sees the stage); the file goes below, once it has.
                (r.stage, r.ended_ms) = (Stage::Cancelled, Some(crate::sys::uptime_ms()));
                Stage::Receiving
            }
            Stage::Verifying => {
                r.stop = Some(true);
                Stage::Verifying
            }
            s => s,
        };
        (stage, r.seq)
    };
    match stage {
        Stage::Receiving => {
            let _w = writer();
            let mut g = recv();
            let Some(r) = g.as_mut().filter(|r| r.seq == seq) else {
                // Another receive started meanwhile (of the same file, from the PC): its file is not touched.
                log!("receive: a cancel found another receive in place: its file stays");
                return ok(200, json!({"state": Stage::Cancelled.name()}));
            };
            FAILED.lock().unwrap_or_else(|e| e.into_inner()).retain(|p| p != &r.partial);
            if let Err(e) = delete(&r.partial) {
                log!("receive: {}: {e}", r.name);
                r.error = Some(e);
            }
            log!("receive: {}: cancelled at {} of {} bytes; deleted {}", r.name, r.have, r.size, r.partial.display());
            ok(200, status_of(r))
        }
        Stage::Verifying => {
            wait_verify(seq);
            match recv().as_ref().filter(|r| r.seq == seq) {
                Some(r) => ok(200, status_of(r)),
                None => Refusal::UnknownId.into(),
            }
        }
        _ => match recv().as_ref().filter(|r| r.seq == seq) {
            Some(r) => ok(200, status_of(r)),
            None => Refusal::UnknownId.into(),
        },
    }
}

/// Waits (up to STOP_WAIT_MS) for receive `seq`'s verify to stop.
fn wait_verify(seq: u64) {
    let deadline = crate::sys::uptime_ms() + STOP_WAIT_MS;
    while recv().as_ref().is_some_and(|r| r.seq == seq && r.stage == Stage::Verifying) && crate::sys::uptime_ms() < deadline {
        std::thread::sleep(Duration::from_millis(50));
    }
}

/// POST /api/pairing/stop, the page's "Stop (keep what arrived)": the receive under way pauses (a verify under way
/// stops first); its file stays and a send of it again goes on where it was (maxpack_logic::receive::stop_check).
pub fn stop_receive() -> Answer {
    let seq = {
        let mut g = swept();
        let Some(r) = g.as_mut() else { return Refusal::UnknownId.into() };
        if let Err(e) = logic::stop_check(r.stage) {
            return e.into();
        }
        if r.stage == Stage::Verifying {
            r.stop = Some(false);
        } else {
            pause(r, None, "stopped from the page");
        }
        r.seq
    };
    wait_verify(seq);
    match recv().as_ref().filter(|r| r.seq == seq) {
        Some(r) => ok(200, status_of(r)),
        None => Refusal::UnknownId.into(),
    }
}

/// Before the payload quits: a verify under way stops and keeps the file (a start after the next one resumes it,
/// whole, and verifies it again); a PUT under way ends with the process, and its bytes stay.
pub fn stop() {
    let seq = {
        let mut g = recv();
        match g.as_mut() {
            Some(r) if r.stage == Stage::Verifying && r.stop.is_none() => {
                r.stop = Some(false);
                r.seq
            }
            _ => return,
        }
    };
    wait_verify(seq);
}

/// `/api/state` `receive`: the receive of this run for the page (never its token), or null.
pub fn json() -> Value {
    let g = swept();
    let Some(r) = g.as_ref() else { return Value::Null };
    let now = crate::sys::uptime_ms();
    let verify = r.verify.as_ref().map(|v| json!({
        "bytes": v.bytes, "total": v.total, "nbad": v.nbad,
        "bad": v.bad.iter().map(|b| json!({"what": b.kind, "name": b.name, "why": b.why})).collect::<Vec<_>>(),
    }));
    json!({
        "seq": r.seq,
        "state": r.stage.name(),
        "name": r.name,
        "drive": r.mount,
        "path": r.partial.to_string_lossy(),
        "dest": r.dest.to_string_lossy(),
        "size": r.size,
        "have": r.have,
        "from": r.from,
        "mbps": if r.stage.active() { r.rate.mbps(10_000) } else { None },
        "cancelling": r.stop == Some(true) && r.stage == Stage::Verifying,
        "verify": verify,
        "error": r.error,
        "elapsed_ms": r.ended_ms.unwrap_or(now).saturating_sub(r.started_ms),
    })
}

/// POST /api/pairing, body `on` or `off`: the switch, saved in config.toml; turned on with no code, a code is made.
/// Off is refused while a receive is under way (cancel it first).
pub fn set_on(state: &State, on: bool) -> Result<(), (u16, String)> {
    // Held across the check and the save: a start (which looks at the switch again under it) cannot slip in.
    let _gate = crate::jobs::gate();
    if !on && active() {
        return Err((409, "a game is arriving from the PC: stop it first".into()));
    }
    let mut cur = state.settings.lock().unwrap_or_else(|e| e.into_inner());
    // From the file, not state.settings: a scan pass may have put an older copy there.
    let mut s = crate::config::load();
    s.receive_from_pc = on;
    if on && !logic::valid_code(&s.receive_code) {
        s.receive_code = new_code();
    }
    if let Err(e) = crate::config::save(&s) {
        log!("receive: save {}: {e}", crate::config::CONFIG_PATH);
        return Err((500, e.to_string()));
    }
    log!("receive: games from the PC {}", if on { "accepted (with the pairing code)" } else { "refused" });
    *cur = s;
    Ok(())
}

/// POST /api/pairing/new: a new code; the old one stops working at once.
pub fn renew(state: &State) -> Result<(), (u16, String)> {
    let mut cur = state.settings.lock().unwrap_or_else(|e| e.into_inner());
    let mut s = crate::config::load();
    s.receive_code = new_code();
    if let Err(e) = crate::config::save(&s) {
        log!("receive: save {}: {e}", crate::config::CONFIG_PATH);
        return Err((500, e.to_string()));
    }
    log!("receive: a new pairing code was made");
    *cur = s;
    Ok(())
}
