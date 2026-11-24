# Changelog

What changed in each version of MaxPack for the console. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the versions are those of `Cargo.toml`.

## 1.0.0

The first public release of MaxPack for the console. It is based on
[Kraken Mount](https://github.com/quer3q/kraken-mount) 0.3.1 by quer3q, keeps its history and its
licence, and replaces the earlier test builds (0.1.x and 0.2.x).

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
