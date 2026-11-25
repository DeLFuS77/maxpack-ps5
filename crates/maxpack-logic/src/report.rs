//! The per-game report (`GET /api/report/<TID>`, the page's "Copy report"): plain text to paste into
//! an issue, with a ready row for `docs/compatibility.md`. See DESIGN.md "Web API".
use crate::crash::Crash;
use crate::fakelib::Mode;

/// maxpack.log lines (those with the title id) at the end of a report.
pub const LOG_LINES: usize = 40;

/// What a report says about one library game.
pub struct Report<'a> {
    /// MaxPack's version (Cargo.toml).
    pub version: &'a str,
    /// The console's firmware ("13.60", `fw_text`); None when it could not be read.
    pub firmware: Option<&'a str>,
    /// /api/state `waiting`: None (MaxPack mounts), "kstuff", "other:<process>".
    pub waiting: Option<&'a str>,
    pub title_id: &'a str,
    pub title_name: &'a str,
    /// param.json contentVersion, or empty.
    pub game_version: &'a str,
    pub path: &'a str,
    /// /api/state `kind` and `packing`.
    pub kind: &'a str,
    pub packing: Option<&'a str>,
    pub drive: &'a str,
    pub status: &'a str,
    pub error: Option<&'a str>,
    /// Hidden in the page's library (`hidden`).
    pub hidden: bool,
    /// No MaxPack overlay at launch (`overlay_off`, a plain folder game): no fakelib, ampr-emu-ng or sandbox mounts.
    pub overlay_off: bool,
    /// The title's fakelib mode (`fakelib_emulators`, `fakelib_off`): what its cache's fakelib/ is made of.
    pub fakelib: Mode,
    /// The Fakelib mode is the recommended settings' own value (maxpack_logic::recommended::Effective).
    pub fakelib_recommended: bool,
    /// The PlayGo stub is the recommended settings' own value.
    pub playgo_recommended: bool,
    pub backport: Option<&'a str>,
    /// /api/ampr kind and version ("bundled v0.5.0-maxpack.3").
    pub ampr: &'a str,
    /// "on", "off", "not added (add_emulators off)" or "not added (fakelib mode off)".
    pub playgo: &'a str,
    pub crash: Option<&'a Crash>,
    /// The log tail the lines with the title id are taken from.
    pub log: &'a str,
}

/// The source kind as /api/state names it: (the report's words, the compatibility list's Format).
pub fn kind_text(kind: &str, packing: Option<&str>) -> (&'static str, &'static str) {
    match (kind, packing) {
        ("pkg", _) => ("pkg", "pkg"),
        ("p5z", _) => ("p5z", "p5z"),
        (_, Some("zstd")) => ("folder (zstd)", "zstd folder"),
        (_, Some("lz4")) => ("folder (lz4)", "LZ4 folder"),
        _ => ("folder", "plain folder"),
    }
}

/// A compatibility-row cell: empty is `?`, a `|` is escaped and a control character (a line break or a
/// tab) is a space, so the row stays one markdown line.
fn cell(s: &str) -> String {
    if s.is_empty() {
        return "?".into();
    }
    s.chars().map(|c| if c.is_control() { ' ' } else { c }).collect::<String>().replace('|', "\\|")
}

/// The compatibility list's Drive for the mount of a game's drive (`Source::drive`). `/mnt/ext1` is the
/// M.2 slot, `/mnt/ext0` USB extended storage; on USB the console does not say whether the drive is an
/// SSD or a hard drive, so the row asks. Any other mount is shown as it is.
pub fn drive_text(drive: &str) -> String {
    let usb = drive.strip_prefix("/mnt/usb").is_some_and(|n| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()));
    match drive {
        "/data" => "internal SSD".into(),
        "/mnt/ext1" => "M.2 SSD (console slot)".into(),
        "/mnt/ext0" => "USB SSD or HDD? (extended storage)".into(),
        _ if usb => "USB SSD or HDD?".into(),
        _ => cell(drive),
    }
}

