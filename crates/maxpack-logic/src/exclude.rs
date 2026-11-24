//! The `exclude` setting of config.toml: absolute paths the scan never opens. A source at an entry or
//! under it is not listed, built or registered (a title still registered stays registered).

/// `path` as an `exclude` entry, as config.toml keeps it: absolute, no control character (a newline
/// or a tab does not survive the file's string syntax), `//`, `.` and `..` resolved as text, no
/// trailing `/` (`/` itself stays `/`). None when it cannot be one.
pub fn entry(path: &str) -> Option<String> {
    if !path.starts_with('/') || path.chars().any(char::is_control) {
        return None;
    }
    Some(format!("/{}", parts(path).join("/")))
}

/// `path` is an `exclude` entry or lies under one, by whole components (`/data/homebrew/A` does not
/// cover `/data/homebrew/AB`); both are resolved as text first, so a hand-written entry with a
/// trailing `/` or a `.` still matches.
pub fn excluded(path: &str, exclude: &[String]) -> bool {
    let p = parts(path);
    exclude.iter().filter(|e| e.starts_with('/')).any(|e| p.starts_with(&parts(e)))
}

/// The `backports` folders to look in for a game found under scan root `owner`, in ShadowMountPlus's
/// order (resolve_backport_path_for_title): the game's own root, the other roots, then
/// `/data/homebrew` (its installed backports) even when `scanpath` replaces it; each root once
/// (compared as text with `//`, `.` and `..` resolved), and none that is `excluded`, so the scan never
/// opens an excluded one.
pub fn backport_dirs(owner: &str, roots: &[String], exclude: &[String]) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for root in std::iter::once(owner).chain(roots.iter().map(String::as_str)).chain(["/data/homebrew"]) {
        let p = parts(root);
        let dir = if p.is_empty() { "/backports".to_string() } else { format!("/{}/backports", p.join("/")) };
        if !out.contains(&dir) && !excluded(&dir, exclude) {
            out.push(dir);
        }
    }
    out
}

/// The components of `path` with `//`, `.` and `..` resolved as text (as `roots`).
fn parts(path: &str) -> Vec<&str> {
    let mut out = Vec::new();
    for c in path.split('/') {
        match c {
            "" | "." => {}
            ".." => {
                out.pop();
            }
            c => out.push(c),
        }
    }
    out
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
    fn backports_are_looked_for_in_the_owner_then_the_roots_then_data_homebrew() {
        let roots: Vec<String> = ["/mnt/usb0/homebrew", "/data/homebrew", "/mnt/ext1/homebrew/"].iter().map(|r| r.to_string()).collect();
        assert_eq!(backport_dirs("/mnt/ext1/homebrew", &roots, &[]),
            ["/mnt/ext1/homebrew/backports", "/mnt/usb0/homebrew/backports", "/data/homebrew/backports"]);
        // /data/homebrew always last, even when no root names it.
        assert_eq!(backport_dirs("/mnt/usb0", &["/mnt/usb0".to_string()], &[]),
            ["/mnt/usb0/backports", "/data/homebrew/backports"]);
        // Each root once, however it is written.
        assert_eq!(backport_dirs("/data/homebrew/", &["/data//homebrew".to_string(), "/data/./homebrew".to_string()], &[]),
            ["/data/homebrew/backports"]);
        // A drive's own root.
        assert_eq!(backport_dirs("/", &[], &[]), ["/backports", "/data/homebrew/backports"]);
    }

    #[test]
    fn an_excluded_backports_folder_is_never_opened() {
        let roots: Vec<String> = ["/mnt/usb0/homebrew", "/data/homebrew"].iter().map(|r| r.to_string()).collect();
        // The backports folder itself, or a folder above it.
        assert_eq!(backport_dirs("/mnt/usb0/homebrew", &roots, &["/data/homebrew/backports".to_string()]),
            ["/mnt/usb0/homebrew/backports"]);
        assert_eq!(backport_dirs("/mnt/usb0/homebrew", &roots, &["/mnt/usb0".to_string()]),
            ["/data/homebrew/backports"]);
        assert!(backport_dirs("/mnt/usb0/homebrew", &roots, &["/".to_string()]).is_empty());
        // One backport inside it excluded: the folder is still looked in (the caller skips that one).
        assert_eq!(backport_dirs("/data/homebrew", &roots, &["/data/homebrew/backports/PPSA01234".to_string()]),
            ["/data/homebrew/backports", "/mnt/usb0/homebrew/backports"]);
    }

    #[test]
    fn a_relative_entry_written_by_hand_excludes_nothing() {
        // config.rs refuses it when reading; if one got through, it must not match a source.
        assert!(!excluded("/data/homebrew/Game", &["data/homebrew/Game".to_string()]));
        assert!(!excluded("/data/homebrew/Game", &["".to_string()]));
    }
}
