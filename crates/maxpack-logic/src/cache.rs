//! The names MaxPack keeps for itself in a game's cache dir (`/data/maxpack/caches/<TID>`, the
//! `/app0` of a pkg or .p5z game): src/cache.rs, ampr_index.rs and launch.rs take them from here, and
//! every one of them is `RESERVED`, so a game's file never takes one (p5z::loose_dest). A new control
//! name goes in `names!` below: it is reserved with it, it cannot be added in one place only.

/// Each name a `pub const`, and all of them in `RESERVED`.
macro_rules! names {
    ($($(#[$doc:meta])* $name:ident = $value:literal;)*) => {
        $($(#[$doc])* pub const $name: &str = $value;)*
        /// Every name of this module: none of them may be a game's file or folder at the root of a
        /// cache (compared without case).
        pub const RESERVED: &[&str] = &[$($name),*];
    };
}

names! {
    /// The stamp (JSON), written last: without it the cache is not built.
    DONE = ".done";
    /// What the last build created, one path per line relative to the cache dir (written before DONE).
    BUILT = ".built";
    /// What fakelib/ was composed from (JSON).
    AMPR_STATE = ".ampr";
    /// pkg caches: one line, no newline, the .pkg relative to /app0 (`kraken/<file name>`), for ampr-emu-ng.
    PKG_IMAGE = "kraken.image";
    /// pkg caches: the mount point of the pkg's folder at /app0/kraken (launch.rs).
    PKG_MOUNT_DIR = "kraken";
    /// pkg caches: the zero volume (pack 1) that sparse holes read from (ampr_index.rs).
    ZERO_VOLUME = "kraken_zero.dat";
    /// .p5z caches: the .p5z relative to /app0 and where each volume is in it (p5z::image_text).
    P5Z_IMAGE = "maxpack.image";
    /// .p5z caches: the mount point of the .p5z's folder at /app0/p5z (launch.rs).
    P5Z_MOUNT_DIR = "p5z";
    /// The cache's lib dir, the common/lib overlay at launch; also a game's lib dir.
    FAKELIB = "fakelib";
    /// A game's other lib dir (SMP: it wins over fakelib/, used as-is); never a game file.
    FAKELIB2 = "fakelib2";
}

/// A game's lib dirs, SMP priority first: the cache's fakelib/ is composed from one of them.
pub const LIB_DIRS: [&str; 2] = [FAKELIB2, FAKELIB];

/// Whether `name` (one path component) is one of MaxPack's names in a cache, in any case.
pub fn reserved(name: &str) -> bool {
    RESERVED.iter().any(|r| r.eq_ignore_ascii_case(name))
}

/// The rest of `lower` (a lowercase path) below lib dir `dir` (`fakelib/x` -> `x`); None when it is
/// not below it.
pub fn below<'a>(lower: &'a str, dir: &str) -> Option<&'a str> {
    lower.strip_prefix(dir)?.strip_prefix('/')
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reserved_and_below() {
        assert_eq!(RESERVED.len(), 10);
        for n in [".done", ".BUILT", "Kraken", "MAXPACK.IMAGE", "fakelib2", "P5Z", "kraken_zero.dat"] {
            assert!(reserved(n), "{n}");
        }
        for n in ["", "done", ".done/x", "fakelibs", "sce_sys", "eboot.bin", "ampr_emu.index", "ampr_zstd.index"] {
            assert!(!reserved(n), "{n}");
        }
        assert_eq!(below("fakelib/libc.prx", FAKELIB), Some("libc.prx"));
        assert_eq!(below("fakelib/a/b", FAKELIB), Some("a/b"));
        assert_eq!(below("fakelib2/x", FAKELIB), None);
        assert_eq!(below("fakelib", FAKELIB), None);
        assert_eq!(below("fakelibx/y", FAKELIB), None);
    }
}
