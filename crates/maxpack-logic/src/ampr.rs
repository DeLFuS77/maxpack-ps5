//! The ampr-emu-ng update (src/main.rs `ampr_update`: `ampr_auto_update` at start, and Settings'
//! "Update ampr-emu-ng now", `POST /api/ampr/update`). Every game loads the `libSceAmpr.sprx` it
//! installs, so it is checked as the payload's own self-update checks `maxpack.elf` (`crate::update`,
//! whose SHA256SUMS parsing, checksum check and gate it reuses): the latest stable release of
//! DeLFuS77/ampr-emu-ng, its `libSceAmpr.sprx` installed only when its SHA-256 is the one the
//! release's `SHA256SUMS` lists for it, and nothing downloaded until the secure-connection self-test
//! has passed on this console. Nothing here touches the network.

use crate::update::{self, Asset};

/// The repository whose releases are looked at: MaxPack's own build of ampr-emu-ng.
pub const REPO: &str = "DeLFuS77/ampr-emu-ng";
/// GitHub's latest published release (not a draft, not a pre-release: betas are pre-releases).
pub const LATEST_URL: &str = "https://api.github.com/repos/DeLFuS77/ampr-emu-ng/releases/latest";
/// The asset installed. The release also carries `libSceAmpr-debug.sprx`, never installed.
pub const SPRX_ASSET: &str = "libSceAmpr.sprx";
/// Read limits: the API answer, SHA256SUMS (as the self-update's) and the sprx.
pub const LATEST_MAX: usize = update::LATEST_MAX;
pub const SUMS_MAX: usize = update::SUMS_MAX;
pub const SPRX_MAX: usize = 16 << 20;

/// A releases/latest answer: its tag, its libSceAmpr.sprx and its SHA256SUMS. `sums` is Err (why)
/// when the release has no usable SHA256SUMS: the look still says whether an update is due, but
/// nothing is installed from such a release.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Release {
    pub tag: String,
    pub sprx: Asset,
    pub sums: Result<Asset, String>,
}

/// The release in a releases/latest answer (`body`). Err: not the answer expected, a draft or a
/// pre-release, or not exactly one libSceAmpr.sprx at REPO's download URL for the tag within SPRX_MAX.
pub fn release(body: &[u8]) -> Result<Release, String> {
    let j: serde_json::Value = serde_json::from_slice(body).map_err(|e| format!("bad JSON: {e}"))?;
    let tag = j["tag_name"].as_str().ok_or("no tag_name")?;
    if j["draft"].as_bool() != Some(false) || j["prerelease"].as_bool() != Some(false) {
        return Err(format!("{tag}: a draft or a pre-release"));
    }
    let assets = j["assets"].as_array().map_or(&[][..], |a| a.as_slice());
    let sprx = update::pick(assets, REPO, tag, SPRX_ASSET, SPRX_MAX)?;
    let sums = update::pick(assets, REPO, tag, update::SUMS_ASSET, SUMS_MAX);
    Ok(Release { tag: tag.to_string(), sprx, sums })
}

/// Nothing is downloaded until the secure-connection self-test has passed on this console: the
/// self-update's gate (`update::tls_gate`), with its 409 and code.
pub fn gate(tls_passed: bool) -> Result<(), Fail> {
    update::tls_gate(tls_passed).map_err(|_| Fail::TlsUnchecked)
}

/// The downloaded libSceAmpr.sprx (`sprx`) is the release's: the asset's `size`, an ELF or SELF, and
/// the SHA-256 that the release's SHA256SUMS (`sums`) lists for libSceAmpr.sprx (that exact name, 64
/// hex digits). Err says what differs; nothing is installed then.
pub fn verify(sprx: &[u8], size: usize, sums: &[u8]) -> Result<(), String> {
    update::verify_asset(sprx, SPRX_ASSET, size, sums)
}

