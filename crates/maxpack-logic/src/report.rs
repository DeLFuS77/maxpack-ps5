//! The per-game report (`GET /api/report/<TID>`, the page's "Copy report"): plain text to paste into
//! an issue, with a ready row for `docs/compatibilidad.md`. See DESIGN.md "Web API".
use crate::crash::Crash;

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
    pub backport: Option<&'a str>,
    /// /api/ampr kind and version ("bundled v0.5.0-maxpack.3").
    pub ampr: &'a str,
    /// "on", "off" or "not added (add_emulators off)".
    pub playgo: &'a str,
    pub crash: Option<&'a Crash>,
    /// The log tail the lines with the title id are taken from.
    pub log: &'a str,
}

/// The source kind as /api/state names it: (the report's words, the compatibility list's words).
pub fn kind_text(kind: &str, packing: Option<&str>) -> (&'static str, &'static str) {
    match (kind, packing) {
        ("pkg", _) => ("pkg", "pkg"),
        ("p5z", _) => ("p5z", "p5z"),
        (_, Some("zstd")) => ("folder (zstd)", "carpeta zstd"),
        (_, Some("lz4")) => ("folder (lz4)", "carpeta lz4"),
        _ => ("folder", "carpeta"),
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

/// A row of `docs/compatibilidad.md` with the result and the date left to fill in ("?"): the
/// console's clock is often years behind, so the date is not taken from it.
pub fn compat_row(r: &Report) -> String {
    let cell = |s: &str| if s.is_empty() { "?".to_string() } else { s.replace('|', "\\|") };
    let cells = [
        cell(r.title_id), cell(r.title_name), cell(r.game_version), kind_text(r.kind, r.packing).1.to_string(),
        cell(r.firmware.unwrap_or("")), format!("MaxPack {}", r.version), "?".into(), "?".into(),
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
    lines.extend([
        format!("Backport: {}", r.backport.unwrap_or("none")),
        format!("ampr-emu-ng: {}", r.ampr),
        format!("PlayGo stub: {}", r.playgo),
        format!("Last crash: {crash}"),
        String::new(),
        "Row for docs/compatibilidad.md (fill in the result and the date):".into(),
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

    fn silksong<'a>(log: &'a str, crash: Option<&'a Crash>) -> Report<'a> {
        Report {
            version: "0.2.0", firmware: Some("13.60"), waiting: None,
            title_id: "PPSA12544", title_name: "Hollow Knight: Silksong", game_version: "01.000.010",
            path: "/data/homebrew/Hollow Knight Silksong", kind: "folder", packing: Some("zstd"), drive: "/data",
            status: "ready", error: None, hidden: false, overlay_off: false, backport: None,
            ampr: "bundled v0.5.0-maxpack.3", playgo: "on", crash, log,
        }
    }

    #[test]
    fn kinds() {
        assert_eq!(kind_text("pkg", None), ("pkg", "pkg"));
        assert_eq!(kind_text("p5z", None), ("p5z", "p5z"));
        assert_eq!(kind_text("folder", Some("plain")), ("folder", "carpeta"));
        assert_eq!(kind_text("folder", Some("zstd")), ("folder (zstd)", "carpeta zstd"));
        assert_eq!(kind_text("folder", Some("lz4")), ("folder (lz4)", "carpeta lz4"));
        assert_eq!(kind_text("folder", None), ("folder", "carpeta"));
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
        let r = silksong("", None);
        assert_eq!(compat_row(&r), "| PPSA12544 | Hollow Knight: Silksong | 01.000.010 | carpeta zstd | 13.60 | MaxPack 0.2.0 | ? | ? |");
        // A | in a name would split the cell; no firmware and no version show as ?.
        let r = Report { title_name: "A | B", firmware: None, game_version: "", kind: "p5z", packing: None, ..silksong("", None) };
        assert_eq!(compat_row(&r), "| PPSA12544 | A \\| B | ? | p5z | ? | MaxPack 0.2.0 | ? | ? |");
    }

    #[test]
    fn the_whole_report() {
        let c = Crash { at: 1_767_225_600, process: "eboot.bin".into(), folder: "PPSA12544_1795166239".into(), status: "killed by signal 11".into() };
        let log = "2026-01-01 00:00:00Z [1] PPSA12544: built\n2026-01-01 00:00:01Z [2] PPSA00001: other game\n2026-01-01 00:00:02Z [3] [launch] PPSA12544 exit pid=233\n";
        let want = "MaxPack 0.2.0: game report\n\
Firmware: 13.60\n\
Mounting: MaxPack mounts the games\n\
Game: Hollow Knight: Silksong (PPSA12544), version 01.000.010\n\
Source: /data/homebrew/Hollow Knight Silksong (folder (zstd), /data)\n\
Status: ready\n\
Backport: none\n\
ampr-emu-ng: bundled v0.5.0-maxpack.3\n\
PlayGo stub: on\n\
Last crash: 2026-01-01 00:00:00Z in eboot.bin, killed by signal 11 (console dump folder PPSA12544_1795166239)\n\
\n\
Row for docs/compatibilidad.md (fill in the result and the date):\n\
| PPSA12544 | Hollow Knight: Silksong | 01.000.010 | carpeta zstd | 13.60 | MaxPack 0.2.0 | ? | ? |\n\
\n\
maxpack.log, last 40 lines with PPSA12544:\n\
2026-01-01 00:00:00Z [1] PPSA12544: built\n\
2026-01-01 00:00:02Z [3] [launch] PPSA12544 exit pid=233\n";
        assert_eq!(text(&silksong(log, Some(&c))), want);
    }

    #[test]
    fn report_variants() {
        let r = Report {
            firmware: None, waiting: Some("other:shadowmountplus.elf"), status: "error", error: Some("PPSA12544 (/x): bad header"),
            hidden: true, backport: Some("/data/homebrew/backports/PPSA12544"), playgo: "not added (add_emulators off)",
            ..silksong("", None)
        };
        let t = text(&r);
        for line in [
            "Firmware: (unknown: please fill in)\n",
            "Mounting: another mounter is running (shadowmountplus.elf): MaxPack only installs\n",
            "Status: error (PPSA12544 (/x): bad header)\n",
            "Hidden in the library: yes\n",
            "Backport: /data/homebrew/backports/PPSA12544\n",
            "PlayGo stub: not added (add_emulators off)\n",
            "Last crash: none recorded\n",
        ] {
            assert!(t.contains(line), "{line:?} not in:\n{t}");
        }
        assert!(t.ends_with("maxpack.log, last 40 lines with PPSA12544:\n"), "{t}");
        let t = text(&Report { waiting: Some("kstuff"), ..silksong("", None) });
        assert!(t.contains("Mounting: waiting for kstuff (MaxPack mounts nothing yet)\n"), "{t}");
        assert!(!t.contains("Hidden"), "{t}");
        assert!(!t.contains("overlay"), "{t}");
    }

    #[test]
    fn a_game_without_the_overlay_says_so() {
        let r = Report { kind: "folder", packing: Some("plain"), overlay_off: true, ..silksong("", None) };
        let t = text(&r);
        let line = "MaxPack overlay: off (overlay_off: the game runs as registered, without MaxPack's fakelib, ampr-emu-ng, emulators or sandbox mounts)\n";
        assert!(t.contains(line), "{t}");
        // Right after the status, before the libraries it does not get.
        assert!(t.find(line).unwrap() < t.find("ampr-emu-ng: bundled").unwrap(), "{t}");
    }

    #[test]
    fn only_the_last_log_lines_with_the_title_are_kept() {
        let mut log = String::new();
        for i in 0..100 {
            log += &format!("line {i} PPSA12544\nline {i} other\n");
        }
        let t = text(&silksong(&log, None));
        let tail = t.split("lines with PPSA12544:\n").nth(1).unwrap();
        let lines: Vec<&str> = tail.lines().collect();
        assert_eq!(lines.len(), LOG_LINES);
        assert_eq!(lines[0], "line 60 PPSA12544");
        assert_eq!(lines[LOG_LINES - 1], "line 99 PPSA12544");
    }
}
