//! Play from the page (`POST /api/launch/<TID>`): when a title may be started, and what the
//! console's launch calls answer.

/// Why a launch is refused.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Refusal {
    /// Not a library title (an invalid title id, an absent one, or only a duplicate copy).
    NotFound,
    /// A game runs: the console starts one at a time, and its overlay stays until it exits.
    GameRunning,
    /// MaxPack does not mount now (another mounter runs, or no kstuff): what the home screen would
    /// start is not MaxPack's to start.
    NotMounting,
    /// The title is not `ready` (pending, checking, building, error).
    NotReady,
}

impl Refusal {
    /// The HTTP status of the refusal.
    pub fn status(self) -> u16 {
        match self {
            Refusal::NotFound => 404,
            Refusal::GameRunning | Refusal::NotMounting | Refusal::NotReady => 409,
        }
    }

    /// The plain-text body (the page shows it as it is).
    pub fn text(self) -> &'static str {
        match self {
            Refusal::NotFound => "not found",
            Refusal::GameRunning => "a game is running",
            Refusal::NotMounting => "MaxPack does not mount now (another mounter runs, or no kstuff)",
            Refusal::NotReady => "the game is not ready",
        }
    }
}

/// `status`: the library entry's status (None when the title is not in the library); `mounts`:
/// MaxPack mounts now; `paused`: a game runs.
pub fn gate(status: Option<&str>, mounts: bool, paused: bool) -> Result<(), Refusal> {
    let status = status.ok_or(Refusal::NotFound)?;
    if paused {
        return Err(Refusal::GameRunning);
    }
    if !mounts {
        return Err(Refusal::NotMounting);
    }
    if status != "ready" {
        return Err(Refusal::NotReady);
    }
    Ok(())
}

/// The answer of sceLncUtilLaunchApp: the launch was taken (submitted, or 0x80940005, which LncUtil
/// returns for titles it starts anyway; CheatRunner's cr_launch.c).
pub fn launch_taken(rc: i32) -> bool {
    rc >= 0 || rc as u32 == 0x8094_0005
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_a_ready_library_title_starts_while_nothing_runs() {
        assert_eq!(gate(Some("ready"), true, false), Ok(()));
        assert_eq!(gate(None, true, false), Err(Refusal::NotFound));
        for s in ["pending", "checking", "building", "error"] {
            assert_eq!(gate(Some(s), true, false), Err(Refusal::NotReady), "{s}");
        }
        assert_eq!(gate(Some("ready"), true, true), Err(Refusal::GameRunning));
        assert_eq!(gate(Some("ready"), false, false), Err(Refusal::NotMounting));
    }

    #[test]
    fn the_first_reason_wins() {
        // Not found before anything else, then a running game, then the mounter, then the status.
        assert_eq!(gate(None, false, true), Err(Refusal::NotFound));
        assert_eq!(gate(Some("building"), false, true), Err(Refusal::GameRunning));
        assert_eq!(gate(Some("building"), false, false), Err(Refusal::NotMounting));
    }

    #[test]
    fn refusals_answer_404_or_409() {
        assert_eq!(Refusal::NotFound.status(), 404);
        for r in [Refusal::GameRunning, Refusal::NotMounting, Refusal::NotReady] {
            assert_eq!(r.status(), 409, "{r:?}");
            assert!(!r.text().is_empty(), "{r:?}");
        }
        assert_eq!(Refusal::GameRunning.text(), "a game is running");
        assert_eq!(Refusal::NotReady.text(), "the game is not ready");
    }

    #[test]
    fn launch_answers() {
        assert!(launch_taken(0));
        assert!(launch_taken(5));
        assert!(launch_taken(0x8094_0005u32 as i32));
        assert!(!launch_taken(0x8094_0001u32 as i32));
        assert!(!launch_taken(-1));
    }
}
