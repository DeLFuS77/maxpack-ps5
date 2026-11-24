//! Game discovery over the scan roots (DESIGN.md).
use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::{self, Error, ErrorKind};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};
use std::time::SystemTime;

use maxpack_logic::dups::{claim, RANK_FOLDER, RANK_P5Z, RANK_PKG};
use maxpack_logic::p5z::Unplayable;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Packing {
    Plain,
    Zstd,
    Lz4,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Pkg,
    Folder(Packing),
    /// A .p5z played without installing it (crate::p5z).
    P5z,
}

#[derive(Clone, Debug)]
pub struct Source {
    pub path: PathBuf,
    pub kind: Kind,
    pub title_id: String,
    pub title_name: String,
    pub version: String,
    /// Mount of the drive holding `path` ("/data", "/mnt/usb0").
    pub drive: String,
    /// .pkg or .p5z file size; 0 for folders.
    pub size: u64,
    pub backport: Option<PathBuf>,
}

pub struct ScanResult {
    pub games: Vec<Source>,
    pub duplicates: Vec<Source>,
    pub errors: Vec<(PathBuf, String)>,
    /// Sources still being copied (stability_wait_seconds): path and title id when known.
    /// Skipped this pass, but not gone: the caller keeps their entry, registration and cache.
    pub unstable: Vec<(PathBuf, Option<String>)>,
    /// ShadowMountPlus disk images where a source could be: listed, never mounted (design stage 3).
    pub images: Vec<Image>,
    /// .p5z files the engine refuses (cut short, damaged, too many volumes): not games.
    pub unplayable: Vec<(PathBuf, Unplayable)>,
}

/// A `.ffpkg`, `.exfat`, `.ffpfs` or `.ffpfsc` file (maxpack_logic::images::smp_image_kind).
#[derive(Clone, Debug)]
pub struct Image {
    pub path: PathBuf,
    pub kind: &'static str,
    /// From a `[PPSA01234]` in the file name; None without one.
    pub title_id: Option<String>,
}

/// Seconds since the youngest max(mtime, ctime) of `paths` when that is under `wait` (SMP
/// sm_stability.c); None = stable. A clock in the future (negative age) counts as stable, as in SMP.
/// A path that cannot be stat'ed is ignored here (the scan reports it).
pub fn still_written(paths: &[PathBuf], wait: u64) -> Option<u64> {
    use std::os::unix::fs::MetadataExt;
    if wait == 0 {
        return None;
    }
    let now = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).ok()?.as_secs() as i64;
    paths.iter().filter_map(|p| fs::metadata(p).ok())
        .map(|m| now - m.mtime().max(m.ctime()))
        .filter(|&age| (0..wait as i64).contains(&age))
        .min()
        .map(|age| age as u64)
}

/// Records an unstable source; logs it once until it is stable again.
fn skip_unstable(result: &mut ScanResult, path: PathBuf, tid: Option<String>, age: u64) {
    let logged = UNSTABLE.get_or_init(|| Mutex::new(HashSet::new()));
    if logged.lock().unwrap_or_else(|e| e.into_inner()).insert(path.clone()) {
        let label = tid.clone().unwrap_or_else(|| path.display().to_string());
        log!("{label}: source still being written (changed {age} s ago), waiting");
    }
    result.unstable.push((path, tid));
}

/// Unstable sources already logged.
static UNSTABLE: OnceLock<Mutex<HashSet<PathBuf>>> = OnceLock::new();

type PkgParam = Result<(String, String, String), String>;
type PkgMemo = HashMap<PathBuf, (u64, SystemTime, PkgParam)>;
static PKG_MEMO: OnceLock<Mutex<PkgMemo>> = OnceLock::new();

fn pkg_param(path: &Path, size: u64, mtime: SystemTime) -> PkgParam {
    let memo = PKG_MEMO.get_or_init(|| Mutex::new(HashMap::new()));
    let mut memo = memo.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((old_size, old_mtime, result)) = memo.get(path) {
        if *old_size == size && *old_mtime == mtime {
            return result.clone();
        }
    }
    let result = crate::cache::peek_pkg(path);
    // Only deterministic results are memoized; a transient I/O error (USB waking up) retries next scan.
    let deterministic = match &result {
        Ok(_) => true,
        Err(e) => matches!(e.kind(), ErrorKind::InvalidData | ErrorKind::UnexpectedEof),
    };
    let result = result.map_err(|e| e.to_string());
    if deterministic {
        memo.insert(path.to_path_buf(), (size, mtime, result.clone()));
    } else {
        memo.remove(path);
    }
    result
}

