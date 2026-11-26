//! Tiny HTTP/1.1 server for the web UI opened in the PS5 browser (Connection: close, no keep-alive).
//! Routes: DESIGN.md "Web API".
use crate::config::{self, Settings};
use crate::scan::{kind_names, Image, Kind, Source};
use crate::{cache, register, storage, sys};
use maxpack_logic::mounter::MounterState;
use std::borrow::Cow;
use std::collections::HashSet;
use std::fs;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

pub const PORT: u16 = 8095;
const MAX_REQUEST: usize = 16 << 10;
const LOG_LINES: usize = 100;
const UI: &str = include_str!("ui.html");
const APP_ICON: &[u8] = include_bytes!("../ps5/icon0.png");

#[derive(Clone)]
pub struct Game {
    pub src: Source,
    /// "pending" | "checking" | "building" | "ready" | "error" | "duplicate"
    pub status: &'static str,
    pub error: Option<String>,
    /// Set with status "error" when the title id is installed on the console
    /// (register::installed_copy): the path that shows it.
    pub installed_copy: Option<String>,
    /// Set with status "error" for an error the page names in its language: "no_space" (a .p5z's
    /// loose files do not fit on /data: cache::NoSpace, tried again every pass), "old_ng" (a .p5z
    /// that grew, and an ampr-emu-ng in /data/maxpack/ampr that does not play it: cache::OldNg).
    pub error_code: Option<&'static str>,
    /// With "no_space": the bytes missing, the margin included.
    pub missing_bytes: Option<u64>,
    pub cache_size: u64,
    pub cache_built: Option<u64>,
}

pub struct State {
    /// The last scan: library games first, then duplicates.
    pub games: Mutex<Vec<Game>>,
    pub settings: Mutex<Settings>,
    /// Title ids to rebuild on the next scan.
    pub forced: Mutex<HashSet<String>>,
    /// Purge requested: done by the scan loop, so it never races a build.
    pub purge: AtomicBool,
    pub busy: AtomicBool,
    /// Whether MaxPack mounts now (main::refresh_mounter: every 2 s and at each scan loop turn):
    /// /api/state `waiting`, and launch::watch overlays nothing new unless Ready.
    pub mounter: Arc<Mutex<MounterState>>,
    /// ShadowMountPlus images of the last scan: listed for the UI, not mounted.
    pub images: Mutex<Vec<Image>>,
    /// .p5z files of the last scan that cannot be played (damaged, too many volumes): listed for the UI.
    pub unplayable: Mutex<Vec<(PathBuf, maxpack_logic::p5z::Unplayable)>>,
    /// Held by the scan loop for a whole pass (scan, purge, build, register); delete takes it.
    pub work: Mutex<()>,
    pub low_space: AtomicBool,
    pub cache_total: Mutex<u64>,
    /// Title ids registered by this run or with a built cache; launch::watch overlays only these.
    pub titles: Arc<Mutex<HashSet<String>>>,
    /// Title ids with a live game session (launch::watch); the scan loop pauses while non-empty.
    pub running: Arc<Mutex<HashSet<String>>>,
    rescan: Mutex<bool>,
    wake: Condvar,
}

impl State {
    pub fn new(settings: Settings) -> State {
        State {
            games: Mutex::new(Vec::new()),
            settings: Mutex::new(settings),
            forced: Mutex::new(HashSet::new()),
            purge: AtomicBool::new(false),
            busy: AtomicBool::new(false),
            // Nothing mounts before main::wait_ready has looked; from Ready, its first look toasts
            // what it waits for.
            mounter: Arc::new(Mutex::new(MounterState::Ready)),
            images: Mutex::new(Vec::new()),
            unplayable: Mutex::new(Vec::new()),
            work: Mutex::new(()),
            low_space: AtomicBool::new(false),
            cache_total: Mutex::new(0),
            titles: Arc::new(Mutex::new(HashSet::new())),
            running: Arc::new(Mutex::new(HashSet::new())),
            rescan: Mutex::new(false),
            wake: Condvar::new(),
        }
    }

    /// Updates the entry of the source at `path` (if still listed).
    pub fn update(&self, path: &Path, f: impl FnOnce(&mut Game)) {
        if let Some(g) = self.games.lock().unwrap().iter_mut().find(|g| g.src.path == path) {
            f(g);
        }
    }

    /// Wakes the scan loop now.
    pub fn wake(&self) {
        *self.rescan.lock().unwrap() = true;
        self.wake.notify_all();
    }

    /// Blocks until `wake` or `timeout`, then clears the flag.
    pub fn wait(&self, timeout: Duration) {
        let flag = self.rescan.lock().unwrap();
        let (mut flag, _) = self.wake.wait_timeout_while(flag, timeout, |f| !*f).unwrap();
        *flag = false;
    }

    /// A game runs: no scan/build/register/purge until it exits (requests stay queued).
    pub fn paused(&self) -> bool {
        !self.running.lock().unwrap().is_empty()
    }

    /// MaxPack mounts now: kstuff answers and no other mounter runs.
    pub fn mounts(&self) -> bool {
        self.mounter.lock().unwrap().mounts()
    }

    /// Another mounter runs: MaxPack only installs (its own mounting pauses, the installer does not).
    pub fn installer_only(&self) -> bool {
        matches!(*self.mounter.lock().unwrap(), MounterState::OtherMounter(_))
    }

    /// Title ids of the library (duplicates excluded).
    pub fn library(&self) -> Vec<String> {
        let games = self.games.lock().unwrap();
        games.iter().filter(|g| g.status != "duplicate").map(|g| g.src.title_id.clone()).collect()
    }

    pub fn source(&self, tid: &str) -> Option<Source> {
        let games = self.games.lock().unwrap();
        games.iter().find(|g| g.src.title_id == tid && g.status != "duplicate").map(|g| g.src.clone())
    }
}

/// What /api/state says of each game beyond its Game entry.
struct GameExtra<'a> {
    /// The settings: the per-title lists (`hidden`, `overlay_off`, and the Fakelib mode and the PlayGo
    /// stub through src/recommended.rs) and `add_emulators` (off: no stub at all, so the UI shows no switch).
    settings: &'a Settings,
    /// The last crash of each title (crashes.json).
    crashes: &'a std::collections::HashMap<String, maxpack_logic::crash::Crash>,
    /// Title ids whose rebuild was asked for and has not started yet (`forced`).
    queued: &'a HashSet<String>,
}

fn game_json(g: &Game, x: &GameExtra) -> serde_json::Value {
    let (kind, packing) = kind_names(g.src.kind);
    let tid = &g.src.title_id;
    let s = x.settings;
    let crash = x.crashes.get(tid).map(|c| serde_json::json!({"at": c.at, "process": c.process, "status": c.status}));
    // The Fakelib mode and the PlayGo stub as used (the user's, else the recommended settings'), and what
    // the list recommends, when it says anything of the two (the sheet's "Back to the recommended setting").
    let eff = crate::recommended::effective(tid, s);
    let recommended = crate::recommended::entry(tid).filter(|e| e.fakelib.is_some() || e.playgo_off.is_some())
        .map(|e| serde_json::json!({"fakelib": e.fakelib.map(|m| m.name()), "playgo_off": e.playgo_off}));
    serde_json::json!({
        "title_id": g.src.title_id,
        "title_name": g.src.title_name,
        "version": g.src.version,
        "path": g.src.path.to_string_lossy(),
        "kind": kind,
        "packing": packing,
        "drive": g.src.drive,
        "size": g.src.size,
        "status": g.status,
        "error": g.error,
        // The path that shows an installed copy of this title id (main::process): the UI says why.
        "installed_copy": g.installed_copy,
        "error_code": g.error_code,
        "missing_bytes": g.missing_bytes,
        "cache_size": g.cache_size,
        "cache_built": g.cache_built,
        "backport": g.src.backport.as_ref().map(|b| b.to_string_lossy()),
        // ponytail: reads the game's .ampr on every state poll (a few hundred bytes per game).
        "playgo": if s.add_emulators { crate::cache::playgo_import(&g.src.title_id) } else { None },
        "playgo_off": s.add_emulators && eff.playgo_off,
        // The page shows it only with "show hidden" on; nothing else changes for the game.
        "hidden": s.hidden.iter().any(|t| t == tid),
        "crash": crash,
        // The page keeps Rebuild disabled through it (and while pending, checking or building).
        "rebuild_queued": x.queued.contains(tid),
        // Listed in overlay_off and a plain folder game (as cache::overlay_off): no overlay at launch.
        "overlay_off": maxpack_logic::overlay::off(kind, packing, s.overlay_off.iter().any(|t| t == tid)),
        // Listed, whatever this copy is: the page shows the switch for a stale entry too, to clear it.
        "overlay_off_listed": s.overlay_off.iter().any(|t| t == tid),
        // What its fakelib/ is composed from (maxpack_logic::fakelib): "full", "emulators" or "off".
        "fakelib_mode": eff.fakelib.name(),
        // Where the Fakelib mode and the PlayGo stub come from: "default", "user" or "recommended".
        "settings_source": eff.source.name(),
        // {"fakelib": mode or null, "playgo_off": bool or null}, or null when the list says nothing of them.
        "recommended": recommended,
        // The list's notice for the game ("long_first_boot"), or null.
        "notice": eff.notice.map(|n| n.name()),
        // A .p5z's last verify on the console while the file is the one verified (src/verify.rs), else null.
        // ponytail: one stat per .p5z game on every state poll.
        "verified": if g.src.kind == Kind::P5z { crate::verify::last(&g.src.path).map(|r| crate::verify::record_json(&r)) } else { None },
    })
}

/// Why take_port gave up.
pub enum PortTaken {
    /// The older copy answered the last quit with "a scan or build is in progress": it is finishing
    /// a build, so trying again in a moment works.
    Building,
    /// Anything else: the phase-2 maxpack.elf (whose server refuses that quit), an older build
    /// without /api/quit, an older copy with a game running, or another app.
    Busy,
}

