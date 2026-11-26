//! What the console's own jobs share: the game sheet's "Verify fully" (src/verify.rs) and the receive from the
//! PC (src/receive.rs). The engine's progress hook and cancel flag are one per process, so one job of the
//! engine runs at a time, the console's or the installer's (DESIGN.md "Console jobs").
use std::collections::VecDeque;

/// MaxPack's own home-screen tile: its page open in the console's browser is not a game.
pub const TILE_ID: &str = "MXPK00001";

/// The title ids of the processes that count as a running game, from the app info of every process: a game's
/// title id (`titles::valid_tid`: four capitals and five digits, not a system app's `NPXS`), not MaxPack's
/// tile; sorted, each once.
pub fn running_games<'a>(titles: impl IntoIterator<Item = &'a str>) -> Vec<String> {
    let mut v: Vec<String> =
        titles.into_iter().filter(|t| crate::titles::valid_tid(t) && *t != TILE_ID).map(str::to_string).collect();
    v.sort();
    v.dedup();
    v
}

/// An installer request (its own route, `inst::installer_path`) that starts a job of the engine: an install,
/// a verify or an uninstall. Refused while a console job runs, and no console job starts while it is
/// forwarded (src/web.rs).
pub fn blocks_installer(method: &str, route: &str) -> bool {
    method == "POST" && matches!(route.split('?').next(), Some("/api/install" | "/api/verify" | "/api/uninstall"))
}

/// What keeps a console job from starting: another job of the engine.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Busy {
    /// The installer runs or queues a job (install, verify, uninstall).
    Install,
    /// A "Verify fully" of a game's sheet runs.
    Verify,
    /// A receive from the PC is under way (receiving or verifying what arrived).
    Receive,
}

impl Busy {
    /// The code the page and the PC are told.
    pub fn code(self) -> &'static str {
        match self {
            Busy::Install => "install_running",
            Busy::Verify => "verify_running",
            Busy::Receive => "receive_running",
        }
    }

    /// The English detail.
    pub fn detail(self) -> &'static str {
        match self {
            Busy::Install => "the installer is running a job; try again when it ends",
            Busy::Verify => "a .p5z is being verified on the console; try again when it ends",
            Busy::Receive => "a receive from the PC is under way; try again when it ends",
        }
    }
}

/// The first of the console's jobs that runs, in that order: the installer, a verify, a receive.
pub fn busy(install: bool, verify: bool, receive: bool) -> Option<Busy> {
    if install {
        Some(Busy::Install)
    } else if verify {
        Some(Busy::Verify)
    } else if receive {
        Some(Busy::Receive)
    } else {
        None
    }
}

/// How long the speed looks back at most.
const RATE_KEEP_MS: u64 = 60_000;
/// Samples closer than this replace the last one, so a fast hook does not fill the window.
const RATE_STEP_MS: u64 = 250;
/// No speed before the samples span this long.
const RATE_MIN_MS: u64 = 1000;

/// A job's speed in MB/s (10^6 bytes a second) from samples of (milliseconds, bytes done so far).
#[derive(Debug, Default, Clone)]
pub struct Rate {
    samples: VecDeque<(u64, u64)>,
}

impl Rate {
    /// One sample: `bytes` done by `ms`. A sample older than the last one, or with fewer bytes (a new phase),
    /// starts over.
    pub fn add(&mut self, ms: u64, bytes: u64) {
        if self.samples.back().is_some_and(|&(t, b)| ms < t || bytes < b) {
            self.samples.clear();
        }
        // Thinned: the last sample moves while it is within a step of the one before it.
        let n = self.samples.len();
        if n >= 2 && ms - self.samples[n - 2].0 < RATE_STEP_MS {
            self.samples[n - 1] = (ms, bytes);
        } else {
            self.samples.push_back((ms, bytes));
        }
        while self.samples.front().is_some_and(|&(t, _)| ms - t > RATE_KEEP_MS) {
            self.samples.pop_front();
        }
    }