type P5zParam = Result<(String, String, String), Unplayable>;
type P5zMemo = HashMap<PathBuf, (u64, SystemTime, P5zParam)>;
static P5Z_MEMO: OnceLock<Mutex<P5zMemo>> = OnceLock::new();

/// (title id, name, version) of a .p5z from the engine's description; the name is the title id
/// when the .p5z has none. A refusal of the engine is memoized by (size, mtime), so a copy that
/// ends later (another size or mtime) is read again; a file that cannot be opened is not.
fn p5z_param(path: &Path, size: u64, mtime: SystemTime) -> Result<(String, String, String), crate::p5z::OpenError> {
    use crate::p5z::OpenError;
    let memo = P5Z_MEMO.get_or_init(|| Mutex::new(HashMap::new()));
    let mut memo = memo.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((old_size, old_mtime, result)) = memo.get(path) {
        if *old_size == size && *old_mtime == mtime {
            return result.clone().map_err(OpenError::Unplayable);
        }
    }
    let result = match crate::p5z::open(path) {
        Ok(play) => {
            let i = play.info;
            let name = if i.name.is_empty() { i.title_id.clone() } else { i.name };
            Ok((i.title_id, name, i.version))
        }
        Err(OpenError::Unplayable(u)) => Err(u),
        Err(e @ OpenError::Io(_)) => {
            memo.remove(path);
            return Err(e);
        }
    };
    memo.insert(path.to_path_buf(), (size, mtime, result.clone()));
    result.map_err(OpenError::Unplayable)
}

/// SMP order (resolve_backport_path_for_title): the game's own scan root, the other roots,
/// then /data/homebrew (SMP's installed backports) even when custom scanpath lines replace it.
fn backport_for(owner: &Path, roots: &[String], tid: &str) -> Option<PathBuf> {
    let mut order: Vec<&Path> = vec![owner];
    for root in roots.iter().map(Path::new).chain([Path::new("/data/homebrew")]) {
        if !order.contains(&root) {
            order.push(root);
        }
    }
    for root in order {
        let dir = root.join("backports");
        let exact = dir.join(tid);
        if exact.is_dir() {
            return Some(exact);
        }
        let Ok(entries) = fs::read_dir(&dir) else { continue };
        let mut matches: Vec<_> = entries.flatten().map(|e| e.path())
            .filter(|p| p.is_dir() && p.file_name().is_some_and(|n| n.to_string_lossy().starts_with(tid)))
            .collect();
        matches.sort();
        if let Some(path) = matches.into_iter().next() {
            return Some(path);
        }
    }
    None
}

/// Source path (.pkg, .p5z or folder) -> TID of each cache dir, from the "source" of its `.done` stamp.
fn cached_sources() -> HashMap<PathBuf, String> {
    let Ok(rd) = fs::read_dir(crate::cache::CACHES) else { return HashMap::new() };
    rd.flatten()
        .filter_map(|e| {
            let tid = e.file_name().to_string_lossy().into_owned();
            let stamp: serde_json::Value = serde_json::from_slice(&fs::read(e.path().join(crate::cache::DONE)).ok()?).ok()?;
            Some((PathBuf::from(stamp["source"].as_str()?), tid))
        })
        .collect()
}

/// (title id, name, version) from the cache's sce_sys/param.json when that cache was built from `pkg`
/// after the pkg's last change and its stamp has the pkg's current size (a same-name replacement
/// that kept an older mtime differs in size). ponytail: trusts the cache; the .pkg is not opened then.
fn cached_param(cached: &HashMap<PathBuf, String>, pkg: &Path, size: u64, mtime: SystemTime) -> Option<(String, String, String)> {
    let dir = crate::cache::dir(cached.get(pkg)?);
    let done = dir.join(crate::cache::DONE);
    if fs::metadata(&done).and_then(|m| m.modified()).ok()? <= mtime {
        return None;
    }
    let stamp: serde_json::Value = serde_json::from_slice(&fs::read(&done).ok()?).ok()?;
    if stamp["size"].as_u64() != Some(size) {
        return None;
    }
    parse_param(&fs::read(dir.join("sce_sys/param.json")).ok()?).ok()
}

