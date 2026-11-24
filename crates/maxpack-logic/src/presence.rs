//! What `/api/state` says of each library title beyond its build status: on the home screen
//! (`registered`), its title mount up (`mounted`), its source there (`source_present`).

/// For each of `tids`, in order: its bytes appear in `db` (app.db read as bytes, as
/// `register::is_registered` searches it; no SELECT, so a deleted row still in a free page counts).
/// One pass over `db` for all of them.
pub fn in_app_db(db: &[u8], tids: &[&str]) -> Vec<bool> {
    let _ = (db, tids);
    Vec::new()
}

/// The mount point of a title's `app0` stack, which `mounted` looks for in the mount table.
pub fn title_mount(tid: &str) -> String {
    let _ = tid;
    String::new()
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
    fn the_title_mount() {
        assert_eq!(title_mount("PPSA01234"), "/system_ex/app/PPSA01234");
    }
}