/// The version that `/data/maxpack/ampr.bundled`'s text (`mark`: `<version> <len> <crc32>`, or `<len>
/// <crc32>` from a v0.4.0 build) records when it records exactly a file of `len` bytes and that crc32:
/// Some(None) for the old format, None when it records another file or does not parse.
pub fn mark_version(mark: &str, len: usize, crc: u32) -> Option<Option<&str>> {
    let f: Vec<&str> = mark.split_whitespace().collect();
    let (v, l, c) = match f[..] {
        [l, c] => (None, l, c),
        [v, l, c] => (Some(v), l, c),
        _ => return None,
    };
    (l.parse::<usize>().ok()? == len && u32::from_str_radix(c, 16).ok()? == crc).then_some(v)
}

/// The version MaxPack knows of a libSceAmpr.sprx (the ampr dir's, or the copy a game's fakelib/ got):
/// `file` is its (len, crc32, equals the bundled copy), None when it is missing. The bundled bytes are the
/// `bundled` version; other bytes are the version the mark records for exactly them (a release the update
/// downloaded, or a copy another MaxPack bundled). None (unknown) for anything else: a file put there by
/// hand, one the mark does not describe (written after it, or the mark could not be written), a v0.4.0
/// mark, no file. A .p5z that grew never plays with an unknown one (crate::p5z::ng_plays).
pub fn known_version<'a>(file: Option<(usize, u32, bool)>, bundled: &'a str, mark: Option<&'a str>) -> Option<&'a str> {
    match file? {
        (_, _, true) => Some(bundled),
        (len, crc, false) => mark.and_then(|m| mark_version(m, len, crc)).flatten(),
    }
}

/// Never a downgrade, at start or with "Update ampr-emu-ng now": the release `tag` is installed only when it
/// is not older than the ampr-emu-ng maxpack.elf carries (`bundled`, its VERSION) nor than the one in use
/// (`in_use`, its known version: `known_version`; None when unknown, which only the bundled one bounds).
/// An older one would stop every .p5z that grew (crate::p5z::GROWN_NG), and a restart puts the bundled
/// one back anyway. A tag that is not a version is refused too. `crate::version` compares X.Y.Z.
pub fn not_older(tag: &str, bundled: &str, in_use: Option<&str>) -> Result<(), Fail> {
    use crate::version::newer;
    let than = if newer(Some(bundled), Some(tag)) {
        format!("the ampr-emu-ng MaxPack carries, {bundled}")
    } else if newer(in_use, Some(tag)) {
        format!("the one in use, {}", in_use.unwrap_or_default())
    } else {
        return Ok(());
    };
    Err(Fail::Older(format!("the latest release {tag} is older than {than} (bundled {bundled})")))
}

/// Why the ampr-emu-ng update installed nothing: the HTTP status and code `POST /api/ampr/update`
/// answers (`{code, detail}`), and the detail for the log.
#[derive(Debug, PartialEq, Eq, Clone)]
pub enum Fail {
    /// The secure-connection self-test has not passed on this console: as the self-update's
    /// (`update::Fail::TlsUnchecked`), the same 409, code and page text.
    TlsUnchecked,
    /// Another ampr-emu-ng update runs (the start-up one or another press).
    UpdateRunning,
    /// The look at LATEST_URL or a download failed.
    Download(String),
    /// The answer is not a release that can be installed (no libSceAmpr.sprx, an asset elsewhere).
    Release(String),
    /// The download is not the release's libSceAmpr.sprx: SHA256SUMS missing, malformed or listing
    /// another SHA-256, or another size or no ELF magic. Not installed; logged and toasted.
    Checksum(String),
    /// Reading what is installed, or writing the new file, failed.
    Install(String),
    /// The latest release is older than the bundled ampr-emu-ng or the one in use (`not_older`).
    Older(String),
}

impl Fail {
    pub fn status(&self) -> u16 {
        match self {
            Fail::TlsUnchecked => update::Fail::TlsUnchecked.status(),
            Fail::UpdateRunning | Fail::Older(_) => 409,
            Fail::Download(_) | Fail::Release(_) | Fail::Checksum(_) => 502,
            Fail::Install(_) => 500,
        }
    }

