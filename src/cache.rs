//! Per-game cache /data/maxpack/caches/<TID> (DESIGN.md "Cache"). Folder: fakelib/ only.
//! FPKG: the cache is /app0: sce_sys, eboot.bin and every PRX and ELF extracted, fakelib/, kraken/ (launch.rs
//! mounts the pkg's folder there), the two AMPR indexes; `.done` (JSON stamp with each input's size and
//! mtime at the build's start, its own mtime = build start) last.
//! .p5z: the cache is /app0 too: its loose files extracted, fakelib/, p5z/ (the .p5z's folder goes
//! there) and maxpack.image (where ng finds each volume inside the .p5z); no index of ours.
//! fakelib/ (every kind) = the game's or backport's libs, then /data/maxpack/ampr/* copied on
//! top (inject_ampr; always for a pkg or a .p5z); in between, the bundled DLC (dlc_emu) and PlayGo (pgo_stub)
//! emulators when the game needs one and ships none (add_emulators; no pgo_stub for playgo_off). A change of the ampr files,
//! inject_ampr or the emulator inputs only recomposes fakelib/ (refresh_fakelib), no full rebuild.
//! /app0 is writable: files the game writes here stay across rebuilds (`.built` lists what a build
//! made, a rebuild removes only that); purge() deletes them too.
//! /data/maxpack/ampr is never copied here: launch.rs mounts it as its own layer.
use crate::ampr_index::{self, Chunk, IndexFile, Kraken};
use crate::config::Settings;
use crate::scan::{Kind, Source};
use kraken_mount_pkg::{Entry, Pkg, PRead};
use maxpack_logic::fresh::{self, Input};
use maxpack_logic::p5z::{image_text, loose_dest, space_short, LooseDest};
use std::collections::{BTreeMap, HashSet};
use std::fs::{self, File};
use std::io::{Error, ErrorKind, Result, Write};
use std::os::unix::fs::FileExt;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

pub const CACHES: &str = "/data/maxpack/caches";
pub const AMPR_DIR: &str = "/data/maxpack/ampr";
pub const DONE: &str = ".done";
/// The game lib dirs, SMP priority first. fakelib2/ is never a game file.
const LIB_DIRS: [&str; 2] = ["fakelib2/", "fakelib/"];
/// What the last build created, one path per line relative to the cache dir (written before `.done`).
const BUILT: &str = ".built";
/// What fakelib/ was composed from (JSON): {"ampr": ampr_state, "emus": emu_state}.
const AMPR_STATE: &str = ".ampr";
/// Bundled third-party emulators (GPL-3.0, THIRD_PARTY_NOTICES), released files (ps5/build.sh checks
/// SHA256SUMS): drakmor/dlc_emu (DLC; its 3 files go together) and drakmor/pgo_stub (PlayGo).
const DLC_EMU_VERSION: &str = include_str!("../vendor/dlc_emu/VERSION");
const PGO_STUB_VERSION: &str = include_str!("../vendor/pgo_stub/VERSION");
/// (file name, contents, project, is the DLC set).
const EMUS: [(&str, &[u8], &str, bool); 4] = [
    ("libSceAppContent.sprx", include_bytes!("../vendor/dlc_emu/libSceAppContent.sprx"), "dlc_emu", true),
    ("libSceNpEntitlementAccess.sprx", include_bytes!("../vendor/dlc_emu/libSceNpEntitlementAccess.sprx"), "dlc_emu", true),
    ("libSceGameUpdate.sprx", include_bytes!("../vendor/dlc_emu/libSceGameUpdate.sprx"), "dlc_emu", true),
    ("libScePlayGo.sprx", include_bytes!("../vendor/pgo_stub/libScePlayGo.sprx"), "pgo_stub", false),
];
/// dlc_emu's config at the game root: its presence asks for the DLC set.
const DLC_INI: &str = "dlc_emu.ini";
/// An import of libScePlayGo in a decrypted eboot.bin: its module name as a plain string.
const PLAYGO_NEEDLE: &[u8] = b"libScePlayGo";
/// pkg caches: the mount point for the pkg's folder at /app0/kraken (launch.rs).
pub const PKG_MOUNT_DIR: &str = "kraken";
/// pkg caches: one line, no newline, the .pkg relative to /app0 (`kraken/<file name>`); ng opens /app0/ + it.
const IMAGE_FILE: &str = "kraken.image";
/// .p5z caches: the mount point for the .p5z's folder at /app0/p5z (launch.rs).
pub const P5Z_MOUNT_DIR: &str = maxpack_logic::p5z::MOUNT_DIR;
/// .p5z caches: the .p5z relative to /app0 and where each volume is in it (maxpack_logic::p5z::image_text).
const P5Z_IMAGE_FILE: &str = maxpack_logic::p5z::IMAGE_FILE;
const CHUNK: usize = 4 << 20;

/// The .pkg file as a kraken_mount_pkg reader (newtype: the orphan rule forbids `impl PRead for File`).
struct Io(File);

impl PRead for Io {
    fn pread(&self, off: u64, buf: &mut [u8]) -> std::result::Result<usize, kraken_mount_pkg::Error> {
        loop {
            match self.0.read_at(buf, off) {
                Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                r => return r.map_err(|e| kraken_mount_pkg::Error::Io(e.raw_os_error().unwrap_or(-1))),
            }
        }
    }
}

fn bad(msg: String) -> Error {
    Error::new(ErrorKind::InvalidData, msg)
}

fn vfs(e: kraken_mount_pkg::Error) -> Error {
    match e {
        kraken_mount_pkg::Error::Io(code) if code > 0 => Error::from_raw_os_error(code),
        e => bad(e.to_string()),
    }
}

/// Files copied out of the pkg: sce_sys/**, eboot.bin, any *.prx / *.sprx, any *.elf outside
/// decrypted/ (an app can exit-spawn an ELF from a subfolder, which SceSysCore loads from disk;
/// dump-forge's decrypted/ copy is never run), and the root-level
/// dlc_emu.ini / playgo_stub.dat (the emulators fopen /app0/<name>).
/// ponytail: no STAMP_V bump for the last two: an older cache lacks them loose, ng still serves them
/// from the pkg through the index.
fn extracted(path: &str) -> bool {
    let lower = path.to_ascii_lowercase();
    lower.starts_with("sce_sys/")
        || lower == "eboot.bin"
        || lower.ends_with(".prx")
        || lower.ends_with(".sprx")
        || (lower.ends_with(".elf") && !lower.starts_with("decrypted/"))
        || lower == DLC_INI
        || lower == "playgo_stub.dat"
}

fn open(pkg: &Path) -> Result<(Io, Pkg)> {
    let file = File::open(pkg)?;
    let len = file.metadata()?.len();
    let io = Io(file);
    let p = Pkg::open(&io, len).map_err(vfs)?;
    Ok((io, p))
}

/// titleId, name and version from the pkg's sce_sys/param.json.
pub fn peek_pkg(pkg: &Path) -> Result<(String, String, String)> {
    let (io, p) = open(pkg)?;
    let param = p.lookup("sce_sys/param.json").ok_or_else(|| bad("no sce_sys/param.json".into()))?;
    crate::scan::parse_param(&read_all(&p, &io, &param)?)
}

/// One file of the pkg, e.g. "sce_sys/icon0.png" (images of a pkg whose cache is not built).
pub fn pkg_file(pkg: &Path, name: &str) -> Result<Vec<u8>> {
    let (io, p) = open(pkg)?;
    let e = p.lookup(name).ok_or_else(|| Error::new(ErrorKind::NotFound, format!("{}: no {name}", pkg.display())))?;
    read_all(&p, &io, &e)
}

fn read_all<R: PRead>(p: &Pkg, io: &R, e: &Entry) -> Result<Vec<u8>> {
    let mut buf = vec![0u8; e.size as usize];
    let n = p.read_at(io, e, 0, &mut buf).map_err(vfs)?;
    buf.truncate(n);
    Ok(buf)
}

/// Copies `e` to `dest` in CHUNK-sized reads.
fn extract<R: PRead>(p: &Pkg, io: &R, e: &Entry, dest: &Path, buf: &mut [u8]) -> Result<()> {
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)?;
    }
    let mut out = File::create(dest)?;
    let mut off = 0u64;
    while off < e.size {
        let n = p.read_at(io, e, off, buf).map_err(vfs)?;
        if n == 0 {
            return Err(bad(format!("{}: short read at {off}", e.path)));
        }
        out.write_all(&buf[..n])?;
        off += n as u64;
    }
    drop(out);
    exec_mode(dest)
}

/// 0777, like SMP's fakelib copies: the PS5 loader refuses a module whose file has no
/// execute bits ("mount flag / attribute error", va_mode 0666).
fn exec_mode(path: &Path) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(path, fs::Permissions::from_mode(0o777))
}

