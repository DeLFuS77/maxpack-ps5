//! The per-title fakelib mode (config.toml `fakelib_emulators` and `fakelib_off`, `POST /api/fakelib/<TID>`):
//! what a cache's `fakelib/` is composed from (DESIGN.md "Cache"). ShadowMountPlus has the same per-game
//! choice (Default/Full, Emulators only, Disabled): a game that shows a black screen with the libraries its
//! dump or backport ships, or stalls at its add-on check with a bundled emulator, can run with less.
//!
//! Sources are named as `/api/state` names them: `kind` "pkg" | "folder" | "p5z", `packing` "plain" |
//! "zstd" | "lz4" for a folder, else None.
use serde_json::Value;

/// What a title's `fakelib/` is composed from. A title in neither list is Full.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    /// The backport's lib dir or the game's own, then the bundled emulators, then ampr-emu-ng (the default).
    Full,
    /// Not the game's or the backport's lib dir: only the bundled emulators the game needs, then ampr-emu-ng.
    Emulators,
    /// Nothing but ampr-emu-ng, and that only when the game cannot run without it.
    Off,
}

impl Mode {
    /// The mode's name in `/api/state`, `POST /api/fakelib/<TID>` and `.ampr`.
    pub fn name(self) -> &'static str {
        match self {
            Mode::Full => "full",
            Mode::Emulators => "emulators",
            Mode::Off => "off",
        }
    }

    /// The mode a name stands for; None for anything else.
    pub fn from_name(name: &str) -> Option<Mode> {
        [Mode::Full, Mode::Emulators, Mode::Off].into_iter().find(|m| m.name() == name)
    }
}

/// The body of `POST /api/fakelib/<TID>`: exactly `full`, `emulators` or `off`; anything else is None (a 400).
pub fn body(b: &[u8]) -> Option<Mode> {
    std::str::from_utf8(b).ok().and_then(Mode::from_name)
}

/// The mode of `tid` from the two lists. A title in both (a hand edit) is Off.
pub fn mode_of(tid: &str, emulators: &[String], off: &[String]) -> Mode {
    if off.iter().any(|t| t == tid) {
        Mode::Off
    } else if emulators.iter().any(|t| t == tid) {
        Mode::Emulators
    } else {
        Mode::Full
    }
}

/// Sets the mode of `tid`: it goes into the list of `mode` and out of the other (Full: out of both), so a
/// title is never in both; the other titles keep their order. True when a list changed.
pub fn set_mode(emulators: &mut Vec<String>, off: &mut Vec<String>, tid: &str, mode: Mode) -> bool {
    let a = crate::titles::set_listed(emulators, tid, mode == Mode::Emulators);
    let b = crate::titles::set_listed(off, tid, mode == Mode::Off);
    a || b
}

/// Takes out of `emulators` every title `off` lists, so a title is in one list at most (Off wins, as in
/// `mode_of`). The titles taken out, in their order.
pub fn exclusive(emulators: &mut Vec<String>, off: &[String]) -> Vec<String> {
    let (dropped, kept) = std::mem::take(emulators).into_iter().partition(|t| off.contains(t));
    *emulators = kept;
    dropped
}

/// Whether a source cannot run without our ampr-emu-ng once its own lib dir is not used: a pkg (pkg mode),
/// a .p5z (p5z mode) and a zstd or LZ4 folder (its packs, which the game's own libSceAmpr.sprx reads in
/// Full); anything unknown too (the safe side). A plain folder can.
pub fn needs_ng(kind: &str, packing: Option<&str>) -> bool {
    // The same sources as those that need MaxPack's overlay: ng comes in it.
    crate::overlay::needed(kind, packing).is_some()
}

/// The settings the composition depends on besides the mode.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Switches {
    /// `add_emulators`: the bundled DLC (dlc_emu) and PlayGo (pgo_stub) emulators.
    pub add_emulators: bool,
    /// `inject_ampr`: our ampr-emu-ng on top of a folder game's fakelib.
    pub inject_ampr: bool,
    /// The title is in `playgo_off`: no bundled PlayGo stub.
    pub playgo_off: bool,
}

/// What `fakelib/` is composed from, in this order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Sources {
    /// The backport's lib dir if any, else the game's own (`fakelib2/`, else `fakelib/`).
    pub game_libs: bool,
    /// The bundled emulators the game needs (the dlc_emu set, and pgo_stub when `pgo_stub`).
    pub emulators: bool,
    /// pgo_stub among them (never without `emulators`).
    pub pgo_stub: bool,
    /// Our ampr-emu-ng (the ampr dir) on top.
    pub ampr: bool,
}