/// Single instance: binds PORT, asking an older copy on it to quit first (POST /api/quit),
/// retrying for 5 s. Err: the port stays taken, so this copy must not run.
pub fn take_port() -> Result<TcpListener, PortTaken> {
    if let Ok(l) = TcpListener::bind(("0.0.0.0", PORT)) {
        return Ok(l);
    }
    let mut building = false;
    for _ in 0..20 {
        // Asked again each try: the older copy refuses while a scan pass (a build) runs.
        if let Ok(mut s) = TcpStream::connect(("127.0.0.1", PORT)) {
            // Its answer (Connection: close) says why it stays. It answers at once: a short wait.
            let _ = s.set_read_timeout(Some(Duration::from_millis(500)));
            if s.write_all(b"POST /api/quit HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n").is_ok() {
                let mut answer = Vec::new();
                let _ = (&mut s).take(4096).read_to_end(&mut answer);
                building = maxpack_logic::http::quit_refused_for_build(&answer);
            }
        }
        std::thread::sleep(Duration::from_millis(250));
        if let Ok(l) = TcpListener::bind(("0.0.0.0", PORT)) {
            return Ok(l);
        }
    }
    Err(if building { PortTaken::Building } else { PortTaken::Busy })
}

/// Listens on 0.0.0.0:PORT forever, one short-lived thread per connection. (The home-screen tile
/// that opens the UI is main::register_tile_when_mounting's.)
pub fn serve(state: Arc<State>, mut listener: TcpListener) {
    log!("web UI on port {PORT}");
    loop {
        match listener.accept() {
            Ok((conn, _)) => {
                let state = state.clone();
                std::thread::spawn(move || {
                    let _ = handle(&state, conn);
                });
            }
            Err(e) => {
                // Rest mode can close the listening socket (#110: after a wake the UI was gone and a
                // second copy could bind the port). ponytail: any accept error rebinds (a transient one
                // only drops the backlog); a dead socket whose accept blocks without an error is not seen.
                log!("web: accept on port {PORT} failed: {e}; binding it again");
                drop(listener);
                listener = rebind(&state);
                log!("web UI on port {PORT} again");
            }
        }
    }
}

/// Binds PORT again, retrying every second. A newer copy that took the port meanwhile wins: this
/// one quits, unless a game runs (its overlay would stay mounted; /api/quit refuses then too).
fn rebind(state: &State) -> TcpListener {
    let mut logged = false;
    loop {
        std::thread::sleep(Duration::from_secs(1));
        match TcpListener::bind(("0.0.0.0", PORT)) {
            Ok(l) => return l,
            Err(e) if e.kind() == std::io::ErrorKind::AddrInUse && !state.paused() => {
                log!("web: port {PORT} taken by another copy; quitting");
                crate::verify::stop();
                crate::receive::stop();
                crate::installer::stop();
                std::process::exit(0)
            }
            Err(e) if !logged => {
                logged = true;
                log!("web: bind port {PORT} failed: {e}; retrying every second");
            }
            Err(_) => {}
        }
    }
}

struct Reply {
    code: u16,
    /// Owned only for what the installer answers (installer::Answer).
    ctype: Cow<'static, str>,
    body: Vec<u8>,
    /// "no-cache" for what the browser's AppCache keeps (page, manifest, images), else "no-store".
    cache: &'static str,
}

fn reply(code: u16, ctype: impl Into<Cow<'static, str>>, body: impl Into<Vec<u8>>) -> Reply {
    Reply { code, ctype: ctype.into(), body: body.into(), cache: "no-store" }
}

/// For the AppCache: revalidated on every load, so a new payload's page replaces the kept one.
fn cached(ctype: &'static str, body: impl Into<Vec<u8>>) -> Reply {
    Reply { cache: "no-cache", ..reply(200, ctype, body) }
}

/// The AppCache manifest: the tile's page opens offline (no payload yet) and starts the saved ELF.
/// Its comment changes with the version and the page, so a new build's page is fetched again.
fn appcache() -> String {
    let hash = UI.bytes().fold(0xcbf29ce484222325u64, |h, b| (h ^ b as u64).wrapping_mul(0x100000001b3));
    format!("CACHE MANIFEST\n# {} {hash:016x}\nCACHE:\n/\n/app-icon.png\nNETWORK:\n*\n", env!("CARGO_PKG_VERSION"))
}

fn no_content() -> Reply {
    reply(204, "", "")
}

fn not_found() -> Reply {
    reply(404, "text/plain", "not found")
}

fn handle(state: &State, mut conn: TcpStream) -> std::io::Result<()> {
    conn.set_read_timeout(Some(Duration::from_secs(5)))?;
    conn.set_write_timeout(Some(Duration::from_secs(10)))?;
    let peer = conn.peer_addr().map(|a| a.ip()).unwrap_or(std::net::IpAddr::V4(std::net::Ipv4Addr::UNSPECIFIED));
    let loopback = peer.is_loopback();
    let r = match read_request(&mut conn)? {
        Ok(Request::Whole(head, body)) => match maxpack_logic::http::check(&head) {
            Ok(()) => route(state, &head, &body, loopback, peer),
            Err(why) => refused(&head, why),
        },
        // PUT /api/receive/data: its body (up to 64 MiB) goes to the file as it comes (src/receive.rs).
        Ok(Request::Stream { head, pre, len }) => match maxpack_logic::http::check(&head) {
            Ok(()) => {
                let target = head.lines().next().unwrap_or("").split(' ').nth(1).unwrap_or("");
                let query = target.split_once('?').map_or("", |(_, q)| q);
                receive_reply(crate::receive::put(state, &head, query, &pre, len, &mut conn, peer))
            }
            Err(why) => refused(&head, why),
        },
        Err(code) => reply(code, "text/plain", "bad or too large request"),
    };
    let reason = match r.code {
        200 => "OK",
        202 => "Accepted",
        204 => "No Content",
        400 => "Bad Request",
        401 => "Unauthorized",
        403 => "Forbidden",
        404 => "Not Found",
        409 => "Conflict",
        411 => "Length Required",
        413 => "Content Too Large",
        429 => "Too Many Requests",
        431 => "Request Header Fields Too Large",
        500 => "Internal Server Error",
        502 => "Bad Gateway",
        503 => "Service Unavailable",
        507 => "Insufficient Storage",
        _ => "Error",
    };
    let mut head = format!("HTTP/1.1 {} {reason}\r\nConnection: close\r\n", r.code);
    if r.code != 204 {
        head += &format!("Content-Type: {}\r\nContent-Length: {}\r\n", r.ctype, r.body.len());
    }
    head += &format!("Cache-Control: {}\r\n\r\n", r.cache);
    conn.write_all(head.as_bytes())?;
    if r.code != 204 {
        conn.write_all(&r.body)?;
    }
    conn.flush()
}

/// A request read: whole (head and body), or the head of PUT /api/receive/data, whose body is left on the
/// connection for src/receive.rs to write as it comes (`pre`: the part read with the head; `len`: Content-Length,
/// None when there is none).
enum Request {
    Whole(String, Vec<u8>),
    Stream { head: String, pre: Vec<u8>, len: Option<u64> },
}

/// The receive's data route, by its request line: its body is not held in memory.
fn streams(head: &str) -> bool {
    let line = head.lines().next().unwrap_or("");
    let mut parts = line.split(' ');
    parts.next() == Some("PUT") && parts.next().is_some_and(|t| t.split('?').next() == Some("/api/receive/data"))
}

/// Request line + headers, and the body by Content-Length; Err(status) when the head exceeds
/// MAX_REQUEST (431), the whole request would (413), or Content-Length is not a number (400). The receive's
/// data route (`streams`) comes back with its head only: its body may be up to 64 MiB; a chunked body is refused
/// (411, as no length).
fn read_request(conn: &mut TcpStream) -> std::io::Result<Result<Request, u16>> {
    let mut buf = Vec::with_capacity(1024);
    let mut chunk = [0u8; 2048];
    // (end of the head, end of the body)
    let mut need: Option<(usize, usize)> = None;
    loop {
        if need.is_none() {
            if let Some(end) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
                let head = String::from_utf8_lossy(&buf[..end]).into_owned();
                let len = head.lines().find_map(|l| {
                    let (k, v) = l.split_once(':')?;
                    k.trim().eq_ignore_ascii_case("content-length").then(|| v.trim().parse::<usize>())
                });
                if streams(&head) {
                    let chunked = maxpack_logic::inst::header(&head, "transfer-encoding").is_some();
                    let len = match (len, chunked) {
                        (Some(Ok(n)), false) => Some(n as u64),
                        (Some(Err(_)), _) => return Ok(Err(400)),
                        _ => None,
                    };
                    return Ok(Ok(Request::Stream { pre: buf[end + 4..].to_vec(), head, len }));
                }
                let len = match len {
                    None => 0,
                    Some(Ok(n)) => n,
                    Some(Err(_)) => return Ok(Err(400)),
                };
                // checked: a huge Content-Length must not wrap (panic=abort would end the payload).
                match (end + 4).checked_add(len) {
                    Some(n) if n <= MAX_REQUEST => need = Some((end, n)),
                    _ => return Ok(Err(413)),
                }
            }
        }
        match need {
            Some((end, n)) if buf.len() >= n => {
                let head = String::from_utf8_lossy(&buf[..end]).into_owned();
                return Ok(Ok(Request::Whole(head, buf[end + 4..n].to_vec())));
            }
            None if buf.len() > MAX_REQUEST => return Ok(Err(431)),
            _ => {}
        }
        let n = conn.read(&mut chunk)?;
        if n == 0 {
            return Err(std::io::ErrorKind::UnexpectedEof.into());
        }
        buf.extend_from_slice(&chunk[..n]);
    }
}