/// `e`'s Kraken blocks as index chunks (a sparse hole stays a zero chunk: stored_len 0).
fn chunks(p: &Pkg, e: &Entry) -> Result<Vec<Chunk>> {
    let blocks = p.blocks(e).map_err(vfs)?;
    let mut out = Vec::with_capacity(blocks.len());
    for b in blocks {
        let kraken = b.compressed.then_some(Kraken { even_len: b.desc.even_len, mode: b.desc.mode });
        out.push(Chunk { pkg_off: b.pkg_off, stored_len: b.stored_len, raw_len: b.raw_len, kraken });
    }
    Ok(out)
}

/// Every entry under `dir` (dirs included), recursive, symlinks not followed. Missing dir: empty.
fn walk(dir: &Path) -> Vec<(PathBuf, fs::Metadata)> {
    let mut out = Vec::new();
    let mut todo = vec![dir.to_path_buf()];
    while let Some(d) = todo.pop() {
        let Ok(rd) = fs::read_dir(&d) else { continue };
        for e in rd.flatten() {
            let Ok(m) = fs::symlink_metadata(e.path()) else { continue };
            if m.is_dir() {
                todo.push(e.path());
            }
            out.push((e.path(), m));
        }
    }
    out
}

/// Removes everything inside `dir`, keeping `dir` (it may be a nullfs source).
fn empty_dir(dir: &Path) -> Result<()> {
    for entry in fs::read_dir(dir)? {
        let path = entry?.path();
        if fs::symlink_metadata(&path)?.is_dir() {
            fs::remove_dir_all(&path)?;
        } else {
            fs::remove_file(&path)?;
        }
    }
    Ok(())
}

/// Removes what the last build listed in `.built`, keeping files the game wrote; a dir goes only
/// when it ends up empty. A cache without `.built` (an older build) is emptied.
fn clear_build(dir: &Path) -> Result<()> {
    let Ok(list) = fs::read_to_string(dir.join(BUILT)) else { return empty_dir(dir) };
    // Recomposed after the build too (refresh_fakelib), so .built may not list all of it: always ours.
    match fs::remove_dir_all(dir.join("fakelib")) {
        Err(e) if e.kind() != ErrorKind::NotFound => return Err(e),
        _ => {}
    }
    let mut dirs = Vec::new();
    for rel in list.lines().filter(|l| !l.is_empty()) {
        // Our own file, but never step outside the cache.
        if Path::new(rel).components().any(|c| !matches!(c, std::path::Component::Normal(_))) {
            continue;
        }
        let path = dir.join(rel);
        match fs::symlink_metadata(&path) {
            Ok(m) if m.is_dir() => dirs.push(path),
            Ok(_) => fs::remove_file(&path)?,
            Err(_) => {}
        }
    }
    dirs.sort_by_key(|d| std::cmp::Reverse(d.components().count()));
    for d in dirs {
        let _ = fs::remove_dir(&d); // fails while it still holds game files: kept
    }
    for name in [BUILT, DONE] {
        match fs::remove_file(dir.join(name)) {
            Err(e) if e.kind() != ErrorKind::NotFound => return Err(e),
            _ => {}
        }
    }
    Ok(())
}

/// Paths under `dir` that were not there before the build (`before`), deepest dirs last.
fn created(dir: &Path, before: &HashSet<PathBuf>) -> Vec<(PathBuf, bool)> {
    walk(dir).into_iter().filter(|(p, _)| !before.contains(p)).map(|(p, m)| (p, m.is_dir())).collect()
}

/// Removes what a failed build made, keeping the game's files.
fn remove_created(dir: &Path, before: &HashSet<PathBuf>) {
    let mut new = created(dir, before);
    new.sort_by_key(|(p, _)| std::cmp::Reverse(p.components().count()));
    for (p, is_dir) in new {
        let _ = if is_dir { fs::remove_dir(&p) } else { fs::remove_file(&p) };
    }
}

/// Of a built pkg or .p5z cache (its `.done` stamp's "kind" and "source"): the folder below /app0
/// where launch.rs mounts the source file's folder (PKG_MOUNT_DIR or P5Z_MOUNT_DIR), and that file.
/// None for a folder game or without a stamp.
pub fn mount_source(tid: &str) -> Option<(&'static str, String)> {
    let stamp: serde_json::Value = serde_json::from_slice(&fs::read(dir(tid).join(DONE)).ok()?).ok()?;
    let at = match stamp["kind"].as_str()? {
        "pkg" => PKG_MOUNT_DIR,
        "p5z" => P5Z_MOUNT_DIR,
        _ => return None,
    };
    Some((at, stamp["source"].as_str()?.to_string()))
}

pub fn dir(tid: &str) -> PathBuf {
    Path::new(CACHES).join(tid)
}

pub fn app0(src: &Source) -> PathBuf {
    match src.kind {
        Kind::Pkg | Kind::P5z => dir(&src.title_id),
        Kind::Folder(_) => src.path.clone(),
    }
}

pub fn sce_sys(src: &Source) -> PathBuf {
    app0(src).join("sce_sys")
}

fn secs(t: SystemTime) -> u64 {
    t.duration_since(SystemTime::UNIX_EPOCH).map_or(0, |d| d.as_secs())
}

pub fn built_at(tid: &str) -> Option<u64> {
    fs::metadata(dir(tid).join(DONE)).and_then(|m| m.modified()).ok().map(secs)
}

pub fn du(path: &Path) -> u64 {
    walk(path).iter().filter(|(_, m)| m.is_file()).map(|(_, m)| m.len()).sum()
}

pub fn size(tid: &str) -> u64 {
    du(&dir(tid))
}

pub fn total_size() -> u64 {
    du(Path::new(CACHES))
}

/// Deletes caches/<TID> not in `keep`; empties the kept ones so they rebuild. Stops before the next
/// title once `go_on()` is false (MaxPack must stop mounting: another mounter started, whose mount
/// may be at /system_ex/app/<TID>, or kstuff went away); Ok(false) then, Ok(true) when done.
pub fn purge(keep: &[String], go_on: &dyn Fn() -> bool) -> Result<bool> {
    let Ok(rd) = fs::read_dir(CACHES) else { return Ok(true) };
    for e in rd.flatten() {
        if !go_on() {
            return Ok(false);
        }
        let path = e.path();
        let name = e.file_name().to_string_lossy().into_owned();
        let r = if !keep.contains(&name) {
            // A pkg cache is app0: drop its /system_ex/app stack first, or it keeps the deleted inode.
            if let Err(err) = crate::register::unmount_title(&name) {
                log!("{name}: purge skipped {}: unmount: {err}", path.display());
                continue;
            }
            fs::remove_dir_all(&path)
        } else {
            empty_dir(&path)
        };
        if let Err(err) = r {
            log!("purge {}: {err}", path.display());
            return Err(err);
        }
    }
    Ok(true)
}

/// `mode` with the `want` bits added, or None when none is missing (fix_access only adds bits).
fn add_bits(mode: u32, want: u32) -> Option<u32> {
    let mode = mode & 0o7777;
    (mode & want != want).then_some(mode | want)
}

/// fix_access's tally: modes fixed, chmods failed and the first failure.
#[derive(Default)]
struct Access {
    fixed: u64,
    failed: u64,
    first: Option<(PathBuf, Error)>,
}

impl Access {
    /// Adds `dirs` to a dir, `files` to a regular file; anything else (symlinks: never followed) is skipped.
    fn entry(&mut self, path: &Path, m: &fs::Metadata, dirs: u32, files: u32) {
        use std::os::unix::fs::{MetadataExt, PermissionsExt};
        let want = if m.is_dir() { dirs } else if m.is_file() { files } else { return };
        let Some(mode) = add_bits(m.mode(), want) else { return };
        match fs::set_permissions(path, fs::Permissions::from_mode(mode)) {
            Ok(()) => self.fixed += 1,
            Err(e) => {
                self.failed += 1;
                self.first.get_or_insert((path.to_path_buf(), e));
            }
        }
    }

    /// `path` itself (lstat), then everything under it when it is a dir.
    fn tree(&mut self, path: &Path, dirs: u32, files: u32) {
        let Ok(m) = fs::symlink_metadata(path) else { return };
        self.entry(path, &m, dirs, files);
        if m.is_dir() {
            for (p, m) in walk(path) {
                self.entry(&p, &m, dirs, files);
            }
        }
    }
}

