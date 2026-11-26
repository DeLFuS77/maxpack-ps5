//! Versions of the sprx we ship and of the one a game carries: ampr-emu-ng's (the bundled VERSION, a
//! release's tag, ampr.bundled's record, the `ampr-emu-ng X.Y.Z` identity in a file). MaxPack's own
//! self-update compares its versions apart (crate::update).

/// `vX.Y.Z` (leading v optional), optionally followed by MaxPack's own build suffix `-maxpack.N` (our
/// build N of upstream X.Y.Z; its sprx identity says X.Y.Z only): (X, Y, Z, N), N = 0 without the suffix
/// (upstream's own build). Ordered by X.Y.Z, then N: v0.5.1-maxpack.2 > v0.5.1-maxpack.1 > v0.5.1, and
/// v0.5.2 > v0.5.1-maxpack.N. None = unknown (anything else: another suffix, a missing or non-numeric N).
pub fn parse_version(v: &str) -> Option<(u32, u32, u32, u32)> {
    let v = v.trim();
    let (v, rev) = match v.rsplit_once("-maxpack.") {
        Some((base, n)) if !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()) => (base, n.parse::<u32>().ok()?),
        _ => (v, 0),
    };
    let mut it = v.strip_prefix('v').unwrap_or(v).split('.').map(|p| p.parse::<u32>().ok());
    let r = (it.next()??, it.next()??, it.next()??, rev);
    it.next().is_none().then_some(r)
}

/// `a` newer than `b`; an unknown version is older than any.
pub fn newer(a: Option<&str>, b: Option<&str>) -> bool {
    a.and_then(parse_version) > b.and_then(parse_version)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn version_table() {
        assert_eq!(parse_version("v0.5.0"), Some((0, 5, 0, 0)));
        assert_eq!(parse_version("0.5.0-maxpack.2"), Some((0, 5, 0, 2)));
        assert_eq!(parse_version(" v0.5.1-maxpack.10 "), Some((0, 5, 1, 10)));
        assert_eq!(parse_version("0.5"), None);
        assert!(newer(Some("0.6.0"), Some("0.5.0")));
        assert!(!newer(None, Some("0.5.0")));
        assert!(newer(Some("0.5.0"), None));
    }

    /// Our builds of one upstream version are told apart by their MaxPack revision: v0.5.1-maxpack.2 is
    /// newer than v0.5.1-maxpack.1, which is newer than upstream's own v0.5.1 (revision 0, no p5z mode);
    /// a newer upstream version is newer than any build of an older one.
    #[test]
    fn the_maxpack_revision_orders_builds_of_one_upstream_version() {
        assert!(newer(Some("v0.5.1-maxpack.2"), Some("v0.5.1-maxpack.1")));
        assert!(newer(Some("v0.5.1-maxpack.1"), Some("v0.5.1")));
        assert!(newer(Some("v0.5.1-maxpack.10"), Some("v0.5.1-maxpack.9")));
        assert!(newer(Some("v0.5.2"), Some("v0.5.1-maxpack.99")));
        assert!(newer(Some("v0.5.2-maxpack.1"), Some("v0.5.2")));
        for (a, b) in [("v0.5.1-maxpack.1", "v0.5.1-maxpack.1"), ("v0.5.1-maxpack.1", "v0.5.1-maxpack.2"), ("v0.5.1", "v0.5.1-maxpack.1"),
                       ("v0.5.1-maxpack.99", "v0.5.2"), ("0.5.1-maxpack.2", "v0.5.1-maxpack.2")] {
            assert!(!newer(Some(a), Some(b)), "{a} > {b}");
        }
        // Anything else is unknown, older than any version.
        for v in ["v0.5.1-maxpack.", "v0.5.1-maxpack.x", "v0.5.1-maxpack.-1", "v0.5.1-beta", "v0.5.1-maxpack.1.2", "v0.5.1-maxpack.1-maxpack.2",
                  "maxpack.1", "-maxpack.1", "", "V0.5.1"] {
            assert_eq!(parse_version(v), None, "{v:?}");
            assert!(newer(Some("v0.0.0"), Some(v)), "{v:?}");
        }
    }
}
