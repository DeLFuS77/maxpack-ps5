//! Self-update of the payload (src/update.rs): the latest release of DeLFuS77/maxpack-ps5 on
//! GitHub, looked at once per start, and, when the page asks, its `maxpack.elf`, checked against
//! the release's `SHA256SUMS` before it goes to elfldr. Here: which release is offered and with
//! which assets, what a failed look means, the checksum check, and the state that keeps scan passes
//! and a second update off while one runs. Nothing here touches the network.

/// The repository whose releases are looked at.
pub const REPO: &str = "DeLFuS77/maxpack-ps5";
/// GitHub's latest published release (not a draft, not a pre-release). 404 while there is none,
/// and while the repository is private (the request carries no token).
pub const LATEST_URL: &str = "https://api.github.com/repos/DeLFuS77/maxpack-ps5/releases/latest";
/// The release assets: the payload, and the checksums the release job writes (`sha256sum`).
pub const ELF_ASSET: &str = "maxpack.elf";
pub const SUMS_ASSET: &str = "SHA256SUMS";
/// Read limits: the API answer, SHA256SUMS and the ELF.
pub const LATEST_MAX: usize = 1 << 20;
pub const SUMS_MAX: usize = 64 << 10;
pub const ELF_MAX: usize = 32 << 20;
/// The start-up look: at most this many tries, this far apart, and only after no answer or a server
/// error (`look_failed`).
pub const LOOK_TRIES: u32 = 3;
pub const LOOK_DELAY_SECS: u64 = 20;
/// After the ELF went to elfldr: no scan pass and no second update for this long, so the new copy
/// finds no build to wait for. It runs out by itself, so a new copy that never starts only delays
/// the scan.
pub const HOLD_MS: u64 = 90_000;

/// A release asset: its download URL and size.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Asset {
    pub url: String,
    pub size: usize,
}

/// A newer release the page offers: its tag and both assets.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Offer {
    pub version: String,
    pub elf: Asset,
    pub sums: Asset,
}

/// What the latest release is to the running copy.
#[derive(Debug, PartialEq, Eq)]
pub enum Latest {
    /// Newer, with both assets: offered.
    Newer(Offer),
    /// The running version or older (its tag): nothing offered.
    NotNewer(String),
    /// Not offered, and why (for the log): no ELF or no SHA256SUMS, an asset elsewhere, a tag that is
    /// not `vX.Y.Z`, a draft or a pre-release, an answer that does not parse.
    Unusable(String),
}

/// What a releases/latest answer (`body`) offers to the running version (`running`, Cargo.toml's).
pub fn latest(body: &[u8], running: &str) -> Latest {
    let _ = (body, running);
    Latest::Unusable(String::new())
}

/// What a failed look at LATEST_URL means; `status` is the HTTP status, None when no answer came.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum LookFailed {
    /// 404: no published release (none yet, or the repository is private). One log line, no retry.
    NoRelease,
    /// No answer, or a server error: tried again, LOOK_TRIES in all.
    TryAgain,
    /// Any other answer (a 403 rate limit, say): logged, not tried again before the next start.
    GiveUp,
}

pub fn look_failed(status: Option<u16>) -> LookFailed {
    let _ = status;
    LookFailed::TryAgain
}

/// ELF or SELF (the signed form elfldr also takes).
pub fn is_module(b: &[u8]) -> bool {
    let _ = b;
    false
}

/// The SHA-256 that `sums` (a SHA256SUMS file: `<64 hex>  <name>` or `<64 hex> *<name>` lines)
/// lists for `name`. Err: not text, no line for `name`, or more than one.
pub fn listed_sha256(sums: &[u8], name: &str) -> Result<[u8; 32], String> {
    let _ = (sums, name);
    Err(String::new())
}

/// The downloaded ELF is the offered one: the asset's size, an ELF or SELF, and the SHA-256 that the
/// release's SHA256SUMS (`sums`) lists for maxpack.elf. Err says what differs; nothing is sent then.
pub fn verify(elf: &[u8], offer: &Offer, sums: &[u8]) -> Result<(), String> {
    let _ = (elf, offer, sums);
    Ok(())
}

