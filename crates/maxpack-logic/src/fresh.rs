//! Whether the files a cache was built from are still the same (cache.rs `check`): `.done` records
//! each input's size and mtime when the build starts, and `check` compares them for equality. Never
//! with the console clock or `.done`'s own mtime: a jailbroken console is often offline with its
//! clock years behind (2012 on a real one), so a file written on a PC carries a "future" mtime that
//! must not make its cache stale on every pass.

use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::time::{SystemTime, UNIX_EPOCH};

/// One input of a cache as `.done` records it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Input {
    pub path: String,
    pub size: u64,
    /// Nanoseconds since the epoch (negative before it), as the filesystem gave it.
    pub mtime_ns: i64,
}

/// The `.done` key that holds the inputs (a list of `Input`).
pub const KEY: &str = "inputs";

/// `t` in nanoseconds since the epoch, negative before it, saturated at the ends of i64.
pub fn ns(t: SystemTime) -> i64 {
    match t.duration_since(UNIX_EPOCH) {
        Ok(d) => i64::try_from(d.as_nanos()).unwrap_or(i64::MAX),
        Err(e) => i64::try_from(e.duration().as_nanos()).map_or(i64::MIN, |n| -n),
    }
}

/// The inputs `stamp` (a `.done`) recorded; none when the key is missing or malformed.
pub fn recorded(stamp: &serde_json::Value) -> Vec<Input> {
    serde_json::from_value(stamp[KEY].clone()).unwrap_or_default()
}

/// (size, mtime ns) `stamp` recorded for `path`; None when it recorded no such input.
pub fn recorded_of(stamp: &serde_json::Value, path: &str) -> Option<(u64, i64)> {
    recorded(stamp).into_iter().find(|i| i.path == path).map(|i| (i.size, i.mtime_ns))
}

/// The inputs as `.done` stores them under `KEY`.
pub fn to_json(inputs: &[Input]) -> serde_json::Value {
    serde_json::to_value(inputs).unwrap_or_default()
}