    /// The page shows its own text for `tls_unchecked` (the self-update's), `update_running` and
    /// `download` (the self-update's too), `checksum` and `older` (ui.html `amprFail_checksum`,
    /// `amprFail_older`); the detail for the others.
    pub fn code(&self) -> &'static str {
        match self {
            Fail::TlsUnchecked => update::Fail::TlsUnchecked.code(),
            Fail::UpdateRunning => "update_running",
            Fail::Download(_) => "download",
            Fail::Release(_) => "release",
            Fail::Checksum(_) => "checksum",
            Fail::Install(_) => "install",
            Fail::Older(_) => "older",
        }
    }

    /// English, for the log and the page.
    pub fn detail(&self) -> String {
        match self {
            Fail::TlsUnchecked => update::Fail::TlsUnchecked.detail(),
            Fail::UpdateRunning => "an ampr-emu-ng update is already running".into(),
            Fail::Download(e) => format!("download failed: {e}"),
            Fail::Release(e) => format!("not a release to install: {e}"),
            Fail::Checksum(e) => format!("not the released {SPRX_ASSET}, not installed: {e}"),
            Fail::Install(e) => format!("install failed: {e}"),
            Fail::Older(e) => format!("not installed: {e}; a .p5z that grew would no longer start with it"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sha256;
    use serde_json::json;

    /// A stand-in for libSceAmpr.sprx (a SELF), and for the debug build the release also carries.
    const SPRX: &[u8] = b"\x4f\x15\x3d\x1d a tiny stand-in for libSceAmpr.sprx";
    const DEBUG: &[u8] = b"\x4f\x15\x3d\x1d a tiny stand-in for libSceAmpr-debug.sprx";
    const TAG: &str = "v0.5.1-maxpack.2";

    fn asset(tag: &str, name: &str, size: usize) -> serde_json::Value {
        json!({"name": name, "size": size,
            "browser_download_url": format!("https://github.com/DeLFuS77/ampr-emu-ng/releases/download/{tag}/{name}")})
    }

    fn answer(tag: &str, assets: Vec<serde_json::Value>) -> serde_json::Value {
        json!({"tag_name": tag, "draft": false, "prerelease": false, "assets": assets})
    }

    /// As DeLFuS77/ampr-emu-ng's releases are: the debug build first, then the sprx and SHA256SUMS.
    fn full(tag: &str) -> Vec<u8> {
        answer(tag, vec![asset(tag, "libSceAmpr-debug.sprx", 847_584), asset(tag, "libSceAmpr.sprx", 716_176), asset(tag, "SHA256SUMS", 170)])
            .to_string().into_bytes()
    }

    fn line(data: &[u8], name: &str) -> String {
        format!("{}  {name}\n", sha256::hex(&sha256::digest(data)))
    }

    /// The release's SHA256SUMS: both builds, as `sha256sum` writes it.
    fn sums() -> Vec<u8> {
        format!("{}{}", line(SPRX, "libSceAmpr.sprx"), line(DEBUG, "libSceAmpr-debug.sprx")).into_bytes()
    }

    #[test]
    fn the_release_with_its_sprx_and_sha256sums() {
        let dl = format!("https://github.com/DeLFuS77/ampr-emu-ng/releases/download/{TAG}/");
        assert_eq!(release(&full(TAG)), Ok(Release {
            tag: TAG.into(),
            sprx: Asset { url: format!("{dl}libSceAmpr.sprx"), size: 716_176 },
            sums: Ok(Asset { url: format!("{dl}SHA256SUMS"), size: 170 }),
        }));
    }

    #[test]
    fn a_release_without_a_usable_sha256sums_is_read_but_cannot_be_installed() {
        let sums_err = |assets: Vec<serde_json::Value>| {
            let r = release(answer(TAG, assets).to_string().as_bytes()).expect("the release is read");
            assert_eq!(r.tag, TAG);
            r.sums.expect_err("no usable SHA256SUMS")
        };
        assert!(sums_err(vec![asset(TAG, "libSceAmpr.sprx", 100)]).contains("SHA256SUMS"));
        assert!(!sums_err(vec![asset(TAG, "libSceAmpr.sprx", 100), asset(TAG, "SHA256SUMS", 0)]).is_empty());
        assert!(!sums_err(vec![asset(TAG, "libSceAmpr.sprx", 100), asset(TAG, "SHA256SUMS", SUMS_MAX + 1)]).is_empty());
        let mut moved = asset(TAG, "SHA256SUMS", 170);
        moved["browser_download_url"] = json!("https://example.com/SHA256SUMS");
        assert!(!sums_err(vec![asset(TAG, "libSceAmpr.sprx", 100), moved]).is_empty());
        assert!(!sums_err(vec![asset(TAG, "libSceAmpr.sprx", 100), asset(TAG, "SHA256SUMS", 170), asset(TAG, "SHA256SUMS", 170)]).is_empty());
    }

    #[test]
    fn a_release_that_is_not_one_to_install() {
        let bad = |body: Vec<u8>| release(&body).expect_err("refused");
        // No libSceAmpr.sprx (the debug build alone does not do), or two.
        assert!(bad(answer(TAG, vec![asset(TAG, "libSceAmpr-debug.sprx", 100), asset(TAG, "SHA256SUMS", 170)]).to_string().into_bytes()).contains("libSceAmpr.sprx"));
        assert!(!bad(answer(TAG, vec![asset(TAG, "libSceAmpr.sprx", 100), asset(TAG, "libSceAmpr.sprx", 100)]).to_string().into_bytes()).is_empty());
        // Elsewhere: another repository (quer3q's build, say), or another tag's download.
        let mut moved = asset(TAG, "libSceAmpr.sprx", 100);
        moved["browser_download_url"] = json!(format!("https://github.com/someone/ampr-emu-ng/releases/download/{TAG}/libSceAmpr.sprx"));
        assert!(!bad(answer(TAG, vec![moved, asset(TAG, "SHA256SUMS", 170)]).to_string().into_bytes()).is_empty());
        assert!(!bad(answer(TAG, vec![asset("v0.5.1-maxpack.1", "libSceAmpr.sprx", 100), asset(TAG, "SHA256SUMS", 170)]).to_string().into_bytes()).is_empty());
        // Empty, or over the 16 MiB cap.
        assert!(!bad(answer(TAG, vec![asset(TAG, "libSceAmpr.sprx", 0), asset(TAG, "SHA256SUMS", 170)]).to_string().into_bytes()).is_empty());
        assert!(!bad(answer(TAG, vec![asset(TAG, "libSceAmpr.sprx", SPRX_MAX + 1), asset(TAG, "SHA256SUMS", 170)]).to_string().into_bytes()).is_empty());
        // A draft or a pre-release (a beta), or not the answer expected.
        let mut pre = answer(TAG, vec![asset(TAG, "libSceAmpr.sprx", 100), asset(TAG, "SHA256SUMS", 170)]);
        pre["prerelease"] = json!(true);
        assert!(!bad(pre.to_string().into_bytes()).is_empty());
        pre["prerelease"] = json!(false);
        pre["draft"] = json!(true);
        assert!(!bad(pre.to_string().into_bytes()).is_empty());
        assert!(!bad(b"not json".to_vec()).is_empty());
        assert!(!bad(br#"{"message":"Not Found"}"#.to_vec()).is_empty());
    }

    #[test]
    fn a_good_sum_is_installed() {
        assert_eq!(verify(SPRX, SPRX.len(), &sums()), Ok(()));
        // An ELF too; the other line first; CRLF; the binary-mode marker; upper-case hex.
        let elf = b"\x7fELF\x02\x01\x01\x09 an ELF stand-in";
        let text = format!("{}\r\n{} *libSceAmpr.sprx\r\n", line(DEBUG, "libSceAmpr-debug.sprx").trim_end(),
            sha256::hex(&sha256::digest(elf)).to_uppercase());
        assert_eq!(verify(elf, elf.len(), text.as_bytes()), Ok(()));
    }

    #[test]
    fn a_wrong_sum_is_refused() {
        // One byte changed: same size and magic, another SHA-256.
        let mut changed = SPRX.to_vec();
        *changed.last_mut().unwrap() ^= 1;
        assert!(verify(&changed, changed.len(), &sums()).unwrap_err().contains("SHA-256"));
        // The debug build's sum on the sprx's line: the debug build served under the sprx's name.
        let swapped = line(DEBUG, "libSceAmpr.sprx");
        assert!(verify(SPRX, SPRX.len(), swapped.as_bytes()).unwrap_err().contains("SHA-256"));
        // The right sum, but the download is not what the release says (cut short, an HTML page).
        assert!(verify(&SPRX[..SPRX.len() - 1], SPRX.len(), &sums()).is_err());
        let html = vec![b'<'; SPRX.len()];
        assert!(verify(&html, html.len(), line(&html, "libSceAmpr.sprx").as_bytes()).is_err());
    }

    #[test]
    fn a_missing_line_is_refused() {
        // Only the debug build, a name that only starts or ends the same, another file, nothing.
        for text in [
            line(SPRX, "libSceAmpr-debug.sprx"),
            format!("{}{}", line(SPRX, "libSceAmpr.sprx.sig"), line(SPRX, "old-libSceAmpr.sprx")),
            line(SPRX, "maxpack.elf"),
            line(SPRX, "ampr/libSceAmpr.sprx"),
            String::new(),
        ] {
            let e = verify(SPRX, SPRX.len(), text.as_bytes()).unwrap_err();
            assert!(e.contains("libSceAmpr.sprx"), "{text:?}: {e}");
        }
    }

    #[test]
    fn a_malformed_sha256sums_is_refused() {
        let h = sha256::hex(&sha256::digest(SPRX));
        for text in [
            // Listed twice (even with the same sum), a short or a long hash, a non-hex digit, one space.
            format!("{h}  libSceAmpr.sprx\n{h}  libSceAmpr.sprx\n"),
            format!("{}  libSceAmpr.sprx\n", &h[..62]),
            format!("{h}00  libSceAmpr.sprx\n"),
            format!("{}zz  libSceAmpr.sprx\n", &h[..62]),
            format!("{h} libSceAmpr.sprx\n"),
            // An HTML error page instead of the file.
            "<!DOCTYPE html><html><body>404</body></html>".to_string(),
        ] {
            assert!(verify(SPRX, SPRX.len(), text.as_bytes()).is_err(), "{text:?}");
        }
        // Not text.
        assert!(verify(SPRX, SPRX.len(), b"\xff\xfe\x00").is_err());
    }

    #[test]
    fn the_gate_is_the_self_updates() {
        assert_eq!(gate(true), Ok(()));
        // Closed: refused with the self-update's 409 and code, so the page shows the same text.
        let f = gate(false).unwrap_err();
        assert_eq!(f, Fail::TlsUnchecked);
        let own = update::Fail::TlsUnchecked;
        assert_eq!((f.status(), f.code(), f.detail()), (own.status(), own.code(), own.detail()));
        assert_eq!(update::tls_gate(false), Err(update::Fail::TlsUnchecked));
        assert_eq!(update::tls_gate(true), Ok(()));
    }

    /// What MaxPack knows of a libSceAmpr.sprx (the ampr dir's, or the copy in a game's fakelib/): the
    /// bundled version for the bundled bytes, the version ampr.bundled records for exactly those bytes
    /// (len and crc32), and nothing for anything else.
    #[test]
    fn the_version_maxpack_knows_of_a_file() {
        const B: &str = "v0.5.1-maxpack.2";
        let mark = "v0.5.1-maxpack.1 716176 0a1b2c3d";
        // The bundled bytes: the bundled version, whatever the mark says.
        for m in [None, Some(mark), Some("garbage")] {
            assert_eq!(known_version(Some((720_048, 7, true)), B, m), Some(B), "{m:?}");
        }
        // A copy we wrote: the version its mark records, for exactly that len and crc32.
        assert_eq!(known_version(Some((716_176, 0x0a1b_2c3d, false)), B, Some(mark)), Some("v0.5.1-maxpack.1"));
        assert_eq!(known_version(Some((716_176, 0x0a1b_2c3d, false)), B, Some("v0.5.1-maxpack.1 716176 0A1B2C3D\n")), Some("v0.5.1-maxpack.1"));
        // Unknown: another len or crc32 (a file put there by hand, one written after the mark), a v0.4.0
        // mark (no version), no mark, a mark that does not parse, no file.
        for (file, m) in [
            (Some((716_177, 0x0a1b_2c3d, false)), Some(mark)),
            (Some((716_176, 0x0a1b_2c3e, false)), Some(mark)),
            (Some((716_176, 0x0a1b_2c3d, false)), Some("716176 0a1b2c3d")),
            (Some((716_176, 0x0a1b_2c3d, false)), None),
            (Some((716_176, 0x0a1b_2c3d, false)), Some("")),
            (Some((716_176, 0x0a1b_2c3d, false)), Some("v1 v2 716176 0a1b2c3d")),
            (Some((716_176, 0x0a1b_2c3d, false)), Some("v0.5.1-maxpack.1 big 0a1b2c3d")),
            (None, Some(mark)),
        ] {
            assert_eq!(known_version(file, B, m), None, "{file:?} {m:?}");
        }
        // The mark itself: (len, crc32) recorded, its version if any.
        assert_eq!(mark_version(mark, 716_176, 0x0a1b_2c3d), Some(Some("v0.5.1-maxpack.1")));
        assert_eq!(mark_version("716176 0a1b2c3d", 716_176, 0x0a1b_2c3d), Some(None));
        assert_eq!(mark_version(mark, 716_176, 0), None);
    }

    /// Never a downgrade: a release older than the ampr-emu-ng maxpack.elf carries, or than the one in use,
    /// is not installed (a .p5z that grew would stop with it), nor one whose tag is not a version.
    #[test]
    fn an_older_release_is_never_installed() {
        let b = "v0.5.1-maxpack.2";
        // Older than the bundled one (an earlier build of the same upstream version, upstream's own), or
        // than the one in use, or not a version.
        for (tag, in_use) in [("v0.5.1-maxpack.1", None), ("v0.5.1-maxpack.1", Some(b)), ("v0.5.1", None),
                              ("v0.5.1-maxpack.2", Some("v0.6.0-maxpack.1")), ("v0.5.1-maxpack.3", Some("v0.5.1-maxpack.4")),
                              ("latest", None), ("v0.5.2-beta", Some(b))] {
            let f = not_older(tag, b, in_use).expect_err("refused");
            assert_eq!((f.status(), f.code()), (409, "older"), "{tag} {in_use:?}");
            assert!(f.detail().contains(tag) && f.detail().contains(b), "{}", f.detail());
        }
        for (tag, in_use) in [("v0.5.1-maxpack.2", None), ("v0.5.1-maxpack.2", Some(b)), ("v0.6.0-maxpack.1", Some(b)),
                              ("v0.5.1-maxpack.3", Some(b)), ("v0.5.1-maxpack.9", Some("v0.5.1-maxpack.1")),
                              ("v0.6.0-maxpack.1", Some("not a version"))] {
            assert_eq!(not_older(tag, b, in_use), Ok(()), "{tag} {in_use:?}");
        }
    }

    #[test]
    fn fail_codes() {
        let all = [
            (Fail::TlsUnchecked, 409, "tls_unchecked"),
            (Fail::UpdateRunning, 409, "update_running"),
            (Fail::Download("HTTP status 503".into()), 502, "download"),
            (Fail::Release("no libSceAmpr.sprx".into()), 502, "release"),
            (Fail::Checksum("SHA256SUMS lists no libSceAmpr.sprx".into()), 502, "checksum"),
            (Fail::Install("No space left on device".into()), 500, "install"),
            (Fail::Older("v0.5.1-maxpack.1 is older than v0.5.1-maxpack.2".into()), 409, "older"),
        ];
        for (f, status, code) in all {
            assert_eq!((f.status(), f.code()), (status, code), "{f:?}");
            assert!(!f.detail().is_empty(), "{f:?}");
        }
        assert!(Fail::Checksum("SHA256SUMS lists no libSceAmpr.sprx".into()).detail().contains("SHA256SUMS lists no libSceAmpr.sprx"));
        assert!(Fail::Download("HTTP status 503".into()).detail().contains("HTTP status 503"));
    }
}
