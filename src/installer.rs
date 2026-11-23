//! The phase-2 installer inside the payload: the MaxPack engine in C (vendor/maxpack, built and
//! linked by ps5/build.sh and ps5/linker.sh), set up by ps5/installer.c as the engine's own
//! src/ps5/main_ps5.c does. It has no server of its own: web.rs hands it the page's `/api/inst/<x>`
//! requests as `/api/<x>` through `mp_api_call`. It keeps working while another mounter runs (the
//! "installer only" mode): nothing here looks at the mounter state.
use crate::web::State;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::path::PathBuf;
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

/// The engine commit vendor/maxpack was copied from (tools/sync_maxpack.sh).
const ENGINE_COMMIT: &str = include_str!("../vendor/maxpack/COMMIT");
/// The longest a quit waits for the installer to pause: a newer copy asking for the port retries
/// for 5 s (web::take_port).
const STOP_WAIT: Duration = Duration::from_secs(3);

/// `mp_api` (vendor/maxpack src/web/mp_api.h), opaque.
#[repr(C)]
struct MpApi {
    _private: [u8; 0],
}

/// `mp_api_out` (vendor/maxpack src/web/mp_api_call.h).
#[repr(C)]
struct MpApiOut {
    status: c_int,
    ctype: [c_char; 64],
    /// malloc'd; mp_api_out_free frees it.
    body: *mut u8,
    len: usize,
}

extern "C" {
    // ps5/installer.c: mp_inst_env_default + this runtime and notify, the ShadowMountPlus config
    // of /data/maxpack/config.json, mp_api_new. NULL with the message in err.
    fn maxpack_installer_new(
        notify: extern "C" fn(*mut c_void, *const c_char), runtime: *const u8, runtime_len: usize,
        runtime_mtime: i64, runtime_version: *const c_char, err: *mut c_char, errlen: usize,
    ) -> *mut MpApi;
    // vendor/maxpack src/web/mp_api_call.h: 0 = answered (out filled), -1 = out of memory or bad arguments.
    fn mp_api_call(
        api: *mut MpApi, method: *const c_char, path: *const c_char, host: *const c_char, ctype: *const c_char,
        body: *const u8, body_len: usize, loopback: c_int, out: *mut MpApiOut,
    ) -> c_int;
    fn mp_api_out_free(out: *mut MpApiOut);
    // vendor/maxpack src/web/mp_api.h: 0 once nothing runs or waits in the queue, -1 after timeout_ms.
    fn mp_api_wait_idle(api: *mut MpApi, timeout_ms: u32) -> c_int;
}

struct Api {
    ptr: *mut MpApi,
    /// One request at a time, as the engine's own single-threaded server serves it (the web server
    /// here runs one thread per connection).
    /// ponytail: a slow scan of the drives holds the other installer requests meanwhile.
    serial: Mutex<()>,
}

// SAFETY: the engine guards its state with its own mutexes (its worker and ShadowMountPlus threads
// run beside the requests); requests are serialized by `serial` anyway. The API is never freed.
unsafe impl Send for Api {}
unsafe impl Sync for Api {}

/// Set once by `start`'s thread: the API, or why it did not start.
static INSTALLER: OnceLock<Result<Api, String>> = OnceLock::new();
/// For the `notify` setting of the engine's toasts.
static STATE: OnceLock<Arc<State>> = OnceLock::new();

/// One answer of the installer, for web.rs.
pub struct Answer {
    pub status: u16,
    pub ctype: String,
    pub body: Vec<u8>,
}

/// The engine's toasts ("<name> instalado", ...): a system toast when `notify` is on, else only the
/// log, as main::toast.
extern "C" fn notify(_ctx: *mut c_void, msg: *const c_char) {
    if msg.is_null() {
        return;
    }
    // SAFETY: the engine passes a NUL-terminated message that lives for the call.
    let msg = unsafe { CStr::from_ptr(msg) }.to_string_lossy();
    let on = STATE.get().is_none_or(|s| s.settings.lock().unwrap_or_else(|e| e.into_inner()).notify);
    if on {
        crate::sys::notify(&msg);
    } else {
        log!("{msg}");
    }
}