/// The sources of a title's `fakelib/` for its mode, its kind and packing, and the settings. Full is the
/// composition MaxPack always made: the game's libs, the emulators with `add_emulators`, ampr-emu-ng with
/// `inject_ampr` and always for a pkg or a .p5z. Emulators drops the game's libs, so a packed folder, whose
/// own libSceAmpr.sprx read its packs, gets ours whatever `inject_ampr` says. Off drops the emulators too
/// and keeps ampr-emu-ng only for a source that `needs_ng`, whatever `inject_ampr` says.
pub fn sources(mode: Mode, kind: &str, packing: Option<&str>, sw: Switches) -> Sources {
    let (game_libs, emulators, ampr) = match mode {
        Mode::Full => (true, sw.add_emulators, sw.inject_ampr || matches!(kind, "pkg" | "p5z")),
        Mode::Emulators => (false, sw.add_emulators, sw.inject_ampr || needs_ng(kind, packing)),
        Mode::Off => (false, false, needs_ng(kind, packing)),
    };
    Sources { game_libs, emulators, pgo_stub: emulators && !sw.playgo_off, ampr }
}

/// What follows `<TID>: fakelib: ` in the log line of a composition in `mode` (one per composition); None
/// for Full, which logs nothing new.
pub fn note(mode: Mode, src: Sources) -> Option<&'static str> {
    match mode {
        Mode::Full => None,
        Mode::Emulators => Some("mode emulators (the game's and backport's fakelib not used)"),
        Mode::Off if src.ampr => Some("mode off (only ampr-emu-ng)"),
        Mode::Off => Some("mode off (empty)"),
    }
}

/// The `.ampr` key of the mode a `fakelib/` was composed with; written only for a mode other than Full.
pub const RECORD_KEY: &str = "fakelib_mode";

/// Writes `mode` into a `.ampr` record: RECORD_KEY for Emulators or Off, nothing for Full, so a Full
/// record is what builds before the setting wrote.
pub fn record(state: &mut Value, mode: Mode) {
    if let (Some(obj), false) = (state.as_object_mut(), mode == Mode::Full) {
        obj.insert(RECORD_KEY.into(), mode.name().into());
    }
}

