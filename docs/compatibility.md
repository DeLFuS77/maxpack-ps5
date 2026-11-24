# Compatibility

Which games have been tested with MaxPack, on which console and with what result. The list is public:
anyone can add a test. One row per game, version, firmware and mounter; if the result changes with
another version of MaxPack or of the game, it goes in a new row and the old one stays.

Reports in English or Spanish are welcome. You can also open a
[game report](https://github.com/DeLFuS77/maxpack-ps5/issues/new?template=game_report.yml) and the
row will be added from it.

## How to add a row

1. On MaxPack's page, open the game's sheet and press **Copy report**. At the end the report has the
   row ready: `TITLE_ID`, name, version, source, firmware and mounter.
2. Fill in the result and the date. The real date: the clock of an offline console is often years
   behind, which is why the report does not fill it in.
3. Add the row to the table, in `TITLE_ID` order.

Columns:
- **Title ID**: the one in `sce_sys/param.json` (`PPSA...`, `CUSA...`).
- **Game**: the name shown on the home screen.
- **Version**: `contentVersion` in `param.json`.
- **Source**: `pkg` (debug FPKG), `folder` (unpacked), `zstd folder` or `lz4 folder` (packed) or
  `p5z` (a MaxPack file, played without installing). In brackets, what it was packed with when it
  matters.
- **Firmware**: the console's (Settings > System > System Software).
- **Mounter**: the one that mounted the game, with its version (`MaxPack 1.0.0`,
  `ShadowMountPlus 1.7beta12`...). MaxPack 1.0.0 is the first published version (mounting, and playing
  from the `.p5z`); rows that say "MaxPack stage 1" or "stage 2" come from the earlier test builds
  (0.1.x and 0.2.x).
- **Result**: what was checked (boots, saves, loads, plays up to a given point) and what fails.
- **Date**: the day of the test, `YYYY-MM-DD`.

## List

| Title ID | Game | Version | Source | Firmware | Mounter | Result | Date |
|---|---|---|---|---|---|---|---|
| PPSA12544 | Hollow Knight: Silksong | 01.000.010 | zstd folder (packed with MaxPack) | 13.60 | ShadowMountPlus 1.7beta12 and MaxPack stage 1 | Boots, saves and loads a game | 2026-11-24 |