/// The runtime's mtime for the files the installer writes (ps5/build.sh: the bundled sprx's, as the
/// engine's CMake takes it for -DMAXPACK_RUNTIME); 0 in a host `cargo check`.
fn runtime_mtime() -> i64 {
    option_env!("MAXPACK_RUNTIME_MTIME").and_then(|s| s.trim().parse().ok()).unwrap_or(0)
}

/// Starts the installer on its own thread (recovering an interrupted replace and the first scan of
/// the drives take a moment); `/api/inst/*` answers 503 "starting" until then. `runtime`: the
/// ampr-emu-ng the installer puts into what it installs (main::BUNDLED_AMPR, the same file the
/// payload bundles).
pub fn start(state: Arc<State>, runtime: &'static [u8], runtime_version: &'static str) {
    let _ = STATE.set(state);
    std::thread::spawn(move || {
        let t0 = Instant::now();
        let commit = ENGINE_COMMIT.trim();
        let commit = &commit[..commit.len().min(7)];
        let r = create(runtime, runtime_version.trim());
        match &r {
            Ok(_) => log!("installer: ready in {} ms (MaxPack engine {commit}, runtime ampr-emu-ng {} {} bytes)",
                t0.elapsed().as_millis(), runtime_version.trim(), runtime.len()),
            Err(e) => log!("installer: not started (MaxPack engine {commit}): {e}"),
        }
        let _ = INSTALLER.set(r);
    });
}

fn create(runtime: &'static [u8], version: &str) -> Result<Api, String> {
    // Lives as long as the API (the whole run): env.runtime.version keeps the pointer.
    let version: &'static CStr = Box::leak(CString::new(version).map_err(|_| "NUL in the runtime version")?.into_boxed_c_str());
    let mut err = [0 as c_char; 512];
    // SAFETY: runtime and version are 'static; err is writable for err.len() bytes.
    let ptr = unsafe {
        maxpack_installer_new(notify, runtime.as_ptr(), runtime.len(), runtime_mtime(), version.as_ptr(),
            err.as_mut_ptr(), err.len())
    };
    if ptr.is_null() {
        return Err(c_text(&err));
    }
    Ok(Api { ptr, serial: Mutex::new(()) })
}