/// Opens up `src`'s files for the game and the system services (save data, trophies), which read
/// them with another uid than the payload. Only adds bits, only chmods when some are missing,
/// never follows symlinks: the cache dir (dirs 0777: /app0 is writable; files +0444); a pkg's
/// .pkg (+0444) and its folder (+0555, the /app0/kraken nullfs; ng falls back to LZ4 mode when it
/// cannot open the .pkg), a .p5z and its folder the same (the /app0/p5z nullfs); a folder game's
/// tree (dirs +0555, files +0444). Never fails: one log line per title when something was fixed or
/// failed (exFAT USB, read-only), with the first failure.
/// ponytail: one lstat walk of the cache and the game folder per run (main.rs calls it once per
/// title); a 150k-file folder costs that walk, and files added later wait for the next run.
pub fn fix_access(src: &Source) {
    let mut a = Access::default();
    a.tree(&dir(&src.title_id), 0o777, 0o444);
    match src.kind {
        Kind::Pkg | Kind::P5z => {
            if let Ok(m) = fs::symlink_metadata(&src.path) {
                a.entry(&src.path, &m, 0, 0o444);
            }
            if let Some(parent) = src.path.parent() {
                if let Ok(m) = fs::symlink_metadata(parent) {
                    a.entry(parent, &m, 0o555, 0);
                }
            }
        }
        Kind::Folder(_) => a.tree(&src.path, 0o555, 0o444),
    }
    if a.fixed + a.failed == 0 {
        return;
    }
    let first = a.first.map_or(String::new(), |(p, e)| format!("; first failure {}: {e}", p.display()));
    log!("{}: access ({}): {} modes fixed, {} failed{first}", src.title_id, src.path.display(), a.fixed, a.failed);
}

/// The backport's lib dir in SMP order (sm_fakelib.c resolve_game_fakelib_source_for_path):
/// fakelib2 (exclusive), then fakelib.
fn bp_lib(src: &Source) -> Option<PathBuf> {
    let b = src.backport.as_ref()?;
    ["fakelib2", "fakelib"].iter().map(|d| b.join(d)).find(|p| p.is_dir())
}

/// The fakelib dir on disk the cache's fakelib/ is built from: backport fakelib2/fakelib, then
/// a folder game's fakelib2/fakelib. None for a pkg or .p5z without one (its libs are inside the file).
fn lib_dir(src: &Source) -> Option<PathBuf> {
    bp_lib(src).or_else(|| match src.kind {
        Kind::Pkg | Kind::P5z => None,
        Kind::Folder(_) => ["fakelib2", "fakelib"].iter().map(|d| src.path.join(d)).find(|p| p.is_dir()),
    })
}

/// The cache layout version in the stamp. 2: the pkg reader also extracts container-only sce_sys
/// files (save_data.png, pic0/pic1, snd0.at9, trophy00.ucp, nptitle.dat, npbind.dat), and files the
/// index cannot map are extracted; every v1 cache rebuilds once. 3: *.elf outside decrypted/ extracted too.
/// 4: `.done` records each input's size and mtime (`fresh::KEY`) and `check` compares them for
/// equality, no longer an input's mtime against `.done`'s (a console clock behind the PC that wrote
/// the drive made such a cache stale on every pass); every v3 cache rebuilds once.
const STAMP_V: u64 = 4;

pub fn stamp(src: &Source, _s: &Settings) -> serde_json::Value {
    serde_json::json!({
        "v": STAMP_V,
        "kind": match src.kind {
            Kind::Pkg => "pkg",
            Kind::Folder(_) => "folder",
            Kind::P5z => "p5z",
        },
        "source": src.path.to_string_lossy(),
        "size": src.size,
        "backport": src.backport.as_ref().map(|b| b.to_string_lossy()),
        // Which lib dir was used: a removed or newly added fakelib/fakelib2 changes it.
        "fakelib": lib_dir(src).map(|p| p.to_string_lossy().into_owned()),
    })
}

/// Existing paths whose size and mtime `.done` records at the build's start (`record_inputs`): any
/// change, an input added or one removed rebuilds (`check`). The ampr dir is not one: launch mounts it.
pub fn inputs(src: &Source, _s: &Settings) -> Vec<PathBuf> {
    let mut v = Vec::new();
    match src.kind {
        Kind::Pkg | Kind::P5z => {
            v.push(src.path.clone());
            // The whole backport is copied into a pkg or .p5z cache.
            if let Some(b) = &src.backport {
                v.push(b.clone());
                v.extend(walk(b).into_iter().map(|e| e.0));
            }
        }
        // ponytail: only the lib dir of a folder's backport feeds its cache; the rest is a live unionfs.
        // Not the game folder itself: the game writes there (/app0 is writable), which must not rebuild.
        Kind::Folder(_) => {
            v.extend([src.path.join("eboot.bin"), src.path.join("sce_sys/param.json")]);
            v.retain(|p| p.exists());
            if let Some(lib) = lib_dir(src) {
                v.extend(walk(&lib).into_iter().map(|e| e.0));
                v.push(lib);
            }
        }
    }
    v
}

/// `inputs` as they are now: each path with its (size, mtime ns), or None when its lstat fails.
fn input_states(src: &Source, s: &Settings) -> Vec<(String, Option<(u64, i64)>)> {
    inputs(src, s)
        .into_iter()
        .map(|p| {
            let state = fs::symlink_metadata(&p).ok().and_then(|m| Some((m.len(), fresh::ns(m.modified().ok()?))));
            (p.to_string_lossy().into_owned(), state)
        })
        .collect()
}

/// What `.done` records of the inputs, taken before a build reads them: an input that changes during
/// the build differs from the record, so the next check rebuilds.
fn record_inputs(src: &Source, s: &Settings) -> Vec<Input> {
    input_states(src, s)
        .into_iter()
        .filter_map(|(path, state)| state.map(|(size, mtime_ns)| Input { path, size, mtime_ns }))
        .collect()
}

pub enum Fresh {
    Fresh,
    Missing,
    Stale(String),
    /// Only fakelib/ is out of date (the ampr files, inject_ampr or emu_state changed, a legacy game_fakelib
    /// stamp): refresh_fakelib.
    Fakelib(String),
    /// The same game moved from this old source path (the old path is gone): relink, no rebuild.
    Moved(PathBuf),
}

/// `.done` key (pkg only, not part of `stamp`): `pkg_id` of the .pkg the cache was built from.
const PKG_ID: &str = "pkg_id";
const ID_SPAN: usize = 0x10000;

/// A .pkg's identity: FNV-1a 64 of its size, first 64 KiB (the FIH header), the 4 KiB CNT header
/// (content id, digests) and last 64 KiB. ponytail: a sample, not a full hash (128 KiB read, not the
/// whole pkg); together with the old path being gone it tells a moved pkg from another build.
pub fn pkg_id(pkg: &Path) -> Result<String> {
    let f = File::open(pkg)?;
    let len = f.metadata()?.len();
    let mut v = len.to_le_bytes().to_vec();
    let mut feed = |b: &[u8]| v.extend_from_slice(b);
    let mut buf = vec![0u8; ID_SPAN];
    let head = ID_SPAN.min(len as usize);
    f.read_exact_at(&mut buf[..head], 0)?;
    feed(&buf[..head]);
    let cnt = buf.get(0x58..0x60).map_or(0, |b| u64::from_le_bytes(b.try_into().unwrap()));
    if cnt.checked_add(0x1000).is_some_and(|e| e <= len) {
        f.read_exact_at(&mut buf[..0x1000], cnt)?;
        feed(&buf[..0x1000]);
    }
    let tail = ID_SPAN.min(len as usize);
    f.read_exact_at(&mut buf[..tail], len - tail as u64)?;
    feed(&buf[..tail]);
    Ok(crate::register::fnv1a(&v))
}

/// `.done` key (folder only, not part of `stamp`): `folder_id` of the folder the cache was built from.
const FOLDER_ID: &str = "folder_id";
/// `.done` key (.p5z only, not part of `stamp`): `p5z::id` of the .p5z the cache was built from.
const P5Z_ID: &str = "p5z_id";

/// A folder game's identity: FNV-1a 64 of each input file's path (relative to the folder) and
/// size, plus the bytes of every input but eboot.bin (param.json and the lib dir's files, which
/// the cache's fakelib/ is a copy of). ponytail: eboot.bin by size only (it can be large) and no
/// mtimes (a copy to exFAT rounds them); another dump differs in eboot.bin's size, a lib or param.json.
fn folder_id(src: &Source, s: &Settings) -> String {
    let mut v = Vec::new();
    let mut feed = |b: &[u8]| v.extend_from_slice(b);
    let eboot = src.path.join("eboot.bin");
    let mut files = inputs(src, s);
    files.sort();
    for p in files {
        let Ok(m) = fs::symlink_metadata(&p) else { continue };
        if m.is_file() {
            feed(p.strip_prefix(&src.path).unwrap_or(&p).to_string_lossy().as_bytes());
            feed(&m.len().to_le_bytes());
            if p != eboot {
                feed(&fs::read(&p).unwrap_or_default());
            }
        }
    }
    crate::register::fnv1a(&v)
}

