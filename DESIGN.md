# MaxPack design

A Rust PS5 ELF payload (`src/`). It finds games on the internal SSD and on USB/extended drives,
keeps a per-game cache on the internal SSD, registers each game on the home screen and, at
launch, overlays the game's fakelib and the shared ampr-emu-ng dir on the sandbox's system libs.
The ampr-emu-ng runtime is NOT built here: our own build of it (DeLFuS77/ampr-emu-ng: quer3q's v0.5.1, with its pkg mode, plus our p5z mode; now `v0.5.1-maxpack.1`; its CI
run and commit are in `vendor/ampr-emu-ng/SOURCE`) is bundled from `vendor/ampr-emu-ng/` (`VERSION`, which ends in
`-maxpack.N`; `SHA256SUMS` copied from that CI run, checked by `ps5/build.sh`) and installed into `/data/maxpack/ampr/` at start.
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
| `exclude` | [] | array of absolute paths the scan never opens (`maxpack_logic::exclude`: no control character; `//`, `.` and `..` resolved as text, trailing `/` trimmed, duplicates dropped): a source at an entry or under it (whole path components, `scan::excluded`) is skipped by the scan before duplicate detection, never listed or descended into (an excluded root is not even listed): not in games, duplicates, errors or unstable, never built or registered. A title still registered stays registered (as ShadowMountPlus removing a manual source) with its built cache, whose overlay its launch still gets; auto-remove leaves it too, since its source is still there. Edited in the settings form ("Excluded paths"); a source excluded while a pass runs is skipped before its build or register. Ported from the upstream project's 0.3.2 |
| `inject_ampr` | true | copy `/data/maxpack/ampr/*` on top of a folder game's fakelib; pkg games always get it (pkg mode exists only in ampr-emu-ng) (a change only refreshes `fakelib/`) |
| `add_emulators` | true | add the bundled DLC (dlc_emu) and PlayGo (pgo_stub) emulators to a game's fakelib when it needs them and ships none (a change only refreshes `fakelib/`) |
| `playgo_off` | [] | array of title ids (`playgo_off = ["PPSA01556"]`) that get no bundled PlayGo stub: pgo_stub is neither added nor upgraded, a `libScePlayGo.sprx` the game or backport ships stays as shipped; the dlc_emu set is unaffected. Per game from the UI (`POST /api/playgo/<TID>`); a change only refreshes `fakelib/` |
| `hidden` | [] | array of title ids the page's library does not show unless its "show hidden" switch is on (a per-browser choice). Page only: a hidden game is still built, registered, mounted and on the home screen, and `/api/state` still lists it (`hidden: true`). Per game from the game sheet (`POST /api/hidden/<TID>`); the settings form keeps the list |
| `overlay_off` | [] | array of title ids whose cache gets an empty `fakelib/`, so the launch watcher mounts nothing in their sandbox: no overlay (not the game's or backport's fakelib, not ampr-emu-ng, not the emulators), no `/data`, USB, fonts or writable `/app0` either; the title runs as registered (for a game that must not get MaxPack's libraries, such as a Playden title shell). Plain folder games only (`maxpack_logic::overlay`): a pkg is read through ampr-emu-ng's pkg mode, a `.p5z` only through our ampr-emu-ng's p5z mode, and a zstd or LZ4 folder's packs only through ampr-emu-ng, all of which come in that overlay; so the web refuses it for them (409) and a listed title whose library copy is one of those (packed since, or a pkg or `.p5z` copy took over) keeps its fakelib. Per game from the game sheet (`POST /api/overlay/<TID>`); the settings form keeps the list; a change only refreshes `fakelib/`. Ported from the upstream project's 0.3.2, with the `.p5z` and packed folder refusals ours |
| `ampr_auto_update` | true | once per start, download a newer ampr-emu-ng release from GitHub (a hand-placed `libSceAmpr.sprx` is kept) |
| `backports` | true | apply `<scan root>/backports/<TID>*` |
| `scan_interval_seconds` | 10 | scan loop period (1..3600) |
| `stability_wait_seconds` | 10 | skip a source changed less than this many seconds ago (still being copied) (0..3600, 0 = off) |
| `low_space_gb` | 20 | warn when `/data` free space is below this |
| `auto_remove_missing` | true | remove a title from the home screen when its source stays missing for `auto_remove_delay_seconds` (the cache stays; see Flow) |
| `auto_remove_delay_seconds` | 300 | how long a source must stay missing before `auto_remove_missing` removes its title (1..86400) |
| `notify` | true | system toasts |
| `log` | true | write `/data/maxpack/maxpack.log` (stdout always gets the lines) |
| `debug` | false | klog capture to `/data/maxpack/klog.txt` and the `/file/data/...` route |

Invalid values are logged and replaced by the default; a string item of an array (`scanpath`, `exclude`, `playgo_off`, `hidden`, `overlay_off`) that is not
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
  contract, ampr-emu-ng's `docs/p5z-mode.md`; both sides test the same golden text, `"maxpack-image 1\np5z/Marvel’s
  Spider-Man™ 2 [PPSA01234].p5z\n0 1234 5678\n"`): a name that is not UTF-8 or breaks those rules, or a volume list
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
  `<TID>: fakelib: PlayGo stub off (playgo_off)`; `fakelib/` starts from scratch, so a stub added earlier is gone.
  A plain folder title in `overlay_off` (`cache::overlay_off`) stops right after `fakelib/` is emptied (no base, no
  emulators, no ampr), logged `<TID>: fakelib: overlay off (overlay_off)`; `.ampr` is written with `"overlay_off": true`
  and the earlier "playgo" memo kept. Launch then mounts nothing (empty `fakelib/`).
  Then, with `inject_ampr` and always for a pkg game (the package's own `libSceAmpr.sprx` cannot read the
  `.pkg` named in `kraken.image`) or a .p5z game (a game's own ng has no p5z mode, so `keep_game_ng` does not apply:
  ours is always on top), every file of `/data/maxpack/ampr/` (not `libkernel.sprx`) is copied on top,
  so ours wins over the game's own `libSceAmpr.sprx`, except a game's or backport's ampr-emu-ng strictly newer than
  ours (the `ampr-emu-ng X.Y.Z` identity in both files; ours without one counts as older), which is kept and logged
  `<TID>: fakelib: kept the game's ampr-emu-ng <v> (newer than <v>)` (no ng downgrade, also over a hand-placed copy). `.ampr` records what fakelib was composed from: `{"ampr": {name: [size, mtime]} or null,
  "emus": null without `add_emulators`, else the dlc_emu and pgo_stub versions, whether a `dlc_emu.ini` is on
  disk (folder or backport root), the eboot's [size, mtime] and "playgo_off" (the title is in `playgo_off`), "playgo": [eboot size, mtime secs, imports PlayGo]}`
  (a pkg's own `dlc_emu.ini` is not in it, a pkg change rebuilds through the stamp). "playgo" memoizes the PlayGo
  import search: `eboot.bin` is read again only when its (size, mtime) differs (a read error is not memoized), and
  `check` never compares it. An older `.ampr` without `emus` differs, so every cache refreshes `fakelib/` once; that
  one-time refresh also drops a `libSceSaveData.native.sprx` an older build copied (no separate check for such files).
  `"overlay_off": true` is in `.ampr` only for a title `cache::overlay_off` takes (an older `.ampr` stays fresh).
  When "ampr", "emus" or "overlay_off" differ (the ampr dir, `inject_ampr`, the emulator inputs or `overlay_off` changed) or `.done` still has a
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
copy; a file put there by hand is kept; see Auto-update below), ampr chmod, web thread, klog thread when `debug`, then wait
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
Auto-update (ampr-emu-ng): at start, when `ampr_auto_update` is on (whatever `inject_ampr` says: pkg games always use the ampr dir), a background thread asks
`https://api.github.com/repos/DeLFuS77/ampr-emu-ng/releases/latest` (stable releases only; betas are
pre-releases and skipped). When its `tag_name` is newer than the installed version, it downloads the
asset `libSceAmpr.sprx` (size must equal the asset size, max 16 MiB, ELF or SELF magic), writes a temp
file in `/data/maxpack`, chmod 0777, renames it to `/data/maxpack/ampr/libSceAmpr.sprx`, records
it in `ampr.bundled` as `<tag> <len> <crc32>` and toasts `i18n::ampr_updated`. A copy we wrote (bundled
or downloaded) is ours: the bundled copy replaces ours only when the bundled version is newer; a
hand-placed file is kept by both, and ownership is checked again after the download. Network errors are
logged and retried up to 3 times 20 s apart, then nothing until the next start. An install wakes the scan
loop (fakelib refresh); one update runs at a time. Settings has "Update ampr-emu-ng now"
(`POST /api/ampr/update`): forced, one attempt, installs the latest stable release whatever is
installed; a hand-placed file is first moved to `/data/maxpack/libSceAmpr.sprx.hand` (one backup,
outside `ampr/`). HTTPS goes through the console's sceHttp/sceSsl with 4 built-in root CAs
(from SMP).
Running game: while `launch::watch` reports a live session (`running`), the loop does no
scan/build/register/purge (a game starting mid-scan stops the scan before the next title);
rescan/rebuild/purge requests stay queued and run after the game exits (2 s poll).
Toasts: `sceNotificationSend` system toast (dump-forge/SMP JSON, icon `/user/data/maxpack/notify-icon.png`),
the old `sceKernelSendNotificationRequest` only as fallback. Toast texts (`src/i18n.rs`) follow the console language
(`sceSystemServiceParamGetInt` lang, read per toast; dump-forge's 11: en ru zh de es fr ja pt it ko ar), English when it
cannot be read; "MaxPack", title names and paths stay as they are. A failed game is retried
only when its inputs change (`main::inputs_key`: the stamp, the input mtimes, its `playgo_off` membership and
`cache::overlay_off`) or a rebuild is forced (not a `.p5z` build short of space, `cache::NoSpace`, or whose
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
still mounted. A later copy of the payload adopts the overlay, `/data`, USB, font and `/app0/kraken` or `/app0/p5z` mounts.
When a session's cleanup ends, `register::spawn_snd0info(<TID>, 0)` (own thread).
No overlay when neither layer applies, and with an empty `fakelib/` (an `overlay_off` title) no sandbox mount at all.

## Crash report (`src/crash.rs`, `maxpack_logic::crash`)
When an app crashes, the console writes `/user/devlog/system/sce_coredumps.0/<TID>_<n>/` with
`prosperocore-<n>-0x<pid>-<process>.prosperodmp` inside, and deletes the folder seconds later. No thread or watch of
its own: the launch watcher's NOTE_EXIT for a game it mounted (an exit-spawned ELF included) logs the exit status
from the event's data (a wait(2) status) on the `exit pid=...` line (`exited with N`, `killed by signal N`), then,
once the sandbox's nullfs mounts are gone (they must go first, see Launch), reads that folder once for `<TID>_*`
folders holding a dump with that pid. A launch it did not overlay (another mounter ran it, or its `fakelib/` is
empty: `overlay_off`) gets the exit line only: no crash is recorded for it, since the page would blame our dump, ng or
PlayGo stub. If none is there yet, it looks once more when that session's cleanup ends (0.4 to 3 s later, or when a
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
  pub playgo_off: Vec<String>, pub hidden: Vec<String>, pub overlay_off: Vec<String>, pub ampr_auto_update: bool,
  pub backports: bool, pub scan_interval_secs: u64, pub stability_wait_secs: u64, pub low_space_gb: u64,
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
pub fn excluded(path: &std::path::Path, exclude: &[String]) -> bool; // path is an exclude entry or under one (whole components)
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
pub fn check(src: &Source, s: &Settings) -> Fresh;
pub struct NoSpace { pub missing: u64 /* margin included */ }       // a .p5z build stopped at its space check (inside an io::Error)
pub fn missing_bytes(e: &std::io::Error) -> Option<u64>;           // Some(missing) when e is a NoSpace
pub struct TryAgain(String);                                        // a .p5z build or refresh stopped at the engine's open, tried again next pass (inside an io::Error)
pub fn try_again(e: &std::io::Error) -> bool;                      // e is a TryAgain
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
pub fn registered(tids: &[&str]) -> Vec<bool>;            // is_registered for many, in tids order, one app.db read (all false when unreadable)
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
pub(crate) fn parse_version(v: &str) -> Option<(u32, u32, u32)>; // "X.Y.Z", leading "v" allowed
pub(crate) fn newer(a: Option<&str>, b: Option<&str>) -> bool;   // a strictly newer than b; an unknown version is older than any

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
```

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
  requiring `application/json` on every POST: the page's text bodies (`delete`, `eject`, `playgo`, `quit`), the
  loopback quit of a newer copy and older copies' quit requests keep working unchanged.
- `GET /` ui.html (`__VERSION__` filled in); `GET /app-icon.png` the app icon (`ps5/icon0.png`);
  `GET /maxpack.appcache` the AppCache manifest (version + page hash). These three are `no-cache`,
  the rest `no-store`.
- `GET /api/state`:
  `{version, busy, paused, waiting:"kstuff"|"other:<process>"|null, running:[TID], images:[{path, kind:"ffpkg"|"exfat"|"ffpfs"|"ffpfsc", title_id?}] (ShadowMountPlus images found where a source could be: never mounted; `title_id` from a `[TITLE_ID]` in the file name, absent otherwise), games:[{title_id, title_name, version, path, kind:"pkg"|"folder"|"p5z", packing:"plain"|"zstd"|"lz4"|null,
  drive, size, status:"pending"|"checking"|"building"|"ready"|"error"|"duplicate", error, installed_copy (the path that
  shows this title id installed on the console, with status "error": MaxPack leaves it alone; else null), error_code ("no_space" for a .p5z that does not fit, with status "error"; else null), missing_bytes (bytes short, margin included, with `no_space`; else null), cache_size, cache_built, backport,
  playgo: bool|null (the memoized "eboot imports PlayGo", null when unknown or `add_emulators` is off), playgo_off: bool (false while `add_emulators` is off),
  hidden: bool (in the `hidden` setting: the page lists it only with "show hidden" on),
  crash: {at, process, status}|null (the last crash, from crashes.json, see Crash report),
  rebuild_queued: bool (a rebuild asked for, not started yet: the title is in `forced`; the page keeps Rebuild disabled through it and while the game is pending, checking or building),
  registered: bool (`register::registered`: the staged `param.json` files and the title id in app.db, one app.db read per request),
  mounted: bool (`/system_ex/app/<TID>` is a mount point, one mount table per request), source_present: bool (the source path exists),
  overlay_off: bool (listed in `overlay_off` and a plain folder game, as `cache::overlay_off`)}],
  unplayable:[{path, code:"damaged"|"too_many_volumes", detail}] (the `.p5z` files of the last scan that cannot be played: `detail` is the engine's reason, or the volume count; the page lists them under "Archivos .p5z que no se pueden jugar" and never as games),
  drives:[{mount, label, kind, total, free, ejectable}], cache:{path, size, free, low_space, ampr_present}, log:[last 100 lines of the last 64 KiB]}`
- `GET /api/settings` -> settings JSON; `POST /api/settings` (JSON body) -> save, apply, rescan; a body
  without `playgo_off`, `hidden`, `overlay_off` or `exclude` keeps that current list from the file (the settings form
  sends `exclude` only); 409 when `overlay_off` names a library game that needs the overlay (a pkg, a `.p5z` or a
  packed folder, `maxpack_logic::overlay::needed`, the reason as text). A `scanpath` entry that
  may never be a root (`maxpack_logic::roots::never_root`) is 400 JSON `{code:"forbidden_root", path, detail}`
  and nothing is saved; the page says it in its language.
- `POST /api/playgo/<TID>`, body exactly `on` or `off` -> removes the title from / adds it to `playgo_off`,
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
  version, source path, kind, drive, status and error, hidden, and "MaxPack overlay: off" for an `overlay_off` game), the backport, ampr-emu-ng (`/api/ampr` kind and
  version), the PlayGo stub (on, off, or not added), the last crash, a ready row for `docs/compatibilidad.md`
  (`compat_row`: result and date left as `?`, since the console's clock is often wrong; a `|` in the name escaped),
  and the last 40 lines of the log tail (64 KiB) that contain the title id; 404 for an invalid or absent library
  title. The game sheet's "Copy report" copies it.
- `GET /api/ampr` -> `{kind: "bundled"|"downloaded"|"hand"|"missing"|"error", version, updating}` of
  `ampr/libSceAmpr.sprx`; `POST /api/ampr/update` -> forced update, answered when done: 200 + result text,
  409 while one runs, 502 + error text.
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
  Before exiting, the installer is stopped in order (`installer::stop`); the same when another copy took the port.
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
there in `vendor/maxpack/COMMIT.tag`, now `motor-consola-3`, e9e0ec4: its `ampr_emu.index` reader takes a slot table aligned to 8 bytes, as other packers write it), with the zstd and
zlib its submodules pin. `ps5/build.sh` builds it with the SDK's `prospero-cmake -DMAXPACK_PS5=ON` into static
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
The page's **Install** tab is phase 2's page (`vendor/maxpack/web/index.html`) in this UI's style: the job with
progress, speed and time left (Pause), unfinished installs (Resume, Discard), what can be installed (icon,
saving or "copied as is", Search again, a path added by hand through `probe`), the destination drives with
their free space, Install (a replace asks first, listing the copies it deletes), and what MaxPack installed
(Verify, Uninstall). The free-space check before a copy (the only copy the page offers; it moves nothing):
each destination drive says whether the chosen game fits, as the engine's plan counts it (`install_size` plus
the engine's 256 MiB `space_margin` against the drive's free space, plus what an unfinished install of that title
already copied there), "Does not fit: N more needed" when not, and Install then says so without asking the engine;
the engine checks again when it plans the install (`no_space`). It polls `/api/inst/state` every second while open; the engine's error codes are shown in
the UI's words, its Spanish `detail` under them. Its texts are English and Spanish (the other languages fall
back to English). While `waiting` is `other:<process>` the banner says MaxPack only installs and that the page
opens at `http://<console IP>:8095`. `state.images` is listed under the library grid ("for now, with
ShadowMountPlus"). `tools/mock_ui.js` (Node, development only) serves the page with fixed data, behind the same
Host and Origin checks as the payload (with a hidden game, a recorded crash, Play, Hide, the report, a queued
rebuild, a plain folder game with its overlay off, the Excluded paths, and an install that does not fit on the internal SSD).

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
the game's status, crash, hidden or overlay state changes or it starts or stops running, so a run that left no dump
still has its exit lines, and the copy happens inside the press: `execCommand('copy')` on the
sheet's text box, else the clipboard API; when neither can, the text stays shown and selected), Hide / Show in the
library (`POST /api/hidden`), and Delete set apart. A recorded crash adds "Last crash" to the fields and a hint.
Under the PlayGo switch, a plain folder game (kind `folder`, packing `plain`; not a duplicate) has "MaxPack libraries
at launch" (`POST /api/overlay`, `overlay_off`): on by default; off, the sheet notes that the game runs as registered.
A pkg, a `.p5z` or a packed folder never shows it (the payload refuses it for them). Settings has "Excluded paths"
under "Scan paths", one path per line (`exclude`).
New texts are in English and Spanish; the other languages show the English ones.

## Build and check
`cargo check --target x86_64-unknown-freebsd` (fast, host). Full payload: `ps5/build.sh` (Docker).
