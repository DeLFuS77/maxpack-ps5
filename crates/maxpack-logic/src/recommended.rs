//! Recommended per-game settings (DESIGN.md "Recommended settings"): a list, bundled in the ELF
//! (`data/recommended.json`) and refreshed from GitHub, of the Fakelib mode, the PlayGo stub and a
//! notice that a game needs. A game whose settings the user chose by hand (`manual_settings`, or
//! listed in `fakelib_emulators`, `fakelib_off` or `playgo_off`) keeps them.
//!
//! The list names no game: each title is keyed by `key`, the first 16 bytes of the SHA-256 of
//! KEY_PREFIX and its title id, in lowercase hex (`tools/recommended_key.js` prints it).
//!
//! What the console takes: a wrong structure rejects the whole file (not JSON, over MAX_BYTES or
//! MAX_TITLES, a `format` other than 1, a `version` that is not a positive integer, a key that is
//! not 32 lowercase hex digits, an entry that is not an object, a known field of the wrong JSON type,
//! a malformed `fw_min` / `fw_max`); an unknown value of a known field (a newer Fakelib mode or
//! notice) or an unknown field is ignored for that entry, so a newer list stays readable.
use crate::fakelib::{self, Mode};
use serde_json::{Map, Value};
use std::collections::HashMap;

/// What a title id is hashed with to make its key.
pub const KEY_PREFIX: &str = "maxpack-recommended-v1:";
/// A list file over this many bytes is refused.
pub const MAX_BYTES: usize = 262144;
/// A list with more titles than this is refused.
pub const MAX_TITLES: usize = 4096;

/// A text the console shows for a game (the page's sheet and a notification at its launch).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Notice {
    /// The game can take several minutes to start, with a black screen.
    LongFirstBoot,
}

impl Notice {
    /// The notice's name in the list and in `/api/state`.
    pub fn name(self) -> &'static str {
        match self {
            Notice::LongFirstBoot => "long_first_boot",
        }
    }

    /// The notice a name stands for; None for anything else.
    pub fn from_name(name: &str) -> Option<Notice> {
        [Notice::LongFirstBoot].into_iter().find(|n| n.name() == name)
    }
}

/// One title's entry. None: the list says nothing of it (or a value this version does not know).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Entry {
    pub fakelib: Option<Mode>,
    /// `playgo: false` in the file: no bundled PlayGo stub (as `playgo_off`).
    pub playgo_off: Option<bool>,
    pub notice: Option<Notice>,
    /// The console's firmware must be at least this (inclusive), as `parse_fw` reads it.
    pub fw_min: Option<(u8, u8)>,
    /// The console's firmware must be at most this (inclusive).
    pub fw_max: Option<(u8, u8)>,
}

/// A parsed list.
#[derive(Clone, Debug, PartialEq)]
pub struct List {
    /// Goes up with each change of the list; the copy with the higher one is used (`newer`).
    pub version: u64,
    titles: HashMap<String, Entry>,
}

impl List {
    /// How many titles it has.
    pub fn len(&self) -> usize {
        self.titles.len()
    }

    pub fn is_empty(&self) -> bool {
        self.titles.is_empty()
    }
}

/// The key of title id `tid` in the list.
pub fn key(tid: &str) -> String {
    let d = crate::sha256::digest(format!("{KEY_PREFIX}{tid}").as_bytes());
    d[..16].iter().map(|b| format!("{b:02x}")).collect()
}

/// A firmware bound `NN.NN` ("13.60") as the console's version reads it, BCD: (0x13, 0x60). None for
/// anything else ("13.6", "4.03").
pub fn parse_fw(s: &str) -> Option<(u8, u8)> {
    let b = s.as_bytes();
    let digits = [0, 1, 3, 4].iter().all(|&i| b.get(i).is_some_and(u8::is_ascii_digit));
    (b.len() == 5 && b[2] == b'.' && digits).then(|| ((b[0] - b'0') << 4 | (b[1] - b'0'), (b[3] - b'0') << 4 | (b[4] - b'0')))
}

fn valid_key(k: &str) -> bool {
    k.len() == 32 && k.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// A known field of the wrong type, or a malformed firmware bound: the whole file is refused.
fn field<'a>(e: &'a Map<String, Value>, k: &str, name: &str, want: &str, ok: fn(&Value) -> bool) -> Result<Option<&'a Value>, String> {
    match e.get(name) {
        Some(v) if !ok(v) => Err(format!("title {k}: {name} is not {want}")),
        v => Ok(v),
    }
}

