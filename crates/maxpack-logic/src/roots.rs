//! Scan roots to use the first time: the usual ones plus those added by hand in Kraken Mount and
//! ShadowMountPlus.
//!
//! Formats followed (read, not run):
//! - Kraken Mount `config.toml`: `scanpath = ["/a", "/b"]` or `scanpath = "/a"`, `#` comments,
//!   multi-line arrays, `'literal'` and `"escaped"` strings (`src/config.rs`: `parse`, `toml_value`).
//! - ShadowMountPlus `config.ini` (`src/sm_config_file.c` `sm_ini_read`, `src/sm_config_mount.c`
//!   `add_runtime_scan_path`): `[scan]` / `path=` (section and key case-insensitive, one `path=` line per
//!   root), lines starting with `#` or `;` are comments, an unquoted value ends at `#` or `;`, a
//!   `"quoted"` value allows `\\` and `\"`; the old flat format uses `scanpath=` with no section.
//! - ShadowMountPlus `manual.lst` (`src/sm_manual.c` `sm_manual_for_each_path`): one path per line,
//!   trimmed, trailing `/` removed, `#` comments.

/// The other mounters' settings files, only ever read (for the first `config.toml`).
pub const KM_CONFIG_TOML: &str = "/data/krakenmount/config.toml";
pub const SMP_CONFIG_INI: &str = "/data/shadowmount/config.ini";
pub const SMP_MANUAL_LST: &str = "/data/shadowmount/manual.lst";

/// The other mounters' data folders (`/user/data` is the same folder as `/data`). MaxPack never
/// writes or deletes anything there, so no scan root and no source may be inside one.
const OTHER_MOUNTER_DATA: [&str; 4] =
    ["/data/krakenmount", "/data/shadowmount", "/user/data/krakenmount", "/user/data/shadowmount"];

/// `path` is one of the other mounters' data folders or inside one. `//`, `.` and `..` are resolved
/// as text first; symlinks are not followed.
pub fn in_other_mounter_data(path: &str) -> bool {
    let parts = |p: &str| {
        let mut out: Vec<String> = Vec::new();
        for c in p.split('/') {
            match c {
                "" | "." => {}
                ".." => {
                    out.pop();
                }
                c => out.push(c.to_string()),
            }
        }
        out
    };
    let p = parts(path);
    OTHER_MOUNTER_DATA.iter().any(|d| p.starts_with(&parts(d)))
}

/// `scanpath` of the first `config.toml`. None when the other mounters add no root to
/// `all_defaults`: then there is no `scanpath` line, so the usual roots keep following the drives
/// as they come and go. Otherwise every usual root and then theirs, because one `scanpath` line
/// replaces the usual roots. Roots inside the other mounters' data folders are left out.
pub fn first_scan_paths(
    all_defaults: &[String],
    km_config_toml: Option<&str>,
    smp_config_ini: Option<&str>,
    smp_manual_lst: Option<&str>,
) -> Option<Vec<String>> {
    let usual = import_roots(all_defaults, None, None, None);
    let theirs: Vec<String> = import_roots(&[], km_config_toml, smp_config_ini, smp_manual_lst)
        .into_iter()
        .filter(|r| !usual.contains(r) && !in_other_mounter_data(r))
        .collect();
    (!theirs.is_empty()).then(|| usual.into_iter().chain(theirs).collect())
}

/// Roots to use the first time: `defaults`, then Kraken Mount's, then ShadowMountPlus's `config.ini`
/// and `manual.lst`, in that order, without trailing `/`, repeats or anything that is not a clean
/// absolute path.
pub fn import_roots(
    defaults: &[String],
    km_config_toml: Option<&str>,
    smp_config_ini: Option<&str>,
    smp_manual_lst: Option<&str>,
) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let mut add = |raw: &str| {
        if let Some(p) = clean(raw) {
            if !out.contains(&p) {
                out.push(p);
            }
        }
    };
    for d in defaults {
        add(d);
    }
    if let Some(t) = km_config_toml {
        km_roots(t).iter().for_each(|p| add(p));
    }
    if let Some(t) = smp_config_ini {
        smp_ini_roots(t).iter().for_each(|p| add(p));
    }
    if let Some(t) = smp_manual_lst {
        // manual.lst lists GAME folders, and a root's entries are what gets explored as games (the root
        // itself is never one): the folder to scan is the parent of each entry.
        for line in unbom(t).lines() {
            let line = line.trim();
            if line.starts_with('#') {
                continue;
            }
            if let Some((parent, _)) = line.trim_end_matches('/').rsplit_once('/') {
                add(parent); // a game directly under `/` gives "", which `clean` drops
            }
        }
    }
    out
}

/// `t` without a leading UTF-8 byte order mark.
fn unbom(t: &str) -> &str {
    t.strip_prefix('\u{feff}').unwrap_or(t)
}

/// A clean absolute path without trailing `/`; `None` if relative, empty, `/`, with `..` or control bytes.
fn clean(raw: &str) -> Option<String> {
    let p = raw.trim();
    if !p.starts_with('/') || p.chars().any(|c| c.is_control()) {
        return None;
    }
    let p = p.trim_end_matches('/');
    if p.is_empty() || p.split('/').any(|c| c == "..") {
        return None;
    }
    Some(p.to_string())
}

