# MaxPack design

A Rust PS5 ELF payload (`src/`). It finds games on the internal SSD and on USB/extended drives,
keeps a per-game cache on the internal SSD, registers each game on the home screen and, at
launch, overlays the game's fakelib on the sandbox's system libs (the shared ampr-emu-ng dir's files are copied
into that fakelib when the cache is built; the dir itself is never mounted).
The ampr-emu-ng runtime is NOT built here: our own build of it (DeLFuS77/ampr-emu-ng: quer3q's v0.5.1, with its pkg mode, plus our p5z mode,
which also plays a `.p5z` that grew; now the release `v0.5.1-maxpack.2`, whose release, commit and CI run are in
`vendor/ampr-emu-ng/SOURCE`) is bundled from `vendor/ampr-emu-ng/` (`VERSION`, which ends in
`-maxpack.N`; `SHA256SUMS` copied from that release, checked by `ps5/build.sh`) and installed into `/data/maxpack/ampr/` at start.
Two third-party emulators are bundled the same way (released files, `VERSION` and `SHA256SUMS` from
their GitHub releases, checked by `ps5/build.sh`; GPL-3.0, `THIRD_PARTY_NOTICES`): drakmor's dlc_emu
(`vendor/dlc_emu/`: `libSceAppContent.sprx`, `libSceNpEntitlementAccess.sprx`, `libSceGameUpdate.sprx`)
and pgo_stub (`vendor/pgo_stub/`: `libScePlayGo.sprx`). They are embedded in the payload and written
only into a cache's `fakelib/`, never into `/data/maxpack/ampr/`.
`crates/kraken-mount-pkg` reads debug FPKGs (the reader plus its Kraken block decoder, module `decoder`).

## Supported sources (nothing else)
- **FPKG**: a dump-forge debug `.pkg` file directly in a scan root.
  Sparse layout extents (holes: a kind-0x40 boundary up to the next boundary) are read as zeros.
- **Folder**: a directory directly in a scan root with `eboot.bin` and `sce_sys/param.json`.
  Packing: `ampr_zstd.index` present = zstd, else `ampr_assets.index` = lz4, else plain.
