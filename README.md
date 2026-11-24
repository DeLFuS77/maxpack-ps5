<p align="center"><img src="ps5/icon0.png" width="128" alt="MaxPack"></p>

# MaxPack for the console

MaxPack is a PS5 game tool by DeLFuS77. This repository is its console part: a payload (an ELF
written in Rust) for a PS5 with a jailbreak that finds games on the internal SSD, the M.2 drive and
USB drives, registers them on the home screen and mounts them when they start, and also installs
them. It is based on [Kraken Mount](https://github.com/quer3q/kraken-mount) by quer3q, and keeps its
history and its licence.

MaxPack is made of three repositories:

- **[DeLFuS77/maxpack](https://github.com/DeLFuS77/maxpack)**: the PC app and command line, which
  pack a game into a `.p5z` or a zstd folder, and the full
  [user guide](https://github.com/DeLFuS77/maxpack/blob/main/docs/guide.md).
- **[DeLFuS77/maxpack-ps5](https://github.com/DeLFuS77/maxpack-ps5)**: this repository, the console
  payload.
- **[DeLFuS77/ampr-emu-ng](https://github.com/DeLFuS77/ampr-emu-ng)**: MaxPack bundles its own build
  of it (quer3q's ampr-emu-ng plus the mode that reads a game from inside its `.p5z`).

**Supported formats**: debug FPKG (`.pkg`), game folders (unpacked, or packed with zstd or LZ4) and
MaxPack `.p5z` files, which are played as they are, without installing them. The installer also
installs `.p5z` files and ShadowMountPlus images.

**Legal note.** MaxPack ships no games and nothing copyrighted, and it touches no keys, encryption or
DRM: it is meant for backups of games you own, dumped from your own console. You are responsible for
what you do with it. It is not affiliated with or endorsed by Sony.

## Download and start

1. Download `maxpack.elf` and `SHA256SUMS` from
   [Releases](https://github.com/DeLFuS77/maxpack-ps5/releases) and check the file
   (`sha256sum -c SHA256SUMS`).
2. With kstuff loaded, send it to the ELF loader (elfldr, port 9021), for example
   `nc -w 3 <ps5-ip> 9021 < maxpack.elf`, or with your payload loader.
3. MaxPack saves a copy of itself in `/data/maxpack/maxpack.elf` and adds a **MaxPack** tile to the
   home screen, which opens its page (and starts that copy when it is not running). The page also
   opens from a phone or a PC at `http://<ps5-ip>:8095`.
4. Put games in `/data/homebrew` (or `homebrew/` on a USB drive), or add scan paths in Settings.
   Each game shows up in the page and on the home screen once it is registered.

If ShadowMountPlus or Kraken Mount is running, MaxPack only installs until it is closed (see below).

## Updates

At start MaxPack looks at the latest release in this repository. When a newer one is out, the page
shows "MaxPack <version> is available"; **Update** downloads its `maxpack.elf`, checks it against
that release's `SHA256SUMS` (nothing is sent when it does not match) and sends it to elfldr, and the
new copy takes over. Close the running game first. While the repository has no published release,
nothing is offered.

Before its first update, MaxPack checks that the console refuses forged certificates: press
**Check secure connections** in Settings (or in the update notice). It must be refused by three test
sites with bad certificates at badssl.com and reach badssl.com and GitHub; until it has passed once,
no update is downloaded or installed.

To keep it updated with [ps5-payload-manager](https://github.com/itsPLK/ps5-payload-manager), add
`https://github.com/DeLFuS77/maxpack-ps5/releases/latest/download/payloads.json` as a custom
repository. Each published release carries that catalog (the `.elf` and its SHA-256, which the
manager checks on every download). The page's **Update** does not change the manager's copy: if
the manager starts MaxPack at boot, update it there too.

## Status

MaxPack 1.0.0 is the first public release. It has run on a PS5 (firmware 13.60); what has been tried, game by game, and
with what result is in the [compatibility list](docs/compatibility.md). The full checklist for a
console is [`docs/console-tests.md`](docs/console-tests.md); not every item of it has been checked
yet. Tried a game? Add it with a [game report](https://github.com/DeLFuS77/maxpack-ps5/issues/new?template=game_report.yml). Logic that needs no console is tested on the PC.

What Kraken Mount 0.3.1, its base, does, MaxPack does the same way: finding games in the usual
roots, a cache per game, registering on the home screen and mounting at launch, ampr-emu-ng, the
DLC and PlayGo emulators, backports, moved games, removing the ones that are gone, ejecting a USB
drive, the notifications and the settings. From what Kraken Mount added later it brings: games that
start an `*.elf` on exit, the page coming back by itself after rest mode, the icon on FW 12+, the
Play button, unexpected closes with their report, search, the drive filter and "Rebuild cache"
disabled until it ends. From its 0.3.2: the console fonts inside the game, "Excluded paths" (in
Settings: the scan never opens them), the self-update with `payloads.json`, and, in the sheet of an
unpacked folder game, "MaxPack libraries at launch", which can be turned off for a game that must
run as it is (a `.pkg`, a `.p5z` or a zstd or LZ4 folder cannot run without them).

What MaxPack adds:
- **The installer** (Install tab), MaxPack's phase-2 installer with its C engine inside the payload:
  it installs `.p5z` files, folders and images, with a space check, resume, verify, replace with
  confirmation, and uninstall. It does not uninstall or replace while a game is open, and
  uninstalling a game MaxPack mounts also takes it off the home screen.
- **"Installer only" mode**: while ShadowMountPlus or Kraken Mount runs, MaxPack mounts nothing, but
  the page and the installer stay up (what it installs is registered in ShadowMountPlus, as in phase
  2). When the other one closes, MaxPack mounts again.
- **Adoption**: games that ShadowMountPlus or Kraken Mount registered are adopted without
  duplicates, and the first time the paths added by hand in them are imported. MaxPack writes
  nothing in `/data/krakenmount` or `/data/shadowmount`, so you can go back to them.
- **Playing from the `.p5z`**: a `.p5z` in a scan root (or one folder below) shows up as one more
  game, without installing it. Only its loose files are extracted to the internal SSD
  (`/data/maxpack/caches/<TID>`); the volumes are read from the `.p5z` itself, which is mounted
  read-only at `/app0/p5z` at launch, so the file has to stay where it is. It needs space for the
  loose files plus 256 MiB: if they do not fit, the notification says how much is missing, and the
  game mounts by itself once that is freed. A damaged `.p5z`, or one with more than 256 volumes, is
  not a game: it is listed under ".p5z files that cannot be played". The engine has to say "damaged"
  on two passes in a row over the same file for that: a short read failure (a USB drive that resets)
  is only retried, also while building its cache. If the same game is also there as a folder or a
  `.pkg`, that copy wins and the `.p5z` is listed as a duplicate. The file name may have accents, ™,
  ’ or Japanese, as MaxPack on the PC writes them; only control characters are refused. It always
  runs with our ampr-emu-ng, the only one that knows this mode.
- **The cache and the clock**: each cache records the size and date of the files it comes from (the
  `.pkg`, the `.p5z`, the backport, the folder's files) and is rebuilt only when one of them changes.
  They are not compared with the console's clock, which on an offline console is often years behind.
- **Our ampr-emu-ng**: the bundled one is our own build (`v0.5.1-maxpack.1`: quer3q's v0.5.1 with
  the `.p5z` mode), from [DeLFuS77/ampr-emu-ng](https://github.com/DeLFuS77/ampr-emu-ng)
  (`vendor/ampr-emu-ng/SOURCE`). Its automatic update is off until MaxPack is published.
- **The library page**: "Play" starts a ready game from its sheet (from a phone too), "Copy report"
  gives a text for an issue with the firmware, the last unexpected close and the row for the
  [compatibility list](docs/compatibility.md), and a game can be hidden from the library (it stays
  mounted and on the home screen; "Show hidden" shows it). Before installing, each drive says
  whether the game fits, and when it does not, the copy does not start.
- **Protections**: it never registers or mounts a title installed on the console, never scans
  system or data folders, and port 8095 answers only the page opened at the console's IP (or
  `localhost`), not other websites.

> **The phase-2 `maxpack.elf`.** It has the same name and port, but it cannot hand the port over to
> a new copy. If it is still in an autoload list it starts first, and this one closes with the
> notification "port 8095 busy". Remove it from those lists and restart the console first.

## Identity

- Binary: `maxpack.elf`; its own copy in `/data/maxpack/maxpack.elf`.
- Data in `/data/maxpack/`: `config.toml`, `maxpack.log`, `caches/<TID>`, `ampr/`, `crashes.json`,
  and the installer's (`state/`, `installed/`, `tmp/`).
- Home-screen tile: `MXPK00001`. Web page on port **8095**.
- Its own version, the one in `Cargo.toml` (the page and `/api/state` take it from there): 1.0.0 is
  the first published one, with both stages (mounting, and playing from the `.p5z`); 0.1.x and 0.2.x
  were the test builds. It does not follow Kraken Mount's numbering, whose 0.3.1 is the base.
- The part that talks to [ampr-emu-ng](https://github.com/DeLFuS77/ampr-emu-ng) keeps its names
  (`kraken.image`, `/app0/kraken/`, `kraken.pkg`, `kraken_zero.dat`), and the internal crate is
  still called `kraken-mount-pkg`, to keep merges from the original easy.

## Building

```
ps5/build.sh
```

It needs Docker. It builds in two stages (the first is the copy the tile starts, the second embeds
it) and leaves `target/ps5/maxpack.elf`. The Docker image is `maxpack-ps5` and uses the PS5 payload
SDK v0.45. Quick checks on the PC, without Docker:
`cargo check --workspace --locked --target x86_64-unknown-freebsd` and
`cargo test -p maxpack-logic`. In CI the `check` job also runs `tools/check_identity.sh`, which
fails if an old name or path comes back. How it is built, the rules for a change and the contract
between modules are in [DESIGN.md](DESIGN.md).

## Help and reports

Bug reports, game reports and ideas are welcome, in English or Spanish: use the
[issue templates](https://github.com/DeLFuS77/maxpack-ps5/issues/new/choose). For a security
problem, see [SECURITY.md](SECURITY.md). What changed in each version is in
[CHANGELOG.md](CHANGELOG.md).

## Credits

- [quer3q/kraken-mount](https://github.com/quer3q/kraken-mount): Kraken Mount, the base of this
  project.
- [quer3q/ampr-emu-ng](https://github.com/quer3q/ampr-emu-ng), by the same author: MaxPack bundles
  its own build of it, [DeLFuS77/ampr-emu-ng](https://github.com/DeLFuS77/ampr-emu-ng).
- [quer3q/ps5-dump-forge](https://github.com/quer3q/ps5-dump-forge): the Kraken decoder of
  `crates/kraken-mount-pkg` is ported from it.
- [drakmor/ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus): the ideas and code behind
  the home-screen registration, the `fakelib` overlay and the backports are ported from it.
- [drakmor/dlc_emu](https://github.com/drakmor/dlc_emu): the bundled DLC emulator.
- [drakmor/pgo_stub](https://github.com/drakmor/pgo_stub): the bundled PlayGo stub.
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk): the PS5 toolchain and libraries.
- [zstd](https://github.com/facebook/zstd) and [zlib](https://zlib.net), in the installer's engine,
  and [SQLite](https://www.sqlite.org).

## Licence

GPL-3.0-or-later (`LICENSE`), with no warranty. The third-party notices are in
`THIRD_PARTY_NOTICES`.
