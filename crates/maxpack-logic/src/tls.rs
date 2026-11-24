//! The secure-connection self-test (src/tls.rs, `POST /api/selftest/tls`). The self-update trusts
//! what it downloads over HTTPS (the release's `SHA256SUMS` comes over the same channel as the
//! ELF), so it is only as safe as the console's certificate checks. This test shows them on the
//! console itself: libSceHttp must refuse a certificate that chains to no trusted root, one for
//! another host and an expired one, and still connect to a good site with the same chain as the
//! wrong-host one and to GitHub's API. The self-update offers and installs nothing until it has
//! passed once on this console (`passed`, `maxpack_logic::update::Fail::TlsUnchecked`).
//! Here: the probes, what each outcome means, the verdict and the record kept in FILE. Nothing
//! here touches the network.

/// The last conclusive result. Not in the game caches, not in config.toml (it is not a setting).
pub const FILE: &str = "/data/maxpack/tls_check.json";

/// What a probe must get.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Expect {
    /// The TLS layer must refuse the connection.
    Refused,
    /// The connection must work (an HTTP status comes back, whatever it is).
    Connects,
}

impl Expect {
    pub fn name(self) -> &'static str {
        match self {
            Expect::Refused => "refused",
            Expect::Connects => "connects",
        }
    }
}

pub struct Probe {
    pub url: &'static str,
    pub expect: Expect,
}

/// In this order. badssl.com and wrong.host.badssl.com serve the same `*.badssl.com` certificate
/// (Let's Encrypt, ISRG Root X1, one of src/net.rs's roots): badssl.com connecting shows that chain
/// is trusted, so wrong.host being refused shows the host check, not an unknown root. untrusted-root
/// chains to BadSSL's own root. expired.badssl.com's chain is also outside src/net.rs's roots, so
/// its refusal does not single out the date check: sceHttpsGetSslError's detail (`ssl_detail`)
/// says which checks failed.
pub const PROBES: [Probe; 5] = [
    Probe { url: "https://untrusted-root.badssl.com/", expect: Expect::Refused },
    Probe { url: "https://wrong.host.badssl.com/", expect: Expect::Refused },
    Probe { url: "https://expired.badssl.com/", expect: Expect::Refused },
    Probe { url: "https://badssl.com/", expect: Expect::Connects },
    Probe { url: "https://api.github.com/", expect: Expect::Connects },
];

/// What one request got.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// TLS went through and an HTTP status came back.
    Connected(u16),
    /// No status: the libSceHttp error code when there is one, sceHttpsGetSslError's detail when it
    /// gave one, and the message for the log and the page.
    Failed { code: Option<u32>, ssl_detail: Option<u32>, error: String },
}

/// SCE_HTTPS_ERROR_CERT and SCE_HTTPS_ERROR_HANDSHAKE (the HTTPS layer refused the peer) and
/// SCE_HTTP_ERROR_SSL (libSceHttp: the SSL layer failed). The values are PS4's and the Vita's
/// (shadPS4's http_error.h, vitasdk's psp2/net/http.h); the PS5 SDK has stubs, no headers. Resolver
/// errors (0x804360xx), timeouts and SCE_HTTPS_ERROR_IO / _INTERNAL / _PROXY are not a refusal.
const TLS_CODES: [u32; 3] = [0x8043_5060, 0x8043_5061, 0x8043_1075];
/// sceHttpsGetSslError's detail bits that name a failed certificate check: INVALID_CERT, CN_CHECK,
/// NOT_AFTER_CHECK, NOT_BEFORE_CHECK, UNKNOWN_CA (SSL_INTERNAL, 0x01, is not one).
const CERT_DETAIL: u32 = 0x02 | 0x04 | 0x08 | 0x10 | 0x20;

/// The failure is the TLS layer refusing the peer, not a failure before it (no DNS, no route, a
/// timeout) that would prove nothing.
pub fn refused_by_tls(code: Option<u32>, ssl_detail: Option<u32>) -> bool {
    code.is_some_and(|c| TLS_CODES.contains(&c)) || ssl_detail.is_some_and(|d| d & CERT_DETAIL != 0)
}

/// One probe's result.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Check {
    /// As expected.
    Ok,
    /// A certificate that must be refused was accepted: the checks are off.
    Accepted,
    /// It proves nothing: a must-refuse probe that failed before TLS, or a must-connect probe that
    /// did not connect (no internet, the site down, a root that is not in src/net.rs).
    Unproven,
}