- **.p5z**: a MaxPack game file (`maxpack pack --p5z`) directly in a scan root, or one level below like the other
  sources (`scan::candidates`): `Kind::P5z`, played without installing it. The engine (`mp_play_*`, `src/p5z.rs`)
  validates it and describes it (title id, name, version, loose files, volumes); the scan takes the title from that
  description. The game itself stays inside the `.p5z`: only its loose files go to the cache (see Cache). A `.p5z`
  the engine refuses (cut short, damaged, more than 256 volumes) is not a game: it is listed apart
  (`ScanResult::unplayable`, `/api/state.unplayable`, one toast per path and run). A "damaged" verdict is believed
  only the second time (`maxpack_logic::p5z::refusal`): a read error during the open (a USB drive that resets, the
  engine out of memory) looks the same, so the first one for a (path, size, mtime) is a scan error that is tried
  again next pass (`cannot be read now, tried again next pass: <reason>`; not memoized, not listed, no toast), and
  only the same verdict in the next pass for the same size and mtime is memoized, listed and toasted. More than 256
  volumes comes from a description that read fine: believed at once. Meanwhile it holds its title (Flow, Stability),
  and the engine's open in a build follows the same rule (Cache). A `.p5z` still being copied is skipped like a
  pkg (`stability_wait_seconds`).
  A `.p5z` that grew on the PC (`maxpack add --dlc`, `maxpack update`: index version 2, appended to, never rewritten)
  is a game like any other: the engine's description adds `"index_version":2,"grown":N` (absent for one that never
  grew; `maxpack_logic::p5z::PlayInfo::grew`; a version other than 1 or 2, or a count that does not go with it, is
  damaged) and still lists every volume, 0..n-1, the old ones too, with no chunk left in them or with room no chunk
  uses. It plays only with an ampr-emu-ng that knows such a file (`maxpack_logic::p5z::GROWN_NG`,
  `v0.5.1-maxpack.2`; Cache: `ng_gate`). While a journal (`<name>.p5z.maxpack-append`) says an append on the PC was
  cut short, the engine reads the file as it was before it (or as it grew, when that append had ended before its
  journal went); the console never cuts it nor deletes the journal (recovery is the PC's).
- Any drive. The cache is always on the internal SSD.

## Paths
| Path | What |
|---|---|
| `/data/maxpack/config.toml` | settings (created on first start: the defaults, plus the scan roots the other mounters had; see Settings) |
| `/data/maxpack/caches/<TID>/` | per-game cache |
| `/data/maxpack/ampr.bundled` | `<version> <len> <crc32>` of the `libSceAmpr.sprx` we last wrote, bundled or downloaded (v0.4.0 builds wrote `<len> <crc32>`: ours, version unknown; outside `ampr/`, whose files all go into fakelib) |
| `/data/maxpack/ampr/` | ampr-emu-ng files (`libSceAmpr.sprx`, ...), copied on top of every game's `fakelib/` in its cache (chmod 0777 at start; a `libkernel.sprx` there is never copied and logs a warning) |
| `/data/maxpack/maxpack.log` | log (setting `log`); each line starts with the UTC time and the milliseconds since boot (`2026-11-19 20:51:03Z [10643021] `); starts over past 1 MiB, no kept copy |
| `/data/maxpack/maxpack.elf` | self copy the tile starts (Dump Forge style, below) |
| `/data/maxpack/crashes.json` | the last crash of each title (see Crash report); not in a cache, which a pkg or .p5z game sees as `/app0` |
| `/data/maxpack/recommended.json` | the last downloaded list of recommended settings whose `version` was higher than the one in use (see Recommended settings); written tmp + rename |

## Drives and scan roots (`src/storage.rs`)
Drives: `/data` (label "Internal") always, plus each of `/mnt/usb0..7` ("USB n+1") and
`/mnt/ext0..1` ("Extended n") that is a mount point (statfs `f_mntonname == path`).
Default scan roots: SMP `SM_DEFAULT_SCAN_PATHS_INITIALIZER` order, only for drives present
(`/data/homebrew`, `/data/etaHEN/games`, ext `homebrew`/`etaHEN/games`, usb `homebrew`, usb `etaHEN/games`,
usb roots, ext roots). `scanpath=` lines in config replace them.
A dir named `backports` in a scan root is never a game.
Never a root and never a source (`maxpack_logic::roots::never_root` / `never_source`; `.`, `..` and `//` resolved as
text): `/` (as a root), `/system*`, `/user` (but `/user/data` goes by the `/data` rules), `/mnt/sandbox`,
`/mnt/extN/user`, `/mnt/extN/ps5/user`, `/data/maxpack`, `/data/krakenmount` and `/data/shadowmount`. Applied to
the roots imported at the first start, to `POST /api/settings`, to the roots of each pass (`main::usable_roots`,
for a hand-edited `config.toml`; each one logged once) and to every candidate source (`scan::scan`).
Depth (`scan::candidates`, SMP scan_depth=2): a root's entries are scanned, and also the entries of each subfolder that is
not a game folder, not `backports` (any case: exFAT), not a dot entry and not itself a scan root (one level only, dot
entries and `backports` skipped). A drive root (`/mnt/<x>`) is never descended: its `homebrew` and `etaHEN/games` are
roots of their own. Every root's own entries come first, in root order, then all nested ones; a nested source never
beats a title already held (`claim`), so an old copy in a subfolder cannot take over. A path already listed in this
pass is not listed again.
Title name (`scan::parse_param`, as Dump Forge's gameTitle): the `localizedParameters` `titleName` in the
console language (`i18n::title_tag`: `SCE_SYSTEM_PARAM_LANG_*` -> its param.json tag, all 31 languages,
en-US when unreadable), else the same language in another region (es-419 -> es-ES), else the default
language's, else en-US, else any, else the title id; blank names skipped. Read at each parse, so a language
change shows on the next scan (an unbuilt pkg's memoized read keeps its first language until it changes).
It is display only: UI, sort order, toasts, logs.
Eject (`POST /api/eject`): `/mnt/usb0..7` and `/mnt/ext0` are ejectable (`/mnt/ext1` is the M.2 slot,
SMP `sm_usb_mount_path`). The request goes to the system automounter over IPMI to `SceAutoMounterIpc`
in `ps5/usb.c` (`km_usb_eject`, a port of SMP `sm_usb.c`), which checks the mount still has the
source and fsid seen at request time; the payload never unmounts the drive itself.

## Settings (`src/config.rs`, file `/data/maxpack/config.toml`)
A flat TOML subset: `key = value` with strings, `true`/`false`, integers and arrays of strings; `#` comments.
| Key | Default | Meaning |
|---|---|---|
| `scanpath` | (defaults above) | array of paths; replaces the default scan roots. First start only (no `config.toml`): the roots added by hand in the other mounters are imported, read only: `scanpath` of `/data/krakenmount/config.toml` (the last line wins), `[scan] path` of `/data/shadowmount/config.ini`, and the parent folder of each `/data/shadowmount/manual.lst` entry (`maxpack_logic::roots`). With any, `scanpath` lists every default root first, for every drive whether present or not, then theirs; with none there is no `scanpath` line. A root that may never be one (Drives and scan roots: the system, Sony's user data, MaxPack's and the other mounters' data folders) is never imported nor scanned, and the web UI cannot save it |
| `exclude` | [] | array of absolute paths the scan never opens (`maxpack_logic::exclude`: no control character; `//`, `.` and `..` resolved as text, trailing `/` trimmed, duplicates dropped): a source at an entry or under it (whole path components, `scan::excluded`) is skipped by the scan before duplicate detection, never listed or descended into (an excluded root is not even listed): not in games, duplicates, errors or unstable, never built or registered. A title still registered stays on the home screen (as ShadowMountPlus removing a manual source) with its built cache, whose overlay its launch still gets, and auto-remove leaves it too, since its source is still there; but it stays playable only until a reboot (or, for a source on USB, a replug): `register::mount` redoes the `/system_ex/app/<TID>` nullfs only for scanned titles, so after that its tile has no `/app0`. A purge (Settings) deletes its cache and unmounts its `/system_ex/app/<TID>`, since only the scanned titles are kept; for a pkg or `.p5z` that cache is its `/app0`, so the tile no longer starts. Take a game off the home screen before excluding it (Delete, or the installer's Uninstall), or take the path off `exclude` to have it back. Edited in the settings form ("Excluded paths"); a source excluded while a pass runs is skipped before its build or register. Ported from the upstream project's 0.3.2 |
| `inject_ampr` | true | copy `/data/maxpack/ampr/*` on top of a folder game's fakelib; pkg games always get it (pkg mode exists only in ampr-emu-ng) (a change only refreshes `fakelib/`) |
| `add_emulators` | true | add the bundled DLC (dlc_emu) and PlayGo (pgo_stub) emulators to a game's fakelib when it needs them and ships none (a change only refreshes `fakelib/`) |
| `playgo_off` | [] | array of title ids (`playgo_off = ["PPSA10001"]`) that get no bundled PlayGo stub: pgo_stub is neither added nor upgraded, a `libScePlayGo.sprx` the game or backport ships stays as shipped; the dlc_emu set is unaffected. Per game from the UI (`POST /api/playgo/<TID>`); a change only refreshes `fakelib/` |
| `hidden` | [] | array of title ids the page's library does not show unless its "show hidden" switch is on (a per-browser choice). Page only: a hidden game is still built, registered, mounted and on the home screen, and `/api/state` still lists it (`hidden: true`). Per game from the game sheet (`POST /api/hidden/<TID>`); the settings form keeps the list |
| `overlay_off` | [] | array of title ids whose cache gets an empty `fakelib/`, so the launch watcher mounts nothing in their sandbox: no overlay (not the game's or backport's fakelib, not ampr-emu-ng, not the emulators), no `/data`, USB, fonts or writable `/app0` either; the title runs as registered (for a game that must not get MaxPack's libraries, such as a Playden title shell). Plain folder games only (`maxpack_logic::overlay`): a pkg is read through ampr-emu-ng's pkg mode, a `.p5z` only through our ampr-emu-ng's p5z mode, and a zstd or LZ4 folder's packs only through ampr-emu-ng, all of which come in that overlay; so the web refuses it for them (409) and a listed title whose library copy is one of those (packed since, or a pkg or `.p5z` copy took over) keeps its fakelib. Per game from the game sheet (`POST /api/overlay/<TID>`); the settings form keeps the list; a change only refreshes `fakelib/`. Ported from the upstream project's 0.3.2, with the `.p5z` and packed folder refusals ours |
| `fakelib_emulators` | [] | array of title ids whose fakelib mode is `emulators`: their cache's `fakelib/` gets nothing from the game's or the backport's lib dir, only the bundled emulators they need (`add_emulators`, `playgo_off` as usual) and ampr-emu-ng (see Cache). For a game that shows a black screen with the libraries its dump or backport ships. Per game from the game sheet (`POST /api/fakelib/<TID>`); the settings form keeps the list; a change only refreshes `fakelib/` |
| `fakelib_off` | [] | array of title ids whose fakelib mode is `off`: their `fakelib/` gets only ampr-emu-ng, and that only for a pkg, a `.p5z` or a zstd or LZ4 folder (a plain folder gets an empty `fakelib/`, so MaxPack mounts nothing at its launch, as with `overlay_off`: no overlay, no `/data`, USB, fonts or writable `/app0` in its sandbox). For a game that still stalls with the emulators. A title is in one of the two lists at most (`maxpack_logic::fakelib::set_mode`); one listed in both by hand is `off` and the next save drops it from `fakelib_emulators` (`fakelib::exclusive`, logged). In neither: `full`, the composition as it always was. Like `fakelib_emulators` otherwise |
| `manual_settings` | [] | array of title ids whose Fakelib mode and PlayGo stub the user chose on the game sheet: the recommended settings do not apply to them (see Recommended settings). A title in `fakelib_emulators`, `fakelib_off` or `playgo_off` counts as chosen too, so a console upgraded from 1.0.1 keeps every choice it had. Added by `POST /api/fakelib/<TID>` and `POST /api/playgo/<TID>`, cleared with the three others by `POST /api/recommended/<TID>`; the settings form keeps the list |
| `ampr_auto_update` | false | once per start, download a newer ampr-emu-ng release from GitHub, installed only when it matches the release's `SHA256SUMS` and once the secure-connection self-test has passed (a hand-placed `libSceAmpr.sprx` is kept; see Auto-update in Flow) |
| `recommended_update` | true | once per start, download the list of recommended settings from this repository once the secure-connection self-test has passed (see Recommended settings); off: the copy in use stays (the bundled one, or the last download) |
| `backports` | true | apply `<scan root>/backports/<TID>*` (the game's own root, the other roots, then `/data/homebrew`: `maxpack_logic::exclude::backport_dirs`; an excluded `backports` folder is never opened and an excluded backport never used) |
| `scan_interval_seconds` | 10 | scan loop period (1..3600) |
| `stability_wait_seconds` | 10 | skip a source changed less than this many seconds ago (still being copied) (0..3600, 0 = off) |
| `low_space_gb` | 20 | warn when `/data` free space is below this |
| `auto_remove_missing` | true | remove a title from the home screen when its source stays missing for `auto_remove_delay_seconds` (the cache stays; see Flow) |
| `auto_remove_delay_seconds` | 300 | how long a source must stay missing before `auto_remove_missing` removes its title (1..86400) |
| `notify` | true | system toasts |
| `log` | true | write `/data/maxpack/maxpack.log` (stdout always gets the lines) |
| `debug` | false | klog capture to `/data/maxpack/klog.txt` and the `/file/data/...` route |

Invalid values are logged and replaced by the default; a string item of an array (`scanpath`, `exclude`, `playgo_off`, `hidden`, `overlay_off`, `fakelib_emulators`, `fakelib_off`, `manual_settings`) that is not
a valid value is logged and skipped, the other items stay (an array that does not parse, such as one with a number in it,
is ignored as a whole). The web UI edits the same file.

## Cache (`src/cache.rs`)
`/data/maxpack/caches/<TID>/`, stamp file `.done` written last.
- Folder: `fakelib/` only. `/app0` is the game folder itself.
- .p5z (`build_p5z`): like a pkg, the cache IS `/app0`, but only the loose files of the `.p5z` are extracted (format
  roles 0, 2 and 3; never the volumes (1), the runtime (4) or the originals (5)), 0777. `maxpack_logic::p5z::loose_dest`
  skips an unsafe path, a nested lib dir and any path whose first component, in any case and at any depth (`.done/x`
  too), is a name MaxPack keeps in a cache (counted in one log line); a file straight inside `fakelib/` or `fakelib2/`
  goes to the game's lib dir of the fakelib composition. Those names (`.done`, `.built`, `.ampr`, `kraken.image`,
  `kraken`, `kraken_zero.dat`, `maxpack.image`, `p5z`, `fakelib`, `fakelib2`) have one source, `maxpack_logic::cache`
  (`RESERVED`, which every name there is in by construction), where `cache.rs`, `ampr_index.rs` and `launch.rs` take
  them from; a test checks each of them is refused. The same rule keeps a backport's files off them. The volumes stay in the
  `.p5z`: ampr-emu-ng reads them through `maxpack.image`, and the `.p5z` ships its own `ampr_zstd.index` and
  `ampr_emu.index` among the loose files, so MaxPack writes no index. The cache also has `fakelib/`, `p5z/` (an empty
  mount point, see Launch) and `maxpack.image`: UTF-8, `\n` lines, no `\r`: `maxpack-image 1`, then `p5z/<file name>`
  (the `.p5z` relative to `/app0`), then `<id> <offset> <length>` per volume (ids 0..n-1 in order, n from 1 to 256;
  `maxpack_logic::p5z::image_text`). The file name is the directory's, in UTF-8 (MaxPack's PC app names a `.p5z`
  `<Title> [<TITLE_ID>].p5z` with every character of the title: ™, ’, accents, Japanese), and ampr-emu-ng opens the
  `.p5z` with those same bytes. It must not be empty, `.` or `..`, nor hold a byte below 0x20, 0x7F, `/` or `\` (the
  contract, ampr-emu-ng's `docs/p5z-mode.md`; both sides test the same golden text, `"maxpack-image 1\np5z/Starfall’s
  Odyssey™ 2 [PPSA01234].p5z\n0 1234 5678\n"`): a name that is not UTF-8 or breaks those rules, or a volume list
  outside the contract, fails the build with that reason. Space (`maxpack_logic::p5z::space_short`): before
  extracting anything, the loose files plus `SPACE_MARGIN` (256 MiB, the installer's `space_margin`) must fit in the
  free space of `/data`, else the build stops with `cache::NoSpace { missing }` (`cache::missing_bytes`). `main.rs` does
  not memoize it (no `failed` entry) and tries again every pass, so the game builds by itself once the space is there:
  the game shows status `error` with `error_code` `no_space` and `missing_bytes`, and one toast (`i18n::no_space`) and
  one log line per run say how much is missing. Meanwhile the loose bytes measured are kept for that (path, size,
  mtime) (`maxpack_logic::p5z::NeedMemo`): a retry measures only the free space, and the `.p5z` (maybe on USB) is
  opened and validated again only once it fits; any other size or mtime drops the memo.
  The engine's open in a build or a fakelib refresh follows the scan's rule (`cache::open_p5z`,
  `maxpack_logic::p5z::BuildRefusals`): a read error, or a first "damaged" verdict for a (path, size, mtime), stops it
  with `cache::TryAgain` (`cannot be read now, tried again next pass: <reason>`), which `main.rs` does not memoize either
  (no `failed` entry, no toast, status `error`; logged when its message changes); only the same verdict at the next try
  for the same size and mtime (a read error or a good open in between starts over), or more than 256 volumes, fails the
  build as usual, memoized until the inputs change. The extracted `sce_sys/param.json` must carry the title id the
  `.p5z`'s description gave, else the build fails. The stamp's `kind` is `p5z` and `.done` carries `p5z_id` (FNV-1a 64
  of the size as 8 bytes little-endian, the 64-byte header and the 32-byte footer, `p5z::id`; the identity `is_move`
  compares; a golden value is tested, so a change of it, which would make no cache relink when its `.p5z` moves, fails).
  A `.p5z` that grew (`PlayInfo::grew`) is built only while the ampr-emu-ng it gets on top of its `fakelib/`, the
  `libSceAmpr.sprx` of `/data/maxpack/ampr/` (always, whatever the game or backport ships), plays it
  (`cache::ng_gate`, `maxpack_logic::p5z::ng_plays`: `v0.5.1-maxpack.2` or later). Its version is the one MaxPack
  knows (`main::ampr_version_of`, `maxpack_logic::ampr::known_version`): the bundled `VERSION` for the bundled
  bytes, the version `ampr.bundled` records for exactly those bytes (len and crc32: a downloaded release); a file
  put there by hand, bytes the mark does not describe, a v0.4.0 mark, none or a read error is unknown, and unknown
  never plays a grown `.p5z`. That file is not always the bundled one: a hand-placed file is kept, and an update
  could put another there (it never installs an older release: Flow, Auto-update). Otherwise the build stops right
  after the engine's open, before anything is extracted, with a `cache::OldNg` (`this .p5z grew (index version 2) and
  needs ampr-emu-ng v0.5.1-maxpack.2 or later; the ampr-emu-ng in use is <v> (...): not started with it`): a failed
  build, so the cache has no game files and neither Play nor the tile can start the game half-way (an older
  ampr-emu-ng refuses the grown index at the game's start and serves none of its packed files). `main.rs` shows status
  `error` with `error_code` `old_ng`, toasts `i18n::old_ng` once and memoizes it (the games a pass stops so are toasted
  together after its last game: one `i18n::old_ng_many` with their count when more than one); `inputs_key` holds the ampr dir's
  state (`cache::ampr_state`) and the mark's text (`main::ampr_mark`), so another `libSceAmpr.sprx`, or a mark
  written after it, tries the build again (a restart too: the bundled copy replaces an older one of ours at start).
  The check before extraction only spares the work: the one that counts is `compose_fakelib`'s, on the copy that
  `fakelib/` got, which is what the game loads, made after the state `check` compares was read. A file swapped in
  the ampr dir during a long build is therefore either the refused copy (an OldNg, nothing recorded) or one that
  differs from the record (the next pass refreshes and checks it). A fakelib refresh of a built cache of a grown
  `.p5z` (the ampr dir changed) whose copy is refused does not keep that `fakelib/`: it removes `.done` and rebuilds,
  which clears the cache and stops the same way (so a rebuild that fails before clearing, an I/O error, leaves no
  cache that says it is built). A `.p5z` that
  never grew is not looked at (an older ampr-emu-ng plays it as before). A grown `.p5z`'s `maxpack.image` has a line
  for every volume, as the description lists them: a volume with no chunk, or with room no chunk uses, keeps its
  region (ampr-emu-ng checks each chunk against its own record).
- FPKG: it IS `/app0`: `sce_sys/**` (with the files only the pkg container holds: `save_data.png`,
  `pic0`/`pic1`, `snd0.at9`, `trophy00.ucp`, `nptitle.dat`, `npbind.dat`, which save data and
  trophies read from `/app0/sce_sys`), `eboot.bin`, every `*.prx`/`*.sprx`, every `*.elf` outside `decrypted/`
  (SceSysCore loads an exit-spawned ELF, which may sit in a subfolder, from disk, never through ng), and a root-level `dlc_emu.ini` / `playgo_stub.dat` (the emulators
  open `/app0/<name>`; an older cache lacks them loose, ng still serves them from the pkg) extracted (0777),
  `fakelib/`, `kraken/` (mount point for the pkg's folder, see Launch), `kraken.image` (one line, no
  newline: `kraken/<pkg file name>`, relative to `/app0`, the file ng opens; missing means stale), `ampr_emu.index` +
  `ampr_assets.index` (`src/ampr_index.rs`; every non-extracted file packed, fakelib and
  backport files loose). Backport files (except its fakelib) are copied loose over pkg files.
  A file with sparse holes stays packed: each hole block is a RAW chunk of pack 1,
  `kraken_zero.dat` (an AMPRDAT3 volume in the cache: one 64 KiB header page + 256 KiB zeros,
  written only when a hole is packed; ng's pkg mode opens packs 1 and up as AMPRDAT3 volumes,
  `ampr-sys` `rt/pack/pkg.rs` `build_pack_path_pkg`). Any file `ampr_index::packable` refuses is
  extracted loose instead: holes that do not start and end on its 256 KiB block grid (logged per
  file with the title id and path), or a short compressed block that decodes past the file's end
  (ps5-dump-forge packs several small files into one block), or data starting inside such a shared
  block (`Pkg::blocks` refuses it; `read_at` decodes it); one line per title: count and bytes.
  The dir itself is never deleted: `/system_ex/app/<TID>` is a nullfs of it.
- `/app0` is writable, so a game may write into its cache (pkg) or its dump folder (folder, also on
  USB). `.built` (written before `.done`) lists what the build created; a rebuild removes only those
  paths (a dir only once empty), so files the game wrote stay. A failed build removes what it wrote and leaves a `.built`
  that lists only what it could not remove (normally empty) and no `.done`, so the next build (for a `.p5z` short of
  space, the next pass) never deletes files the game wrote into its cache.
  A cache without `.built` (older build) is emptied. Only Purge deletes the game's files.
- `fakelib/` composition: base = backport `fakelib/` (all files) if present, else the game's
  `fakelib/` (all files but `libSceSaveData.native.sprx`, which breaks saves; the old `game_fakelib`
  setting is gone and ignored in `config.toml`); `libkernel.sprx` always dropped. A backport's lib dir
  keeps `libSceSaveData.native.sprx` (a backport is deliberate). `fakelib2/` (SMP) wins
  over `fakelib/` and is used as-is (it keeps `libSceSaveData.native.sprx` too); it is never indexed or copied as game files. All files 0777.
  Then, with `add_emulators`, each bundled emulator the game needs is added when `fakelib/` has no file of
  that name (case-insensitive). A file of that name that is a known older release (dlc_emu 0.1a or 0.2, pgo_stub 0.4
  or 0.5, matched by size and crc32, both builds of each) is replaced by the bundled one, whether or not the game
  needs it, logged `<TID>: fakelib: upgraded <name> (<project> <version>)`; any other copy (custom, the bundled
  release, a log build) is kept, so the game's or backport's own copy wins and nothing is downgraded: the dlc_emu set
  (its 3 files, each only if missing) when `dlc_emu.ini` (exact name, as dlc_emu opens it) is at the folder game's root, the backport's root or
  the pkg's root; `libScePlayGo.sprx` when `eboot.bin` (the backport's if it has one, else the game's; pkg:
  the extracted copy) contains the string `libScePlayGo` (an import; an encrypted eboot or an import only in
  another prx is missed). Each add logs `<TID>: fakelib: added <name> (<project> <version>)`.
  The dlc_emu set is all-or-nothing: when any of its 3 files is a copy that is neither a known older release nor the bundled file, none of the 3
  is added or upgraded (no mixed set), logged `<TID>: fakelib: kept the game's dlc_emu files (not a known release)`; pgo_stub goes per file.
  A title in `playgo_off` skips pgo_stub entirely (no add, no upgrade, the eboot is not searched), logged
  `<TID>: fakelib: PlayGo stub off (playgo_off or the recommended settings)`; `fakelib/` starts from scratch, so a stub added earlier is gone.
  Here and below, `playgo_off` and the fakelib mode are the effective ones (`recommended::effective`): the user's
  choice, else the recommended settings' (see Recommended settings). A composition reads them once
  (`cache::effective`) for what it composes and for the `.ampr` it writes, so a list put in use meanwhile is never
  recorded as composed: the next `check` sees it and refreshes again.
  A plain folder title in `overlay_off` (`cache::overlay_off`) stops right after `fakelib/` is emptied (no base, no
  emulators, no ampr), logged `<TID>: fakelib: overlay off (overlay_off)`; `.ampr` is written with `"overlay_off": true`
  and the earlier "playgo" memo kept. Launch then mounts nothing (empty `fakelib/`).
  The fakelib mode (`fakelib_emulators`, `fakelib_off`; `cache::fakelib_mode`) decides which of these steps run
  (`maxpack_logic::fakelib::sources`, from the mode, the kind and packing, `add_emulators`, `inject_ampr` and
  `playgo_off`): `full` (a title in neither list) runs them all, as this list says. `emulators` skips the base (nothing from the
  backport's or the game's lib dir; the backport's other files are copied or overlaid as always), runs the emulators step
  (same rules; `playgo_off` still applies) and gets ampr-emu-ng as `full` does, and also without `inject_ampr` for a zstd
  or LZ4 folder, whose own `libSceAmpr.sprx` read its packs and is no longer there. `off` skips the base and the
  emulators and gets ampr-emu-ng only for a game that cannot run without it (`fakelib::needs_ng`: a pkg, a `.p5z`, a
  zstd or LZ4 folder), whatever `inject_ampr` says; a plain folder gets an empty `fakelib/` (launch then mounts
  nothing, as for `overlay_off`: no overlay, no `/data`, USB, fonts or writable `/app0` in the sandbox). Each composition in a mode other than `full` logs one line:
  `<TID>: fakelib: mode emulators (the game's and backport's fakelib not used)`, `<TID>: fakelib: mode off (only
  ampr-emu-ng)` or `<TID>: fakelib: mode off (empty)`, with `, recommended setting` after it when the list itself
  sets the mode (`Effective::fakelib_recommended`). `overlay_off` comes first: such a title stops at the empty
  `fakelib/` whatever its mode.
  Then, with `inject_ampr` and always for a pkg game (the package's own `libSceAmpr.sprx` cannot read the
  `.pkg` named in `kraken.image`) or a .p5z game (a game's own ng has no p5z mode, so `keep_game_ng` does not apply:
  ours is always on top), every file of `/data/maxpack/ampr/` (not `libkernel.sprx`) is copied on top,
  so ours wins over the game's own `libSceAmpr.sprx`, except a game's or backport's ampr-emu-ng strictly newer than
  ours (the `ampr-emu-ng X.Y.Z` identity in both files; ours without one counts as older), which is kept and logged
  `<TID>: fakelib: kept the game's ampr-emu-ng <v> (newer than <v>)` (no ng downgrade, also over a hand-placed copy). `.ampr` records what fakelib was composed from: `{"ampr": {name: [size, mtime]} or null,
  "emus": null without `add_emulators` or in the fakelib mode `off`, else the dlc_emu and pgo_stub versions, whether a `dlc_emu.ini` is on
  disk (folder or backport root), the eboot's [size, mtime] and "playgo_off" (the title is in `playgo_off`), "playgo": [eboot size, mtime secs, imports PlayGo]}`
  (a pkg's own `dlc_emu.ini` is not in it, a pkg change rebuilds through the stamp). "playgo" memoizes the PlayGo
  import search: `eboot.bin` is read again only when its (size, mtime) differs (a read error is not memoized), and
  `check` never compares it. An older `.ampr` without `emus` differs, so every cache refreshes `fakelib/` once; that
  one-time refresh also drops a `libSceSaveData.native.sprx` an older build copied (no separate check for such files).
  `"overlay_off": true` is in `.ampr` only for a title `cache::overlay_off` takes (an older `.ampr` stays fresh).
  `"fakelib_mode": "emulators"|"off"` is in `.ampr` only for a mode other than `full` (`fakelib::record`); a `.ampr`
  without it (every cache built before the setting, and every `full` one) reads as `full` (`fakelib::recorded`), so the
  upgrade refreshes no cache. A new list of recommended settings that changes a game's effective mode or PlayGo stub
  makes the recorded state differ in the same way, so that game's `fakelib/` is recomposed at the next pass.
  When "ampr", "emus" or "overlay_off" differ, or the recorded fakelib mode does (the ampr dir, `inject_ampr`, the emulator inputs, `overlay_off` or the mode changed), or `.done` still has a
  `game_fakelib` other than `full` (a cache built before the setting was removed), `check` says `Fakelib` and only `fakelib/` is
  recomposed (`refresh_fakelib`, which then rewrites `.done` with its own mtime and its inputs record), no extraction or re-registration. `fakelib/` is not in the AMPR index.
- A folder game's cache is only `fakelib/`, `.ampr`, `.built`, `.done`: nothing of the game is copied.
- Fresh when `.done` exists, its JSON stamp equals the expected one
  (`{"v":4,"kind","source","size","backport","fakelib"}`; extra keys of an older stamp are ignored), and every input
  has the size and mtime `.done` recorded (`"inputs": [{"path","size","mtime_ns"}]`, taken before the build reads
  them, so a change during the build differs), with none added or removed (`maxpack_logic::fresh::changed`). Equality
  only, never against the clock or `.done`'s mtime: a jailbroken console is often offline with its clock years
  behind (2012 on a real one), and a source written on a PC has a "future" mtime that must not rebuild every pass.
  Inputs: FPKG = the .pkg, backport dir (recursive);
  folder = `eboot.bin`, `sce_sys/param.json`, its lib dir and files (not the folder itself: the game
  writes there); .p5z = the file and the backport, and `maxpack.image` must exist. Otherwise rebuilt: a different `.p5z` of the same
  title (another version: another size, header or footer) rebuilds the cache. A pkg's `.done` also carries `pkg_id` (FNV-1a 64 of the size,
  the first 64 KiB, the 4 KiB CNT header and the last 64 KiB), a folder's `folder_id` (FNV-1a 64 of
  each input file's path relative to the folder and size, plus the bytes of every input but
  `eboot.bin`: `param.json` and the lib dir's files); neither is
  compared by the freshness check.
- `"v"` is the cache layout. Another value is Stale (`cache layout v1 -> v2`), checked before Moved,
  so every older cache rebuilds once, moved or not. v2: the container-only `sce_sys` files and the
  extraction of every file the index cannot map. v3: `*.elf` outside `decrypted/`. v4: the recorded inputs.
- `scan::cached_param` trusts a cache's `sce_sys/param.json` (the `.pkg` or `.p5z` is not opened) when that
  cache's `.done` recorded the file at its path with its current size and mtime (`fresh::recorded_of`).
- Moved (`Fresh::Moved(old source)`, `relink`): the stamp differs only in "source" (and "fakelib"
  when it is the folder's own lib dir under the moved path), the old source path is gone, and the
  files are the same: pkg = `pkg_id` equal (the moved .pkg has another path and may have another mtime), its other
  inputs (the backport) as `.done` recorded them; folder = `folder_id` equal (mtimes not compared: a copy-then-delete
  move relinks too); .p5z = `p5z_id` equal, its other inputs as recorded. A copy while the old one
  still exists is a duplicate, never a move. Relink (`.done` mtime now and the inputs recorded as they are now, the
  moved source at its new path): pkg = new stamp, `kraken.image` rewritten when
  the file name changed, extracted files and indexes kept (ng checks the pkg's
  size only); folder = new stamp; .p5z = new stamp and `maxpack.image` rewritten (the `.p5z` is opened for its
  volumes), the extracted files kept: nothing is extracted again, and the log says `moved ... relinking`. Anything else is Stale (a cache stamped before `pkg_id`/`folder_id`
  existed rebuilds once when its game moves).
- Access (`fix_access`, once per run per title from `main.rs`; never fails): games and system
  services (save data, trophies) read the game's files with another uid than the payload. It only
  adds bits, chmods only when some are missing, lstats and never follows symlinks, skips entries
  that are neither file nor dir: the cache dir recursively (dirs 0777, `/app0` is writable; files
  +0444); pkg = the .pkg +0444 and its folder +0555 (the `/app0/kraken` nullfs: ng falls back to
  LZ4 mode when it cannot open the .pkg), a .p5z the same with its folder (the `/app0/p5z` nullfs); folder = the game folder recursively (dirs +0555, files
  +0444). chmod failures (exFAT USB, read-only) are counted: one log line per title with the title
  id, modes fixed, failures and the first failed path and error (none when nothing changed).
- Purge (settings): delete caches of titles not in the library (`register::unmount_title` first;
  a dir whose unmount fails is skipped and logged), empty and rebuild the rest. It stops before the
  next title once MaxPack must stop mounting (another mounter started, or kstuff went away) and goes on at the next pass
  that mounts.

## Flow (`src/main.rs`)
Start: `umask(0)` first, so every dir/file the payload creates is 0777/0666 (as SMP, which mkdirs
0777) whatever elfldr passes; then create dirs, load config, take port 8095 (asks an older copy to quit), install the bundled
`libSceAmpr.sprx` when `ampr/libSceAmpr.sprx` is missing or matches `ampr.bundled` (an older bundled
copy; a file put there by hand is kept; see Auto-update below), ampr chmod, the self copy and the self-update look
(Self-update below) on their own threads, web thread, klog thread when `debug`, then wait
(2 s poll, one toast, `waiting` in `/api/state` for the UI banner) until kstuff answers its KEKCALL_CHECK probe (`sys::kstuff_loaded`,
SMP `sm_kstuff_is_loaded`), then the mounter thread and the launch watcher thread.
The installer (`src/installer.rs`) starts on its own thread right after the port is taken, whatever the
mounter state (see Installer below). The home-screen tile registers once, after that wait, and never
while another mounter runs (`main::register_tile_when_mounting`: on FW 12+ the tile goes in with
InstallAll, which installs whatever is staged in `/user/app`, the other mounter's titles too); it is
registered once that mounter stops, and meanwhile the page opens at `http://<console IP>:8095`.
Another mounter: kstuff, then one walk of the process list for a `shadowmountplus.elf` or `kraken-mount.elf` process, is looked at every
2 s (`watch_mounter`, which wakes the loop on a change) and at each loop turn, through `maxpack_logic::mounter::Debounced` into
`State::mounter`: a mounter found counts at once; it counts as gone only after two clean looks at least 2 s apart, and a walk of the
process list cut short (`launch::running_names` error: sysctl failed, a bad record) still counts a mounter seen before it
(`maxpack_logic::mounter::walk_look`); only an empty failed walk is an Error, which changes nothing. While it is not Ready: no scan,
build, register, purge or auto-remove (both also stop before their next title), `/api/delete` and `/api/eject` answer 409, a pass in progress stops before its next title (a title built meanwhile is neither registered
nor mounted), the launch watcher mounts no new overlay (a launch first seen then never gets one; overlays already up still go on
exit as always), one toast per change, `waiting` = `"other:<process>"`; the web UI stays up (`State::installer_only`). Back to Ready:
every title is checked again (`registered` and `failed` cleared), so titles the other mounter remounted are adopted again. Loop every `scan_interval_seconds` or on
wake: reload config (under the settings lock, which the web's setting changes hold too, so a pass never puts an older
copy over a newer one), scan (sources in `exclude` skipped, see Settings; a source excluded while the pass runs is
skipped before its build or register), then for each game: freshness check, build if stale or forced,
register (once per run, and after every rebuild), status `ready`. Right before the register (or the
mount-only path below), once per run per title, `cache::fix_access` opens up the cache and the
source for the game and the system services (it only adds missing bits); a relinked moved game
gets it right after `cache::relink`. A game whose cache is fresh and
which is already registered (`register::is_registered`) is not re-registered: only `register::mount`
redoes the missing app0 nullfs (gone after a reboot); for such a pkg the scan reads title id/name from
the cache's `sce_sys/param.json` and never opens the .pkg. A fresh title registered by this run still
gets `register::mount` every scan (a no-op when app0 is mounted there; redoes it after a USB replug);
a title that leaves the scan is forgotten, so it goes through the full path when it is back.
A moved game (`Fresh::Moved`, not forced): a folder's `/system_ex/app/<TID>` stack is redone on the
new folder first (`register::remount`, backport unionfs included, never MNT_FORCE), then
`cache::relink`; logged with the title id and both paths, toast `i18n::moved`; then the usual
fresh path. A failed relink or remount falls back to a rebuild.
Stability (`stability_wait_seconds`, SMP sm_stability.c): a source whose youngest max(mtime, ctime) is
under the wait is still being copied (a pkg: the file; a folder: the folder, `sce_sys/`,
`sce_sys/param.json` and `eboot.bin` only) and is skipped this pass (`ScanResult::unstable`, logged once until stable): not gone, so its entry,
registration and cache stay, and a new one shows up once it is stable. A Purge waits until no
source is being copied (purge empties kept caches, and an unstable one would not rebuild that pass). A clock in the future counts as stable.
An unstable source reserves its title id for duplicate detection (a pkg: the title id of an earlier cache of that path; a
folder: its `param.json`, else that cache), so a copy in flight that would win keeps the other copy out (an unstable loser reserves nothing).
A `.p5z` the scan tries again next pass (it cannot be opened now, or a first "damaged" verdict) reserves the title id of
an earlier cache of that path the same way: another `.p5z` of that title cannot take it, and rebuild its cache, for that one pass.
Auto-remove (`auto_remove_missing`, SMP auto_remove_missing_games): after each full scan pass (timers
start only after the first one, so a drive mounted late is not missing; not while paused), a built cache
(`.done`) whose stamp source is gone (ENOENT/ENOTDIR only) starts a timer in memory (a restart starts over).
Excluded, timer reset: a title this scan lists (moved, or still being copied), a running one, one a
ShadowMountPlus image of this scan names as `[TID]` (`maxpack_logic::images::image_has_title`), one with DLC in
`/user/addcont/<TID>` (a stat error there protects too). Missing for `auto_remove_delay_seconds`: unregister
as `/api/delete` (each title rechecks that no game runs; unmount without force, a busy mount retried next pass;
uninstall, then `register::forget` so a returning source registers in full: `is_registered` reads app.db as bytes); the cache stays for a
return; a failed uninstall is logged once and not retried until the source returns or a restart.
A title with an installed copy (`/user/app/<TID>/app.pkg` or `/mnt/ext{0,1}/user/app/<TID>`; a stat error other than
ENOENT/ENOTDIR counts as one; `register::installed_copy`, the rule in `maxpack_logic::sony`) or no staged
`/user/app/<TID>/sce_sys/param.json` is never removed (`register::is_ours`, rechecked every pass, logged once per run).
Installed games: a source whose title id has such an installed copy is never registered or mounted, and its launch
gets no overlay (`main::process`, checked every pass before anything else; status `error` with `installed_copy`, logged
once until the install goes; also left out of `titles`). A dumped folder of a retail game that is also installed stays as
it is, and so does the installed game.
Auto-update (ampr-emu-ng, `main::ampr_update`, `maxpack_logic::ampr`): every game loads the
`libSceAmpr.sprx` it installs, so it is checked as the self-update checks `maxpack.elf` (Self-update, below:
the same `SHA256SUMS` parsing, checksum check and gate). At start, when `ampr_auto_update` is on (whatever
`inject_ampr` says: pkg games always use the ampr dir), a background thread first asks the secure-connection
self-test's record: until it has passed on this console (`update::tls_gate`, `ampr::gate`) nothing is asked
or downloaded, and one line is logged (`ampr: update: nothing installed (tls_unchecked): ...`). Then it asks
`https://api.github.com/repos/DeLFuS77/ampr-emu-ng/releases/latest` (stable releases only: betas are
pre-releases, and a draft or pre-release answer is refused). The release must have exactly one
`libSceAmpr.sprx` at `https://github.com/DeLFuS77/ampr-emu-ng/releases/download/<tag>/libSceAmpr.sprx`
(1 byte to 16 MiB; `libSceAmpr-debug.sprx` is never taken), else it is logged and nothing is installed.
When its `tag_name` is newer than the installed version, it needs the release's `SHA256SUMS` asset too
(exactly one, at the same download URL, up to 64 KiB): without it nothing is downloaded. It downloads
`SHA256SUMS`, then `libSceAmpr.sprx`, and installs it only when its size is the asset's, it starts with the
ELF or SELF magic and its SHA-256 is the one `SHA256SUMS` lists for `libSceAmpr.sprx` (that exact name, 64 hex
digits, listed once: `update::listed_sha256`). A missing, malformed or mismatching `SHA256SUMS` (or another
size or magic) installs nothing, is logged with why (`ampr: update: nothing installed (checksum): ...`) and
toasted (`i18n::ampr_refused`). A checked file is written to a temp file in `/data/maxpack`, chmod 0777,
renamed to `/data/maxpack/ampr/libSceAmpr.sprx`, recorded in `ampr.bundled` as `<tag> <len> <crc32>` and
toasted (`i18n::ampr_updated`). A copy we wrote (bundled or downloaded) is ours: the bundled copy replaces
ours only when the bundled version is newer; a hand-placed file is kept by both, and ownership is checked
again after the download. Network errors are logged and retried up to 3 times 20 s apart (not a body over its size
limit, `net::too_large`, which stays over it), then nothing
until the next start. An install wakes the scan loop (fakelib refresh); one update runs at a time.
Never a downgrade, at start or forced (`maxpack_logic::ampr::not_older`): a release older than the bundled
`VERSION` or than the version in use (`known_version`), or whose tag is not a version, is refused before anything
is downloaded or moved aside (`older`, 409: an older ampr-emu-ng stops every `.p5z` that grew, Cache), logged
`ampr: update: nothing installed (older): ...`.
Settings has "Update ampr-emu-ng now" (`POST /api/ampr/update`): forced, one attempt, installs the latest
stable release whatever is installed, with the same gate and checks; a hand-placed file is first moved to
`/data/maxpack/libSceAmpr.sprx.hand` (one backup, outside `ampr/`); a release older than the bundled one or the one in
use is refused there too (`older`, the page's `amprFail_older`). Until the secure-connection self-test
has passed, Settings says under the ampr-emu-ng row that its updates stay off (`amprNeedsTls`), and the
button's refusal shows the self-update's text (`updFail_tls_unchecked`). HTTPS goes through the console's
sceHttp/sceSsl with 4 built-in root CAs (from SMP).
Running game: while `launch::watch` reports a live session (`running`), the loop does no
scan/build/register/purge (a game starting mid-scan stops the scan before the next title);
rescan/rebuild/purge requests stay queued and run after the game exits (2 s poll).
Toasts: `sceNotificationSend` system toast (dump-forge/SMP JSON, icon `/user/data/maxpack/notify-icon.png`),
the old `sceKernelSendNotificationRequest` only as fallback. Toast texts (`src/i18n.rs`) follow the console language
(`sceSystemServiceParamGetInt` lang, read per toast; dump-forge's 11: en ru zh de es fr ja pt it ko ar), English when it
cannot be read; "MaxPack", title names and paths stay as they are. A failed game is retried
only when its inputs change (`main::inputs_key`: the stamp, the input mtimes, the ampr dir's files (`cache::ampr_state`) and `ampr.bundled`, its effective `playgo_off`,
`cache::overlay_off` and `cache::fakelib_mode`) or a rebuild is forced (not a `.p5z` build short of space, `cache::NoSpace`, or whose
`.p5z` the engine could not open just now, `cache::TryAgain`: both are tried again every pass, their tries not logged).
Duplicate title ids: a folder beats a pkg, which beats a .p5z
(`maxpack_logic::dups::claim`; ranks `RANK_FOLDER` < `RANK_PKG` < `RANK_P5Z`), then the first scan root wins; the others
show `duplicate`. A `.p5z` installed with the Install tab is thus superseded by its folder and shows as a duplicate; the
cache, whose stamp now differs in its kind, is rebuilt as the folder's, which removes the loose files the `.p5z` had
extracted (they are in its `.built`) and frees their space. When that folder is uninstalled, the `.p5z` is the game again. Low `/data` space (checked before the games, before and after each
build, after the scan): toast once per run and a UI banner.

## Register (`src/register.rs`)
SMP port: copy `sce_sys` to `/user/app/<TID>/sce_sys` + `icon0.png`, appmeta, trophy data,
nullfs `app0 -> /system_ex/app/<TID>` (app0 = cache dir for FPKG, game folder for Folder),
for a Folder with a backport a unionfs of the backport dir on top (SMP
`mount_backport_overlay`), then `sceAppInstUtilAppInstallTitleDir`. Where it does not resolve
(FW 12+) the title is only staged: InstallAll registers every folder under `/user/app`, so the scan
pass calls it once after its last game for every title staged in it (SMP install queue). Not
when the pass stopped for a game start: nothing registers while a game runs, and the titles staged
so far are not registered yet, so the pass after the game exits stages them again and installs them. Logged `<TIDs>: <dlsym error>; sceAppInstUtilAppInstallAll:
<rc> (<ms> ms)`; a failure marks each of those titles failed. A batch of more than one title
gets one toast (`<n> games registered` / `<n> games failed`) instead of one per title.
The tile (`MXPK00001`) is staged the same way on FW 12+ (`tile_staged`): the pass calls InstallAll
for it even with no game staged (not while a game runs), and `tile.ok` gets its hash once that succeeds
(a failed call leaves it `pending`, retried at the next start). The staged tile is taken before the call, so a tile the
tile thread stages while InstallAll runs waits for the next pass instead of being marked registered by a call that
never saw it (ported from the upstream project's 0.3.2). The tile registers again whenever
`tile.ok` lacks its param.json hash, a tile file changed, or app.db's `tbl_contentinfo` has no `MXPK00001`
row (`SELECT COUNT(*)`; an unreadable app.db trusts `tile.ok`).
Every staged copy (sce_sys, `icon0.png`, appmeta, trophy files) is set to 0666 after writing: a
copy keeps the source's mode, and a 0600 file from a game folder would stay unreadable for the
trophy and save services. A copied file named `param.json` gets SMP `copy_param_json_rewrite`: the
first `upgradable` becomes `standard` (`applicationDrmType`), logged `<TID>: param.json patched:
<dst>`; only the staged copies, never the game's own `sce_sys`. Trophy files
(`/system_data/priv/appmeta/<TID>/{trophy2,uds}/npbind.dat`, `param.json`) already there are not
overwritten (SMP), but get +0444 when a read bit is missing.
`/system_ex` is read-only unless etaHEN, SMP or kstuff-lite remounted it (elfldr does not): when
`mkdir -p /system_ex/app/<TID>` fails with EROFS, register remounts it once like SMP `remount_system_ex`
(`nmount` MNT_UPDATE of `/dev/ssd0.system_ex`) and retries. An existing title dir (EEXIST) needs no remount.
Title sound (`set_snd0info`, SMP `update_snd0info`): when `/user/appmeta/<TID>/snd0.at9` exists,
`UPDATE tbl_contentinfo SET snd0info = '/user/appmeta/<TID>/snd0.at9' WHERE titleId = <TID>` (rows
already pointing there are skipped) in `/system_data/priv/mms/app.db`, through `ps5/appdb.c` and the
SQLite 3.46.1 amalgamation (opened read-write, never created, busy timeout 3 s, one mutex for both
threads). Run 200 ms after a successful install call, once per run for a title already registered
(backfills older registrations), and after each game exit (the system rewrites the path); each runs on its own thread
(`spawn_snd0info`, the post-install one after the 200 ms), so a busy app.db (up to 3 s) holds neither the scan nor the watcher. Logged
`<TID>: snd0info set (<n> row(s))` only when a row changed; a failure is logged, never fatal.

## Launch (`src/launch.rs`)
As before (SMP sm_fakelib port): unionfs of `caches/<TID>/fakelib` (when non-empty) over the sandbox's
`<x>/common/lib` (the game's libs with ampr already copied on top, see Cache); one layer, no
mount of the ampr dir. Also nullfs of `/data` at `<sandbox>/data`, plus a nullfs of every mounted
`/mnt/usbN` / `/mnt/extN` at `<sandbox>/mnt/usbN` (pkg and packs on USB), and a read-only nullfs of
`/preinst/common/font` at `<sandbox>/preinst/common/font` when it exists (the console's fonts, which homebrew such as
ProsperoEden draws CJK, Arabic and Thai text with; SMP `mount_sandbox_fonts`, ported from the upstream project's 0.3.2).
Then, once the system's
`<sandbox>/app0` is mounted: when it is read-only, a read-write nullfs of `/system_ex/app/<TID>` (the
registered stack, backport unionfs included) over it; for a pkg game, a nullfs of the pkg's folder
(from the `.done` stamp's "source") at `<sandbox>/app0/kraken`; ng opens the .pkg named in `/app0/kraken.image`.
A single-file nullfs of the .pkg is refused by the kernel (ENOTDIR), hence the folder mount.
A .p5z game gets the same at `<sandbox>/app0/p5z`: `cache::mount_source` takes the folder below `/app0` from the stamp's
`kind` (`kraken` or `p5z`) and the file from its "source"; the `.p5z`'s folder is mounted read-only, so the game cannot
write beside it, and ampr-emu-ng finds the file through `/app0/maxpack.image` as `p5z/<file name>`.
All removed on exit, newest first: the nullfs mounts at once (left in the sandbox they hang ShellCore's
teardown and the console powers off), the overlay once ShellCore takes it down with common/lib (checked
every 250 ms), at most 3 s later (`EXIT_GRACE_MS`), or when the sandbox dir goes. An exec of the same title
in the same sandbox meanwhile (an exit-spawned ELF; a new overlay would fail with EBUSY)
cancels that and mounts the nullfs mounts again (~10 ms after the exec: the forked child gets its title id
only at the exec, so they cannot go back earlier). When the exec fallback cannot mount the overlay, they are
still mounted. A later copy of the payload (the previous one quit or crashed mid-game) adopts, in each sandbox where
our overlay is on `common/lib`, that overlay and our nullfs mounts there, in the order they were made so they go newest
first at the exit (`maxpack_logic::sandbox::ours`): `/data` and the drives (each from that very path), the fonts, the
writable `/app0` (a nullfs from `/system_ex/app/<TID>` that is not read-only, on top of a read-only mount of that
`/app0`: the system's, which is the only one `mount_app0` covers), then `/app0/kraken` or `/app0/p5z` (from the folder
of the stamp's source). Not adopted: a sandbox with our nullfs mounts and no overlay of ours (the exec fallback after a
failed overlay mounts them anyway), since ShadowMountPlus and the upstream project make the same mounts from the same
sources and nothing in the mount table tells them apart; its teardown can still hang if the payload died in that game.
When a session's cleanup ends, `register::spawn_snd0info(<TID>, 0)` (own thread).
No overlay when neither layer applies, and with an empty `fakelib/` (an `overlay_off` title, or a plain folder in the
fakelib mode `off`) no sandbox mount at all.

## Crash report (`src/crash.rs`, `maxpack_logic::crash`)
When an app crashes, the console writes `/user/devlog/system/sce_coredumps.0/<TID>_<n>/` with
`prosperocore-<n>-0x<pid>-<process>.prosperodmp` inside, and deletes the folder seconds later. No thread or watch of
its own: the launch watcher's NOTE_EXIT for a game it mounted (an exit-spawned ELF included) logs the exit status
from the event's data (a wait(2) status) on the `exit pid=...` line (`exited with N`, `killed by signal N`), then,
once the sandbox's nullfs mounts are gone (they must go first, see Launch), reads that folder once for `<TID>_*`
folders holding a dump with that pid. Only for a launch with our overlay on its `common/lib` at exit (the session's
`layers`, which only the cleanup's unmount takes, after this; an adopted session or an exit-spawned ELF in the same
sandbox has them too): a launch another mounter ran (even a unionfs of its own there), one with an empty `fakelib/`
(`overlay_off`), or one whose overlay failed to mount gets the exit line only, since the page would blame our dump, ng
or PlayGo stub. If no dump is there yet, it looks once more when that session's cleanup ends (0.4 to 3 s later, or when a
stale session is replaced, after its unmount), and logs
`pid=... killed by signal N; no crash dump in ...` when a killed game left none. A match logs `<TID>: crashed
(<process>, <status>), dump found at exit|<ms> ms after exit; the console's crash dump: <folder path>` and is kept as
the title's last crash in `/data/maxpack/crashes.json` (`{"<TID>": {at, process, folder, status}}`, keys sorted,
temp file + rename; a file that does not parse, or a key that is not a title id, reads as no crash), so it survives a
payload restart or a reboot. `at` is the console's clock, which may be years behind. `/api/state` shows it per game
and the report (`GET /api/report/<TID>`) names it. Ported from the upstream project's crash report (after its
0.3.1), with our paths.

## Module interfaces (contract between modules; keep these names)
```rust
// config.rs
pub const CONFIG_PATH: &str = "/data/maxpack/config.toml";
#[derive(Clone, PartialEq, Debug)]
pub struct Settings { pub scan_paths: Vec<String>, pub exclude: Vec<String>, pub inject_ampr: bool, pub add_emulators: bool,
  pub playgo_off: Vec<String>, pub hidden: Vec<String>, pub overlay_off: Vec<String>,
  pub fakelib_emulators: Vec<String>, pub fakelib_off: Vec<String>, pub manual_settings: Vec<String>,
  pub ampr_auto_update: bool, pub recommended_update: bool, pub backports: bool, pub scan_interval_secs: u64, pub stability_wait_secs: u64, pub low_space_gb: u64,
  pub auto_remove_missing: bool, pub auto_remove_delay_secs: u64, pub notify: bool, pub log: bool, pub debug: bool }
impl Default for Settings
pub fn load() -> Settings;                       // creates the file with defaults when missing
pub fn save(s: &Settings) -> std::io::Result<()>;
pub fn to_json(s: &Settings) -> serde_json::Value;          // keys = config keys, scanpath = array
pub fn from_json(v: &serde_json::Value) -> Result<Settings, String>;

// storage.rs
pub struct Drive { pub mount: String, pub label: String, pub kind: &'static str /* internal|usb|ext */,
  pub total: u64, pub free: u64 }
pub fn drives() -> Vec<Drive>;
pub fn drive_of(path: &std::path::Path) -> String;          // mount of the drive holding path ("/data", "/mnt/usb0")
pub fn default_scan_roots() -> Vec<String>;
pub fn mounted_aliases() -> Vec<String>;                    // mounted /mnt/usbN and /mnt/extN
pub fn ejectable(mount: &str) -> bool;                      // /mnt/usb0..7, /mnt/ext0

// scan.rs
#[derive(Clone, Copy, PartialEq, Eq, Debug)] pub enum Packing { Plain, Zstd, Lz4 }
#[derive(Clone, Copy, PartialEq, Eq, Debug)] pub enum Kind { Pkg, Folder(Packing), P5z }
pub fn kind_names(kind: Kind) -> (&'static str, Option<&'static str>); // /api/state kind and packing, maxpack_logic's names
#[derive(Clone, Debug)]
pub struct Source { pub path: std::path::PathBuf, pub kind: Kind, pub title_id: String, pub title_name: String,
  pub version: String, /* param.json contentVersion, or empty */
  pub drive: String, pub size: u64 /* pkg or .p5z file size; folder: 0 */, pub backport: Option<std::path::PathBuf> }
pub struct ScanResult { pub games: Vec<Source>, pub duplicates: Vec<Source>, pub errors: Vec<(std::path::PathBuf, String)>,
  pub unstable: Vec<(std::path::PathBuf, Option<String> /* title id when known */)>, // still being copied: skipped, not gone
  pub images: Vec<Image>,                                                  // ShadowMountPlus images: listed, never mounted
  pub unplayable: Vec<(std::path::PathBuf, maxpack_logic::p5z::Unplayable)> } // .p5z files the engine refuses: not games
pub fn scan(roots: &[String], exclude: &[String], backports: bool, stability_wait: u64) -> ScanResult;  // memoizes pkg param reads and believed .p5z verdicts by (path, size, mtime)
pub fn excluded(path: &std::path::Path, exclude: &[String]) -> bool; // path is an exclude entry or under one (whole components); backports too
pub fn still_written(paths: &[std::path::PathBuf], wait: u64) -> Option<u64>; // age in s of the youngest max(mtime, ctime) under wait; None = stable
pub fn parse_param(json: &[u8]) -> std::io::Result<(String, String, String)>; // (title_id, title_name, version); name: see below
pub fn valid_tid(t: &str) -> bool;

// cache.rs
pub const CACHES: &str = "/data/maxpack/caches";
pub const AMPR_DIR: &str = "/data/maxpack/ampr";
pub fn dir(tid: &str) -> std::path::PathBuf;
pub fn peek_pkg(pkg: &std::path::Path) -> std::io::Result<(String, String, String)>; // title_id, name, version (via scan::parse_param)
pub fn pkg_file(pkg: &std::path::Path, name: &str) -> std::io::Result<Vec<u8>>; // one file, e.g. "sce_sys/icon0.png"
pub enum Fresh { Fresh, Missing, Stale(String), Fakelib(String), Moved(std::path::PathBuf /* old source */) }
pub fn relink(src: &Source, s: &Settings, from: &std::path::Path) -> std::io::Result<()>; // Moved: no rebuild
pub fn pkg_id(pkg: &std::path::Path) -> std::io::Result<String>;   // sampled identity, see Cache
pub fn refresh_fakelib(src: &Source, s: &Settings) -> std::io::Result<()>; // Fakelib: fakelib/ only
pub fn overlay_off(src: &Source, s: &Settings) -> bool;                 // listed in overlay_off and a plain folder: empty fakelib/
pub fn fakelib_mode(src: &Source, s: &Settings) -> maxpack_logic::fakelib::Mode; // effective: recommended::effective
pub fn playgo_off(src: &Source, s: &Settings) -> bool;                  // effective: recommended::effective
pub fn check(src: &Source, s: &Settings) -> Fresh;
pub struct NoSpace { pub missing: u64 /* margin included */ }       // a .p5z build stopped at its space check (inside an io::Error)
pub fn missing_bytes(e: &std::io::Error) -> Option<u64>;           // Some(missing) when e is a NoSpace
pub struct TryAgain(String);                                        // a .p5z build or refresh stopped at the engine's open, tried again next pass (inside an io::Error)
pub fn try_again(e: &std::io::Error) -> bool;                      // e is a TryAgain
pub struct OldNg(String);                                           // a grown .p5z's build stopped: the ampr dir's ampr-emu-ng does not play it (inside an io::Error)
pub fn old_ng(e: &std::io::Error) -> bool;                         // e is an OldNg
pub fn ampr_state(src: &Source, s: &Settings) -> serde_json::Value; // {name: [size, mtime]} of the ampr dir's files fakelib/ gets, or null (main::inputs_key too)
pub use maxpack_logic::cache::{DONE, PKG_MOUNT_DIR, P5Z_MOUNT_DIR}; // the cache dir's names are maxpack_logic::cache's
pub fn mount_source(tid: &str) -> Option<(&'static str, String)>;  // pkg or .p5z: (folder below /app0: "kraken" | "p5z", the file)
pub fn build(src: &Source, s: &Settings) -> std::io::Result<()>;   // always rebuilds
pub fn app0(src: &Source) -> std::path::PathBuf;                    // pkg, .p5z: dir(tid); folder: src.path
pub fn sce_sys(src: &Source) -> std::path::PathBuf;                 // where sce_sys lives for this source
pub fn built_at(tid: &str) -> Option<u64>;                          // .done mtime, unix secs
pub fn size(tid: &str) -> u64;                                      // bytes, recursive
pub fn du(path: &std::path::Path) -> u64;                           // file bytes, recursive; no symlink traversal
pub fn total_size() -> u64;
pub fn purge(keep: &[String]) -> std::io::Result<()>;               // delete caches/<TID> not in keep; empty the kept ones
pub fn fix_access(src: &Source);                                    // add missing read/exec bits, see Cache "Access"

// register.rs
pub fn register(title_id: &str, app0: &std::path::Path, sce_sys: &std::path::Path, backport: Option<&std::path::Path>) -> std::io::Result<()>;
pub fn register_tile(port: u16);
pub fn tile_staged() -> bool;                             // FW 12+: the tile waits for the pass's install_all
pub fn is_registered(title_id: &str) -> bool;
pub fn registered(tids: &[&str]) -> Vec<bool>;            // is_registered for many, in tids order; app.db read again only when its (len, mtime) or tids change (all false when unreadable)
pub fn set_snd0info(title_id: &str);                      // app.db snd0info -> /user/appmeta/<TID>/snd0.at9; logs, never fails
pub fn spawn_snd0info(title_id: &str, delay_ms: u64);    // set_snd0info on its own thread after delay_ms
pub fn mount(title_id: &str, app0: &std::path::Path, backport: Option<&std::path::Path>) -> std::io::Result<()>;
pub fn remount(title_id: &str, app0: &std::path::Path, backport: Option<&std::path::Path>) -> std::io::Result<()>; // moved folder: whole stack
pub fn unmount_title(tid: &str) -> std::io::Result<()>;   // every layer at /system_ex/app/<TID>, never MNT_FORCE
pub fn uninstall(tid: &str) -> std::io::Result<()>;       // sceAppInstUtilAppUnInstall
pub fn forget(tid: &str);                                 // after an auto-remove: drop the staged sce_sys/param.json so is_registered is false
pub fn installed_copy(title_id: &str) -> Option<std::path::PathBuf>; // the path that shows the title id installed on the console
pub fn is_ours(title_id: &str) -> Result<(), std::path::PathBuf>; // Err(path) = an installed copy, or no staged param.json: never auto-remove

// main.rs (shared version compare, used by cache.rs for ampr-emu-ng identities)
pub(crate) fn parse_version(v: &str) -> Option<(u32, u32, u32, u32)>; // "X.Y.Z" or "X.Y.Z-maxpack.N", leading "v" allowed: (X, Y, Z, N), N = 0 without the suffix
pub(crate) fn newer(a: Option<&str>, b: Option<&str>) -> bool;   // a strictly newer than b; an unknown version is older than any
pub(crate) fn ampr_version_of(path: &std::path::Path) -> Option<String>; // a libSceAmpr.sprx (ampr dir's or a fakelib copy): bundled VERSION or ampr.bundled's; None = unknown
pub(crate) fn ampr_mark() -> Option<String>;                     // ampr.bundled's text (inputs_key)

// installer.rs (the MaxPack engine through ps5/installer.c and mp_api_call; Answer = {status, ctype, body})
pub fn start(state: Arc<State>, runtime: &'static [u8], runtime_version: &'static str); // own thread; 503 until up
pub fn call(method: &str, path: &str, host: &str, ctype: &str, body: &[u8], loopback: bool) -> Answer; // one at a time
pub fn stop();                                            // before exit: pause, wait for the queue (3 s at most)

// launch.rs (titles: overlay these; running: title ids with a live session, kept by the watcher)
pub fn watch(titles: Arc<Mutex<HashSet<String>>>, running: Arc<Mutex<HashSet<String>>>);

// crash.rs (the pure part, file format and matching, is maxpack_logic::crash)
pub fn load() -> HashMap<String, maxpack_logic::crash::Crash>;           // crashes.json
pub fn check(tid: &str, pid: libc::pid_t, status: &str, when: &str) -> bool; // dump found: logged and recorded

// sys.rs (the page's Play and the report's firmware)
pub fn launch_app(title_id: &str) -> Result<(), String>; // sceLncUtilLaunchApp, then sceSystemServiceLaunchApp
pub fn firmware() -> Option<String>;                     // "13.60" from the SDK's kernel_get_fw_version

// net.rs (blocking GET over sceHttp/sceSsl, 4 pinned root CAs; Err on non-200, sce error or > max bytes)
pub fn https_get(url: &str, max: usize) -> std::io::Result<Vec<u8>>;
pub fn status_of(e: &std::io::Error) -> Option<u16>;    // the HTTP status of a non-200 answer
pub fn too_large(e: &std::io::Error) -> bool;          // a body over max: never tried again
```

## Recommended settings (`src/recommended.rs`, `maxpack_logic::recommended`)
A list of per-game settings that a game needs to start: its Fakelib mode, its PlayGo stub, and a notice. Out of
scope: every other setting (`overlay_off`, backports, scans). The console never sends anything about its games.

- **Files.** `data/recommended.json` in the repository, bundled in the ELF (`include_bytes!`; its parse is a test,
  `bundled_list_parses`), and the last downloaded copy, `/data/maxpack/recommended.json`. At start `recommended::init`
  takes the cached copy when it parses and its `version` is higher than the bundled one's, else the bundled list:
  `recommended: v<version> (<n> titles, bundled|downloaded)`. Before that line, `recommended: cached copy ignored:
  <reason>` when the copy cannot be read or does not parse (the file stays), or `recommended: removed
  /data/maxpack/recommended.json: v<x> is not newer than the bundled v<y>` when it parses but is not newer (a
  MaxPack update bundles a newer list): that file is of no use any more and is deleted, so the line comes once
  (when the delete fails: `recommended: cached copy ignored: v<x> is not newer than the bundled v<y> (remove:
  <error>)`, and the file stays).
- **Format.** `{"format": 1, "version": <positive integer, higher for each change>, "titles": {"<key>": {entry}}}`.
  An entry may have `fakelib` (`full`, `emulators` or `off`), `playgo` (`false`: no bundled PlayGo stub, as
  `playgo_off`), `notice` (`long_first_boot`), and `fw_min` / `fw_max` (`"NN.NN"`, inclusive bounds on the console's
  firmware, compared as its BCD version; on a console whose firmware cannot be read an entry with a bound never applies).
- **Key.** The list names no game: a title is keyed by the lowercase hex of the first 16 bytes of the SHA-256 of
  `maxpack-recommended-v1:` and its title id (`key("PPSA01234")` is `17c2e30975e7e93c42a3ccaa639d8e5b`).
  `node tools/recommended_key.js <TITLE_ID>` prints it. It does not hide a title from someone who tries the few
  possible ids; it keeps the file from being read at a glance.
- **Limits and checks.** At most 262144 bytes and 4096 titles. A wrong structure refuses the whole file: not JSON
  (or nested past serde_json's depth limit), over a limit, `format` other than 1, `version` not a positive integer,
  no `titles` object, a key that is not 32 lowercase hex digits, an entry that is not an object, a known field of
  the wrong JSON type, a malformed `fw_min` / `fw_max`. An unknown value of a known field (a newer Fakelib mode or
  notice) or an unknown field is ignored for that entry, so a newer list stays readable by an older MaxPack.
- **Precedence** (`maxpack_logic::recommended::effective`, asked by `cache::fakelib_mode`, `cache::playgo_off`,
  `main::inputs_key`, `/api/state` and the report). A title in any of `manual_settings`, `fakelib_emulators`,
  `fakelib_off` or `playgo_off` is the user's: its mode and stub come from those lists (`settings_source` "user").
  Otherwise an entry with `fakelib` or `playgo` gives them (a missing one is Full / with the stub; "recommended";
  `fakelib_recommended` / `playgo_recommended` say which of the two the entry itself sets, and only those get the
  sheet's tag and the report's `(recommended)`).
  Otherwise Full with the stub ("default"). The notice is the entry's in every case. A hand change on the sheet adds
  the title to `manual_settings`; when its settings were the list's, the list's values are written into the lists
  first, so the one the user did not touch stays as shown (`hand_choice`). `POST /api/recommended/<TID>` takes it
  out of all four lists. Values copied so stay the user's even if a later list drops the game's entry: the sheet's
  way back needs an entry (`recommended` not null, Library page), so they are then changed on the sheet like any
  other choice of the user's.
- **Refresh** (`recommended::spawn_refresh`, once per start). Skipped with `recommended: refresh off` without
  `recommended_update`. Skipped with `recommended: refresh skipped until the next start: the secure connection check
  has not passed` when the self-test has not passed by the time MaxPack starts (`tls::passed`, asked once): a check
  that passes later in the same run does not start it; the next start does. Otherwise
  `GET https://raw.githubusercontent.com/DeLFuS77/maxpack-ps5/main/data/recommended.json` (a redirect is followed,
  as for every request of `net.rs`, which turns on sceHttp's auto-redirect, and whatever answers is parsed as
  strictly; 262144 bytes at most;
  tried again as the self-update's look, `maxpack_logic::update::look_failed`, after no answer or a server error;
  a body over that size is not tried again, `net::too_large`),
  checked with `parse`, and only when its `version` is higher than the list in use: saved to
  `/data/maxpack/recommended.json` (tmp + rename), put in use, `recommended: downloaded v<version> (<n> titles)`, and
  the scan loop woken, which recomposes the `fakelib/` of every game whose effective mode or stub changed (Cache).
  Else `recommended: kept v<version> (download is not newer)`, `recommended: download refused: <reason>` or
  `recommended: download failed: <error>` (`... download refused: GET <url>: body over 262144 bytes` for one too
  large); the list in use stays.
- **Notice at launch** (`recommended::on_launch`, from the launch watcher: every new sandbox of one of our titles,
  and an exec the sandbox watch missed). With `notify`, the notice as a notification in the console's language
  (`i18n::notice_long_first_boot`), sent from its own thread; logged `<TID>: notice long_first_boot`, or
  `... not shown (notify off)`. The page shows it in the game's sheet too.

## Web API (`src/web.rs`, port 8095, served to `src/ui.html`)
Rest mode can close the listening socket: on an accept error the server binds port 8095 again every second.
If a newer copy took the port meanwhile, this copy quits (not while a game runs).

Every request passes `maxpack_logic::http::check` before its route, or gets 403 (`/api/inst/*`: the engine's
JSON `{ok:false, code:"forbidden", detail}`; the rest: plain text in Spanish and English; logged at most once a
minute):
- Host must be an IPv4 literal, a bracketed IPv6 literal or `localhost`, with an optional port (the engine's
  `host_ok`, a little stricter). A DNS name never passes, so a page that rebinds its name to the console (DNS
  rebinding) reads and changes nothing, and the page opened by a name gets 403 on everything.
- Every method other than GET and HEAD (the page's state changes are POSTs) must come from the page itself:
  `Sec-Fetch-Site: cross-site` is refused, and an `Origin`, when sent, must be `http://` (or `https://`) plus
  this very Host (`null` is refused). No Origin passes: browsers send one on every cross-origin POST, so only
  something that is not another site's page (curl, `web::take_port`'s loopback quit) sends none. Chosen over
  requiring `application/json` on every POST: the page's text bodies (`delete`, `eject`, `playgo`, `fakelib`, `quit`), the
  loopback quit of a newer copy and older copies' quit requests keep working unchanged.
- `GET /` ui.html (`__VERSION__` filled in); `GET /app-icon.png` the app icon (`ps5/icon0.png`);
  `GET /maxpack.appcache` the AppCache manifest (version + page hash). These three are `no-cache`,
  the rest `no-store`.
- `GET /api/state`:
  `{version, update:{version, size, tls_passed}|null (a newer release found at start, see Self-update), tls_check:{passed?, at?, version?, running} (the secure-connection self-test), busy, paused, waiting:"kstuff"|"other:<process>"|null, running:[TID], images:[{path, kind:"ffpkg"|"exfat"|"ffpfs"|"ffpfsc", title_id?}] (ShadowMountPlus images found where a source could be: never mounted; `title_id` from a `[TITLE_ID]` in the file name, absent otherwise), games:[{title_id, title_name, version, path, kind:"pkg"|"folder"|"p5z", packing:"plain"|"zstd"|"lz4"|null,
  drive, size, status:"pending"|"checking"|"building"|"ready"|"error"|"duplicate", error, installed_copy (the path that
  shows this title id installed on the console, with status "error": MaxPack leaves it alone; else null), error_code ("no_space" for a .p5z that does not fit, "old_ng" for a .p5z that grew and an ampr-emu-ng that does not play it (Cache), with status "error"; else null), missing_bytes (bytes short, margin included, with `no_space`; else null), cache_size, cache_built, backport,
  playgo: bool|null (the memoized "eboot imports PlayGo", null when unknown or `add_emulators` is off), playgo_off: bool (false while `add_emulators` is off),
  hidden: bool (in the `hidden` setting: the page lists it only with "show hidden" on),
  crash: {at, process, status}|null (the last crash, from crashes.json, see Crash report),
  rebuild_queued: bool (a rebuild asked for, not started yet: the title is in `forced`; the page keeps Rebuild disabled through it and while the game is pending, checking or building),
  registered: bool (`register::registered`: the staged `param.json` files and the title id in app.db; app.db is read and scanned again only when its length or mtime, or the titles asked, changed: `maxpack_logic::presence::DbMemo`),
  mounted: bool (`/system_ex/app/<TID>` is a mount point, one mount table per request), source_present: bool (the source path exists),
  overlay_off: bool (listed in `overlay_off` and a plain folder game, as `cache::overlay_off`),
  overlay_off_listed: bool (listed in `overlay_off`, whatever this copy is: true with `overlay_off` false is a stale entry),
  fakelib_mode: "full"|"emulators"|"off" (the title's effective fakelib mode, `recommended::effective`; `playgo_off` is the effective one too),
  settings_source: "default"|"user"|"recommended" (where the two come from), recommended: {fakelib: mode|null,
  playgo_off: bool|null}|null (what the list says of the two for this title on this firmware; null when it says nothing
  of them), notice: "long_first_boot"|null (the list's notice for the title)}],
  unplayable:[{path, code:"damaged"|"too_many_volumes", detail}] (the `.p5z` files of the last scan that cannot be played: `detail` is the engine's reason, or the volume count; the page lists them under "Archivos .p5z que no se pueden jugar" and never as games),
  drives:[{mount, label, kind, total, free, ejectable}], cache:{path, size, free, low_space, ampr_present}, log:[last 100 lines of the last 64 KiB]}`
- `GET /api/settings` -> settings JSON; `POST /api/settings` (JSON body) -> save, apply, rescan; a body
  without `playgo_off`, `hidden`, `overlay_off`, `fakelib_emulators`, `fakelib_off`, `manual_settings` or `exclude` keeps that current list
  from the file (the settings form sends `exclude` only); then a title in both fakelib lists is kept in `fakelib_off`
  only (`fakelib::exclusive`, logged); 409 when `overlay_off` names a library game that needs the overlay (a pkg, a `.p5z` or a
  packed folder, `maxpack_logic::overlay::needed`, the reason as text). A `scanpath` entry that
  may never be a root (`maxpack_logic::roots::never_root`) is 400 JSON `{code:"forbidden_root", path, detail}`
  and nothing is saved; the page says it in its language.
- `POST /api/playgo/<TID>`, body exactly `on` or `off` -> removes the title from / adds it to `playgo_off`,
  and adds it to `manual_settings` (`maxpack_logic::recommended::hand_choice`: when its settings were the recommended
  ones, their Fakelib mode is written into the lists first, so it stays as shown),
  saves, applies, rescans, logs `<TID>: PlayGo stub on|off`, 204; 404 for an invalid or absent library
  title, 400 for any other body.
- `POST /api/hidden/<TID>`, body exactly `on` or `off` -> adds the title to / removes it from `hidden`
  (`maxpack_logic::titles::set_listed`: never twice, the others keep their order), saves, logs `<TID>: hidden in
  the library|shown in the library`, 204; no rescan (nothing but the page changes). 404 for an invalid title id or,
  with `on`, one not in the library (`off` also takes a title that is not there now, so a stale entry can go); 400
  for any other body. A `POST /api/settings` without `hidden` keeps the list, as for `playgo_off`.
- `POST /api/overlay/<TID>`, body exactly `on` or `off` -> removes the title from / adds it to `overlay_off`
  (`set_listed`), saves, applies, rescans (a fakelib refresh), logs `<TID>: overlay on|off`, 204; 404 for an invalid
  or absent library title, 400 for any other body, 409 for `off` on a game that needs the overlay (logged
  `<TID>: overlay off refused: <path> (<reason>)`).
- `POST /api/fakelib/<TID>`, body exactly `full`, `emulators` or `off` (`maxpack_logic::fakelib::body`) -> puts the
  title in `fakelib_emulators` or `fakelib_off` and out of the other, or out of both for `full`
  (`fakelib::set_mode`), and adds it to `manual_settings` (`hand_choice`, as for PlayGo: a recommended PlayGo setting
  stays as shown), saves, applies, rescans (a fakelib refresh before the next launch; while a game runs the
  pass waits for its exit, as for PlayGo), logs `<TID>: fakelib mode <mode>`, 204; 404 for an invalid or absent
  library title, 400 for any other body.
- `POST /api/recommended/<TID>`, body exactly `reset` -> "Back to the recommended setting": takes the title out of
  `manual_settings`, `fakelib_emulators`, `fakelib_off` and `playgo_off` (`maxpack_logic::recommended::reset`), saves,
  applies, rescans (a fakelib refresh when that changes its mode or stub), logs `<TID>: back to the recommended
  settings (fakelib mode <mode>, PlayGo stub on|off, <source>)`, 204; 404 for an invalid or absent library title,
  400 for any other body.
- `POST /api/launch/<TID>` -> starts the title for the foreground user as its home-screen tile does
  (`sys::launch_app`: `sceLncUtilLaunchApp` through its NID in libSceSystemService, taken when it returns >= 0 or
  0x80940005; else `sceSystemServiceLaunchApp`), 204 once a launch is taken; the launch watcher then sees the game
  as for any launch. Refused by `maxpack_logic::play::gate`, in this order: 404 for an invalid or absent library
  title, 409 `a game is running`, 409 while MaxPack does not mount (another mounter or no kstuff: what the home screen
  would start is not MaxPack's), 409 `the game is not ready` (any status but `ready`); 502 + error text when both
  calls fail. Logged `<TID>: launch from the web UI` and each call's return code. Like every POST it passes the
  Origin and Host checks above: another site's page cannot start a game.
- `GET /api/report/<TID>` -> plain text for an issue (`maxpack_logic::report::text`): MaxPack's version, the
  firmware (`sys::firmware`, else "unknown: please fill in"), who mounts (`waiting`), the game (name, title id,
  version, source path, kind, drive, status and error, hidden, and "MaxPack overlay: off" for an `overlay_off` game), the backport,
  the fakelib mode when not `full` (`Fakelib mode: emulators only (...)`, `Fakelib mode: off (only ampr-emu-ng)` or
  `(empty: nothing at launch)` for a plain folder), ampr-emu-ng (`/api/ampr` kind and
  version), the PlayGo stub (on, off, or not added), the last crash, a ready row for `docs/compatibility.md`
  (`compat_row`, in the list's column order: Original → `.p5z`, Format (`kind_text`), Volumes, Drive (`drive_text`:
  `/data` internal SSD, `/mnt/ext1` M.2 SSD, `/mnt/usbN` and `/mnt/ext0` "USB SSD or HDD?"), Firmware, Loader/mounter
  (`MaxPack <version>`), Fakelib mode (Full, Emulators only, Off, or none for an `overlay_off` game), Result, Date, Notes;
  no game name, title id or game version, since the list names no game; the sizes, a `.p5z`'s volumes, the result and
  the date are left as `?`, the date because the console's clock is often wrong; a `|` escaped and a control character
  a space), and the last 40 lines of the log tail (64 KiB) that contain the title id; 404 for an invalid or absent library
  title. The game sheet's "Copy report" copies it.
- `GET /api/ampr` -> `{kind: "bundled"|"downloaded"|"hand"|"missing"|"error", version, updating}` of
  `ampr/libSceAmpr.sprx`; `POST /api/ampr/update` -> forced update (Flow, Auto-update), answered when done:
  200 + result text, else `{code, detail}` (`maxpack_logic::ampr::Fail`): 409 `tls_unchecked` until the
  secure-connection self-test has passed (the self-update's code and page text), 409 `update_running`, 502
  `download`, `release` or `checksum` (`SHA256SUMS` missing, malformed or not matching: nothing installed;
  the page's `amprFail_checksum`), 409 `older` (the latest release is older than the bundled ampr-emu-ng or the
  one in use: nothing installed; `amprFail_older`), 500 `install`. The page shows its own text for `tls_unchecked`,
  `update_running`, `download`, `checksum` and `older`, the detail for the rest.
- `POST /api/rescan`, `POST /api/rebuild/<TID>`, `POST /api/rebuild-all`, `POST /api/purge` -> 204.
- `GET /api/sizes/<TID>` -> `{game: u64, dlc: u64, save: u64|null, cache: u64}` in bytes,
  measured on request; 404 for an invalid or absent library title. Game is the pkg file or folder,
  DLC is summed across `/user/addcont` and both extended drives' `user/addcont` and `ps5/user/addcont`,
  save is summed across eight-hex-digit `/user/home/<id>/<family>/<TID>` directories, where family
  is `savedata_prospero` for a `PPSA` title id and `savedata` otherwise.
  Missing directories count as zero; unreadable `/user/home` gives `save: null`.
- `POST /api/delete/<TID>`, body: the source path the UI showed (the game's `path`) -> 204 on success,
  404 for an invalid or absent library title, 409 while the scan loop is in a pass (it holds
  `State::work` for scan, purge, build and register; delete only try-locks it), when the body is not
  the current library source, while a game runs, while MaxPack does not mount (another mounter runs,
  or no kstuff: what is at `/system_ex/app/<TID>` may be that mounter's), this title builds, the source path is unsafe, or
  its title mount cannot be unmounted; 500 on source or cache deletion failure. It validates the
  source is inside a scan root with no symlink from the root down (links above the root are not checked), and is a .pkg or .p5z file or game
  directory, then unmounts `/system_ex/app/<TID>` without force and deletes the source. If that
  fails, the cache and tile stay and the next pass remounts the title (a folder partly removed stays so).
  Then it uninstalls the home-screen entry (failure logged), removes the title from the launch set
  and its game row, wakes the scan loop, deletes the cache, and logs and optionally notifies.
- `POST /api/eject`, body: the drive's mount path (`/mnt/usb0`) -> 204 once the path is no longer a
  mount point; 400 when not ejectable, 404 when not mounted, 409 while the scan loop is in a pass
  (try-lock of `State::work`, held to the end so no pass builds, registers or remounts meanwhile),
  while a game runs, while MaxPack does not mount (as delete), when another partition of the same disk (`/dev/daN` followed by the end, `p` or
  `s`) is mounted, when a title mount from the drive cannot be unmounted (error text), or when the
  drive changed (`km_usb_eject` ENOENT/ESTALE); 502 with errno and native status in hex when the
  request fails, or when the mount is still there after 5 s (polled every 100 ms). Before the request it
  unmounts, without force, every `/system_ex/app/<TID>` whose source (minus `<above>:`) is the drive
  or under it; other mounts are left to the system (as SMP). Then it wakes the scan loop: the next
  pass forgets the gone drive's games, or after a failure remounts the titles. Every outcome is
  logged as `eject <path>: ...`.
- `GET /icon/<TID>` icon0.png, `GET /pic/<TID>` pic0.png (or pic1.png): from the source's sce_sys, fallback
  `/user/appmeta/<TID>/`, then (pkg) from the .pkg itself via `cache::pkg_file`; 404 when none. Keyed by title id; the UI must re-request when the title id appears.
- `GET /file/data/...` only with `debug=1`. `POST /api/quit`: loopback with an empty body (a newer copy
  taking over) or body `shutdown` from any host (the UI's Shut down, toast "MaxPack stopped"); 409 while a game runs.
  A newer copy's quit is also 409 while a scan pass runs (try-lock of `State::work`, then held until the exit so no
  pass starts): quitting mid-build would leave a cache without `.built`, which the next start empties.
  `web::take_port` asks again on each of its 20 tries (5 s) and reads each answer: when the last one was that 409
  (`maxpack_logic::http::QUIT_BUILDING`) the new copy's toast says MaxPack is finishing a build and to try again in a
  moment (`i18n::finishing_build`), otherwise "port 8095 busy" (`i18n::port_busy`).
  Before exiting, the installer is stopped in order (`installer::stop`); the same when another copy took the port.
- `POST /api/self_update`, body `{"version":"<tag>"}` -> see Self-update: 200 `{}` once the checked ELF went to
  elfldr, else `{code, detail}` (`maxpack_logic::update::Fail`): 400 `not_offered`, 409 `game_running`,
  `update_running`, `scan_running` or `install_running`, 503 `no_elfldr`, 502 `download`, `checksum` or `send`;
  400 text for a body without a string `version`. Behind the same Host and Origin checks as every POST.
  409 `tls_unchecked` until the secure-connection self-test has passed on this console (Self-update, below).
- `POST /api/selftest/tls` -> the secure-connection self-test (`src/tls.rs`, `maxpack_logic::tls`), answered when it
  ends (up to about a minute): 200 `{verdict:"passed"|"broken"|"inconclusive", passed, verify_option:"set"|"0x...",
  probes:[{url, expect:"refused"|"connects", check:"ok"|"accepted"|"unproven", status?, code?, ssl_detail?, error?}]}`;
  409 text while another run is under way. Behind the same Host and Origin checks as every POST.
- `/api/inst/<x>`: the installer's own API as `/api/<x>` (`maxpack_logic::inst::installer_path`): GET `state`,
  `icon?p5z=`; POST `scan`, `probe`, `install`, `pause`, `verify`, `uninstall`, `discard` (JSON bodies, see the
  engine's `src/web/mp_api.h`); anything else 404. Method, Host, Content-Type and body go as sent, `loopback`
  from the TCP peer; the engine checks Host (an IP or localhost) and, on POST, `application/json`, and its
  answer goes back as it is (an application error is 200 `{ok:false, code, detail}`). 503
  `{ok:false, code:"starting"|"no_installer", detail}` until it is up or when it could not start.
  As the mounter, around it (`maxpack_logic::inst`): while a game runs (`State::paused`), an `uninstall`, and an
  `install` whose `replace` is not empty, are 409 `{ok:false, code:"game_running"}` (the engine deletes folders
  with no such check). An `uninstall` the installer accepts is followed through its `/api/state` every 500 ms
  (`UninstallWatch`, up to 1 h); once the game is uninstalled the scan loop is woken and `web::uninstalled` ends it
  as `/api/delete` does: unmount without force, `register::uninstall` and `forget`, out of `titles` and the
  library, cache deleted. Only while MaxPack mounts (in "installer only" mode the other mounter owns the home
  entries), only for a title in `titles` whose library copy (when the uninstall was asked for) is gone, never a
  title id installed on the console, and after a running game exits; anything else is logged and left to
  auto-remove.
  Never gated on the mounter state: that is the "installer only" mode.
- Not ported from the upstream project's 0.3.2: its ShadowMountPlus-shaped `POST /api/v1/*` routes (for Playden).
  They would pass the checks above (Playden is a native app on the console: libcurl to `127.0.0.1`, no Origin), but
  Playden calls ShadowMountPlus only, at `127.0.0.1:10101`, and the upstream project serves them on its own port,
  which MaxPack never takes; on 8095 nothing would call them. Its self-update and the release's `payloads.json` are
  ported (Self-update, below), with a checksum check the upstream project does not make, and the secure-connection
  self-test it waits for.

## Self-update (`src/update.rs`, `maxpack_logic::update`)
- At start (`update::spawn_check`, its own thread) MaxPack asks GitHub for the latest published release of
  DeLFuS77/maxpack-ps5 (`https://api.github.com/repos/DeLFuS77/maxpack-ps5/releases/latest`, no token, through
  `net::https_get`). A 404 means none is published, or the repository is private: one log line
  (`update: no published release ...`), nothing offered, no retry. No answer, or a 5xx, is tried again 20 s later,
  3 tries in all; any other status (a 403 rate limit), or an answer over its size limit (`net::too_large`), is logged
  once. Only one log line says how the look ended.
- A release is offered (`maxpack_logic::update::latest`) when its tag is `vX.Y.Z` (no pre-release suffix), newer
  than `Cargo.toml`'s version by numbers, not a draft or a pre-release, and it has exactly one `maxpack.elf` and one
  `SHA256SUMS` asset, each at `https://github.com/DeLFuS77/maxpack-ps5/releases/download/<tag>/<name>` and within the
  read limits (32 MiB, 64 KiB). Anything else is logged and not offered. The offer is `update` in `/api/state`.
- `POST /api/self_update` (`update::install`): refused when the version is not the offer, while a game runs, while
  another update runs or one was sent less than 90 s ago, while a scan pass holds `State::work`, or while the
  installer runs a job (`installer::idle`: quitting would pause it), or until the secure-connection self-test has
  passed (below). Otherwise it probes elfldr
  (`127.0.0.1:9021`), downloads `SHA256SUMS` and `maxpack.elf` (one try each), and sends the ELF to elfldr only
  when its size is the asset's, it starts with the ELF (or SELF) magic and its SHA-256
  (`maxpack_logic::sha256`, FIPS 180-4, tested on the host) is the one `SHA256SUMS` lists for `maxpack.elf`
  (`update::verify`). A game or an install started during the download refuses the send. Every outcome is logged.
- The secure-connection self-test (`src/tls.rs`, `maxpack_logic::tls`). `SHA256SUMS` comes over the same HTTPS
  channel as the ELF, so the checksum stops a network attacker only if the console checks certificates.
  `net::https_get` loads 4 root CAs and sets `SERVER_VERIFY | CN_CHECK | NOT_AFTER_CHECK | KNOWN_CA_CHECK` on every
  template with `sceHttpsEnableOption` (a failure is logged once and reported, not fatal); `NOT_BEFORE_CHECK` is left
  to the firmware, since the console's clock may be behind. Whether the console applies them is shown by Settings >
  "Check secure connections" (`POST /api/selftest/tls`): a HEAD with 8 s timeouts to five sites, one after the
  other. `untrusted-root.badssl.com`, `wrong.host.badssl.com` and `expired.badssl.com` must be refused by the TLS
  layer (SCE_HTTPS_ERROR_CERT or _HANDSHAKE, SCE_HTTP_ERROR_SSL, or a certificate bit in sceHttpsGetSslError's
  detail; a DNS error or a timeout proves nothing); `badssl.com` (the same `*.badssl.com` certificate as wrong.host,
  under ISRG Root X1, so its refusal is the host check) and `api.github.com` must connect. Any bad site accepted is
  `broken`; everything as expected is `passed`; anything else is `inconclusive`. `passed` and `broken` are kept in
  `/data/maxpack/tls_check.json` (`{passed, at, version}`); `inconclusive` leaves it as it was. Until the last
  conclusive run passed, `update.tls_passed` is false in `/api/state`, the notice asks for the check (its button runs
  it) instead of offering Update, and `update::install` refuses with `tls_unchecked` before anything is downloaded
  (`update::tls_gate`; the ampr-emu-ng update, which installs code every game loads, waits for the same gate).
  `/api/state` `tls_check` is `{passed, at, version}` of that record (empty without one) and `running`.
- While an update runs, and for 90 s after its ELF was sent (`HOLD_MS`), the scan loop starts no pass
  (`update::holding`), so the new copy finds none to wait for. The new copy takes the port as any newer copy
  (`web::take_port`, above) and saves its own self copy; the hold runs out by itself if it never starts.
- The page shows the offer above the library ("MaxPack <version> is available", Update and Later; Later hides it
  until the page reloads). While it runs the notice says the ELF is downloaded and checked; the saved copy is not
  started from the console's browser meanwhile; once `/api/state` answers with another version the page reloads,
  and after 2 minutes without that it says the new version did not start. Each error code has its own text.
- Releases carry `payloads.json` for ps5-payload-manager (`ps5/make-payloads-json.sh`, run by the release job and
  tried by the payload job on every push): one payload, `maxpack.elf` of that release with its SHA-256. The address
  to add to the manager is `https://github.com/DeLFuS77/maxpack-ps5/releases/latest/download/payloads.json`. The
  release job refuses a tag that is not `v` plus `Cargo.toml`'s version, so the tag the page compares is the
  version the ELF reports.

## Self copy and start on open (as Dump Forge)
`ps5/build.sh` builds twice: stage 1, then stage 2 (`--features self-copy`, env `MAXPACK_SELF_ELF`) embedding
stage 1. The pushed stage 2 saves stage 1 to `/data/maxpack/maxpack.elf` at start when the bytes differ
(temp + fsync + rename; the last pushed wins). The tile opens the AppCache'd page; on the console's own browser
(127.0.0.1:8095) a failed `/api/state` makes it ask elfldr once per page load:
`GET http://127.0.0.1:9021/data/maxpack/maxpack.elf?args=serve&pipe=0&n=<nonce>`, then it keeps polling.
- Requests are capped at 16 KiB: head too large 431, head + Content-Length too large (or overflowing) 413,
  non-numeric Content-Length 400.

## Installer (`src/installer.rs`, `ps5/installer.c`, `vendor/maxpack`)
The phase-2 installer is the MaxPack engine in C: `vendor/maxpack` is a copy of its sources at one commit
(`vendor/maxpack/COMMIT`, written by `tools/sync_maxpack.sh <maxpack checkout> <commit>`, and the tag that keeps it
there in `vendor/maxpack/COMMIT.tag`; now `motor-consola-4`, 5463d75, the engine of MaxPack 1.0.7 for the PC; before it
`motor-consola-3`, e8de15a), with the zstd and
zlib its submodules pin. Since then the engine reads a `.p5z` that grew (Supported sources), and its installer refuses
one (`grown_p5z`: its packs are laid out as only ampr-emu-ng's p5z mode reads them; play it from the `.p5z`, or make a
compact copy on the PC with `maxpack compact`, which is index version 1 again). Verify compares an installed `.p5z`'s
loose files only with the `.p5z` it was installed from (still there, the same `archive_id`, index version 1): once
that `.p5z` grew on the PC, Verify checks only the folder's packs, as when the `.p5z` is gone, and an install that
works is not called damaged. The page shows the counts either way (the engine's detail says "without the .p5z to
compare the loose files", which the page does not show). `ps5/build.sh` builds it with the SDK's `prospero-cmake -DMAXPACK_PS5=ON` into static
libraries (`target/ps5/engine`) that `ps5/linker.sh` links, and checks in lld's link map that `mp_api_call` and
`mp_inst_install` are in `maxpack.elf` (`maxpack.elf.txt`). `ps5/installer.c` sets it up as the engine's
`src/ps5/main_ps5.c` does: `mp_inst_env_default` (`/data`, `/mnt/usb0..7`, `/mnt/ext0..1`), the ShadowMountPlus
client with `smp_fakelib_mode` from `/data/maxpack/config.json` (installs register in ShadowMountPlus as in
phase 2), the runtime = `BUNDLED_AMPR` (the bundled `libSceAmpr.sprx`, embedded once; mtime from build.sh),
toasts through `sys::notify` when `notify` is on. Its data stays in `/data/maxpack/` (`state/`, `installed/`,
`tmp/`, `config.json`). It has no server: web.rs forwards `/api/inst/*` (one request at a time, as the engine's
own server). Quitting pauses a running install (POST `/api/pause`, again for each queued job that starts) and
waits up to 3 s for the queue (`mp_api_wait_idle`); a paused install is listed as unfinished at the next start,
where Install resumes it.
The page's **Install** tab is phase 2's page (the engine's `web/index.html`, gone since its commit f8e931a) in this UI's style: the job with
progress, speed and time left (Pause), unfinished installs (Resume, Discard), what can be installed (icon,
saving or "copied as is", Search again, a path added by hand through `probe`), the destination drives with
their free space, Install (a replace asks first, listing the copies it deletes), and what MaxPack installed
(Verify, Uninstall). The free-space check before a copy (the only copy the page offers; it moves nothing):
each destination drive says whether the chosen game fits, as the engine's plan counts it (`install_size` plus
the engine's 256 MiB `space_margin` against the drive's free space, plus what an unfinished install of that title
already copied there), "Does not fit: N more needed" when not, and Install then says so without asking the engine;
the engine checks again when it plans the install (`no_space`). It polls `/api/inst/state` every second while open; the engine's error codes are shown in
the UI's words (`grown_p5z` among them), its `detail` (the engine's, in English) under them. Its texts are in all 11 languages, like the rest of the page. While `waiting` is `other:<process>` the banner says MaxPack only installs and that the page
opens at `http://<console IP>:8095`. `state.images` is listed under the library grid ("for now, with
ShadowMountPlus"). `tools/mock_ui.js` (Node, development only) serves the page with fixed data, behind the same
Host and Origin checks as the payload (with a hidden game, a recorded crash, Play, Hide, the report, a queued
rebuild, a plain folder game with its overlay off, a pkg game in the fakelib mode Off, the Excluded paths, an install that does not fit on the internal SSD,
a `.p5z` that grew with an ampr-emu-ng too old for it (`old_ng`), `--last grown` for the installer's `grown_p5z`, and, with `--update`, a newer release on offer whose Update ends as the switch `--update-fail` says; "Update
ampr-emu-ng now" answers `tls_unchecked` until `--tls passed`, then installs, or with `--ampr-fail checksum`
refuses, or with `--ampr-fail older` refuses an older release).

## Library page (`src/ui.html`)
Over the grid: the type chips (L2/R2), the drive pills and a search box. A drive pill's body is a filter button:
pressed, the pill turns white and the grid shows only that drive's games (`drive`); pressed again, all; a drive that
goes stops filtering; Unmount stays in the pill. The search box keeps the cards whose name or title id has every word
of the query, case and accents ignored (`/` focuses it, Escape clears it). Hidden games (`hidden`) are left out of the
grid and the chip counts; once one is hidden, a "Show hidden (n)" switch shows beside the search box, and on it lists
them dimmed with "Hidden" on their badge (a per-browser choice, `localStorage` `maxpack.showHidden`). The empty state
says which filter left nothing. The game sheet's buttons: Play (`POST /api/launch`: a ready title while no game runs
and MaxPack mounts; after a launch it stays off up to 10 s, until the watcher reports the game), Rebuild cache (off while
`rebuild_queued` or pending, checking or building), Copy report (the report is fetched when the sheet opens and when
the game's status, crash, hidden, overlay state or fakelib mode changes or it starts or stops running, so a run that left no dump
still has its exit lines, and the copy happens inside the press: `execCommand('copy')` on the
sheet's text box, else the clipboard API; when neither can, the text stays shown and selected), Hide / Show in the
library (`POST /api/hidden`), and Delete set apart. A recorded crash adds "Last crash" to the fields and a hint.
Under the PlayGo switch, every library copy (not a duplicate) has "Fakelib", a select of Full, Emulators only and Off
(`POST /api/fakelib`, `fakelib_mode`; off while the game builds or the change is sent; a failed change shows its error
and the select goes back to the mode in the state), with one hint: if the game shows a black screen or stalls while
checking add-ons, try "Emulators only", then "Off". A plain folder game gets one more line: Off mounts nothing at
its launch (no `/data`, USB, fonts or writable `/app0` in its sandbox). For an `overlay_off` game the select is
greyed (disabled) with a note, since `overlay_off` wins. In Off, the PlayGo switch is greyed with a note that it has
no effect (it keeps its value for when the mode changes). Disabled switches and selects in the sheet take the
buttons' disabled look (opacity 0.5). A select keeps the row's look and the D-pad's list of the sheet's
controls (arrows move between controls; the PS5 browser opens its own picker).
Under it, a plain folder game (kind `folder`, packing `plain`; not a duplicate) has "MaxPack libraries
at launch" (`POST /api/overlay`, `overlay_off`): on by default; off, the sheet notes that the game runs as registered.
A pkg, a `.p5z` or a packed folder does not show it (the payload refuses turning it off for them), except while the
title is still listed in `overlay_off` from an earlier copy (`overlay_off_listed`: packed since, or a pkg or `.p5z` took
over): then the switch shows off with a note that this copy keeps the libraries, so the stale entry can be cleared
(turned on, the row goes). The entry is never dropped by itself: a plain folder copy coming back would use it again.
The recommended settings: while `settings_source` is "recommended", "Recommended setting" shows as a tag beside the
"Fakelib" label when `recommended.fakelib` is set and beside "PlayGo stub" when `recommended.playgo_off` is set (never
beside a value the list leaves at its default); a game with a `notice` shows its text above the sheet's fields; once the user chose
the settings (`settings_source` "user") and the list recommends something for the game (`recommended` not null), "Back
to the recommended setting" (`POST /api/recommended/<TID>`, body `reset`) shows under the Fakelib row, with a line
under it that says what it sets (`resetWhat`: the list's Fakelib mode, Full when it sets none, and the PlayGo stub
when the sheet shows that switch and the mode is not Off), and is in the D-pad's list of the sheet's controls only
while it shows. A hand change draws the settings as the user's until the
next state; the report is fetched again when `settings_source` or the stub changes. Settings has "Excluded paths"
under "Scan paths", one path per line (`exclude`), and "Download updated recommended settings"
(`recommended_update`) under the ampr-emu-ng update.
The sheet's error line says in the page's language why a `.p5z` short of space (`no_space`) or a `.p5z` that grew with
an ampr-emu-ng too old for it (`old_ng`: what to do, restart MaxPack or make a compact copy on the PC) is not built; any
other error shows the payload's English text.
Every text of the page is in all 11 languages: `tools/check_i18n.js` (run by CI) fails when a language lacks a key, has
an extra one, changes a `{placeholder}` or keeps the English text.

## Layout
| Path | What |
|---|---|
| `src/main.rs` | start-up, scan loop (pauses while a game runs), self copy, the bundled ampr-emu-ng and its updater |
| `src/config.rs` | `/data/maxpack/config.toml` settings |
| `src/storage.rs` | drives and default scan roots |
| `src/scan.rs` | game discovery, backports, duplicates |
| `src/cache.rs` | per-game cache `/data/maxpack/caches/<TID>`, freshness, `.built`, purge |
| `src/ampr_index.rs` | writes `ampr_emu.index` + `ampr_assets.index` for pkg caches |
| `src/register.rs` | home-screen registration, `/system_ex/app/<TID>` mounts, the tile |
| `src/launch.rs` | launch watcher: the fakelib overlay, writable `/app0`, `/app0/kraken` or `/app0/p5z`, the console fonts |
| `src/crash.rs` | a game's crash dump at its exit, the last crash per title in `/data/maxpack/crashes.json` |
| `src/p5z.rs` | the engine's `mp_play_*` (describe a `.p5z`, extract its loose files) |
| `src/web.rs`, `src/ui.html` | HTTP server and the single-file UI |
| `src/installer.rs`, `ps5/installer.c` | the phase-2 installer (MaxPack engine in C) behind `/api/inst/*` |
| `src/sys.rs` | log, toasts, mount syscalls, kstuff probe |
| `src/net.rs` | HTTPS GET over the console's sceHttp/sceSsl with explicit certificate checks (ampr-emu-ng updater, self-update, recommended settings) and the self-test's HEAD probe |
| `src/tls.rs` | the secure-connection self-test (`POST /api/selftest/tls`) the self-update and the ampr-emu-ng update wait for |
| `src/update.rs` | self-update: the latest release looked at once per start, `POST /api/self_update` |
| `src/recommended.rs`, `data/recommended.json` | the recommended per-game settings: the bundled list, the cached download, the once-per-start refresh, the effective settings, the notice at launch |
| `src/i18n.rs` | toast texts in the console language (the UI's 11 languages) |
| `crates/maxpack-logic` | pure logic tested on the host (`cargo test -p maxpack-logic`) |
| `crates/kraken-mount-pkg` | FPKG reader and its Kraken decoder (`decoder` module) |
| `vendor/maxpack` | copy of the MaxPack engine sources (with zstd and zlib) the payload links |
| `vendor/ampr-emu-ng`, `vendor/dlc_emu`, `vendor/pgo_stub` | the bundled sprx files, each with `VERSION` and `SHA256SUMS` |
| `ps5/` | Docker build, linker, C shims (`compat.c` holds PS5 libc adapters, `appdb.c` writes app.db through the SQLite amalgamation the Dockerfile fetches), `make-payloads-json.sh` (the release's catalog for ps5-payload-manager) |
| `tools/` | `check_identity.sh` (CI), `sync_maxpack.sh`, `mock_ui.js` (the page without a console), `recommended_key.js` (a title id's key in `data/recommended.json`) |

## Vendored components
- ampr-emu-ng is its own project (DeLFuS77/ampr-emu-ng) and is not built here. To update the bundled copy, replace
  `vendor/ampr-emu-ng/libSceAmpr.sprx`, `SHA256SUMS` and `VERSION` (ending in `-maxpack.N`) from its build, and
  `SOURCE` (repository, commit, CI run) and the Corresponding Source link in `THIRD_PARTY_NOTICES`. A file put into
  `/data/maxpack/ampr/` by hand overrides it on the console. Changes to ampr-emu-ng itself go to its repository.
  A CI build of a branch there may be bundled before its release, as `v0.5.1-maxpack.2` was while it was tested
  (`VERSION` then holds the name of the coming release); `SOURCE` and `THIRD_PARTY_NOTICES` say so, and the
  release's `libSceAmpr.sprx` and `SHA256SUMS` replace it once it is published. `VERSION` is what
  the console compares (the update of ampr-emu-ng, `ampr.bundled`, `maxpack_logic::p5z::GROWN_NG`):
  `maxpack_logic::version::parse_version` orders by X.Y.Z, then by the `-maxpack.N` revision (none = 0, upstream's
  own build), so a new MaxPack build of the same upstream version takes the next N (the repository's rule,
  `v<upstream version>-maxpack.<n>`), and its crates keep upstream's version, which is what the sprx's identity and
  launch notice say. A test (`p5z::tests::the_bundled_ampr_emu_ng_plays_a_grown_p5z`) fails when the bundled one
  is older than `GROWN_NG`.
- `vendor/dlc_emu/` and `vendor/pgo_stub/` are drakmor's released binaries (DLC and PlayGo emulators, added to a
  fakelib only when missing); to update, replace the sprx files, `SHA256SUMS` and `VERSION` from the new GitHub
  release.
- `vendor/maxpack` is a copy of the MaxPack engine (C, its own repository), not a submodule: the build needs nothing
  but this repository, and the copy pins the exact sources. Refresh it with
  `tools/sync_maxpack.sh <maxpack checkout> <commit>`: it writes `vendor/maxpack/COMMIT`, and `COMMIT.tag` from the
  tag on that commit (tag it first, `motor-consola-N`, so a rebase there cannot orphan it). It is never edited here.
- References, read only: ShadowMountPlus (SMP, the C original of the register, fakelib and backport logic) and
  ps5-dump-forge (toolchain, self copy, tile, toasts, UI patterns).

## Build and check
- Fast check on the host, no Docker: `cargo check --workspace --locked --target x86_64-unknown-freebsd`, with
  0 errors and 0 warnings (CI sets `RUSTFLAGS=-Dwarnings`).
- Payload: `ps5/build.sh` (Docker, two stages: stage 1 is the saved copy, stage 2 embeds it) ->
  `target/ps5/maxpack.elf`. A link error like `undefined symbol: X@FBSD_1.0` means the PS5 libc lacks that
  versioned symbol: add an adapter in `ps5/compat.c` (see `statfs`, `getmntinfo`).
- The payload crate builds only for the PS5/FreeBSD target, so it has no `cargo test`. Pure logic goes in
  `crates/maxpack-logic` (`cargo test -p maxpack-logic`, any host), tests first. Logic that cannot move there is
  checked by copying its pure functions into a scratch file, compiled with `rustc` on the host and asserted on a fake
  directory (done for the cache rebuild logic).
- UI script: CI extracts the `<script>` of `src/ui.html` and runs `node --check` on it.
- `tools/check_identity.sh` (CI) fails if the old name or paths come back.
- The page by eye, without a console: `node tools/mock_ui.js` (fixed data; switches at the top of the file).

## Rules for changes
- Naming: "MaxPack" in anything the user sees (UI, toasts, tile); `maxpack` for identifiers and paths. The MaxPack
  paths on the PS5 are fixed (`/data/maxpack/...`, `/user/data/maxpack/...`, tile `MXPK00001`, port 8095, ELF
  `maxpack.elf`): never rename them. What keeps the old name is the ampr-emu-ng contract (`kraken.image`,
  `/app0/kraken/`, `kraken.pkg`, `kraken_zero.dat`, the `ampr-emu-ng X.Y.Z` search) and the internal crate
  `kraken-mount-pkg` (kept so merges from upstream stay easy).
- Version: MaxPack's own, in `Cargo.toml`. 1.0.0 is the first published version (stage 1, mount, and stage 2, play
  from the `.p5z`); 0.1.x and 0.2.x were the stage 1 and stage 2 test builds. A release is tagged `v<version>` (the
  release job checks it against `Cargo.toml`). The upstream project's numbers (0.3.1 is the base) are never taken
  when merging from it.
- This file is the contract. Change it together with the code, and keep the module interface names.
- Mount safety: never `MNT_FORCE` from register or purge; a busy mount means a running game. The scan loop must not
  build, register or purge while a game runs (`running`, from the launch watcher).
- `/app0` is writable. Game-written files in a cache survive Rebuild (`.built`); only Purge deletes them.
- Old-layout caches (`/data/maxpack/<TID>`) are deleted by hand: there is no cleanup code for them.
- Pkg-specific behaviour in ampr-emu-ng goes behind `pkg_mode()`; zstd and LZ4 behaviour stays unchanged. A pkg
  performance change needs an A/B launch of at least two pkg games, with the numbers.
- Licenses: a dependency added or bumped in `Cargo.lock` gets its license text in `THIRD_PARTY_NOTICES`.
- Style: the smallest correct diff, no new dependencies, a `ponytail:` comment on each deliberate shortcut and its
  limits, errors logged with the title id and the path.
- UI: plain JS and CSS in one file that works in the PS5 browser (old WebKit, D-pad, 1920x1080) and on phones, with
  no external requests.