/// 403 for a request that maxpack_logic::http::check refuses: a Host that is not an IP or
/// localhost (the page opened by a name, or DNS rebinding), or a state change from another site.
/// The installer's routes answer as the engine does (JSON, code `forbidden`), the rest plain text in
/// Spanish and English (it may be the page itself that is refused). Logged at most once a minute.
fn refused(head: &str, why: maxpack_logic::http::Refusal) -> Reply {
    use maxpack_logic::http::Refusal;
    use maxpack_logic::inst::header;
    static LOGGED: Mutex<Option<std::time::Instant>> = Mutex::new(None);
    let line = head.lines().next().unwrap_or("");
    {
        let mut last = LOGGED.lock().unwrap_or_else(|e| e.into_inner());
        if last.is_none_or(|t| t.elapsed() >= Duration::from_secs(60)) {
            *last = Some(std::time::Instant::now());
            log!("web: refused {line:?} ({why:?}): Host {:?}, Origin {:?}, Sec-Fetch-Site {:?} (one line a minute at most)",
                header(head, "host").unwrap_or(""), header(head, "origin"), header(head, "sec-fetch-site"));
        }
    }
    let (es, en) = match why {
        Refusal::Host => (
            "MaxPack: abre esta página con la dirección IP de la consola (http://<IP de la PS5>:8095), no con un nombre.",
            "MaxPack: open this page at the console's IP address (http://<PS5 IP>:8095), not by a name.",
        ),
        Refusal::CrossSite => (
            "MaxPack: petición rechazada: no viene de la página de MaxPack.",
            "MaxPack: request refused: it did not come from the MaxPack page.",
        ),
    };
    if line.split(' ').nth(1).is_some_and(|t| t.starts_with("/api/inst/")) {
        let body = serde_json::json!({"ok": false, "code": "forbidden", "detail": es}).to_string();
        return reply(403, "application/json", body);
    }
    // The receive's routes answer the PC in the contract's shape.
    if line.split(' ').nth(1).is_some_and(|t| t.starts_with("/api/receive/")) {
        return reply(403, "application/json", serde_json::json!({"error": "forbidden", "detail": en}).to_string());
    }
    reply(403, "text/plain; charset=utf-8", format!("{es}\n{en}\n"))
}

/// POST /api/pairing and /api/pairing/new: `act` when maxpack_logic::receive::pairing_allowed says so (turning
/// receiving on and a new code only from the console's own browser), else 403 `{code:"console_only", detail}`.
fn pairing(p: maxpack_logic::receive::Pairing, loopback: bool, act: impl FnOnce() -> Result<(), (u16, String)>) -> Reply {
    if !maxpack_logic::receive::pairing_allowed(p, loopback) {
        log!("receive: {p:?} refused: only from the console's own browser");
        let body = serde_json::json!({"code": "console_only", "detail": "only from MaxPack's page on the console itself"});
        return reply(403, "application/json", body.to_string());
    }
    match act() {
        Ok(()) => no_content(),
        Err((code, why)) => reply(code, "text/plain", why),
    }
}

/// An answer of src/receive.rs as JSON.
fn receive_reply(a: crate::receive::Answer) -> Reply {
    reply(a.status, "application/json", a.body.to_string())
}

/// `/api/receive/*`, the PC's routes (src/receive.rs; DESIGN.md "Receive from the PC"): each behind the switch,
/// the pairing code and its lockout. The data PUT never comes here: `handle` streams it.
fn receive_route(state: &State, head: &str, method: &str, path: &str, query: &str, body: &[u8], peer: std::net::IpAddr) -> Reply {
    use crate::receive as rx;
    if let Err(r) = rx::gate(state, head, query, peer) {
        return receive_reply(r.into());
    }
    let a = match (method, path) {
        ("GET", "/api/receive/drives") => rx::drives(state),
        ("POST", "/api/receive/start") => rx::start(state, body),
        ("GET", "/api/receive/status") => rx::status(query),
        ("POST", "/api/receive/finish") => rx::finish(query),
        ("POST", "/api/receive/cancel") => match maxpack_logic::receive::query_param(query, "id") {
            Some(id) if maxpack_logic::receive::valid_token(id) => rx::cancel(Some(id)),
            _ => maxpack_logic::receive::Refusal::BadRequest("expected ?id=<token>".into()).into(),
        },
        _ => return reply(404, "application/json", serde_json::json!({"error": "not_found", "detail": "no such route"}).to_string()),
    };
    receive_reply(a)
}

fn route(state: &State, head: &str, body: &[u8], loopback: bool, peer: std::net::IpAddr) -> Reply {
    let mut parts = head.lines().next().unwrap_or("").split(' ');
    let method = parts.next().unwrap_or("");
    let target = parts.next().unwrap_or("");
    let (path, query) = target.split_once('?').unwrap_or((target, ""));
    match (method, path) {
        // The installer's routes (also in installer-only mode: never gated on the mounter state).
        (_, p) if p.starts_with("/api/inst/") => installer(state, head, method, target, body, loopback),
        // The PC's routes to send a .p5z here (also in installer-only mode: they mount nothing).
        (_, p) if p.starts_with("/api/receive/") => receive_route(state, head, method, path, query, body, peer),
        ("GET", "/") => cached("text/html; charset=utf-8", UI.replace("__VERSION__", env!("CARGO_PKG_VERSION"))),
        ("GET", "/maxpack.appcache") => cached("text/cache-manifest", appcache()),
        ("GET", "/app-icon.png") => cached("image/png", APP_ICON),
        ("GET", "/api/state") => reply(200, "application/json", api_state(state, loopback)),
        // Settings > "Receive games from the PC" (src/receive.rs): the switch and a new pairing code.
        ("POST", "/api/pairing") => match maxpack_logic::titles::switch_body(body) {
            None => reply(400, "text/plain", "expected on or off"),
            Some(on) => {
                use maxpack_logic::receive::Pairing;
                pairing(if on { Pairing::On } else { Pairing::Off }, loopback, || crate::receive::set_on(state, on))
            }
        },
        ("POST", "/api/pairing/new") => {
            pairing(maxpack_logic::receive::Pairing::NewCode, loopback, || crate::receive::renew(state))
        }
        // The page's "Stop (keep what arrived)" and "Delete what arrived" for the receive of this run.
        ("POST", "/api/pairing/stop") => receive_reply(crate::receive::stop_receive()),
        ("POST", "/api/pairing/cancel") => receive_reply(crate::receive::cancel(None)),
        // A .maxpack-recv a send left (state.receive_left), body its path.
        ("POST", "/api/pairing/left") => match crate::receive::delete_leftover(state, body) {
            Ok(()) => no_content(),
            Err((code, why)) => reply(code, "text/plain", why),
        },
        ("GET", p) if p.starts_with("/api/sizes/") => sizes(state, &p["/api/sizes/".len()..]),
        ("GET", p) if p.starts_with("/api/report/") => report(state, &p["/api/report/".len()..]),
        ("GET", "/api/settings") => {
            let v = config::to_json(&state.settings.lock().unwrap());
            reply(200, "application/json", v.to_string())
        }
        ("POST", "/api/settings") => post_settings(state, body),
        ("GET", "/api/ampr") => reply(200, "application/json", crate::ampr_status().to_string()),
        // The UI's "Update now": forced, one attempt, answered when done (one thread per request):
        // 200 + the result line, else `{code, detail}` (maxpack_logic::ampr::Fail), 409
        // tls_unchecked until the secure-connection self-test has passed, as /api/self_update.
        ("POST", "/api/ampr/update") => match crate::ampr_update(state, true, 1) {
            Ok(m) => reply(200, "text/plain", m),
            Err(f) => {
                let body = serde_json::json!({"code": f.code(), "detail": f.detail()}).to_string();
                reply(f.status(), "application/json", body)
            }
        },
        // A newer copy taking over the port (loopback, empty body) or the UI's Shut down (body
        // "shutdown", as Dump Forge's Stop: from any IP, but only from the page itself, as every POST).
        ("POST", "/api/quit") if (loopback || body == b"shutdown") && state.paused() => {
            log!("quit refused: a game is running (its overlay would stay mounted)");
            reply(409, "text/plain", "a game is running")
        }
        ("POST", "/api/self_update") => self_update(state, body),
        // Settings' "Check secure connections" (src/tls.rs): answered when every probe is done.
        ("POST", "/api/selftest/tls") => match crate::tls::run() {
            Ok(v) => reply(200, "application/json", v.to_string()),
            Err(e) => reply(409, "text/plain", e),
        },
        ("POST", "/api/quit") if loopback || body == b"shutdown" => {
            let user = body == b"shutdown";
            // A newer copy waits for the scan pass: quitting mid-build would leave a cache without
            // `.built`, which the next start empties (game-written files included). Held until the
            // exit, so no pass starts meanwhile.
            if !user {
                let Ok(work) = state.work.try_lock() else {
                    return reply(409, "text/plain", maxpack_logic::http::QUIT_BUILDING);
                };
                std::mem::forget(work);
            }
            let toast = user && state.settings.lock().unwrap().notify;
            log!("{}", if user { "shut down from the web UI" } else { "quit requested by a newer copy" });
            std::thread::spawn(move || {
                if toast {
                    sys::notify(crate::i18n::stopped(crate::i18n::lang()));
                }
                // A verify is cancelled; an install in progress pauses, so it resumes at the next start.
                crate::verify::stop();
                crate::receive::stop();
                crate::installer::stop();
                std::thread::sleep(Duration::from_millis(200));
                std::process::exit(0)
            });
            no_content()
        }
        ("POST", "/api/rescan") => {
            state.wake();
            no_content()
        }
        ("POST", "/api/rebuild-all") => {
            let tids = state.library();
            log!("rebuild of all {} games requested", tids.len());
            state.forced.lock().unwrap().extend(tids);
            state.wake();
            no_content()
        }
        ("POST", p) if p.starts_with("/api/playgo/") => post_title_list(state, TitleList::PlaygoOff, &p["/api/playgo/".len()..], body),
        ("POST", p) if p.starts_with("/api/hidden/") => post_title_list(state, TitleList::Hidden, &p["/api/hidden/".len()..], body),
        ("POST", p) if p.starts_with("/api/overlay/") => post_title_list(state, TitleList::OverlayOff, &p["/api/overlay/".len()..], body),
        ("POST", p) if p.starts_with("/api/fakelib/") => post_fakelib(state, &p["/api/fakelib/".len()..], body),
        ("POST", p) if p.starts_with("/api/recommended/") => post_recommended(state, &p["/api/recommended/".len()..], body),
        ("POST", p) if p.starts_with("/api/rebuild/") => rebuild(state, &p["/api/rebuild/".len()..]),
        ("POST", p) if p.starts_with("/api/delete/") => delete(state, &p["/api/delete/".len()..], body),
        ("POST", p) if p.starts_with("/api/launch/") => launch(state, &p["/api/launch/".len()..]),
        ("POST", "/api/eject") => eject(state, body),
        // "Verify fully" on a .p5z game's sheet (src/verify.rs).
        ("POST", "/api/verify/cancel") => {
            if crate::verify::cancel() { no_content() } else { reply(409, "text/plain", "no verify is running") }
        }
        ("POST", p) if p.starts_with("/api/verify/") => match crate::verify::start(state, &p["/api/verify/".len()..], body) {
            Ok(()) => no_content(),
            Err(r) => reply(r.status(), "application/json", serde_json::json!({"code": r.code(), "detail": r.detail()}).to_string()),
        },
        ("POST", "/api/purge") => {
            log!("cache purge requested");
            state.purge.store(true, Ordering::SeqCst);
            state.wake();
            no_content()
        }
        ("GET", p) if p.starts_with("/icon/") => image(state, &p["/icon/".len()..], &["icon0.png"]),
        ("GET", p) if p.starts_with("/pic/") => image(state, &p["/pic/".len()..], &["pic0.png", "pic1.png"]),
        // Debug: read a file under /data (logs, klog.txt). Read-only.
        ("GET", p) if p.starts_with("/file/data/") && state.settings.lock().unwrap().debug => {
            dbg_file(&p["/file".len()..], query)
        }
        _ => not_found(),
    }
}

