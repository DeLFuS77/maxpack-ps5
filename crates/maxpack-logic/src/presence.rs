//! What `/api/state` says of each library title beyond its build status: on the home screen
//! (`registered`), its title mount up (`mounted`), its source there (`source_present`).

/// For each of `tids`, in order: its bytes appear in `db` (app.db read as bytes, as
/// `register::is_registered` searches it; no SELECT, so a deleted row still in a free page counts).
/// One pass over `db` for all of them.
pub fn in_app_db(db: &[u8], tids: &[&str]) -> Vec<bool> {
    let mut found = vec![false; tids.len()];
    // By length: every title id is 9 bytes, so this is one pass in practice.
    let mut lens: Vec<usize> = tids.iter().map(|t| t.len()).filter(|&n| n > 0).collect();
    lens.sort_unstable();
    lens.dedup();
    for n in lens {
        let wanted: std::collections::HashSet<&[u8]> = tids.iter().filter(|t| t.len() == n).map(|t| t.as_bytes()).collect();
        // Most windows of app.db start with a byte no wanted title starts with: skipped unhashed.
        let mut first = [false; 256];
        for t in &wanted {
            first[t[0] as usize] = true;
        }
        let mut seen = std::collections::HashSet::new();
        for w in db.windows(n) {
            if first[w[0] as usize] && wanted.contains(w) {
                seen.insert(w);
            }
        }
        for (i, t) in tids.iter().enumerate() {
            if t.len() == n && seen.contains(t.as_bytes()) {
                found[i] = true;
            }
        }
    }
    found
}

/// `in_app_db` memoized on app.db's stamp (length and mtime, as the caller stats it) and the titles
/// asked: the page polls `/api/state` every second or two, and app.db seldom changes, so a poll reads
/// and scans it only when it did change or the library did.
#[derive(Default)]
pub struct DbMemo {
    key: Option<((u64, i64, i64), Vec<String>)>,
    found: Vec<bool>,
}

impl DbMemo {
    /// `stamp`: (length, mtime seconds, mtime nanoseconds) of app.db now; `read` reads it, only when
    /// the stamp or `tids` differ from the last good read. A read error is returned and not memoized.
    pub fn get<E>(&mut self, stamp: (u64, i64, i64), tids: &[&str], read: impl FnOnce() -> Result<Vec<u8>, E>)
        -> Result<Vec<bool>, E> {
        let _ = (stamp, tids, &self.key, &self.found);
        read().map(|db| in_app_db(&db, tids))
    }
}

/// The mount point of a title's `app0` stack, which `mounted` looks for in the mount table.
pub fn title_mount(tid: &str) -> String {
    format!("/system_ex/app/{tid}")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn title_ids_found_in_app_db() {
        let db = b"SQLite format 3\0...PPSA01234\x00\x01CUSA00001junkPPSA0999";
        assert_eq!(in_app_db(db, &["PPSA01234", "CUSA00001", "PPSA09999", "PPSA0999"]), [true, true, false, true]);
        // In order, the same title twice, at the very start and the very end.
        let db = b"PPSA00001xxPPSA00002";
        assert_eq!(in_app_db(db, &["PPSA00002", "PPSA00001", "PPSA00002"]), [true, true, true]);
        // Nothing asked, nothing read; an empty or short db has none.
        assert!(in_app_db(db, &[]).is_empty());
        assert_eq!(in_app_db(b"", &["PPSA00001"]), [false]);
        assert_eq!(in_app_db(b"PPSA0000", &["PPSA00001"]), [false]);
    }

    #[test]
    fn titles_of_other_lengths_are_searched_too() {
        // A hand-edited entry that is not a valid title id is simply searched for as it is.
        assert_eq!(in_app_db(b"abcMXPK00001def", &["MXPK00001", "abc", ""]), [true, true, false]);
    }

    #[test]
    fn app_db_is_read_again_only_when_it_or_the_titles_change() {
        let mut memo = DbMemo::default();
        let reads = std::cell::Cell::new(0);
        let read = |db: &'static [u8]| {
            let reads = &reads;
            move || -> Result<Vec<u8>, String> {
                reads.set(reads.get() + 1);
                Ok(db.to_vec())
            }
        };
        let tids = ["PPSA00001", "PPSA00002"];
        assert_eq!(memo.get((10, 5, 0), &tids, read(b"PPSA00001")), Ok(vec![true, false]));
        assert_eq!(reads.get(), 1);
        // Same stamp, same titles: the last answer, nothing read (even if the bytes were different).
        assert_eq!(memo.get((10, 5, 0), &tids, read(b"PPSA00002")), Ok(vec![true, false]));
        assert_eq!(reads.get(), 1);
        // Another length, another mtime (seconds or nanoseconds): read again.
        assert_eq!(memo.get((11, 5, 0), &tids, read(b"PPSA00002")), Ok(vec![false, true]));
        assert_eq!(memo.get((11, 6, 0), &tids, read(b"PPSA00001PPSA00002")), Ok(vec![true, true]));
        assert_eq!(memo.get((11, 6, 1), &tids, read(b"")), Ok(vec![false, false]));
        assert_eq!(reads.get(), 4);
        // Other titles (one more, or another order): read again.
        assert_eq!(memo.get((11, 6, 1), &["PPSA00002", "PPSA00001"], read(b"PPSA00001")), Ok(vec![false, true]));
        assert_eq!(memo.get((11, 6, 1), &["PPSA00002"], read(b"PPSA00002")), Ok(vec![true]));
        assert_eq!(reads.get(), 6);
    }

    #[test]
    fn a_failed_read_of_app_db_is_not_memoized() {
        let mut memo = DbMemo::default();
        assert_eq!(memo.get((10, 5, 0), &["PPSA00001"], || Err::<Vec<u8>, _>("busy")), Err("busy"));
        let mut read = false;
        assert_eq!(memo.get((10, 5, 0), &["PPSA00001"], || { read = true; Ok::<_, &str>(b"PPSA00001".to_vec()) }), Ok(vec![true]));
        assert!(read);
        // And a failure after a good read leaves the next try free to read too.
        assert_eq!(memo.get((12, 5, 0), &["PPSA00001"], || Err::<Vec<u8>, _>("busy")), Err("busy"));
        let mut read = false;
        assert_eq!(memo.get((12, 5, 0), &["PPSA00001"], || { read = true; Ok::<_, &str>(Vec::new()) }), Ok(vec![false]));
        assert!(read);
    }

    #[test]
    fn the_title_mount() {
        assert_eq!(title_mount("PPSA01234"), "/system_ex/app/PPSA01234");
    }
}