fn scan_error(result: &mut ScanResult, path: PathBuf, error: impl std::fmt::Display) {
    let message = error.to_string();
    // Log only new or changed errors per path: a bad entry would otherwise log every scan.
    let logged = LOGGED.get_or_init(|| Mutex::new(HashMap::new()));
    let mut logged = logged.lock().unwrap_or_else(|e| e.into_inner());
    if logged.get(&path) != Some(&message) {
        log!("{}: {message}", path.display());
        logged.insert(path.clone(), message.clone());
    }
    result.errors.push((path, message));
}

/// Last logged scan error per path.
static LOGGED: OnceLock<Mutex<HashMap<PathBuf, String>>> = OnceLock::new();

/// Sorted direct entries of `dir` without dot entries (macOS ._X.pkg AppleDouble files, as in
/// SMP); None when `dir` is gone.
fn list(dir: &Path, result: &mut ScanResult) -> Option<Vec<PathBuf>> {
    let entries = match fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(e) if e.kind() == ErrorKind::NotFound => return None,
        Err(e) => {
            scan_error(result, dir.to_path_buf(), e);
            return None;
        }
    };
    let mut paths = Vec::new();
    for entry in entries {
        match entry {
            Ok(entry) if entry.file_name().to_string_lossy().starts_with('.') => {}
            Ok(entry) => paths.push(entry.path()),
            Err(e) => scan_error(result, dir.to_path_buf(), e),
        }
    }
    paths.sort();
    Some(paths)
}

/// `backports` in any case: an exFAT drive finds `Backports` under that name too (backport_for).
fn is_backports(path: &Path) -> bool {
    path.file_name().is_some_and(|n| n.eq_ignore_ascii_case("backports"))
}

/// Candidate sources of one root: (its own entries sorted, the nested ones). Nested = the
/// entries, sorted, of each subfolder that is not a game folder, not `backports` and not itself a
/// root (SMP scan_depth=2). A path already in `listed` (this pass) is left out.
/// ponytail: one level below a root only (SMP's default depth), and none below a drive root
/// (/mnt/usbN, /mnt/extN): its homebrew and etaHEN/games are roots of their own, and walking
/// every top folder of a drive (Movies/...) each pass costs too much on USB. A game two folders
/// down, or in another folder at a drive's root, needs its own scanpath line.
fn candidates(root: &Path, roots: &[String], listed: &mut HashSet<PathBuf>, result: &mut ScanResult) -> (Vec<PathBuf>, Vec<PathBuf>) {
    let (mut top, mut nested) = (Vec::new(), Vec::new());
    let drive_root = root.parent() == Some(Path::new("/mnt"));
    for path in list(root, result).unwrap_or_default() {
        if is_backports(&path) || !listed.insert(path.clone()) {
            continue;
        }
        let descend = !drive_root && path.is_dir()
            && !(path.join("eboot.bin").is_file() && path.join("sce_sys/param.json").is_file())
            && !roots.iter().any(|r| Path::new(r) == path);
        if descend {
            for child in list(&path, result).unwrap_or_default() {
                if !is_backports(&child) && listed.insert(child.clone()) {
                    nested.push(child);
                }
            }
        }
        top.push(path);
    }
    (top, nested)
}

