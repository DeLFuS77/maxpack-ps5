//! MaxPack (maxpack-ps5): finds FPKG .pkg files, .p5z files and game folders on the internal SSD and USB/extended
//! drives, keeps a per-game cache in /data/maxpack/caches/<TID>, registers each game on the
//! home screen and overlays its fakelib at launch (DESIGN.md). Web UI on port web::PORT.

// ponytail: ampr_index keeps two unused format constants (FILE_STREAMING, CHUNK_STREAMING).
#![allow(dead_code)]

#[macro_use]
mod sys;
mod i18n;
mod ampr_index;
mod cache;
mod config;
mod crash;
mod installer;
mod launch;
mod net;
mod p5z;
mod recommended;
mod register;
mod scan;
mod storage;
mod tls;
mod update;
mod web;

use cache::Fresh;
use config::Settings;
use maxpack_logic::mounter::{display_name, walk_look, Debounced, MounterState, OTHER_MOUNTERS};
use scan::{Kind, Source};
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;
use web::State;

pub const DATA_DIR: &str = "/data/maxpack";

/// A system toast when `notify=1`; always logged.
fn toast(s: &Settings, msg: &str) {
    if s.notify {
        sys::notify(msg);
    } else {
        log!("{msg}");
    }
}

/// What a failed game's retry waits for: another stamp, or any build input (pkg/folder files,
/// backport, ampr dir) appearing, vanishing or changing mtime, or the game's PlayGo, overlay or
/// fakelib mode switch (the effective ones: a new recommended list changes them too).
fn inputs_key(src: &Source, s: &Settings) -> String {
    let mut k = cache::stamp(src, s).to_string();
    k += &format!("|playgo_off={}", cache::playgo_off(src, s));
    k += &format!("|overlay_off={}", cache::overlay_off(src, s));
    k += &format!("|fakelib_mode={}", cache::fakelib_mode(src, s).name());
    for p in cache::inputs(src, s) {
        let mtime = std::fs::metadata(&p).and_then(|m| m.modified()).ok();
        k += &format!("|{}={mtime:?}", p.display());
    }
    k
}

/// Low /data space: the UI banner, and a toast once per run. Checked before and after builds.
fn check_space(state: &State, s: &Settings) {
    static WARNED: AtomicBool = AtomicBool::new(false);
    let free = storage::drives().into_iter().find(|d| d.mount == "/data").map_or(u64::MAX, |d| d.free);
    let low = free < s.low_space_gb << 30;
    state.low_space.store(low, Ordering::SeqCst);
    if low && !WARNED.swap(true, Ordering::SeqCst) {
        toast(s, &i18n::low_space(i18n::lang(), free >> 30));
    }
}

/// A folder game's backport is a unionfs over its app0 (register.rs); a pkg's or .p5z's is copied
/// into its cache.
fn backport_of(src: &Source) -> Option<&std::path::Path> {
    match src.kind {
        Kind::Folder(_) => src.backport.as_deref(),
        Kind::Pkg | Kind::P5z => None,
    }
}

/// Titles whose last build is tried again every pass, the tries not logged: one short of space
/// (cache::missing_bytes), or one whose .p5z the engine could not open just now (cache::try_again).
/// The space warning, a log line and a toast, goes once per title and run (SPACE_WARNED); a failed
/// open is logged when its message changes (OPEN_LOGGED: title id, message), with no toast.
static RETRYING: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());
static SPACE_WARNED: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());
static OPEN_LOGGED: std::sync::Mutex<Vec<(String, String)>> = std::sync::Mutex::new(Vec::new());

/// What build_and_register did with a title.
#[derive(PartialEq)]
enum Reg {
    /// Registered by an earlier run: only mounted.
    Kept,
    /// Registered now.
    New,
    /// Staged and mounted, registered by the pass's `install_staged` (FW 12+).
    Staged,
    /// Built, but another mounter started meanwhile: neither registered nor mounted; a later pass
    /// takes the title again (its cache is fresh by then).
    Deferred,
}

/// Builds when stale or forced, then registers.
fn build_and_register(state: &State, s: &Settings, src: &Source, fresh: &Fresh, forced: bool) -> std::io::Result<Reg> {
    let tid = &src.title_id;
    let retrying = RETRYING.lock().unwrap_or_else(|e| e.into_inner()).contains(tid);
    if let (false, Fresh::Fakelib(why)) = (forced, fresh) {
        // Only fakelib/ is behind: recompose it, then go on as a fresh cache (mount or register).
        if !retrying {
            log!("{tid}: {why}; refreshing fakelib only");
        }
        cache::refresh_fakelib(src, s)?;
    }
    let stale = forced || !matches!(fresh, Fresh::Fresh | Fresh::Fakelib(_));
    let app0 = cache::app0(src);
    if !stale && register::is_registered(tid) {
        // ponytail: registered by an earlier run; only the nullfs is redone (gone after a
        // reboot; a no-op when mounted): no sce_sys copy, no install call.
        cache::fix_access(src);
        register::mount(tid, &app0, backport_of(src))?;
        // Backfills titles registered before snd0info was written, and resets a rewritten path;
        // off the scan thread (and the work lock): a busy app.db waits up to 3 s.
        register::spawn_snd0info(tid, 0);
        return Ok(Reg::Kept);
    }
    if stale {
        if !retrying {
            match fresh {
                Fresh::Stale(why) => log!("{tid}: cache stale ({why}); rebuilding"),
                Fresh::Missing => log!("{tid}: no cache; building"),
                Fresh::Fresh | Fresh::Fakelib(_) | Fresh::Moved(_) => log!("{tid}: rebuild forced"),
            }
        }
        state.update(&src.path, |g| g.status = "building");
        check_space(state, s);
        let r = cache::build(src, s);
        check_space(state, s);
        r?;
        // A build can take minutes: another mounter started during it (main::watch_mounter) gets
        // no register or mount from us now.
        if !state.mounts() {
            log!("{tid}: built; not registered while another mounter runs");
            return Ok(Reg::Deferred);
        }
    }
    // Once per run per title: build_and_register runs only for titles not registered in this run.
    cache::fix_access(src);
    let t0 = std::time::Instant::now();
    let new = register::register(tid, &app0, &cache::sce_sys(src), backport_of(src))?;
    log!("{tid}: {} in {} ms", if new { "registered" } else { "staged" }, t0.elapsed().as_millis());
    Ok(if new { Reg::New } else { Reg::Staged })
}

/// Fresh::Moved: a folder game's /system_ex/app/<TID> stack (nullfs + backport unionfs) is redone
/// on the new folder first, so a failed relink is retried next scan; then the cache is relinked
/// (a pkg's or .p5z's app0 is its cache dir: no remount; launch.rs mounts the file's folder from the stamp).
/// Only reached from the scan loop, which never runs while a game does.
fn relink_moved(s: &Settings, src: &Source, from: &std::path::Path) -> std::io::Result<()> {
    if matches!(src.kind, Kind::Folder(_)) {
        register::remount(&src.title_id, &cache::app0(src), backport_of(src))?;
    }
    cache::relink(src, s, from)?;
    // The moved copy may have other modes, and a registered title skips build_and_register.
    cache::fix_access(src);
    toast(s, &i18n::moved(i18n::lang(), &src.title_name, &src.path.to_string_lossy()));
    Ok(())
}