/// `/api/inst/<x>`: the installer's `/api/<x>` (maxpack_logic::inst::installer_path), with the
/// method, Host, Content-Type and body as sent and `loopback` from the TCP peer; its answer as it
/// is. Its POST bodies are small JSON (paths): MAX_REQUEST is plenty.
/// As the mounter, around it: an uninstall, or an install that replaces other copies, is refused
/// while a game runs (409 `game_running`: the engine deletes folders with no such check); and an
/// uninstall it accepts is followed to its end (installer::after_uninstall), so MaxPack's home
/// entry and mount of that game go with it.
fn installer(state: &State, head: &str, method: &str, target: &str, body: &[u8], loopback: bool) -> Reply {
    use maxpack_logic::inst::{answer_ok, game_blocks, header, installer_path, needs_no_game, uninstall_title};
    let Some(path) = installer_path(method, target) else { return not_found() };
    // The gate must read what it forwards: an uninstall whose body is not exactly one top-level
    // string `title_id` it can parse (invalid JSON or UTF-8, a repeated, missing or non-string key;
    // the engine's parser is laxer, and reads the first of a repeated key) never reaches the engine.
    let uninstall = (method == "POST" && path == "/api/uninstall").then(|| uninstall_title(body));
    if uninstall == Some(None) {
        log!("installer: {path} refused: no single readable title_id");
        let body = serde_json::json!({"ok": false, "code": "bad_json", "detail": "title_id"}).to_string();
        return reply(400, "application/json", body);
    }
    let uninstall = uninstall.flatten();
    // `paused()` only knows the titles MaxPack tracks: for an uninstall (it names its title id) the
    // process list is asked too, so a game another mounter launched blocks it as well, and a failed
    // walk refuses. A replace carries paths, not a title id: it stays on `paused()` alone.
    let blocked = needs_no_game(method, &path, body) && {
        let paused = state.paused();
        let walk = match (paused, path == "/api/uninstall", uninstall.as_deref()) {
            (false, true, Some(tid)) => crate::launch::title_runs(tid),
            _ => Some(false),
        };
        game_blocks(paused, walk)
    };
    if blocked {
        log!("installer: {path} refused: a game is running");
        let body = serde_json::json!({"ok": false, "code": "game_running", "detail": "a game is running"}).to_string();
        return reply(409, "application/json", body);
    }
    // A job of the installer shares the engine's cancel flag and progress hook with the console's own jobs
    // (src/jobs.rs): not while one runs, and none starts while this is forwarded.
    let _job = if maxpack_logic::job::blocks_installer(method, &path) {
        let Some(g) = crate::jobs::hold_off() else {
            log!("installer: {path} refused: a verify or a receive from the PC runs on the console");
            let body = serde_json::json!({"ok": false, "code": "console_busy",
                "detail": "a .p5z is being verified or a game is arriving from the PC"}).to_string();
            return reply(409, "application/json", body);
        };
        Some(g)
    } else {
        None
    };
    // The library's copy of that title now: the one MaxPack mounts, if any.
    let source = uninstall.as_deref().and_then(|t| state.source(t)).map(|s| s.path);
    let (host, ctype) = (header(head, "host").unwrap_or(""), header(head, "content-type").unwrap_or(""));
    let a = crate::installer::call(method, &path, host, ctype, body, loopback);
    if let (Some(tid), true) = (uninstall, a.status == 200 && answer_ok(&a.body)) {
        crate::installer::after_uninstall(tid, source);
    }
    reply(a.status, a.ctype, a.body)
}

/// The end of an installer uninstall of `tid` (installer::after_uninstall), as /api/delete ends
/// after it removed a source: unmount without force, off the home screen (register::uninstall,
/// forget), out of `titles` and the library, cache deleted. Only while MaxPack mounts (in
/// "installer only" mode the other mounter owns the home entries: nothing here), and only for a
/// title MaxPack mounts from the copy that is now gone (`source`, the library's copy when the
/// uninstall was asked for). Never a title id installed on the console. Waits for a running game
/// to exit (up to `deadline`); a failure is logged and left to auto-remove.
pub fn uninstalled(state: &State, tid: &str, source: Option<&Path>, deadline: std::time::Instant) {
    let Some(src) = source else {
        log!("{tid}: uninstalled by the installer; not in MaxPack's library, so nothing of MaxPack's to remove");
        return;
    };
    if !crate::gone(src) {
        log!("{tid}: uninstalled by the installer; the library copy {} is still there: kept", src.display());
        return;
    }
    // The scan loop holds `work` for a whole pass; a running game pauses it, and its mount stays.
    let _work = loop {
        if !state.paused() {
            let work = state.work.lock().unwrap_or_else(|e| e.into_inner());
            if !state.paused() {
                break work;
            }
        }
        if std::time::Instant::now() >= deadline {
            log!("{tid}: uninstalled by the installer; a game kept running: its home entry is left to auto-remove");
            return;
        }
        std::thread::sleep(Duration::from_secs(2));
    };
    if !state.mounts() {
        log!("{tid}: uninstalled by the installer while MaxPack does not mount: its home entry is left to the mounter in charge");
        return;
    }
    if !state.titles.lock().unwrap().contains(tid) {
        log!("{tid}: uninstalled by the installer; not a title MaxPack registered or built: nothing to remove");
        return;
    }
    if let Some(p) = register::installed_copy(tid) {
        log!("{tid}: uninstalled by the installer; an installed copy exists ({}): its home entry is kept", p.display());
        return;
    }
    if let Err(e) = register::unmount_title(tid) {
        log!("{tid}: uninstalled by the installer; unmount: {e} (auto-remove tries again)");
        return;
    }
    match register::uninstall(tid) {
        Ok(()) => register::forget(tid),
        Err(e) => log!("{tid}: uninstall {}: {e}", src.display()),
    }
    state.titles.lock().unwrap().remove(tid);
    state.games.lock().unwrap().retain(|g| g.src.path != src);
    let cache_dir = cache::dir(tid);
    if cache_dir.exists() {
        if let Err(e) = fs::remove_dir_all(&cache_dir) {
            log!("{tid}: delete cache {}: {e}", cache_dir.display());
        }
    }
    log!("{tid}: uninstalled by the installer ({}): unmounted and removed from the home screen", src.display());
    state.wake();
}

/// POST /api/self_update, body `{"version":"<tag>"}`: downloads the newer release found at start,
/// checks it against its SHA256SUMS and sends it to elfldr; the new copy then takes over the port.
/// 200 `{}`, else `{code, detail}` with update::install's status (maxpack_logic::update::Fail).
/// Behind the same Host and Origin checks as every POST (`handle`).
fn self_update(state: &State, body: &[u8]) -> Reply {
    let v: Option<serde_json::Value> = serde_json::from_slice(body).ok();
    let Some(version) = v.as_ref().and_then(|v| v["version"].as_str()) else {
        return reply(400, "text/plain", "expected {\"version\":\"<tag>\"}");
    };
    // The new copy's quit waits for a pass and take_port gives up after 5 s: install holds new
    // passes off (update::holding) and checks that none runs.
    match crate::update::install(version, &|| state.paused(), &|| state.work.try_lock().is_ok(), &crate::installer::idle) {
        Ok(()) => reply(200, "application/json", "{}"),
        Err(f) => {
            let body = serde_json::json!({"code": f.code(), "detail": f.detail()}).to_string();
            reply(f.status(), "application/json", body)
        }
    }
}