fn entry(k: &str, v: &Value) -> Result<Entry, String> {
    let e = v.as_object().ok_or_else(|| format!("title {k}: not an object"))?;
    let text = |name| field(e, k, name, "a string", Value::is_string).map(|v| v.and_then(Value::as_str));
    let fw = |name| match text(name)? {
        None => Ok(None),
        Some(s) => parse_fw(s).map(Some).ok_or_else(|| format!("title {k}: {name} {s:?} is not NN.NN")),
    };
    Ok(Entry {
        fakelib: text("fakelib")?.and_then(Mode::from_name),
        playgo_off: field(e, k, "playgo", "true or false", Value::is_boolean)?.and_then(Value::as_bool).map(|on| !on),
        notice: text("notice")?.and_then(Notice::from_name),
        fw_min: fw("fw_min")?,
        fw_max: fw("fw_max")?,
    })
}

/// Reads a list file; Err: a short reason for the log.
pub fn parse(bytes: &[u8]) -> Result<List, String> {
    if bytes.len() > MAX_BYTES {
        return Err(format!("{} bytes, over {MAX_BYTES}", bytes.len()));
    }
    let v: Value = serde_json::from_slice(bytes).map_err(|e| format!("not JSON: {e}"))?;
    let top = v.as_object().ok_or("not a JSON object")?;
    if top.get("format").and_then(Value::as_u64) != Some(1) {
        return Err(format!("format {} is not 1", top.get("format").unwrap_or(&Value::Null)));
    }
    let version = top.get("version").and_then(Value::as_u64).filter(|&n| n > 0)
        .ok_or_else(|| format!("version {} is not a positive integer", top.get("version").unwrap_or(&Value::Null)))?;
    let titles = top.get("titles").and_then(Value::as_object).ok_or("no titles object")?;
    if titles.len() > MAX_TITLES {
        return Err(format!("{} titles, over {MAX_TITLES}", titles.len()));
    }
    let mut out = HashMap::with_capacity(titles.len());
    for (k, v) in titles {
        if !valid_key(k) {
            return Err(format!("title key {k:?} is not 32 lowercase hex digits"));
        }
        out.insert(k.clone(), entry(k, v)?);
    }
    Ok(List { version, titles: out })
}

/// The list to use: `other` (a downloaded copy) when its version is higher than the bundled one's.
pub fn newer<'a>(bundled: &'a List, other: Option<&'a List>) -> &'a List {
    match other {
        Some(o) if o.version > bundled.version => o,
        _ => bundled,
    }
}

/// The entry of `tid` that applies on firmware `fw` (None: unknown, so an entry with a firmware bound
/// does not apply).
pub fn entry_for<'a>(list: &'a List, tid: &str, fw: Option<(u8, u8)>) -> Option<&'a Entry> {
    let e = list.titles.get(&key(tid))?;
    if e.fw_min.is_none() && e.fw_max.is_none() {
        return Some(e);
    }
    let fw = fw?;
    (e.fw_min.is_none_or(|m| fw >= m) && e.fw_max.is_none_or(|m| fw <= m)).then_some(e)
}

/// Where a game's Fakelib mode and PlayGo stub come from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Source {
    /// Nobody chose them: Full, with the stub.
    Default,
    /// The user, on the game's sheet (or by hand in config.toml).
    User,
    /// The list.
    Recommended,
}

impl Source {
    /// The name in `/api/state` `settings_source`.
    pub fn name(self) -> &'static str {
        match self {
            Source::Default => "default",
            Source::User => "user",
            Source::Recommended => "recommended",
        }
    }
}

/// A game's settings as used: what builds its fakelib/, and the notice to show.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Effective {
    pub fakelib: Mode,
    pub playgo_off: bool,
    pub source: Source,
    pub notice: Option<Notice>,
}