    /// The speed over the last `over_ms` (at most what is kept); None until the samples span RATE_MIN_MS.
    pub fn mbps(&self, over_ms: u64) -> Option<f64> {
        let &(t1, b1) = self.samples.back()?;
        // The oldest sample within the window.
        let &(t0, b0) = self.samples.iter().find(|&&(t, _)| t1 - t <= over_ms)?;
        let span = t1 - t0;
        (span >= RATE_MIN_MS).then(|| (b1 - b0) as f64 / 1000.0 / span as f64)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn games_are_game_title_ids_but_the_tile() {
        let seen = ["PPSA01234", "NPXS40087", "MXPK00001", "", "PPSA01234", "CUSA00001", "ppsa01234", "PPSA0123"];
        assert_eq!(running_games(seen), ["CUSA00001", "PPSA01234"]);
        assert!(running_games([]).is_empty());
        assert!(running_games(["NPXS20001", TILE_ID]).is_empty());
    }

    #[test]
    fn the_installer_jobs_are_blocked() {
        for r in ["/api/install", "/api/verify", "/api/uninstall", "/api/install?x=1"] {
            assert!(blocks_installer("POST", r), "{r}");
        }
        for (m, r) in [("GET", "/api/state"), ("POST", "/api/scan"), ("POST", "/api/probe"), ("POST", "/api/pause"),
            ("POST", "/api/discard"), ("GET", "/api/install"), ("POST", "/api/installx")] {
            assert!(!blocks_installer(m, r), "{m} {r}");
        }
    }

    #[test]
    fn busy_names_the_first_job() {
        assert_eq!(busy(false, false, false), None);
        assert_eq!(busy(true, true, true), Some(Busy::Install));
        assert_eq!(busy(false, true, true), Some(Busy::Verify));
        assert_eq!(busy(false, false, true), Some(Busy::Receive));
        assert_eq!(Busy::Install.code(), "install_running");
        assert_eq!(Busy::Verify.code(), "verify_running");
        assert_eq!(Busy::Receive.code(), "receive_running");
        assert!(Busy::Receive.detail().contains("receive"));
    }

    #[test]
    fn rate_over_a_window() {
        let mut r = Rate::default();
        assert_eq!(r.mbps(10_000), None);
        r.add(0, 0);
        r.add(500, 50_000_000);
        // Half a second of samples: not yet.
        assert_eq!(r.mbps(10_000), None);
        r.add(1000, 100_000_000);
        assert_eq!(r.mbps(10_000), Some(100.0));
        // 10 s at 100 MB/s, then 10 s at 50 MB/s: the last 10 s say 50, the 20 s say 75.
        for s in 2..=10 {
            r.add(s * 1000, s * 100_000_000);
        }
        for s in 11..=20 {
            r.add(s * 1000, 1_000_000_000 + (s - 10) * 50_000_000);
        }
        assert_eq!(r.mbps(10_000), Some(50.0));
        assert_eq!(r.mbps(20_000), Some(75.0));
    }

    #[test]
    fn rate_keeps_a_minute_and_thins_fast_samples() {
        let mut r = Rate::default();
        for ms in (0..=120_000).step_by(10) {
            r.add(ms, ms * 1000);
        }
        // 1 MB/s whatever the window, and the window is at most a minute of samples, one per 250 ms or so.
        assert_eq!(r.mbps(300_000).map(|v| v.round()), Some(1.0));
        // (a sample moves while it is within a step of the one before it, so two are a step apart less one tick)
        assert!(r.samples.len() <= (RATE_KEEP_MS / RATE_STEP_MS) as usize * 11 / 10, "{}", r.samples.len());
        assert!(r.samples.front().unwrap().0 >= 120_000 - RATE_KEEP_MS - RATE_STEP_MS);
    }

    #[test]
    fn rate_starts_over_on_fewer_bytes_or_an_older_time() {
        let mut r = Rate::default();
        r.add(0, 0);
        r.add(2000, 200_000_000);
        assert_eq!(r.mbps(10_000), Some(100.0));
        // A new phase counts from 0 again.
        r.add(3000, 0);
        assert_eq!(r.mbps(10_000), None);
        r.add(5000, 20_000_000);
        assert_eq!(r.mbps(10_000), Some(10.0));
        r.add(1000, 30_000_000);
        assert_eq!(r.mbps(10_000), None);
    }
}