/// Whether stamp `new` is `old` with only the source's location changed: every key equal but
/// "source" (and "fakelib" when it is the game's own lib dir under the moved source).
fn only_moved(old: &serde_json::Value, new: &serde_json::Value) -> bool {
    let (Some(from), Some(to)) = (old["source"].as_str(), new["source"].as_str()) else { return false };
    if from == to {
        return false;
    }
    new.as_object().into_iter().flatten().all(|(k, v)| match k.as_str() {
        "source" => true,
        "fakelib" if old.get(k) != Some(v) => {
            let rebased = old[k].as_str().and_then(|p| p.strip_prefix(from)).map(|rest| format!("{to}{rest}"));
            rebased.is_some() && rebased.as_deref() == v.as_str()
        }
        _ => old.get(k) == Some(v),
    })
}

/// The move decision: a relink only when the stamps differ by location alone, the old source is
/// gone (a copy while the old one still exists is a duplicate), and the files are the same (pkg:
/// `pkg_id` equal; .p5z: `p5z_id` equal; folder: `folder_id` equal).
fn is_move(old: &serde_json::Value, new: &serde_json::Value, old_exists: bool, same_files: impl FnOnce() -> bool) -> bool {
    only_moved(old, new) && !old_exists && same_files()
}

/// `is_move`'s file check. ponytail: a cache built before `pkg_id` was stamped is rebuilt once
/// when its pkg moves.
fn same_files(src: &Source, s: &Settings, old: &serde_json::Value) -> bool {
    let (key, id): (_, fn(&Path) -> Result<String>) = match src.kind {
        // folder_id covers the bytes of everything a folder cache is made from (param.json, the
        // lib dir), so a copy with new mtimes (copy, then delete the old one) still relinks.
        Kind::Folder(_) => return old[FOLDER_ID].as_str() == Some(folder_id(src, s).as_str()),
        Kind::Pkg => (PKG_ID, pkg_id),
        Kind::P5z => (P5Z_ID, crate::p5z::id),
    };
    // The moved file itself has another path and may have another mtime (a copy to another drive):
    // its id decides. The other inputs (the backport, copied into the cache) must be as `.done`
    // recorded them (fresh::changed: equality, never against `.done`'s mtime).
    let from = old["source"].as_str().unwrap_or_default();
    let was: Vec<Input> = fresh::recorded(old).into_iter().filter(|i| i.path != from).collect();
    let to = src.path.to_string_lossy();
    let now: Vec<_> = input_states(src, s).into_iter().filter(|(p, _)| *p != to).collect();
    fresh::changed(&was, &now).is_none()
        && old[key].as_str().is_some_and(|old_id| id(&src.path).ok().as_deref() == Some(old_id))
}

/// The ampr files fakelib/ should carry: {name: [size, mtime]} of /data/maxpack/ampr's
/// regular files (libkernel.sprx never copied), or null when `injects_ampr` is false.
fn ampr_state(src: &Source, s: &Settings) -> serde_json::Value {
    if !injects_ampr(src, s) {
        return serde_json::Value::Null;
    }
    let mut m = serde_json::Map::new();
    for e in fs::read_dir(AMPR_DIR).into_iter().flatten().flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        let Ok(md) = fs::metadata(e.path()) else { continue };
        if md.is_file() && ampr_copied(&name) {
            m.insert(name, serde_json::json!([md.len(), md.modified().map(secs).unwrap_or(0)]));
        }
    }
    serde_json::Value::Object(m)
}

/// Whether fakelib/ gets the ampr dir's files: with inject_ampr, and always for a pkg (#73) or a
/// .p5z: pkg and p5z modes exist only in our ampr-emu-ng, the game's own libSceAmpr.sprx cannot
/// read the file named in kraken.image or maxpack.image.
fn injects_ampr(src: &Source, s: &Settings) -> bool {
    s.inject_ampr || matches!(src.kind, Kind::Pkg | Kind::P5z)
}

/// libkernel.sprx from the ampr dir would replace the system one for every game: never copied.
fn ampr_copied(name: &str) -> bool {
    !name.eq_ignore_ascii_case("libkernel.sprx")
}

/// The entry of `dir` named `name`, compared case-insensitively. Missing dir: None.
fn find_ci(dir: &Path, name: &str) -> Option<PathBuf> {
    fs::read_dir(dir).into_iter().flatten().flatten()
        .find(|e| e.file_name().to_string_lossy().eq_ignore_ascii_case(name))
        .map(|e| e.path())
}

/// Known older releases of the bundled emulators, (file name, len, crc32), each in its two builds:
/// dlc_emu 0.1a and 0.2, pgo_stub 0.4 and 0.5. A fakelib copy matching one is upgraded.
/// ponytail: the binaries carry no version string, so only these known releases are upgraded; any
/// other copy (custom, the bundled release, a log build of it) is left alone: no downgrade.
const OLD_EMUS: [(&str, u64, u32); 14] = [
    ("libSceAppContent.sprx", 39366, 0xcb58_8874),
    ("libSceAppContent.sprx", 31174, 0x641c_afda),
    ("libSceNpEntitlementAccess.sprx", 37414, 0x548e_8426),
    ("libSceNpEntitlementAccess.sprx", 33206, 0x4b1d_e94a),
    ("libSceAppContent.sprx", 35558, 0x518e_6f93),
    ("libSceAppContent.sprx", 28966, 0x1c74_5846),
    ("libSceGameUpdate.sprx", 10326, 0x4820_0ddd),
    ("libSceGameUpdate.sprx", 9110, 0x226b_c515),
    ("libSceNpEntitlementAccess.sprx", 37750, 0xaf0f_f584),
    ("libSceNpEntitlementAccess.sprx", 33222, 0xebad_2e15),
    ("libScePlayGo.sprx", 17430, 0x9bdb_d12e),
    ("libScePlayGo.sprx", 11590, 0x3163_6f06),
    ("libScePlayGo.sprx", 20582, 0x46bc_f017),
    ("libScePlayGo.sprx", 14390, 0xb801_64e7),
];

/// Whether `data`, a file named `name` (case-insensitive), is a known older release (OLD_EMUS).
fn old_emu(name: &str, data: &[u8]) -> bool {
    OLD_EMUS.iter().any(|&(n, len, crc)| {
        n.eq_ignore_ascii_case(name) && data.len() as u64 == len && ampr_index::crc32(&[data]) == crc
    })
}

/// old_emu for the file at `path`, read only when its size is a known one.
fn old_emu_file(path: &Path, name: &str) -> bool {
    let Ok(len) = fs::metadata(path).map(|m| m.len()) else { return false };
    OLD_EMUS.iter().any(|&(n, l, _)| l == len && n.eq_ignore_ascii_case(name))
        && fs::read(path).is_ok_and(|d| old_emu(name, &d))
}

/// ng's library file name, in the ampr dir and in a game's fakelib.
const AMPR_LIB: &str = "libSceAmpr.sprx";
/// ng's embedded identity, followed by its X.Y.Z version (plain bytes).
const NG_ID: &[u8] = b"ampr-emu-ng ";

/// The version text of the first `ampr-emu-ng X.Y.Z` identity of `data` that parses
/// (crate::parse_version); None for no identity (upstream ampr_emu, anything else).
fn ng_version(data: &[u8]) -> Option<&str> {
    data.windows(NG_ID.len()).enumerate().filter(|(_, w)| *w == NG_ID).find_map(|(i, _)| {
        let rest = &data[i + NG_ID.len()..];
        let end = rest.iter().position(|b| !(b.is_ascii_digit() || *b == b'.' || *b == b'v')).unwrap_or(rest.len());
        std::str::from_utf8(&rest[..end]).ok().filter(|v| crate::parse_version(v).is_some())
    })
}

/// Whether the game's own ng is newer than ours (crate::newer): `game` must be an ampr-emu-ng, an
/// `ours` without identity is older than any.
fn game_ng_newer(game: Option<&str>, ours: Option<&str>) -> bool {
    crate::newer(game, ours)
}

/// Whether to keep the game's or backport's libSceAmpr.sprx in `fakelib` over AMPR_DIR's: only an
/// ampr-emu-ng strictly newer than AMPR_DIR's (logged). ponytail: a hand-placed ampr build in
/// AMPR_DIR also loses to a newer game copy.
fn keep_game_ng(tid: &str, fakelib: &Path) -> bool {
    let (Some(game), Some(ours)) = (find_ci(fakelib, AMPR_LIB), find_ci(Path::new(AMPR_DIR), AMPR_LIB)) else {
        return false;
    };
    let v = |p: &Path| fs::read(p).ok().and_then(|d| ng_version(&d).map(str::to_owned));
    let (g, o) = (v(&game), v(&ours));
    let keep = game_ng_newer(g.as_deref(), o.as_deref());
    if keep {
        let fmt = |v: &Option<String>| v.clone().unwrap_or_else(|| "an unknown build".into());
        log!("{tid}: fakelib: kept the game's ampr-emu-ng {} (newer than {})", fmt(&g), fmt(&o));
    }
    keep
}