fn post_settings(state: &State, body: &[u8]) -> Reply {
    let value = serde_json::from_slice::<serde_json::Value>(body).map_err(|e| format!("settings: {e}"));
    // A scan path MaxPack never scans (maxpack_logic::roots::never_root): JSON for the UI, which
    // says it in the page's language.
    let roots = value.as_ref().ok().and_then(|v| v.get("scanpath")).and_then(|v| v.as_array());
    for r in roots.into_iter().flatten().filter_map(|r| r.as_str()) {
        if let Some(why) = maxpack_logic::roots::never_root(r.trim()) {
            log!("settings rejected: scanpath {r:?} is {}", why.why());
            let body = serde_json::json!({"code": "forbidden_root", "path": r.trim(),
                "detail": format!("scanpath {r:?} is {}: MaxPack never scans it", why.why())});
            return reply(400, "application/json", body.to_string());
        }
    }
    let parsed = value.and_then(|v| Ok((config::from_json(&v)?, v)));
    let (mut s, v) = match parsed {
        Ok(s) => s,
        Err(e) => {
            log!("settings rejected: {e}");
            return reply(400, "text/plain", e);
        }
    };
    // overlay_off only for a game that can run without MaxPack's overlay (maxpack_logic::overlay).
    for tid in &s.overlay_off {
        if let Some(why) = state.source(tid).and_then(|src| { let (k, p) = kind_names(src.kind); maxpack_logic::overlay::needed(k, p) }) {
            log!("settings rejected: overlay_off {tid}: {why}");
            return reply(409, "text/plain", format!("{tid}: {why}"));
        }
    }
    // Held across the save: a concurrent /api/playgo, /api/hidden, /api/overlay or /api/fakelib change is not lost.
    let mut cur = state.settings.lock().unwrap();
    // The settings form does not send the per-game lists: keep them, from the file (a scan pass
    // may have put an older copy into state.settings). The same for `exclude` when a body lacks it.
    const KEPT: [&str; 7] = ["playgo_off", "hidden", "overlay_off", "fakelib_emulators", "fakelib_off", "manual_settings", "exclude"];
    if KEPT.iter().any(|k| v.get(k).is_none()) {
        let file = config::load();
        if v.get("playgo_off").is_none() {
            s.playgo_off = file.playgo_off;
        }
        if v.get("manual_settings").is_none() {
            s.manual_settings = file.manual_settings;
        }
        if v.get("hidden").is_none() {
            s.hidden = file.hidden;
        }
        if v.get("overlay_off").is_none() {
            s.overlay_off = file.overlay_off;
        }
        if v.get("fakelib_emulators").is_none() {
            s.fakelib_emulators = file.fakelib_emulators;
        }
        if v.get("fakelib_off").is_none() {
            s.fakelib_off = file.fakelib_off;
        }
        if v.get("exclude").is_none() {
            s.exclude = file.exclude;
        }
    }
    // The receive's switch and pairing code have their own routes (POST /api/pairing): always the file's.
    {
        let file = config::load();
        (s.receive_from_pc, s.receive_code) = (file.receive_from_pc, file.receive_code);
    }
    // A title is in one fakelib list at most (a body or the file's other list may name it too): off wins.
    let both = maxpack_logic::fakelib::exclusive(&mut s.fakelib_emulators, &s.fakelib_off);
    if !both.is_empty() {
        log!("settings: {both:?} in both fakelib_emulators and fakelib_off: taken as fakelib_off");
    }
    if let Err(e) = config::save(&s) {
        log!("settings: save {}: {e}", config::CONFIG_PATH);
        return reply(500, "text/plain", e.to_string());
    }
    log!("settings saved");
    *cur = s;
    drop(cur);
    state.wake();
    no_content()
}

/// The per-title lists of config.toml the game sheet switches.
#[derive(Clone, Copy)]
enum TitleList {
    /// POST /api/playgo/<TID>: `on` = the bundled PlayGo stub (out of playgo_off), `off` = none.
    PlaygoOff,
    /// POST /api/hidden/<TID>: `on` = hidden in the page's library, `off` = shown.
    Hidden,
    /// POST /api/overlay/<TID>: `on` = MaxPack's overlay at launch (out of overlay_off), `off` = none.
    OverlayOff,
}

/// POST /api/playgo/<TID>, /api/hidden/<TID> or /api/overlay/<TID>, body `on` or `off`. 404 for an
/// invalid title id or one not in the library (taking a title out of `hidden` also works for one
/// that is not there now, so a hand-edited or stale entry can go); 400 for any other body; 409 for
/// an overlay `off` of a game that needs it (a pkg, a .p5z or a packed folder: maxpack_logic::overlay).
fn post_title_list(state: &State, which: TitleList, tid: &str, body: &[u8]) -> Reply {
    let on = maxpack_logic::titles::switch_body(body);
    let unhide = matches!(which, TitleList::Hidden) && on == Some(false);
    let src = crate::scan::valid_tid(tid).then(|| state.source(tid)).flatten();
    if !crate::scan::valid_tid(tid) || (src.is_none() && !unhide) {
        return not_found();
    }
    let Some(on) = on else { return reply(400, "text/plain", "expected on or off") };
    if let (TitleList::OverlayOff, false, Some(src)) = (which, on, &src) {
        let (kind, packing) = kind_names(src.kind);
        if let Some(why) = maxpack_logic::overlay::needed(kind, packing) {
            log!("{tid}: overlay off refused: {} ({why})", src.path.display());
            return reply(409, "text/plain", why);
        }
    }
    let mut cur = state.settings.lock().unwrap();
    // From the file, not state.settings: a scan pass may have put an older copy there.
    let mut s = config::load();
    // The PlayGo stub by hand: the title's settings are the user's from now on (manual_settings).
    if matches!(which, TitleList::PlaygoOff) {
        hand_choice(tid, &mut s);
    }
    let (list, listed, what) = match which {
        TitleList::PlaygoOff => (&mut s.playgo_off, !on, if on { "PlayGo stub on" } else { "PlayGo stub off" }),
        TitleList::Hidden => (&mut s.hidden, on, if on { "hidden in the library" } else { "shown in the library" }),
        TitleList::OverlayOff => (&mut s.overlay_off, !on, if on { "overlay on" } else { "overlay off" }),
    };
    maxpack_logic::titles::set_listed(list, tid, listed);
    if let Err(e) = config::save(&s) {
        log!("{tid}: {what}: save {}: {e}", config::CONFIG_PATH);
        return reply(500, "text/plain", e.to_string());
    }
    log!("{tid}: {what}");
    *cur = s;
    drop(cur);
    // A PlayGo or overlay change refreshes fakelib/ on the next pass; hidden changes nothing there.
    if matches!(which, TitleList::PlaygoOff | TitleList::OverlayOff) {
        state.wake();
    }
    no_content()
}

/// POST /api/fakelib/<TID>, body `full`, `emulators` or `off`: the title's fakelib mode
/// (maxpack_logic::fakelib), kept in `fakelib_emulators` / `fakelib_off` (in one of them at most; full
/// in neither), and the title's settings are the user's from now on (`manual_settings`). 404 for an
/// invalid title id or one not in the library, 400 for any other body. The next pass recomposes its
/// fakelib/ (Fresh::Fakelib), as a PlayGo change does.
fn post_fakelib(state: &State, tid: &str, body: &[u8]) -> Reply {
    if !crate::scan::valid_tid(tid) || state.source(tid).is_none() {
        return not_found();
    }
    let Some(mode) = maxpack_logic::fakelib::body(body) else { return reply(400, "text/plain", "expected full, emulators or off") };
    let mut cur = state.settings.lock().unwrap();
    // From the file, not state.settings: a scan pass may have put an older copy there.
    let mut s = config::load();
    hand_choice(tid, &mut s);
    maxpack_logic::fakelib::set_mode(&mut s.fakelib_emulators, &mut s.fakelib_off, tid, mode);
    let what = format!("fakelib mode {}", mode.name());
    if let Err(e) = config::save(&s) {
        log!("{tid}: {what}: save {}: {e}", config::CONFIG_PATH);
        return reply(500, "text/plain", e.to_string());
    }
    log!("{tid}: {what}");
    *cur = s;
    drop(cur);
    state.wake();
    no_content()
}

/// Before a change of `tid`'s Fakelib mode or PlayGo stub on its sheet: the title goes into
/// manual_settings, and the recommended setting it had for the other of the two is kept as the user's
/// (maxpack_logic::recommended::hand_choice).
fn hand_choice(tid: &str, s: &mut Settings) {
    let before = crate::recommended::effective(tid, s);
    maxpack_logic::recommended::hand_choice(tid, before, &mut s.fakelib_emulators, &mut s.fakelib_off, &mut s.playgo_off,
        &mut s.manual_settings);
}

/// POST /api/recommended/<TID>, body `reset`: the sheet's "Back to the recommended setting": the title
/// out of manual_settings, fakelib_emulators, fakelib_off and playgo_off
/// (maxpack_logic::recommended::reset), so the recommended settings apply to it again; the next pass
/// recomposes its fakelib/ if that changes it. 404 for an invalid title id or one not in the library,
/// 400 for any other body.
fn post_recommended(state: &State, tid: &str, body: &[u8]) -> Reply {
    if !crate::scan::valid_tid(tid) || state.source(tid).is_none() {
        return not_found();
    }
    if body != b"reset" {
        return reply(400, "text/plain", "expected reset");
    }
    let mut cur = state.settings.lock().unwrap();
    // From the file, not state.settings: a scan pass may have put an older copy there.
    let mut s = config::load();
    maxpack_logic::recommended::reset(tid, &mut s.fakelib_emulators, &mut s.fakelib_off, &mut s.playgo_off, &mut s.manual_settings);
    if let Err(e) = config::save(&s) {
        log!("{tid}: back to the recommended settings: save {}: {e}", config::CONFIG_PATH);
        return reply(500, "text/plain", e.to_string());
    }
    let eff = crate::recommended::effective(tid, &s);
    log!("{tid}: back to the recommended settings (fakelib mode {}, PlayGo stub {}, {})", eff.fakelib.name(),
        if eff.playgo_off { "off" } else { "on" }, eff.source.name());
    *cur = s;
    drop(cur);
    state.wake();
    no_content()
}

