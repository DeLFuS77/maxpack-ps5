//! Which nullfs mounts in a game's sandbox are MaxPack's, for a later copy of the payload to adopt
//! (`launch.rs` `adopt`: a copy that quit or crashed mid-game left them, and left in the sandbox they
//! hang its teardown). Only for a sandbox whose `common/lib` has our overlay: that proves the launch was
//! ours, since another mounter's mounts of `/data`, the drives, the fonts or `/app0` look the same.

/// One mount of the mount table (oldest first, as getmntinfo lists them).
#[derive(Debug, Clone, Copy)]
pub struct Mount<'a> {
    pub on: &'a str,
    pub fstype: &'a str,
    pub from: &'a str,
    pub rdonly: bool,
}

/// The indexes in `mounts` of our nullfs mounts in sandbox `root` (`/mnt/sandbox/<TID>_<n>`) of title
/// `tid`, in the order `mount_data` makes them, so the session unmounts them newest first: `/data`
/// and each of `aliases` (`/mnt/usbN`, `/mnt/extN`) at the same path below `root`, from that very path
/// (ShellCore's own `/data` comes from `/user/data`); the fonts (`fonts`); the writable `/app0`, a
/// nullfs from `/system_ex/app/<TID>` that is not read-only and sits on a read-only mount of
/// `<root>/app0` (the system's, which `mount_app0` covers only when it is read-only); then
/// `/app0/<dir>` from the folder of the pkg or .p5z (`source`: (dir, folder), `cache::mount_source`).
pub fn ours(mounts: &[Mount], root: &str, tid: &str, aliases: &[String], fonts: &str, source: Option<(&str, &str)>) -> Vec<usize> {
    let _ = (mounts, root, tid, aliases, fonts, source);
    Vec::new()
}

#[cfg(test)]
mod tests {
    use super::*;

    const ROOT: &str = "/mnt/sandbox/PPSA01234_000";
    const FONTS: &str = "/preinst/common/font";

    fn m<'a>(on: &'a str, fstype: &'a str, from: &'a str, rdonly: bool) -> Mount<'a> {
        Mount { on, fstype, from, rdonly }
    }

    #[test]
    fn our_mounts_in_the_order_they_were_made() {
        let usb = vec!["/mnt/usb0".to_string(), "/mnt/ext1".to_string()];
        let mounts = [
            m("/", "ufs", "/dev/ssd0", false),
            m("/mnt/sandbox/PPSA01234_000/app0", "nullfs", "/system_ex/app/PPSA01234", true), // 1: the system's
            m("/mnt/sandbox/PPSA01234_000/data", "nullfs", "/user/data", false),           // 2: ShellCore's
            m("/mnt/sandbox/PPSA01234_000/common/lib", "unionfs", "<above>:/data/maxpack/caches/PPSA01234/fakelib", false),
            m("/mnt/sandbox/PPSA01234_000/data", "nullfs", "/data", false),                 // 4
            m("/mnt/sandbox/PPSA01234_000/mnt/ext1", "nullfs", "/mnt/ext1", false),         // 5
            m("/mnt/sandbox/PPSA01234_000/mnt/usb0", "nullfs", "/mnt/usb0", false),         // 6
            m("/mnt/sandbox/PPSA01234_000/preinst/common/font", "nullfs", FONTS, true),     // 7
            m("/mnt/sandbox/PPSA01234_000/app0", "nullfs", "/system_ex/app/PPSA01234", false), // 8: ours
            m("/mnt/sandbox/PPSA01234_000/app0/kraken", "nullfs", "/mnt/usb0/homebrew", true), // 9
            m("/mnt/sandbox/PPSA09999_000/data", "nullfs", "/data", false),                 // another sandbox
        ];
        // Pushed /data, the aliases in their order, the fonts, /app0, /app0/kraken: popped newest first.
        assert_eq!(ours(&mounts, ROOT, "PPSA01234", &usb, FONTS, Some(("kraken", "/mnt/usb0/homebrew"))), [4, 6, 5, 7, 8, 9]);
        // No pkg or .p5z source: no /app0/<dir>; another dir name or folder does not match.
        assert_eq!(ours(&mounts, ROOT, "PPSA01234", &usb, FONTS, None), [4, 6, 5, 7, 8]);
        assert_eq!(ours(&mounts, ROOT, "PPSA01234", &usb, FONTS, Some(("p5z", "/mnt/usb0/homebrew"))), [4, 6, 5, 7, 8]);
        assert_eq!(ours(&mounts, ROOT, "PPSA01234", &usb, FONTS, Some(("kraken", "/data/homebrew"))), [4, 6, 5, 7, 8]);
        // A drive not mounted now is not looked for.
        assert_eq!(ours(&mounts, ROOT, "PPSA01234", &[], FONTS, None), [4, 7, 8]);
    }

    #[test]
    fn the_writable_app0_is_ours_only_on_top_of_a_read_only_one() {
        // The system's /app0 is writable already: mount_app0 mounted nothing, that one is not ours.
        let system_rw = [
            m("/mnt/sandbox/PPSA01234_000/app0", "nullfs", "/system_ex/app/PPSA01234", false),
            m("/mnt/sandbox/PPSA01234_000/data", "nullfs", "/data", false),
        ];
        assert_eq!(ours(&system_rw, ROOT, "PPSA01234", &[], FONTS, None), [1]);
        // Ours mounted after the read-only one, not before it; another title's stack is not ours.
        let mounts = [
            m("/mnt/sandbox/PPSA01234_000/app0", "nullfs", "/system_ex/app/PPSA01234", false),
            m("/mnt/sandbox/PPSA01234_000/app0", "nullfs", "/system_ex/app/PPSA01234", true),
            m("/mnt/sandbox/PPSA01234_000/app0", "nullfs", "/system_ex/app/PPSA05555", false),
            m("/mnt/sandbox/PPSA01234_000/app0", "unionfs", "/system_ex/app/PPSA01234", false),
            m("/mnt/sandbox/PPSA01234_000/app0", "nullfs", "/system_ex/app/PPSA01234", false),
        ];
        assert_eq!(ours(&mounts, ROOT, "PPSA01234", &[], FONTS, None), [4]);
    }

    #[test]
    fn mounts_from_other_sources_are_left_alone() {
        let mounts = [
            m("/mnt/sandbox/PPSA01234_000/data", "nullfs", "/user/data", false),
            m("/mnt/sandbox/PPSA01234_000/data", "unionfs", "/data", false),
            m("/mnt/sandbox/PPSA01234_000/preinst/common/font", "nullfs", "/preinst/common/font2", true),
            m("/mnt/sandbox/PPSA01234_000/mnt/usb0", "nullfs", "/mnt/usb1", false),
            m("/mnt/sandbox/PPSA01234_0000/data", "nullfs", "/data", false),
        ];
        assert!(ours(&mounts, ROOT, "PPSA01234", &["/mnt/usb0".to_string()], FONTS, None).is_empty());
    }
}