impl Check {
    pub fn name(self) -> &'static str {
        match self {
            Check::Ok => "ok",
            Check::Accepted => "accepted",
            Check::Unproven => "unproven",
        }
    }
}

pub fn check(expect: Expect, outcome: &Outcome) -> Check {
    match (expect, outcome) {
        (Expect::Refused, Outcome::Connected(_)) => Check::Accepted,
        (Expect::Refused, Outcome::Failed { code, ssl_detail, .. }) if refused_by_tls(*code, *ssl_detail) => Check::Ok,
        (Expect::Connects, Outcome::Connected(_)) => Check::Ok,
        _ => Check::Unproven,
    }
}

/// The whole test.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verdict {
    /// Every probe as expected: the self-update may run.
    Passed,
    /// A bad certificate was accepted: the self-update stays off (also after an earlier pass).
    Broken,
    /// Nothing accepted, but not everything proven: try again; the record does not change.
    Inconclusive,
}

impl Verdict {
    pub fn name(self) -> &'static str {
        match self {
            Verdict::Passed => "passed",
            Verdict::Broken => "broken",
            Verdict::Inconclusive => "inconclusive",
        }
    }
}

/// `checks` are PROBES' in order; anything but exactly one per probe proves nothing.
pub fn verdict(checks: &[Check]) -> Verdict {
    if checks.contains(&Check::Accepted) {
        Verdict::Broken
    } else if checks.len() == PROBES.len() && checks.iter().all(|c| *c == Check::Ok) {
        Verdict::Passed
    } else {
        Verdict::Inconclusive
    }
}

/// FILE: the last conclusive verdict, when (Unix seconds, the console's clock) and which MaxPack ran it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Record {
    pub passed: bool,
    pub at: u64,
    pub version: String,
}

/// The record after a run: a pass or a broken result replaces it, an inconclusive one keeps it.
pub fn after_run(old: Option<Record>, v: Verdict, at: u64, version: &str) -> Option<Record> {
    match v {
        Verdict::Passed => Some(Record { passed: true, at, version: version.to_string() }),
        Verdict::Broken => Some(Record { passed: false, at, version: version.to_string() }),
        Verdict::Inconclusive => old,
    }
}

/// The self-update may run: the last conclusive run passed.
pub fn passed(r: Option<&Record>) -> bool {
    r.is_some_and(|r| r.passed)
}

pub fn to_json(r: &Record) -> String {
    serde_json::json!({"passed": r.passed, "at": r.at, "version": r.version}).to_string()
}