/// Serves an absolute path under /data (no traversal above /data). Whole file: first 256 KiB as
/// text. With ?off=N&len=M: those bytes (M <= 16 MiB) as octet-stream, read in place, so large
/// files can be pulled in pieces.
fn dbg_file(path: &str, query: &str) -> Reply {
    use std::os::unix::fs::FileExt;
    if path.contains("..") {
        return not_found();
    }
    // config.toml holds the receive's pairing code: never served, by whatever path names it.
    let config = |p: &str| std::fs::canonicalize(p).ok();
    if let Some(real) = config(path) {
        let tmp = format!("{}.tmp", config::CONFIG_PATH);
        if [config(config::CONFIG_PATH), config(&tmp)].iter().flatten().any(|c| *c == real) {
            return not_found();
        }
    }
    let arg = |k: &str| {
        query.split('&').find_map(|kv| kv.strip_prefix(k)?.strip_prefix('=')?.parse::<u64>().ok())
    };
    if let (Some(off), Some(len)) = (arg("off"), arg("len")) {
        let Ok(f) = std::fs::File::open(path) else { return not_found() };
        let mut buf = vec![0u8; len.min(16 << 20) as usize];
        let mut n = 0;
        while n < buf.len() {
            match f.read_at(&mut buf[n..], off + n as u64) {
                Ok(0) => break,
                Ok(k) => n += k,
                Err(_) => return not_found(),
            }
        }
        buf.truncate(n);
        return reply(200, "application/octet-stream", buf);
    }
    match std::fs::read(path) {
        Ok(mut b) => {
            b.truncate(256 * 1024);
            reply(200, "text/plain; charset=utf-8", b)
        }
        Err(_) => not_found(),
    }
}

/// `loopback`: the page is open on the console itself, the only place it is shown the pairing code.
fn api_state(state: &State, loopback: bool) -> Vec<u8> {
    let settings = state.settings.lock().unwrap().clone();
    // ponytail: crashes.json is read on every state poll (a few hundred bytes per crashed title).
    let crashes = crate::crash::load();
    // A rebuild asked for but not started yet (the pass picks it up, or a game is running).
    let queued = state.forced.lock().unwrap().clone();
    let x = GameExtra { settings: &settings, crashes: &crashes, queued: &queued };
    // A copy: presence reads app.db, the mount table and the sources without holding the lock.
    let games = state.games.lock().unwrap().clone();
    let games: Vec<_> = games.iter().zip(presence(&games)).map(|(g, [registered, mounted, present])| {
        let mut v = game_json(g, &x);
        v["registered"] = registered.into();
        v["mounted"] = mounted.into();
        v["source_present"] = present.into();
        v
    }).collect();
    let all = storage::drives();
    let drives: Vec<_> = all
        .iter()
        .map(|d| serde_json::json!({"mount": d.mount, "label": d.label, "kind": d.kind, "total": d.total, "free": d.free,
            "ejectable": storage::ejectable(&d.mount)}))
        .collect();
    let free = all.iter().find(|d| d.mount == "/data").map_or(0, |d| d.free);
    let log = log_tail();
    let log = String::from_utf8_lossy(&log);
    let lines: Vec<&str> = log.lines().collect();
    let tail = &lines[lines.len().saturating_sub(LOG_LINES)..];
    let mut running: Vec<String> = state.running.lock().unwrap().iter().cloned().collect();
    running.sort();
    // {path, kind, title_id?}: no title_id key when the file name has none.
    let images: Vec<_> = state.images.lock().unwrap().iter().map(|i| {
        let mut o = serde_json::json!({"path": i.path.to_string_lossy(), "kind": i.kind});
        if let Some(t) = &i.title_id {
            o["title_id"] = t.as_str().into();
        }
        o
    }).collect();
    // {path, code, detail}: detail is the engine's reason (damaged) or the volume count (too_many_volumes).
    let unplayable: Vec<_> = state.unplayable.lock().unwrap().iter().map(|(p, u)| {
        let detail = match u {
            maxpack_logic::p5z::Unplayable::Damaged(why) => why.clone(),
            maxpack_logic::p5z::Unplayable::TooManyVolumes(n) => n.to_string(),
        };
        serde_json::json!({"path": p.to_string_lossy(), "code": u.code(), "detail": detail})
    }).collect();
    let v = serde_json::json!({
        "version": env!("CARGO_PKG_VERSION"),
        // {version, size, tls_passed} of a newer release found at start (the page offers Update
        // once tls_passed), else null.
        "update": crate::update::available(),
        // The secure-connection self-test: {passed, at, version} of its last conclusive run ({}:
        // never), and running.
        "tls_check": crate::tls::summary(),
        "busy": state.busy.load(Ordering::SeqCst),
        "paused": !running.is_empty(),
        // null | "kstuff" | "other:<process>"
        "waiting": state.mounter.lock().unwrap().api_waiting(),
        "running": running,
        "games": games,
        "images": images,
        "unplayable": unplayable,
        "drives": drives,
        // The verify of this run (src/verify.rs), or null.
        "verify": crate::verify::json(),
        // The receive from the PC of this run (src/receive.rs), or null; and Settings > "Receive games from the
        // PC": the switch, and the pairing code only for the page open on the console itself (anyone on the network
        // can read this page; the code is what keeps them from writing to the drives).
        "receive": crate::receive::json(),
        // The .maxpack-recv files sends left on the drives: [{path, name, size}].
        "receive_left": crate::receive::leftovers_json(state),
        "pairing": {
            "on": settings.receive_from_pc,
            "code": (loopback && maxpack_logic::receive::valid_code(&settings.receive_code)).then_some(settings.receive_code.as_str()),
            "here": loopback,
        },
        "cache": {
            "path": cache::CACHES,
            "size": *state.cache_total.lock().unwrap(),
            "free": free,
            "low_space": state.low_space.load(Ordering::SeqCst),
            "ampr_present": Path::new(cache::AMPR_DIR).join("libSceAmpr.sprx").is_file(),
        },
        "log": tail,
    });
    v.to_string().into_bytes()
}

/// Per game: [registered (register::registered: the staged files and the title id in app.db), mounted
/// (/system_ex/app/<TID> is a mount point), source present (its path exists)], from app.db (read again
/// only when it changed) and one mount table for all of them.
fn presence(games: &[Game]) -> Vec<[bool; 3]> {
    let tids: Vec<&str> = games.iter().map(|g| g.src.title_id.as_str()).collect();
    let registered = register::registered(&tids);
    static FAILED: AtomicBool = AtomicBool::new(false);
    let mounts: HashSet<String> = match mount_table() {
        Ok(m) => {
            FAILED.store(false, Ordering::Relaxed);
            m.into_iter().map(|(on, _, _)| on).collect()
        }
        Err(e) => {
            // Once until it reads again: the page polls this every second or two.
            if !FAILED.swap(true, Ordering::Relaxed) {
                log!("title status: getfsstat: {e}; titles shown as not mounted");
            }
            HashSet::new()
        }
    };
    games.iter().zip(registered).map(|(g, registered)| {
        let tid = &g.src.title_id;
        [registered, mounts.contains(&maxpack_logic::presence::title_mount(tid)), g.src.path.exists()]
    }).collect()
}

/// The last LOG_TAIL bytes of maxpack.log (sys.rs rotates it at ~4 MiB), from a line start.
fn log_tail() -> Vec<u8> {
    use std::io::{Seek, SeekFrom};
    const LOG_TAIL: u64 = 64 << 10;
    let Ok(mut f) = std::fs::File::open(sys::LOG_PATH) else { return Vec::new() };
    let len = f.metadata().map_or(0, |m| m.len());
    let off = len.saturating_sub(LOG_TAIL);
    let mut b = Vec::new();
    if f.seek(SeekFrom::Start(off)).is_err() || f.take(LOG_TAIL).read_to_end(&mut b).is_err() {
        return Vec::new();
    }
    if off > 0 {
        let cut = b.iter().position(|&c| c == b'\n').map_or(b.len(), |i| i + 1);
        b.drain(..cut);
    }
    b
}

/// Err(409) while MaxPack does not mount: what is at /system_ex/app/<TID> may be another
/// mounter's, so nothing here unmounts it. (Rebuild, rebuild-all and purge only queue requests for
/// the scan loop, which waits for Ready; they need no check.)
fn mounts_or_409(state: &State) -> Result<(), Reply> {
    let why = match *state.mounter.lock().unwrap() {
        MounterState::Ready => return Ok(()),
        MounterState::OtherMounter(p) => format!("another mounter is running ({p}): MaxPack touches no game mount until it stops"),
        MounterState::WaitKstuff => "waiting for kstuff: MaxPack touches no game mount until it answers".to_string(),
    };
    Err(reply(409, "text/plain", why))
}

fn rebuild(state: &State, tid: &str) -> Reply {
    if !crate::scan::valid_tid(tid) || state.source(tid).is_none() {
        return not_found();
    }
    log!("{tid}: rebuild requested");
    state.forced.lock().unwrap().insert(tid.to_string());
    state.wake();
    no_content()
}

/// POST /api/launch/<TID>: starts a ready library title for the foreground user, as its home-screen
/// tile does (sys::launch_app). Refused (maxpack_logic::play::gate) for an absent title, while a game
/// runs, while MaxPack does not mount, or when the title is not ready; 502 + text when the console
/// refuses the launch.
fn launch(state: &State, tid: &str) -> Reply {
    let status = if crate::scan::valid_tid(tid) {
        state.games.lock().unwrap().iter().find(|g| g.src.title_id == tid && g.status != "duplicate").map(|g| g.status)
    } else {
        None
    };
    if let Err(r) = maxpack_logic::play::gate(status, state.mounts(), state.paused()) {
        if status.is_some() {
            log!("{tid}: launch from the web UI refused: {}", r.text());
        }
        return reply(r.status(), "text/plain", r.text());
    }
    log!("{tid}: launch from the web UI");
    match sys::launch_app(tid) {
        Ok(()) => no_content(),
        Err(e) => {
            log!("{tid}: launch: {e}");
            reply(502, "text/plain", e)
        }
    }
}