/// One game of the scan: check, build, register, status. A failed game is retried only when
/// its inputs_key changes or a rebuild is forced.
/// `staged` collects the titles waiting for the pass's `install_staged`.
fn process(
    state: &State, s: &Settings, src: &Source, registered: &mut HashSet<String>, failed: &mut HashMap<PathBuf, String>,
    staged: &mut Vec<Source>,
) {
    let tid = &src.title_id;
    // Never over a title id installed on the console (a retail game, or an FPKG installed through
    // the system): no register, no mount on its /system_ex/app/<TID>, no overlay at its launch.
    // Looked at every pass: the title goes on as usual once that install is gone.
    static INSTALLED_LOGGED: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());
    let installed = register::installed_copy(tid);
    {
        let mut logged = INSTALLED_LOGGED.lock().unwrap_or_else(|e| e.into_inner());
        let first = !logged.contains(tid);
        match &installed {
            Some(p) if first => {
                log!("{tid} ({}): an installed copy of this title exists ({}): MaxPack does not register or mount this copy",
                    src.path.display(), p.display());
                logged.push(tid.clone());
            }
            None if !first => logged.retain(|t| t != tid),
            _ => {}
        }
    }
    if let Some(p) = installed {
        registered.remove(tid);
        state.titles.lock().unwrap().remove(tid);
        let e = format!("{tid} ({}): an installed copy of this title exists ({}): MaxPack does not register or mount this copy",
            src.path.display(), p.display());
        state.update(&src.path, |g| {
            (g.status, g.error, g.installed_copy) = ("error", Some(e), Some(p.to_string_lossy().into_owned()));
            (g.error_code, g.missing_bytes) = (None, None);
        });
        return;
    }
    let forced = state.forced.lock().unwrap().remove(tid);
    let mut fresh = cache::check(src, s);
    if let (false, Fresh::Moved(from)) = (forced, &fresh) {
        fresh = match relink_moved(s, src, from) {
            Ok(()) => cache::check(src, s),
            // The rebuild path below logs and reports what still fails.
            Err(e) => Fresh::Stale(format!("relink failed: {e}")),
        };
    }
    let r = if !forced && failed.get(&src.path) == Some(&inputs_key(src, s)) {
        state.update(&src.path, |g| g.status = "error");
        return;
    } else if !forced && matches!(fresh, Fresh::Fresh) && registered.contains(tid) {
        // Each scan: redo /system_ex/app/<TID> when it is gone or stale (USB replug, rest
        // mode); a no-op when app0 is mounted there. Status stays "ready" (no UI flicker).
        match register::mount(tid, &cache::app0(src), backport_of(src)) {
            Ok(()) => {
                state.update(&src.path, |g| g.status = "ready");
                return;
            }
            Err(e) => Err(e),
        }
    } else {
        state.update(&src.path, |g| g.status = "checking");
        build_and_register(state, s, src, &fresh, forced)
    };
    match r {
        Ok(Reg::Staged) => {
            state.update(&src.path, |g| g.status = "checking");
            staged.push(src.clone());
        }
        // Not in `registered` nor `failed`: the next pass that mounts takes it again.
        Ok(Reg::Deferred) => state.update(&src.path, |g| g.status = "pending"),
        r => finish(state, s, src, registered, failed, r.map(|reg| reg == Reg::New), true),
    }
}

/// FW 12+: one sceAppInstUtilAppInstallAll for every title staged this pass, then each gets
/// what `process` gives a title registered on its own (status, toast, or the error).
fn install_staged(
    state: &State, s: &Settings, staged: &mut Vec<Source>, registered: &mut HashSet<String>,
    failed: &mut HashMap<PathBuf, String>,
) {
    if staged.is_empty() && !register::tile_staged() {
        return;
    }
    let tids: Vec<String> = staged.iter().map(|g| g.title_id.clone()).collect();
    let r = register::install_all(&tids);
    let one = tids.len() == 1;
    for src in staged.drain(..) {
        let r = r.as_ref().map(|_| true).map_err(|e| std::io::Error::new(e.kind(), e.to_string()));
        finish(state, s, &src, registered, failed, r, one);
    }
    // One toast for a batch: the console shows queued toasts one by one and the PS button opens
    // the one on screen, so a toast per game (32 at once on a rebuild of all) blocked leaving a game.
    if tids.len() > 1 {
        let l = i18n::lang();
        toast(s, &if r.is_ok() { i18n::registered_many(l, tids.len()) } else { i18n::failed_many(l, tids.len()) });
    }
}

/// Status, toast (`notify`) and failure record of a processed title. Ok(true): registered now.
/// A build short of space (cache::NoSpace) or whose .p5z the engine could not open just now
/// (cache::TryAgain) is not recorded in `failed`: it is tried again every pass.
fn finish(
    state: &State, s: &Settings, src: &Source, registered: &mut HashSet<String>, failed: &mut HashMap<PathBuf, String>,
    r: std::io::Result<bool>, notify: bool,
) {
    let tid = &src.title_id;
    let (size, built) = (cache::size(tid), cache::built_at(tid));
    let short = r.as_ref().err().and_then(cache::missing_bytes);
    let again = r.as_ref().err().is_some_and(cache::try_again);
    {
        let mut v = RETRYING.lock().unwrap_or_else(|e| e.into_inner());
        v.retain(|t| t != tid);
        v.extend((short.is_some() || again).then(|| tid.clone()));
    }
    // Taken out: put back below only while the open keeps failing.
    let open_logged = {
        let mut v = OPEN_LOGGED.lock().unwrap_or_else(|e| e.into_inner());
        v.iter().position(|(t, _)| t == tid).map(|i| v.remove(i).1)
    };
    match r {
        Ok(new) => {
            failed.remove(&src.path);
            registered.insert(tid.clone());
            state.titles.lock().unwrap().insert(tid.clone());
            state.update(&src.path, |g| {
                (g.status, g.error, g.installed_copy, g.cache_size, g.cache_built) = ("ready", None, None, size, built);
                (g.error_code, g.missing_bytes) = (None, None);
            });
            if new && notify {
                toast(s, &i18n::registered(i18n::lang(), &src.title_name));
            }
        }
        Err(e) => {
            let e = format!("{tid} ({}): {e}", src.path.display());
            if let Some(missing) = short {
                failed.remove(&src.path);
                state.update(&src.path, |g| {
                    (g.status, g.error, g.installed_copy, g.cache_size, g.cache_built) = ("error", Some(e.clone()), None, size, built);
                    (g.error_code, g.missing_bytes) = (Some("no_space"), Some(missing));
                });
                let mut warned = SPACE_WARNED.lock().unwrap_or_else(|e| e.into_inner());
                if !warned.contains(tid) {
                    warned.push(tid.clone());
                    log!("{e}; tried again every pass");
                    if notify {
                        toast(s, &i18n::no_space(i18n::lang(), &src.title_name, missing));
                    }
                }
                return;
            }
            if again {
                // As the scan's: not memoized, no toast; logged when the message changes.
                failed.remove(&src.path);
                state.update(&src.path, |g| {
                    (g.status, g.error, g.installed_copy, g.cache_size, g.cache_built) = ("error", Some(e.clone()), None, size, built);
                    (g.error_code, g.missing_bytes) = (None, None);
                });
                if open_logged.as_deref() != Some(e.as_str()) {
                    log!("{e}");
                }
                OPEN_LOGGED.lock().unwrap_or_else(|e| e.into_inner()).push((tid.clone(), e));
                return;
            }
            log!("{e}");
            failed.insert(src.path.clone(), inputs_key(src, s));
            state.update(&src.path, |g| {
                (g.status, g.error, g.installed_copy, g.cache_size, g.cache_built) = ("error", Some(e.clone()), None, size, built);
                (g.error_code, g.missing_bytes) = (None, None);
            });
            if notify {
                toast(s, &i18n::failed(i18n::lang(), &src.title_name));
            }
        }
    }
}