/// The first difference between the inputs `.done` recorded and the inputs now (path, and its
/// (size, mtime ns) or None when it cannot be stat'ed), as `check` logs it: "<path> changed",
/// "<path> missing", "<path> added" or "<path> removed". None: every input has the size and mtime
/// it had when the cache was built, whatever the clock says.
pub fn changed(recorded: &[Input], now: &[(String, Option<(u64, i64)>)]) -> Option<String> {
    let was: HashMap<&str, (u64, i64)> = recorded.iter().map(|i| (i.path.as_str(), (i.size, i.mtime_ns))).collect();
    for (path, state) in now {
        match (state, was.get(path.as_str())) {
            (None, _) => return Some(format!("{path} missing")),
            (Some(_), None) => return Some(format!("{path} added")),
            (Some(s), Some(w)) if s != w => return Some(format!("{path} changed")),
            _ => {}
        }
    }
    let here: HashSet<&str> = now.iter().map(|(p, _)| p.as_str()).collect();
    recorded.iter().find(|i| !here.contains(i.path.as_str())).map(|i| format!("{} removed", i.path))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, UNIX_EPOCH};

    /// 2026-11-24 00:00:00 UTC and 2012-01-02 00:00:00 UTC.
    const PC_2026: u64 = 1_795_478_400;
    const CONSOLE_2012: u64 = 1_325_462_400;

    fn input(path: &str, size: u64, mtime_ns: i64) -> Input {
        Input { path: path.to_string(), size, mtime_ns }
    }

    fn now(v: &[Input]) -> Vec<(String, Option<(u64, i64)>)> {
        v.iter().map(|i| (i.path.clone(), Some((i.size, i.mtime_ns)))).collect()
    }

    #[test]
    fn ns_table() {
        assert_eq!(ns(UNIX_EPOCH), 0);
        assert_eq!(ns(UNIX_EPOCH + Duration::from_millis(1500)), 1_500_000_000);
        assert_eq!(ns(UNIX_EPOCH - Duration::from_secs(2)), -2_000_000_000);
        assert_eq!(ns(UNIX_EPOCH + Duration::from_secs(PC_2026)), PC_2026 as i64 * 1_000_000_000);
        // Year 2286: past what i64 nanoseconds hold, saturated.
        assert_eq!(ns(UNIX_EPOCH + Duration::from_secs(10_000_000_000)), i64::MAX);
    }

    #[test]
    fn source_from_a_pc_clock_ahead_of_the_console_is_fresh() {
        // A .p5z and a backport written on the PC in 2026; the cache built on a console whose clock
        // says 2012, so `.done`'s mtime is 2012 (the old rule, input mtime >= `.done` mtime, said
        // stale on every pass).
        let pc = ns(UNIX_EPOCH + Duration::from_secs(PC_2026));
        let done_mtime = ns(UNIX_EPOCH + Duration::from_secs(CONSOLE_2012));
        assert!(pc >= done_mtime);
        let built = [
            input("/mnt/usb0/Game [PPSA01234].p5z", 2_331_829_804, pc),
            input("/mnt/usb0/backports/PPSA01234", 4096, pc + 7),
            input("/mnt/usb0/backports/PPSA01234/fakelib/libc.prx", 1000, pc + 9),
        ];
        assert_eq!(changed(&built, &now(&built)), None);
        // In another order too.
        let mut shuffled = now(&built);
        shuffled.reverse();
        assert_eq!(changed(&built, &shuffled), None);
    }

    #[test]
    fn changed_size_or_mtime_is_stale() {
        let built = [input("/data/g.pkg", 100, 5_000_000_000), input("/data/bp/eboot.bin", 7, 6)];
        let with = |k: usize, size: u64, mtime: i64| {
            let mut v = now(&built);
            v[k].1 = Some((size, mtime));
            v
        };
        assert_eq!(changed(&built, &with(0, 101, 5_000_000_000)), Some("/data/g.pkg changed".into()));
        assert_eq!(changed(&built, &with(0, 100, 5_000_000_001)), Some("/data/g.pkg changed".into()));
        // An older mtime is a change too (another copy of the file), not "older than `.done`".
        assert_eq!(changed(&built, &with(1, 7, 5)), Some("/data/bp/eboot.bin changed".into()));
        // An input that cannot be stat'ed.
        let mut gone = now(&built);
        gone[1].1 = None;
        assert_eq!(changed(&built, &gone), Some("/data/bp/eboot.bin missing".into()));
        // A file added to or removed from the inputs (a backport gains or loses a file).
        let mut more = now(&built);
        more.push(("/data/bp/new.prx".into(), Some((1, 1))));
        assert_eq!(changed(&built, &more), Some("/data/bp/new.prx added".into()));
        assert_eq!(changed(&built, &now(&built[..1])), Some("/data/bp/eboot.bin removed".into()));
        // A `.done` without the record (none recorded) and inputs now: stale.
        assert_eq!(changed(&[], &now(&built[..1])), Some("/data/g.pkg added".into()));
        assert_eq!(changed(&[], &[]), None);
    }

    #[test]
    fn stamp_round_trip() {
        let built = vec![input("/mnt/usb0/a b™.p5z", 1, -3), input("/x", u64::MAX, i64::MAX)];
        let mut stamp = serde_json::json!({"v": 4});
        stamp[KEY] = to_json(&built);
        assert_eq!(recorded(&stamp), built);
        let back: serde_json::Value = serde_json::from_str(&stamp.to_string()).unwrap();
        assert_eq!(recorded(&back), built);
        assert_eq!(recorded(&serde_json::json!({"v": 4})), Vec::<Input>::new());
        assert_eq!(recorded(&serde_json::json!({"inputs": "x"})), Vec::<Input>::new());
        assert_eq!(KEY, "inputs");
    }

    #[test]
    fn recorded_one() {
        let built = vec![input("/data/a.pkg", 10, 20), input("/data/bp", 30, 40)];
        let mut stamp = serde_json::json!({});
        stamp[KEY] = to_json(&built);
        assert_eq!(recorded_of(&stamp, "/data/a.pkg"), Some((10, 20)));
        assert_eq!(recorded_of(&stamp, "/data/b.pkg"), None);
    }
}
