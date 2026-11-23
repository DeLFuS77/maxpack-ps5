/// ShadowMountPlus image kind by file extension (case-insensitive). MaxPack does not mount these yet (stage 3).
pub fn smp_image_kind(file_name: &str) -> Option<&'static str> {
    let (stem, ext) = file_name.rsplit_once('.')?;
    if stem.is_empty() {
        return None;
    }
    match ext.to_ascii_lowercase().as_str() {
        "ffpkg" => Some("ffpkg"),
        "exfat" => Some("exfat"),
        "ffpfs" => Some("ffpfs"),
        "ffpfsc" => Some("ffpfsc"),
        _ => None,
    }
}

/// The title id written between brackets in an image's file name (`Juego [PPSA01234].ffpkg`): the
/// first `[...]` that holds a game title id. None when there is none.
pub fn bracket_title_id(file_name: &str) -> Option<&str> {
    file_name.split('[').skip(1).filter_map(|r| r.split_once(']').map(|(id, _)| id)).find(|id| is_title_id(id))
}

/// The image file `file_name` names game title `tid` in any of its `[...]`: ShadowMountPlus may
/// serve that title from it, so auto-remove leaves the title alone.
pub fn image_has_title(file_name: &str, tid: &str) -> bool {
    is_title_id(tid) && file_name.split('[').skip(1).filter_map(|r| r.split_once(']')).any(|(id, _)| id == tid)
}

/// A game title id: 4 capitals and 5 digits, not a system app (NPXS); the payload's scan::valid_tid.
fn is_title_id(t: &str) -> bool {
    let b = t.as_bytes();
    b.len() == 9 && b[..4].iter().all(u8::is_ascii_uppercase) && b[4..].iter().all(u8::is_ascii_digit)
        && !t.starts_with("NPXS")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn image_carries_its_bracketed_title() {
        assert!(image_has_title("Juego [PPSA01234].exfat", "PPSA01234"));
        // Any bracket counts, not only the first title id.
        assert!(image_has_title("[PPSA00001] Juego [PPSA01234].ffpkg", "PPSA01234"));
        assert!(!image_has_title("Juego [PPSA01234].exfat", "PPSA01235"));
        assert!(!image_has_title("Juego.exfat", "PPSA01234"));
        assert!(!image_has_title("Juego [ppsa01234].exfat", "PPSA01234"));
        assert!(!image_has_title("Juego [].exfat", ""));
    }

    #[test]
    fn bracket_title_id_from_name() {
        assert_eq!(bracket_title_id("Astro Bot [PPSA01234].ffpkg"), Some("PPSA01234"));
        assert_eq!(bracket_title_id("[CUSA00001][v1.00] x.exfat"), Some("CUSA00001"));
        assert_eq!(bracket_title_id("x [v1.00] [PPSA01234].ffpfs"), Some("PPSA01234"));
        for name in [
            "PPSA01234.exfat", "[ppsa01234].exfat", "[NPXS20001].exfat", "[PPSA0123].exfat",
            "[PPSA012345].exfat", "[PPSA01234.exfat", "x.ffpfsc", "[ PPSA01234 ].exfat",
        ] {
            assert_eq!(bracket_title_id(name), None, "{name}");
        }
    }

    #[test]
    fn smp_image_kind_by_extension() {
        assert_eq!(smp_image_kind("Juego.FFPKG"), Some("ffpkg"));
        assert_eq!(smp_image_kind("x.exfat"), Some("exfat"));
        assert_eq!(smp_image_kind("x.ffpfs"), Some("ffpfs"));
        assert_eq!(smp_image_kind("x.ffpfsc"), Some("ffpfsc"));
        assert_eq!(smp_image_kind("a.b.exfat"), Some("exfat"));
        assert_eq!(smp_image_kind("x.pkg"), None);
        assert_eq!(smp_image_kind("x.p5z"), None);
        assert_eq!(smp_image_kind("ffpkg"), None);
        assert_eq!(smp_image_kind(".ffpkg.txt"), None);
    }
}
