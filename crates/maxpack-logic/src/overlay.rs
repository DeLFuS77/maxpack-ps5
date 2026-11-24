//! The per-title `overlay_off` setting of config.toml: a game whose cache gets an empty `fakelib/`,
//! so its launch mounts nothing in its sandbox (no overlay, no `/data`, USB, fonts or writable `/app0`)
//! and it runs as registered. Only a plain folder game can run so: the others are read only through
//! ampr-emu-ng, which comes in that overlay.
//!
//! Sources are named as `/api/state` names them: `kind` "pkg" | "folder" | "p5z", `packing` "plain" |
//! "zstd" | "lz4" for a folder, else None.

/// Why the overlay of this source cannot be turned off; None when it can (a plain folder).
pub fn needed(kind: &str, packing: Option<&str>) -> Option<&'static str> {
    match (kind, packing) {
        ("folder", Some("plain")) => None,
        ("pkg", _) => Some("a pkg game is read through ampr-emu-ng's pkg mode, which comes in MaxPack's overlay"),
        ("p5z", _) => Some("a .p5z game is played only through our ampr-emu-ng's p5z mode, which comes in MaxPack's overlay"),
        ("folder", Some("zstd" | "lz4")) => Some("a packed folder game's packs are read only through ampr-emu-ng, which comes in MaxPack's overlay"),
        _ => Some("not a plain folder game"),
    }
}

/// Whether the title's cache gets an empty `fakelib/`: `listed` in `overlay_off` and its source can
/// do without the overlay. A listed title whose source needs it (its folder was packed since, or the
/// library copy is now a pkg or a .p5z) keeps its fakelib.
pub fn off(kind: &str, packing: Option<&str>, listed: bool) -> bool {
    listed && needed(kind, packing).is_none()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_a_plain_folder_can_do_without_the_overlay() {
        assert_eq!(needed("folder", Some("plain")), None);
        let pkg = needed("pkg", None).expect("a pkg needs it");
        assert!(pkg.contains("pkg mode"), "{pkg}");
        let p5z = needed("p5z", None).expect("a .p5z needs it");
        assert!(p5z.contains("p5z mode"), "{p5z}");
        for packing in ["zstd", "lz4"] {
            let why = needed("folder", Some(packing)).expect("a packed folder needs it");
            assert!(why.contains("packs"), "{why}");
        }
        // Anything unknown keeps its overlay.
        assert!(needed("folder", None).is_some());
        assert!(needed("", None).is_some());
        assert!(needed("exfat", None).is_some());
    }

    #[test]
    fn the_cache_gets_an_empty_fakelib_only_when_listed_and_possible() {
        assert!(off("folder", Some("plain"), true));
        assert!(!off("folder", Some("plain"), false));
        for (kind, packing) in [("pkg", None), ("p5z", None), ("folder", Some("zstd")), ("folder", Some("lz4"))] {
            assert!(!off(kind, packing, true), "{kind} {packing:?}");
            assert!(!off(kind, packing, false), "{kind} {packing:?}");
        }
    }
}
