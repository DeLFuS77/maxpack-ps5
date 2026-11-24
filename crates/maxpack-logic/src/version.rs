//! Versions of the sprx we ship and of the one a game carries.

/// `vX.Y.Z` (leading v optional), optionally followed by MaxPack's own build suffix `-maxpack.N`
/// (our build of upstream X.Y.Z, same sprx identity), which compares equal to X.Y.Z; None = unknown.
pub fn parse_version(v: &str) -> Option<(u32, u32, u32)> {
    let v = v.trim();
    let v = match v.rsplit_once("-maxpack.") {
        Some((base, n)) if !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()) => base,
        _ => v,
    };
    let mut it = v.strip_prefix('v').unwrap_or(v).split('.').map(|p| p.parse::<u32>().ok());
    let r = (it.next()??, it.next()??, it.next()??);
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
        assert_eq!(parse_version("v0.5.0"), Some((0, 5, 0)));
        assert_eq!(parse_version("0.5.0-maxpack.2"), Some((0, 5, 0)));
        assert_eq!(parse_version("0.5"), None);
        assert!(newer(Some("0.6.0"), Some("0.5.0")));
        assert!(!newer(None, Some("0.5.0")));
        assert!(newer(Some("0.5.0"), None));
    }
}
