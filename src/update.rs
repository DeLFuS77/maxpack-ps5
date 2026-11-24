//! Self-update (maxpack_logic::update): the latest release of DeLFuS77/maxpack-ps5, looked at once
//! per start, and, when the page asks (POST /api/self_update), its maxpack.elf downloaded, checked
//! against the release's SHA256SUMS and sent to elfldr. The new copy then takes the web port
//! (web::take_port asks this copy to quit, which waits for a running scan pass) and saves its own
//! self copy. While the repository is private or has no published release the look gets a 404:
//! one log line, nothing offered, no retry.

use maxpack_logic::update::{self as logic, Fail, Latest, LookFailed, Offer, Updater};
use std::io::Write;
use std::net::{Shutdown, SocketAddr, TcpStream};
use std::sync::{Mutex, MutexGuard};
use std::time::Duration;

const ELFLDR: ([u8; 4], u16) = ([127, 0, 0, 1], 9021);
const RUNNING: &str = env!("CARGO_PKG_VERSION");

/// The offer found at start and the update in flight.
static UPDATER: Mutex<Updater> = Mutex::new(Updater::new());

fn updater() -> MutexGuard<'static, Updater> {
    UPDATER.lock().unwrap_or_else(|e| e.into_inner())
}

/// An update runs or its ELF was sent less than HOLD_MS ago: the scan loop starts no pass.
pub fn holding() -> bool {
    updater().holding(crate::sys::uptime_ms())
}

/// `{version, size}` of the newer release found at start (/api/state `update`), None when there is none.
pub fn available() -> Option<serde_json::Value> {
    updater().offer().map(|o| serde_json::json!({"version": o.version, "size": o.elf.size}))
}

/// The start-up look, in its own thread: once per start, LOOK_TRIES at most and only after no answer
/// or a server error (the console may not be online yet); one log line says how it ended.
pub fn spawn_check() {
    std::thread::spawn(|| {
        let url = logic::LATEST_URL;
        let mut tries = 0;
        let body = loop {
            tries += 1;
            match crate::net::https_get(url, logic::LATEST_MAX) {
                Ok(b) => break b,
                Err(e) => match logic::look_failed(crate::net::status_of(&e)) {
                    LookFailed::NoRelease => {
                        log!("update: no published release of {} (HTTP 404: none yet, or the repository is private); nothing offered", logic::REPO);
                        return;
                    }
                    LookFailed::TryAgain if tries < logic::LOOK_TRIES => {
                        std::thread::sleep(Duration::from_secs(logic::LOOK_DELAY_SECS));
                    }
                    _ => {
                        log!("update: {e} (try {tries} of {}); nothing offered until the next start", logic::LOOK_TRIES);
                        return;
                    }
                },
            }
        };
        match logic::latest(&body, RUNNING) {
            Latest::Newer(o) => {
                log!("update: {} is available (running {RUNNING})", o.version);
                updater().set_offer(Some(o));
            }
            Latest::NotNewer(tag) => log!("update: the latest release is {tag}, running {RUNNING}: nothing to offer"),
            Latest::Unusable(why) => log!("update: the latest release is not offered: {why}"),
        }
    });
}

/// Downloads release `version` (it must be the one found at start), checks it and sends it to
/// elfldr. `paused`: a game runs (the new copy's take-over would be refused), asked before and again
/// after the download; `pass_idle`: no scan pass holds State::work, asked once the update is under
/// way (a later pass then waits on `holding`); `installer_idle`: the installer copies nothing
/// (quitting would pause it).
pub fn install(version: &str, paused: &dyn Fn() -> bool, pass_idle: &dyn Fn() -> bool, installer_idle: &dyn Fn() -> bool)
    -> Result<(), Fail> {
    let game = paused();
    let offer = match updater().begin(version, game, crate::sys::uptime_ms()) {
        Ok(o) => o,
        Err(f) => {
            log!("update: {version} refused ({}): {}", f.code(), f.detail());
            return Err(f);
        }
    };
    let r = if !pass_idle() {
        Err(Fail::ScanRunning)
    } else if !installer_idle() {
        Err(Fail::InstallRunning)
    } else {
        download_and_send(&offer, paused)
    };
    updater().end(r.is_ok(), crate::sys::uptime_ms());
    match &r {
        Ok(()) => log!("update: {} ({} bytes) matches SHA256SUMS and went to elfldr; the new copy takes over", offer.version, offer.elf.size),
        Err(f) => log!("update: {} not installed ({}): {}", offer.version, f.code(), f.detail()),
    }
    r
}

fn download_and_send(o: &Offer, paused: &dyn Fn() -> bool) -> Result<(), Fail> {
    let addr = SocketAddr::from(ELFLDR);
    let connect = || TcpStream::connect_timeout(&addr, Duration::from_secs(1)).map_err(|e| Fail::NoElfldr(format!("{addr}: {e}")));
    // Probe before the download; the probe connection is closed at once.
    drop(connect()?);
    // ponytail: one try each: the person waits for the answer in the page and can press Update again.
    let sums = crate::net::https_get(&o.sums.url, logic::SUMS_MAX).map_err(|e| Fail::Download(e.to_string()))?;
    let elf = crate::net::https_get(&o.elf.url, logic::ELF_MAX).map_err(|e| Fail::Download(e.to_string()))?;
    logic::verify(&elf, o, &sums).map_err(Fail::Verify)?;
    if paused() {
        return Err(Fail::GameRunning);
    }
    let mut s = connect()?;
    // A stalled elfldr must not hold this request (and the update) for good.
    let _ = s.set_write_timeout(Some(Duration::from_secs(30)));
    s.write_all(&elf)
        .and_then(|()| s.shutdown(Shutdown::Write))
        .map_err(|e| Fail::Send(format!("{} bytes to {addr}: {e}", elf.len())))
}