/// The settings of `tid`: the user's when any of the four lists names it (`manual_settings`, and the
/// lists of 1.0.1 and earlier, which only a choice of the user's ever filled), else the entry's when
/// it has a Fakelib mode or a PlayGo setting (a missing one is Full / with the stub), else Full with the
/// stub. The notice is the entry's in every case.
pub fn effective(tid: &str, emulators: &[String], off: &[String], playgo_off: &[String], manual: &[String], entry: Option<&Entry>) -> Effective {
    let notice = entry.and_then(|e| e.notice);
    let listed = |l: &[String]| l.iter().any(|t| t == tid);
    if [manual, emulators, off, playgo_off].into_iter().any(listed) {
        return Effective { fakelib: fakelib::mode_of(tid, emulators, off), playgo_off: listed(playgo_off), source: Source::User, notice };
    }
    match entry.filter(|e| e.fakelib.is_some() || e.playgo_off.is_some()) {
        Some(e) => Effective {
            fakelib: e.fakelib.unwrap_or(Mode::Full),
            playgo_off: e.playgo_off.unwrap_or(false),
            source: Source::Recommended,
            notice,
        },
        None => Effective { fakelib: Mode::Full, playgo_off: false, source: Source::Default, notice },
    }
}

/// Before the user's change of the Fakelib mode or the PlayGo stub on `tid`'s sheet: `tid` goes into
/// `manual`, and, when its settings were the list's (`before`, its `effective` until now), the list's
/// values go into the lists, so the setting the user did not touch stays as it was shown. The change
/// itself is made after this.
pub fn hand_choice(tid: &str, before: Effective, emulators: &mut Vec<String>, off: &mut Vec<String>, playgo_off: &mut Vec<String>, manual: &mut Vec<String>) {
    if before.source == Source::Recommended {
        fakelib::set_mode(emulators, off, tid, before.fakelib);
        crate::titles::set_listed(playgo_off, tid, before.playgo_off);
    }
    crate::titles::set_listed(manual, tid, true);
}