/// A fixed C buffer up to its first NUL (or whole), lossy UTF-8.
fn c_text(buf: &[c_char]) -> String {
    let bytes: Vec<u8> = buf.iter().take_while(|&&c| c != 0).map(|&c| c as u8).collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

fn json_error(status: u16, code: &str, detail: &str) -> Answer {
    let body = serde_json::json!({"ok": false, "code": code, "detail": detail}).to_string().into_bytes();
    Answer { status, ctype: "application/json".into(), body }
}

/// One request to the installer: `path` is its own route (maxpack_logic::inst::installer_path),
/// `host` and `ctype` as the browser sent them (the engine checks both), `loopback` when the
/// request comes from the console itself.
pub fn call(method: &str, path: &str, host: &str, ctype: &str, body: &[u8], loopback: bool) -> Answer {
    match INSTALLER.get() {
        None => json_error(503, "starting", "the installer is starting"),
        Some(Err(e)) => json_error(503, "no_installer", e),
        Some(Ok(api)) => {
            let _one = api.serial.lock().unwrap_or_else(|e| e.into_inner());
            raw_call(api.ptr, method, path, host, ctype, body, loopback)
        }
    }
}

fn raw_call(api: *mut MpApi, method: &str, path: &str, host: &str, ctype: &str, body: &[u8], loopback: bool) -> Answer {
    let (Ok(m), Ok(p), Ok(h), Ok(c)) = (CString::new(method), CString::new(path), CString::new(host), CString::new(ctype))
    else {
        return json_error(400, "bad_request", "NUL in the request");
    };
    let mut out = MpApiOut { status: 0, ctype: [0; 64], body: std::ptr::null_mut(), len: 0 };
    // SAFETY: every string is NUL-terminated and outlives the call; body is readable for its length;
    // out is ours, and mp_api_out_free takes back what the engine allocated in it.
    let rc = unsafe {
        mp_api_call(api, m.as_ptr(), p.as_ptr(), h.as_ptr(), c.as_ptr(), body.as_ptr(), body.len(), loopback as c_int, &mut out)
    };
    if rc != 0 {
        unsafe { mp_api_out_free(&mut out) };
        log!("installer: {method} {path}: mp_api_call failed (out of memory?)");
        return json_error(500, "io_error", "the installer could not answer");
    }
    let body = if out.body.is_null() {
        Vec::new()
    } else {
        // SAFETY: the engine's body holds out.len bytes until mp_api_out_free.
        unsafe { std::slice::from_raw_parts(out.body, out.len) }.to_vec()
    };
    let status = u16::try_from(out.status).unwrap_or(500);
    let ctype = c_text(&out.ctype);
    unsafe { mp_api_out_free(&mut out) };
    Answer { status, ctype, body }
}

/// How long an accepted uninstall is followed (it deletes a whole game folder; a USB drive is slow).
const UNINSTALL_WAIT: Duration = Duration::from_secs(3600);

/// After the installer accepted an uninstall of `tid` (web.rs): its `/api/state` is read every
/// 500 ms (maxpack_logic::inst::UninstallWatch) until the job ends; once the game is uninstalled the
/// scan loop is woken and web::uninstalled takes MaxPack's home entry and mount of it away.
/// `source`: the library's copy of `tid` when the uninstall was asked for.
pub fn after_uninstall(tid: String, source: Option<PathBuf>) {
    use maxpack_logic::inst::{Uninstall, UninstallWatch};
    let Some(state) = STATE.get().cloned() else { return };
    std::thread::spawn(move || {
        let deadline = Instant::now() + UNINSTALL_WAIT;
        let mut watch = UninstallWatch::default();
        loop {
            std::thread::sleep(Duration::from_millis(500));
            let a = call("GET", "/api/state", "127.0.0.1", "", b"", true);
            match watch.see(&a.body, &tid) {
                Uninstall::Done => break,
                Uninstall::Failed => {
                    log!("{tid}: the installer's uninstall failed; MaxPack's home entry and mount stay");
                    state.wake();
                    return;
                }
                Uninstall::Pending if Instant::now() >= deadline => {
                    log!("{tid}: the installer's uninstall did not end in {} s; MaxPack leaves its home entry to auto-remove",
                        UNINSTALL_WAIT.as_secs());
                    return;
                }
                Uninstall::Pending => {}
            }
        }
        state.wake();
        crate::web::uninstalled(&state, &tid, source.as_deref(), deadline);
    });
}

/// Before the payload quits (POST /api/quit, or another copy took the port): the running install
/// is paused as the page's Pause does (it saves its state: the next start lists it under Install,
/// unfinished, to resume), and the queue is waited for up to STOP_WAIT. Each pause stops the job
/// running then; a queued job that starts meanwhile is paused on the next round.
/// ponytail: a verify or uninstall that has not ended by then is cut off with the payload (the
/// engine recovers an interrupted replace at its next start); jobs still queued are dropped.
pub fn stop() {
    let Some(Ok(api)) = INSTALLER.get() else { return };
    // SAFETY: the API lives for the whole run; mp_api_wait_idle only takes the engine's own lock.
    if unsafe { mp_api_wait_idle(api.ptr, 0) } == 0 {
        return;
    }
    log!("installer: pausing before quitting");
    let deadline = Instant::now() + STOP_WAIT;
    loop {
        // Not through `serial`: a long scan must not hold the quit up, and /api/pause only sets a
        // flag under the engine's lock.
        let a = raw_call(api.ptr, "POST", "/api/pause", "127.0.0.1", "application/json", b"{}", true);
        if a.status != 200 {
            log!("installer: pause: HTTP {} {}", a.status, String::from_utf8_lossy(&a.body));
        }
        if unsafe { mp_api_wait_idle(api.ptr, 250) } == 0 {
            log!("installer: paused; an unfinished install resumes from Install at the next start");
            return;
        }
        if Instant::now() >= deadline {
            log!("installer: still busy after {} s; quitting anyway (an install resumes from its saved state)",
                STOP_WAIT.as_secs());
            return;
        }
    }
}
