//! The .p5z game file as a playable source: the rules of the payload that need no PS5. The engine's
//! `info --play` JSON is read here, each loose file is given its destination, and the `maxpack.image`
//! text that ampr-emu-ng reads is written here (the text is a contract: see `image_text`). A .p5z
//! that grew by appending (index version 2) is played only with an ampr-emu-ng that knows it
//! (`ng_plays`).

use crate::cache::{reserved, LIB_DIRS, P5Z_MOUNT_DIR};
use serde::Deserialize;
use std::collections::BTreeMap;

/// Most volumes one game file may have (the limit of the `maxpack.image` contract).
pub const MAX_VOLUMES: usize = 256;
/// Free space kept over the loose files, the same margin the installer keeps.
pub const SPACE_MARGIN: u64 = 256 << 20;
/// The first ampr-emu-ng whose p5z mode plays a .p5z that grew by appending: chunks in any order, room
/// no chunk uses, volumes with no chunk, and volume headers that count fewer volumes than the index.
/// An older one refuses the grown index at the game's start and serves none of its packed files.
pub const GROWN_NG: &str = "v0.5.1-maxpack.2";

#[derive(Clone, Debug, PartialEq, Eq, Deserialize)]
pub struct Loose {
    pub path: String,
    pub size: u64,
}

#[derive(Clone, Debug, PartialEq, Eq, Deserialize)]
pub struct Volume {
    pub id: u32,
    pub offset: u64,
    pub length: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PlayInfo {
    pub title_id: String,
    pub version: String,
    pub name: String,
    pub archive_id: String,
    pub size: u64,
    pub loose_bytes: u64,
    pub loose: Vec<Loose>,
    pub volumes: Vec<Volume>,
    /// The version of the .p5z's index: 1 as packed, 2 once it grew by appending (a DLC added, or the
    /// files a new version of the game changes).
    pub index_version: u32,
    /// How many times it grew: 0 for index version 1, at least 1 for 2.
    pub grown: u32,
}

impl PlayInfo {
    /// The .p5z grew by appending: only an ampr-emu-ng from GROWN_NG on plays it (`ng_plays`).
    pub fn grew(&self) -> bool {
        self.index_version >= 2
    }
}

/// Why a .p5z cannot be played.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Unplayable {
    /// A short English reason (it goes to the log and to the page).
    Damaged(String),
    TooManyVolumes(usize),
    /// Its index is of this version, above the ones this MaxPack reads: made by a newer MaxPack for the PC, to be
    /// played once MaxPack for the console is updated. Never "damaged".
    Newer(u32),
}

impl Unplayable {
    pub fn code(&self) -> &'static str {
        match self {
            Unplayable::Damaged(_) => "damaged",
            Unplayable::TooManyVolumes(_) => "too_many_volumes",
            Unplayable::Newer(_) => "newer",
        }
    }
}

/// A .p5z's header and footer, in bytes (the engine's P5Z_HEADER and P5Z_FOOTER).
pub const HEADER_LEN: u64 = 64;
pub const FOOTER_LEN: u64 = 32;
/// The largest index the engine reads (its INDEX_MAX).
pub const INDEX_MAX: u64 = 1 << 31;

/// Where a .p5z's footer says its index is, and the CRC-32 of its compressed bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IndexAt {
    pub offset: u64,
    pub len: u64,
    pub crc: u32,
}

/// CRC-32 (IEEE, as zlib's `crc32`, which the engine's footer and index use), going on from `prev` (0 to begin).
pub fn crc32(prev: u32, data: &[u8]) -> u32 {
    let mut c = !prev;
    for &b in data {
        c ^= u32::from(b);
        for _ in 0..8 {
            c = (c >> 1) ^ (0xedb8_8320 & 0u32.wrapping_sub(c & 1));
        }
    }
    !c
}

fn le64(b: &[u8]) -> u64 {
    u64::from_le_bytes(b[..8].try_into().unwrap_or([0; 8]))
}

fn le32(b: &[u8]) -> u32 {
    u32::from_le_bytes(b[..4].try_into().unwrap_or([0; 4]))
}

/// The index a .p5z of `size` bytes has, by its last 32 bytes, as the engine reads them: `P5ZFOOT1`, the footer's
/// own CRC-32, and an index of 1 byte to INDEX_MAX right before the footer, past the header. None for anything
/// else (cut short, damaged, an append under way).
pub fn footer_index(footer: &[u8; 32], size: u64) -> Option<IndexAt> {
    if &footer[..8] != b"P5ZFOOT1" || crc32(0, &footer[..28]) != le32(&footer[28..]) {
        return None;
    }
    let (offset, len) = (le64(&footer[8..]), le64(&footer[16..]));
    let end = offset.checked_add(len)?.checked_add(FOOTER_LEN)?;
    (offset >= HEADER_LEN && len > 0 && len <= INDEX_MAX && end == size).then_some(IndexAt { offset, len, crc: le32(&footer[24..]) })
}

/// The version an index says, from its first bytes once decompressed: `P5ZIDX01`, then the version (u32 LE).
pub fn index_head_version(head: &[u8]) -> Option<u32> {
    (head.len() >= 12 && &head[..8] == b"P5ZIDX01").then(|| le32(&head[8..]))
}

/// What the console makes of an index version it read itself (src/p5z.rs, for a .p5z the engine refused, which it
/// does before saying why for a version it does not know): Newer above the versions read here, else nothing to add.
pub fn index_verdict(version: u32) -> Option<Unplayable> {
    (version > INDEX_VERSIONS[INDEX_VERSIONS.len() - 1]).then_some(Unplayable::Newer(version))
}

/// Extension p5z in any case, with a name before it.
pub fn is_p5z_name(name: &str) -> bool {
    matches!(name.rsplit_once('.'), Some((stem, ext)) if !stem.is_empty() && ext.eq_ignore_ascii_case("p5z"))
}

/// What `maxpack info --play` prints; every field is required but `index_version` and `grown`, which
/// the engine adds only for a .p5z that grew (absent: version 1, grown 0).
#[derive(Deserialize)]
struct Raw {
    title_id: String,
    version: String,
    name: String,
    archive_id: String,
    size: u64,
    loose_bytes: u64,
    loose: Vec<Loose>,
    volumes: Vec<Volume>,
    #[serde(default = "first_index")]
    index_version: u32,
    #[serde(default)]
    grown: u32,
}

fn first_index() -> u32 {
    1
}

