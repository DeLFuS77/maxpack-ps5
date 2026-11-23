//! Forwarding the page's installer requests (`/api/inst/<x>`) to the phase-2 installer (the MaxPack
//! engine in C), which answers them as `/api/<x>` (its own API, vendor/maxpack src/web/mp_api.h),
//! and what the payload does around them as the mounter: no uninstall or replace while a game runs,
//! and the home entry of an uninstalled game goes with it.

use serde_json::Value;

/// The installer's route for a request target (path and query) of the payload's server:
/// `/api/inst/<x>[?query]` -> `/api/<x>[?query]` when `<x>` is a route the page uses with that
/// method (GET `state`, `icon`; POST `scan`, `probe`, `install`, `pause`, `verify`, `uninstall`,
/// `discard`). None for anything else: it never reaches the installer (its `/api/quit` included:
/// quitting is the payload's own `/api/quit`).
pub fn installer_path(method: &str, target: &str) -> Option<String> {
    let rest = target.strip_prefix("/api/inst/")?;
    let (name, query) = match rest.split_once('?') {
        Some((name, query)) => (name, Some(query)),
        None => (rest, None),
    };
    let known = match method {
        "GET" => matches!(name, "state" | "icon"),
        "POST" => matches!(name, "scan" | "probe" | "install" | "pause" | "verify" | "uninstall" | "discard"),
        _ => false,
    };
    known.then(|| match query {
        Some(q) => format!("/api/{name}?{q}"),
        None => format!("/api/{name}"),
    })
}

/// The value of header `name` (any case) in an HTTP request head (request line and headers),
/// trimmed; the first one when it repeats. None when absent.
pub fn header<'a>(head: &'a str, name: &str) -> Option<&'a str> {
    head.lines().skip(1).find_map(|l| {
        let (k, v) = l.split_once(':')?;
        k.trim().eq_ignore_ascii_case(name).then(|| v.trim())
    })
}

/// An installer request (`installer_path`'s route, query and all) that deletes or replaces a copy
/// of a game: an uninstall, or an install whose `replace` lists copies to delete. web.rs refuses
/// these while a game runs. Fails safe: an install body that is not JSON, or a `replace` that is
/// anything but absent, null or an empty array, counts as replacing.
pub fn needs_no_game(method: &str, route: &str, body: &[u8]) -> bool {
    if method != "POST" {
        return false;
    }
    match route.split('?').next().unwrap_or(route) {
        "/api/uninstall" => true,
        "/api/install" => match serde_json::from_slice::<Value>(body) {
            Ok(v) => match v.get("replace") {
                None | Some(Value::Null) => false,
                Some(Value::Array(a)) => !a.is_empty(),
                Some(_) => true,
            },
            Err(_) => true,
        },
        _ => false,
    }
}

/// Whether a game that must not be deleted under is running: MaxPack's own `paused` (a title it
/// tracks runs), or `walk`, what the process walk found for the title being uninstalled
/// (Some(runs), or None when the walk itself failed). Fails safe: a failed walk blocks.
pub fn game_blocks(paused: bool, walk: Option<bool>) -> bool {
    paused || walk != Some(false)
}

/// A JSON object body whose top level has the key `title_id` more than once. serde_json keeps the
/// last of a repeated key and the engine's mp_json_get the first, so such a body would name one
/// title to the gate and another to the engine: web.rs refuses it (400 bad_json). Keys inside
/// nested values or strings do not count. Not an object, or not JSON: false (the engine answers).
pub fn title_id_repeats(body: &[u8]) -> bool {
    use serde::de::{Deserializer, IgnoredAny, MapAccess, Visitor};
    struct Count(usize);
    impl<'de> Visitor<'de> for Count {
        type Value = usize;
        fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
            f.write_str("an object")
        }
        fn visit_map<A: MapAccess<'de>>(mut self, mut m: A) -> Result<usize, A::Error> {
            while let Some(k) = m.next_key::<String>()? {
                m.next_value::<IgnoredAny>()?;
                if k == "title_id" {
                    self.0 += 1;
                }
            }
            Ok(self.0)
        }
    }
    let mut de = serde_json::Deserializer::from_slice(body);
    de.deserialize_map(Count(0)).is_ok_and(|n| n > 1)
}

/// The title id of an uninstall request's body (`{"title_id": "PPSA01234", "confirm": true}`), when
/// it is a game title id.
/// None when `title_id` repeats (title_id_repeats): the engine reads the first, serde the last.
pub fn uninstall_title(body: &[u8]) -> Option<String> {
    if title_id_repeats(body) {
        return None;
    }
    let v: Value = serde_json::from_slice(body).ok()?;
    let tid = v.get("title_id")?.as_str()?;
    crate::images::is_title_id(tid).then(|| tid.to_string())
}