/// "Back to the recommended setting": `tid` out of the four lists. True when one changed.
pub fn reset(tid: &str, emulators: &mut Vec<String>, off: &mut Vec<String>, playgo_off: &mut Vec<String>, manual: &mut Vec<String>) -> bool {
    [emulators, off, playgo_off, manual].into_iter().fold(false, |changed, l| crate::titles::set_listed(l, tid, false) | changed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fakelib::Mode;

    fn list(items: &[&str]) -> Vec<String> {
        items.iter().map(|s| s.to_string()).collect()
    }

    /// A list file with `titles` (a JSON object's text) and the usual format and version.
    fn file(titles: &str) -> String {
        format!(r#"{{"format": 1, "version": 7, "titles": {titles}}}"#)
    }

    /// The key of PPSA01234, the made-up title of these tests.
    const K: &str = "17c2e30975e7e93c42a3ccaa639d8e5b";

    fn one(entry: &str) -> List {
        parse(file(&format!(r#"{{"{K}": {entry}}}"#)).as_bytes()).unwrap()
    }

    #[test]
    fn key_matches_known_value() {
        assert_eq!(key("PPSA01234"), "17c2e30975e7e93c42a3ccaa639d8e5b");
        assert_eq!(key("PPSA01234").len(), 32);
        assert_ne!(key("PPSA01235"), key("PPSA01234"));
    }

    #[test]
    fn parse_reads_fields() {
        let l = one(r#"{"fakelib":"emulators","playgo":false,"notice":"long_first_boot","fw_min":"10.00","fw_max":"13.60"}"#);
        assert_eq!(l.version, 7);
        assert_eq!(l.len(), 1);
        let e = entry_for(&l, "PPSA01234", Some((0x13, 0x60))).unwrap();
        assert_eq!(
            *e,
            Entry {
                fakelib: Some(Mode::Emulators),
                playgo_off: Some(true),
                notice: Some(Notice::LongFirstBoot),
                fw_min: Some((0x10, 0x00)),
                fw_max: Some((0x13, 0x60)),
            }
        );
        // `playgo: true` is the stub on.
        assert_eq!(entry_for(&one(r#"{"playgo":true}"#), "PPSA01234", None).unwrap().playgo_off, Some(false));
        for (name, mode) in [("full", Mode::Full), ("emulators", Mode::Emulators), ("off", Mode::Off)] {
            let l = one(&format!(r#"{{"fakelib":"{name}"}}"#));
            assert_eq!(entry_for(&l, "PPSA01234", None).unwrap().fakelib, Some(mode), "{name}");
        }
        // Another title is not in the list.
        assert_eq!(entry_for(&l, "PPSA01235", Some((0x13, 0x60))), None);
    }

    #[test]
    fn parse_ignores_unknown_values_and_fields() {
        let l = one(r#"{"fakelib":"turbo","notice":"later","colour":"red"}"#);
        assert_eq!(*entry_for(&l, "PPSA01234", None).unwrap(), Entry::default());
        // An unknown top-level field is ignored too.
        let l = parse(br#"{"format": 1, "version": 3, "titles": {}, "comment": "x"}"#).unwrap();
        assert_eq!((l.version, l.len()), (3, 0));
    }

    #[test]
    fn notice_names() {
        assert_eq!(Notice::LongFirstBoot.name(), "long_first_boot");
        assert_eq!(Notice::from_name("long_first_boot"), Some(Notice::LongFirstBoot));
        assert_eq!(Notice::from_name("Long_First_Boot"), None);
        assert_eq!(Notice::from_name(""), None);
    }

    #[test]
    fn parse_rejects_not_json() {
        assert!(parse(b"").is_err());
        assert!(parse(b"{\"format\": 1,").is_err());
        assert!(parse(b"[1, 2]").is_err());
        assert!(parse(b"\xff\xfe").is_err());
    }

    #[test]
    fn parse_rejects_deep_nesting() {
        let deep = format!("{}{}", "[".repeat(10_000), "]".repeat(10_000));
        assert!(parse(file(&deep).as_bytes()).is_err());
        let deep = format!(r#"{{"{K}": {{"x": {}1{}}}}}"#, "[".repeat(10_000), "]".repeat(10_000));
        assert!(parse(file(&deep).as_bytes()).is_err());
    }

    #[test]
    fn parse_rejects_format_2() {
        assert!(parse(br#"{"format": 2, "version": 7, "titles": {}}"#).is_err());
        assert!(parse(br#"{"format": "1", "version": 7, "titles": {}}"#).is_err());
        assert!(parse(br#"{"version": 7, "titles": {}}"#).is_err());
    }

    #[test]
    fn parse_rejects_version_0_or_missing() {
        assert!(parse(br#"{"format": 1, "version": 0, "titles": {}}"#).is_err());
        assert!(parse(br#"{"format": 1, "titles": {}}"#).is_err());
        assert!(parse(br#"{"format": 1, "version": -3, "titles": {}}"#).is_err());
        assert!(parse(br#"{"format": 1, "version": 1.5, "titles": {}}"#).is_err());
        assert!(parse(br#"{"format": 1, "version": "7", "titles": {}}"#).is_err());
    }

    #[test]
    fn parse_rejects_missing_titles() {
        assert!(parse(br#"{"format": 1, "version": 7}"#).is_err());
        assert!(parse(br#"{"format": 1, "version": 7, "titles": []}"#).is_err());
    }

    #[test]
    fn parse_rejects_a_bad_key() {
        let short = &K[..31];
        assert!(parse(file(&format!(r#"{{"{short}": {{}}}}"#)).as_bytes()).is_err());
        let upper = K.to_uppercase();
        assert!(parse(file(&format!(r#"{{"{upper}": {{}}}}"#)).as_bytes()).is_err());
        // A title id instead of its key.
        assert!(parse(file(r#"{"PPSA01234": {}}"#).as_bytes()).is_err());
        let not_hex = format!("{}g", &K[..31]);
        assert!(parse(file(&format!(r#"{{"{not_hex}": {{}}}}"#)).as_bytes()).is_err());
    }

    #[test]
    fn parse_rejects_an_entry_that_is_an_array() {
        assert!(parse(file(&format!(r#"{{"{K}": ["fakelib", "off"]}}"#)).as_bytes()).is_err());
        assert!(parse(file(&format!(r#"{{"{K}": "off"}}"#)).as_bytes()).is_err());
    }

    #[test]
    fn parse_rejects_playgo_no() {
        assert!(parse(file(&format!(r#"{{"{K}": {{"playgo": "no"}}}}"#)).as_bytes()).is_err());
        // Every known field of the wrong JSON type.
        for bad in [r#"{"fakelib": 2}"#, r#"{"notice": true}"#, r#"{"fw_max": 13.6}"#, r#"{"playgo": 0}"#] {
            assert!(parse(file(&format!(r#"{{"{K}": {bad}}}"#)).as_bytes()).is_err(), "{bad}");
        }
    }

    #[test]
    fn parse_rejects_fw_13_6() {
        assert!(parse(file(&format!(r#"{{"{K}": {{"fw_min": "13.6"}}}}"#)).as_bytes()).is_err());
        assert!(parse(file(&format!(r#"{{"{K}": {{"fw_max": "x"}}}}"#)).as_bytes()).is_err());
    }

    #[test]
    fn parse_rejects_4097_titles() {
        let titles = |n: usize| (0..n).map(|i| format!(r#""{i:032x}": {{}}"#)).collect::<Vec<_>>().join(",");
        let ok = file(&format!("{{{}}}", titles(MAX_TITLES)));
        assert!(ok.len() <= MAX_BYTES);
        assert_eq!(parse(ok.as_bytes()).unwrap().len(), MAX_TITLES);
        let over = file(&format!("{{{}}}", titles(MAX_TITLES + 1)));
        assert!(over.len() <= MAX_BYTES, "rejected for its titles, not its size");
        assert!(parse(over.as_bytes()).is_err());
        assert!(parse(file(&format!("{{{}}}", titles(10_000))).as_bytes()).is_err());
    }

    #[test]
    fn parse_rejects_max_bytes_plus_1() {
        let pad = |n: usize| {
            let mut f = file("{}");
            f += &" ".repeat(n - f.len());
            f
        };
        assert!(parse(pad(MAX_BYTES).as_bytes()).is_ok());
        assert!(parse(pad(MAX_BYTES + 1).as_bytes()).is_err());
    }

    #[test]
    fn newer_keeps_bundled_on_tie_or_lower() {
        let bundled = parse(br#"{"format": 1, "version": 7, "titles": {}}"#).unwrap();
        let tie = parse(file(&format!(r#"{{"{K}": {{}}}}"#)).as_bytes()).unwrap();
        let lower = parse(br#"{"format": 1, "version": 6, "titles": {}}"#).unwrap();
        assert!(std::ptr::eq(newer(&bundled, Some(&tie)), &bundled));
        assert!(std::ptr::eq(newer(&bundled, Some(&lower)), &bundled));
        assert!(std::ptr::eq(newer(&bundled, None), &bundled));
    }

    #[test]
    fn newer_takes_higher() {
        let bundled = parse(br#"{"format": 1, "version": 7, "titles": {}}"#).unwrap();
        let higher = parse(br#"{"format": 1, "version": 8, "titles": {}}"#).unwrap();
        assert!(std::ptr::eq(newer(&bundled, Some(&higher)), &higher));
    }

    #[test]
    fn parse_fw_reads_nn_nn() {
        assert_eq!(parse_fw("13.60"), Some((0x13, 0x60)));
        assert_eq!(parse_fw("04.03"), Some((0x04, 0x03)));
        assert_eq!(parse_fw("10.00"), Some((0x10, 0x00)));
        for bad in ["13.6", "x", "", "4.03", "13.600", "13,60", "1a.60", "+3.60", "13.6 ", " 13.60", "١٣.٦٠"] {
            assert_eq!(parse_fw(bad), None, "{bad:?}");
        }
    }

    #[test]
    fn entry_for_fw_bounds() {
        let l = one(r#"{"fakelib":"off","fw_min":"13.60","fw_max":"13.60"}"#);
        assert!(entry_for(&l, "PPSA01234", Some((0x13, 0x60))).is_some());
        assert!(entry_for(&l, "PPSA01234", Some((0x13, 0x50))).is_none());
        assert!(entry_for(&l, "PPSA01234", Some((0x14, 0x00))).is_none());
        // Only a lower bound: every later firmware too.
        let l = one(r#"{"fakelib":"off","fw_min":"13.60"}"#);
        assert!(entry_for(&l, "PPSA01234", Some((0x13, 0x60))).is_some());
        assert!(entry_for(&l, "PPSA01234", Some((0x20, 0x00))).is_some());
        assert!(entry_for(&l, "PPSA01234", Some((0x12, 0x99))).is_none());
        // Only an upper bound.
        let l = one(r#"{"fakelib":"off","fw_max":"10.01"}"#);
        assert!(entry_for(&l, "PPSA01234", Some((0x10, 0x01))).is_some());
        assert!(entry_for(&l, "PPSA01234", Some((0x04, 0x03))).is_some());
        assert!(entry_for(&l, "PPSA01234", Some((0x10, 0x02))).is_none());
    }

    #[test]
    fn entry_for_fw_unknown_skips_bounded() {
        for bounds in [r#""fw_min":"13.60""#, r#""fw_max":"13.60""#, r#""fw_min":"10.00","fw_max":"13.60""#] {
            let l = one(&format!(r#"{{"fakelib":"off",{bounds}}}"#));
            assert!(entry_for(&l, "PPSA01234", None).is_none(), "{bounds}");
        }
    }

    #[test]
    fn entry_for_unbounded_applies_without_fw() {
        let l = one(r#"{"notice":"long_first_boot"}"#);
        assert!(entry_for(&l, "PPSA01234", None).is_some());
        assert!(entry_for(&l, "PPSA01234", Some((0x04, 0x03))).is_some());
    }

    #[test]
    fn source_names() {
        assert_eq!(Source::Default.name(), "default");
        assert_eq!(Source::User.name(), "user");
        assert_eq!(Source::Recommended.name(), "recommended");
    }

    #[test]
    fn effective_default() {
        let e = effective("PPSA01234", &[], &[], &[], &[], None);
        assert_eq!(e, Effective { fakelib: Mode::Full, playgo_off: false, source: Source::Default, notice: None });
        // Another title's choices change nothing for this one.
        let other = list(&["PPSA01235"]);
        let e = effective("PPSA01234", &other, &other, &other, &other, None);
        assert_eq!(e.source, Source::Default);
    }

    #[test]
    fn effective_recommended_off() {
        let entry = Entry { fakelib: Some(Mode::Off), ..Entry::default() };
        let e = effective("PPSA01234", &[], &[], &[], &[], Some(&entry));
        assert_eq!(e, Effective { fakelib: Mode::Off, playgo_off: false, source: Source::Recommended, notice: None });
        // Only PlayGo: the Fakelib mode stays Full.
        let entry = Entry { playgo_off: Some(true), notice: Some(Notice::LongFirstBoot), ..Entry::default() };
        let e = effective("PPSA01234", &[], &[], &[], &[], Some(&entry));
        assert_eq!(e, Effective { fakelib: Mode::Full, playgo_off: true, source: Source::Recommended, notice: Some(Notice::LongFirstBoot) });
    }

    #[test]
    fn effective_notice_only_stays_default() {
        let entry = Entry { notice: Some(Notice::LongFirstBoot), ..Entry::default() };
        let e = effective("PPSA01234", &[], &[], &[], &[], Some(&entry));
        assert_eq!(e, Effective { fakelib: Mode::Full, playgo_off: false, source: Source::Default, notice: Some(Notice::LongFirstBoot) });
    }

    #[test]
    fn effective_manual_wins() {
        let entry = Entry { fakelib: Some(Mode::Off), playgo_off: Some(true), notice: Some(Notice::LongFirstBoot), ..Entry::default() };
        let e = effective("PPSA01234", &[], &[], &[], &list(&["PPSA01234"]), Some(&entry));
        assert_eq!(e, Effective { fakelib: Mode::Full, playgo_off: false, source: Source::User, notice: Some(Notice::LongFirstBoot) });
        // The user's own mode, from the lists.
        let e = effective("PPSA01234", &list(&["PPSA01234"]), &[], &[], &list(&["PPSA01234"]), Some(&entry));
        assert_eq!((e.fakelib, e.source), (Mode::Emulators, Source::User));
    }

    #[test]
    fn effective_old_lists_are_user_choices() {
        // A console upgraded from 1.0.1: the title is in playgo_off only, never in manual_settings.
        let entry = Entry { fakelib: Some(Mode::Off), ..Entry::default() };
        let e = effective("PPSA01234", &[], &[], &list(&["PPSA01234"]), &[], Some(&entry));
        assert_eq!(e, Effective { fakelib: Mode::Full, playgo_off: true, source: Source::User, notice: None });
        let e = effective("PPSA01234", &list(&["PPSA01234"]), &[], &[], &[], Some(&entry));
        assert_eq!((e.fakelib, e.playgo_off, e.source), (Mode::Emulators, false, Source::User));
        let entry = Entry { fakelib: Some(Mode::Emulators), ..Entry::default() };
        let e = effective("PPSA01234", &[], &list(&["PPSA01234"]), &[], &[], Some(&entry));
        assert_eq!((e.fakelib, e.source), (Mode::Off, Source::User));
    }

    #[test]
    fn reset_clears_all_four() {
        let (mut emus, mut off, mut pgo, mut manual) =
            (list(&["PPSA01234", "PPSA00001"]), list(&["PPSA01234"]), list(&["PPSA00002", "PPSA01234"]), list(&["PPSA01234"]));
        assert!(reset("PPSA01234", &mut emus, &mut off, &mut pgo, &mut manual));
        assert_eq!((emus, off, pgo, manual), (list(&["PPSA00001"]), list(&[]), list(&["PPSA00002"]), list(&[])));
        // Only one list names it.
        let (mut emus, mut off, mut pgo, mut manual) = (list(&[]), list(&[]), list(&[]), list(&["PPSA01234"]));
        assert!(reset("PPSA01234", &mut emus, &mut off, &mut pgo, &mut manual));
        assert!(manual.is_empty());
        // Nothing to clear.
        assert!(!reset("PPSA01234", &mut emus, &mut off, &mut pgo, &mut manual));
    }

    #[test]
    fn a_hand_choice_keeps_the_other_recommended_setting() {
        // The list says Fakelib off and no PlayGo stub; the user then sets Fakelib to emulators only.
        let entry = Entry { fakelib: Some(Mode::Off), playgo_off: Some(true), ..Entry::default() };
        let (mut emus, mut off, mut pgo, mut manual) = (list(&[]), list(&[]), list(&[]), list(&["PPSA00001"]));
        let before = effective("PPSA01234", &emus, &off, &pgo, &manual, Some(&entry));
        hand_choice("PPSA01234", before, &mut emus, &mut off, &mut pgo, &mut manual);
        crate::fakelib::set_mode(&mut emus, &mut off, "PPSA01234", Mode::Emulators);
        let after = effective("PPSA01234", &emus, &off, &pgo, &manual, Some(&entry));
        assert_eq!((after.fakelib, after.playgo_off, after.source), (Mode::Emulators, true, Source::User));
        assert_eq!(manual, list(&["PPSA00001", "PPSA01234"]));
        // The other way: the PlayGo stub by hand keeps the list's Fakelib mode.
        let (mut emus, mut off, mut pgo, mut manual) = (list(&[]), list(&[]), list(&[]), list(&[]));
        let before = effective("PPSA01234", &emus, &off, &pgo, &manual, Some(&entry));
        hand_choice("PPSA01234", before, &mut emus, &mut off, &mut pgo, &mut manual);
        crate::titles::set_listed(&mut pgo, "PPSA01234", false);
        let after = effective("PPSA01234", &emus, &off, &pgo, &manual, Some(&entry));
        assert_eq!((after.fakelib, after.playgo_off, after.source), (Mode::Off, false, Source::User));
    }

    #[test]
    fn a_hand_choice_without_a_recommendation_only_marks_the_title() {
        for (emus, pgo) in [(list(&[]), list(&[])), (list(&["PPSA01234"]), list(&["PPSA01234"]))] {
            let (mut emus, mut off, mut pgo, mut manual) = (emus, list(&[]), pgo, list(&[]));
            let (e0, p0) = (emus.clone(), pgo.clone());
            let before = effective("PPSA01234", &emus, &off, &pgo, &manual, None);
            hand_choice("PPSA01234", before, &mut emus, &mut off, &mut pgo, &mut manual);
            assert_eq!((emus, off, pgo, manual), (e0, list(&[]), p0, list(&["PPSA01234"])));
        }
        // Already the user's: the lists stay as they are, whatever the list says.
        let entry = Entry { fakelib: Some(Mode::Off), ..Entry::default() };
        let (mut emus, mut off, mut pgo, mut manual) = (list(&[]), list(&[]), list(&[]), list(&["PPSA01234"]));
        let before = effective("PPSA01234", &emus, &off, &pgo, &manual, Some(&entry));
        hand_choice("PPSA01234", before, &mut emus, &mut off, &mut pgo, &mut manual);
        assert_eq!((emus, off, pgo, manual), (list(&[]), list(&[]), list(&[]), list(&["PPSA01234"])));
    }

    #[test]
    fn bundled_list_parses() {
        let l = parse(include_bytes!("../../../data/recommended.json")).unwrap();
        assert_eq!(l.len(), 2);
        assert!(l.version > 0);
    }
}