/// The index versions this MaxPack reads: a later one may need a later ampr-emu-ng than GROWN_NG.
const INDEX_VERSIONS: [u32; 2] = [1, 2];

/// The loose files a game needs to be installed and run.
const REQUIRED: [&str; 4] = ["ampr_zstd.index", "ampr_emu.index", "eboot.bin", "sce_sys/param.json"];

pub fn parse_play_json(s: &str) -> Result<PlayInfo, Unplayable> {
    let bad = |m: &str| Unplayable::Damaged(m.to_string());
    let raw: Raw = serde_json::from_str(s).map_err(|e| Unplayable::Damaged(format!("bad info: {e}")))?;
    if raw.volumes.len() > MAX_VOLUMES {
        return Err(Unplayable::TooManyVolumes(raw.volumes.len()));
    }
    if raw.volumes.is_empty() {
        return Err(bad("no volumes"));
    }
    if raw.volumes.iter().enumerate().any(|(i, v)| v.id as usize != i) {
        return Err(bad("volume ids are not 0..n-1 in order"));
    }
    // A later version comes from a newer MaxPack for the PC: never "damaged" (index_verdict).
    if let Some(newer) = index_verdict(raw.index_version) {
        return Err(newer);
    }
    if !INDEX_VERSIONS.contains(&raw.index_version) {
        return Err(Unplayable::Damaged(format!("index version {} (this MaxPack reads versions 1 and 2)", raw.index_version)));
    }
    if (raw.index_version >= 2) != (raw.grown > 0) {
        return Err(Unplayable::Damaged(format!("index version {} that grew {} times", raw.index_version, raw.grown)));
    }
    if let Some(missing) = REQUIRED.iter().find(|r| !raw.loose.iter().any(|l| l.path == **r)) {
        return Err(Unplayable::Damaged(format!("missing loose file {missing}")));
    }
    let sum = raw.loose.iter().try_fold(0u64, |a, l| a.checked_add(l.size));
    if sum != Some(raw.loose_bytes) {
        return Err(bad("loose_bytes is not the sum of the loose files"));
    }
    Ok(PlayInfo {
        title_id: raw.title_id,
        version: raw.version,
        name: raw.name,
        archive_id: raw.archive_id,
        size: raw.size,
        loose_bytes: raw.loose_bytes,
        loose: raw.loose,
        volumes: raw.volumes,
        index_version: raw.index_version,
        grown: raw.grown,
    })
}

/// Whether the ampr-emu-ng in use plays `info`'s .p5z: `ng` is its version as MaxPack knows it (the
/// bundled copy's VERSION, or the tag of the release it downloaded), None when it cannot tell (a file
/// put there by hand, a copy an older MaxPack wrote, none). A .p5z that never grew plays with any, as
/// before; one that grew only with GROWN_NG or later (`crate::version`, which compares X.Y.Z), so an
/// older one never starts it half-way. Err: why not, in English.
pub fn ng_plays(info: &PlayInfo, ng: Option<&str>) -> Result<(), String> {
    ng_plays_against(info, ng, GROWN_NG)
}

/// ng_plays with `need` in place of GROWN_NG. A `need` that does not parse refuses every .p5z that
/// grew (fails closed: no ampr-emu-ng is known to play it).
fn ng_plays_against(info: &PlayInfo, ng: Option<&str>, need: &str) -> Result<(), String> {
    if !info.grew() {
        return Ok(());
    }
    let Some(least) = crate::version::parse_version(need) else {
        return Err(format!("this .p5z grew (index version {}) and the ampr-emu-ng it needs ({need:?}) is not a version", info.index_version));
    };
    if ng.and_then(crate::version::parse_version).is_some_and(|v| v >= least) {
        return Ok(());
    }
    let have = match ng.map(str::trim) {
        Some(v) if !v.is_empty() => format!("the ampr-emu-ng in use is {v}"),
        _ => "the version of the ampr-emu-ng in use is not known".to_string(),
    };
    Err(format!("this .p5z grew (index version {}) and needs ampr-emu-ng {need} or later; {have}", info.index_version))
}

/// Where a loose file of the .p5z goes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LooseDest {
    /// The game's own folder (what /app0 shows).
    App0,
    /// The game's fakelib folder (a file straight inside fakelib/ or fakelib2/).
    GameLib,
    /// Never written: unsafe paths and the names the mount keeps for itself.
    Skip,
}

/// Unsafe paths are skipped, and so is any path whose first component is one of MaxPack's names in
/// a cache (crate::cache::RESERVED, any case, at any depth: `.done/x` too), but a file straight
/// inside a lib dir, which goes to the fakelib composition.
pub fn loose_dest(path: &str) -> LooseDest {
    if path.is_empty() || path.starts_with('/') || path.contains(['\\', '\0']) {
        return LooseDest::Skip;
    }
    let parts: Vec<&str> = path.split('/').collect();
    if parts.iter().any(|p| p.is_empty() || *p == "." || *p == "..") {
        return LooseDest::Skip;
    }
    let first = parts[0];
    if parts.len() == 2 && LIB_DIRS.iter().any(|d| d.eq_ignore_ascii_case(first)) {
        LooseDest::GameLib
    } else if reserved(first) {
        LooseDest::Skip
    } else {
        LooseDest::App0
    }
}

/// The `maxpack.image` text (UTF-8, `\n` lines, no `\r`): `maxpack-image 1`, `p5z/<file name>`
/// (the .p5z relative to /app0), then `<id> <offset> <length>` per volume. ampr-emu-ng parses
/// exactly this. The file name is the one the directory gave, in UTF-8 (MaxPack's PC app keeps every
/// character of the title in it), and ampr-emu-ng opens the .p5z with those same bytes.
pub fn image_text(file_name: &str, volumes: &[Volume]) -> Result<String, String> {
    if file_name.is_empty() || file_name == "." || file_name == ".."
        || file_name.contains(['/', '\\'])
        // The contract has no control byte: none below 0x20 (\n, \r, NUL, tab...) and no 0x7F.
        || file_name.bytes().any(|b| b < 0x20 || b == 0x7f) {
        return Err(format!("unusable file name {file_name:?}"));
    }
    if volumes.is_empty() || volumes.len() > MAX_VOLUMES {
        return Err(format!("{} volumes (1 to {MAX_VOLUMES} allowed)", volumes.len()));
    }
    if volumes.iter().enumerate().any(|(i, v)| v.id as usize != i) {
        return Err("volume ids are not 0..n-1 in order".to_string());
    }
    let mut text = format!("maxpack-image 1\n{P5Z_MOUNT_DIR}/{file_name}\n");
    for v in volumes {
        text.push_str(&format!("{} {} {}\n", v.id, v.offset, v.length));
    }
    Ok(text)
}

