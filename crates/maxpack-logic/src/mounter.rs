/// Programs that mount the same games and must not run together with MaxPack.
pub const OTHER_MOUNTERS: [&str; 2] = ["shadowmountplus.elf", "kraken-mount.elf"];

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MounterState {
    /// MaxPack mounts.
    Ready,
    /// kstuff is not loaded yet: nothing can be mounted.
    WaitKstuff,
    /// Another mounter is running (its process name); MaxPack stays out of its way.
    OtherMounter(&'static str),
}

/// `running(name)` says whether a process with that name is alive; it is only asked once kstuff is loaded.
pub fn mounter_state(kstuff_loaded: bool, running: &dyn Fn(&str) -> bool) -> MounterState {
    if !kstuff_loaded {
        return MounterState::WaitKstuff;
    }
    for name in OTHER_MOUNTERS {
        if running(name) {
            return MounterState::OtherMounter(name);
        }
    }
    MounterState::Ready
}

impl MounterState {
    /// Whether MaxPack mounts (and overlays `fakelib`) in this state.
    pub fn mounts(self) -> bool {
        self == MounterState::Ready
    }

    /// The `waiting` field of the API: `None` | `"kstuff"` | `"other:<process>"`.
    pub fn api_waiting(self) -> Option<String> {
        match self {
            MounterState::Ready => None,
            MounterState::WaitKstuff => Some("kstuff".to_string()),
            MounterState::OtherMounter(n) => Some(format!("other:{n}")),
        }
    }
}

/// One look at the process list for OTHER_MOUNTERS.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Look {
    /// This one runs (the first of OTHER_MOUNTERS that does).
    Found(&'static str),
    /// The list was read and none of them runs.
    NotFound,
    /// The list could not be read (sysctl failed, a bad record): it says nothing either way.
    Error,
}

/// The look for `running(name)`; `None` when the process list could not be read.
pub fn look(running: Option<&dyn Fn(&str) -> bool>) -> Look {
    let Some(running) = running else { return Look::Error };
    walk_look(running, false)
}

/// The look of one walk of the process list, `failed` when a bad record or a sysctl error cut it
/// short: a mounter seen before that is Found all the same (never lost to a later bad record);
/// nothing seen is Error when the walk failed, NotFound when it ran to the end.
pub fn walk_look(running: &dyn Fn(&str) -> bool, failed: bool) -> Look {
    match OTHER_MOUNTERS.into_iter().find(|n| running(n)) {
        Some(n) => Look::Found(n),
        None if failed => Look::Error,
        None => Look::NotFound,
    }
}

/// How far apart the two clean looks must be before MaxPack mounts again after another mounter.
pub const GONE_MS: u64 = 2000;

/// mounter_state with memory, so one bad look never lets MaxPack mount over a running mounter:
/// entering OtherMounter is immediate; leaving it takes two clean looks (`NotFound`) at least
/// GONE_MS apart, with no error and no find between them. An error changes nothing.
#[derive(Clone, Copy, Debug)]
pub struct Debounced {
    /// The other mounter last found, until it is seen gone.
    other: Option<&'static str>,
    /// Time (ms) of the first clean look since then.
    clear_since: Option<u64>,
}

impl Debounced {
    pub const fn new() -> Self {
        Debounced { other: None, clear_since: None }
    }

    /// The state after one look at `now_ms` (a monotonic clock); kstuff first, as mounter_state.
    pub fn update(&mut self, kstuff_loaded: bool, look: Look, now_ms: u64) -> MounterState {
        match look {
            Look::Found(n) => (self.other, self.clear_since) = (Some(n), None),
            Look::Error => self.clear_since = None,
            Look::NotFound if self.other.is_some() => match self.clear_since {
                None => self.clear_since = Some(now_ms),
                Some(t) if now_ms.saturating_sub(t) >= GONE_MS => (self.other, self.clear_since) = (None, None),
                Some(_) => {}
            },
            Look::NotFound => {}
        }
        let other = self.other;
        mounter_state(kstuff_loaded, &|n| other == Some(n))
    }
}

impl Default for Debounced {
    fn default() -> Self {
        Self::new()
    }
}

/// The name people know a mounter by, for toasts and the log: ShadowMount+ (as the other texts call
/// it) and the original of MaxPack; any other process keeps its own name.
pub fn display_name(process: &str) -> &str {
    match process {
        "shadowmountplus.elf" => "ShadowMount+",
        "kraken-mount.elf" => "Kraken Mount",
        other => other,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn display_names() {
        assert_eq!(display_name("shadowmountplus.elf"), "ShadowMount+");
        assert_eq!(display_name("kraken-mount.elf"), "Kraken Mount");
        assert_eq!(display_name("otro.elf"), "otro.elf");
        for n in OTHER_MOUNTERS {
            assert_ne!(display_name(n), n, "{n} has a display name");
        }
    }

    const SMP: &str = "shadowmountplus.elf";
    const KM: &str = "kraken-mount.elf";

    #[test]
    fn look_from_the_process_list() {
        assert_eq!(look(None), Look::Error);
        assert_eq!(look(Some(&|_| false)), Look::NotFound);
        assert_eq!(look(Some(&|n| n == KM)), Look::Found(KM));
        // Both running: OTHER_MOUNTERS order.
        assert_eq!(look(Some(&|_| true)), Look::Found(SMP));
    }

    #[test]
    fn walk_cut_short_keeps_what_it_found() {
        let none: &dyn Fn(&str) -> bool = &|_| false;
        let smp: &dyn Fn(&str) -> bool = &|n| n == SMP;
        let km: &dyn Fn(&str) -> bool = &|n| n == KM;
        let both: &dyn Fn(&str) -> bool = &|_| true;
        // (seen before the walk ended, walk failed, look)
        let table: [(&dyn Fn(&str) -> bool, bool, Look); 6] = [
            (none, false, Look::NotFound),
            (none, true, Look::Error),
            (smp, false, Look::Found(SMP)),
            (smp, true, Look::Found(SMP)), // a later bad record does not lose it
            (km, true, Look::Found(KM)),
            (both, true, Look::Found(SMP)),
        ];
        for (i, (running, failed, want)) in table.into_iter().enumerate() {
            assert_eq!(walk_look(running, failed), want, "row {i}");
        }
    }

    #[test]
    fn debounce_leaving_other_needs_two_clean_looks_2s_apart() {
        use Look::{Error, Found, NotFound};
        use MounterState::{OtherMounter, Ready};
        let steps: &[(Look, u64, MounterState)] = &[
            (NotFound, 0, Ready),
            (Found(SMP), 100, OtherMounter(SMP)), // entering Other is immediate
            (Error, 2200, OtherMounter(SMP)),     // an error keeps Other
            (NotFound, 2300, OtherMounter(SMP)),  // one clean look keeps Other
            (NotFound, 4299, OtherMounter(SMP)),  // a second one less than 2 s later keeps Other
            (NotFound, 4300, Ready),              // two clean looks 2 s apart: gone
            (Error, 9000, Ready),                 // an error keeps Ready too (the last state)
            (Found(KM), 9100, OtherMounter(KM)),
            (NotFound, 9200, OtherMounter(KM)),
            (Error, 10000, OtherMounter(KM)),     // an error between clean looks starts over
            (NotFound, 11300, OtherMounter(KM)),  // 2.1 s after the first clean look, but after the error
            (NotFound, 13300, Ready),
            (Found(SMP), 14000, OtherMounter(SMP)),
            (NotFound, 14100, OtherMounter(SMP)),
            (Found(SMP), 15000, OtherMounter(SMP)), // a find in between starts over too
            (NotFound, 16200, OtherMounter(SMP)),
            (NotFound, 18200, Ready),
        ];
        let mut d = Debounced::new();
        for (i, &(look, ms, want)) in steps.iter().enumerate() {
            assert_eq!(d.update(true, look, ms), want, "step {i}: {look:?} at {ms} ms");
        }
    }

    #[test]
    fn debounce_kstuff_first_and_other_remembered() {
        let mut d = Debounced::new();
        assert_eq!(d.update(false, Look::Found(SMP), 0), MounterState::WaitKstuff);
        // kstuff back: the mounter found meanwhile still counts.
        assert_eq!(d.update(true, Look::NotFound, 100), MounterState::OtherMounter(SMP));
        assert_eq!(d.update(false, Look::NotFound, 2100), MounterState::WaitKstuff);
        assert_eq!(d.update(true, Look::NotFound, 2200), MounterState::Ready);
    }

    #[test]
    fn mounter_state_transitions() {
        assert_eq!(mounter_state(false, &|_| false), MounterState::WaitKstuff);
        // Without kstuff nothing else matters.
        assert_eq!(mounter_state(false, &|_| true), MounterState::WaitKstuff);
        let s = mounter_state(true, &|n| n == "shadowmountplus.elf");
        assert_eq!(s, MounterState::OtherMounter("shadowmountplus.elf"));
        assert!(!s.mounts());
        assert_eq!(s.api_waiting(), Some("other:shadowmountplus.elf".to_string()));
        let k = mounter_state(true, &|n| n == "kraken-mount.elf");
        assert_eq!(k.api_waiting(), Some("other:kraken-mount.elf".to_string()));
        let r = mounter_state(true, &|_| false);
        assert_eq!(r, MounterState::Ready);
        assert!(r.mounts());
        assert_eq!(r.api_waiting(), None);
        assert!(!MounterState::WaitKstuff.mounts());
        assert_eq!(MounterState::WaitKstuff.api_waiting(), Some("kstuff".to_string()));
    }
}
