//! A library `.p5z` verified on the console from its game sheet (`POST /api/verify/<TID>`, src/verify.rs): the
//! request's checks, what a verify found (kept in `/data/maxpack/verified.json` by path, shown while the file is
//! the one verified) and the line of it in the game's report, which names no file of the game.
use crate::job::Busy;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

/// How a `.p5z` is verified. `quick` is the engine's quick verify (`maxpack verify --quick`): each volume's and
/// stored file's SHA-256 and each volume's header, read at the speed of the disk without decompressing the packs.
/// `full` is the engine's own verify (`maxpack verify`), which unpacks every packed file and checks each one's
/// SHA-256; on the console it runs the quick one first (src/verify.rs).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Quick,
    Full,
}

impl Kind {
    pub fn name(self) -> &'static str {
        match self {
            Kind::Quick => "quick",
            Kind::Full => "full",
        }
    }
}

/// The body of `POST /api/verify/<TID>`: exactly `quick` or `full`.
pub fn kind(body: &[u8]) -> Option<Kind> {
    match body {
        b"quick" => Some(Kind::Quick),
        b"full" => Some(Kind::Full),
        _ => None,
    }
}

/// What the engine's quick verify did (ps5/verify.c's JSON, `parse_quick`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum QuickOutcome {
    /// It ran to its end: the report says whether everything matched.
    Checked(Quick),
    /// It could not check: not a .p5z, its header, footer or index damaged, cut short, an I/O error.
    Error(String),
    Cancelled,
}

/// The quick verify's report (the engine's mp_qv_report).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Quick {
    pub ok: bool,
    /// Bytes of the records' regions read, of all of them.
    pub bytes: u64,
    pub total: u64,
    /// How many records did not match; `bad` lists the first of them (the engine's MP_QV_MAX_LISTED).
    pub nbad: u64,
    /// The index version: 1 as packed, 2 grown.
    pub version: u32,
    /// The records checked: volumes, and files stored in the .p5z.
    pub volumes: u32,
    pub files: u32,
    pub bad: Vec<QuickBad>,
}

/// A record that did not match.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QuickBad {
    /// "volume" (ampr_zstd-NNN.pak) or "file" (stored in the .p5z: a loose file of the game, an index...).
    pub what: &'static str,
    /// A volume's number (the NNN).
    pub volume: Option<u32>,
    /// The volume's name or the file's path in the .p5z (lossy UTF-8).
    pub name: String,
    /// The SHA-256 the index records, and the one of what is there (None when nothing whole could be hashed),
    /// in lowercase hex.
    pub expected: String,
    pub found: Option<String>,
    /// How it failed: "sha256", "header", "data" or "read".
    pub why: String,
}

#[derive(Deserialize)]
struct QuickRaw {
    ok: bool,
    bytes: u64,
    total: u64,
    nbad: u64,
    version: u32,
    volumes: u32,
    files: u32,
    bad: Vec<QuickBadRaw>,
}

#[derive(Deserialize)]
struct QuickBadRaw {
    kind: u32,
    volume: u32,
    path_hex: String,
    expected: String,
    found: String,
    why: String,
}

/// Bytes from lowercase or uppercase hex; None for an odd length or another character.
fn unhex(s: &str) -> Option<Vec<u8>> {
    let b = s.as_bytes();
    if b.len() % 2 != 0 {
        return None;
    }
    let digit = |c: u8| (c as char).to_digit(16).map(|d| d as u8);
    b.chunks(2).map(|p| Some(digit(p[0])? << 4 | digit(p[1])?)).collect()
}

/// A SHA-256 in hex: 64 hex digits, as lowercase.
fn sha_hex(s: &str) -> Option<String> {
    (s.len() == 64 && s.bytes().all(|c| c.is_ascii_hexdigit())).then(|| s.to_ascii_lowercase())
}