/// Finds the sources under `roots`: the .pkg and .p5z files and game folders (eboot.bin and
/// sce_sys/param.json) directly in a root, and those one folder below it (see `candidates`).
pub fn scan(roots: &[String], backports: bool, stability_wait: u64) -> ScanResult {
    let mut result =
        ScanResult { games: Vec::new(), duplicates: Vec::new(), errors: Vec::new(), unstable: Vec::new(), images: Vec::new(),
            unplayable: Vec::new() };
    // Title id -> (rank of the winner, its index in `games`; None while the winner is still being copied).
    let mut seen: HashMap<String, (u8, Option<usize>)> = HashMap::new();
    // A slot is emptied when a later source beats it.
    let mut games: Vec<Option<Source>> = Vec::new();
    let cached = cached_sources();
    // Every root's own entries first, in root order, then the nested ones: a nested copy never
    // beats a source found directly in a root (claim).
    let (mut listed, mut order, mut later) = (HashSet::new(), Vec::new(), Vec::new());
    for root in roots {
        let root = Path::new(root);
        let (top, nested) = candidates(root, roots, &mut listed, &mut result);
        order.extend(top.into_iter().map(|p| (root, p, false)));
        later.extend(nested.into_iter().map(|p| (root, p, true)));
    }
    order.extend(later);
    for (root, path, nested) in order {
        // Never a source there: a game's app0 is written (fix_access, the game's own writes, delete).
        // maxpack_logic::roots::never_source: the system, Sony's user data and installed games, the
        // sandboxes, MaxPack's and the other mounters' data folders.
        if maxpack_logic::roots::never_source(&path.to_string_lossy()).is_some() {
            continue;
        }
        let metadata = match fs::metadata(&path) {
            Ok(metadata) => metadata,
            Err(e) => {
                scan_error(&mut result, path, e);
                continue;
            }
        };
        let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        if let Some(kind) = metadata.is_file().then(|| maxpack_logic::images::smp_image_kind(&name)).flatten() {
            let title_id = maxpack_logic::images::bracket_title_id(&name).map(str::to_string);
            result.images.push(Image { path, kind, title_id });
            continue;
        }
        let (kind, size, param) = if metadata.is_file()
            && path.extension().is_some_and(|e| e.eq_ignore_ascii_case("pkg")) {
            // Before the pkg is opened: a half-copied header is not parsed. The title id is
            // the one of an earlier cache of this path, if any.
            if let Some(age) = still_written(std::slice::from_ref(&path), stability_wait) {
                let tid = cached.get(&path).cloned();
                if let Some(t) = &tid {
                    claim(&mut seen, &mut games, &mut result.duplicates, t, RANK_PKG, nested, None);
                }
                skip_unstable(&mut result, path, tid, age);
                continue;
            }
            let mtime = match metadata.modified() {
                Ok(mtime) => mtime,
                Err(e) => {
                    scan_error(&mut result, path, e);
                    continue;
                }
            };
            let param = match cached_param(&cached, &path, metadata.len(), mtime) {
                Some(p) => Ok(p),
                None => pkg_param(&path, metadata.len(), mtime),
            };
            (Kind::Pkg, metadata.len(), param)
        } else if metadata.is_file() && maxpack_logic::p5z::is_p5z_name(&name) {
            // As a pkg: a half-copied .p5z is not opened, and an earlier cache of it holds its title id.
            if let Some(age) = still_written(std::slice::from_ref(&path), stability_wait) {
                let tid = cached.get(&path).cloned();
                if let Some(t) = &tid {
                    claim(&mut seen, &mut games, &mut result.duplicates, t, RANK_P5Z, nested, None);
                }
                skip_unstable(&mut result, path, tid, age);
                continue;
            }
            let mtime = match metadata.modified() {
                Ok(mtime) => mtime,
                Err(e) => {
                    scan_error(&mut result, path, e);
                    continue;
                }
            };
            let param = match cached_param(&cached, &path, metadata.len(), mtime) {
                Some(p) => Ok(p),
                None => match p5z_param(&path, metadata.len(), mtime) {
                    Ok(p) => Ok(p),
                    Err(crate::p5z::OpenError::Unplayable(u)) => {
                        scan_error(&mut result, path.clone(), format!("cannot be played ({}): {}", u.code(), crate::p5z::reason(&u)));
                        result.unplayable.push((path, u));
                        continue;
                    }
                    Err(crate::p5z::OpenError::Io(e)) => Err(e.to_string()),
                },
            };
            (Kind::P5z, metadata.len(), param)
        } else if metadata.is_dir() && !is_backports(&path)
            && path.join("eboot.bin").is_file() && path.join("sce_sys/param.json").is_file() {
            // ponytail: only the folder itself, sce_sys/, its param.json and eboot.bin are checked, so
            // a big file deep inside can still be in flight; a full walk per pass costs too much on USB.
            let probe = [path.clone(), path.join("sce_sys"), path.join("sce_sys/param.json"), path.join("eboot.bin")];
            if let Some(age) = still_written(&probe, stability_wait) {
                // A param.json mid-copy does not parse: the title id of an earlier cache of this path.
                let tid = fs::read(path.join("sce_sys/param.json")).ok()
                    .and_then(|json| parse_param(&json).ok()).map(|(tid, _, _)| tid)
                    .or_else(|| cached.get(&path).cloned());
                if let Some(t) = &tid {
                    claim(&mut seen, &mut games, &mut result.duplicates, t, RANK_FOLDER, nested, None);
                }
                skip_unstable(&mut result, path, tid, age);
                continue;
            }
            let packing = if path.join("ampr_zstd.index").is_file() {
                Packing::Zstd
            } else if path.join("ampr_assets.index").is_file() {
                Packing::Lz4
            } else {
                Packing::Plain
            };
            (Kind::Folder(packing), 0, fs::read(path.join("sce_sys/param.json"))
                .and_then(|json| parse_param(&json)).map_err(|e| e.to_string()))
        } else {
            continue;
        };
        let (title_id, title_name, version) = match param {
            Ok(param) => param,
            Err(e) => {
                scan_error(&mut result, path, e);
                continue;
            }
        };
        if !valid_tid(&title_id) {
            scan_error(&mut result, path, format!("invalid title id {title_id:?}"));
            continue;
        }
        let source = Source {
            drive: crate::storage::drive_of(&path),
            backport: if backports { backport_for(root, roots, &title_id) } else { None },
            path, kind, title_id, title_name, version, size,
        };
        let rank = match source.kind {
            Kind::Folder(_) => RANK_FOLDER,
            Kind::Pkg => RANK_PKG,
            Kind::P5z => RANK_P5Z,
        };
        let tid = source.title_id.clone();
        claim(&mut seen, &mut games, &mut result.duplicates, &tid, rank, nested, Some(source));
    }
    result.games = games.into_iter().flatten().collect();
    result.games.sort_by(|a, b| a.title_name.cmp(&b.title_name));
    // A fixed error that comes back is logged again.
    if let Some(logged) = LOGGED.get() {
        let errors: HashSet<&PathBuf> = result.errors.iter().map(|(p, _)| p).collect();
        logged.lock().unwrap_or_else(|e| e.into_inner()).retain(|p, _| errors.contains(p));
    }
    // A source copied again later is logged again.
    if let Some(logged) = UNSTABLE.get() {
        let unstable: HashSet<&PathBuf> = result.unstable.iter().map(|(p, _)| p).collect();
        logged.lock().unwrap_or_else(|e| e.into_inner()).retain(|p| unstable.contains(p));
    }
    result
}
pub fn parse_param(json: &[u8]) -> io::Result<(String, String, String)> {
    let v: serde_json::Value = serde_json::from_slice(json)
        .map_err(|e| Error::new(ErrorKind::InvalidData, format!("param.json: {e}")))?;
    let tid = v["titleId"].as_str().or_else(|| v["title_id"].as_str()).unwrap_or_default();
    if !valid_tid(tid) {
        return Err(Error::new(ErrorKind::InvalidData, format!("param.json: bad titleId {tid:?}")));
    }
    let lp = &v["localizedParameters"];
    // ponytail: the console language at parse time; an unbuilt pkg's memoized name keeps the
    // language of its first read until the pkg changes or the payload restarts.
    let name = local_title(lp, crate::i18n::title_tag())
        .or_else(|| lp["defaultLanguage"].as_str().and_then(|lang| lp[lang]["titleName"].as_str()))
        .filter(|name| !name.is_empty())
        .or_else(|| lp["en-US"]["titleName"].as_str().filter(|name| !name.is_empty()))
        .or_else(|| lp.as_object().and_then(|langs| langs.values()
            .filter_map(|lang| lang["titleName"].as_str()).find(|name| !name.is_empty())))
        .unwrap_or(tid);
    Ok((tid.to_owned(), name.to_owned(), v["contentVersion"].as_str().unwrap_or_default().to_owned()))
}
/// The `localizedParameters` title for language tag `want`: that tag (any case), else the same
/// language in another region (es-419 -> es-ES, en-GB -> en-US), as Dump Forge's gameTitle; blank
/// names do not count.
fn local_title<'a>(lp: &'a serde_json::Value, want: &str) -> Option<&'a str> {
    let langs = lp.as_object()?;
    let base = |t: &str| t.split('-').next().unwrap_or("").to_ascii_lowercase();
    let name = |v: &'a serde_json::Value| v["titleName"].as_str().filter(|n| !n.trim().is_empty());
    let titles = || langs.iter().filter(|(k, _)| *k != "defaultLanguage");
    titles().find(|(k, _)| k.eq_ignore_ascii_case(want)).and_then(|(_, v)| name(v))
        .or_else(|| titles().filter(|(k, _)| base(k) == base(want)).find_map(|(_, v)| name(v)))
}

/// Game title ids only: NPXS ids are system apps (SMP is_supported_game_title_id).
pub fn valid_tid(t: &str) -> bool {
    let b = t.as_bytes();
    b.len() == 9 && b[..4].iter().all(u8::is_ascii_uppercase) && b[4..].iter().all(u8::is_ascii_digit)
        && !t.starts_with("NPXS")
}