/// A stable id of a .p5z that survives a rename or a move: FNV-1a 64 of the size (LE), the
/// 64-byte header and the 32-byte footer, as 16 lowercase hex digits.
pub fn p5z_id(header: &[u8; 64], footer: &[u8; 32], size: u64) -> String {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for b in size.to_le_bytes().iter().chain(header).chain(footer) {
        h ^= u64::from(*b);
        h = h.wrapping_mul(0x0000_0100_0000_01b3);
    }
    format!("{h:016x}")
}

/// How many bytes are missing to install `need` loose bytes and keep the margin; None = fits.
pub fn space_short(free: u64, need: u64) -> Option<u64> {
    let want = need.saturating_add(SPACE_MARGIN);
    want.checked_sub(free).filter(|s| *s > 0)
}

/// What a scan pass makes of the engine's refusal of a .p5z.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    /// Memoized by (size, mtime), listed in `unplayable` and toasted.
    Believe,
    /// Tried again next pass, like a file that cannot be opened.
    Retry,
}

/// A `Damaged` verdict may be a read error and not the file (a USB drive that resets or is pulled
/// during the open, the engine out of memory): a first `Damaged` for a file is provisional, and only
/// the same verdict in the next pass for the same file (same size and mtime) is believed. `previous`
/// is the last pass's provisional refusal of this path: (size, mtime, verdict). Too many volumes
/// comes from a description that opened and read fine: believed at once.
pub fn refusal<M: PartialEq>(previous: Option<&(u64, M, Unplayable)>, size: u64, mtime: &M, verdict: &Unplayable) -> Refusal {
    match verdict {
        // Newer: from an index that read whole, CRCs and all (or the engine's own description).
        Unplayable::TooManyVolumes(_) | Unplayable::Newer(_) => Refusal::Believe,
        Unplayable::Damaged(_) => match previous {
            Some((s, m, v)) if *s == size && m == mtime && v == verdict => Refusal::Believe,
            _ => Refusal::Retry,
        },
    }
}

/// The scan's rule (`refusal`) for the engine's open of a .p5z during a build or a fakelib refresh
/// (the scan keeps its own refusals, pass by pass): the last try's provisional refusal of each path.
pub struct BuildRefusals<K, M> {
    last: BTreeMap<K, (u64, M, Unplayable)>,
}

impl<K, M> BuildRefusals<K, M> {
    pub const fn new() -> Self {
        BuildRefusals { last: BTreeMap::new() }
    }
}

impl<K: Ord, M: PartialEq> BuildRefusals<K, M> {
    /// What a build makes of the engine's failure to open the .p5z at `key` (its size and mtime
    /// now). `verdict` None: the file could not be read, tried again (Retry) and never believed;
    /// it also drops the last refusal, so a damaged verdict after it is a first one. Else `refusal`
    /// against the last try's: a Retry is kept for the next try, a Believe drops it (the build is
    /// memoized then, and a later try, a forced rebuild, starts over).
    pub fn judge(&mut self, key: K, size: u64, mtime: M, verdict: Option<Unplayable>) -> Refusal {
        let Some(verdict) = verdict else {
            self.last.remove(&key);
            return Refusal::Retry;
        };
        let r = refusal(self.last.get(&key), size, &mtime, &verdict);
        match r {
            Refusal::Retry => {
                self.last.insert(key, (size, mtime, verdict));
            }
            Refusal::Believe => {
                self.last.remove(&key);
            }
        }
        r
    }

    /// Drops the last refusal of `key`: the .p5z opened (that refusal was not the file), or it
    /// cannot even be stat'ed now (a read error).
    pub fn forget(&mut self, key: &K) {
        self.last.remove(key);
    }
}

/// The loose bytes a .p5z needs (its loose files `loose_dest` writes), kept by path for that very
/// file (the same size and mtime) while it is short of space: a retry then measures only the free
/// space, and opens and validates the .p5z again once it fits. Any other size or mtime drops it.
pub struct NeedMemo<K, M> {
    need: BTreeMap<K, (u64, M, u64)>,
}

impl<K, M> NeedMemo<K, M> {
    pub const fn new() -> Self {
        NeedMemo { need: BTreeMap::new() }
    }
}

impl<K: Ord, M: PartialEq> NeedMemo<K, M> {
    /// The need kept for the file at `key` when it still has this size and mtime; a memo of
    /// another size or mtime is dropped.
    pub fn get(&mut self, key: &K, size: u64, mtime: &M) -> Option<u64> {
        match self.need.get(key) {
            Some((s, m, need)) if *s == size && m == mtime => Some(*need),
            Some(_) => {
                self.need.remove(key);
                None
            }
            None => None,
        }
    }

    pub fn set(&mut self, key: K, size: u64, mtime: M, need: u64) {
        self.need.insert(key, (size, mtime, need));
    }

    pub fn forget(&mut self, key: &K) {
        self.need.remove(key);
    }
}

/// A failed look at a .p5z's index (`LookMemo`) is tried again after this long.
pub const LOOK_RETRY_MS: u64 = 30_000;
/// The most files a LookMemo keeps; past it, it starts over.
pub const LOOK_MAX: usize = 256;

/// The index version the console's own look read (src/p5z.rs index_version) of each .p5z the Install tab lists, kept
/// by path for that very file (the same size and mtime); a look that read nothing (None) only for LOOK_RETRY_MS.
pub struct LookMemo<K, M> {
    seen: BTreeMap<K, (u64, M, Option<u32>, u64)>,
}

impl<K, M> LookMemo<K, M> {
    pub const fn new() -> Self {
        LookMemo { seen: BTreeMap::new() }
    }
}

impl<K: Ord, M: PartialEq> LookMemo<K, M> {
    /// What the last look found, while it holds: Some(Some(version)) for this very file, Some(None) for a failed look
    /// within LOOK_RETRY_MS; None: look again.
    pub fn get(&self, key: &K, size: u64, mtime: &M, now_ms: u64) -> Option<Option<u32>> {
        let (s, m, version, at) = self.seen.get(key)?;
        let fresh = version.is_some() || now_ms.saturating_sub(*at) < LOOK_RETRY_MS;
        (*s == size && m == mtime && fresh).then_some(*version)
    }