/// GET /api/report/<TID>: plain text for an issue (maxpack_logic::report); 404 for an invalid or
/// absent library title.
fn report(state: &State, tid: &str) -> Reply {
    if !crate::scan::valid_tid(tid) {
        return not_found();
    }
    let Some(g) = state.games.lock().unwrap().iter().find(|g| g.src.title_id == tid && g.status != "duplicate").cloned()
    else {
        return not_found();
    };
    let s = state.settings.lock().unwrap().clone();
    let waiting = state.mounter.lock().unwrap().api_waiting();
    let ampr = crate::ampr_status();
    let ampr = format!("{} {}", ampr["kind"].as_str().unwrap_or("?"), ampr["version"].as_str().unwrap_or("")).trim_end().to_string();
    let eff = crate::recommended::effective(tid, &s);
    let fakelib = eff.fakelib;
    let playgo = if !s.add_emulators {
        "not added (add_emulators off)"
    } else if fakelib == maxpack_logic::fakelib::Mode::Off {
        "not added (fakelib mode off)"
    } else if eff.playgo_off {
        "off"
    } else {
        "on"
    };
    let (fakelib_recommended, playgo_recommended) = (eff.fakelib_recommended, eff.playgo_recommended);
    let crash = crate::crash::load().remove(tid);
    let firmware = sys::firmware();
    let log = log_tail();
    let log = String::from_utf8_lossy(&log);
    let (kind, packing) = kind_names(g.src.kind);
    let (path, backport) = (g.src.path.to_string_lossy(), g.src.backport.as_ref().map(|b| b.to_string_lossy()));
    // A .p5z's last verify on the console, in words that name no file of the game.
    let verified = (g.src.kind == Kind::P5z).then(|| maxpack_logic::verify::report_line(crate::verify::last(&g.src.path).as_ref()));
    let text = maxpack_logic::report::text(&maxpack_logic::report::Report {
        version: env!("CARGO_PKG_VERSION"), firmware: firmware.as_deref(), waiting: waiting.as_deref(),
        title_id: tid, title_name: &g.src.title_name, game_version: &g.src.version, path: &path, kind, packing,
        drive: &g.src.drive, status: g.status, error: g.error.as_deref(), hidden: s.hidden.iter().any(|t| t == tid), overlay_off: crate::cache::overlay_off(&g.src, &s),
        fakelib, fakelib_recommended, playgo_recommended, backport: backport.as_deref(), ampr: &ampr, playgo, crash: crash.as_ref(), log: &log,
        verified: verified.as_deref(),
    });
    reply(200, "text/plain; charset=utf-8", text)
}

fn sizes(state: &State, tid: &str) -> Reply {
    if !crate::scan::valid_tid(tid) {
        return not_found();
    }
    let Some(src) = state.source(tid) else { return not_found() };
    let game = match src.kind {
        Kind::Pkg | Kind::P5z => fs::metadata(&src.path).map(|m| m.len()).unwrap_or_else(|e| {
            log!("{tid}: stat {}: {e}", src.path.display());
            src.size
        }),
        Kind::Folder(_) => cache::du(&src.path),
    };
    let dlc: u64 = [
        "/user/addcont", "/mnt/ext0/user/addcont", "/mnt/ext0/ps5/user/addcont",
        "/mnt/ext1/user/addcont", "/mnt/ext1/ps5/user/addcont",
    ].iter().map(|root| cache::du(&Path::new(root).join(tid))).sum();
    let family = if tid.starts_with("PPSA") { "savedata_prospero" } else { "savedata" };
    let save = match fs::read_dir("/user/home") {
        Ok(entries) => {
            let mut total = 0;
            let mut failed = false;
            for entry in entries {
                match entry.and_then(|e| Ok((e.file_name(), e.file_type()?, e.path()))) {
                    Ok((name, ty, home)) => {
                        let name = name.to_string_lossy();
                        if ty.is_dir() && name.len() == 8 && name.bytes().all(|b| b.is_ascii_hexdigit()) {
                            total += cache::du(&home.join(family).join(tid));
                        }
                    }
                    Err(e) => {
                        log!("{tid}: read /user/home: {e}");
                        failed = true;
                        break;
                    }
                }
            }
            if failed { None } else { Some(total) }
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Some(0),
        Err(e) => {
            log!("{tid}: read /user/home: {e}");
            None
        }
    };
    reply(200, "application/json", serde_json::json!({
        "game": game, "dlc": dlc, "save": save, "cache": cache::size(tid),
    }).to_string())
}

fn safe_delete_source(src: &Source, roots: &[String]) -> Result<(), &'static str> {
    let path = &src.path;
    if !path.is_absolute() || path.components().any(|c| c == Component::ParentDir) {
        return Err("source path is not a safe absolute path");
    }
    if ["/", "/data", "/user", "/mnt", "/system", "/system_ex"].iter().any(|p| path == Path::new(p))
        || path.parent() == Some(Path::new("/mnt"))
        || storage::drives().iter().any(|d| path == Path::new(&d.mount)) {
        return Err("source path is a protected root");
    }
    let roots: Vec<&Path> = roots.iter().map(|r| {
        let trimmed = r.trim_end_matches('/');
        Path::new(if trimmed.is_empty() { "/" } else { trimmed })
    }).collect();
    if roots.iter().any(|root| path == *root || root.starts_with(path)) {
        return Err("source path is a scan root or its parent");
    }
    let Some(root) = roots.iter().filter(|root| path.starts_with(root)).max_by_key(|r| r.as_os_str().len()) else {
        return Err("source path is outside scan roots");
    };
    // No symlink from the scan root down to the source: the path text must be where it lives.
    // ponytail: links above the root are not checked (system paths may be links; the root is the user's choice).
    // ponytail: checked once; a link swapped in before remove_dir_all is not caught (std does not follow it).
    if path.ancestors().skip(1).take_while(|a| a.starts_with(root))
        .any(|a| fs::symlink_metadata(a).map(|m| m.file_type().is_symlink()).unwrap_or(true)) {
        return Err("source path goes through a symlink");
    }
    let meta = fs::symlink_metadata(path).map_err(|_| "source path is missing or inaccessible")?;
    match src.kind {
        Kind::Pkg if meta.is_file() && path.extension().is_some_and(|e| e.eq_ignore_ascii_case("pkg")) => Ok(()),
        Kind::P5z if meta.is_file() && path.extension().is_some_and(|e| e.eq_ignore_ascii_case("p5z")) => Ok(()),
        Kind::Folder(_) if meta.is_dir() => Ok(()),
        Kind::Pkg => Err("source is not a .pkg file"),
        Kind::P5z => Err("source is not a .p5z file"),
        Kind::Folder(_) => Err("source is not a directory"),
    }
}

fn delete(state: &State, tid: &str, body: &[u8]) -> Reply {
    if !crate::scan::valid_tid(tid) {
        return not_found();
    }
    // The scan loop holds `work` while it scans, builds or registers; never wait a build out here.
    let Ok(_work) = state.work.try_lock() else {
        return reply(409, "text/plain", "a scan or build is in progress; try again in a moment");
    };
    let Some(src) = state.source(tid) else { return not_found() };
    // The UI sends the path it showed in the confirmation; a scan may have picked another copy since.
    if body != src.path.as_os_str().as_encoded_bytes() {
        log!("{tid}: delete refused: asked for {:?}, library source is {}", String::from_utf8_lossy(body), src.path.display());
        return reply(409, "text/plain", "the library changed; reopen the game and try again");
    }
    if state.paused() || state.running.lock().unwrap().contains(tid) {
        return reply(409, "text/plain", "a game is running");
    }
    if let Err(r) = mounts_or_409(state) {
        log!("{tid}: delete refused: MaxPack does not mount now");
        return r;
    }
    if state.games.lock().unwrap().iter().any(|g| g.src.title_id == tid && matches!(g.status, "building" | "checking")) {
        return reply(409, "text/plain", "the game is being built");
    }
    if crate::verify::uses(&src.path) {
        return reply(409, "text/plain", "the game is being verified");
    }
    let roots = {
        let settings = state.settings.lock().unwrap();
        if settings.scan_paths.is_empty() { storage::default_scan_roots() } else { settings.scan_paths.clone() }
    };
    if let Err(reason) = safe_delete_source(&src, &roots) {
        log!("{tid}: delete rejected {}: {reason}", src.path.display());
        return reply(409, "text/plain", reason);
    }
    if let Err(e) = register::unmount_title(tid) {
        log!("{tid}: unmount {}: {e}", src.path.display());
        return reply(409, "text/plain", e.to_string());
    }
    // Source first: if it cannot be removed, the cache (game-written files) and the tile stay.
    let result = match src.kind {
        Kind::Pkg | Kind::P5z => fs::remove_file(&src.path),
        Kind::Folder(_) => fs::remove_dir_all(&src.path),
    };
    if let Err(e) = result {
        log!("{tid}: delete source {}: {e}", src.path.display());
        // Unmounted above: the next pass remounts it (process: register::mount on a registered title).
        // ponytail: a folder remove_dir_all that failed midway leaves a partial source; not restored.
        state.wake();
        return reply(500, "text/plain", e.to_string());
    }
    if let Err(e) = register::uninstall(tid) {
        // ponytail: the tile may stay; the next scan does not re-register a deleted source.
        log!("{tid}: uninstall {}: {e}", src.path.display());
    }
    state.titles.lock().unwrap().remove(tid);
    state.games.lock().unwrap().retain(|g| g.src.path != src.path);
    state.wake();
    let cache_dir = cache::dir(tid);
    if cache_dir.exists() {
        if let Err(e) = fs::remove_dir_all(&cache_dir) {
            log!("{tid}: delete cache {}: {e}", cache_dir.display());
            return reply(500, "text/plain", format!("source deleted; cache not: {e}"));
        }
    }
    log!("{tid}: deleted {}", src.path.display());
    if state.settings.lock().unwrap().notify {
        sys::notify(&crate::i18n::deleted(crate::i18n::lang(), &src.title_name));
    }
    no_content()
}

