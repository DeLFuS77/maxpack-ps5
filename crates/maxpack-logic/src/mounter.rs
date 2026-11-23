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