/// Replaces the UI's game list with the scan: an entry keeps its status while its source
/// stays listed, new ones are "pending", duplicates "duplicate". A source still being copied
/// keeps its old entry (it is not gone), a new one shows up once it is stable.
fn publish(state: &State, r: &scan::ScanResult) {
    let mut games = state.games.lock().unwrap();
    let old = std::mem::take(&mut *games);
    for src in &r.games {
        let prev = old.iter().find(|g| g.src.path == src.path && g.src.title_id == src.title_id && g.status != "duplicate");
        games.push(match prev {
            Some(g) => web::Game { src: src.clone(), ..g.clone() },
            None => web::Game {
                src: src.clone(), status: "pending", error: None, installed_copy: None, cache_size: 0, cache_built: None,
                error_code: None, missing_bytes: None,
            },
        });
    }
    for src in &r.duplicates {
        games.push(web::Game {
            src: src.clone(), status: "duplicate", error: None, installed_copy: None, cache_size: 0, cache_built: None,
            error_code: None, missing_bytes: None,
        });
    }
    for (path, _) in &r.unstable {
        if let Some(g) = old.iter().find(|g| &g.src.path == path && g.status != "duplicate") {
            if !games.iter().any(|n| n.src.title_id == g.src.title_id && n.status != "duplicate") {
                games.push(g.clone());
            }
        }
    }
}
/// A toast for each .p5z that cannot be played, once per path and run (the page lists them all).
fn warn_unplayable(s: &Settings, r: &scan::ScanResult) {
    static WARNED: std::sync::Mutex<Vec<PathBuf>> = std::sync::Mutex::new(Vec::new());
    let mut warned = WARNED.lock().unwrap_or_else(|e| e.into_inner());
    for (path, _) in &r.unplayable {
        if !warned.contains(path) {
            warned.push(path.clone());
            let name = path.file_name().map_or_else(|| path.to_string_lossy(), |n| n.to_string_lossy());
            toast(s, &i18n::p5z_unplayable(i18n::lang(), &name));
        }
    }
}

/// Debug: copies the kernel log (/dev/klog: module load failures, process crashes) to
/// /data/maxpack/klog.txt, read through the web UI's /file route.
/// ponytail: restarts the file at 4 MiB; only one reader of /dev/klog can exist.
fn klog_capture() {
    use std::io::{Read, Write};
    const OUT: &str = "/data/maxpack/klog.txt";
    let mut klog = match std::fs::File::open("/dev/klog") {
        Ok(f) => f,
        Err(e) => {
            log!("klog: cannot open /dev/klog: {e}");
            return;
        }
    };
    log!("klog: capturing to {OUT}");
    let mut buf = vec![0u8; 64 * 1024];
    let mut out = std::fs::File::create(OUT).ok();
    let mut written = 0usize;
    loop {
        let n = match klog.read(&mut buf) {
            Ok(0) => {
                std::thread::sleep(std::time::Duration::from_millis(100));
                continue;
            }
            Ok(n) => n,
            Err(e) => {
                log!("klog: read failed: {e}");
                return;
            }
        };
        if written > 4 << 20 {
            out = std::fs::File::create(OUT).ok();
            written = 0;
        }
        if let Some(f) = out.as_mut() {
            let _ = f.write_all(&buf[..n]);
        }
        written += n;
    }
}

/// The ELF the tile's page asks elfldr to start (ui.html `ELF`), as Dump Forge saves itself.
pub const SELF_COPY: &str = "/data/maxpack/maxpack.elf";
/// Stage 1 of ps5/build.sh, embedded only in the pushed (stage 2) ELF; the saved copy has none.
#[cfg(feature = "self-copy")]
const SELF_ELF: &[u8] = include_bytes!(env!("MAXPACK_SELF_ELF"));
#[cfg(not(feature = "self-copy"))]
const SELF_ELF: &[u8] = &[];

/// Saves SELF_ELF to SELF_COPY unless it is there already: temp file, fsync, rename.
/// ponytail: the last pushed ELF wins, no version order (Dump Forge keeps the newer one).
fn save_self_copy() {
    if SELF_ELF.is_empty() || std::fs::read(SELF_COPY).is_ok_and(|b| b == SELF_ELF) {
        return;
    }
    let tmp = format!("{SELF_COPY}.tmp");
    let r = std::fs::File::create(&tmp)
        .and_then(|mut f| std::io::Write::write_all(&mut f, SELF_ELF).and_then(|()| f.sync_all()))
        .and_then(|()| std::fs::rename(&tmp, SELF_COPY));
    match r {
        Ok(()) => log!("saved {SELF_COPY} ({} bytes): the tile starts it", SELF_ELF.len()),
        Err(e) => log!("save {SELF_COPY}: {e}"),
    }
}

/// The ampr-emu-ng release bundled in the ELF (vendor/ampr-emu-ng; ps5/build.sh checks its SHA-256).
const BUNDLED_AMPR: &[u8] = include_bytes!("../vendor/ampr-emu-ng/libSceAmpr.sprx");
const BUNDLED_AMPR_VERSION: &str = include_str!("../vendor/ampr-emu-ng/VERSION");
/// `<version> <len> <crc32>` of the copy we last wrote (bundled or downloaded); v0.4.0 wrote
/// `<len> <crc32>` (version unknown). Outside AMPR_DIR: every file there goes into fakelib/.
const BUNDLED_MARK: &str = "/data/maxpack/ampr.bundled";
const AMPR_ASSET: &str = ampr_logic::SPRX_ASSET;