/// One TOML string (`"..."` with `\\` and `\"`, or `'...'`) at the start of `s`, and the rest.
fn toml_str(s: &str) -> Option<(String, &str)> {
    let mut chars = s.char_indices();
    let (_, q) = chars.next()?;
    if q == '\'' {
        let end = s[1..].find('\'')?;
        return Some((s[1..1 + end].to_string(), &s[end + 2..]));
    }
    if q != '"' {
        return None;
    }
    let mut out = String::new();
    let mut esc = false;
    for (i, c) in chars {
        if esc {
            match c {
                '\\' | '"' => out.push(c),
                _ => return None, // other escapes are not roots we understand
            }
            esc = false;
        } else if c == '\\' {
            esc = true;
        } else if c == '"' {
            return Some((out, &s[i + 1..]));
        } else {
            out.push(c);
        }
    }
    None
}

/// `v` without a trailing `# comment` outside strings.
fn strip_toml_comment(v: &str) -> &str {
    let (mut quote, mut esc) = (None, false);
    for (i, c) in v.char_indices() {
        if esc {
            esc = false;
            continue;
        }
        match (quote, c) {
            (None, '#') => return &v[..i],
            (None, '"' | '\'') => quote = Some(c),
            (Some('"'), '\\') => esc = true,
            (Some(q), c) if c == q => quote = None,
            _ => {}
        }
    }
    v
}

fn km_roots(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut lines = unbom(text).lines();
    while let Some(line) = lines.next() {
        let line = line.trim();
        let Some((k, v)) = line.split_once('=') else { continue };
        if k.trim() != "scanpath" {
            continue;
        }
        out.clear(); // as in Kraken Mount, a later scanpath replaces an earlier one
        let mut v = strip_toml_comment(v.trim()).trim_end().to_string();
        if v.starts_with('[') {
            while !v.ends_with(']') {
                let Some(more) = lines.next() else { break };
                v = format!("{} {}", v, strip_toml_comment(more.trim()).trim_end());
            }
        }
        let Some(inner) = v.strip_prefix('[') else {
            // A single string; a bad one is dropped.
            if let Some((t, rest)) = toml_str(&v) {
                if rest.trim().is_empty() {
                    out.push(t);
                }
            }
            continue;
        };
        let Some(inner) = inner.strip_suffix(']') else { continue };
        let mut rest = inner.trim();
        while !rest.is_empty() {
            let Some((item, r)) = toml_str(rest) else { break };
            out.push(item);
            rest = r.trim_start().trim_start_matches(',').trim_start();
        }
    }
    out
}

