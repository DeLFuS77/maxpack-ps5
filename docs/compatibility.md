# Compatibility

Which games have been tested with MaxPack, on which console and with what result. The list is public:
anyone can add a test. One row per test; if the result changes with another version of MaxPack, another
drive or another Fakelib mode, it goes in a new row and the old one stays.

**No game names and no title IDs.** By the project owner's choice, this list identifies a game only by its
numbers: its size before and after packing, its volumes, and the setup it was tested on. Please do not add
a name or a title ID to a row, not even in the notes; a report or a pull request that has one is edited
before it goes in. Games A, B and C are the ones in the README's
[Tested on a real PS5](../README.md#tested-on-a-real-ps5).

Reports in English or Spanish are welcome.

## How to add a row

1. On MaxPack's page, open the game's sheet and press **Copy report**. At the end the report has the
   row ready, in this list's column order and with no name or title ID: format, drive, firmware,
   MaxPack's version and Fakelib mode are filled in, and each `?` is for you to fill.
2. Fill in each `?`: the sizes, a `.p5z`'s volumes, whether a USB drive is an SSD or a hard drive, the
   result and the date. The real date: the clock of an offline console is often years behind, which is
   why the report does not fill it in.
3. Open a [game report](https://github.com/DeLFuS77/maxpack-ps5/issues/new?template=game_report.yml) and
   paste the row there (the row is added from it), or add it at the end of the table in a pull request.

Columns:
- **Original → `.p5z`**: the game's size before and after packing, in GiB as the console page shows
  them, and how much smaller (`7.8 → 2.3 GiB (−70 %)`). For a packed folder, the folder's size after
  packing.
- **Format**: `p5z` (a MaxPack file, played without installing), `zstd folder` or `LZ4 folder` (packed),
  `pkg` (debug FPKG), `plain folder` (unpacked) or `installed` (put on the console with MaxPack's Install
  tab, which has not been tested on a real PS5 yet).
- **Volumes**: how many volumes the `.p5z` has (`maxpack info` on the PC shows it); `—` for the other
  formats.
- **Drive**: `internal SSD`, `M.2 SSD (console slot)`, `USB SSD` or `USB HDD`, with how it is connected
  when it matters (an M.2 SSD in a USB-C enclosure is a USB SSD).
- **Firmware**: the console's (Settings > System > System Software).
- **Loader/mounter**: the one that mounted the game, with its version (`MaxPack 1.0.0`,
  `ShadowMountPlus 1.7beta12`...). MaxPack 1.0.0 is the first published version; a "test build" and
  "MaxPack stage 1" are earlier builds made while it was being tested.
- **Fakelib mode**: MaxPack's setting on the game's sheet (`Full`, `Emulators only` or `Off`), or
  `none (MaxPack libraries at launch off)`; for another mounter, its own mode.
- **Result**: what was checked (boots, saves, loads, plays up to a given point) and what fails.
- **Date**: the day of the test, `YYYY-MM-DD`.
- **Notes**: anything else worth knowing: what was tried before it worked, how fast it loads.

## List

| Original → `.p5z` | Format | Volumes | Drive | Firmware | Loader/mounter | Fakelib mode | Result | Date | Notes |
|---|---|---|---|---|---|---|---|---|---|
| 7.8 → 2.3 GiB (−70 %) | p5z | 1 | internal SSD | 13.60 | MaxPack test build (before 1.0.0) | Full | Plays, saves and loads | 2026-11-24 | Game A. |
| 69.0 → 33.2 GiB (−51 %) | p5z | 9 | USB SSD (M.2 over USB-C) | 13.60 | MaxPack test build (before 1.0.0) | Full | Played without problems | 2026-11-24 | Game B. Reads from the `.p5z` took about 2 ms each on this drive. |
| 124.4 → 74.1 GiB (−40 %) | p5z | 19 | USB SSD (the same M.2 over USB-C) | 13.60 | MaxPack test build, then 1.0.0 | Off | Reaches the main menu and opens its saves | 2026-11-24 | Game C. Only ampr-emu-ng in its `fakelib/`: set by hand during the test, then with the 1.0.0 setting. With the dump's backport libraries it showed a black screen; with the bundled PlayGo stub it stalled at the add-on check. |
| not noted | zstd folder | — | not noted | 13.60 | ShadowMountPlus 1.7beta12 and MaxPack stage 1 | ShadowMountPlus's default (Full) | Boots, saves and loads | 2026-11-24 | Game A as a folder packed with MaxPack for the PC, tested before the `.p5z` rows. |

Also seen on that console: a mechanical USB hard drive was far too slow for a large `.p5z` (see the
README's [Storage advice](../README.md#storage-advice)).
