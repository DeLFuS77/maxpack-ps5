//! The duplicate rule of the scan: which copy of a title id is the game.

use std::collections::HashMap;

/// Lower rank wins: a folder beats a pkg, which beats a .p5z.
pub const RANK_FOLDER: u8 = 0;
pub const RANK_PKG: u8 = 1;
pub const RANK_P5Z: u8 = 2;

/// Duplicate rule: the lowest rank wins; within a rank the first source in scan order wins and
/// the others go to `duplicates`. A source still being copied (`source` None) claims its title id
/// the same way, so a mid-copy winner keeps the other copy out of the library (an unstable
/// loser claims nothing); it itself goes only to `unstable`. A `nested` source (one folder below
/// a root, scanned after every root's own entries) never beats a holder: an old copy kept in a
/// subfolder cannot take over a game that already runs from a root.
///
/// `seen`: title id -> (rank of the winner, its index in `games`; None while it is still being
/// copied). A slot of `games` is emptied when a later source beats it.
pub fn claim<T>(seen: &mut HashMap<String, (u8, Option<usize>)>, games: &mut Vec<Option<T>>,
                duplicates: &mut Vec<T>, tid: &str, rank: u8, nested: bool, source: Option<T>) {
    match seen.get(tid) {
        Some(_) if nested => duplicates.extend(source),
        // The holder wins unless this one has a lower rank.
        Some(&(held, _)) if rank >= held => duplicates.extend(source),
        held => {
            if let Some(&(_, Some(i))) = held {
                duplicates.extend(games[i].take());
            }
            let slot = source.map(|s| {
                games.push(Some(s));
                games.len() - 1
            });
            seen.insert(tid.to_string(), (rank, slot));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Runs `claim` over the sources (name, rank, nested, stable) in order; returns (games, duplicates).
    fn run(sources: &[(&'static str, u8, bool, bool)]) -> (Vec<&'static str>, Vec<&'static str>) {
        let (mut seen, mut games, mut dups) = (HashMap::new(), Vec::new(), Vec::new());
        for &(name, rank, nested, stable) in sources {
            claim(&mut seen, &mut games, &mut dups, "T", rank, nested, stable.then_some(name));
        }
        (games.into_iter().flatten().collect(), dups)
    }

    #[test]
    fn claim_rank_table() {
        // The old bool rule (folder beats pkg) with ranks folder/pkg only.
        assert_eq!(run(&[("f", RANK_FOLDER, false, true), ("p", RANK_PKG, false, true)]), (vec!["f"], vec!["p"]));
        assert_eq!(run(&[("p", RANK_PKG, false, true), ("f", RANK_FOLDER, false, true)]), (vec!["f"], vec!["p"]));
        assert_eq!(run(&[("f1", RANK_FOLDER, false, true), ("f2", RANK_FOLDER, false, true)]), (vec!["f1"], vec!["f2"]));
        assert_eq!(run(&[("p1", RANK_PKG, false, true), ("p2", RANK_PKG, false, true)]), (vec!["p1"], vec!["p2"]));
        // Nested never beats a holder, even a pkg holder against a nested folder.
        assert_eq!(run(&[("p", RANK_PKG, false, true), ("f", RANK_FOLDER, true, true)]), (vec!["p"], vec!["f"]));
        // A nested source with no holder yet wins, and a later non-nested better one takes over.
        assert_eq!(run(&[("n", RANK_PKG, true, true), ("f", RANK_FOLDER, false, true)]), (vec!["f"], vec!["n"]));
        // An unstable folder keeps the title; a later pkg is a duplicate and the folder is not.
        assert_eq!(run(&[("f", RANK_FOLDER, false, false), ("p", RANK_PKG, false, true)]), (vec![], vec!["p"]));
        // An unstable pkg holder is replaced by a folder.
        assert_eq!(run(&[("p", RANK_PKG, false, false), ("f", RANK_FOLDER, false, true)]), (vec!["f"], vec![]));
        // The new p5z rank.
        assert_eq!(run(&[("z", RANK_P5Z, false, true), ("p", RANK_PKG, false, true)]), (vec!["p"], vec!["z"]));
        assert_eq!(run(&[("p", RANK_PKG, false, true), ("z", RANK_P5Z, false, true)]), (vec!["p"], vec!["z"]));
        assert_eq!(run(&[("z", RANK_P5Z, false, true), ("f", RANK_FOLDER, true, true)]), (vec!["z"], vec!["f"]));
        assert_eq!(run(&[("z1", RANK_P5Z, false, true), ("z2", RANK_P5Z, false, true)]), (vec!["z1"], vec!["z2"]));
    }
}
