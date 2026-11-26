//! One job of the MaxPack engine at a time (DESIGN.md "Console jobs"): the game sheet's verify (src/verify.rs)
//! or one of the installer's jobs (install, verify, uninstall). The engine's progress hook and cancel flag are one
//! per process, so the two never run side by side. GATE is held while a console job is started (the installer
//! checked idle, the job set) and while web.rs forwards an installer request that starts a job (`hold_off`), so
//! neither slips in beside the other.
use crate::web::State;
use maxpack_logic::job::Busy;
use std::sync::{Mutex, MutexGuard};

static GATE: Mutex<()> = Mutex::new(());

/// Held while a console job checks `busy` and sets itself up.
pub fn gate() -> MutexGuard<'static, ()> {
    GATE.lock().unwrap_or_else(|e| e.into_inner())
}

/// The job of the engine that runs now, if any: the installer's (one running or queued), a verify.
pub fn busy() -> Option<Busy> {
    maxpack_logic::job::busy(!crate::installer::idle(), crate::verify::running(), false)
}

/// For an installer request that starts a job (maxpack_logic::job::blocks_installer): None while a console job
/// runs; else the guard to hold while the request is forwarded.
pub fn hold_off() -> Option<MutexGuard<'static, ()>> {
    let g = gate();
    (!crate::verify::running()).then_some(g)
}

/// A game runs now, whoever started it (`paused` only knows MaxPack's launches), or the process list could not
/// be read to tell: true, with what to log.
pub fn game_running(state: &State) -> Option<String> {
    let running: Vec<String> = state.running.lock().unwrap_or_else(|e| e.into_inner()).iter().cloned().collect();
    if !running.is_empty() {
        return Some(running.join(", "));
    }
    match crate::launch::games_running() {
        Ok(g) if g.is_empty() => None,
        Ok(g) => Some(g.join(", ")),
        Err(e) => Some(format!("the process list could not be read: {e}")),
    }
}