/// dlc_emu.ini at a folder game's root or the backport's root (a pkg's or .p5z's own is checked in compose_fakelib).
/// Exact name: dlc_emu opens /app0/dlc_emu.ini, and the internal SSD is case-sensitive.
fn dlc_ini_on_disk(src: &Source) -> bool {
    (matches!(src.kind, Kind::Folder(_)) && src.path.join(DLC_INI).is_file())
        || src.backport.as_ref().is_some_and(|b| b.join(DLC_INI).is_file())
}

/// The eboot.bin searched for a PlayGo import: the backport's if it has one, else the folder's,
/// else (pkg, .p5z) the cache's extracted copy.
fn eboot(src: &Source) -> PathBuf {
    match &src.backport {
        Some(b) if b.join("eboot.bin").is_file() => b.join("eboot.bin"),
        _ if matches!(src.kind, Kind::Pkg | Kind::P5z) => dir(&src.title_id).join("eboot.bin"),
        _ => src.path.join("eboot.bin"),
    }
}

/// What the emulator choice depends on, stat only (no eboot read): null without add_emulators.
/// ponytail: a pkg's own dlc_emu.ini is not here; a pkg change rebuilds through the stamp.
fn emu_state(src: &Source, s: &Settings) -> serde_json::Value {
    if !s.add_emulators {
        return serde_json::Value::Null;
    }
    let eboot = fs::metadata(eboot(src)).ok().map(|m| (m.len(), m.modified().map(secs).unwrap_or(0)));
    serde_json::json!({
        "dlc_emu": DLC_EMU_VERSION.trim(),
        "pgo_stub": PGO_STUB_VERSION.trim(),
        "dlc_ini": dlc_ini_on_disk(src),
        "eboot": eboot,
        "playgo_off": playgo_off(src, s),
    })
}

/// Whether the PlayGo stub is turned off for this title (the playgo_off setting).
fn playgo_off(src: &Source, s: &Settings) -> bool {
    s.playgo_off.iter().any(|t| *t == src.title_id)
}

/// The memoized "the eboot imports PlayGo" (AMPR_STATE "playgo"[2]) of title `tid`; None when unknown.
/// ponytail: the memo as last written, not checked against the eboot now.
pub fn playgo_import(tid: &str) -> Option<bool> {
    let b = fs::read(dir(tid).join(AMPR_STATE)).ok()?;
    serde_json::from_slice::<serde_json::Value>(&b).ok()?["playgo"][2].as_bool()
}

/// The AMPR_STATE contents for `src` now.
fn fakelib_state(src: &Source, s: &Settings) -> serde_json::Value {
    serde_json::json!({"ampr": ampr_state(src, s), "emus": emu_state(src, s)})
}

/// Whether `r` contains `needle` (non-empty), read in `chunk`-sized pieces that overlap by
/// needle.len() - 1 bytes, so a match across two reads is found.
fn contains(r: &mut impl std::io::Read, needle: &[u8], chunk: usize) -> Result<bool> {
    let keep = needle.len().saturating_sub(1);
    let mut buf = vec![0u8; chunk + keep];
    let mut have = 0;
    loop {
        let n = match r.read(&mut buf[have..]) {
            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
            r => r?,
        };
        if n == 0 {
            return Ok(false);
        }
        let end = have + n;
        if buf[..end].windows(needle.len()).any(|w| w == needle) {
            return Ok(true);
        }
        let k = keep.min(end);
        buf.copy_within(end - k..end, 0);
        have = k;
    }
}

/// Whether the game imports libScePlayGo. ponytail: only eboot.bin is searched, a PlayGo import in
/// another prx module is missed; an encrypted eboot never matches (no stub, the safe side).
/// A missing eboot: false. A read error: logged with the title id and path, None (taken as false,
/// not memoized).
fn wants_playgo(src: &Source) -> Option<bool> {
    let path = eboot(src);
    let found = File::open(&path).and_then(|mut f| contains(&mut f, PLAYGO_NEEDLE, CHUNK));
    match found {
        Ok(b) => Some(b),
        Err(e) if e.kind() == ErrorKind::NotFound => Some(false),
        Err(e) => {
            log!("{}: {}: PlayGo check: {e}", src.title_id, path.display());
            None
        }
    }
}

/// wants_playgo, memoized in AMPR_STATE's "playgo" as [len, mtime secs, result] of the eboot:
/// `had` is the previous AMPR_STATE, `state` the one being written (gets the memo).
/// ponytail: an eboot rewritten with the same size and second keeps the old result.
fn wants_playgo_memo(src: &Source, had: &serde_json::Value, state: &mut serde_json::Value) -> bool {
    let Some((len, mtime)) = fs::metadata(eboot(src)).ok().map(|m| (m.len(), m.modified().map(secs).unwrap_or(0))) else {
        return wants_playgo(src).unwrap_or(false);
    };
    let old = &had["playgo"];
    if old[0].as_u64() == Some(len) && old[1].as_u64() == Some(mtime) {
        if let Some(b) = old[2].as_bool() {
            state["playgo"] = old.clone();
            return b;
        }
    }
    let b = wants_playgo(src);
    if let Some(b) = b {
        state["playgo"] = serde_json::json!([len, mtime, b]);
    }
    b.unwrap_or(false)
}

pub fn check(src: &Source, s: &Settings) -> Fresh {
    let done = dir(&src.title_id).join(DONE);
    if fs::metadata(&done).is_err() {
        return Fresh::Missing;
    }
    let old: serde_json::Value =
        fs::read(&done).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default();
    let new = stamp(src, s);
    // An older layout rebuilds, moved or not (only_moved would refuse it too: "v" differs).
    if old.get("v") != new.get("v") {
        return Fresh::Stale(format!("cache layout v{} -> v{STAMP_V}", old.get("v").unwrap_or(&serde_json::Value::Null)));
    }
    if old.get("source") != new.get("source") {
        if let Some(from) = old["source"].as_str().map(PathBuf::from) {
            let exists = fs::symlink_metadata(&from).is_ok();
            if is_move(&old, &new, exists, || same_files(src, s, &old)) {
                return Fresh::Moved(from);
            }
        }
    }
    for (k, v) in new.as_object().into_iter().flatten() {
        if old.get(k) != Some(v) {
            return Fresh::Stale(format!("{k} changed"));
        }
    }
    // A cache built before kraken.image existed (or with it deleted): ng could not find the pkg.
    // The same for a .p5z's maxpack.image.
    let image = match src.kind {
        Kind::Pkg => Some(IMAGE_FILE),
        Kind::P5z => Some(P5Z_IMAGE_FILE),
        Kind::Folder(_) => None,
    };
    if let Some(image) = image.filter(|i| !dir(&src.title_id).join(i).is_file()) {
        return Fresh::Stale(format!("{image} missing"));
    }
    // Every input with the size and mtime `.done` recorded at the build's start, and no input added
    // or removed. Equality only: a source written on a PC whose clock is years ahead of the
    // console's is fresh (maxpack_logic::fresh).
    if let Some(why) = fresh::changed(&fresh::recorded(&old), &input_states(src, s)) {
        return Fresh::Stale(why);
    }
    // A `.done` from before the game_fakelib setting was removed: emulators/off filtered fakelib/.
    // refresh_fakelib recomposes it in full and rewrites `.done` without the key; "full" is fresh.
    if old.get("game_fakelib").is_some_and(|m| m != "full") {
        return Fresh::Fakelib("game_fakelib setting removed".into());
    }
    let had: serde_json::Value = fs::read(dir(&src.title_id).join(AMPR_STATE))
        .ok()
        .and_then(|b| serde_json::from_slice(&b).ok())
        .unwrap_or_default();
    // An older `.ampr` (ampr files only, no "emus") differs too: every cache refreshes once, which
    // also drops a libSceSaveData.native.sprx that an older build copied (keep_game_lib).
    // Only "ampr" and "emus" count: the "playgo" memo never triggers a refresh.
    let new = fakelib_state(src, s);
    if ["ampr", "emus"].iter().any(|&k| had.get(k) != new.get(k)) {
        return Fresh::Fakelib(format!("{AMPR_DIR}, inject_ampr, the bundled emulators or playgo_off changed"));
    }
    Fresh::Fresh
}

/// Whether fakelib file `name` goes into the cache's fakelib/: everything but libkernel.sprx.
fn keep_fakelib(name: &str) -> bool {
    !name.eq_ignore_ascii_case("libkernel.sprx")
}

