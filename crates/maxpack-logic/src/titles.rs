//! Title ids and the per-title lists of config.toml (`playgo_off`, `hidden`) that the page switches
//! one title at a time (`POST /api/playgo/<TID>`, `POST /api/hidden/<TID>`).

/// A game's title id: four upper-case letters and five digits (PPSA01234, CUSA01234), never a
/// system app (NPXS).
pub fn valid_tid(t: &str) -> bool {
    let b = t.as_bytes();
    b.len() == 9 && b[..4].iter().all(u8::is_ascii_uppercase) && b[4..].iter().all(u8::is_ascii_digit)
        && !t.starts_with("NPXS")
}

/// Puts `tid` in `list` (`on`) or takes it out (`off`): the other titles keep their order, a title
/// is never listed twice (an earlier duplicate goes too). True when the list changed.
pub fn set_listed(list: &mut Vec<String>, tid: &str, on: bool) -> bool {
    let before = list.clone();
    let mut seen = false;
    list.retain(|t| t != tid || (on && !std::mem::replace(&mut seen, true)));
    if on && !seen {
        list.push(tid.to_string());
    }
    *list != before
}

/// The body of a per-title switch: exactly `on` (Some(true)) or `off` (Some(false)); anything else
/// is None (a 400).
pub fn switch_body(body: &[u8]) -> Option<bool> {
    match body {
        b"on" => Some(true),
        b"off" => Some(false),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn title_ids() {
        for ok in ["PPSA01234", "CUSA00001", "PPSA10001"] {
            assert!(valid_tid(ok), "{ok}");
        }
        for bad in ["", "PPSA0123", "PPSA012345", "ppsa01234", "PPS101234", "PPSA0123A", "NPXS20001", "PPSA 1234", "ＰPSA01234"] {
            assert!(!valid_tid(bad), "{bad:?}");
        }
    }

    #[test]
    fn a_title_goes_in_and_out_of_a_list() {
        let mut l: Vec<String> = vec!["PPSA00001".into(), "PPSA00002".into()];
        assert!(set_listed(&mut l, "PPSA00003", true));
        assert_eq!(l, ["PPSA00001", "PPSA00002", "PPSA00003"]);
        // Already there: nothing changes, it is not listed twice.
        assert!(!set_listed(&mut l, "PPSA00003", true));
        assert_eq!(l.len(), 3);
        // Out: the others keep their order.
        assert!(set_listed(&mut l, "PPSA00001", false));
        assert_eq!(l, ["PPSA00002", "PPSA00003"]);
        assert!(!set_listed(&mut l, "PPSA00001", false));
        assert_eq!(l, ["PPSA00002", "PPSA00003"]);
    }

    #[test]
    fn a_title_listed_twice_by_hand_is_set_once() {
        let mut l: Vec<String> = vec!["PPSA00002".into(), "PPSA00001".into(), "PPSA00002".into()];
        assert!(set_listed(&mut l, "PPSA00002", true));
        assert_eq!(l, ["PPSA00002", "PPSA00001"]);
        let mut l: Vec<String> = vec!["PPSA00002".into(), "PPSA00002".into()];
        assert!(set_listed(&mut l, "PPSA00002", false));
        assert!(l.is_empty());
    }

    #[test]
    fn switch_bodies() {
        assert_eq!(switch_body(b"on"), Some(true));
        assert_eq!(switch_body(b"off"), Some(false));
        for bad in [&b""[..], b"ON", b"on\n", b" off", b"true", b"1", b"hide"] {
            assert_eq!(switch_body(bad), None, "{:?}", String::from_utf8_lossy(bad));
        }
    }
}