fn mark_format(version: &str, len: usize, crc: u32) -> String {
    format!("{} {len} {crc:08x}", version.trim())
}

/// The mark's version when it records exactly (len, crc): Some(None) for the old format.
fn mark_version(mark: &str, len: usize, crc: u32) -> Option<Option<&str>> {
    let f: Vec<&str> = mark.split_whitespace().collect();
    let (v, l, c) = match f[..] {
        [l, c] => (None, l, c),
        [v, l, c] => (Some(v), l, c),
        _ => return None,
    };
    (l.parse::<usize>().ok()? == len && u32::from_str_radix(c, 16).ok()? == crc).then_some(v)
}

pub(crate) use maxpack_logic::version::{newer, parse_version};
use maxpack_logic::ampr::{self as ampr_logic, Fail as AmprFail};

#[derive(Debug, PartialEq)]
enum Installed<'a> {
    Missing,
    /// Put there by hand: never replaced.
    Hand,
    /// A copy we wrote, with its version (None = unknown).
    Ours(Option<&'a str>),
}

/// `cur`: (len, crc32, equals the bundled copy) of AMPR_DIR/libSceAmpr.sprx, None when missing.
fn installed<'a>(cur: Option<(usize, u32, bool)>, bundled_version: &'a str, mark: Option<&'a str>) -> Installed<'a> {
    match cur {
        None => Installed::Missing,
        Some((_, _, true)) => Installed::Ours(Some(bundled_version)),
        Some((len, crc, false)) => match mark.and_then(|m| mark_version(m, len, crc)) {
            Some(v) => Installed::Ours(v),
            None => Installed::Hand,
        },
    }
}

/// The updater downloads `tag` when the file is missing or ours and older.
fn should_update(inst: &Installed, tag: &str) -> bool {
    match inst {
        Installed::Missing => true,
        Installed::Hand => false,
        Installed::Ours(v) => newer(Some(tag), *v),
    }
}

/// Writes `b` to AMPR_DIR/libSceAmpr.sprx (0777) and records it in BUNDLED_MARK as `version`.
/// Temp file beside AMPR_DIR, not in it, so a scan never copies a half-written one.
fn write_ampr(path: &str, b: &[u8], version: &str) -> std::io::Result<()> {
    let tmp = format!("{DATA_DIR}/libSceAmpr.sprx.tmp");
    let r = std::fs::File::create(&tmp)
        .and_then(|mut f| std::io::Write::write_all(&mut f, b).and_then(|()| f.sync_all()))
        .and_then(|()| std::fs::set_permissions(&tmp, std::os::unix::fs::PermissionsExt::from_mode(0o777)))
        .and_then(|()| std::fs::rename(&tmp, path));
    if r.is_err() {
        let _ = std::fs::remove_file(&tmp);
    }
    r?;
    // ponytail: no mark = the file reads as hand-placed and is never replaced again (by the bundled
    // copy or the updater) until deleted; an error here is reported, not undone.
    write_mark(version, b)
}

fn write_mark(version: &str, b: &[u8]) -> std::io::Result<()> {
    std::fs::write(BUNDLED_MARK, mark_format(version, b.len(), ampr_index::crc32(&[b])))
        .map_err(|e| std::io::Error::new(e.kind(), format!("write {BUNDLED_MARK}: {e}")))
}