/// keep_fakelib for the game's own lib dir, also without libSceSaveData.native.sprx: it breaks
/// saves (#68: CE-107098-5 until removed; #64: ShadowMount+ "full" fails, "emulators only" works).
/// ponytail: a backport's lib dir and a game's fakelib2/ keep it (keep_fakelib): both are deliberate
/// overrides, used as-is.
fn keep_game_lib(name: &str) -> bool {
    keep_fakelib(name) && !name.eq_ignore_ascii_case("libSceSaveData.native.sprx")
}

/// Copies the regular files directly in `from` that pass `keep` into `to`, 0777.
fn copy_flat(from: &Path, to: &Path, keep: impl Fn(&str) -> bool) -> Result<()> {
    let Ok(rd) = fs::read_dir(from) else { return Ok(()) };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if !fs::metadata(e.path()).is_ok_and(|m| m.is_file()) || !keep(&name) {
            continue;
        }
        let dest = to.join(&name);
        fs::copy(e.path(), &dest)?;
        exec_mode(&dest)?;
    }
    Ok(())
}

fn loose(path: String, size: u64) -> IndexFile {
    IndexFile { path, size, chunks: None }
}

/// The bundled emulators (EMUS) into `fakelib`: a file of that name the game or backport ships is
/// upgraded when it is a known older release (OLD_EMUS), else left alone; a missing one is added
/// when wanted (dlc_emu: `dlc`; pgo_stub: `playgo()`, called only then). The dlc_emu set is one
/// unit: if any of its files is not a known older release, none of the three is touched (no mixed
/// set). pgo_stub goes per file; with `pgo_off` it is skipped entirely (not added, not upgraded),
/// so a libScePlayGo.sprx the game or backport ships stays as shipped.
fn add_emus(tid: &str, fakelib: &Path, dlc: bool, pgo_off: bool, mut playgo: impl FnMut() -> bool) -> Result<()> {
    // Per entry: None = missing, Some((path, known old, the bundled file itself)).
    let found: Vec<_> = EMUS.iter().map(|&(name, data, ..)| find_ci(fakelib, name).map(|p| {
        let old = old_emu_file(&p, name);
        let current = !old && fs::read(&p).is_ok_and(|b| b == data);
        (p, old, current)
    })).collect();
    // A copy of the bundled release fits the set too: only other content keeps the game's set.
    let dlc_kept = EMUS.iter().zip(&found).any(|(e, f)| e.3 && f.as_ref().is_some_and(|(_, old, cur)| !old && !cur));
    if dlc_kept {
        log!("{tid}: fakelib: kept the game's dlc_emu files (not a known release)");
    }
    for (&(name, data, from, is_dlc), f) in EMUS.iter().zip(found) {
        if is_dlc && dlc_kept {
            continue;
        }
        if !is_dlc && pgo_off {
            log!("{tid}: fakelib: PlayGo stub off (playgo_off)");
            continue;
        }
        let version = if is_dlc { DLC_EMU_VERSION } else { PGO_STUB_VERSION }.trim();
        let (dest, what) = match f {
            // pgo_stub: playgo() only fills the import memo here (an old stub is upgraded either
            // way), so the UI offers the per-game switch for this game too.
            Some((have, true, _)) => {
                if !is_dlc {
                    playgo();
                }
                (have, "upgraded")
            }
            Some(_) => continue,
            // The eboot is read only when still needed.
            None if (if is_dlc { dlc } else { playgo() }) => (fakelib.join(name), "added"),
            None => continue,
        };
        fs::write(&dest, data)?;
        exec_mode(&dest)?;
        log!("{tid}: fakelib: {what} {name} ({from} {version})");
    }
    Ok(())
}

/// The source file that holds the game's own lib dir, for compose_fakelib.
#[derive(Clone, Copy)]
enum Inside<'a> {
    Pkg(&'a Io, &'a Pkg),
    P5z(&'a crate::p5z::Play),
}

/// fakelib/ from scratch: the backport's lib dir (all files) if any, else the game's own (pkg,
/// .p5z: extracted from `inside`; folder: copied), all of it but libSceSaveData.native.sprx; libkernel.sprx
/// never; then each bundled emulator (EMUS) the game needs and fakelib/ lacks (add_emulators: the
/// DLC set for a dlc_emu.ini at the game or backport root, pgo_stub for a PlayGo import in
/// eboot.bin, memoized in AMPR_STATE, none for a title in playgo_off), never over a file of that name unless it is a known older
/// release (add_emus: OLD_EMUS, then upgraded; the dlc_emu set as one unit); then the ampr files on
/// top (inject_ampr, always for a pkg or a .p5z), except over the game's own newer ampr-emu-ng (never
/// for a .p5z). All 0777.
/// Writes AMPR_STATE. Freshness needs no new state: the game's libs
/// are inputs (pkg, .p5z: stamp), AMPR_DIR is ampr_state, the bundled versions are emu_state.
fn compose_fakelib(src: &Source, s: &Settings, inside: Option<Inside>) -> Result<()> {
    let dir = dir(&src.title_id);
    let fakelib = dir.join("fakelib");
    match fs::remove_dir_all(&fakelib) {
        Err(e) if e.kind() != ErrorKind::NotFound => return Err(e),
        _ => {}
    }
    fs::create_dir_all(&fakelib)?;
    if let Some(bp) = bp_lib(src) {
        copy_flat(&bp, &fakelib, keep_fakelib)?;
    } else if let Some(Inside::Pkg(io, p)) = inside {
        // SMP order inside the game: fakelib2/ (used as-is) wins over fakelib/.
        let game_lib = if p.files().any(|e| e.path.to_ascii_lowercase().starts_with(LIB_DIRS[0])) {
            LIB_DIRS[0]
        } else {
            LIB_DIRS[1]
        };
        let mut buf = vec![0u8; CHUNK];
        for e in p.files().filter(|e| !e.is_dir) {
            let lower = e.path.to_ascii_lowercase();
            let Some(name) = lower.strip_prefix(game_lib) else { continue };
            let name = &e.path[e.path.len() - name.len()..];
            if !name.contains('/') && (if game_lib == LIB_DIRS[0] { keep_fakelib(name) } else { keep_game_lib(name) }) {
                extract(p, io, &e, &fakelib.join(name), &mut buf)?;
            }
        }
    } else if let Some(Inside::P5z(play)) = inside {
        // The same among a .p5z's loose files: those straight inside a lib dir (loose_dest GameLib,
        // so the name is safe), compared without case as in the pkg.
        let libs: Vec<(&str, &str, &str)> = play.info.loose.iter()
            .filter(|l| loose_dest(&l.path) == LooseDest::GameLib)
            .filter_map(|l| l.path.split_once('/').map(|(d, name)| (l.path.as_str(), d, name)))
            .collect();
        let (game_lib, keep): (_, fn(&str) -> bool) = if libs.iter().any(|(_, d, _)| d.eq_ignore_ascii_case("fakelib2")) {
            ("fakelib2", keep_fakelib)
        } else {
            ("fakelib", keep_game_lib)
        };
        for (rel, d, name) in libs {
            if d.eq_ignore_ascii_case(game_lib) && keep(name) {
                let dest = fakelib.join(name);
                play.extract(rel, &dest)?;
                exec_mode(&dest)?;
            }
        }
    } else if let Some(lib) = lib_dir(src) {
        let keep = if lib.ends_with("fakelib2") { keep_fakelib } else { keep_game_lib };
        copy_flat(&lib, &fakelib, keep)?;
    }
    // Our ampr-emu-ng on top of whatever the game or backport ships (its own libSceAmpr.sprx included).
    // State read before the copy: a file replaced mid-copy (ampr auto-update) then differs at the
    // next check and refreshes fakelib again, instead of being recorded as already copied.
    let mut state = fakelib_state(src, s);
    let had: serde_json::Value =
        fs::read(dir.join(AMPR_STATE)).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default();
    // An unused memo is kept: it is keyed by the eboot's size and mtime.
    if had.get("playgo").is_some() {
        state["playgo"] = had["playgo"].clone();
    }
    if s.add_emulators {
        let dlc = dlc_ini_on_disk(src)
            || match inside {
                Some(Inside::Pkg(_, p)) => p.files().any(|e| !e.is_dir && e.path == DLC_INI),
                Some(Inside::P5z(play)) => play.info.loose.iter().any(|l| l.path == DLC_INI),
                None => false,
            };
        add_emus(&src.title_id, &fakelib, dlc, playgo_off(src, s), || wants_playgo_memo(src, &had, &mut state))?;
    }
    if injects_ampr(src, s) {
        // No ng downgrade: the game's own newer ampr-emu-ng stays. Never in a .p5z game: the game's
        // own ng has no p5z mode, ours always goes on top.
        let keep = src.kind != Kind::P5z && keep_game_ng(&src.title_id, &fakelib);
        copy_flat(Path::new(AMPR_DIR), &fakelib, |n| ampr_copied(n) && !(keep && n.eq_ignore_ascii_case(AMPR_LIB)))?;
    }
    fs::write(dir.join(AMPR_STATE), state.to_string())
}

