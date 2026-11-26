//! A library `.p5z` verified on the console from its game sheet (`POST /api/verify/<TID>`, src/verify.rs): the
//! request's checks, what a verify found (kept in `/data/maxpack/verified.json` by path, shown while the file is
//! the one verified) and the line of it in the game's report, which names no file of the game.
use crate::job::Busy;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

/// How a `.p5z` is verified: `full` is the engine's own verify (`maxpack verify` on the PC), which unpacks
/// every packed file and checks each one's SHA-256.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Full,
}

impl Kind {
    pub fn name(self) -> &'static str {
        match self {
            Kind::Full => "full",
        }
    }
}

/// The body of `POST /api/verify/<TID>`: exactly `full`.
pub fn kind(body: &[u8]) -> Option<Kind> {
    match body {
        b"full" => Some(Kind::Full),
        _ => None,
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
            Refusal::BadRequest => "expected the body full",
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
    if r.ok {
        return format!("{}, OK on {when} ({total} files)", r.kind);
    }
    let mut line = format!("{}, damaged on {when}: {} of {total} files fail", r.kind, r.files_bad);
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
        assert_eq!(Kind::Full.name(), "full");
        for b in [&b""[..], b"FULL", b"full\n", b"quick ", b"{}", b"fast"] {
            assert_eq!(kind(b), None, "{b:?}");
        }
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