/// Why an update did not reach elfldr: the HTTP status and code the page gets, and the detail.
#[derive(Debug, PartialEq, Eq, Clone)]
pub enum Fail {
    /// The version asked for is not the one offered (none was, or another one).
    NotOffered,
    /// A game runs: the new copy's take-over would be refused (its overlay stays mounted).
    GameRunning,
    /// An update runs, or its ELF was just sent (HOLD_MS).
    UpdateRunning,
    /// A scan pass (a build) runs: the new copy would have to wait for it longer than it waits.
    ScanRunning,
    /// The installer copies a game: quitting would pause it.
    InstallRunning,
    /// elfldr does not answer on 127.0.0.1:9021.
    NoElfldr(String),
    /// A download failed (SHA256SUMS or the ELF).
    Download(String),
    /// The ELF is not the release's: size, magic or SHA-256.
    Verify(String),
    /// Sending it to elfldr failed.
    Send(String),
}

impl Fail {
    pub fn status(&self) -> u16 {
        500
    }

    /// The page shows its own text for each code (ui.html `updFail_<code>`).
    pub fn code(&self) -> &'static str {
        ""
    }

    /// English, for the log and the page's details.
    pub fn detail(&self) -> String {
        String::new()
    }
}

/// The offer found at start and the update in flight, kept behind one lock (src/update.rs).
#[derive(Debug, Default)]
pub struct Updater {
    offer: Option<Offer>,
    updating: bool,
    /// uptime ms until which scan passes and updates stay off after a send (0: none).
    hold_until: u64,
}

impl Updater {
    pub const fn new() -> Updater {
        Updater { offer: None, updating: false, hold_until: 0 }
    }

    pub fn set_offer(&mut self, offer: Option<Offer>) {
        let _ = offer;
    }

    pub fn offer(&self) -> Option<&Offer> {
        None
    }

    /// An update runs or its ELF was sent less than HOLD_MS ago: no scan pass starts.
    pub fn holding(&self, now_ms: u64) -> bool {
        let _ = now_ms;
        false
    }

    /// Starts the update to `version`: the offer it installs, or why not. `game_running`: a game
    /// runs now.
    pub fn begin(&mut self, version: &str, game_running: bool, now_ms: u64) -> Result<Offer, Fail> {
        let _ = (version, game_running, now_ms);
        Err(Fail::NotOffered)
    }

