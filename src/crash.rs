//! Game crashes on the console (maxpack_logic::crash): the launch watcher looks for the dump of a
//! game it mounted when the game exits, and once more when that session's cleanup ends (launch.rs
//! on_exit, recheck_exits). The last crash of each title is kept in maxpack_logic::crash::FILE.
//! See DESIGN.md "Crash report".
use maxpack_logic::crash::{find_in, load_from, save_to, Crash, DIR, FILE};
use std::collections::HashMap;
use std::path::Path;

/// The last crash of each title.
pub fn load() -> HashMap<String, Crash> {
    load_from(Path::new(FILE))
}

/// Looks for the dump of `pid` in the console's crash folder; when there is one, records it as
/// `tid`'s last crash. `when` is for the log ("at exit", "<ms> ms after exit").
/// ponytail: read-modify-write without a lock: only the launch watcher's thread writes the file.
pub fn check(tid: &str, pid: libc::pid_t, status: &str, when: &str) -> bool {
    let Some((folder, process)) = find_in(Path::new(DIR), tid, pid) else { return false };
    log!("{tid}: crashed ({process}, {status}), dump found {when}; the console's crash dump: {DIR}/{folder}");
    let at = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs());
    let mut all = load();
    all.insert(tid.to_string(), Crash { at, process, folder, status: status.to_string() });
    if let Err(e) = save_to(Path::new(FILE), &all) {
        log!("{tid}: could not save {FILE}: {e}");
    }
    true
}