/// The compatibility list's Fakelib mode, with the page's words; a game without the overlay gets none.
fn fakelib_text(r: &Report) -> &'static str {
    match r.fakelib {
        _ if r.overlay_off => "none (MaxPack libraries at launch off)",
        Mode::Full => "Full",
        Mode::Emulators => "Emulators only",
        Mode::Off => "Off",
    }
}

/// The firmware from the SDK's `kernel_get_fw_version()` (BCD: 0x13600000 is 13.60, the low 16 bits
/// are not part of it); None for 0 or a value that is not BCD.
pub fn fw_text(v: u32) -> Option<String> {
    let bcd = |b: u32| (b >> 4 <= 9 && b & 0xf <= 9).then_some((b >> 4) * 10 + (b & 0xf));
    let (major, minor) = (bcd((v >> 24) & 0xff)?, bcd((v >> 16) & 0xff)?);
    (major > 0).then(|| format!("{major}.{minor:02}"))
}

/// Unix seconds as `2026-11-19 20:51:03Z` (the log's stamp).
pub fn utc(secs: u64) -> String {
    // Howard Hinnant's days-to-civil.
    let (days, rem) = (secs / 86_400, secs % 86_400);
    let z = days as i64 + 719_468;
    let (era, doe) = (z.div_euclid(146_097), z.rem_euclid(146_097));
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!("{year:04}-{month:02}-{day:02} {:02}:{:02}:{:02}Z", rem / 3600, rem / 60 % 60, rem % 60)
}

/// A row of `docs/compatibility.md`, in its column order: Original → `.p5z`, Format, Volumes, Drive,
/// Firmware, Loader/mounter, Fakelib mode, Result, Date, Notes. The list names no game, so the row has
/// no name, title id or game version. What the report does not know is left as `?` to fill in: the
/// sizes, a `.p5z`'s volumes (a folder or a pkg has none: `—`), the result and the date (the console's
/// clock is often years behind, so the date is not taken from it). Notes are left empty.
pub fn compat_row(r: &Report) -> String {
    let volumes = if r.kind == "p5z" { "?" } else { "—" };
    let cells = [
        "?".into(), kind_text(r.kind, r.packing).1.to_string(), volumes.into(), drive_text(r.drive),
        cell(r.firmware.unwrap_or("")), format!("MaxPack {}", r.version), fakelib_text(r).into(), "?".into(), "?".into(),
        String::new(),
    ];
    format!("| {} |", cells.join(" | "))
}

