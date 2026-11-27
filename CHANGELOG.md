# Changelog

What changed in each version of MaxPack for the console. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the versions are those of `Cargo.toml`.

## Unreleased

### Added

- **Verify a `.p5z` on the console.** A `.p5z` game's sheet has **Verify quickly**, which reads the file at the
  speed of the drive and checks each volume and loose file against the SHA-256 it was written with, and
  **Verify fully**, which also unpacks every file and checks it, as `maxpack verify` does on the PC. The sheet
  keeps the result with its date: every file matches, or what is damaged and that it can be repaired on the PC
  with its repair data or copied again. **Copy report** says it too, without naming a file of the game. It
  waits while a game runs, an install runs or a game arrives from the PC.
- **Receive games from the PC.** With Settings > **Receive games from the PC** on, MaxPack for the PC sends a
  `.p5z` straight to a drive of the console (`maxpack send`), with the six-digit pairing code that only the
  console's own screen shows. A send cut short goes on where it was, also after a restart. The console checks
  what arrived with the quick verify: a `.p5z` that checks out shows up in the library, one that does not
  stays aside with what is wrong. A banner follows it on every screen, with **Stop** (keeps what arrived) and
  **Delete what arrived** (asks first); a send nobody finishes pauses after 10 minutes, and Settings lists the
  files sends left behind. Off by default, and only the console's own browser turns it on; wrong codes lock
  the sender out for a minute. MaxPack for the PC can delete what a send left without starting another, also
  on a full drive, and a refusal for space says how much of that game is already there. A send cancelled on
  the PC pauses on the console at once (one the PC could not end is let go after a minute without data when
  the PC deletes it), and a file name that differs only in case is the same file, as on exFAT. If what arrived
  cannot be deleted, the banner says why and where it stays.

### Fixed

- **"Back to the recommended setting"** says what it sets, under the button ("Sets Fakelib to "Off"."), so a
  game you set by hand before 1.0.2 shows what the list would change. With "Add the bundled emulators" off
  it no longer says it sets the PlayGo stub, which is then never added.
- A `.p5z` made by a newer MaxPack for the PC (a later index version) is listed, and announced, as made by a
  newer MaxPack for the PC, to be played once MaxPack for the console is updated, instead of "Damaged or
  incomplete"; Install no longer calls it damaged either.
- Install marks a `.p5z` that grew on the PC as not installable as soon as it is listed (play it from the
  `.p5z`, or make a compact copy on the PC), and Install refuses it at once, instead of only when its job
  starts. It tells from the file's index alone, so the list stays quick with many large `.p5z` files.
- A game whose `fakelib/` refresh turns into a rebuild (an ampr-emu-ng too old for a `.p5z` that grew)
  shows "Building", not "Checking".
- When an older ampr-emu-ng stops several `.p5z` games that grew at once, the console shows one notice with
  their count instead of one per game.
- A new list of recommended settings that arrives while a game's `fakelib/` is being made no longer leaves
  that `fakelib/` made with the old Fakelib mode but recorded as made with the new one; the next pass now
  makes it again with the new list.
- A download over its size limit (the recommended settings, the self-update's look, ampr-emu-ng) is not
  tried twice more, 20 s apart; the recommended settings log it as refused, not failed.
- A downloaded list of recommended settings that is not newer than the one in `maxpack.elf` (after a MaxPack
  update) is deleted, instead of a log line saying so at every start.
- A list of recommended settings that is refused quotes at most 40 characters of the key or value at fault
  in the log, so a malformed file cannot fill it.
- Settings > About and the README's credits name ProsperoPkgTool (pearlxcore), whose port the installer's
  engine carries since 1.0.2.
- The README said that a value of the recommended settings this version does not know is refused; it is
  ignored for that game, and only a file that does not check out is refused.
- **The page after another `maxpack.elf`.** The console's browser keeps the page, and after a different
  `maxpack.elf` was sent it showed the last page it had seen until MaxPack was opened again. From this version
  on, the new payload's page replaces it as soon as the browser has fetched it, also when the payload comes
  back while the page is open.

## 1.0.2 - 2026-11-26

### Added

- **Recommended settings per game.** MaxPack carries a list of the settings some games need to start, so
  they work the first time: a game known to need it gets its **Fakelib** mode (or its PlayGo stub) by
  itself, and its sheet says "Recommended setting" beside the value. The first entries are the two games
  tested on the console that needed it: one only reaches its menu with Fakelib **Off** (firmware 13.60 and
  later), the other stays on a black screen for minutes before it starts.
- **A notice for a game that is slow to start**: its sheet shows it, and the console shows it as a
  notification, in its language, when the game starts.
- **What you choose wins.** Changing Fakelib or the PlayGo stub in a game's sheet makes that game's
  settings yours (`manual_settings` in `config.toml`); **Back to the recommended setting** undoes it. A game
  you had set by hand in 1.0.1 keeps your setting.
- **The list stays current.** It comes inside `maxpack.elf`, and once per start, after "Check secure
  connections" has passed, MaxPack downloads the latest one from this repository: only data, checked
  whole before it is used, and used only when it is newer. Nothing about your games is sent. Settings >
  "Download updated recommended settings" (on by default) turns it off. The list names no game: each one
  is a code made from its title id (`tools/recommended_key.js`).
- "Copy report" says `(recommended)` after the Fakelib mode or the PlayGo stub that the list sets.
- **A `.p5z` that grew on the PC plays.** MaxPack for the PC can add a DLC to a `.p5z` already made
  (`maxpack add`), or turn it into a newer version of the game by compressing only what changed
  (`maxpack update`), by appending to the file. Such a `.p5z` shows up and plays like any other, from the
  drive it is on, with every volume it has, the old ones too.
- MaxPack carries ampr-emu-ng
  [v0.5.1-maxpack.2](https://github.com/DeLFuS77/ampr-emu-ng/releases/tag/v0.5.1-maxpack.2), whose p5z
  mode reads those files. Its launch notice says v0.5.1, upstream's version, as every MaxPack build of it
  does: Settings shows which build.

### Changed

- **Never half-way.** A `.p5z` that grew needs that ampr-emu-ng. When `/data/maxpack/ampr/` holds an older
  one (put there by hand), MaxPack does not start the game: its sheet says why and what to do (restart MaxPack to put back its own, deleting one you
  put there by hand, or make a compact copy on the PC with `maxpack compact`), and a notice names it once.
  A `.p5z` that never grew plays as before, with any.
- **The ampr-emu-ng update never installs an older release.** At start or with "Update ampr-emu-ng now", a
  release older than the ampr-emu-ng MaxPack carries, or than the one in use, is not installed, and Settings
  says why: a `.p5z` that grew would no longer start with it.
- **Install** says that a `.p5z` that grew cannot be installed: play it from the `.p5z`, or make a compact
  copy on the PC and install that.
- The installer is the MaxPack engine as it is now on the PC (it was the one of before the PC's 1.0.0):
  its messages under an error are in English. `THIRD_PARTY_NOTICES` lists ProsperoPkgTool (MIT), whose
  port the engine now carries.

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
