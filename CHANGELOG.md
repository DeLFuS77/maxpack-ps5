# Changelog

What changed in each version of MaxPack for the console. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the versions are those of `Cargo.toml`.

## 1.0.1 - 2026-11-24

### Security

- **The ampr-emu-ng update is checked like MaxPack's own self-update.** Every game loads the
  `libSceAmpr.sprx` it installs, but it only checked the file's size and that it is a module. Now the
  update (at start with "Auto-update ampr-emu-ng", and Settings > "Update ampr-emu-ng now") also downloads
  the release's `SHA256SUMS` and installs `libSceAmpr.sprx` only when its SHA-256 is the one listed for
  it. A missing, malformed or different `SHA256SUMS` installs nothing: the log says why and a notice
  says it was not installed.
- **It waits for "Check secure connections".** As the self-update, the ampr-emu-ng update downloads
  nothing until that check has passed once on the console; Settings says so under ampr-emu-ng, and
  "Update ampr-emu-ng now" answers with the self-update's message.

### Changed

- The [compatibility list](docs/compatibility.md) row in "Copy report" carries no game name, title id
  or game version, since the list names no game: it follows the list's new columns (Original → `.p5z`,
  Format, Volumes, Drive, Firmware, Loader/mounter, Fakelib mode, Result, Date, Notes), in English.

### Fixed

- The bug report form: its "Other mounters running at the same time" list offered "None", an option
  name GitHub's issue forms reserve; it is now "Nothing else".

## 1.0.0 - 2026-11-24

The first public release of MaxPack for the console. It is based on
[Kraken Mount](https://github.com/quer3q/kraken-mount) 0.3.1 by quer3q, keeps its history and its
licence, and replaces the earlier test builds (0.1.x and 0.2.x).

### Fakelib per game

- **Fakelib** per game, in the game's sheet: **Full** (the default), **Emulators only** (none of the libraries the
  dump or the backport ships in `fakelib/`, only the bundled emulators the game needs and ampr-emu-ng) or **Off**
  (only ampr-emu-ng, and only for a `.pkg`, a `.p5z` or a packed folder, which cannot run without it; on an unpacked
  folder MaxPack then mounts nothing at launch, as with "MaxPack libraries at launch" off). For a game
  that shows a black screen or stalls while checking add-ons, with no files to delete over FTP. Stored in
  `config.toml` as `fakelib_emulators` and `fakelib_off`; `POST /api/fakelib/<TID>`; only that game's `fakelib/` is
  recomposed, and existing caches are not refreshed by the update.

### Mounting, like Kraken Mount

- Finds games on the internal SSD, the M.2 drive and USB drives, registers them on the home screen and
  mounts them when they start: debug FPKG (`.pkg`) and game folders, unpacked or packed with zstd or
  LZ4.
- A cache per game, ampr-emu-ng, the DLC and PlayGo emulators, backports, moved games, removing the
  ones that are gone, ejecting a USB drive, the notifications and the settings, as in Kraken Mount.
- Games that start an `*.elf` on exit, the page coming back by itself after rest mode, the icon on
  FW 12+, unexpected closes with their report, and the console fonts inside the game.

### Playing from the `.p5z`

- A MaxPack `.p5z` in a scan root shows up as one more game, without installing it: only its loose
  files are extracted to the internal SSD, and the volumes are read from the `.p5z` itself.
- Damaged files and games that do not fit are reported with a clear message instead of failing
  silently.
- It runs with our own build of ampr-emu-ng (`v0.5.1-maxpack.1`), the only one that knows this mode.

### The installer

- The Install tab installs `.p5z` files, game folders and ShadowMountPlus images, with a space check,
  resume, verify, replace with confirmation and uninstall.
- "Installer only" mode while ShadowMountPlus or Kraken Mount is running, and adoption of the games
  they registered, without duplicates. Nothing is written in their data folders.

### The page

- Play from a game's sheet (from a phone too), search, a drive filter, hiding games from the library,
  "Rebuild cache" disabled until it ends, "Excluded paths", and "MaxPack libraries at launch" per game.
- "Copy report" gives a text for an issue with the firmware, the last unexpected close and a row for
  the [compatibility list](docs/compatibility.md).
- The interface and the notices come in the console's language.
- Protections: it never registers or mounts a title installed on the console, never scans system or
  data folders, and port 8095 answers only the page opened at the console's IP (or `localhost`).

### Self-update

- At start MaxPack looks at the latest release of this repository and the page offers it; **Update**
  downloads `maxpack.elf`, checks it against the release's `SHA256SUMS` and hands the port over to the
  new copy.
- Nothing is downloaded or installed until Settings > "Check secure connections" has passed once on the
  console: it must refuse an untrusted root, a wrong host and an expired certificate, and still reach
  badssl.com and GitHub.
- Each release carries a `payloads.json` catalog for
  [ps5-payload-manager](https://github.com/itsPLK/ps5-payload-manager).

### Credits

[quer3q](https://github.com/quer3q) (Kraken Mount, ampr-emu-ng, ps5-dump-forge),
[drakmor](https://github.com/drakmor) (ShadowMountPlus, dlc_emu, pgo_stub) and the
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) authors. See the README and
`THIRD_PARTY_NOTICES`.

### Legal

MaxPack is for backups of games you own. It includes no games, firmware, keys or other copyrighted
content, does not help to obtain them and does not bypass encryption, keys or DRM; piracy is not
supported. It is provided "as is", without warranty, under the GPL-3.0-or-later: use it at your own
risk. The authors are not responsible for data loss, lost saves, damage to the console or storage,
bans or any other problem. MaxPack is not affiliated with or endorsed by Sony Interactive
Entertainment. Full text: [Legal and disclaimer](README.md#legal-and-disclaimer).