/// Recomposes only fakelib/ (Fresh::Fakelib): no extraction besides a pkg's or .p5z's own fakelib, no
/// index, no re-registration. ponytail: a launch reads fakelib/ while this runs only if a game
/// starts mid-refresh (the scan loop is paused while one runs).
pub fn refresh_fakelib(src: &Source, s: &Settings) -> Result<()> {
    log!("{}: refreshing {}", src.title_id, dir(&src.title_id).join("fakelib").display());
    let pkg = if src.kind == Kind::Pkg { Some(open(&src.path)?) } else { None };
    let play = if src.kind == Kind::P5z { Some(crate::p5z::open(&src.path)?) } else { None };
    let inside = pkg.as_ref().map(|(io, p)| Inside::Pkg(io, p)).or(play.as_ref().map(Inside::P5z));
    compose_fakelib(src, s, inside)
        .and_then(|()| {
            // check() matched the stamp and inputs: the current stamp with `.done`'s own mtime and
            // inputs record (drops a legacy game_fakelib key, keeps the input checks).
            let d = dir(&src.title_id);
            let done = d.join(DONE);
            let mtime = fs::metadata(&done)?.modified()?;
            let old: serde_json::Value = serde_json::from_slice(&fs::read(&done)?).unwrap_or_default();
            write_done(&d, src, s, mtime, &fresh::recorded(&old))
        })
        .map_err(|e| {
            let e = Error::new(e.kind(), format!("{} ({}): fakelib refresh: {e}", src.title_id, src.path.display()));
            log!("{e}");
            e
        })
}

/// A .p5z build stopped at its space check: its loose files and SPACE_MARGIN do not fit on /data.
/// clear_build has run (the last build's files are gone, the game's own stay), nothing was
/// extracted. The caller (main.rs) does not memoize it: it is tried again every pass.
#[derive(Debug)]
pub struct NoSpace {
    /// Bytes missing, the margin included.
    pub missing: u64,
    msg: String,
}

impl std::fmt::Display for NoSpace {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        f.write_str(&self.msg)
    }
}

impl std::error::Error for NoSpace {}

/// The bytes missing when `e` is a build's NoSpace.
pub fn missing_bytes(e: &Error) -> Option<u64> {
    e.get_ref()?.downcast_ref::<NoSpace>().map(|n| n.missing)
}

/// Always rebuilds `src`'s cache. Errors carry the title id and source path and are logged, but a
/// NoSpace (kept as such for missing_bytes), which main.rs logs once. A failed build leaves no
/// `.done` and a `.built` of what it could not remove (normally empty): the next build's
/// clear_build removes only that, never the game's files.
pub fn build(src: &Source, s: &Settings) -> Result<()> {
    let d = dir(&src.title_id);
    // A .p5z logs it once its loose files fit (build_p5z): one short of space retries every pass, quietly.
    if src.kind != Kind::P5z {
        log!("{}: building cache {} from {}", src.title_id, d.display(), src.path.display());
    }
    let t0 = std::time::Instant::now();
    let mut before = HashSet::new();
    let mut cleared = false;
    let r = (|| {
        // Keep the dir: /system_ex/app/<TID> is a nullfs of this exact directory, and deleting it
        // would leave that mount on a dead inode (launch fails with 0x80940033).
        fs::create_dir_all(&d)?;
        clear_build(&d)?;
        // What is left is the game's own files: never part of the build, never removed by it.
        before = walk(&d).into_iter().map(|e| e.0).collect();
        cleared = true;
        build_inner(src, s, &before)
    })();
    if r.is_ok() {
        log!("{}: cache built in {:.1} s", src.title_id, t0.elapsed().as_secs_f64());
    }
    r.map_err(|e| {
        let what = format!("{} ({}): cache build", src.title_id, src.path.display());
        let e = match e.get_ref().and_then(|i| i.downcast_ref::<NoSpace>()) {
            Some(n) => Error::new(e.kind(), NoSpace { missing: n.missing, msg: format!("{what}: {}", n.msg) }),
            None => {
                let e = Error::new(e.kind(), format!("{what}: {e}"));
                log!("{e}");
                e
            }
        };
        // Without `.done` the cache is unusable: free what this build wrote.
        remove_created(&d, &before);
        // What is left is the game's files: say so in `.built`, or the next clear_build (a NoSpace
        // retries next pass) finds none and empties the dir. Without `.done` it stays untrusted.
        if cleared {
            if let Err(w) = write_built(&d, &before) {
                log!("{}: {}: {w}", src.title_id, d.join(BUILT).display());
            }
        }
        e
    })
}

/// Writes `.built`: what the build created that is still under `dir` (not in `before`), one path
/// per line relative to `dir`.
fn write_built(dir: &Path, before: &HashSet<PathBuf>) -> Result<()> {
    let mut list = String::new();
    for (p, _) in created(dir, before) {
        if let Ok(rel) = p.strip_prefix(dir) {
            list += &rel.to_string_lossy();
            list.push('\n');
        }
    }
    fs::write(dir.join(BUILT), list)
}

/// Writes `.built` (what this build created) and then `.done`.
fn finish(dir: &Path, src: &Source, s: &Settings, start: SystemTime, inputs: &[Input], before: &HashSet<PathBuf>) -> Result<()> {
    write_built(dir, before)?;
    write_done(dir, src, s, start, inputs)
}

/// Writes `.done` with mtime `start` (the build's start: `built_at`) and `inputs` (`fresh::KEY`), the
/// inputs as the build found them (`record_inputs`), which `check` compares for equality.
/// The stamp also gets the source's identity, `pkg_id`, `p5z_id` or `folder_id` (for `is_move`;
/// check() compares neither).
fn write_done(dir: &Path, src: &Source, s: &Settings, start: SystemTime, inputs: &[Input]) -> Result<()> {
    let done = dir.join(DONE);
    let mut st = stamp(src, s);
    st[fresh::KEY] = fresh::to_json(inputs);
    let (key, id) = match src.kind {
        Kind::Pkg => (PKG_ID, pkg_id(&src.path)?),
        Kind::P5z => (P5Z_ID, crate::p5z::id(&src.path)?),
        Kind::Folder(_) => (FOLDER_ID, folder_id(src, s)),
    };
    st[key] = id.into();
    fs::write(&done, st.to_string())?;
    File::options().write(true).open(&done)?.set_modified(start)
}

/// Fresh::Moved: points the cache at `src`, the same game moved from `from`, without a rebuild.
/// pkg: new stamp source, kraken.image rewritten when the file name changed, `.done` mtime now
/// (the moved .pkg may be newer than the old `.done`; its pkg_id matched); extracted files and the
/// indexes stay (ng checks the pkg's size only, not its mtime). .p5z: the same with maxpack.image,
/// for which the .p5z is opened again (its volumes; its p5z_id matched). Folder: new stamp, `.done`
/// mtime now; main.rs remounts /system_ex/app/<TID>. Errors carry the title id and both paths.
pub fn relink(src: &Source, s: &Settings, from: &Path) -> Result<()> {
    let d = dir(&src.title_id);
    log!("{}: moved {} -> {}; relinking {}", src.title_id, from.display(), src.path.display(), d.display());
    let r = (|| {
        let image = match src.kind {
            Kind::Pkg => Some((IMAGE_FILE, format!("{PKG_MOUNT_DIR}/{}", utf8_name(&src.path)?))),
            Kind::P5z => {
                let play = crate::p5z::open(&src.path)?;
                let text = image_text(utf8_name(&src.path)?, &play.info.volumes).map_err(|e| bad(format!("{P5Z_IMAGE_FILE}: {e}")))?;
                Some((P5Z_IMAGE_FILE, text))
            }
            Kind::Folder(_) => None,
        };
        if let Some((file, image)) = image {
            if fs::read_to_string(d.join(file)).ok().as_deref() != Some(image.as_str()) {
                fs::write(d.join(file), image)?;
            }
        }
        // `.done` records the inputs as they are now (the moved source has its new path, maybe a new
        // mtime). A legacy game_fakelib `.done` never gets here: it has no pkg_id/folder_id, so it is
        // never a move.
        write_done(&d, src, s, SystemTime::now(), &record_inputs(src, s))
    })();
    r.map_err(|e| {
        let e = Error::new(e.kind(), format!("{} ({} -> {}): relink: {e}", src.title_id, from.display(), src.path.display()));
        log!("{e}");
        e
    })
}

/// `path`'s file name, which kraken.image and maxpack.image name: UTF-8 only.
fn utf8_name(path: &Path) -> Result<&str> {
    path.file_name().and_then(|n| n.to_str()).ok_or_else(|| bad(format!("{}: file name is not UTF-8", path.display())))
}

