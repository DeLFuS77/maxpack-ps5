//! The `exclude` setting of config.toml: absolute paths the scan never opens. A source at an entry or
//! under it is not listed, built or registered (a title still registered stays registered).

/// `path` as an `exclude` entry, as config.toml keeps it: absolute, no control character (a newline
/// or a tab does not survive the file's string syntax), `//`, `.` and `..` resolved as text, no
/// trailing `/` (`/` itself stays `/`). None when it cannot be one.
pub fn entry(path: &str) -> Option<String> {
    let _ = path;
    None
}

/// `path` is an `exclude` entry or lies under one, by whole components (`/data/homebrew/A` does not
/// cover `/data/homebrew/AB`); both are resolved as text first, so a hand-written entry with a
/// trailing `/` or a `.` still matches.
pub fn excluded(path: &str, exclude: &[String]) -> bool {
    let _ = (path, exclude);
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn entries_are_kept_as_config_toml_writes_them() {
        assert_eq!(entry("/data/homebrew/Game").as_deref(), Some("/data/homebrew/Game"));
        assert_eq!(entry("/data/homebrew/Game/").as_deref(), Some("/data/homebrew/Game"));
        assert_eq!(entry("/data/homebrew/Game///").as_deref(), Some("/data/homebrew/Game"));
        assert_eq!(entry("//data//homebrew/./Game").as_deref(), Some("/data/homebrew/Game"));
        assert_eq!(entry("/data/homebrew/x/../Game").as_deref(), Some("/data/homebrew/Game"));
        assert_eq!(entry("/").as_deref(), Some("/"));
        assert_eq!(entry("///").as_deref(), Some("/"));
        // Names keep their bytes: spaces, accents, brackets.
        assert_eq!(entry("/mnt/usb0/homebrew/Juego ñ [PPSA01234].p5z").as_deref(),
            Some("/mnt/usb0/homebrew/Juego ñ [PPSA01234].p5z"));
    }

    #[test]
    fn entries_that_cannot_be_one() {
        for bad in ["", "data/homebrew", "homebrew/", " /data/homebrew", "/data/home\nbrew", "/data/\thomebrew",
            "/data/homebrew\r", "/data/\u{7f}x", "/data/\u{0}x"] {
            assert_eq!(entry(bad), None, "{bad:?}");
        }
    }

    #[test]
    fn a_source_at_an_entry_or_under_it_is_excluded() {
        let ex = vec!["/data/homebrew/Game".to_string(), "/mnt/usb0/old".to_string()];
        assert!(excluded("/data/homebrew/Game", &ex));
        assert!(excluded("/data/homebrew/Game/", &ex));
        assert!(excluded("/data/homebrew/Game/sub/eboot.bin", &ex));
        assert!(excluded("/mnt/usb0/old/A.pkg", &ex));
        // Whole components only.
        assert!(!excluded("/data/homebrew/GameB", &ex));
        assert!(!excluded("/data/homebrew/Gam", &ex));
        assert!(!excluded("/data/homebrew", &ex));
        assert!(!excluded("/mnt/usb0/older/A.pkg", &ex));
        assert!(!excluded("/mnt/usb1/old/A.pkg", &ex));
        // Resolved as text on both sides.
        assert!(excluded("/data//homebrew/./Game", &ex));
        assert!(excluded("/data/homebrew/x/../Game/eboot.bin", &ex));
        let hand = vec!["/data/homebrew/Game/".to_string(), "/mnt/usb0/./x".to_string()];
        assert!(excluded("/data/homebrew/Game", &hand));
        assert!(excluded("/mnt/usb0/x/y", &hand));
        // Nothing excluded by an empty list; `/` excludes everything.
        assert!(!excluded("/data/homebrew/Game", &[]));
        assert!(excluded("/data/homebrew/Game", &["/".to_string()]));
    }

    #[test]
    fn a_relative_entry_written_by_hand_excludes_nothing() {
        // config.rs refuses it when reading; if one got through, it must not match a source.
        assert!(!excluded("/data/homebrew/Game", &["data/homebrew/Game".to_string()]));
        assert!(!excluded("/data/homebrew/Game", &["".to_string()]));
    }
}