    /// The update ended; `sent`: its ELF went to elfldr (then HOLD_MS of hold).
    pub fn end(&mut self, sent: bool, now_ms: u64) {
        let _ = (sent, now_ms);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sha256;
    use serde_json::json;

    const ELF: &[u8] = b"\x7fELF\x02\x01\x01\x09 a tiny stand-in for maxpack.elf";

    fn asset(tag: &str, name: &str, size: usize) -> serde_json::Value {
        json!({"name": name, "size": size,
            "browser_download_url": format!("https://github.com/DeLFuS77/maxpack-ps5/releases/download/{tag}/{name}")})
    }

    fn release(tag: &str, assets: Vec<serde_json::Value>) -> Vec<u8> {
        json!({"tag_name": tag, "draft": false, "prerelease": false, "assets": assets}).to_string().into_bytes()
    }

    fn both(tag: &str) -> Vec<u8> {
        release(tag, vec![asset(tag, "maxpack.elf", 3_456_789), asset(tag, "SHA256SUMS", 78), asset(tag, "payloads.json", 600)])
    }

    fn offer() -> Offer {
        let dl = "https://github.com/DeLFuS77/maxpack-ps5/releases/download/v1.0.1/";
        Offer {
            version: "v1.0.1".into(),
            elf: Asset { url: format!("{dl}maxpack.elf"), size: ELF.len() },
            sums: Asset { url: format!("{dl}SHA256SUMS"), size: 78 },
        }
    }

    fn sums_for(data: &[u8]) -> Vec<u8> {
        format!("{}  maxpack.elf\n", sha256::hex(&sha256::digest(data))).into_bytes()
    }

    #[test]
    fn a_newer_release_with_both_assets_is_offered() {
        let dl = "https://github.com/DeLFuS77/maxpack-ps5/releases/download/v1.0.1/";
        assert_eq!(latest(&both("v1.0.1"), "1.0.0"), Latest::Newer(Offer {
            version: "v1.0.1".into(),
            elf: Asset { url: format!("{dl}maxpack.elf"), size: 3_456_789 },
            sums: Asset { url: format!("{dl}SHA256SUMS"), size: 78 },
        }));
        assert!(matches!(latest(&both("v2.0.0"), "1.9.9"), Latest::Newer(o) if o.version == "v2.0.0"));
        // Numbers, not text: 1.10.0 is newer than 1.9.0.
        assert!(matches!(latest(&both("v1.10.0"), "1.9.0"), Latest::Newer(_)));
    }

    #[test]
    fn the_same_or_an_older_release_is_not_offered() {
        assert_eq!(latest(&both("v1.0.0"), "1.0.0"), Latest::NotNewer("v1.0.0".into()));
        assert_eq!(latest(&both("v0.9.9"), "1.0.0"), Latest::NotNewer("v0.9.9".into()));
    }

    #[test]
    fn a_release_that_cannot_be_checked_or_is_elsewhere_is_not_offered() {
        let unusable = |body: Vec<u8>| matches!(latest(&body, "1.0.0"), Latest::Unusable(why) if !why.is_empty());
        // No SHA256SUMS: the ELF could not be checked.
        assert!(unusable(release("v1.0.1", vec![asset("v1.0.1", "maxpack.elf", 100)])));
        assert!(unusable(release("v1.0.1", vec![asset("v1.0.1", "SHA256SUMS", 78)])));
        // An asset whose URL is not this repository's download of this tag.
        let mut moved = asset("v1.0.1", "maxpack.elf", 100);
        moved["browser_download_url"] = json!("https://example.com/maxpack.elf");
        assert!(unusable(release("v1.0.1", vec![moved, asset("v1.0.1", "SHA256SUMS", 78)])));
        assert!(unusable(release("v1.0.1", vec![asset("v1.0.0", "maxpack.elf", 100), asset("v1.0.1", "SHA256SUMS", 78)])));
        // Empty, or over the read limits.
        assert!(unusable(release("v1.0.1", vec![asset("v1.0.1", "maxpack.elf", 0), asset("v1.0.1", "SHA256SUMS", 78)])));
        assert!(unusable(release("v1.0.1", vec![asset("v1.0.1", "maxpack.elf", ELF_MAX + 1), asset("v1.0.1", "SHA256SUMS", 78)])));
        assert!(unusable(release("v1.0.1", vec![asset("v1.0.1", "maxpack.elf", 100), asset("v1.0.1", "SHA256SUMS", SUMS_MAX + 1)])));
        // Not vX.Y.Z (a pre-release name), a draft or a pre-release, or not the answer expected.
        assert!(unusable(both("v1.1.0-beta.1")));
        assert!(unusable(both("latest")));
        let mut draft: serde_json::Value = serde_json::from_slice(&both("v1.0.1")).unwrap();
        draft["prerelease"] = json!(true);
        assert!(unusable(draft.to_string().into_bytes()));
        draft["prerelease"] = json!(false);
        draft["draft"] = json!(true);
        assert!(unusable(draft.to_string().into_bytes()));
        assert!(unusable(b"not json".to_vec()));
        assert!(unusable(br#"{"message":"Not Found"}"#.to_vec()));
        // A running version that is not X.Y.Z offers nothing either.
        assert!(matches!(latest(&both("v1.0.1"), "dev"), Latest::Unusable(_)));
    }

    #[test]
    fn a_failed_look_is_tried_again_only_without_an_answer_or_on_a_server_error() {
        assert_eq!(look_failed(Some(404)), LookFailed::NoRelease);
        assert_eq!(look_failed(None), LookFailed::TryAgain);
        assert_eq!(look_failed(Some(500)), LookFailed::TryAgain);
        assert_eq!(look_failed(Some(503)), LookFailed::TryAgain);
        assert_eq!(look_failed(Some(403)), LookFailed::GiveUp);
        assert_eq!(look_failed(Some(401)), LookFailed::GiveUp);
        assert_eq!(look_failed(Some(301)), LookFailed::GiveUp);
    }

    #[test]
    fn modules_are_elf_or_self() {
        assert!(is_module(ELF));
        assert!(is_module(&[0x4f, 0x15, 0x3d, 0x1d, 0]));
        assert!(!is_module(b"<!DOCTYPE html>"));
        assert!(!is_module(b"\x7fEL"));
    }

    #[test]
    fn sha256sums_lines() {
        let a = "ab".repeat(32);
        let b = "CD".repeat(32);
        let want_a = [0xab; 32];
        assert_eq!(listed_sha256(format!("{a}  maxpack.elf\n").as_bytes(), "maxpack.elf"), Ok(want_a));
        // Binary-mode marker, CRLF, other files around it, upper-case hex.
        let text = format!("{b}  payloads.json\r\n{a} *maxpack.elf\r\n\r\n");
        assert_eq!(listed_sha256(text.as_bytes(), "maxpack.elf"), Ok(want_a));
        assert_eq!(listed_sha256(format!("{b}  maxpack.elf").as_bytes(), "maxpack.elf"), Ok([0xcd; 32]));
        // Not listed; listed twice; a name that only starts or ends the same; a short or bad hash; not text.
        for bad in [
            format!("{a}  payloads.json\n"),
            format!("{a}  maxpack.elf\n{b}  maxpack.elf\n"),
            format!("{a}  maxpack.elf.sig\n{a}  old-maxpack.elf\n"),
            format!("{}  maxpack.elf\n", &a[..62]),
            format!("{}zz  maxpack.elf\n", &a[..62]),
            format!("{a} maxpack.elf\n"),
            String::new(),
        ] {
            assert!(listed_sha256(bad.as_bytes(), "maxpack.elf").is_err(), "{bad:?}");
        }
        assert!(listed_sha256(b"\xff\xfe", "maxpack.elf").is_err());
    }

    #[test]
    fn only_the_released_elf_passes() {
        let o = offer();
        assert_eq!(verify(ELF, &o, &sums_for(ELF)), Ok(()));
        // One byte changed: same size and magic, another SHA-256.
        let mut changed = ELF.to_vec();
        *changed.last_mut().unwrap() ^= 1;
        assert!(verify(&changed, &o, &sums_for(ELF)).unwrap_err().contains("SHA-256"));
        // Cut short, or longer than the asset.
        assert!(verify(&ELF[..ELF.len() - 1], &o, &sums_for(ELF)).is_err());
        // An HTML error page of the same size.
        let html = vec![b'<'; ELF.len()];
        assert!(verify(&html, &o, &sums_for(&html)).is_err());
        // SHA256SUMS without maxpack.elf.
        assert!(verify(ELF, &o, b"").is_err());
    }

    #[test]
    fn fail_codes() {
        let all = [
            (Fail::NotOffered, 400, "not_offered"),
            (Fail::GameRunning, 409, "game_running"),
            (Fail::UpdateRunning, 409, "update_running"),
            (Fail::ScanRunning, 409, "scan_running"),
            (Fail::InstallRunning, 409, "install_running"),
            (Fail::NoElfldr("refused".into()), 503, "no_elfldr"),
            (Fail::Download("HTTP status 404".into()), 502, "download"),
            (Fail::Verify("SHA-256".into()), 502, "checksum"),
            (Fail::Send("broken pipe".into()), 502, "send"),
        ];
        for (f, status, code) in all {
            assert_eq!((f.status(), f.code()), (status, code), "{f:?}");
            assert!(!f.detail().is_empty(), "{f:?}");
        }
        assert!(Fail::Download("HTTP status 404".into()).detail().contains("HTTP status 404"));
    }

    #[test]
    fn one_update_at_a_time_and_no_pass_meanwhile() {
        let mut u = Updater::new();
        // Nothing offered (no release, or the start-up look has not answered yet).
        assert_eq!(u.begin("v1.0.1", false, 1000), Err(Fail::NotOffered));
        u.set_offer(Some(offer()));
        assert_eq!(u.offer(), Some(&offer()));
        assert_eq!(u.begin("v1.0.2", false, 1000), Err(Fail::NotOffered));
        assert_eq!(u.begin("v1.0.1", true, 1000), Err(Fail::GameRunning));
        assert!(!u.holding(1000));
        assert_eq!(u.begin("v1.0.1", false, 1000), Ok(offer()));
        // While it downloads: no pass, no second update.
        assert!(u.holding(1000));
        assert_eq!(u.begin("v1.0.1", false, 1001), Err(Fail::UpdateRunning));
        // Failed: free again at once.
        u.end(false, 2000);
        assert!(!u.holding(2000));
        assert_eq!(u.begin("v1.0.1", false, 3000), Ok(offer()));
        // Sent: held for HOLD_MS, then free (a new copy that never started only delayed the scan).
        u.end(true, 4000);
        assert!(u.holding(4000 + HOLD_MS - 1));
        assert_eq!(u.begin("v1.0.1", false, 4000 + HOLD_MS - 1), Err(Fail::UpdateRunning));
        assert!(!u.holding(4000 + HOLD_MS));
        assert_eq!(u.begin("v1.0.1", false, 4000 + HOLD_MS), Ok(offer()));
    }
}