/// The mode a `.ampr` record says its `fakelib/` was composed with. No RECORD_KEY (an older `.ampr`, a Full
/// one, or none at all) or an unknown value is Full, so the caches built before the setting stay fresh.
pub fn recorded(state: &Value) -> Mode {
    state.get(RECORD_KEY).and_then(Value::as_str).and_then(Mode::from_name).unwrap_or(Mode::Full)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    const ALL: [Mode; 3] = [Mode::Full, Mode::Emulators, Mode::Off];
    /// Every kind of source: (kind, packing).
    const KINDS: [(&str, Option<&str>); 5] =
        [("folder", Some("plain")), ("folder", Some("zstd")), ("folder", Some("lz4")), ("pkg", None), ("p5z", None)];
    const DEFAULTS: Switches = Switches { add_emulators: true, inject_ampr: true, playgo_off: false };

    fn list(items: &[&str]) -> Vec<String> {
        items.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn names_and_bodies() {
        for (mode, name) in [(Mode::Full, "full"), (Mode::Emulators, "emulators"), (Mode::Off, "off")] {
            assert_eq!(mode.name(), name);
            assert_eq!(Mode::from_name(name), Some(mode));
            assert_eq!(body(name.as_bytes()), Some(mode));
        }
        for bad in [&b""[..], b"Full", b"OFF", b"off\n", b" emulators", b"emulator", b"on", b"default", b"disabled"] {
            assert_eq!(body(bad), None, "{:?}", String::from_utf8_lossy(bad));
        }
        assert_eq!(Mode::from_name("none"), None);
    }

    #[test]
    fn a_title_in_neither_list_is_full() {
        let (emus, off) = (list(&["PPSA00002"]), list(&["PPSA00003"]));
        assert_eq!(mode_of("PPSA00001", &emus, &off), Mode::Full);
        assert_eq!(mode_of("PPSA00002", &emus, &off), Mode::Emulators);
        assert_eq!(mode_of("PPSA00003", &emus, &off), Mode::Off);
        // Listed in both by hand: Off, the one that takes the most away.
        assert_eq!(mode_of("PPSA00004", &list(&["PPSA00004"]), &list(&["PPSA00004"])), Mode::Off);
        assert_eq!(mode_of("PPSA00001", &[], &[]), Mode::Full);
    }

    #[test]
    fn a_title_is_in_one_list_at_most() {
        let (mut emus, mut off) = (list(&["PPSA00009"]), list(&["PPSA00008"]));
        assert!(set_mode(&mut emus, &mut off, "PPSA00001", Mode::Emulators));
        assert_eq!((emus.clone(), off.clone()), (list(&["PPSA00009", "PPSA00001"]), list(&["PPSA00008"])));
        // Off takes it out of fakelib_emulators.
        assert!(set_mode(&mut emus, &mut off, "PPSA00001", Mode::Off));
        assert_eq!((emus.clone(), off.clone()), (list(&["PPSA00009"]), list(&["PPSA00008", "PPSA00001"])));
        // And back to Emulators takes it out of fakelib_off.
        assert!(set_mode(&mut emus, &mut off, "PPSA00001", Mode::Emulators));
        assert_eq!((emus.clone(), off.clone()), (list(&["PPSA00009", "PPSA00001"]), list(&["PPSA00008"])));
        // The same mode again changes nothing.
        assert!(!set_mode(&mut emus, &mut off, "PPSA00001", Mode::Emulators));
        // Full: out of both; the others stay as they were.
        assert!(set_mode(&mut emus, &mut off, "PPSA00001", Mode::Full));
        assert_eq!((emus.clone(), off.clone()), (list(&["PPSA00009"]), list(&["PPSA00008"])));
        assert!(!set_mode(&mut emus, &mut off, "PPSA00001", Mode::Full));
        // A title listed in both by hand: any mode leaves it in one list at most.
        let (mut emus, mut off) = (list(&["PPSA00001"]), list(&["PPSA00001"]));
        assert!(set_mode(&mut emus, &mut off, "PPSA00001", Mode::Emulators));
        assert_eq!((emus, off), (list(&["PPSA00001"]), list(&[])));
        for mode in ALL {
            let (mut emus, mut off) = (list(&["PPSA00001"]), list(&["PPSA00001"]));
            set_mode(&mut emus, &mut off, "PPSA00001", mode);
            assert_eq!(mode_of("PPSA00001", &emus, &off), mode, "{mode:?}");
            assert!(emus.len() + off.len() <= 1, "{mode:?}: {emus:?} {off:?}");
        }
    }

    #[test]
    fn a_hand_edit_in_both_lists_keeps_off() {
        let mut emus = list(&["PPSA00001", "PPSA00002", "PPSA00003"]);
        let dropped = exclusive(&mut emus, &list(&["PPSA00003", "PPSA00001", "PPSA00007"]));
        assert_eq!(dropped, list(&["PPSA00001", "PPSA00003"]));
        assert_eq!(emus, list(&["PPSA00002"]));
        assert!(exclusive(&mut emus, &list(&["PPSA00001"])).is_empty());
        assert_eq!(emus, list(&["PPSA00002"]));
    }

    #[test]
    fn which_sources_cannot_do_without_ampr_emu_ng() {
        assert!(!needs_ng("folder", Some("plain")));
        for (kind, packing) in [("folder", Some("zstd")), ("folder", Some("lz4")), ("pkg", None), ("p5z", None)] {
            assert!(needs_ng(kind, packing), "{kind} {packing:?}");
        }
        // Anything unknown keeps it.
        assert!(needs_ng("folder", None));
        assert!(needs_ng("", None));
    }

    #[test]
    fn full_is_the_composition_maxpack_always_made() {
        for (kind, packing) in KINDS {
            let s = sources(Mode::Full, kind, packing, DEFAULTS);
            assert_eq!(s, Sources { game_libs: true, emulators: true, pgo_stub: true, ampr: true }, "{kind} {packing:?}");
        }
        // Without inject_ampr, only a pkg or a .p5z gets ours: a packed folder reads its packs with its own.
        let no_inject = Switches { inject_ampr: false, ..DEFAULTS };
        for (kind, packing) in KINDS {
            let ampr = matches!(kind, "pkg" | "p5z");
            assert_eq!(sources(Mode::Full, kind, packing, no_inject).ampr, ampr, "{kind} {packing:?}");
        }
        let no_emus = Switches { add_emulators: false, ..DEFAULTS };
        assert_eq!(sources(Mode::Full, "pkg", None, no_emus), Sources { game_libs: true, emulators: false, pgo_stub: false, ampr: true });
    }

    #[test]
    fn emulators_drops_the_game_libs_only() {
        for (kind, packing) in KINDS {
            let s = sources(Mode::Emulators, kind, packing, DEFAULTS);
            assert_eq!(s, Sources { game_libs: false, emulators: true, pgo_stub: true, ampr: true }, "{kind} {packing:?}");
        }
        // Without inject_ampr a packed folder still gets ours: its own libSceAmpr.sprx is gone with its libs.
        let no_inject = Switches { inject_ampr: false, ..DEFAULTS };
        for (kind, packing) in KINDS {
            let ampr = !(kind == "folder" && packing == Some("plain"));
            assert_eq!(sources(Mode::Emulators, kind, packing, no_inject).ampr, ampr, "{kind} {packing:?}");
        }
        // add_emulators off: no emulators to keep, ampr-emu-ng as in Full.
        let no_emus = Switches { add_emulators: false, ..DEFAULTS };
        let s = sources(Mode::Emulators, "folder", Some("plain"), no_emus);
        assert_eq!(s, Sources { game_libs: false, emulators: false, pgo_stub: false, ampr: true });
    }

    #[test]
    fn playgo_off_still_applies_in_full_and_emulators() {
        let sw = Switches { playgo_off: true, ..DEFAULTS };
        for mode in [Mode::Full, Mode::Emulators] {
            for (kind, packing) in KINDS {
                let s = sources(mode, kind, packing, sw);
                assert!(s.emulators && !s.pgo_stub, "{mode:?} {kind} {packing:?}: {s:?}");
            }
        }
        // pgo_stub never goes without the emulators.
        for mode in ALL {
            for (kind, packing) in KINDS {
                for (add_emulators, playgo_off) in [(false, false), (false, true), (true, true)] {
                    let s = sources(mode, kind, packing, Switches { add_emulators, playgo_off, ..DEFAULTS });
                    assert!(!s.pgo_stub, "{mode:?} {kind} {packing:?}: {s:?}");
                }
            }
        }
    }

    #[test]
    fn off_keeps_ampr_emu_ng_only_where_the_game_needs_it() {
        for inject_ampr in [true, false] {
            let sw = Switches { inject_ampr, ..DEFAULTS };
            for (kind, packing) in KINDS {
                let s = sources(Mode::Off, kind, packing, sw);
                let ampr = !(kind == "folder" && packing == Some("plain"));
                assert_eq!(s, Sources { game_libs: false, emulators: false, pgo_stub: false, ampr }, "{kind} {packing:?} {inject_ampr}");
            }
        }
    }

    #[test]
    fn one_log_note_per_mode_but_full() {
        for (kind, packing) in KINDS {
            assert_eq!(note(Mode::Full, sources(Mode::Full, kind, packing, DEFAULTS)), None);
            assert_eq!(note(Mode::Emulators, sources(Mode::Emulators, kind, packing, DEFAULTS)),
                Some("mode emulators (the game's and backport's fakelib not used)"));
        }
        for (kind, packing) in [("folder", Some("zstd")), ("folder", Some("lz4")), ("pkg", None), ("p5z", None)] {
            assert_eq!(note(Mode::Off, sources(Mode::Off, kind, packing, DEFAULTS)), Some("mode off (only ampr-emu-ng)"));
        }
        assert_eq!(note(Mode::Off, sources(Mode::Off, "folder", Some("plain"), DEFAULTS)), Some("mode off (empty)"));
    }

    #[test]
    fn an_older_ampr_record_reads_as_full() {
        // What builds before the setting wrote: no fakelib_mode key.
        let old = json!({
            "ampr": {"libSceAmpr.sprx": [123456, 1767225600]},
            "emus": {"dlc_emu": "0.3", "pgo_stub": "0.6", "dlc_ini": false, "eboot": [1000, 1767225600], "playgo_off": false},
            "playgo": [1000, 1767225600, true],
        });
        assert_eq!(recorded(&old), Mode::Full);
        // No .ampr at all, one that is not an object, an unknown value.
        assert_eq!(recorded(&Value::Null), Mode::Full);
        assert_eq!(recorded(&json!([1, 2])), Mode::Full);
        assert_eq!(recorded(&json!({"fakelib_mode": "sideways"})), Mode::Full);
        assert_eq!(recorded(&json!({"fakelib_mode": 2})), Mode::Full);
        // Full writes nothing: the record stays what older builds wrote, so check() keeps them fresh.
        let mut full = old.clone();
        record(&mut full, Mode::Full);
        assert_eq!(full, old);
        for mode in [Mode::Emulators, Mode::Off] {
            let mut st = old.clone();
            record(&mut st, mode);
            assert_eq!(st[RECORD_KEY], mode.name());
            assert_eq!(recorded(&st), mode);
            assert_ne!(st, old, "{mode:?}");
        }
    }
}