/// The whole report.
pub fn text(r: &Report) -> String {
    let mounting = match r.waiting {
        None => "MaxPack mounts the games".to_string(),
        Some("kstuff") => "waiting for kstuff (MaxPack mounts nothing yet)".to_string(),
        Some(w) => format!("another mounter is running ({}): MaxPack only installs", w.strip_prefix("other:").unwrap_or(w)),
    };
    let version = if r.game_version.is_empty() { "?" } else { r.game_version };
    let status = match r.error {
        Some(e) if !e.is_empty() => format!("{} ({e})", r.status),
        _ => r.status.to_string(),
    };
    let crash = r.crash.map_or("none recorded".to_string(), |c| {
        format!("{} in {}, {} (console dump folder {})", utc(c.at), c.process, c.status, c.folder)
    });
    let mut lines = vec![
        format!("MaxPack {}: game report", r.version),
        format!("Firmware: {}", r.firmware.unwrap_or("(unknown: please fill in)")),
        format!("Mounting: {mounting}"),
        format!("Game: {} ({}), version {version}", r.title_name, r.title_id),
        format!("Source: {} ({}, {})", r.path, kind_text(r.kind, r.packing).0, r.drive),
        format!("Status: {status}"),
    ];
    if r.hidden {
        lines.push("Hidden in the library: yes".into());
    }
    if r.overlay_off {
        lines.push("MaxPack overlay: off (overlay_off: the game runs as registered, without MaxPack's fakelib, ampr-emu-ng, emulators or sandbox mounts)".into());
    }
    lines.push(format!("Backport: {}", r.backport.unwrap_or("none")));
    let mode = match r.fakelib {
        Mode::Full if r.fakelib_recommended => Some("full"),
        Mode::Full => None,
        Mode::Emulators => Some("emulators only (the game's and backport's fakelib not used)"),
        Mode::Off if crate::fakelib::needs_ng(r.kind, r.packing) => Some("off (only ampr-emu-ng)"),
        Mode::Off => Some("off (empty: nothing at launch)"),
    };
    // From the recommended settings: said after each of the two only when the list itself sets it.
    let from_list = |set: bool| if set { " (recommended)" } else { "" };
    if let Some(mode) = mode {
        lines.push(format!("Fakelib mode: {mode}{}", from_list(r.fakelib_recommended)));
    }
    let playgo_from_list = from_list(r.playgo_recommended && matches!(r.playgo, "on" | "off"));
    lines.extend([
        format!("ampr-emu-ng: {}", r.ampr),
        format!("PlayGo stub: {}{playgo_from_list}", r.playgo),
        format!("Last crash: {crash}"),
        String::new(),
        "Row for docs/compatibility.md (fill in each ?; it names no game):".into(),
        compat_row(r),
        String::new(),
        format!("maxpack.log, last {LOG_LINES} lines with {}:", r.title_id),
    ]);
    let log: Vec<&str> = r.log.lines().filter(|l| l.contains(r.title_id)).collect();
    lines.extend(log[log.len().saturating_sub(LOG_LINES)..].iter().map(|l| l.to_string()));
    let mut out = lines.join("\n");
    out.push('\n');
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn starfall<'a>(log: &'a str, crash: Option<&'a Crash>) -> Report<'a> {
        Report {
            version: "1.0.0", firmware: Some("13.60"), waiting: None,
            title_id: "PPSA10001", title_name: "Starfall Odyssey", game_version: "01.000.010",
            path: "/data/homebrew/Starfall Odyssey", kind: "folder", packing: Some("zstd"), drive: "/data",
            status: "ready", error: None, hidden: false, overlay_off: false, fakelib: Mode::Full, fakelib_recommended: false,
            playgo_recommended: false, backport: None, ampr: "bundled v0.5.0-maxpack.3", playgo: "on", crash, log,
        }
    }

    #[test]
    fn a_recommended_setting_says_so() {
        let line = |r: &Report, start: &str| text(r).lines().find(|l| l.starts_with(start)).map(str::to_owned);
        let r = Report { kind: "pkg", packing: None, fakelib: Mode::Off, fakelib_recommended: true, ..starfall("", None) };
        assert_eq!(line(&r, "Fakelib").as_deref(), Some("Fakelib mode: off (only ampr-emu-ng) (recommended)"));
        let r = Report { fakelib: Mode::Emulators, fakelib_recommended: true, ..starfall("", None) };
        assert_eq!(line(&r, "Fakelib").as_deref(),
            Some("Fakelib mode: emulators only (the game's and backport's fakelib not used) (recommended)"));
        // A Full the list sets is said too.
        let r = Report { fakelib_recommended: true, ..starfall("", None) };
        assert_eq!(line(&r, "Fakelib").as_deref(), Some("Fakelib mode: full (recommended)"));
        // The list's PlayGo stub; with both, both say so.
        let r = Report { playgo_recommended: true, playgo: "off", ..starfall("", None) };
        assert_eq!(line(&r, "PlayGo").as_deref(), Some("PlayGo stub: off (recommended)"));
        let r = Report { fakelib: Mode::Emulators, fakelib_recommended: true, playgo_recommended: true, playgo: "on", ..starfall("", None) };
        assert!(line(&r, "Fakelib").unwrap().ends_with(" (recommended)"));
        assert_eq!(line(&r, "PlayGo").as_deref(), Some("PlayGo stub: on (recommended)"));
        // Not added at all: nothing of the list's to say there.
        let r = Report { playgo_recommended: true, playgo: "not added (add_emulators off)", ..starfall("", None) };
        assert_eq!(line(&r, "PlayGo").as_deref(), Some("PlayGo stub: not added (add_emulators off)"));
        // The user's or the default: as before.
        assert_eq!(line(&starfall("", None), "Fakelib"), None);
        assert_eq!(line(&starfall("", None), "PlayGo").as_deref(), Some("PlayGo stub: on"));
        // The compatibility row keeps the mode alone.
        let r = Report { fakelib: Mode::Off, fakelib_recommended: true, ..starfall("", None) };
        assert!(compat_row(&r).contains(" | Off | "), "{}", compat_row(&r));
    }

    #[test]
    fn only_the_setting_the_list_sets_says_recommended() {
        let line = |r: &Report, start: &str| text(r).lines().find(|l| l.starts_with(start)).map(str::to_owned);
        // The list sets only the Fakelib mode: the stub, on by default, is not the list's.
        let r = Report { fakelib: Mode::Emulators, fakelib_recommended: true, playgo: "on", ..starfall("", None) };
        assert_eq!(line(&r, "PlayGo").as_deref(), Some("PlayGo stub: on"));
        // The list sets only the PlayGo stub: Full is the default, no Fakelib line.
        let r = Report { playgo_recommended: true, playgo: "off", ..starfall("", None) };
        assert_eq!(line(&r, "Fakelib"), None);
        assert_eq!(line(&r, "PlayGo").as_deref(), Some("PlayGo stub: off (recommended)"));
    }

    #[test]
    fn kinds() {
        assert_eq!(kind_text("pkg", None), ("pkg", "pkg"));
        assert_eq!(kind_text("p5z", None), ("p5z", "p5z"));
        assert_eq!(kind_text("folder", Some("plain")), ("folder", "plain folder"));
        assert_eq!(kind_text("folder", Some("zstd")), ("folder (zstd)", "zstd folder"));
        assert_eq!(kind_text("folder", Some("lz4")), ("folder (lz4)", "LZ4 folder"));
        assert_eq!(kind_text("folder", None), ("folder", "plain folder"));
    }

    #[test]
    fn drives() {
        assert_eq!(drive_text("/data"), "internal SSD");
        assert_eq!(drive_text("/mnt/ext1"), "M.2 SSD (console slot)");
        // On USB the report cannot tell an SSD from a hard drive: the row asks.
        assert_eq!(drive_text("/mnt/ext0"), "USB SSD or HDD? (extended storage)");
        assert_eq!(drive_text("/mnt/usb0"), "USB SSD or HDD?");
        assert_eq!(drive_text("/mnt/usb7"), "USB SSD or HDD?");
        // Any other mount is shown as it is, and none at all as ?.
        assert_eq!(drive_text("/mnt/usbx"), "/mnt/usbx");
        assert_eq!(drive_text("/mnt/ext2"), "/mnt/ext2");
        assert_eq!(drive_text(""), "?");
    }

    #[test]
    fn firmware_versions() {
        assert_eq!(fw_text(0x1360_0000).as_deref(), Some("13.60"));
        assert_eq!(fw_text(0x0403_0001).as_deref(), Some("4.03"));
        assert_eq!(fw_text(0x1001_0000).as_deref(), Some("10.01"));
        assert_eq!(fw_text(0x0900_ffff).as_deref(), Some("9.00"));
        for bad in [0, 0x0000_1234, 0x1a00_0000, 0x130a_0000, 0x00_60_0000] {
            assert_eq!(fw_text(bad), None, "{bad:#x}");
        }
    }

    #[test]
    fn utc_times() {
        assert_eq!(utc(0), "1970-01-01 00:00:00Z");
        assert_eq!(utc(951_782_400), "2000-02-29 00:00:00Z");
        assert_eq!(utc(1_700_000_000), "2023-11-14 22:13:20Z");
        assert_eq!(utc(1_767_225_600 + 86_399), "2026-01-01 23:59:59Z");
    }

    #[test]
    fn the_compatibility_row() {
        // Original -> .p5z, Format, Volumes, Drive, Firmware, Loader/mounter, Fakelib mode, Result, Date, Notes.
        let r = starfall("", None);
        assert_eq!(compat_row(&r), "| ? | zstd folder | — | internal SSD | 13.60 | MaxPack 1.0.0 | Full | ? | ? |  |");
        // A .p5z has volumes, to fill in; no firmware shows as ?.
        let r = Report { firmware: None, kind: "p5z", packing: None, drive: "/mnt/ext1", fakelib: Mode::Emulators, ..starfall("", None) };
        assert_eq!(compat_row(&r), "| ? | p5z | ? | M.2 SSD (console slot) | ? | MaxPack 1.0.0 | Emulators only | ? | ? |  |");
        let r = Report { kind: "pkg", packing: None, drive: "/mnt/usb0", fakelib: Mode::Off, ..starfall("", None) };
        assert_eq!(compat_row(&r), "| ? | pkg | — | USB SSD or HDD? | 13.60 | MaxPack 1.0.0 | Off | ? | ? |  |");
        // Without the overlay there is no fakelib at all, whatever its mode says.
        let r = Report { kind: "folder", packing: Some("plain"), overlay_off: true, fakelib: Mode::Off, ..starfall("", None) };
        assert_eq!(compat_row(&r), "| ? | plain folder | — | internal SSD | 13.60 | MaxPack 1.0.0 | none (MaxPack libraries at launch off) | ? | ? |  |");
    }

    #[test]
    fn the_row_names_no_game() {
        // The list identifies a game by its numbers only: no name, no title id, no game version.
        let r = Report { kind: "p5z", packing: None, ..starfall("", None) };
        let row = compat_row(&r);
        for game in ["Starfall", "PPSA10001", "01.000.010", "/data/homebrew"] {
            assert!(!row.contains(game), "{game:?} in {row:?}");
        }
        // The rest of the report still has them, for the issue.
        assert!(text(&r).contains("Game: Starfall Odyssey (PPSA10001), version 01.000.010\n"), "{}", text(&r));
    }

    #[test]
    fn control_characters_never_break_the_row() {
        // A | would split a cell and a line break, a tab or another control character would break the
        // markdown line: a | is escaped and each control character is a space.
        let r = Report { drive: "/mnt/odd|one\r\nx\t\u{7}", firmware: Some("13.6\n0"), ..starfall("", None) };
        let row = compat_row(&r);
        assert_eq!(row, "| ? | zstd folder | — | /mnt/odd\\|one  x   | 13.6 0 | MaxPack 1.0.0 | Full | ? | ? |  |");
        assert!(!row.chars().any(char::is_control), "{row:?}");
        // The whole report keeps the row on one line too.
        assert!(text(&r).contains(&format!("\n{row}\n")), "{}", text(&r));
    }

    #[test]
    fn the_whole_report() {
        let c = Crash { at: 1_767_225_600, process: "eboot.bin".into(), folder: "PPSA10001_1795166239".into(), status: "killed by signal 11".into() };
        let log = "2026-01-01 00:00:00Z [1] PPSA10001: built\n2026-01-01 00:00:01Z [2] PPSA00001: other game\n2026-01-01 00:00:02Z [3] [launch] PPSA10001 exit pid=233\n";
        let want = "MaxPack 1.0.0: game report\n\
Firmware: 13.60\n\
Mounting: MaxPack mounts the games\n\
Game: Starfall Odyssey (PPSA10001), version 01.000.010\n\
Source: /data/homebrew/Starfall Odyssey (folder (zstd), /data)\n\
Status: ready\n\
Backport: none\n\
ampr-emu-ng: bundled v0.5.0-maxpack.3\n\
PlayGo stub: on\n\
Last crash: 2026-01-01 00:00:00Z in eboot.bin, killed by signal 11 (console dump folder PPSA10001_1795166239)\n\
\n\
Row for docs/compatibility.md (fill in each ?; it names no game):\n\
| ? | zstd folder | — | internal SSD | 13.60 | MaxPack 1.0.0 | Full | ? | ? |  |\n\
\n\
maxpack.log, last 40 lines with PPSA10001:\n\
2026-01-01 00:00:00Z [1] PPSA10001: built\n\
2026-01-01 00:00:02Z [3] [launch] PPSA10001 exit pid=233\n";
        assert_eq!(text(&starfall(log, Some(&c))), want);
    }

    #[test]
    fn report_variants() {
        let r = Report {
            firmware: None, waiting: Some("other:shadowmountplus.elf"), status: "error", error: Some("PPSA10001 (/x): bad header"),
            hidden: true, backport: Some("/data/homebrew/backports/PPSA10001"), playgo: "not added (add_emulators off)",
            ..starfall("", None)
        };
        let t = text(&r);
        for line in [
            "Firmware: (unknown: please fill in)\n",
            "Mounting: another mounter is running (shadowmountplus.elf): MaxPack only installs\n",
            "Status: error (PPSA10001 (/x): bad header)\n",
            "Hidden in the library: yes\n",
            "Backport: /data/homebrew/backports/PPSA10001\n",
            "PlayGo stub: not added (add_emulators off)\n",
            "Last crash: none recorded\n",
        ] {
            assert!(t.contains(line), "{line:?} not in:\n{t}");
        }
        assert!(t.ends_with("maxpack.log, last 40 lines with PPSA10001:\n"), "{t}");
        let t = text(&Report { waiting: Some("kstuff"), ..starfall("", None) });
        assert!(t.contains("Mounting: waiting for kstuff (MaxPack mounts nothing yet)\n"), "{t}");
        assert!(!t.contains("Hidden"), "{t}");
        assert!(!t.contains("overlay"), "{t}");
    }

    #[test]
    fn a_game_without_the_overlay_says_so() {
        let r = Report { kind: "folder", packing: Some("plain"), overlay_off: true, ..starfall("", None) };
        let t = text(&r);
        let line = "MaxPack overlay: off (overlay_off: the game runs as registered, without MaxPack's fakelib, ampr-emu-ng, emulators or sandbox mounts)\n";
        assert!(t.contains(line), "{t}");
        // Right after the status, before the libraries it does not get.
        assert!(t.find(line).unwrap() < t.find("ampr-emu-ng: bundled").unwrap(), "{t}");
    }

    #[test]
    fn a_fakelib_mode_other_than_full_says_so() {
        let line = |r: &Report| text(r).lines().find(|l| l.starts_with("Fakelib")).map(str::to_owned);
        assert_eq!(line(&starfall("", None)), None);
        let r = Report { fakelib: Mode::Emulators, ..starfall("", None) };
        assert_eq!(line(&r).as_deref(), Some("Fakelib mode: emulators only (the game's and backport's fakelib not used)"));
        let r = Report { kind: "pkg", packing: None, fakelib: Mode::Off, ..r };
        assert_eq!(line(&r).as_deref(), Some("Fakelib mode: off (only ampr-emu-ng)"));
        let r = Report { kind: "folder", packing: Some("plain"), fakelib: Mode::Off, ..r };
        assert_eq!(line(&r).as_deref(), Some("Fakelib mode: off (empty: nothing at launch)"));
        // Beside the other libraries: after the backport, before ampr-emu-ng.
        let t = text(&r);
        assert!(t.find("Backport:").unwrap() < t.find("Fakelib mode:").unwrap(), "{t}");
        assert!(t.find("Fakelib mode:").unwrap() < t.find("ampr-emu-ng: bundled").unwrap(), "{t}");
    }

    #[test]
    fn only_the_last_log_lines_with_the_title_are_kept() {
        let mut log = String::new();
        for i in 0..100 {
            log += &format!("line {i} PPSA10001\nline {i} other\n");
        }
        let t = text(&starfall(&log, None));
        let tail = t.split("lines with PPSA10001:\n").nth(1).unwrap();
        let lines: Vec<&str> = tail.lines().collect();
        assert_eq!(lines.len(), LOG_LINES);
        assert_eq!(lines[0], "line 60 PPSA10001");
        assert_eq!(lines[LOG_LINES - 1], "line 99 PPSA10001");
    }
}