/// FILE's text. Not JSON, or `passed` not a boolean: no record (the test has to run again).
pub fn parse(text: &str) -> Option<Record> {
    let v: serde_json::Value = serde_json::from_str(text).ok()?;
    Some(Record {
        passed: v["passed"].as_bool()?,
        at: v["at"].as_u64().unwrap_or(0),
        version: v["version"].as_str().unwrap_or("").to_string(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn failed(code: Option<u32>, ssl_detail: Option<u32>) -> Outcome {
        Outcome::Failed { code, ssl_detail, error: "x".into() }
    }

    /// Each probe as it should go on a console whose checks work.
    fn good() -> Vec<Check> {
        PROBES.iter().map(|p| match p.expect {
            Expect::Refused => check(p.expect, &failed(Some(0x8043_5060), Some(0x20))),
            Expect::Connects => check(p.expect, &Outcome::Connected(200)),
        }).collect()
    }

    #[test]
    fn the_probes_need_a_refusal_and_two_controls() {
        let refused: Vec<_> = PROBES.iter().filter(|p| p.expect == Expect::Refused).map(|p| p.url).collect();
        assert_eq!(refused, ["https://untrusted-root.badssl.com/", "https://wrong.host.badssl.com/", "https://expired.badssl.com/"]);
        // wrong.host's control (same certificate) and GitHub's API, which the self-update uses.
        assert!(PROBES.iter().any(|p| p.url == "https://badssl.com/" && p.expect == Expect::Connects));
        assert!(PROBES.iter().any(|p| p.url == "https://api.github.com/" && p.expect == Expect::Connects));
        assert!(PROBES.iter().all(|p| p.url.starts_with("https://")));
    }

    #[test]
    fn only_a_tls_refusal_counts_as_refused() {
        // SCE_HTTPS_ERROR_CERT, _HANDSHAKE, SCE_HTTP_ERROR_SSL; or a certificate detail bit.
        for code in [0x8043_5060, 0x8043_5061, 0x8043_1075] {
            assert!(refused_by_tls(Some(code), None), "{code:#x}");
        }
        for bit in [0x02, 0x04, 0x08, 0x10, 0x20] {
            assert!(refused_by_tls(Some(0x8043_5062), Some(bit)), "{bit:#x}");
            assert!(refused_by_tls(None, Some(bit | 0x01)), "{bit:#x}");
        }
        // No DNS, a timeout, HTTPS IO / INTERNAL / PROXY, no code at all, SSL_INTERNAL alone.
        for code in [0x8043_6002, 0x8043_6007, 0x8043_1068, 0x8043_5062, 0x8043_5063, 0x8043_5064, 0x8041_0100] {
            assert!(!refused_by_tls(Some(code), None), "{code:#x}");
            assert!(!refused_by_tls(Some(code), Some(0)), "{code:#x}");
        }
        assert!(!refused_by_tls(None, None));
        assert!(!refused_by_tls(Some(0x8043_6002), Some(0x01)));
    }

    #[test]
    fn each_probe_against_what_it_must_get() {
        assert_eq!(check(Expect::Refused, &failed(Some(0x8043_5060), None)), Check::Ok);
        // Accepted: any HTTP status means TLS went through.
        assert_eq!(check(Expect::Refused, &Outcome::Connected(200)), Check::Accepted);
        assert_eq!(check(Expect::Refused, &Outcome::Connected(404)), Check::Accepted);
        // No DNS for the bad host proves nothing.
        assert_eq!(check(Expect::Refused, &failed(Some(0x8043_6002), None)), Check::Unproven);
        assert_eq!(check(Expect::Refused, &failed(None, None)), Check::Unproven);
        assert_eq!(check(Expect::Connects, &Outcome::Connected(200)), Check::Ok);
        assert_eq!(check(Expect::Connects, &Outcome::Connected(403)), Check::Ok);
        // A control refused (its root moved) or unreachable: unproven, never accepted.
        assert_eq!(check(Expect::Connects, &failed(Some(0x8043_5060), Some(0x20))), Check::Unproven);
        assert_eq!(check(Expect::Connects, &failed(Some(0x8043_6002), None)), Check::Unproven);
    }

    #[test]
    fn the_verdict() {
        assert_eq!(verdict(&good()), Verdict::Passed);
        // One bad certificate accepted is broken, whatever the rest did.
        let mut v = good();
        v[1] = Check::Accepted;
        assert_eq!(verdict(&v), Verdict::Broken);
        v[4] = Check::Unproven;
        assert_eq!(verdict(&v), Verdict::Broken);
        // Offline, or one control down: inconclusive.
        let mut v = good();
        v[3] = Check::Unproven;
        assert_eq!(verdict(&v), Verdict::Inconclusive);
        assert_eq!(verdict(&[Check::Unproven; 5]), Verdict::Inconclusive);
        // Not one per probe: nothing proven.
        assert_eq!(verdict(&good()[..4]), Verdict::Inconclusive);
        assert_eq!(verdict(&[]), Verdict::Inconclusive);
    }

    #[test]
    fn the_record_follows_the_last_conclusive_run() {
        assert!(!passed(None));
        let pass = after_run(None, Verdict::Passed, 100, "1.0.0");
        assert_eq!(pass, Some(Record { passed: true, at: 100, version: "1.0.0".into() }));
        assert!(passed(pass.as_ref()));
        // Inconclusive later (offline): the pass stays.
        let kept = after_run(pass.clone(), Verdict::Inconclusive, 200, "1.0.0");
        assert_eq!(kept, pass);
        // Broken later: the pass is gone.
        let broken = after_run(kept, Verdict::Broken, 300, "1.0.1");
        assert_eq!(broken, Some(Record { passed: false, at: 300, version: "1.0.1".into() }));
        assert!(!passed(broken.as_ref()));
        // Never conclusive: still nothing.
        assert_eq!(after_run(None, Verdict::Inconclusive, 1, "1.0.0"), None);
    }

    #[test]
    fn the_record_file() {
        let r = Record { passed: true, at: 1_790_000_000, version: "1.0.0".into() };
        assert_eq!(parse(&to_json(&r)), Some(r));
        assert_eq!(parse(r#"{"passed":false}"#), Some(Record { passed: false, at: 0, version: String::new() }));
        // A bad file is no record: the test has to pass again.
        for bad in ["", "not json", "[]", r#"{"passed":"yes"}"#, r#"{"at":5}"#, "null"] {
            assert_eq!(parse(bad), None, "{bad:?}");
        }
    }
}