/// The installer accepted the request: its answer is `{"ok": true, ...}`.
pub fn answer_ok(body: &[u8]) -> bool {
    serde_json::from_slice::<Value>(body).is_ok_and(|v| v.get("ok") == Some(&Value::Bool(true)))
}

/// Where an accepted uninstall of a title stands, seen in the installer's `/api/state`.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Uninstall {
    /// Queued or running (or the state could not be read): look again.
    Pending,
    /// Its record is gone (or the installer says done): the game was uninstalled.
    Done,
    /// The job is over and the record is still listed: it failed.
    Failed,
}

/// Follows one uninstall through successive `/api/state` answers. The engine reads its records
/// before it locks the job and the queue, so one answer can list the record of a job that ended
/// just after: "over with the record listed" counts as Failed only when seen twice in a row (or
/// when `last` says that this uninstall failed).
#[derive(Debug, Default)]
pub struct UninstallWatch {
    ended_once: bool,
}

impl UninstallWatch {
    pub fn see(&mut self, state: &[u8], tid: &str) -> Uninstall {
        let ended = std::mem::take(&mut self.ended_once);
        let Ok(v) = serde_json::from_slice::<Value>(state) else { return Uninstall::Pending };
        let Some(installed) = v.get("installed").and_then(Value::as_array) else { return Uninstall::Pending };
        let is = |o: &Value, kind: &str| {
            o.get("kind").and_then(Value::as_str) == Some(kind) && o.get("title_id").and_then(Value::as_str) == Some(tid)
        };
        if v.get("job").is_some_and(|j| is(j, "uninstall")) {
            return Uninstall::Pending;
        }
        let last = v.get("last").filter(|l| is(l, "uninstall"));
        let last_code = last.and_then(|l| l.get("code")).and_then(Value::as_str);
        if last_code == Some("done") || !installed.iter().any(|r| r.get("title_id").and_then(Value::as_str) == Some(tid)) {
            return Uninstall::Done;
        }
        // Not running and nothing queued: this uninstall is over, with the record still there.
        if v.get("queue").and_then(Value::as_u64) == Some(0) {
            if last_code.is_some() || ended {
                return Uninstall::Failed;
            }
            self.ended_once = true;
        }
        Uninstall::Pending
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn installer_path_rewrites_the_page_routes() {
        assert_eq!(installer_path("GET", "/api/inst/state").as_deref(), Some("/api/state"));
        assert_eq!(installer_path("GET", "/api/inst/icon?p5z=%2Fmnt%2Fusb0%2Fa.p5z").as_deref(),
            Some("/api/icon?p5z=%2Fmnt%2Fusb0%2Fa.p5z"));
        for r in ["scan", "probe", "install", "pause", "verify", "uninstall", "discard"] {
            assert_eq!(installer_path("POST", &format!("/api/inst/{r}")), Some(format!("/api/{r}")), "{r}");
        }
        // A query on a POST is the installer's business (it ignores it).
        assert_eq!(installer_path("POST", "/api/inst/scan?x=1").as_deref(), Some("/api/scan?x=1"));
    }

    #[test]
    fn installer_path_refuses_other_methods_and_routes() {
        // The method of each route only.
        assert_eq!(installer_path("POST", "/api/inst/state"), None);
        assert_eq!(installer_path("GET", "/api/inst/install"), None);
        assert_eq!(installer_path("PUT", "/api/inst/scan"), None);
        assert_eq!(installer_path("get", "/api/inst/state"), None);
        // Quitting is the payload's own /api/quit; the installer's page is not served.
        assert_eq!(installer_path("POST", "/api/inst/quit"), None);
        assert_eq!(installer_path("GET", "/api/inst/"), None);
        assert_eq!(installer_path("GET", "/api/inst"), None);
        // Exactly one name after the prefix.
        for t in ["/api/inst/state/x", "/api/inst//state", "/api/inst/../state", "/api/inst/state%2F",
            "/api/installer/state", "/api/inststate", "/inst/state", "/api/state", "api/inst/state",
            "/API/INST/state", "/api/inst/State", "/api/inst/state#x"] {
            assert_eq!(installer_path("GET", t), None, "{t}");
        }
    }

    #[test]
    fn header_by_name_any_case() {
        let head = "POST /api/inst/install HTTP/1.1\r\nHost: 192.168.1.20:8095\r\n\
            content-type:  application/json; charset=utf-8 \r\nContent-Length: 2\r\nX-Empty:\r\nHost: second";
        assert_eq!(header(head, "Host"), Some("192.168.1.20:8095"));
        assert_eq!(header(head, "host"), Some("192.168.1.20:8095"));
        assert_eq!(header(head, "Content-Type"), Some("application/json; charset=utf-8"));
        assert_eq!(header(head, "content-length"), Some("2"));
        assert_eq!(header(head, "X-Empty"), Some(""));
        assert_eq!(header(head, "Origin"), None);
        // The request line is not a header, and a name must match whole.
        assert_eq!(header(head, "POST /api/inst/install HTTP/1.1\r\nHost"), None);
        assert_eq!(header(head, "Content"), None);
        assert_eq!(header("GET / HTTP/1.1", "Host"), None);
        // LF-only lines too.
        assert_eq!(header("GET / HTTP/1.1\nHost: 127.0.0.1", "Host"), Some("127.0.0.1"));
    }
    #[test]
    fn uninstall_and_replace_need_no_game() {
        // Every uninstall, whatever its body.
        assert!(needs_no_game("POST", "/api/uninstall", br#"{"title_id":"PPSA01234","confirm":true}"#));
        assert!(needs_no_game("POST", "/api/uninstall", b"not json"));
        assert!(needs_no_game("POST", "/api/uninstall?x=1", b"{}"));
        // An install that replaces other copies.
        assert!(needs_no_game("POST", "/api/install", br#"{"p5z":"/mnt/usb0/a.p5z","dest":"/data","replace":["/data/homebrew/PPSA01234"]}"#));
        assert!(needs_no_game("POST", "/api/install?y", br#"{"replace":[""]}"#));
        // Fails safe: a body or a replace the engine might read otherwise.
        for b in [&b"{"[..], b"", br#"{"replace":"/data/homebrew/X"}"#, br#"{"replace":{"a":1}}"#, br#"{"replace":1}"#] {
            assert!(needs_no_game("POST", "/api/install", b), "{}", String::from_utf8_lossy(b));
        }
        // An install that replaces nothing, and every other route.
        for b in [&br#"{"p5z":"/mnt/usb0/a.p5z","dest":"/data"}"#[..], br#"{"p5z":"/mnt/usb0/replace.p5z","replace":[]}"#,
            br#"{"replace":null}"#, br#"[]"#] {
            assert!(!needs_no_game("POST", "/api/install", b), "{}", String::from_utf8_lossy(b));
        }
        for r in ["/api/scan", "/api/probe", "/api/pause", "/api/verify", "/api/discard", "/api/state", "/api/icon",
            "/api/uninstalled", "/api/installx"] {
            assert!(!needs_no_game("POST", r, br#"{"replace":["x"]}"#), "{r}");
        }
        assert!(!needs_no_game("GET", "/api/uninstall", b""));
        assert!(!needs_no_game("GET", "/api/install", br#"{"replace":["x"]}"#));
    }

    #[test]
    fn game_blocks_table() {
        // (paused, walk, blocks): walk is None when the process walk failed.
        for (paused, walk, want) in [
            (true, Some(false), true),
            (true, Some(true), true),
            (true, None, true),
            (false, Some(true), true),
            (false, None, true),
            (false, Some(false), false),
        ] {
            assert_eq!(game_blocks(paused, walk), want, "paused={paused} walk={walk:?}");
        }
    }

    #[test]
    fn title_id_repeats_table() {
        for (b, want) in [
            (&br#"{"title_id":"PPSA01234","confirm":true}"#[..], false),
            (br#"{"title_id":"PPSA01234","title_id":"CUSA00001","confirm":true}"#, true),
            (br#"{"title_id":"A","x":1,"title_id":"B"}"#, true),
            (br#"{"note":"has \"title_id\" inside","title_id":"PPSA01234"}"#, false),
            (br#"{"x":{"title_id":"A"},"title_id":"PPSA01234"}"#, false),
            (br#"{"x":"title_id","title_id":"PPSA01234"}"#, false),
            (b"x", false),
            (br#"["title_id","title_id"]"#, false),
        ] {
            assert_eq!(title_id_repeats(b), want, "{}", String::from_utf8_lossy(b));
        }
        let dup = br#"{"title_id":"PPSA01234","title_id":"CUSA00001","confirm":true}"#;
        assert_eq!(uninstall_title(dup), None);
        assert_eq!(uninstall_title(br#"{"title_id":"PPSA01234","confirm":true}"#).as_deref(), Some("PPSA01234"));
    }

    #[test]
    fn uninstall_title_and_answer() {
        assert_eq!(uninstall_title(br#"{"title_id":"PPSA01234","confirm":true}"#).as_deref(), Some("PPSA01234"));
        assert_eq!(uninstall_title(br#"{"confirm":true,"title_id":"CUSA00001"}"#).as_deref(), Some("CUSA00001"));
        for b in [&br#"{"title_id":"NPXS20001"}"#[..], br#"{"title_id":"ppsa01234"}"#, br#"{"title_id":"PPSA0123"}"#,
            br#"{"title_id":"../../x"}"#, br#"{"title_id":1}"#, br#"{}"#, b"x", br#"["PPSA01234"]"#] {
            assert_eq!(uninstall_title(b), None, "{}", String::from_utf8_lossy(b));
        }
        assert!(answer_ok(br#"{"ok":true}"#));
        assert!(answer_ok(br#"{"ok":true,"found":{}}"#));
        for b in [&br#"{"ok":false,"code":"needs_confirm","detail":"/data/homebrew/X"}"#[..], br#"{"ok":"true"}"#, br#"{}"#,
            b"", b"ok"] {
            assert!(!answer_ok(b), "{}", String::from_utf8_lossy(b));
        }
    }

    /// An installer `/api/state` answer: `installed` title ids, `job` (kind, title id), `queue`,
    /// `last` (kind, title id, code).
    fn st(installed: &[&str], job: Option<(&str, &str)>, queue: u64, last: Option<(&str, &str, &str)>) -> Vec<u8> {
        let inst: Vec<Value> =
            installed.iter().map(|t| serde_json::json!({"title_id": t, "dir": format!("/data/homebrew/{t}")})).collect();
        serde_json::json!({
            "ok": true, "installed": inst, "pending": [], "found": [],
            "job": job.map(|(k, t)| serde_json::json!({"kind": k, "title_id": t, "name": "", "stage": "", "done": 0,
                "total": 0, "paused": false})),
            "queue": queue,
            "last": last.map(|(k, t, c)| serde_json::json!({"kind": k, "title_id": t, "code": c, "detail": ""})),
        }).to_string().into_bytes()
    }

    #[test]
    fn an_uninstall_followed_to_its_end() {
        let t = "PPSA01234";
        let mut w = UninstallWatch::default();
        // Queued behind an install, then running, then its record is gone.
        assert_eq!(w.see(&st(&[t, "PPSA05678"], Some(("install", "PPSA05678")), 1, None), t), Uninstall::Pending);
        assert_eq!(w.see(&st(&[t], Some(("uninstall", t)), 0, Some(("install", "PPSA05678", "done"))), t), Uninstall::Pending);
        assert_eq!(w.see(&st(&[], None, 0, Some(("uninstall", t, "done"))), t), Uninstall::Done);
        // The record already gone while another job runs: done.
        let mut w = UninstallWatch::default();
        assert_eq!(w.see(&st(&["PPSA05678"], Some(("verify", "PPSA05678")), 0, Some(("verify", "PPSA05678", "done"))), t),
            Uninstall::Done);
        // `last` says done even if a stale record is listed.
        let mut w = UninstallWatch::default();
        assert_eq!(w.see(&st(&[t], None, 0, Some(("uninstall", t, "done"))), t), Uninstall::Done);
    }

    #[test]
    fn a_failed_uninstall_is_seen() {
        let t = "PPSA01234";
        // The engine says so.
        let mut w = UninstallWatch::default();
        assert_eq!(w.see(&st(&[t], None, 0, Some(("uninstall", t, "io_error"))), t), Uninstall::Failed);
        // Over, record still listed, `last` overwritten by a later job: Failed on the second look.
        let mut w = UninstallWatch::default();
        let over = st(&[t], Some(("install", "PPSA05678")), 0, Some(("verify", "PPSA05678", "done")));
        assert_eq!(w.see(&over, t), Uninstall::Pending);
        assert_eq!(w.see(&over, t), Uninstall::Failed);
        // One such look between two "running" ones does not count (the engine's records-then-lock race).
        let mut w = UninstallWatch::default();
        assert_eq!(w.see(&st(&[t], None, 0, None), t), Uninstall::Pending);
        assert_eq!(w.see(&st(&[t], Some(("uninstall", t)), 0, None), t), Uninstall::Pending);
        assert_eq!(w.see(&st(&[t], None, 0, None), t), Uninstall::Pending);
        assert_eq!(w.see(&st(&[], None, 0, None), t), Uninstall::Done);
        // A stale failure of an earlier try while this one is still queued: pending.
        let mut w = UninstallWatch::default();
        assert_eq!(w.see(&st(&[t], Some(("install", "PPSA05678")), 1, Some(("uninstall", t, "io_error"))), t),
            Uninstall::Pending);
    }

    #[test]
    fn an_unreadable_state_is_pending() {
        let t = "PPSA01234";
        let mut w = UninstallWatch::default();
        for b in [&b""[..], b"<html>", br#"{"ok":false,"code":"starting","detail":""}"#, br#"{"ok":true}"#, br#"[]"#] {
            assert_eq!(w.see(b, t), Uninstall::Pending, "{}", String::from_utf8_lossy(b));
            assert_eq!(w.see(b, t), Uninstall::Pending, "{}", String::from_utf8_lossy(b));
        }
    }
}