/// ps5/verify.c's JSON of a quick verify: `{"result":0, ok, bytes, total, nbad, nlisted, version, volumes, files,
/// bad:[{kind, volume, path_hex, expected, found, why}]}`, `{"result":-1,"error_hex":...}` or `{"result":-2}`.
/// Paths and the engine's message come hex-encoded, so any bytes survive. Err for anything else.
pub fn parse_quick(json: &str) -> Result<QuickOutcome, String> {
    let v: serde_json::Value = serde_json::from_str(json).map_err(|e| format!("quick verify: bad report: {e}"))?;
    match v.get("result").and_then(|r| r.as_i64()) {
        Some(0) => {}
        Some(-1) => {
            let hex = v.get("error_hex").and_then(|e| e.as_str()).unwrap_or("");
            let msg = unhex(hex).ok_or("quick verify: bad report: error_hex")?;
            let msg = String::from_utf8_lossy(&msg).into_owned();
            return Ok(QuickOutcome::Error(if msg.is_empty() { "no reason given".into() } else { msg }));
        }
        Some(-2) => return Ok(QuickOutcome::Cancelled),
        r => return Err(format!("quick verify: bad report: result {r:?}")),
    }
    let raw: QuickRaw = serde_json::from_value(v).map_err(|e| format!("quick verify: bad report: {e}"))?;
    let mut bad = Vec::with_capacity(raw.bad.len());
    for b in raw.bad {
        let what = match b.kind {
            1 => "volume",
            2 => "file",
            k => return Err(format!("quick verify: bad report: kind {k}")),
        };
        let path = unhex(&b.path_hex).ok_or("quick verify: bad report: path_hex")?;
        let name = match (what, path.is_empty()) {
            ("volume", true) => format!("ampr_zstd-{:03}.pak", b.volume),
            _ => String::from_utf8_lossy(&path).into_owned(),
        };
        let expected = sha_hex(&b.expected).ok_or("quick verify: bad report: expected")?;
        let found = match b.found.as_str() {
            "" => None,
            f => Some(sha_hex(f).ok_or("quick verify: bad report: found")?),
        };
        let why: String = b.why.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '_').take(16).collect();
        bad.push(QuickBad { what, volume: (what == "volume").then_some(b.volume), name, expected, found, why });
    }
    Ok(QuickOutcome::Checked(Quick {
        ok: raw.ok, bytes: raw.bytes, total: raw.total, nbad: raw.nbad, version: raw.version, volumes: raw.volumes,
        files: raw.files, bad,
    }))
}

/// What a quick verify found, as the store keeps it: its records (volumes and stored files) counted as the full
/// verify counts files, and what failed by name.
pub fn quick_record(q: &Quick, size: u64, mtime: i64, at: u64) -> Record {
    let all = u64::from(q.volumes) + u64::from(q.files);
    Record {
        size, mtime, kind: Kind::Quick.name().into(), at, ok: q.ok && q.nbad == 0, files_ok: all.saturating_sub(q.nbad),
        files_bad: q.nbad, bad: q.bad.iter().map(|b| Bad { what: b.what.into(), name: b.name.clone() }).collect(),
    }
}

/// What the request's checks look at.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Facts {
    /// The title is in the library (not a duplicate) and its copy there is a `.p5z`: None when it is not in
    /// the library at all.
    pub p5z: Option<bool>,
    /// Another job of the engine (job::busy).
    pub busy: Option<Busy>,
    /// A game runs, whoever started it (MaxPack's launches and the process list; a walk that failed counts).
    pub game_running: bool,
}

/// Why a verify does not start.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Refusal {
    BadRequest,
    NotFound,
    NotP5z,
    Busy(Busy),
    GameRunning,
}

impl Refusal {
    pub fn status(self) -> u16 {
        match self {
            Refusal::BadRequest => 400,
            Refusal::NotFound => 404,
            Refusal::NotP5z | Refusal::Busy(_) | Refusal::GameRunning => 409,
        }
    }