fn smp_ini_roots(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut section = String::new();
    for line in unbom(text).lines() {
        let s = line.trim();
        if s.is_empty() || s.starts_with('#') || s.starts_with(';') {
            continue;
        }
        if let Some(rest) = s.strip_prefix('[') {
            if let Some(end) = rest.find(']') {
                section = rest[..end].trim().to_ascii_lowercase();
            }
            continue;
        }
        let Some((k, v)) = s.split_once('=') else { continue };
        let k = k.trim().to_ascii_lowercase();
        // The flat `scanpath` is only the old format, which has no sections.
        if !((section.is_empty() && k == "scanpath") || (section == "scan" && k == "path")) {
            continue;
        }
        let v = v.trim();
        if v.starts_with('"') {
            if let Some((t, rest)) = toml_str(v) {
                let rest = rest.trim();
                if rest.is_empty() || rest.starts_with('#') || rest.starts_with(';') {
                    out.push(t);
                }
            }
        } else {
            let end = v.find(['#', ';']).unwrap_or(v.len());
            out.push(v[..end].trim().to_string());
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(v: &[&str]) -> Vec<String> {
        v.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn import_keeps_defaults_and_custom() {
        let got = import_roots(
            &s(&["/data/homebrew"]),
            Some("scanpath = [\"/mnt/usb0/juegos/\"]"),
            Some("[scan]\npath=/data/mis"),
            Some("/mnt/ext0/x\n# no\n"),
        );
        assert_eq!(got, s(&["/data/homebrew", "/mnt/usb0/juegos", "/data/mis", "/mnt/ext0"]));
    }

    #[test]
    fn import_ignores_garbage() {
        let got = import_roots(
            &s(&["/data/homebrew", "/data/homebrew/"]),
            Some("scanpath = [\"rel/ativa\", \"\", \"/a/../b\", \"/ok\", \"/ok/\", \"/bin\u{1}ary\"]"),
            Some("[scan]\npath=\npath=../x\npath=/bin\u{1}\n"),
            Some("\n   \nrelativa\n/a/../b\n\u{0}\u{1}\n"),
        );
        assert_eq!(got, s(&["/data/homebrew", "/ok"]));
        assert_eq!(import_roots(&s(&["/d"]), None, None, None), s(&["/d"]));
    }

    #[test]
    fn km_formats() {
        let toml = "# c\nscan_interval_seconds = 5\nscanpath = [\n  \"/a\", # uno\n  '/b/',\n]\n";
        assert_eq!(import_roots(&[], Some(toml), None, None), s(&["/a", "/b"]));
        assert_eq!(import_roots(&[], Some("scanpath = \"/solo\""), None, None), s(&["/solo"]));
        assert_eq!(import_roots(&[], Some("scanpath = [\"/q\\\\x\"]"), None, None), s(&["/q\\x"]));
    }

    #[test]
    fn smp_ini_formats() {
        let ini = "; c\r\n[General]\r\npath=/no\r\n[ SCAN ]\r\nDepth=2\r\nPath = /a/ ; nota\r\nPATH=\"/b c\"\r\n# x\r\npath=/d#e\r\n[emulators]\r\npath=/emu\r\n";
        assert_eq!(import_roots(&[], None, Some(ini), None), s(&["/a", "/b c", "/d"]));
        // Old flat format: scanpath outside any [scan] section.
        assert_eq!(import_roots(&[], None, Some("scanpath=/old\nread_only=on\n"), None), s(&["/old"]));
    }

    #[test]
    fn manual_lst_crlf_and_comments() {
        let lst = "# h\r\n  /mnt/ext0/x/ \r\n/mnt/ext0/x\r\n\r\n#/z\r\n/y";
        // Entries are game folders: the root is their parent; a game directly under `/` has none.
        assert_eq!(import_roots(&[], None, None, Some(lst)), s(&["/mnt/ext0"]));
        let two = "/mnt/a/g1\n/mnt/b/g2\n/mnt/a/g3\n";
        assert_eq!(import_roots(&[], None, None, Some(two)), s(&["/mnt/a", "/mnt/b"]));
    }

    #[test]
    fn km_last_scanpath_wins() {
        let toml = "scanpath = [\"/viejo\"]\nscanpath = [\"/n1\", \"/n2\"]\n";
        assert_eq!(import_roots(&[], Some(toml), None, None), s(&["/n1", "/n2"]));
    }

    #[test]
    fn smp_flat_scanpath_only_without_section() {
        let ini = "[general]\nscanpath=/no\n[scan]\nscanpath=/tampoco\npath=/si\n";
        assert_eq!(import_roots(&[], None, Some(ini), None), s(&["/si"]));
    }

    #[test]
    fn first_start_without_custom_roots_writes_no_scanpath() {
        let d = s(&["/data/homebrew", "/mnt/usb0/homebrew"]);
        assert_eq!(first_scan_paths(&d, None, None, None), None);
        // Usual roots again, or garbage, import nothing either: the usual roots stay dynamic.
        let km = "scanpath = [\"/data/homebrew/\"]";
        assert_eq!(first_scan_paths(&d, Some(km), Some("[scan]\npath=relativa"), Some("/data/homebrew/JUEGO\n")), None);
    }

    #[test]
    fn first_start_lists_usual_roots_then_custom() {
        let d = s(&["/data/homebrew", "/mnt/usb0/homebrew"]);
        let got = first_scan_paths(&d, Some("scanpath = [\"/mnt/usb1/juegos\"]"), Some("[scan]\npath=/data/mis"), Some("/mnt/ext0/x/G1\n"));
        assert_eq!(got, Some(s(&["/data/homebrew", "/mnt/usb0/homebrew", "/mnt/usb1/juegos", "/data/mis", "/mnt/ext0/x"])));
    }

    #[test]
    fn other_mounter_data_is_never_a_root() {
        for p in [
            "/data/krakenmount", "/data/krakenmount/caches/PPSA01234", "/data/shadowmount/",
            "/user/data/shadowmount/x", "/user/data/krakenmount", "/data//krakenmount/./caches",
            "/data/homebrew/../krakenmount",
        ] {
            assert!(in_other_mounter_data(p), "{p}");
        }
        for p in ["/data/krakenmount2", "/data/homebrew", "/data", "/", "/mnt/usb0/krakenmount", "/data/krakenmount/..", "/mnt/usb0/../usb1"] {
            assert!(!in_other_mounter_data(p), "{p}");
        }
        let km = "scanpath = [\"/data/krakenmount/caches\", \"/ok\"]";
        let got = first_scan_paths(&s(&["/d"]), Some(km), Some("[scan]\npath=/data/shadowmount"), Some("/data/shadowmount/x/G\n"));
        assert_eq!(got, Some(s(&["/d", "/ok"])));
    }

    #[test]
    fn bom_is_ignored() {
        let bom = "\u{feff}";
        assert_eq!(import_roots(&[], Some(&format!("{bom}scanpath = [\"/k\"]")), None, None), s(&["/k"]));
        assert_eq!(import_roots(&[], None, Some(&format!("{bom}[scan]\npath=/i")), None), s(&["/i"]));
        assert_eq!(import_roots(&[], None, Some(&format!("{bom}scanpath=/f")), None), s(&["/f"]));
        assert_eq!(import_roots(&[], None, None, Some(&format!("{bom}/m/g"))), s(&["/m"]));
    }
}