// Lives in ps5/usb.c (IPMI to SceAutoMounterIpc, SMP sm_usb.c port).
extern "C" {
    fn km_usb_eject(path: *const libc::c_char, source: *const libc::c_char, fsid: u64, native_status: *mut i32) -> i32;
}

/// (mount point, source, fsid) of every mount. getfsstat into our own buffer: ps5/compat.c's
/// getmntinfo shares one static buffer with the launch watcher's thread.
fn mount_table() -> std::io::Result<Vec<(String, String, u64)>> {
    let mut extra = 8; // room for mounts added between the two calls
    for _ in 0..4 {
        let n = unsafe { libc::getfsstat(std::ptr::null_mut(), 0, libc::MNT_NOWAIT) };
        if n < 0 {
            return Err(std::io::Error::last_os_error());
        }
        let mut buf: Vec<libc::statfs> = vec![unsafe { std::mem::zeroed() }; n as usize + extra];
        let bytes = (buf.len() * std::mem::size_of::<libc::statfs>()) as libc::c_long;
        let n = unsafe { libc::getfsstat(buf.as_mut_ptr(), bytes, libc::MNT_NOWAIT) };
        if n < 0 {
            return Err(std::io::Error::last_os_error());
        }
        // A full buffer may be a cut table: the dependency checks need every mount.
        if (n as usize) < buf.len() {
            return Ok(buf[..n as usize].iter().map(mount_entry).collect());
        }
        extra *= 4;
    }
    Err(std::io::Error::other("the mount table kept growing"))
}

fn mount_entry(fs: &libc::statfs) -> (String, String, u64) {
    // SMP sm_usb_filesystem_id: the 8-byte fsid as one u64.
    let fsid = unsafe { std::mem::transmute::<libc::fsid_t, u64>(fs.f_fsid) };
    // Bounded: a name that fills its field has no NUL.
    (crate::launch::c_field(&fs.f_mntonname), crate::launch::c_field(&fs.f_mntfromname), fsid)
}

/// The mount exactly at `path`: Ok(None) when the path resolves to its parent FS (an unmounted
/// /mnt/usbN reports /mnt) or is gone (ENOENT); any other statfs error is Err (SMP sm_usb_check_mount).
fn mount_at(path: &str) -> std::io::Result<Option<(String, String, u64)>> {
    let c = std::ffi::CString::new(path).map_err(|_| std::io::Error::from_raw_os_error(libc::EINVAL))?;
    let mut fs: libc::statfs = unsafe { std::mem::zeroed() };
    if unsafe { libc::statfs(c.as_ptr(), &mut fs) } != 0 {
        let e = std::io::Error::last_os_error();
        return if e.raw_os_error() == Some(libc::ENOENT) { Ok(None) } else { Err(e) };
    }
    Ok(Some(mount_entry(&fs)).filter(|m| m.0 == path))
}

/// SMP usb_disk_source_length: the length of "/dev/daN" when followed by the end, 'p' or 's'.
fn disk_len(source: &str) -> usize {
    let Some(rest) = source.strip_prefix("/dev/da") else { return 0 };
    let digits = rest.bytes().take_while(u8::is_ascii_digit).count();
    if digits == 0 {
        return 0;
    }
    match rest.as_bytes().get(digits) {
        None | Some(b'p' | b's') => 7 + digits,
        _ => 0,
    }
}

/// POST /api/eject: unmounts our titles on the drive, then asks the system to eject it (SMP
/// run_usb_eject_job). `work` is held throughout so the scan loop cannot remount meanwhile.
fn eject(state: &State, body: &[u8]) -> Reply {
    let path = String::from_utf8_lossy(body).into_owned();
    if !storage::ejectable(&path) {
        return reply(400, "text/plain", "not an ejectable drive");
    }
    let (source, fsid) = match mount_at(&path) {
        Ok(Some((_, source, fsid))) => (source, fsid),
        Ok(None) => return reply(404, "text/plain", "the drive is not mounted"),
        Err(e) => {
            log!("eject {path}: statfs: {e}");
            return reply(500, "text/plain", format!("cannot read the drive: {e}"));
        }
    };
    let Ok(work) = state.work.try_lock() else {
        return reply(409, "text/plain", "a scan or build is in progress; try again in a moment");
    };
    if state.paused() {
        return reply(409, "text/plain", "a game is running");
    }
    if crate::verify::uses(Path::new(&path)) {
        log!("eject {path}: refused: a .p5z on it is being verified");
        return reply(409, "text/plain", "a .p5z on this drive is being verified");
    }
    if crate::receive::uses(Path::new(&path)) {
        log!("eject {path}: refused: a game from the PC is arriving on it");
        return reply(409, "text/plain", "a game from the PC is arriving on this drive");
    }
    // Eject unmounts every /system_ex/app/<TID> from the drive, whoever mounted it.
    if let Err(r) = mounts_or_409(state) {
        log!("eject {path}: refused: MaxPack does not mount now");
        return r;
    }
    // ponytail: the drive's identity is taken now, not when the page was drawn; a drive swapped in
    // between is ejected. Harmless: it is only an unmount.
    // An unreadable table fails closed: the dependency checks below need it.
    let mounts = match mount_table() {
        Ok(m) => m,
        Err(e) => {
            log!("eject {path}: getfsstat: {e}");
            return reply(500, "text/plain", format!("cannot read the mount table: {e}"));
        }
    };
    let disk = disk_len(&source);
    let other = mounts.iter().find(|(on, from, _)| {
        *on != path && disk != 0 && disk_len(from) == disk && from[..disk] == source[..disk]
    });
    if let Some((on, from, _)) = other {
        log!("eject {path}: another partition of the drive is mounted: {on} ({from})");
        return reply(409, "text/plain", format!("another partition of this drive is mounted: {on}"));
    }
    let mut tids: Vec<&str> = mounts
        .iter()
        .filter_map(|(on, from, _)| {
            let tid = on.strip_prefix("/system_ex/app/").filter(|t| crate::scan::valid_tid(t))?;
            let from = from.strip_prefix("<above>:").unwrap_or(from);
            (from == path || from.starts_with(&format!("{path}/"))).then_some(tid)
        })
        .collect();
    tids.sort_unstable();
    tids.dedup();
    for tid in tids {
        if let Err(e) = register::unmount_title(tid) {
            log!("eject {path}: {tid}: unmount: {e}");
            drop(work);
            state.wake();
            return reply(409, "text/plain", e.to_string());
        }
    }
    // The launch watcher does not take `work`: a game may have started since the check above (SMP
    // rechecks too). ponytail: a game starting between here and the IPMI call is not caught; its
    // sandbox's /mnt/usbN alias then keeps the drive busy or goes with it.
    let result = if state.paused() {
        log!("eject {path}: a game started; not ejected");
        reply(409, "text/plain", "a game is running")
    } else {
        eject_and_confirm(&path, &source, fsid)
    };
    drop(work);
    // The next pass forgets the gone drive's games; on failure it remounts the titles unmounted above.
    state.wake();
    result
}

fn eject_and_confirm(path: &str, source: &str, fsid: u64) -> Reply {
    let (Ok(cpath), Ok(csource)) = (std::ffi::CString::new(path), std::ffi::CString::new(source)) else {
        return reply(400, "text/plain", "NUL in the path");
    };
    let mut native = 0i32;
    let rc = unsafe { km_usb_eject(cpath.as_ptr(), csource.as_ptr(), fsid, &mut native) };
    if rc == libc::ENOENT || rc == libc::ESTALE {
        log!("eject {path}: the mount changed (errno {rc})");
        return reply(409, "text/plain", "the drive changed; refresh and try again");
    }
    if rc != 0 {
        log!("eject {path}: failed: errno {rc}, native status {:#010x}", native as u32);
        return reply(502, "text/plain", format!("eject failed: errno {rc}, native status {:#010x}", native as u32));
    }
    for _ in 0..50 {
        // Only a seen unmount counts; a statfs error keeps polling (SMP: unconfirmed).
        if let Ok(None) = mount_at(path) {
            log!("eject {path}: done ({source})");
            return no_content();
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    log!("eject {path}: requested, but still mounted after 5 s");
    reply(502, "text/plain", "unmount not confirmed; keep the drive connected and refresh")
}

/// The first of `names` in the source's sce_sys, else in /user/appmeta/<TID>/, else (a pkg) in the .pkg.
fn image(state: &State, tid: &str, names: &[&str]) -> Reply {
    if !crate::scan::valid_tid(tid) {
        return not_found();
    }
    let src = state.source(tid);
    let mut dirs: Vec<PathBuf> = src.iter().map(cache::sce_sys).collect();
    dirs.push(Path::new("/user/appmeta").join(tid));
    for dir in &dirs {
        for name in names {
            if let Ok(png) = std::fs::read(dir.join(name)) {
                return reply(200, "image/png", png);
            }
        }
    }
    // A pkg whose cache is not built (yet, or it failed): straight from the .pkg. A .p5z has no such
    // preview: its icon shows once its cache is built.
    if let Some(src) = src.filter(|s| s.kind == Kind::Pkg) {
        for name in names {
            match cache::pkg_file(&src.path, &format!("sce_sys/{name}")) {
                Ok(png) => return reply(200, "image/png", png),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => log!("{tid}: image {name} from {}: {e}", src.path.display()),
            }
        }
    }
    not_found()
}