    pub fn code(self) -> &'static str {
        match self {
            Refusal::BadRequest => "bad_request",
            Refusal::NotFound => "not_found",
            Refusal::NotP5z => "not_p5z",
            Refusal::Busy(b) => b.code(),
            Refusal::GameRunning => "game_running",
        }
    }

    pub fn detail(self) -> &'static str {
        match self {
            Refusal::BadRequest => "expected the body quick or full",
            Refusal::NotFound => "not a game of the library",
            Refusal::NotP5z => "only a .p5z is verified on the console",
            Refusal::Busy(b) => b.detail(),
            Refusal::GameRunning => "a game is running: close it first",
        }
    }
}

/// The checks, in this order: in the library, a `.p5z`, no other job of the engine, no game running.
pub fn gate(f: &Facts) -> Result<(), Refusal> {
    match f.p5z {
        None => Err(Refusal::NotFound),
        Some(false) => Err(Refusal::NotP5z),
        Some(true) if f.busy.is_some() => Err(Refusal::Busy(f.busy.unwrap_or(Busy::Install))),
        Some(true) if f.game_running => Err(Refusal::GameRunning),
        Some(true) => Ok(()),
    }
}

/// Something a verify found damaged.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Bad {
    /// "volume" (an `ampr_zstd-NNN.pak` of the packs) or "file" (a loose file, or a packed one).
    pub what: String,
    /// The volume's name or the file's path in the game.
    pub name: String,
}

/// What a verify of a `.p5z` found.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Record {
    /// The size and mtime (seconds) of the file verified: the record is shown only while both still match.
    pub size: u64,
    pub mtime: i64,
    /// Kind::name.
    pub kind: String,
    /// When it ended, in Unix seconds by the console's clock (which may be years behind).
    pub at: u64,
    pub ok: bool,
    pub files_ok: u64,
    pub files_bad: u64,
    /// What failed, when the verify names it (the full verify only counts, see DESIGN.md).
    #[serde(default)]
    pub bad: Vec<Bad>,
}

/// The records kept, by the `.p5z`'s path.
pub type Store = BTreeMap<String, Record>;

/// At most this many records are kept: the oldest go first.
pub const MAX_RECORDS: usize = 500;

/// The kept records from `verified.json`; an unreadable file is none (a verify is simply asked again).
pub fn parse(text: &str) -> Store {
    serde_json::from_str(text).unwrap_or_default()
}

/// `verified.json`'s text.
pub fn render(s: &Store) -> String {
    serde_json::to_string(s).unwrap_or_else(|_| "{}".into())
}

/// Records `rec` for `path` (replacing an older one), keeping MAX_RECORDS at most: the oldest by `at` go.
pub fn put(s: &mut Store, path: &str, rec: Record) {
    s.insert(path.to_string(), rec);
    while s.len() > MAX_RECORDS {
        let Some(oldest) = s.iter().min_by_key(|(_, r)| r.at).map(|(p, _)| p.clone()) else { break };
        s.remove(&oldest);
    }
}

/// The record of `path` while the file is the one verified (same size and mtime).
pub fn current<'a>(s: &'a Store, path: &str, size: u64, mtime: i64) -> Option<&'a Record> {
    s.get(path).filter(|r| r.size == size && r.mtime == mtime)
}

/// A volume's name as the packs name them (letters, digits, `_`, `-` and `.`, as the engine's sums allow), so
/// the report can quote it.
fn plain_volume(name: &str) -> bool {
    !name.is_empty() && name.len() <= 64 && !name.starts_with('.')
        && name.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || matches!(b, b'_' | b'-' | b'.'))
}