    pub fn set(&mut self, key: K, size: u64, mtime: M, version: Option<u32>, now_ms: u64) {
        if self.seen.len() >= LOOK_MAX {
            self.seen.clear();
        }
        self.seen.insert(key, (size, mtime, version, now_ms));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const GOOD: &str = r#"{"title_id":"TEST00001","version":"01.000.000","name":"","archive_id":"c8fbfad535278d2f795372c334b345bb","size":2094371,"loose_bytes":39617,"loose":[{"path":"eboot.bin","size":10000},{"path":"empty.txt","size":0},{"path":"sce_sys/param.json","size":200},{"path":"ampr_emu.index","size":17856},{"path":"ampr_zstd.index","size":11561}],"volumes":[{"id":0,"offset":10149,"length":2067009}]}"#;

    /// `maxpack info --play` of the engine in vendor/maxpack (mp_play_json) for a .p5z that never grew:
    /// no index_version or grown, the JSON of before. The test game (PPSA00000) after `maxpack compact`.
    const ENGINE_V1: &str = r#"{"title_id":"PPSA00000","version":"01.001.000","name":"MaxPack Test Game","archive_id":"6463a59194edf62f12e7969155bea93e","size":458786,"loose_bytes":344957,"loose":[{"path":"addcont0/data/empty.bin","size":0},{"path":"addcont0/module/libdlc.prx","size":4004},{"path":"addcont0/sce_sys/icon0.png","size":3008},{"path":"addcont0/sce_sys/param.json","size":184},{"path":"data/empty.bin","size":0},{"path":"dlc_emu.ini","size":82},{"path":"eboot.bin","size":200004},{"path":"modules/libgame.prx","size":70004},{"path":"sce_sys/about/legal.txt","size":3000},{"path":"sce_sys/changeinfo/changeinfo.xml","size":53},{"path":"sce_sys/icon0.png","size":20008},{"path":"sce_sys/keystone","size":96},{"path":"sce_sys/license.dat","size":1024},{"path":"sce_sys/nptitle.dat","size":80},{"path":"sce_sys/param.json","size":241},{"path":"sce_sys/pic0.png","size":30008},{"path":"sce_sys/playgo-chunk.dat","size":504},{"path":"sce_sys/trophy2/trophy00.ucp","size":5000},{"path":"ampr_emu.index","size":5040},{"path":"ampr_zstd.index","size":2617}],"volumes":[{"id":0,"offset":334746,"length":118163}]}"#;

    /// The same engine for that game's .p5z that grew (index version 2), as it was before the compact:
    /// packed in 3 volumes, then `maxpack update` and `maxpack add --dlc` appended volumes 3 and 4. The
    /// update left volume 1 with no chunk at all and room no chunk uses in volumes 0 and 2, and the
    /// headers of volumes 0 to 3 count fewer volumes than the index: every volume is still listed.
    const ENGINE_V2: &str = r#"{"title_id":"PPSA00000","version":"01.001.000","name":"MaxPack Test Game","archive_id":"b2a1216e3c7eb65de2cb3f47a1b3d4a1","size":962113,"loose_bytes":344989,"loose":[{"path":"addcont0/data/empty.bin","size":0},{"path":"addcont0/module/libdlc.prx","size":4004},{"path":"addcont0/sce_sys/icon0.png","size":3008},{"path":"addcont0/sce_sys/param.json","size":184},{"path":"data/empty.bin","size":0},{"path":"dlc_emu.ini","size":82},{"path":"eboot.bin","size":200004},{"path":"modules/libgame.prx","size":70004},{"path":"sce_sys/about/legal.txt","size":3000},{"path":"sce_sys/changeinfo/changeinfo.xml","size":53},{"path":"sce_sys/icon0.png","size":20008},{"path":"sce_sys/keystone","size":96},{"path":"sce_sys/license.dat","size":1024},{"path":"sce_sys/nptitle.dat","size":80},{"path":"sce_sys/param.json","size":241},{"path":"sce_sys/pic0.png","size":30008},{"path":"sce_sys/playgo-chunk.dat","size":504},{"path":"sce_sys/trophy2/trophy00.ucp","size":5000},{"path":"ampr_emu.index","size":5040},{"path":"ampr_zstd.index","size":2649}],"volumes":[{"id":0,"offset":327463,"length":93380},{"id":1,"offset":420843,"length":262208},{"id":2,"offset":683051,"length":138060},{"id":3,"offset":826577,"length":93361},{"id":4,"offset":932609,"length":23423}],"index_version":2,"grown":2}"#;

    fn damaged(s: &str) -> bool {
        matches!(parse_play_json(s), Err(Unplayable::Damaged(_)))
    }

    #[test]
    fn a_p5z_that_never_grew_reads_as_before() {
        for json in [GOOD, ENGINE_V1] {
            let info = parse_play_json(json).unwrap();
            assert_eq!((info.index_version, info.grown, info.grew()), (1, 0, false));
        }
        let info = parse_play_json(ENGINE_V1).unwrap();
        assert_eq!((info.title_id.as_str(), info.version.as_str()), ("PPSA00000", "01.001.000"));
        assert_eq!(info.volumes, vec![Volume { id: 0, offset: 334_746, length: 118_163 }]);
        assert_eq!(image_text("a.p5z", &info.volumes).unwrap(), "maxpack-image 1\np5z/a.p5z\n0 334746 118163\n");
        // The fields said outright with a .p5z's values that never grew: the same.
        let said = ENGINE_V1.replace("}]}", "}],\"index_version\":1,\"grown\":0}");
        assert_eq!(parse_play_json(&said), Ok(info.clone()));
        assert_eq!(parse_play_json(&ENGINE_V1.replace("}]}", "}],\"index_version\":1}")), Ok(info));
    }

    #[test]
    fn a_p5z_that_grew_gives_every_volume_to_maxpack_image() {
        let info = parse_play_json(ENGINE_V2).unwrap();
        assert_eq!((info.index_version, info.grown, info.grew()), (2, 2, true));
        assert_eq!((info.size, info.loose_bytes, info.loose.len()), (962_113, 344_989, 20));
        // Volume 1 has no chunk left and 0 and 2 have holes: each is still one line, at its region.
        assert_eq!(
            image_text("grown.p5z", &info.volumes).unwrap(),
            "maxpack-image 1\np5z/grown.p5z\n0 327463 93380\n1 420843 262208\n2 683051 138060\n3 826577 93361\n4 932609 23423\n"
        );
    }

    #[test]
    fn a_p5z_that_grew_keeps_the_limits() {
        let one = "[{\"id\":0,\"offset\":327463,\"length\":93380},{\"id\":1,\"offset\":420843,\"length\":262208},{\"id\":2,\"offset\":683051,\"length\":138060},{\"id\":3,\"offset\":826577,\"length\":93361},{\"id\":4,\"offset\":932609,\"length\":23423}]";
        assert!(ENGINE_V2.contains(one));
        let vols: Vec<String> = (0..257).map(|i| format!("{{\"id\":{i},\"offset\":0,\"length\":1}}")).collect();
        let many = ENGINE_V2.replace(one, &format!("[{}]", vols.join(",")));
        assert_eq!(parse_play_json(&many), Err(Unplayable::TooManyVolumes(257)));
        let at_most: Vec<String> = (0..256).map(|i| format!("{{\"id\":{i},\"offset\":0,\"length\":1}}")).collect();
        assert!(parse_play_json(&ENGINE_V2.replace(one, &format!("[{}]", at_most.join(",")))).is_ok());
        assert!(damaged(&ENGINE_V2.replace(one, "[]")));
        assert!(damaged(&ENGINE_V2.replace("{\"id\":3,", "{\"id\":5,")));
        assert!(damaged(&ENGINE_V2.replace("\"loose_bytes\":344989", "\"loose_bytes\":344990")));
        assert!(damaged(&ENGINE_V2.replace("ampr_zstd.index", "other.index")));
    }

    #[test]
    fn index_fields_that_do_not_agree_are_damaged() {
        let tail = ",\"index_version\":2,\"grown\":2}";
        assert!(ENGINE_V2.ends_with(tail));
        let with = |t: &str| ENGINE_V2.replace(tail, t);
        // No version 0.
        assert!(damaged(&with(",\"index_version\":0}")));
        // A grown file says how many times it grew; one that never grew says none.
        for t in [",\"index_version\":2}", ",\"index_version\":2,\"grown\":0}", ",\"grown\":1}", ",\"index_version\":1,\"grown\":1}"] {
            assert!(damaged(&with(t)), "{t}");
        }
        // Not numbers, or past u32.
        for t in [",\"index_version\":\"2\",\"grown\":2}", ",\"index_version\":2,\"grown\":-1}", ",\"index_version\":2,\"grown\":4294967296}",
                  ",\"index_version\":null,\"grown\":2}"] {
            assert!(damaged(&with(t)), "{t}");
        }
        assert_eq!(parse_play_json(&with(",\"index_version\":2,\"grown\":4294967295}")).map(|i| i.grown), Ok(u32::MAX));
    }

    /// An index version this MaxPack does not read comes from a newer MaxPack for the PC: never "damaged" (the user
    /// could delete a good file for it), and believed at once, as it comes from an index that read whole.
    #[test]
    fn a_newer_index_is_never_damaged() {
        let tail = ",\"index_version\":2,\"grown\":2}";
        for (t, v) in [(",\"index_version\":3,\"grown\":1}", 3), (",\"index_version\":3}", 3), (",\"index_version\":3,\"grown\":0}", 3),
                       (",\"index_version\":4294967295,\"grown\":1}", u32::MAX)] {
            assert_eq!(parse_play_json(&ENGINE_V2.replace(tail, t)), Err(Unplayable::Newer(v)), "{t}");
        }
        assert_eq!(Unplayable::Newer(3).code(), "newer");
        assert_eq!(refusal::<i64>(None, 10, &5, &Unplayable::Newer(3)), Refusal::Believe);
        // What the console's own look at the index makes of the version it read (the engine refuses such a file
        // before it describes it): only a version above the ones read here is newer.
        assert_eq!(index_verdict(3), Some(Unplayable::Newer(3)));
        assert_eq!(index_verdict(u32::MAX), Some(Unplayable::Newer(u32::MAX)));
        for v in [0, 1, 2] {
            assert_eq!(index_verdict(v), None, "{v}");
        }
    }

    #[test]
    fn crc32_is_zlibs() {
        assert_eq!(crc32(0, b"123456789"), 0xcbf4_3926);
        assert_eq!(crc32(0, b""), 0);
        // In pieces, as a long index is read.
        assert_eq!(crc32(crc32(0, b"1234"), b"56789"), 0xcbf4_3926);
    }

    /// The footer of a .p5z the PC engine packed (a made-up game folder, 1,206,514 bytes): its index is 828
    /// bytes at 1,205,654, with that CRC.
    const FOOTER: &str = "50355a464f4f543196651200000000003c03000000000000ee367d4d4762ee52";

    fn footer() -> [u8; 32] {
        std::array::from_fn(|i| u8::from_str_radix(&FOOTER[2 * i..2 * i + 2], 16).unwrap())
    }

    #[test]
    fn the_footer_says_where_the_index_is() {
        assert_eq!(footer_index(&footer(), 1_206_514), Some(IndexAt { offset: 1_205_654, len: 828, crc: 0x4d7d_36ee }));
        // Another size: the index is not where it says.
        assert_eq!(footer_index(&footer(), 1_206_515), None);
        // Not a footer, or one whose CRC does not match.
        let mut f = footer();
        f[0] = b'X';
        assert_eq!(footer_index(&f, 1_206_514), None);
        let mut f = footer();
        f[20] ^= 1;
        assert_eq!(footer_index(&f, 1_206_514), None);
        // Out of place, with its CRC made right: inside the header, empty, past 2 GiB, wrapping around.
        let with = |offset: u64, len: u64| {
            let mut f = footer();
            f[8..16].copy_from_slice(&offset.to_le_bytes());
            f[16..24].copy_from_slice(&len.to_le_bytes());
            let c = crc32(0, &f[..28]);
            f[28..].copy_from_slice(&c.to_le_bytes());
            f
        };
        assert_eq!(footer_index(&with(1_205_654, 828), 1_206_514).map(|i| i.len), Some(828));
        assert_eq!(footer_index(&with(63, 1_206_514 - 63 - 32), 1_206_514), None);
        assert_eq!(footer_index(&with(1_206_482, 0), 1_206_514), None);
        assert_eq!(footer_index(&with(64, (2 << 30) + 1), 64 + (2 << 30) + 1 + 32), None);
        assert_eq!(footer_index(&with(u64::MAX - 10, 100), 1_206_514), None);
        assert_eq!(footer_index(&footer(), 31), None);
    }

    #[test]
    fn the_index_says_its_version() {
        let head = |v: u32| [&b"P5ZIDX01"[..], &v.to_le_bytes(), &[0; 4]].concat();
        assert_eq!(index_head_version(&head(1)), Some(1));
        assert_eq!(index_head_version(&head(3)), Some(3));
        assert_eq!(index_head_version(&head(3)[..12]), Some(3));
        assert_eq!(index_head_version(&head(3)[..11]), None);
        assert_eq!(index_head_version(&[&b"P5ZIDX02"[..], &3u32.to_le_bytes()].concat()), None);
        assert_eq!(index_head_version(b""), None);
    }

    #[test]
    fn a_grown_p5z_needs_an_ampr_emu_ng_that_knows_it() {
        assert_eq!(GROWN_NG, "v0.5.1-maxpack.2");
        assert_eq!(crate::version::parse_version(GROWN_NG), Some((0, 5, 1, 2)));
        let v1 = parse_play_json(ENGINE_V1).unwrap();
        let v2 = parse_play_json(ENGINE_V2).unwrap();
        // A .p5z that never grew plays as before, with any ampr-emu-ng that has the p5z mode.
        for ng in [None, Some("v0.5.1-maxpack.1"), Some("not a version")] {
            assert_eq!(ng_plays(&v1, ng), Ok(()), "{ng:?}");
        }
        for ng in ["v0.5.1-maxpack.2", "v0.5.1-maxpack.7", "0.5.2", "v0.6.0-maxpack.1", "v1.0.0-maxpack.1"] {
            assert_eq!(ng_plays(&v2, Some(ng)), Ok(()), "{ng}");
        }
        // Older (upstream's own v0.5.1 too: no p5z mode), or a version MaxPack cannot tell (a file put
        // there by hand, none): never half-way.
        for ng in [Some("v0.5.1-maxpack.1"), Some("v0.5.1"), Some("v0.5.0-maxpack.9"), Some("v0.4.0"), Some("v0.5.2-beta"), Some(""), None] {
            let why = ng_plays(&v2, ng).expect_err("refused");
            assert!(why.contains(GROWN_NG), "{ng:?}: {why}");
            if let Some(v) = ng.filter(|v| !v.is_empty()) {
                assert!(why.contains(v), "{ng:?}: {why}");
            }
        }
    }

    /// Fails closed: a needed version that does not parse refuses every .p5z that grew, whatever the
    /// ampr-emu-ng in use, and still none that never grew.
    #[test]
    fn a_needed_version_that_does_not_parse_refuses_every_grown_p5z() {
        let v1 = parse_play_json(ENGINE_V1).unwrap();
        let v2 = parse_play_json(ENGINE_V2).unwrap();
        for need in ["", "not a version", "v0.5", "v0.5.2-beta", "V0.5.2"] {
            for ng in [None, Some("v0.5.1-maxpack.2"), Some("v9.9.9-maxpack.1"), Some("garbage")] {
                assert!(ng_plays_against(&v2, ng, need).is_err(), "{need:?} {ng:?}");
                assert_eq!(ng_plays_against(&v1, ng, need), Ok(()), "{need:?} {ng:?}");
            }
        }
        assert_eq!(ng_plays_against(&v2, Some("v0.5.1-maxpack.2"), GROWN_NG), Ok(()));
    }

    /// The ampr-emu-ng that maxpack.elf carries (vendor/ampr-emu-ng/VERSION, src/main.rs BUNDLED_AMPR)
    /// plays a .p5z that grew: a build that bundles an older one fails here.
    #[test]
    fn the_bundled_ampr_emu_ng_plays_a_grown_p5z() {
        let bundled = include_str!("../../../vendor/ampr-emu-ng/VERSION").trim();
        assert_eq!(ng_plays(&parse_play_json(ENGINE_V2).unwrap(), Some(bundled)), Ok(()), "{bundled}");
    }

    #[test]
    fn is_p5z_name_table() {
        assert!(is_p5z_name("a.p5z"));
        assert!(is_p5z_name("A.P5Z"));
        assert!(!is_p5z_name("a.p5z.part"));
        assert!(!is_p5z_name("a.pkg"));
        assert!(!is_p5z_name("p5z"));
    }

    #[test]
    fn parse_play_json_ok_and_damaged() {
        let info = parse_play_json(GOOD).unwrap();
        assert_eq!(info.title_id, "TEST00001");
        assert_eq!(info.version, "01.000.000");
        assert_eq!(info.name, "");
        assert_eq!(info.archive_id, "c8fbfad535278d2f795372c334b345bb");
        assert_eq!((info.size, info.loose_bytes), (2094371, 39617));
        assert_eq!(info.loose.len(), 5);
        assert_eq!(info.volumes, vec![Volume { id: 0, offset: 10149, length: 2067009 }]);

        assert!(damaged(""));
        assert!(damaged("not json"));
        assert!(damaged("[]"));
        // A missing field.
        assert!(damaged(&GOOD.replace("\"archive_id\":\"c8fbfad535278d2f795372c334b345bb\",", "")));
        // Volume ids not 0..n-1 in order, and no volumes.
        assert!(damaged(&GOOD.replace("\"id\":0", "\"id\":1")));
        assert!(damaged(&GOOD.replace("[{\"id\":0,\"offset\":10149,\"length\":2067009}]", "[]")));
        // A required loose file missing.
        for f in ["ampr_zstd.index", "ampr_emu.index", "eboot.bin", "sce_sys/param.json"] {
            assert!(damaged(&GOOD.replace(f, "other.bin")), "{f}");
        }
        // loose_bytes is not the sum.
        assert!(damaged(&GOOD.replace("\"loose_bytes\":39617", "\"loose_bytes\":39618")));
        // Too many volumes.
        let vols: Vec<String> = (0..257).map(|i| format!("{{\"id\":{i},\"offset\":0,\"length\":1}}")).collect();
        let many = GOOD.replace("[{\"id\":0,\"offset\":10149,\"length\":2067009}]", &format!("[{}]", vols.join(",")));
        assert_eq!(parse_play_json(&many), Err(Unplayable::TooManyVolumes(257)));
        assert_eq!(Unplayable::TooManyVolumes(257).code(), "too_many_volumes");
        assert_eq!(Unplayable::Damaged("x".into()).code(), "damaged");
    }

    #[test]
    fn loose_dest_table() {
        // A reserved name below the root is the game's own.
        for p in ["eboot.bin", "sce_sys/param.json", "empty.txt", "ampr_emu.index", "a/b/c", "a/.done", "x/maxpack.image",
                  "data/kraken/x", "a/fakelib/libc.prx", ".doner", "fakelibs/x", "kraken.images/x"] {
            assert_eq!(loose_dest(p), LooseDest::App0, "{p}");
        }
        for p in ["fakelib/libc.prx", "FAKELIB2/x.sprx"] {
            assert_eq!(loose_dest(p), LooseDest::GameLib, "{p}");
        }
        for p in ["fakelib/sub/x", "../x", "/x", "a//b", ".done", "MAXPACK.IMAGE", "p5z/x", "kraken/x", "kraken.image",
                  "fakelib", "", ".", "a/./b", "a/..", "a/", "a\\b", "a\0b", ".built", ".AMPR", "kraken_zero.dat",
                  "fakelib2", "p5z", "kraken", "P5Z/x/y", "fakelib2/a/b"] {
            assert_eq!(loose_dest(p), LooseDest::Skip, "{p:?}");
        }
        // A control name as the first folder, at any depth: the extraction would make a folder of it.
        for p in [".done/x", ".built/x", ".ampr/x", "maxpack.image/x", "kraken.image/x", "kraken_zero.dat/x",
                  ".DONE/a/b", "Maxpack.Image/x/y", "KRAKEN_ZERO.DAT/x"] {
            assert_eq!(loose_dest(p), LooseDest::Skip, "{p:?}");
        }
    }

    /// Every name MaxPack keeps in a cache (crate::cache, where src/cache.rs takes them from) is
    /// refused as a loose file, alone and as the first folder of a path, in any case: never App0.
    #[test]
    fn loose_dest_refuses_every_cache_name() {
        use crate::cache::*;
        for name in [DONE, BUILT, AMPR_STATE, PKG_IMAGE, PKG_MOUNT_DIR, ZERO_VOLUME, P5Z_IMAGE, P5Z_MOUNT_DIR, FAKELIB, FAKELIB2] {
            assert!(RESERVED.contains(&name), "{name}");
        }
        assert_eq!(LIB_DIRS, [FAKELIB2, FAKELIB]);
        for name in RESERVED {
            for n in [name.to_string(), name.to_ascii_uppercase()] {
                for p in [n.clone(), format!("{n}/x/y")] {
                    assert_eq!(loose_dest(&p), LooseDest::Skip, "{p:?}");
                }
                // Straight inside a lib dir: the fakelib composition's, never /app0's.
                let want = if LIB_DIRS.contains(name) { LooseDest::GameLib } else { LooseDest::Skip };
                assert_eq!(loose_dest(&format!("{n}/x")), want, "{n}/x");
            }
        }
    }

    #[test]
    fn image_text_rules() {
        let v = |id| Volume { id, offset: 1234, length: 5678 };
        assert_eq!(image_text("basic.p5z", &[v(0)]).unwrap(), "maxpack-image 1\np5z/basic.p5z\n0 1234 5678\n");
        assert_eq!(image_text("a.p5z", &[v(0), v(1)]).unwrap(), "maxpack-image 1\np5z/a.p5z\n0 1234 5678\n1 1234 5678\n");
        assert!(image_text("Starfall Odyssey.p5z", &[v(0)]).is_ok());
        // UTF-8 names, as MaxPack's PC app writes them ("<Title> [<TITLE_ID>].p5z").
        for n in ["Café Quest.p5z", "Starfall Odyssey Nordström [PPSA10002].p5z", "スターフォール７ [PPSA10003].p5z",
                  "Starfall Racing® 7.p5z", "...", " a.p5z "] {
            assert_eq!(image_text(n, &[v(0)]).unwrap(), format!("maxpack-image 1\np5z/{n}\n0 1234 5678\n"), "{n:?}");
        }
        for n in ["a/b.p5z", "a\nb", "a\\b", "..", ".", "", "a\rb", "a\0b", "a\tb.p5z", "a\u{7f}b.p5z", "a\u{1}b.p5z",
                  "a\u{1f}b.p5z", "/a.p5z", "a.p5z/"] {
            assert!(image_text(n, &[v(0)]).is_err(), "{n:?}");
        }
        assert!(image_text("a.p5z", &[v(1), v(0)]).is_err());
        assert!(image_text("a.p5z", &[]).is_err());
        let ok: Vec<Volume> = (0..256).map(v).collect();
        assert!(image_text("a.p5z", &ok).is_ok());
        let many: Vec<Volume> = (0..257).map(v).collect();
        assert!(image_text("a.p5z", &many).is_err());
    }

    /// The shared golden case of the contract: ampr-emu-ng's `parse_image` test reads this exact text.
    #[test]
    fn image_text_golden_utf8() {
        let v = Volume { id: 0, offset: 1234, length: 5678 };
        assert_eq!(
            image_text("Starfall’s Odyssey™ 2 [PPSA01234].p5z", &[v]).unwrap(),
            "maxpack-image 1\np5z/Starfall’s Odyssey™ 2 [PPSA01234].p5z\n0 1234 5678\n"
        );
    }

    #[test]
    fn refusal_table() {
        use Refusal::*;
        let damaged = |s: &str| Unplayable::Damaged(s.to_string());
        let many = Unplayable::TooManyVolumes(300);
        let before = |size: u64, mtime: i64, u: &Unplayable| Some((size, mtime, u.clone()));
        // First sighting of a damaged verdict: tried again next pass.
        assert_eq!(refusal(None, 10, &5, &damaged("crc")), Retry);
        // The same verdict for the same file in the next pass: believed.
        assert_eq!(refusal(before(10, 5, &damaged("crc")).as_ref(), 10, &5, &damaged("crc")), Believe);
        // Another verdict, size or mtime: tried again (this pass is the new first sighting).
        assert_eq!(refusal(before(10, 5, &damaged("read error")).as_ref(), 10, &5, &damaged("crc")), Retry);
        assert_eq!(refusal(before(11, 5, &damaged("crc")).as_ref(), 10, &5, &damaged("crc")), Retry);
        assert_eq!(refusal(before(10, 6, &damaged("crc")).as_ref(), 10, &5, &damaged("crc")), Retry);
        assert_eq!(refusal(before(10, 5, &many).as_ref(), 10, &5, &damaged("crc")), Retry);
        // Too many volumes comes from a description that opened and read fine: believed at once.
        assert_eq!(refusal(None, 10, &5, &many), Believe);
        assert_eq!(refusal(before(10, 5, &damaged("crc")).as_ref(), 10, &5, &many), Believe);
    }

    /// The scan's rule for the engine's open during a build: a read error is tried again, a damaged
    /// verdict is believed only when the next try says the same for the same file.
    #[test]
    fn build_refusals_sequences() {
        use Refusal::*;
        let d = |s: &str| Some(Unplayable::Damaged(s.to_string()));
        let mut r = BuildRefusals::new();
        // A read error: tried again, never believed.
        assert_eq!(r.judge("a", 10, 5, None), Retry);
        assert_eq!(r.judge("a", 10, 5, None), Retry);
        // Damaged twice for the same file: believed the second time.
        assert_eq!(r.judge("a", 10, 5, d("crc")), Retry);
        assert_eq!(r.judge("a", 10, 5, d("crc")), Believe);
        // Believed (main.rs then memoizes the build): a later try, a forced rebuild, starts over.
        assert_eq!(r.judge("a", 10, 5, d("crc")), Retry);
        // A read error or an open that worked in between: the next damaged is a first one again.
        assert_eq!(r.judge("a", 10, 5, None), Retry);
        assert_eq!(r.judge("a", 10, 5, d("crc")), Retry);
        r.forget(&"a");
        assert_eq!(r.judge("a", 10, 5, d("crc")), Retry);
        assert_eq!(r.judge("a", 10, 5, d("crc")), Believe);
        // Another verdict, size or mtime: a first one again.
        assert_eq!(r.judge("a", 10, 5, d("crc")), Retry);
        assert_eq!(r.judge("a", 10, 5, d("short")), Retry);
        assert_eq!(r.judge("a", 11, 5, d("short")), Retry);
        assert_eq!(r.judge("a", 11, 6, d("short")), Retry);
        assert_eq!(r.judge("a", 11, 6, d("short")), Believe);
        // Each path on its own.
        assert_eq!(r.judge("b", 1, 1, d("crc")), Retry);
        assert_eq!(r.judge("c", 1, 1, d("crc")), Retry);
        assert_eq!(r.judge("b", 1, 1, d("crc")), Believe);
        // Too many volumes: believed at once.
        assert_eq!(r.judge("d", 1, 1, Some(Unplayable::TooManyVolumes(300))), Believe);
    }

    #[test]
    fn a_look_at_the_index_is_kept_while_the_file_stays_and_a_failed_one_briefly() {
        let mut m = LookMemo::new();
        assert_eq!(m.get(&"a", 10, &5, 0), None);
        m.set("a", 10, 5, Some(2), 0);
        // Kept while the file has that size and mtime, however long.
        assert_eq!(m.get(&"a", 10, &5, 10 * LOOK_RETRY_MS), Some(Some(2)));
        assert_eq!(m.get(&"a", 11, &5, 0), None);
        assert_eq!(m.get(&"a", 10, &6, 0), None);
        assert_eq!(m.get(&"b", 10, &5, 0), None);
        // A look that read nothing (a USB reset, out of memory) is tried again after LOOK_RETRY_MS.
        assert_eq!(LOOK_RETRY_MS, 30_000);
        m.set("c", 10, 5, None, 1000);
        assert_eq!(m.get(&"c", 10, &5, 1000 + LOOK_RETRY_MS - 1), Some(None));
        assert_eq!(m.get(&"c", 10, &5, 1000 + LOOK_RETRY_MS), None);
        // Bounded: past LOOK_MAX entries it starts over.
        let mut m = LookMemo::new();
        for i in 0..LOOK_MAX as u64 {
            m.set(i, 1, 1, Some(1), 0);
        }
        assert_eq!(m.get(&0, 1, &1, 0), Some(Some(1)));
        m.set(LOOK_MAX as u64, 1, 1, Some(1), 0);
        assert_eq!(m.get(&0, 1, &1, 0), None);
        assert_eq!(m.get(&(LOOK_MAX as u64), 1, &1, 0), Some(Some(1)));
    }

    #[test]
    fn need_memo_table() {
        let mut m = NeedMemo::new();
        assert_eq!(m.get(&"a", 10, &5), None);
        m.set("a", 10, 5, 1000);
        // Kept while the file is the same.
        assert_eq!(m.get(&"a", 10, &5), Some(1000));
        assert_eq!(m.get(&"a", 10, &5), Some(1000));
        assert_eq!(m.get(&"b", 10, &5), None);
        // Another size or mtime drops it, also for the old one.
        assert_eq!(m.get(&"a", 11, &5), None);
        assert_eq!(m.get(&"a", 10, &5), None);
        m.set("a", 10, 5, 1000);
        assert_eq!(m.get(&"a", 10, &6), None);
        assert_eq!(m.get(&"a", 10, &5), None);
        m.set("a", 10, 5, 1000);
        m.set("a", 10, 5, 2000);
        assert_eq!(m.get(&"a", 10, &5), Some(2000));
        m.forget(&"a");
        assert_eq!(m.get(&"a", 10, &5), None);
    }

    #[test]
    fn p5z_id_is_stable() {
        let (h, f) = ([1u8; 64], [2u8; 32]);
        let id = p5z_id(&h, &f, 100);
        assert_eq!(id, p5z_id(&h, &f, 100));
        assert_eq!(id.len(), 16);
        assert!(id.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)));
        let mut f2 = f;
        f2[31] ^= 1;
        assert_ne!(id, p5z_id(&h, &f2, 100));
        assert_ne!(id, p5z_id(&h, &f, 101));
    }

    /// Every cache built from a .p5z has this id in `.done`: another byte order, order of the parts
    /// or FNV constant would make every one of them look like another file (no relink when moved).
    /// Worked out apart from this code (FNV-1a 64 of the size as 8 bytes LE, the header, the footer).
    #[test]
    fn p5z_id_golden() {
        let head: [u8; 64] = std::array::from_fn(|i| i as u8);
        let foot: [u8; 32] = std::array::from_fn(|i| 0xff - i as u8);
        assert_eq!(p5z_id(&head, &foot, 2_094_371), "5d19aa60a1471b2a");
    }

    #[test]
    fn space_short_table() {
        const GIB: u64 = 1 << 30;
        assert_eq!(space_short(10 * GIB, GIB), None);
        assert_eq!(space_short(GIB, GIB), Some(256 << 20));
        assert_eq!(space_short(0, 0), Some(256 << 20));
        assert_eq!(space_short(GIB + (256 << 20), GIB), None);
        assert_eq!(space_short(0, u64::MAX), Some(u64::MAX));
    }
}