/// Copies the backport's regular files whose path (relative to it) `keep` takes into the cache
/// `dir` at the same path, 0777, over what is there. Returns each (path, size).
fn copy_backport(src: &Source, dir: &Path, keep: impl Fn(&str) -> bool) -> Result<Vec<(String, u64)>> {
    let Some(bp) = &src.backport else { return Ok(Vec::new()) };
    let mut out = Vec::new();
    for (path, m) in walk(bp).into_iter().filter(|(_, m)| m.is_file()) {
        let rel = path.strip_prefix(bp).unwrap_or(&path);
        let rel = rel.to_str().ok_or_else(|| bad(format!("{}: path is not UTF-8", path.display())))?;
        if !keep(rel) {
            continue;
        }
        let dest = dir.join(rel);
        if let Some(parent) = dest.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::copy(&path, &dest)?;
        exec_mode(&dest)?;
        out.push((rel.to_string(), m.len()));
    }
    Ok(out)
}

/// build_inner of a .p5z: only its loose files go to /data, and only when they fit with
/// SPACE_MARGIN to spare (else NoSpace, before anything is extracted); the volumes stay in the .p5z,
/// where ng reads them through maxpack.image. No index of ours: the .p5z ships ampr_zstd.index and
/// ampr_emu.index among its loose files.
fn build_p5z(src: &Source, s: &Settings, dir: &Path, start: SystemTime, inputs: &[Input], before: &HashSet<PathBuf>) -> Result<()> {
    let play = crate::p5z::open(&src.path)?;
    // First: a file name the contract cannot carry (not UTF-8, or a control byte) is a build failure.
    let image = image_text(utf8_name(&src.path)?, &play.info.volumes).map_err(|e| bad(format!("{P5Z_IMAGE_FILE}: {e}")))?;
    let loose: Vec<_> = play.info.loose.iter().map(|l| (l, loose_dest(&l.path))).collect();
    let need = loose.iter().filter(|(_, d)| *d != LooseDest::Skip).fold(0u64, |a, (l, _)| a.saturating_add(l.size));
    // After clear_build (build): what this cache held before is free again.
    let free = crate::storage::free(&dir.to_string_lossy())
        .map_err(|e| Error::new(e.kind(), format!("statfs {}: {e}", dir.display())))?;
    if let Some(missing) = space_short(free, need) {
        let margin = maxpack_logic::p5z::SPACE_MARGIN >> 20;
        let msg = format!("its loose files ({need} bytes) and a {margin} MiB margin do not fit in the {free} bytes free on /data: {missing} bytes missing");
        return Err(Error::new(ErrorKind::StorageFull, NoSpace { missing, msg }));
    }
    log!("{}: building cache {} from {}", src.title_id, dir.display(), src.path.display());
    let skipped: Vec<&str> = loose.iter().filter(|(_, d)| *d == LooseDest::Skip).map(|(l, _)| l.path.as_str()).collect();
    if let Some(first) = skipped.first() {
        log!("{}: {}: {} loose files not written (unsafe path, a nested lib dir or a name MaxPack keeps), first {first:?}",
            src.title_id, src.path.display(), skipped.len());
    }
    // Each path is loose_dest-checked: relative, no `..`, never MaxPack's own names; under `dir` only.
    for (l, _) in loose.iter().filter(|(_, d)| *d == LooseDest::App0) {
        let dest = dir.join(&l.path);
        play.extract(&l.path, &dest)?;
        exec_mode(&dest)?;
    }
    // The scan took the title id from the .p5z's description; the cache's param.json is what later
    // scans read (scan::cached_param) and what registers: both must say the same.
    let (tid, _, _) = crate::scan::parse_param(&fs::read(dir.join("sce_sys/param.json"))?)?;
    if tid != src.title_id {
        return Err(bad(format!("sce_sys/param.json says {tid}, the .p5z's description {}", src.title_id)));
    }
    compose_fakelib(src, s, Some(Inside::P5z(&play)))?;
    // Backport files go loose over the .p5z's: not its lib dirs, never MaxPack's own names.
    copy_backport(src, dir, |rel| loose_dest(rel) == LooseDest::App0)?;
    // launch.rs mounts the .p5z's folder on /app0/p5z; maxpack.image names the .p5z in it for ng.
    fs::create_dir_all(dir.join(P5Z_MOUNT_DIR))?;
    fs::write(dir.join(P5Z_IMAGE_FILE), image)?;
    finish(dir, src, s, start, inputs, before)
}

fn build_inner(src: &Source, s: &Settings, before: &HashSet<PathBuf>) -> Result<()> {
    let start = SystemTime::now();
    let inputs = record_inputs(src, s);
    let is_pkg = src.kind == Kind::Pkg;
    let dir = dir(&src.title_id);
    if src.kind == Kind::P5z {
        return build_p5z(src, s, &dir, start, &inputs, before);
    }
    let mut files: BTreeMap<String, IndexFile> = BTreeMap::new();
    let pkg = if is_pkg { Some(open(&src.path)?) } else { None };
    if let Some((io, p)) = &pkg {
        let mut buf = vec![0u8; CHUNK];
        // (files, bytes) extracted because the index cannot map them, sparse ones aside (logged each).
        let mut unpackable = (0u64, 0u64);
        for e in p.files().filter(|e| !e.is_dir) {
            let lower = e.path.to_ascii_lowercase();
            // dump-forge always puts a fresh ampr_emu.index (loose files only) into a pkg: ours replaces it.
            if lower == "ampr_emu.index" {
                continue;
            }
            // ponytail: a pkg made from an ampr-packed folder is refused; serving packs from inside a pkg is not supported.
            if lower == "ampr_assets.index" || lower == "ampr_zstd.index" {
                return Err(bad(format!(
                    "the pkg contains {} (ampr packs); make the pkg from the unpacked game folder",
                    e.path
                )));
            }
            // The lib dirs are compose_fakelib's; never game files.
            if LIB_DIRS.iter().any(|d| lower.starts_with(d)) {
                continue;
            }
            // A file stays in the pkg when the index can describe its blocks (a sparse hole is a
            // zero chunk, ampr_index). ponytail: any other file is extracted whole to the internal
            // SSD (its size, holes included): sparse holes off its 256 KiB block grid, or a short
            // compressed block running past its end (packers such as ps5-dump-forge put several
            // small files in one block; issue #52: Astro Bot, 4668 of 156246 files, 19 MB).
            let packed = if extracted(e.path) {
                None
            } else {
                // blocks() refuses (Unsupported only) a file whose data starts inside a block
                // shared with the file before it: extracted too, read_at decodes it.
                let c = if p.blocks(&e).is_ok() { Some(chunks(p, &e)?) } else { None };
                if let Some(c) = c.filter(|c| ampr_index::packable(e.size, c)) {
                    Some(c)
                } else if p.has_sparse(&e) {
                    log!("{}: {}: sparse holes off the 256 KiB grid, extracting {} bytes", src.title_id, e.path, e.size);
                    None
                } else {
                    unpackable.0 += 1;
                    unpackable.1 += e.size;
                    None
                }
            };
            if packed.is_none() {
                extract(p, io, &e, &dir.join(e.path), &mut buf)?;
            }
            files.insert(lower, IndexFile { path: e.path.to_string(), size: e.size, chunks: packed });
        }
        if unpackable.0 > 0 {
            log!(
                "{}: {} files in compressed blocks the index cannot map (shared or short), extracted {} bytes from {}",
                src.title_id,
                unpackable.0,
                unpackable.1,
                src.path.display()
            );
        }
    }
    compose_fakelib(src, s, pkg.as_ref().map(|(io, p)| Inside::Pkg(io, p)))?;
    if !is_pkg {
        return finish(&dir, src, s, start, &inputs, before);
    }
    // Backport files (except its fakelib/fakelib2) go loose over the pkg's.
    let not_lib = |rel: &str| !LIB_DIRS.iter().any(|d| rel.to_ascii_lowercase().starts_with(d));
    for (rel, size) in copy_backport(src, &dir, not_lib)? {
        files.insert(rel.to_ascii_lowercase(), loose(rel, size));
    }
    // fakelib/ is not indexed: it reaches the game as the common/lib overlay, and with a writable
    // /app0 an index miss goes to the real file (ng), so a refresh needs no index rewrite.
    // launch.rs mounts the pkg's folder on /app0/kraken; kraken.image names the pkg in it for ng.
    fs::create_dir_all(dir.join(PKG_MOUNT_DIR))?;
    fs::write(dir.join(IMAGE_FILE), format!("{PKG_MOUNT_DIR}/{}", utf8_name(&src.path)?))?;
    let files: Vec<IndexFile> = files.into_values().collect();
    ampr_index::write(&dir, &src.path, &files)?;
    finish(&dir, src, s, start, &inputs, before)
}