/// The game report's line (after "Verified on the console: "): never, or the kind, the result, the date and
/// the counts. It names no file of the game: only volume names, which are the packs' own (`ampr_zstd-000.pak`),
/// and how many other files failed.
pub fn report_line(rec: Option<&Record>) -> String {
    let Some(r) = rec else { return "never".into() };
    let when = crate::report::utc(r.at);
    let total = r.files_ok + r.files_bad;
    // The quick verify counts the .p5z's records: its volumes and the files stored in it.
    let unit = if r.kind == Kind::Quick.name() { "volumes and files" } else { "files" };
    if r.ok {
        return format!("{}, OK on {when} ({total} {unit})", r.kind);
    }
    let mut line = format!("{}, damaged on {when}: {} of {total} {unit} fail", r.kind, r.files_bad);
    let volumes: Vec<&Bad> = r.bad.iter().filter(|b| b.what == "volume").collect();
    let named: Vec<&str> = volumes.iter().map(|b| b.name.as_str()).filter(|n| plain_volume(n)).collect();
    let mut parts = Vec::new();
    if !volumes.is_empty() {
        let more = volumes.len() - named.len();
        parts.push(match (named.is_empty(), more) {
            (true, n) => format!("volumes: {n}"),
            (false, 0) => format!("volumes: {}", named.join(", ")),
            (false, n) => format!("volumes: {} (+{n} more)", named.join(", ")),
        });
    }
    let files = r.bad.len() - volumes.len();
    if files > 0 {
        parts.push(format!("files: {files}"));
    }
    for p in parts {
        line += "; ";
        line += &p;
    }
    line
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rec(at: u64, ok: bool) -> Record {
        Record { size: 1000, mtime: 7, kind: "full".into(), at, ok, files_ok: if ok { 12 } else { 10 }, files_bad: if ok { 0 } else { 2 },
            bad: Vec::new() }
    }

    #[test]
    fn the_body_names_the_kind() {
        assert_eq!(kind(b"full"), Some(Kind::Full));
        assert_eq!(kind(b"quick"), Some(Kind::Quick));
        assert_eq!(Kind::Full.name(), "full");
        assert_eq!(Kind::Quick.name(), "quick");
        for b in [&b""[..], b"FULL", b"full\n", b"quick ", b"{}", b"fast"] {
            assert_eq!(kind(b), None, "{b:?}");
        }
    }

    const SHA_A: &str = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const SHA_B: &str = "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";

    /// ps5/verify.c's JSON for one report, `bad` as given.
    fn quick_json(ok: bool, nbad: u64, bad: &str) -> String {
        format!(r#"{{"result":0,"ok":{ok},"bytes":900,"total":900,"nbad":{nbad},"nlisted":{nbad},"version":2,"volumes":3,"files":9,"bad":[{bad}]}}"#)
    }

    #[test]
    fn a_quick_report_reads() {
        assert_eq!(parse_quick(&quick_json(true, 0, "")), Ok(QuickOutcome::Checked(Quick {
            ok: true, bytes: 900, total: 900, nbad: 0, version: 2, volumes: 3, files: 9, bad: vec![],
        })));
        // A volume (its name from the engine), a loose file whose path is not UTF-8, a file that did not
        // decompress (no sum found) and a volume without a path (named from its id).
        let bad = format!(concat!(
            r#"{{"kind":1,"volume":3,"path_hex":"{}","expected":"{a}","found":"{b}","why":"sha256"}},"#,
            r#"{{"kind":2,"volume":0,"path_hex":"7361ff2f6562","expected":"{a}","found":"{b}","why":"sha256"}},"#,
            r#"{{"kind":2,"volume":0,"path_hex":"782e62696e","expected":"{a}","found":"","why":"data"}},"#,
            r#"{{"kind":1,"volume":12,"path_hex":"","expected":"{a}","found":"","why":"read"}}"#),
            "616d70725f7a7374642d3030332e70616b", a = SHA_A, b = SHA_B);
        let Ok(QuickOutcome::Checked(q)) = parse_quick(&quick_json(false, 4, &bad)) else { panic!("not read") };
        assert!(!q.ok);
        assert_eq!(q.bad, vec![
            QuickBad { what: "volume", volume: Some(3), name: "ampr_zstd-003.pak".into(), expected: SHA_A.into(), found: Some(SHA_B.into()), why: "sha256".into() },
            QuickBad { what: "file", volume: None, name: "sa\u{fffd}/eb".into(), expected: SHA_A.into(), found: Some(SHA_B.into()), why: "sha256".into() },
            QuickBad { what: "file", volume: None, name: "x.bin".into(), expected: SHA_A.into(), found: None, why: "data".into() },
            QuickBad { what: "volume", volume: Some(12), name: "ampr_zstd-012.pak".into(), expected: SHA_A.into(), found: None, why: "read".into() },
        ]);
    }

    #[test]
    fn a_quick_error_or_cancel_reads() {
        // The engine's message comes hex-encoded: any bytes of a path in it survive.
        assert_eq!(parse_quick(r#"{"result":-1,"error_hex":"6e6f742061202e70357a3a20c3a9"}"#),
            Ok(QuickOutcome::Error("not a .p5z: \u{e9}".into())));
        assert_eq!(parse_quick(r#"{"result":-1,"error_hex":""}"#), Ok(QuickOutcome::Error("no reason given".into())));
        assert_eq!(parse_quick(r#"{"result":-2}"#), Ok(QuickOutcome::Cancelled));
        for junk in ["", "{}", "not json", r#"{"result":7}"#, r#"{"result":-1,"error_hex":"zz"}"#,
            &quick_json(true, 0, r#"{"kind":3,"volume":0,"path_hex":"","expected":"","found":"","why":"x"}"#),
            &quick_json(true, 0, r#"{"kind":2,"volume":0,"path_hex":"0","expected":"","found":"","why":"x"}"#),
            &quick_json(true, 0, r#"{"kind":2,"volume":0,"path_hex":"","expected":"xyz","found":"","why":"x"}"#)] {
            assert!(parse_quick(junk).is_err(), "{junk}");
        }
        // A why the engine never gives is kept to letters, digits and _.
        let q = quick_json(false, 1, &format!(r#"{{"kind":2,"volume":0,"path_hex":"61","expected":"{SHA_A}","found":"","why":"r<e>ad"}}"#));
        let Ok(QuickOutcome::Checked(q)) = parse_quick(&q) else { panic!() };
        assert_eq!(q.bad[0].why, "read");
    }

    #[test]
    fn a_quick_verify_is_recorded() {
        let bad = format!(r#"{{"kind":1,"volume":3,"path_hex":"616d70725f7a7374642d3030332e70616b","expected":"{SHA_A}","found":"{SHA_B}","why":"sha256"}}"#);
        let Ok(QuickOutcome::Checked(q)) = parse_quick(&quick_json(false, 2, &bad)) else { panic!() };
        let r = quick_record(&q, 1000, 7, 1_795_166_239);
        assert_eq!(r, Record { size: 1000, mtime: 7, kind: "quick".into(), at: 1_795_166_239, ok: false, files_ok: 10, files_bad: 2,
            bad: vec![Bad { what: "volume".into(), name: "ampr_zstd-003.pak".into() }] });
        assert_eq!(report_line(Some(&r)),
            "quick, damaged on 2026-11-20 09:17:19Z: 2 of 12 volumes and files fail; volumes: ampr_zstd-003.pak");
        let Ok(QuickOutcome::Checked(q)) = parse_quick(&quick_json(true, 0, "")) else { panic!() };
        let r = quick_record(&q, 1000, 7, 1_795_166_239);
        assert!(r.ok && r.files_ok == 12 && r.files_bad == 0 && r.bad.is_empty());
        assert_eq!(report_line(Some(&r)), "quick, OK on 2026-11-20 09:17:19Z (12 volumes and files)");
    }

    #[test]
    fn gate_order() {
        let ok = Facts { p5z: Some(true), busy: None, game_running: false };
        assert_eq!(gate(&ok), Ok(()));
        assert_eq!(gate(&Facts { p5z: None, busy: Some(Busy::Install), game_running: true }), Err(Refusal::NotFound));
        assert_eq!(gate(&Facts { p5z: Some(false), busy: Some(Busy::Install), game_running: true }), Err(Refusal::NotP5z));
        assert_eq!(gate(&Facts { busy: Some(Busy::Receive), game_running: true, ..ok }), Err(Refusal::Busy(Busy::Receive)));
        assert_eq!(gate(&Facts { game_running: true, ..ok }), Err(Refusal::GameRunning));
    }

    #[test]
    fn refusals_answer() {
        assert_eq!((Refusal::BadRequest.status(), Refusal::BadRequest.code()), (400, "bad_request"));
        assert_eq!((Refusal::NotFound.status(), Refusal::NotFound.code()), (404, "not_found"));
        assert_eq!((Refusal::NotP5z.status(), Refusal::NotP5z.code()), (409, "not_p5z"));
        assert_eq!((Refusal::GameRunning.status(), Refusal::GameRunning.code()), (409, "game_running"));
        let b = Refusal::Busy(Busy::Verify);
        assert_eq!((b.status(), b.code()), (409, "verify_running"));
        assert_eq!(b.detail(), Busy::Verify.detail());
        assert!(!Refusal::NotP5z.detail().is_empty());
    }

    #[test]
    fn store_round_trip_and_junk() {
        let mut s = Store::new();
        put(&mut s, "/data/homebrew/A.p5z", rec(5, true));
        let mut bad = rec(6, false);
        bad.bad = vec![Bad { what: "volume".into(), name: "ampr_zstd-003.pak".into() }];
        put(&mut s, "/mnt/usb0/homebrew/B.p5z", bad);
        assert_eq!(parse(&render(&s)), s);
        assert!(parse("").is_empty());
        assert!(parse("not json").is_empty());
        assert!(parse("[1,2]").is_empty());
        // A record without `bad` (or with fields it does not know) still reads.
        let one = r#"{"/x.p5z":{"size":1,"mtime":2,"kind":"full","at":3,"ok":true,"files_ok":4,"files_bad":0,"later":1}}"#;
        assert_eq!(parse(one)["/x.p5z"].files_ok, 4);
    }

    #[test]
    fn store_keeps_the_newest() {
        let mut s = Store::new();
        for i in 0..(MAX_RECORDS as u64 + 5) {
            put(&mut s, &format!("/data/homebrew/{i}.p5z"), rec(1000 + i, true));
        }
        assert_eq!(s.len(), MAX_RECORDS);
        assert!(!s.contains_key("/data/homebrew/0.p5z"));
        assert!(!s.contains_key("/data/homebrew/4.p5z"));
        assert!(s.contains_key("/data/homebrew/5.p5z"));
        // The same path again replaces its record, it is not a new one.
        put(&mut s, "/data/homebrew/5.p5z", rec(9999, false));
        assert_eq!(s.len(), MAX_RECORDS);
        assert!(!s["/data/homebrew/5.p5z"].ok);
    }

    #[test]
    fn a_record_shows_while_the_file_is_the_same() {
        let mut s = Store::new();
        put(&mut s, "/a.p5z", rec(5, true));
        assert!(current(&s, "/a.p5z", 1000, 7).is_some());
        assert!(current(&s, "/a.p5z", 1001, 7).is_none());
        assert!(current(&s, "/a.p5z", 1000, 8).is_none());
        assert!(current(&s, "/b.p5z", 1000, 7).is_none());
    }

    #[test]
    fn the_report_line_names_no_file() {
        assert_eq!(report_line(None), "never");
        assert_eq!(report_line(Some(&rec(1_795_166_239, true))), "full, OK on 2026-11-20 09:17:19Z (12 files)");
        let mut bad = rec(1_795_166_239, false);
        assert_eq!(report_line(Some(&bad)), "full, damaged on 2026-11-20 09:17:19Z: 2 of 12 files fail");
        bad.bad = vec![
            Bad { what: "volume".into(), name: "ampr_zstd-003.pak".into() },
            Bad { what: "file".into(), name: "Game Name/Content/secret.pak".into() },
            Bad { what: "volume".into(), name: "../Game Name.pak".into() },
        ];
        let line = report_line(Some(&bad));
        assert_eq!(line, "full, damaged on 2026-11-20 09:17:19Z: 2 of 12 files fail; volumes: ampr_zstd-003.pak (+1 more); files: 1");
        assert!(!line.contains("Game Name"));
    }
}
