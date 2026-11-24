//! The secure-connection self-test (maxpack_logic::tls): `POST /api/selftest/tls` runs it from
//! Settings (or from the update notice), and the self-update offers and installs nothing until it
//! has passed once on this console (`passed`, update::install). The last conclusive result is
//! kept in maxpack_logic::tls::FILE.

use maxpack_logic::tls::{self as logic, Outcome, Record, FILE, PROBES};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

/// Each probe's resolve, connect, send and receive timeout: short, the page waits for the answer.
const PROBE_TIMEOUT_US: u32 = 8 * 1000 * 1000;

static RUNNING: AtomicBool = AtomicBool::new(false);
/// FILE, read once at the first use; None inside: no record.
static RECORD: Mutex<Option<Option<Record>>> = Mutex::new(None);

fn with_record<T>(f: impl FnOnce(&mut Option<Record>) -> T) -> T {
    let mut g = RECORD.lock().unwrap_or_else(|e| e.into_inner());
    let r = g.get_or_insert_with(|| std::fs::read_to_string(FILE).ok().and_then(|t| logic::parse(&t)));
    f(r)
}

/// The self-test passed on this console (its last conclusive run).
pub fn passed() -> bool {
    with_record(|r| logic::passed(r.as_ref()))
}

/// For /api/state `tls_check`: `{passed, at, version}` of the last conclusive run (none of the three
/// when there was none), and `running`.
pub fn summary() -> serde_json::Value {
    let mut v = with_record(|r| r.as_ref().map_or(serde_json::json!({}), |r| serde_json::json!({"passed": r.passed, "at": r.at, "version": r.version})));
    v["running"] = RUNNING.load(Ordering::SeqCst).into();
    v
}

fn hex(n: u32) -> String {
    format!("0x{n:08x}")
}

/// Runs every probe, one after the other (a second or two each when online; PROBE_TIMEOUT_US per
/// step when a site does not answer), logs and records the
/// verdict, and returns `{verdict, passed, verify_option, probes: [{url, expect, check, status?,
/// code?, ssl_detail?, error?}]}`. Err while another run is under way.
pub fn run() -> Result<serde_json::Value, &'static str> {
    if RUNNING.swap(true, Ordering::SeqCst) {
        return Err("a secure-connection check is already running");
    }
    let mut checks = Vec::new();
    let mut probes = Vec::new();
    for p in &PROBES {
        let outcome = match crate::net::probe(p.url, PROBE_TIMEOUT_US) {
            Ok(status) => Outcome::Connected(status),
            Err(e) => Outcome::Failed { code: crate::net::sce_code(&e), ssl_detail: crate::net::ssl_detail(&e), error: e.to_string() },
        };
        let c = logic::check(p.expect, &outcome);
        let mut j = serde_json::json!({"url": p.url, "expect": p.expect.name(), "check": c.name()});
        match &outcome {
            Outcome::Connected(status) => {
                log!("tls check: {} connected (HTTP {status}); it must be {}: {}", p.url, p.expect.name(), c.name());
                j["status"] = (*status).into();
            }
            Outcome::Failed { code, ssl_detail, error } => {
                log!("tls check: {error}; it must be {}: {}", p.expect.name(), c.name());
                j["error"] = error.as_str().into();
                if let Some(code) = code {
                    j["code"] = hex(*code).into();
                }
                if let Some(d) = ssl_detail {
                    j["ssl_detail"] = format!("0x{d:02x}").into();
                }
            }
        }
        checks.push(c);
        probes.push(j);
    }
    let verdict = logic::verdict(&checks);
    let at = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs());
    let passed = with_record(|r| {
        let new = logic::after_run(r.clone(), verdict, at, env!("CARGO_PKG_VERSION"));
        if new != *r {
            if let Some(rec) = &new {
                let tmp = format!("{FILE}.tmp");
                if let Err(e) = std::fs::write(&tmp, logic::to_json(rec)).and_then(|()| std::fs::rename(&tmp, FILE)) {
                    log!("tls check: could not save {FILE}: {e}");
                }
            }
            *r = new;
        }
        logic::passed(r.as_ref())
    });
    let rc = crate::net::verify_option_rc();
    log!("tls check: {}{}; the self-update is {}", verdict.name(),
        if rc == 0 { String::new() } else { format!(" (sceHttpsEnableOption failed: {})", hex(rc)) },
        if passed { "allowed" } else { "off" });
    RUNNING.store(false, Ordering::SeqCst);
    Ok(serde_json::json!({
        "verdict": verdict.name(),
        "passed": passed,
        "verify_option": if rc == 0 { "set".to_string() } else { hex(rc) },
        "probes": probes,
    }))
}

