//! The recommended per-game settings (maxpack_logic::recommended; DESIGN.md "Recommended settings"):
//! the list bundled in the ELF (`data/recommended.json`), or the copy last downloaded from GitHub
//! (CACHED) when its version is higher. Every reader of a game's Fakelib mode or PlayGo stub asks
//! `effective`, so a game whose settings the user chose keeps them and the others get the list's.

use crate::config::Settings;
use maxpack_logic::recommended::{self as logic, Effective, Entry, List};
use std::io::Read;
use std::sync::{Arc, OnceLock, RwLock};

/// The list in the repository, the only place it is downloaded from.
pub const URL: &str = "https://raw.githubusercontent.com/DeLFuS77/maxpack-ps5/main/data/recommended.json";
/// The last downloaded copy that was newer than the bundled one.
pub const CACHED: &str = "/data/maxpack/recommended.json";
const BUNDLED: &[u8] = include_bytes!("../data/recommended.json");

/// The list in use; None until `init` (or the first `current`).
static CURRENT: RwLock<Option<Arc<List>>> = RwLock::new(None);

fn bundled() -> List {
    // maxpack_logic::recommended::tests::bundled_list_parses checks this very file.
    logic::parse(BUNDLED).expect("data/recommended.json bundled in the ELF does not parse")
}

/// CACHED as a list: None when there is none; one that cannot be read or parsed is logged.
fn cached() -> Option<List> {
    let mut b = Vec::new();
    let read = std::fs::File::open(CACHED).and_then(|f| f.take(logic::MAX_BYTES as u64 + 1).read_to_end(&mut b));
    match read {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
        Err(e) => {
            log!("recommended: cached copy ignored: read {CACHED}: {e}");
            None
        }
        Ok(_) => match logic::parse(&b) {
            Ok(l) => Some(l),
            Err(e) => {
                log!("recommended: cached copy ignored: {e}");
                None
            }
        },
    }
}

/// At start: the bundled list, or CACHED when it parses and its version is higher. One log line says
/// which is used.
pub fn init() {
    let bundled = bundled();
    let cached = cached();
    if let Some(c) = cached.as_ref().filter(|c| c.version <= bundled.version) {
        log!("recommended: cached copy ignored: v{} is not newer than the bundled v{}", c.version, bundled.version);
    }
    let from = if cached.as_ref().is_some_and(|c| c.version > bundled.version) { "downloaded" } else { "bundled" };
    let list = logic::newer(&bundled, cached.as_ref()).clone();
    log!("recommended: v{} ({} titles, {from})", list.version, list.len());
    set_current(list);
}

/// The list in use.
pub fn current() -> Arc<List> {
    if let Some(l) = CURRENT.read().unwrap_or_else(|e| e.into_inner()).as_ref() {
        return l.clone();
    }
    let mut w = CURRENT.write().unwrap_or_else(|e| e.into_inner());
    w.get_or_insert_with(|| Arc::new(bundled())).clone()
}

/// Puts `list` in use.
fn set_current(list: List) {
    *CURRENT.write().unwrap_or_else(|e| e.into_inner()) = Some(Arc::new(list));
}

/// The once-per-start refresh, in its own thread (`recommended_update`, after the secure-connection
/// self-test has passed on this console): URL downloaded (MAX_BYTES at most; tried again as the
/// self-update's look, after no answer or a server error), checked whole with
/// maxpack_logic::recommended::parse, and put in use, saved to CACHED and the scan loop woken only when
/// its version is higher than the list's in use. Anything else is one log line, and the list in use
/// stays. Nothing about the console's games is sent: the whole list is downloaded.
pub fn spawn_refresh(state: Arc<crate::web::State>) {
    let on = state.settings.lock().unwrap().recommended_update;
    if !on {
        log!("recommended: refresh off");
        return;
    }
    if !crate::tls::passed() {
        log!("recommended: refresh waits for the secure connection check");
        return;
    }
    std::thread::spawn(move || {
        let body = match download() {
            Ok(b) => b,
            Err(e) => {
                log!("recommended: download failed: {e}; the list in use stays until the next start");
                return;
            }
        };
        let list = match logic::parse(&body) {
            Ok(l) => l,
            Err(e) => {
                log!("recommended: download refused: {e}");
                return;
            }
        };
        let now = current();
        if list.version <= now.version {
            log!("recommended: kept v{} (download is not newer)", now.version);
            return;
        }
        let tmp = format!("{CACHED}.tmp");
        if let Err(e) = std::fs::write(&tmp, &body).and_then(|()| std::fs::rename(&tmp, CACHED)) {
            // Used anyway until the next start, which goes back to the copy it finds.
            let _ = std::fs::remove_file(&tmp);
            log!("recommended: could not save {CACHED}: {e}");
        }
        let (version, n) = (list.version, list.len());
        set_current(list);
        log!("recommended: downloaded v{version} ({n} titles)");
        state.wake();
    });
}

/// GET URL, LOOK_TRIES at most as the self-update's look (maxpack_logic::update::look_failed): tried
/// again only after no answer or a server error.
fn download() -> std::io::Result<Vec<u8>> {
    use maxpack_logic::update::{look_failed, LookFailed, LOOK_DELAY_SECS, LOOK_TRIES};
    let mut tries = 0;
    loop {
        tries += 1;
        match crate::net::https_get(URL, logic::MAX_BYTES) {
            Ok(b) => return Ok(b),
            Err(e) if tries < LOOK_TRIES && look_failed(crate::net::status_of(&e)) == LookFailed::TryAgain => {
                log!("recommended: {e} (try {tries} of {LOOK_TRIES})");
                std::thread::sleep(std::time::Duration::from_secs(LOOK_DELAY_SECS));
            }
            Err(e) => return Err(e),
        }
    }
}

/// The console's firmware, read once (it does not change while MaxPack runs).
fn firmware() -> Option<(u8, u8)> {
    static FW: OnceLock<Option<(u8, u8)>> = OnceLock::new();
    *FW.get_or_init(crate::sys::firmware_bcd)
}

/// The list's entry for `tid` on this console's firmware, if one applies.
pub fn entry(tid: &str) -> Option<Entry> {
    logic::entry_for(&current(), tid, firmware()).copied()
}

/// The Fakelib mode, the PlayGo stub and the notice of `tid` as used: the user's choice, else the list's.
pub fn effective(tid: &str, s: &Settings) -> Effective {
    let e = entry(tid);
    logic::effective(tid, &s.fakelib_emulators, &s.fakelib_off, &s.playgo_off, &s.manual_settings, e.as_ref())
}