/// What AMPR_DIR/libSceAmpr.sprx (`path`) is now given BUNDLED_MARK's text, and whether it equals
/// the bundled copy. Err: a read error other than NotFound.
fn read_installed<'a>(path: &str, mark: Option<&'a str>) -> std::io::Result<(Installed<'a>, bool)> {
    let cur = match std::fs::read(path) {
        Ok(cur) => Some((cur.len(), ampr_index::crc32(&[&cur]), cur == BUNDLED_AMPR)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
        Err(e) => return Err(e),
    };
    let bundled = cur.is_some_and(|c| c.2);
    Ok((installed(cur, BUNDLED_AMPR_VERSION.trim(), mark), bundled))
}

/// Puts the bundled libSceAmpr.sprx into AMPR_DIR when it is missing or is a copy we wrote
/// (bundled or downloaded) that is not newer. A file put there by hand (a debug build, say) is kept.
fn install_bundled_ampr() {
    let path = format!("{}/{AMPR_ASSET}", cache::AMPR_DIR);
    let bv = BUNDLED_AMPR_VERSION.trim();
    let mark = std::fs::read_to_string(BUNDLED_MARK).ok();
    match read_installed(&path, mark.as_deref()) {
        Ok((_, true)) => {
            let want = mark_format(bv, BUNDLED_AMPR.len(), ampr_index::crc32(&[BUNDLED_AMPR]));
            if mark.as_deref() != Some(want.as_str()) {
                if let Err(e) = write_mark(bv, BUNDLED_AMPR) {
                    log!("ampr: {e}");
                }
            }
            return;
        }
        Ok((Installed::Ours(v), _)) if newer(v, Some(bv)) => {
            log!("ampr: {path} is ampr-emu-ng {} (newer than the bundled {bv}): kept", v.unwrap_or("?"));
            return;
        }
        Ok((Installed::Ours(_) | Installed::Missing, _)) => {}
        Ok((Installed::Hand, _)) => {
            log!("ampr: {path} is not a bundled ampr-emu-ng: kept (delete it to get {bv})");
            return;
        }
        Err(e) => {
            log!("ampr: read {path}: {e}");
            return;
        }
    }
    match write_ampr(&path, BUNDLED_AMPR, bv) {
        Ok(()) => log!("ampr: installed bundled ampr-emu-ng {bv} ({} bytes) to {path}", BUNDLED_AMPR.len()),
        Err(e) => log!("ampr: install bundled ampr-emu-ng to {path}: {e}"),
    }
}

/// net::https_get with up to `attempts` tries 20 s apart; each failure logged.
fn get_retry(url: &str, max: usize, attempts: u32) -> Result<Vec<u8>, String> {
    let mut last = String::new();
    for n in 1..=attempts {
        match net::https_get(url, max) {
            Ok(b) => return Ok(b),
            Err(e) => {
                log!("ampr: update: GET {url} (attempt {n}/{attempts}): {e}");
                last = e.to_string();
            }
        }
        if n < attempts {
            std::thread::sleep(Duration::from_secs(20));
        }
    }
    Err(last)
}

/// A hand-placed libSceAmpr.sprx moved aside by a forced update (outside AMPR_DIR: never copied).
/// ponytail: one backup; a second forced update over another hand-placed file replaces it.
const HAND_BACKUP: &str = "/data/maxpack/libSceAmpr.sprx.hand";
/// One update at a time: the start-up check and the web UI's "Update now" share it.
static AMPR_UPDATING: AtomicBool = AtomicBool::new(false);

fn hand_placed(path: &str) -> Result<bool, String> {
    let mark = std::fs::read_to_string(BUNDLED_MARK).ok();
    let (inst, _) = read_installed(path, mark.as_deref()).map_err(|e| format!("read {path}: {e}"))?;
    Ok(inst == Installed::Hand)
}

/// Installs the latest stable ampr-emu-ng release from GitHub (maxpack_logic::ampr): every game
/// loads it, so, as the self-update's maxpack.elf, only once the secure-connection self-test has
/// passed on this console (refused with tls_unchecked before anything is downloaded) and only when
/// its SHA-256 is the one the release's SHA256SUMS lists for libSceAmpr.sprx. Without `force`
/// (start-up check): only when the installed file is ours (or missing) and older; a hand-placed file
/// is kept. With `force` (web UI): whatever is installed, a hand-placed file moved to HAND_BACKUP
/// first. Returns a one-line result for the UI, or why nothing was installed (both logged); an
/// install wakes the scan loop to refresh fakelibs.
fn ampr_update(state: &State, force: bool, attempts: u32) -> Result<String, AmprFail> {
    let r = ampr_logic::gate(tls::passed()).and_then(|()| {
        if AMPR_UPDATING.swap(true, Ordering::SeqCst) {
            return Err(AmprFail::UpdateRunning);
        }
        let r = ampr_update_inner(state, force, attempts);
        AMPR_UPDATING.store(false, Ordering::SeqCst);
        r
    });
    match &r {
        Ok(m) => log!("ampr: update: {m}"),
        Err(f) => log!("ampr: update: nothing installed ({}): {}", f.code(), f.detail()),
    }
    r
}

fn ampr_update_inner(state: &State, force: bool, attempts: u32) -> Result<String, AmprFail> {
    let path = format!("{}/{AMPR_ASSET}", cache::AMPR_DIR);
    let url = ampr_logic::LATEST_URL;
    let body = get_retry(url, ampr_logic::LATEST_MAX, attempts).map_err(|e| AmprFail::Download(format!("GET {url}: {e}")))?;
    let rel = ampr_logic::release(&body).map_err(|e| AmprFail::Release(format!("{url}: {e}")))?;
    let mark = std::fs::read_to_string(BUNDLED_MARK).ok();
    let (inst, _) = read_installed(&path, mark.as_deref()).map_err(|e| AmprFail::Install(format!("read {path}: {e}")))?;
    if !force && !should_update(&inst, &rel.tag) {
        let why = match inst {
            Installed::Hand => "hand-placed, kept".to_string(),
            Installed::Ours(v) => format!("installed {} is not older", v.unwrap_or("?")),
            Installed::Missing => String::new(),
        };
        return Ok(format!("latest {} skipped ({path}: {why})", rel.tag));
    }
    // Not the release's file, or nothing to check it with: refused, logged (ampr_update) and toasted.
    let refuse = |why: String| {
        let s = state.settings.lock().unwrap().clone();
        toast(&s, &i18n::ampr_refused(i18n::lang(), &rel.tag));
        AmprFail::Checksum(format!("{}: {why}", rel.tag))
    };
    let sums_asset = rel.sums.clone().map_err(&refuse)?;
    let sums = get_retry(&sums_asset.url, ampr_logic::SUMS_MAX, attempts)
        .map_err(|e| AmprFail::Download(format!("GET {}: {e}", sums_asset.url)))?;
    let b = get_retry(&rel.sprx.url, ampr_logic::SPRX_MAX, attempts)
        .map_err(|e| AmprFail::Download(format!("GET {}: {e}", rel.sprx.url)))?;
    ampr_logic::verify(&b, rel.sprx.size, &sums).map_err(|e| refuse(format!("{}: {e}", rel.sprx.url)))?;
    // Checked again: a file put there by hand during the download is kept (forced: moved aside).
    if hand_placed(&path).map_err(AmprFail::Install)? {
        if !force {
            return Ok(format!("latest {} skipped ({path}: hand-placed meanwhile, kept)", rel.tag));
        }
        std::fs::rename(&path, HAND_BACKUP).map_err(|e| AmprFail::Install(format!("move {path} -> {HAND_BACKUP}: {e}")))?;
        log!("ampr: update: hand-placed {path} moved to {HAND_BACKUP}");
    }
    write_ampr(&path, &b, &rel.tag).map_err(|e| AmprFail::Install(format!("{} to {path}: {e}", rel.tag)))?;
    let s = state.settings.lock().unwrap().clone();
    toast(&s, &i18n::ampr_updated(i18n::lang(), &rel.tag));
    state.wake();
    Ok(format!("installed ampr-emu-ng {} ({} bytes, matches {})", rel.tag, b.len(), maxpack_logic::update::SUMS_ASSET))
}

/// GET /api/ampr: what AMPR_DIR/libSceAmpr.sprx is now, and whether an update runs.
fn ampr_status() -> serde_json::Value {
    let path = format!("{}/{AMPR_ASSET}", cache::AMPR_DIR);
    let mark = std::fs::read_to_string(BUNDLED_MARK).ok();
    let (kind, version) = match read_installed(&path, mark.as_deref()) {
        Ok((_, true)) => ("bundled", Some(BUNDLED_AMPR_VERSION.trim())),
        Ok((Installed::Ours(Some(v)), _)) => ("downloaded", Some(v)),
        // v0.4.0 builds' mark: an older bundled copy.
        Ok((Installed::Ours(None), _)) => ("bundled", None),
        Ok((Installed::Hand, _)) => ("hand", None),
        Ok((Installed::Missing, _)) => ("missing", None),
        Err(_) => ("error", None),
    };
    serde_json::json!({"kind": kind, "version": version, "updating": AMPR_UPDATING.load(Ordering::SeqCst)})
}

/// ampr files are loaded by every game's sandbox: 0777 like the caches. A libkernel.sprx there
/// replaces the system one for every game, which is rarely wanted.
fn prepare_ampr() {
    let Ok(rd) = std::fs::read_dir(cache::AMPR_DIR) else { return };
    for e in rd.flatten() {
        let path = e.path();
        if !path.is_file() {
            continue;
        }
        if let Err(e) = std::fs::set_permissions(&path, std::os::unix::fs::PermissionsExt::from_mode(0o777)) {
            log!("ampr: chmod 0777 {}: {e}", path.display());
        }
        if e.file_name().eq_ignore_ascii_case("libkernel.sprx") {
            log!("ampr: WARNING {} overrides the system libkernel in every game; remove it unless intended", path.display());
        }
    }
}

/// A scan pass that takes at least this long is logged with its timing.
const SLOW_PASS_MS: u128 = 1000;

/// How often the mounter state is looked at (watch_mounter, and the scan loop while it is paused).
const MOUNTER_POLL: Duration = Duration::from_secs(2);

/// Set whenever another mounter is seen; the scan loop takes it once it mounts again: the other
/// mounter may have remounted, registered or removed our titles, so every title is checked again.
static OTHER_SEEN: AtomicBool = AtomicBool::new(false);

/// The mounter state now: kstuff, then one look at the process list for ShadowMountPlus and the
/// original of MaxPack (they register and mount the same titles), through
/// maxpack_logic::mounter::Debounced, so an unreadable list or one missed look never reads as
/// "the other mounter is gone". Stored in `state.mounter` for /api/state and the launch watcher.
/// On a change: logged, one toast (waiting for kstuff, another mounter, or mounting again after
/// one) and, with `wake`, the scan loop woken to act on it. Callers are serialized (the debounce
/// lock), so a later look is never stored before an earlier one.
fn refresh_mounter(state: &State, wake: bool) -> MounterState {
    /// The debounce, and whether the last walk of the process list failed (logged once per streak).
    static DEBOUNCED: std::sync::Mutex<(Debounced, bool)> = std::sync::Mutex::new((Debounced::new(), false));
    let mut guard = DEBOUNCED.lock().unwrap_or_else(|e| e.into_inner());
    let (debounced, list_failed) = &mut *guard;
    let (seen, walk_error) = launch::running_names(&OTHER_MOUNTERS);
    match &walk_error {
        Some(e) if !*list_failed => {
            log!("process list walk failed ({e}); a mounter seen before that counts, else the mounter state stays as it was")
        }
        None if *list_failed => log!("process list readable again"),
        _ => {}
    }
    *list_failed = walk_error.is_some();
    // A mounter seen before a bad record is Found; only an empty walk that failed is an Error.
    let this_look = walk_look(&|n: &str| seen.iter().any(|s| *s == n), walk_error.is_some());
    let m = debounced.update(sys::kstuff_loaded(), this_look, sys::uptime_ms());
    let last = std::mem::replace(&mut *state.mounter.lock().unwrap(), m);
    if matches!(m, MounterState::OtherMounter(_)) {
        OTHER_SEEN.store(true, Ordering::SeqCst);
    }
    if m != last {
        let s = state.settings.lock().unwrap().clone();
        let l = i18n::lang();
        match (m, last) {
            (MounterState::Ready, MounterState::OtherMounter(p)) => {
                log!("{p} is gone; mounting again");
                toast(&s, &i18n::mounting_resumed(l, display_name(p)));
            }
            (MounterState::Ready, _) => log!("kstuff: ready; mounting"),
            (MounterState::WaitKstuff, _) => {
                log!("waiting for kstuff; nothing mounts until it answers");
                toast(&s, i18n::kstuff_waiting(l));
            }
            (MounterState::OtherMounter(p), _) => {
                log!("{p} is running: no scan, build, register or new overlay until it stops (the web UI stays up)");
                // ShadowMount+ keeps its 11-language text; the others get the new one.
                if p == "shadowmountplus.elf" {
                    toast(&s, i18n::smp_running(l));
                } else {
                    toast(&s, &i18n::other_mounter(l, display_name(p)));
                }
            }
        }
        if wake {
            state.wake();
        }
    }
    m
}

/// SMP wait_for_kstuff_ready: polls every 2 s until kstuff answers, one toast when it starts
/// waiting. Another mounter is not waited for here (the web UI and the installer stay up): from
/// now on watch_mounter looks every 2 s and the scan loop only pauses mounting.
fn wait_ready(state: &Arc<State>) {
    while refresh_mounter(state, false) == MounterState::WaitKstuff {
        std::thread::sleep(MOUNTER_POLL);
    }
    let state = state.clone();
    std::thread::spawn(move || watch_mounter(&state));
}

/// The mounter state every 2 s, also while the scan thread is busy with a long build: a mounter
/// started meanwhile stops new overlays (launch::watch) within 2 s, and the scan loop is woken.
fn watch_mounter(state: &State) {
    loop {
        std::thread::sleep(MOUNTER_POLL);
        refresh_mounter(state, true);
    }
}

/// The home-screen tile that opens the UI, once (after wait_ready: the first looks at the other
/// mounters are in). Never while another mounter runs ("installer only", Ruling Q7): on FW 12+ the
/// tile goes in with sceAppInstUtilAppInstallAll, which installs whatever is staged in /user/app,
/// the other mounter's titles too. Registered once that mounter has stopped; meanwhile the page
/// opens in a browser at http://<console IP>:8095 (the UI banner says so).
fn register_tile_when_mounting(state: &State) {
    let mut logged = false;
    while state.installer_only() {
        if !logged {
            log!("tile: not registered while another mounter runs (installer only); the page is at http://<console IP>:{}", web::PORT);
            logged = true;
        }
        std::thread::sleep(MOUNTER_POLL);
    }
    register::register_tile(web::PORT);
}

/// SMP auto_remove_missing_games timers (sm_game_cache.c), in memory only: a restart starts over.
#[derive(Default)]
struct MissingTimers {
    /// Title id -> second (since start) its source was first seen missing.
    since: HashMap<String, u64>,
    /// Removed from the home screen in this run while the source stays missing.
    removed: HashSet<String>,
    /// Skipped as a real install (register::is_ours), logged once per run.
    skipped: HashSet<String>,
}

impl MissingTimers {
    /// `missing`: this pass's unprotected titles with a gone source. Any other title's timer
    /// resets (source back, DLC, running). Returns the titles missing for `delay` s, not yet removed.
    fn due(&mut self, missing: &HashSet<String>, now: u64, delay: u64) -> Vec<String> {
        self.since.retain(|t, _| missing.contains(t));
        self.removed.retain(|t| missing.contains(t));
        let mut due = Vec::new();
        for t in missing {
            let since = *self.since.entry(t.clone()).or_insert(now);
            if !self.removed.contains(t) && now.saturating_sub(since) >= delay {
                due.push(t.clone());
            }
        }
        due.sort();
        due
    }
}

/// ENOENT/ENOTDIR only: any other stat error (a drive that answers badly) is not "gone".
pub(crate) fn gone(path: &std::path::Path) -> bool {
    matches!(std::fs::symlink_metadata(path),
        Err(e) if matches!(e.raw_os_error(), Some(libc::ENOENT | libc::ENOTDIR)))
}

/// Built caches (a `.done` stamp) whose stamp source is gone, as title id -> source. Not a title
/// this scan lists (moved, or still being copied), a running one, one an image of this scan names
/// (`[TID]`: ShadowMountPlus may serve it from there now), or one with DLC in /user/addcont/<TID>
/// (SMP keeps those; a stat error there protects too).
fn missing_sources(r: &scan::ScanResult, running: &HashSet<String>) -> HashMap<String, String> {
    let mut out = HashMap::new();
    for e in std::fs::read_dir(cache::CACHES).into_iter().flatten().flatten() {
        let tid = e.file_name().to_string_lossy().into_owned();
        let Ok(raw) = std::fs::read(e.path().join(cache::DONE)) else { continue };
        let Ok(stamp) = serde_json::from_slice::<serde_json::Value>(&raw) else { continue };
        let Some(source) = stamp["source"].as_str() else { continue };
        if running.contains(&tid)
            || r.games.iter().any(|g| g.title_id == tid)
            || r.unstable.iter().any(|(p, t)| t.as_ref() == Some(&tid) || p.as_os_str() == source)
            || r.images.iter().any(|i| {
                i.path.file_name().is_some_and(|n| maxpack_logic::images::image_has_title(&n.to_string_lossy(), &tid))
            })
            || !gone(source.as_ref())
            || !gone(format!("/user/addcont/{tid}").as_ref())
        {
            continue;
        }
        out.insert(tid, source.to_string());
    }
    out
}

/// One auto-remove step after a full scan pass: start/reset timers, then unregister the due
/// titles as /api/delete does (unmount without force, uninstall); the cache stays for a return.
fn auto_remove(state: &State, s: &Settings, r: &scan::ScanResult, timers: &mut MissingTimers, now: u64) {
    if !s.auto_remove_missing {
        *timers = MissingTimers { skipped: std::mem::take(&mut timers.skipped), ..Default::default() };
        return;
    }
    let running = state.running.lock().unwrap().clone();
    let missing = missing_sources(r, &running);
    let keys: HashSet<String> = missing.keys().cloned().collect();
    for tid in timers.due(&keys, now, s.auto_remove_delay_secs) {
        // A game may have started since `running` was read, or another mounter (which may have
        // mounted this title id): stop, the next full pass retries.
        if state.paused() || !state.mounts() {
            return;
        }
        let source = &missing[&tid];
        // Never uninstall a real installed game that shares the title id; rechecked every pass.
        if let Err(p) = register::is_ours(&tid) {
            if timers.skipped.insert(tid.clone()) {
                log!("{tid}: auto-remove skipped: an installed copy exists ({})", p.display());
            }
            continue;
        }
        if let Err(e) = register::unmount_title(&tid) {
            // Retried next pass: a busy mount means the title is in use.
            log!("{tid}: auto-remove: unmount ({source}): {e}");
            continue;
        }
        // ponytail: a failed uninstall is logged once and not retried until the source returns or a restart.
        timers.removed.insert(tid.clone());
        match register::uninstall(&tid) {
            Ok(()) => {
                register::forget(&tid);
                let n = now.saturating_sub(timers.since[&tid]);
                log!("{tid}: source {source} missing for {n} s, removed from the home screen");
                state.titles.lock().unwrap().remove(&tid);
            }
            Err(e) => log!("{tid}: auto-remove: uninstall ({source}): {e}"),
        }
    }
}

fn main() {
    // ponytail: umask 0 for the whole payload, so every dir/file it creates is 0777/0666 (as SMP,
    // which mkdirs 0777) whatever elfldr passes: the game and the system services (save data,
    // trophies) read them with another uid. Code that wants a narrower mode sets it explicitly.
    unsafe { libc::umask(0) };
    // panic=abort: record the panic in maxpack.log before the payload dies.
    std::panic::set_hook(Box::new(|info| sys::log_line(&format!("PANIC: {info}"))));
    for dir in [DATA_DIR, cache::CACHES, cache::AMPR_DIR] {
        if let Err(e) = std::fs::create_dir_all(dir) {
            sys::notify(&i18n::cannot_create(i18n::lang(), dir, &e.to_string()));
            return;
        }
    }
    let s = config::load();
    sys::LOG_FILE.store(s.log, Ordering::Relaxed);
    let listener = match web::take_port() {
        Ok(l) => l,
        Err(web::PortTaken::Building) => {
            toast(&s, i18n::finishing_build(i18n::lang()));
            return;
        }
        Err(web::PortTaken::Busy) => {
            toast(&s, &i18n::port_busy(i18n::lang(), web::PORT));
            return;
        }
    };
    toast(&s, i18n::started(i18n::lang()));
    // The recommended per-game settings in use: the bundled list or the newer downloaded copy.
    recommended::init();
    install_bundled_ampr();
    prepare_ampr();
    std::thread::spawn(save_self_copy);
    // The latest release of MaxPack, once per start; a 404 (none, or a private repository) offers nothing.
    update::spawn_check();
    // ponytail: debug turned on later takes effect at the next start.
    if s.debug {
        std::thread::spawn(klog_capture);
    }
    let state = Arc::new(State::new(s));
    // The phase-2 installer (/api/inst/*): up whatever the mounter state, as the web UI.
    installer::start(state.clone(), BUNDLED_AMPR, BUNDLED_AMPR_VERSION);
    let st = state.settings.lock().unwrap().clone();
    // Not gated on inject_ampr: pkg games get the ampr dir regardless.
    if st.ampr_auto_update {
        let state = state.clone();
        std::thread::spawn(move || ampr_update(&state, false, 3));
    }
    // The recommended per-game settings from GitHub, once per start (recommended_update, and only once
    // the secure-connection self-test has passed: tls::passed reads its record).
    recommended::spawn_refresh(state.clone());
    let web_state = state.clone();
    std::thread::spawn(move || web::serve(web_state, listener));
    // The web UI is up; nothing mounts until kstuff answers, nor while another mounter runs.
    wait_ready(&state);
    {
        let state = state.clone();
        std::thread::spawn(move || register_tile_when_mounting(&state));
    }
    let (titles, running, mounter) = (state.titles.clone(), state.running.clone(), state.mounter.clone());
    // Built caches are launchable before the first scan ends: watch them from the start (not a
    // title id installed on the console: its launch is the installed game's, never overlaid).
    for e in std::fs::read_dir(cache::CACHES).into_iter().flatten().flatten() {
        let tid = e.file_name().to_string_lossy().into_owned();
        if e.path().join(cache::DONE).is_file() && register::installed_copy(&tid).is_none() {
            titles.lock().unwrap().insert(tid);
        }
    }
    std::thread::spawn(move || launch::watch(titles, running, mounter));
    let (mut registered, mut failed) = (HashSet::new(), HashMap::new());
    // Auto-remove timers start only after the first full pass: a drive not mounted yet at start
    // looks like a missing source until then (the delay covers the rest).
    let (mut timers, mut full_pass, t0) = (MissingTimers::default(), false, std::time::Instant::now());
    loop {
        // Another mounter runs (or kstuff is gone): no scan, build, register, purge or auto-remove,
        // and launch::watch mounts no new overlay; overlays already up still go when their game
        // exits. Requests stay queued as while a game runs. The web UI (and the installer) stay up.
        if !refresh_mounter(&state, false).mounts() {
            state.busy.store(false, Ordering::SeqCst);
            std::thread::sleep(MOUNTER_POLL);
            continue;
        }
        // Back after another mounter: every title goes through build_and_register again (only a
        // remount when it is still on the home screen), and a failure gets one more try.
        if OTHER_SEEN.swap(false, Ordering::SeqCst) && !(registered.is_empty() && failed.is_empty()) {
            log!("another mounter ran: checking every title again");
            registered.clear();
            failed.clear();
        }
        // A game runs: nothing that could touch its app0 or /system_ex mount. Rescan, rebuild
        // and purge requests stay queued (forced / purge) for after it exits.
        // ponytail: polls every 2 s instead of a wake from the watcher.
        if state.paused() {
            state.busy.store(false, Ordering::SeqCst);
            std::thread::sleep(Duration::from_secs(2));
            continue;
        }
        // Held to the end of the pass (dropped at `continue` and before the wait): /api/delete waits on it.
        let work = state.work.lock().unwrap();
        // A self-update is under way: the new copy quits this one soon, so no build starts that it
        // would have to wait for (its quit waits for a pass that runs).
        if update::holding() {
            drop(work);
            state.busy.store(false, Ordering::SeqCst);
            std::thread::sleep(Duration::from_secs(1));
            continue;
        }
        let pass = std::time::Instant::now();
        // Loaded under the settings lock: web.rs loads, changes and saves the file under it too, so
        // this pass never puts an older copy over a newer one in state.settings.
        let s = {
            let mut cur = state.settings.lock().unwrap();
            *cur = config::load();
            cur.clone()
        };
        sys::LOG_FILE.store(s.log, Ordering::Relaxed);
        state.busy.store(true, Ordering::SeqCst);
        let roots = usable_roots(if s.scan_paths.is_empty() { storage::default_scan_roots() } else { s.scan_paths.clone() });
        let r = scan::scan(&roots, &s.exclude, s.backports, s.stability_wait_secs);
        let scan_ms = pass.elapsed().as_millis();
        if state.paused() || !state.mounts() {
            continue;
        }
        // Purge after the scan, so `keep` is this scan's library and every kept title rebuilds below.
        // purge() empties kept caches too, and a source still being copied is not rebuilt this pass:
        // the request waits until no source is in flight.
        static PURGE_WAIT_LOGGED: AtomicBool = AtomicBool::new(false);
        if !r.unstable.is_empty() && state.purge.load(Ordering::SeqCst) {
            if !PURGE_WAIT_LOGGED.swap(true, Ordering::SeqCst) {
                log!("purge waits: {} source(s) still being copied", r.unstable.len());
            }
        } else if state.mounts() && state.purge.swap(false, Ordering::SeqCst) {
            PURGE_WAIT_LOGGED.store(false, Ordering::SeqCst);
            let keep: Vec<String> = r.games.iter().map(|g| g.title_id.clone()).collect();
            // Looked at again before each title: purge unmounts /system_ex/app/<TID>, where another
            // mounter started meanwhile may have mounted that title id.
            match cache::purge(&keep, &|| state.mounts()) {
                Ok(true) => log!("caches purged; {} kept for rebuild", keep.len()),
                Ok(false) => {
                    log!("purge stopped: MaxPack does not mount now (another mounter, or no kstuff); it goes on once MaxPack mounts again");
                    state.purge.store(true, Ordering::SeqCst);
                }
                Err(e) => log!("purge {}: {e}", cache::CACHES),
            }
            state.forced.lock().unwrap().extend(keep);
        }
        publish(&state, &r);
        *state.images.lock().unwrap() = r.images.clone();
        *state.unplayable.lock().unwrap() = r.unplayable.clone();
        warn_unplayable(&s, &r);
        // A source that left the scan (USB unplugged) goes through register::mount again when back.
        // A source still being copied stays registered (skipped this pass, not gone).
        registered.retain(|t| r.games.iter().any(|g| &g.title_id == t)
            || r.unstable.iter().any(|(_, u)| u.as_ref() == Some(t)));
        // Built caches are overlaid at launch from the start, not only once the scan reaches them
        // (never a title id installed on the console: `process` leaves those alone).
        state.titles.lock().unwrap().extend(
            r.games.iter()
                .filter(|g| cache::built_at(&g.title_id).is_some() && register::installed_copy(&g.title_id).is_none())
                .map(|g| g.title_id.clone()),
        );
        check_space(&state, &s);
        let (mut full, mut staged) = (true, Vec::new());
        for src in &r.games {
            if state.paused() {
                log!("a game started; scan paused until it exits");
                full = false;
                break;
            }
            // A first adoption pass can build for minutes: another mounter started meanwhile
            // (watch_mounter) stops it before the next title, not at the end of the pass.
            if !state.mounts() {
                log!("pass stopped before {}: MaxPack does not mount now", src.title_id);
                full = false;
                break;
            }
            // Excluded since this pass loaded the settings (POST /api/settings): left alone now.
            if scan::excluded(&src.path, &state.settings.lock().unwrap().exclude) {
                continue;
            }
            process(&state, &s, src, &mut registered, &mut failed, &mut staged);
        }
        // Not once a game has started (nothing registers while a game runs) or another mounter
        // runs: the staged titles are not in `registered`, so the pass after that stages them again
        // and installs them.
        // ponytail: a title already on the home screen (a rebuild) is only remounted then; its
        // app.db row keeps the old version and name until its next full register.
        if !state.paused() && state.mounts() {
            install_staged(&state, &s, &mut staged, &mut registered, &mut failed);
        }
        if full_pass && full && !state.paused() && state.mounts() {
            auto_remove(&state, &s, &r, &mut timers, t0.elapsed().as_secs());
        }
        full_pass |= full;
        *state.cache_total.lock().unwrap() = cache::total_size();
        check_space(&state, &s);
        // A quick pass every scan_interval would flood the log: only a slow one is logged.
        let pass_ms = pass.elapsed().as_millis();
        if pass_ms >= SLOW_PASS_MS {
            log!("scan pass: {} games in {pass_ms} ms (scan {scan_ms} ms)", r.games.len());
        }
        state.busy.store(false, Ordering::SeqCst);
        drop(work);
        // watch_mounter wakes this wait when the mounter state changes.
        state.wait(Duration::from_secs(s.scan_interval_secs));
    }
}

/// `roots` without those MaxPack never scans (maxpack_logic::roots::never_root: `/`, the system,
/// Sony's user data and installed games, the sandboxes, MaxPack's and the other mounters' data
/// folders); each one left out is logged once. A hand-edited config.toml can still list them.
fn usable_roots(mut roots: Vec<String>) -> Vec<String> {
    use maxpack_logic::roots::{never_root, Forbidden};
    static LOGGED: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());
    roots.retain(|r| {
        let why = never_root(r);
        let mut logged = LOGGED.lock().unwrap_or_else(|e| e.into_inner());
        match why {
            Some(w) if !logged.contains(r) => {
                if w == Forbidden::OtherMounterData {
                    log!("scan root {r} is inside another mounter's data folder: not scanned (MaxPack never writes there)");
                } else {
                    log!("scan root {r} is {}: not scanned (MaxPack never mounts games from there)", w.why());
                }
                logged.push(r.clone());
            }
            _ => {}
        }
        why.is_none()
    });
    roots
}
