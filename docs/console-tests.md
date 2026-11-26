# Console tests

A checklist for testing MaxPack on a PS5, like phase 2's P1-P5. It goes in parts; each says what to do
and what should happen. Tick each box as you check it and note the firmware.

Where to look:
- the log, `/data/maxpack/maxpack.log` (also on the page, at the bottom);
- the state, `http://<console-ip>:8095/api/state`: the fields `waiting`, `games` (with
  `kind == "p5z"` for a `.p5z`, and `error_code` and `missing_bytes` if it does not fit; `hidden`,
  `crash` and `rebuild_queued`), `images` and `unplayable` (the `.p5z` files that cannot be played);
- a game's report, `http://<console-ip>:8095/api/report/<TITLE_ID>` (section 7);
- the installer's state, `http://<console-ip>:8095/api/inst/state`: `job`, `pending`, `installed`
  and `last`;
- the console's notices.

> Work on copies. First save `/data/krakenmount` and `/data/shadowmount` over FTP: MaxPack must not
> touch them, and this way you can compare.

## 0. Before anything

- [ ] **0.0. Without the phase-2 `maxpack.elf`.**
  - Remove the phase-2 `maxpack.elf` from every autoload list (the loader's, etaHEN's or kstuff's) and
    restart the console. It has the same name and uses the same port 8095, but it does not know how to
    hand the port over to a new copy.
  - Expected: when you send this MaxPack, the notice "MaxPack: port 8095 busy, not started. ..." does
    not appear, and the page opens at `http://<console-ip>:8095`.
  - If that notice appears, the phase-2 one (or another copy) is still running: restart without it and
    start over. Do not go on with it running: whatever you saw on port 8095 would be its.
- [ ] **0.1. The original Kraken Mount works on your firmware with a MaxPack game.**
  - Copy a game packed with `maxpack pack` (a folder with `ampr_zstd.index`) to
    `/data/homebrew/<TITLE_ID>` (or to `homebrew/` on a USB drive).
  - Start only the original Kraken Mount (quer3q's, 0.3.1), with neither ShadowMountPlus nor MaxPack.
  - Expected: the game shows up in the menu, starts, the notice "ampr-emu-ng: v... (zstd) running"
    appears, it loads and saves a game.
  - If this fails, nothing else is worth doing: stop, and note the firmware and Kraken Mount's log.
- [ ] **0.2. Close Kraken Mount** from its page (Stop) before going on.

## B. Base (normal mode)

What Kraken Mount does, with MaxPack alone (neither ShadowMountPlus nor Kraken Mount running). Prepare
one game of each kind of source: an unpacked folder, one packed with zstd, one with LZ4 and an FPKG
(`.pkg`).

- [ ] **B.1. Launch and exit ten times with each kind of source.**
  - With each of the four games: launch it, wait for it to load, exit to the menu, and so ten times.
  - Expected, every time: in the log, `<TID> overlay MOUNTED` at launch and `<TID> cleanup done` on
    exit; the console does not hang or shut down, and the game starts again.
  - Note which game and which time if anything fails.
- [ ] **B.2. On exit, nothing forced.** In B.1's log, look for `(MNT_FORCE)` or `cleanup done after
  MNT_FORCE`.
  - Expected: none appears.
  - If one does: a layer stayed busy and MaxPack removed it by force. Note the game, the kind of
    source, the firmware and the log lines of that `<TID>` from `overlay MOUNTED` to `cleanup done`;
    repeat that game five more times. If the console hangs on exit, stop here and keep `maxpack.log`.
- [ ] **B.3. The ampr-emu-ng notice.**
  - Launch the zstd game.
  - Expected: the notice says "ampr-emu-ng: v0.5.1 (zstd) running", **without** `-beta`; with the LZ4
    one, "(lz4)". In Settings, ampr-emu-ng shows as bundled, version `v0.5.1-maxpack.2` (`v0.5.1-maxpack.1`
    in a 1.0.1 build or older). The notice says v0.5.1 with both: it is upstream's version, which every MaxPack
    build keeps; Settings shows which build it is.
  - This build has no `-beta` (the one on `main`, the stage-1 one, did: `v0.5.0-beta.a52648f`;
    `a52648f` is `ecae2ff` in ampr-emu-ng's public history). If
    `-beta` or another version appears, it is not this branch's: look at
    `/data/maxpack/ampr/libSceAmpr.sprx` and `/data/maxpack/ampr.bundled`, and note it.
- [ ] **B.4. The DLC and PlayGo emulators.**
  - With "Add the bundled DLC (dlc_emu) and PlayGo (pgo_stub) emulators when a game needs them and has
    none" on, launch a game with DLC in `/user/addcont/<TID>` and no emulator of its own. Expected: the
    DLC shows up in the game.
  - A game that uses PlayGo: its sheet shows "PlayGo stub", and the game does not ask to finish the
    download. Turn it off and launch again. Expected: in the log, `<TID>: PlayGo stub off`, and the
    cache is rebuilt without it (`fakelib: PlayGo stub off`).
- [ ] **B.5. Backports.** Put a backport in `<root>/backports/<TID>` of a folder game. Expected: the
  game starts with the backport applied (the backport's version), and in `/api/state` its `backport`
  is that folder.
- [ ] **B.6. A moved game.** Move a folder game from `/data/homebrew` to `homebrew/` on a USB drive.
  Expected: the notice "<game> moved to <path>" (without rebuilding the cache) and the game starts from
  the USB drive.
- [ ] **B.7. Removing the ones that disappear, after 300 s.**
  - With "Remove a game from the home screen when its source is missing" on, take a game's folder out
    of the scanned roots (for example to `/data/aside`; inside a root it would count as moved) or
    unplug the USB drive that holds it.
  - Expected: after 300 s (`auto_remove_delay_seconds`), `<TID>: source <path> missing for N s,
    removed from the home screen`, and the game leaves the menu. Before that it stays in the menu.
  - Put the folder back: the game is registered again on the next pass.
- [ ] **B.8. Eject a USB drive.** In the library, "Unmount" on the USB drive's pill. Expected:
  "USB 1 unmounted. You can disconnect it now.", its games leave the library, and they come back when
  you plug it in again.
- [ ] **B.9. The language of the notices.** Change the console's language to English and then to
  another one (for example Spanish), and cause a notice each time (for example "Rebuild cache" on a
  game). Expected: the notice comes in the console's language.
- [ ] **B.10. Settings.**
  - Change the scan interval and press "Save". Expected: "Saved.", and when you reopen Settings it is
    still changed; `/data/maxpack/config.toml` has it.
  - Put `/system_ex/app` (or `/`) in "Scan paths" and press "Save". Expected: "Save failed: MaxPack
    never scans /system_ex/app: ...", and `config.toml` does not change.
  - In Settings, "About" names MaxPack, Kraken Mount (quer3q), ampr-emu-ng, ShadowMountPlus, dlc_emu,
    pgo_stub, ps5-payload-sdk, zstd, zlib, GPL-3.0-or-later and "no warranty".
- [ ] **B.11. Start from the MaxPack tile.** Stop MaxPack from its page ("Stop MaxPack") and open the
  MaxPack tile (`MXPK00001`) on the home screen.
  - Expected: the page comes up at once (the saved one), says "Starting MaxPack..." and in a few
    seconds it works: the loader started `/data/maxpack/maxpack.elf`.
  - If it says "maxpack.elf is not saved yet", send the ELF to port 9021 once and repeat.
- [ ] **B.12. A dumped game whose TITLE_ID is installed on the console.**
  - Put in `/data/homebrew/<TITLE_ID>` the dumped folder of a game you also have installed (from the
    store or from a disc, or an FPKG installed from the debug settings).
  - Expected:
    - in the library it shows with "Error", and its sheet says "This title is installed on the console
      (/user/app/<TITLE_ID>/app.pkg): MaxPack does not register or mount this copy...";
    - in the log, once, `<TITLE_ID> (<path>): an installed copy of this title exists (...): MaxPack
      does not register or mount this copy`;
    - the installed game stays as it was: its tile, and launching it starts the installed one, with no
      `overlay MOUNTED` for that `<TITLE_ID>` in the log.
  - Remove the folder when done.
- [ ] **B.13. Another website cannot touch MaxPack.** From a PC on the same network:
  - `curl -i -X POST -H "Origin: http://example.com" http://<console-ip>:8095/api/rescan`.
    Expected: `403` and "MaxPack: request refused: it did not come from the MaxPack page."
  - `curl -i -H "Host: ps5.example" http://<console-ip>:8095/api/state`. Expected: `403` and
    "MaxPack: open this page at the console's IP address (http://<PS5 IP>:8095), not by a name."
  - `curl -i -X POST http://<console-ip>:8095/api/rescan` (no `Origin`). Expected: `204`.
  - In the log, `web: refused ...` (one line a minute at most).

## 1. Imported roots (the first time)

- [ ] **1.1. With scan paths of their own in the other mounters.**
  - Prepare:
    - a `scanpath` of its own in `/data/krakenmount/config.toml`, for example `/mnt/usb0/games`;
    - a `path=` line in the `[scan]` section of `/data/shadowmount/config.ini`;
    - a game's folder in `/data/shadowmount/manual.lst`, outside the usual roots.
  - Delete `/data/maxpack/config.toml` (or use a console where MaxPack has not run) and start MaxPack.
  - Expected:
    - in the log, `config: first start: imported N scan root(s) from the other mounters: [...]` with
      those paths (from `manual.lst`, the game's **parent** folder);
    - `config.toml` has `scanpath` with the usual roots first (of every drive, plugged in or not) and
      then the imported ones;
    - the games of those paths show up, and also those in `/data/homebrew` and on the USB drive.
- [ ] **1.2. Without scan paths of their own.**
  - Delete `config.toml` and start MaxPack on a console with no scan paths of their own in the other
    mounters.
  - Expected:
    - `imported 0 scan roots` in the log;
    - `config.toml` with no `scanpath` line (only the commented example);
    - a USB drive plugged in afterwards is scanned.
- [ ] **1.3. Only the first time.** Change the other mounter's `scanpath` and restart MaxPack.
  Expected: `config.toml` does not change.
- [ ] **1.4. A USB drive plugged in later, with imported roots.** With 1.1's `config.toml`, plug in a
  USB drive that was not there at start. Expected: its `homebrew/` and `etaHEN/games` are scanned.
- [ ] **1.5. The others' files that cannot be read or are garbage.** Try each case separately,
  deleting `config.toml` before starting MaxPack:
  - a `/data/shadowmount/config.ini` with random bytes (for example a renamed image);
  - a `/data/krakenmount/config.toml` over 256 KiB;
  - a folder called `/data/shadowmount/manual.lst`.
  - Expected:
    - MaxPack starts in a few seconds, without hanging;
    - the big file gives `config: first start: <path>: N bytes, over 262144; skipped`, and the folder,
      `not a regular file; skipped`;
    - the garbage adds no roots;
    - the games of the usual roots show up all the same.
- [ ] **1.6. A root inside another mounter's data.**
  - Add `"/data/krakenmount/caches"` by hand to the `scanpath` of `/data/maxpack/config.toml`.
  - Expected:
    - once in the log, `scan root /data/krakenmount/caches is inside another mounter's data folder:
      not scanned (MaxPack never writes there)`;
    - no game from that folder shows up in the library.
  - Remove the line when done.
- [ ] **1.7. Nothing written in the others.** When you finish this list, compare `/data/krakenmount`
  and `/data/shadowmount` over FTP with the copy from the start. Expected: the same files, sizes and
  dates.

## 2. Another mounter

- [ ] **2.1. ShadowMountPlus started first.** Start ShadowMountPlus and then MaxPack.
  - Expected:
    - the notice "MaxPack: ShadowMount+ is running; waiting for it to stop";
    - MaxPack's page opens and says so;
    - `waiting` is `"other:shadowmountplus.elf"`;
    - in the log, `shadowmountplus.elf is running: no scan, build, register or new overlay`;
    - MaxPack registers and mounts nothing meanwhile.
- [ ] **2.2. ShadowMountPlus closes.**
  - Expected, 2 to 4 s later (MaxPack has to see it closed twice, 2 s apart):
    - the notice "MaxPack: ShadowMount+ stopped; MaxPack mounts games again";
    - `waiting` is `null`;
    - the games are mounted on the next pass.
- [ ] **2.3. ShadowMountPlus started later.**
  - With MaxPack mounting, start ShadowMountPlus.
  - Expected:
    - within about 2 s, the notice and `waiting` as in 2.1;
    - if it was building, `pass stopped before <TID>` in the log and nothing else is built.
  - Launch a MaxPack game. Expected: `[launch] <TID> other:shadowmountplus.elf: not overlaying this
    launch` and no `overlay MOUNTED`.
- [ ] **2.4. A long build when the other one starts.**
  - Put a new FPKG (with no MaxPack cache yet), wait for the game to show as "Building" and start
    ShadowMountPlus before it ends.
  - Expected:
    - when the build ends, `<TID>: built; not registered while another mounter runs` in the log;
    - the game stays in `pending` in `games`, neither registered nor mounted;
    - when ShadowMountPlus closes, the next pass registers and mounts it, without building it again.
- [ ] **2.5. ShadowMountPlus running for 10 minutes.**
  - With ShadowMountPlus running, leave MaxPack for 10 minutes without touching anything.
  - Expected:
    - `waiting` stays at `"other:shadowmountplus.elf"` the whole time;
    - no "mounts games again" notice;
    - in the log of those 10 minutes, no `nullfs`, `registered`, `overlay MOUNTED` or
      `is gone; mounting again` line.
  - If `process list unreadable` appears, note it: the state stays as it was and it must not mount.
- [ ] **2.6. A MaxPack game running when the other one starts.**
  - Launch a game with MaxPack (in the log, `overlay MOUNTED`), start ShadowMountPlus and exit the
    game.
  - Expected: `overlay UNMOUNTED` and `cleanup done` as always, and the console does not hang.
  - Repeat it ten times.
- [ ] **2.7. The other one closes with a game of its own running.**
  - Launch a game with ShadowMountPlus and, with the game open, close ShadowMountPlus.
  - Expected:
    - if MaxPack already had a cache of that game: `not overlaying this launch` at launch, and MaxPack
      does not touch its mount until you exit the game (`paused` in `/api/state`);
    - if it did not: the title ends in error with `unmount /system_ex/app/<TID>, a nullfs of <path>
      (title running?)`, with nothing forced and not repeated on every pass (see 3.4);
    - exiting the game does not hang the console.
- [ ] **2.8. Kraken Mount started later, both ways.**
  - Start the original Kraken Mount from its menu tile. Close it, and start it again by sending its
    ELF to the loader with `nc <console-ip> 9021 < kraken-mount.elf`.
  - Expected in both cases:
    - the notice "MaxPack: Kraken Mount is running; MaxPack mounts no games until it stops";
    - `waiting` is `"other:kraken-mount.elf"`;
    - the page's banner names `kraken-mount.elf`.
  - If it does not detect it in either, note how you started it and, if you can see it, the name of its
    process.
  - Close it from its page (Stop). Expected: the notice "MaxPack: Kraken Mount stopped; MaxPack mounts
    games again" and MaxPack mounts again.
- [ ] **2.9. With the other one running, MaxPack's page responds.**
  - The library, the settings and the log can be seen.
  - Deleting a game or ejecting a USB drive from MaxPack answers "another mounter is running (...):
    MaxPack touches no game mount until it stops", and nothing is unmounted or deleted.
  - "Rebuild cache", "Rebuild all" and "Purge cache" are queued until the other one closes.
- [ ] **2.10. kstuff stops answering mid-session** (only if you can cause it, for example if your
  kstuff dies after rest mode).
  - Expected:
    - a kstuff notice and `waiting` is `"kstuff"`;
    - nothing new is mounted or built;
    - when kstuff is back, in the log `kstuff: ready; mounting`, and MaxPack goes on.

## 3. Adopting ShadowMountPlus and Kraken Mount games

- [ ] **3.1. ShadowMountPlus games, after a restart.**
  - With several folder games registered by ShadowMountPlus, restart the console and start only
    MaxPack.
  - Expected:
    - each game shows up **only once** in the menu;
    - `/data/maxpack/caches/<TID>` is created (for a folder, only `fakelib`);
    - the game starts with `overlay MOUNTED` in the log.
- [ ] **3.2. ShadowMountPlus games, in the same session.**
  - ShadowMountPlus mounted its games; close it and start MaxPack, without restarting.
  - Expected:
    - a folder game from the same path keeps its mount;
    - one with another source gives `/system_ex/app/<TID> is a nullfs of <its path> (not ours, or
      stale); unmounting it` and MaxPack's is mounted;
    - no duplicates in the menu.
- [ ] **3.3. Kraken Mount games, in the same session.**
  - With FPKG and folder games registered by Kraken Mount, close it and start MaxPack.
  - Expected:
    - an **FPKG**: its nullfs of `/data/krakenmount/caches/<TID>` gives `... is a nullfs of
      /data/krakenmount/caches/<TID> (not ours, or stale); unmounting it`, and the one of
      `/data/maxpack/caches/<TID>` is mounted; it is extracted again the first time;
    - a **folder** game: both mount the same folder, so its mount is **kept** (no "unmounting it");
      MaxPack only creates its `fakelib`;
    - no duplicates in the menu;
    - the games start.
- [ ] **3.4. A mount that cannot be removed.**
  - If another mounter's game is still running when MaxPack adopts it, the title ends in error. The
    reason says `unmount /system_ex/app/<TID>, a nullfs of <path> (title running?)`.
  - Expected:
    - it is never forced;
    - the error is not repeated on every pass;
    - after you exit the game, "Rebuild cache" on the page (or restarting MaxPack) fixes it.
- [ ] **3.5. ShadowMountPlus images.**
  - Put `Game [PPSA01234].exfat` and `other.ffpkg` in `/data/homebrew` and in
    `/mnt/usb0/homebrew/<subfolder>/`. A direct subfolder of the USB root does not count: MaxPack does
    not go below a drive's root.
  - Expected:
    - MaxPack does not mount them;
    - `/api/state` lists them in `images`, `{"path", "kind": "exfat", "title_id": "PPSA01234"}` and
      `{"path", "kind": "ffpkg"}` without `title_id`;
    - a game that ShadowMountPlus mounted from an image stays as it was.
- [ ] **3.6. A game that goes from folder to image is not removed.**
  - With a folder game already mounted by MaxPack, replace the folder with
    `Game [<its TITLE_ID>].exfat` in the same root.
  - Expected: after `auto_remove_delay_seconds` (300 s by default), the game **stays** in the menu.
    `removed from the home screen` does not appear for that title.

## 4. Going back to Kraken Mount and to ShadowMountPlus

- [ ] **4.1. Back to Kraken Mount.** Stop MaxPack from its page ("Stop MaxPack") and start the
  original Kraken Mount. Expected: its games show up and start as before, with its cache and its
  settings intact.
- [ ] **4.2. Back to ShadowMountPlus.** The same with ShadowMountPlus.
- [ ] **4.3. And to MaxPack again.** Expected: it adopts again without duplicates, as in 3.1.

## 5. The installer (Install tab)

Prepare on a USB drive a `.p5z` made with `maxpack pack --p5z` and a `.exfat` image of another game,
both in a folder of their own on the drive, for example `/mnt/usb0/install/`: not in the root nor in
`homebrew/` or `etaHEN/games/`. Since stage 2, a `.p5z` in a folder that is scanned (a USB drive's
root is a scan root by default) is already a game: MaxPack builds its cache and registers it before you
install it, and this section does not test that (section 6 does). MaxPack does not scan below a
drive's root, and the Install tab looks one level further down, so it finds them all the same. At
start the log says `installer: ready in N ms (MaxPack engine 5463d7506c3e37fb4956604a28b705460d1e2b0e,
runtime ampr-emu-ng v0.5.1-maxpack.2 720048 bytes)`; a 1.0.1 build or older says engine `e8de15a...` and
runtime `v0.5.1-maxpack.1 716176 bytes`, and a build from before the repositories were public `e9e0ec4`.

- [ ] **5.1. Install a `.p5z` and a `.exfat` as they are.**
  - In Install both show under "Games to install": the `.p5z` with its icon, what it saves and what it
    will take; the `.exfat` with "copied as is".
  - Install the `.p5z` on the internal storage and, when it ends, the `.exfat` on the USB drive.
  - Expected:
    - progress advances with speed and time left, and at the end "<game> installed.";
    - both show under "Installed with MaxPack" and in `/data/maxpack/installed/<TITLE_ID>.json`;
    - the `.p5z` ends up in `/data/homebrew/<TITLE_ID>` and MaxPack mounts it on its next pass: it
      shows in the menu and starts;
    - the `.exfat` ends up in `homebrew/` on the USB drive and MaxPack does **not** mount it: in
      Install it says "Image: for now it is played with ShadowMountPlus, not mounted by MaxPack." (and
      its "Pending in ShadowMountPlus" if that does not answer), and it shows in the library under
      "ShadowMountPlus images";
    - "Verify" ends in "<game> verified." with `0 damaged`.
- [ ] **5.2. Install with ShadowMountPlus running (installer only).**
  - Start ShadowMountPlus and then MaxPack.
  - Expected:
    - the page's banner says "Another mounter is running (shadowmountplus.elf): MaxPack only installs."
      and that the page is opened in a browser at `http://<PS5-IP>:8095`;
    - MaxPack's tile (`MXPK00001`) is **not** registered: in the log, `tile: not registered while
      another mounter runs`; on FW 12+, `sceAppInstUtilAppInstallAll` is not called;
    - open `http://<PS5-IP>:8095` in the console's browser: Install works;
    - install a folder game with zstd packs: it is registered in ShadowMountPlus with the `fakelib`
      mode notice, as in phase 2, and ShadowMountPlus mounts it;
    - MaxPack mounts nothing meanwhile (`waiting` stays at `"other:shadowmountplus.elf"`).
  - Close ShadowMountPlus. Expected: the tile is registered then (`tile MXPK00001: ...`) and MaxPack
    mounts what was installed.
- [ ] **5.3. Replace.**
  - Copy a folder of the same game by hand to `/data/homebrew/<TITLE_ID>` (or another path the loaders
    scan) and install its `.p5z`.
  - Expected: "Install and replace?" with the path and size of the old copy. "Cancel" touches
    nothing; "Install and replace" installs, verifies and only then deletes the old one. One copy and
    one menu entry are left.
- [ ] **5.4. Resume after removing the USB drive.**
  - Start installing a big game from the USB drive and unplug the drive halfway.
  - Expected: the install fails without hanging MaxPack and shows under "Unfinished installs" with
    what was already copied.
  - Plug the drive in again and press "Resume". Expected: it continues where it stopped (it does not
    start from zero) and finishes as in 5.1.
  - Repeat with "Discard": what was copied is deleted (`/data/maxpack/tmp/<TITLE_ID>` or the drive's
    `maxpack-tmp/`) and it disappears from the list.
- [ ] **5.5. Quit with an install running.**
  - With an install copying, press "Stop" on the page (or send the ELF to the loader again: the new
    copy asks the old one to quit).
  - Expected:
    - in the log, `installer: pausing before quitting` and `installer: paused; ...` in under 3 s; the
      new copy takes port 8095 without `port_busy`;
    - when it starts again, the install shows under "Unfinished installs" and "Resume" finishes it.
- [ ] **5.6. The page from a phone.**
  - Open `http://<PS5-IP>:8095` in the browser of a phone on the same network.
  - Expected: the library, Install and the settings show without scrolling sideways; installing,
    pausing, verifying and uninstalling work just as on the console.
  - Open it with a name instead of the IP (for example your router's name for the console). Expected:
    the whole page is refused with "MaxPack: open this page at the console's IP address ...": only the
    IP (or `localhost`) works. With the IP, and another mounter running, the banner says
    `http://<PS5-IP>:8095`.
- [ ] **5.7. Another mounter starts during an install.**
  - With MaxPack mounting, start installing a big `.p5z` and, halfway through the copy, start
    ShadowMountPlus.
  - Expected:
    - within about 2 s, the installer-only banner and `waiting` = `"other:shadowmountplus.elf"`;
    - the copy **goes on** (the percentage does not stop; nobody pauses it);
    - at the end, "Registering <game> in ShadowMountPlus" and the game is registered in
      ShadowMountPlus, with the `fakelib` mode notice; ShadowMountPlus mounts it;
    - MaxPack mounts nothing of that game meanwhile: in the log, no `nullfs`, `registered` or
      `overlay MOUNTED` line with its TITLE_ID.
  - Close ShadowMountPlus. Expected: MaxPack adopts it as in 3.2, without duplicating it.
- [ ] **5.8. Installer only with Kraken Mount.**
  - Start the original Kraken Mount (as in 2.8) and then MaxPack.
  - Expected:
    - the banner says "Another mounter is running (kraken-mount.elf): MaxPack only installs." with
      `http://<PS5-IP>:8095`, and MaxPack's tile is not registered (as in 5.2);
    - Install works; without ShadowMountPlus, "ShadowMountPlus ... connected" does not show, and an
      installed folder game or `.p5z` does not show "Pending in ShadowMountPlus" (an image does);
    - what was installed in `homebrew/` is picked up by Kraken Mount on its next scan if that root is
      its own (an image is not: only ShadowMountPlus mounts those); MaxPack mounts nothing.
- [ ] **5.9. Quit with a queued job and during an uninstall.** Note what happens: this is how it
  behaves today.
  - Install two games in a row (the second shows "1 more queued") and press "Stop" while the first is
    copying.
    - Expected today: the first is paused and shows under "Unfinished installs" when it starts again.
      The second may start within those 3 s and be left unfinished too (continue it with "Resume"); if
      it never started, it is not kept anywhere and has to be installed again.
  - Uninstall a big game and press "Stop" right away.
    - Expected today: pausing does not stop an uninstall; in the log, `installer: still busy after
      3 s; quitting anyway` and MaxPack quits. When it starts again, the game may still be under
      "Installed with MaxPack" with its folder half deleted: "Uninstall" again finishes it (folder,
      registration and its ShadowMountPlus entry).
- [ ] **5.10. First start without kstuff, and when the tile appears.**
  - With the console freshly on and no kstuff loaded, start MaxPack.
  - Expected:
    - a kstuff notice and, on the page, "Waiting for kstuff: it is not running yet. ..."; Install
      answers (`installer: ready` in the log);
    - no `tile` line yet: the tile waits for kstuff.
  - Load kstuff. Expected: `kstuff: ready; mounting` and then `tile MXPK00001: ...` (registered or
    `up to date`); if another mounter is running at that moment, `tile: not registered while another
    mounter runs`, and the tile arrives when that one closes (5.2).

- [ ] **5.11. Uninstall in normal mode.**
  - With MaxPack mounting (no other mounter), uninstall a game installed with MaxPack that is already
    in the menu (from 5.1).
  - Expected:
    - when it ends, in the log, `<TITLE_ID>: uninstalled by the installer (<folder>): unmounted and
      removed from the home screen`;
    - the game leaves the menu at once, not after 300 s; it leaves the library;
      `/data/maxpack/caches/<TITLE_ID>` is gone;
    - the same with "Remove a game from the home screen when its source is missing" turned off.
- [ ] **5.12. Uninstall or replace with the game open.**
  - Open a MaxPack game. With the game open, in Install press "Uninstall" on an installed game.
  - Expected: "Close the game first.", nothing is deleted, and in the log `installer: /api/uninstall
    refused: a game is running`.
  - The same with "Install and replace" (as in 5.3): "Close the game first." and the old copy stays.
  - Close the game: both work.
- [ ] **5.12b. A long replace with the old copy open.**
  - Start "Install and replace" of a big game and, while it is still copying, open the old copy.
  - Note what happens when the copy ends and when you exit the game. Expected today: the lock is only
    checked when the request arrives and the engine deletes the old copy at the end, so the game's
    files may disappear under it.
  - Note whether the console hangs: that would be a mandatory fix.
- [ ] **5.12c. Uninstall with the game open under ShadowMountPlus (installer only).**
  - With ShadowMountPlus mounting and MaxPack in installer-only mode, open a game installed from an
    engine folder and press "Uninstall" on it.
  - Expected: it is refused with "Close the game first." (in the log `installer: /api/uninstall
    refused: a game is running`) and nothing is deleted. Close the game: now it works.
- [ ] **5.13. Uninstall with another mounter running.**
  - With ShadowMountPlus running (installer only), uninstall a game installed with MaxPack.
  - Expected: the installer removes it from ShadowMountPlus as in phase 2; MaxPack does not touch the
    menu (in the log, `uninstalled by the installer while MaxPack does not mount: its home entry is
    left to the mounter in charge`).

## 6. Playing from the .p5z

A MaxPack `.p5z` (made with `maxpack pack --p5z`) in a scan root is played without installing it:
MaxPack extracts only its loose files to `/data/maxpack/caches/<TITLE_ID>` and reads the volumes from
the `.p5z` itself, mounted read-only at `/app0/p5z` at launch. Use a game of a reasonable size (a few
GB) and, for the space, one whose loose files you know.

MaxPack does not mount while another mounter runs: test **first with ShadowMountPlus closed** (6.11 is
the test with it running).

- [ ] **6.1. The ampr-emu-ng notice.**
  - Copy the `.p5z` to `/data/homebrew` and wait for it to show in the menu; in `/api/state` its
    `kind` is `"p5z"`. Launch it.
  - Expected: the notice says "ampr-emu-ng: v0.5.1 (p5z) running" (no `-beta`); in the log,
    `<TID>: building cache` at the start and `<TID> overlay MOUNTED` at launch; in
    `/data/maxpack/caches/<TID>` there are `maxpack.image`, `p5z/` and `fakelib/`, and the loose
    files, not the volumes. `maxpack.image` is text: `maxpack-image 1`, `p5z/<name>.p5z` and one line
    per volume.
  - If `(zstd)`, `(lz4)` or another mode appears, or none, stop and keep the log.
- [ ] **6.2. A `.p5z` on a USB drive.**
  - Put the `.p5z` in `homebrew/` on a USB drive (or a folder further down) and remove it from
    `/data/homebrew`.
  - Expected: it starts, videos play without stutter, loading and saving a game work, and 10 minutes
    of play without errors. You exit to the menu without a hang; in the log, `cleanup done`.
  - Also expected: `<TID>: building cache` shows only once, not on every pass, even if the console's
    clock is years behind the PC that wrote the USB drive (the cache compares the `.p5z`'s size and
    date with the ones it noted, not with the clock).
- [ ] **6.3. A `.p5z` on the M.2 drive.** The same from `homebrew/` on `/mnt/ext1`. Expected: the same
  as 6.2.
- [ ] **6.4. A `.p5z` on the internal SSD.**
  - Copy it to `/data/homebrew/` (another folder of `/data` does not count: it is a scan root).
  - Expected: the same as 6.2.
- [ ] **6.5. Move it to another drive.**
  - With the cache already built, move the `.p5z` from one drive to another (for example from the USB
    drive to `/data/homebrew`), the same file.
  - Expected: the notice "<game> moved to <path>", and in the log `<TID>: moved <old path> -> <new
    path>; relinking <cache>`, **without** `building cache` or extracting again; the game starts from
    the new drive and its saves and files are still there.
- [ ] **6.6. Replace it with another version.**
  - Replace the `.p5z` with another of the same game (another version, or the same one made again).
  - Expected: in the log, `building cache` again (the cache is rebuilt with the new one's loose
    files), and the game starts with the new version.
- [ ] **6.7. Install it with the Install tab.**
  - In Install, install that `.p5z` on the internal storage.
  - Expected: the folder copy wins, the `.p5z` shows as `duplicate` and the game starts from the
    folder; the cache becomes that of a folder game (only `fakelib/`) and the space of the `.p5z`'s
    loose files is freed: compare `/data`'s free space or `/api/state` (`cache.size`) before and
    after.
  - Uninstall it (Install > Uninstall). Expected: the `.p5z` is the game again and starts.
- [ ] **6.8. A truncated `.p5z`.**
  - Copy a `.p5z` and cut its end off (for example `head -c <half> game.p5z > cut.p5z`), in a scan
    root.
  - Expected:
    - it does not show in the library as a game;
    - on the first pass that sees it, the log says `cannot be read now, tried again next pass: ...` (a
      momentary read failure looks the same) and it does not show in the list yet;
    - on the next pass (about 10 s later, `scan_interval`), with the same reason, it shows under
      ".p5z files that cannot be played", with "Damaged or incomplete"; in `/api/state`, `unplayable`
      lists it with `code` `damaged` and its `detail`;
    - a notice "<file> cannot be played: it is damaged or incomplete", once;
    - the other games stay as they were. Delete the file when done: it disappears from the list.
- [ ] **6.9. No space.**
  - Fill `/data` until it has less than the `.p5z`'s loose files plus 256 MiB, and put the `.p5z`
    (with its cache deleted: "Purge cache").
  - Expected:
    - the game shows with "Error" and its sheet says how much is missing; `/api/state`: `error_code`
      `no_space` and `missing_bytes`;
    - a notice "<game> does not fit on the internal SSD: N GB missing", once;
    - in the log, one `... bytes missing; tried again every pass` line, once per session.
  - Free up space. Expected: without touching anything, on the next pass it is built and the game is
    mounted and starts.
- [ ] **6.10. Exit the game ten times.**
  - Launch the `.p5z` and exit to the menu, ten times.
  - Expected, every time: `overlay MOUNTED` and `cleanup done`, with no `(MNT_FORCE)`, and the console
    does not hang or shut down.
- [ ] **6.11. Another mounter running.**
  - With ShadowMountPlus running, start MaxPack with the `.p5z` in a root.
  - Expected: the "another mounter" notice and `waiting` = `"other:shadowmountplus.elf"`; the `.p5z` is
    neither mounted nor built (no `building cache`, `registered` or `overlay MOUNTED` line of its), and
    the installer keeps working (Install answers).
  - Close ShadowMountPlus. Expected: within 2-4 s MaxPack mounts again and the `.p5z` shows in the
    menu.
- [ ] **6.12. A name with accents, a trademark sign or Japanese.**
  - Rename the `.p5z` the way MaxPack on the PC names it for a title like that, for example
    `Starfall’s Odyssey™ 2 [PPSA01234].p5z` (or `Iron Harbor™ DIRECTOR’S CUT [PPSA01411].p5z`), with the game
    closed.
  - Expected: it is relinked as in 6.5 (`moved ... relinking`, no `building cache`), line 2 of
    `maxpack.image` is `p5z/` and the new name as it is, the notice says "(p5z) running" and the game
    starts. If "ampr-emu-ng: p5z: bad maxpack.image" appears, stop and keep `maxpack.image` and the
    log.
- [ ] **6.13. Speed: the packed folder against the `.p5z`.**
  - Best with a game that, packed, goes over 4 GB: that way its `.p5z` has several volumes (in
    `maxpack.image`, one line per volume, as in 6.1).
  - On the PC, extract the packed folder from that same `.p5z`:
    `maxpack unpack "<game>.p5z" --folder -o "<folder>"`.
  - Both go to the same drive (for example `homebrew/` on the same USB drive), but not at the same
    time: if both are there, the folder wins (as in 6.7).
  - The folder first. When it shows in the menu, time:
    - the first load: from pressing the tile to the game's main menu;
    - loading a save: from choosing the save until you can play.
  - Then take the folder out of the root and put the `.p5z`. When it shows in the menu (in
    `/api/state`, its `kind` is `"p5z"`), time the same, with the same save.
  - Take each measurement three times and note them all, with the drive, the `.p5z`'s size and how
    many volumes it has.
  - Expected: similar times with the folder and with the `.p5z`. If the `.p5z` is clearly slower, note
    the times and keep the log.

## 7. Play, report, hide and what came from Kraken Mount

What came with the `consola-mejoras` branch, with MaxPack alone. Every result worth keeping, also
note it in [`compatibility.md`](compatibility.md) (7.3's report has the row ready).

- [ ] **7.1. Play from the sheet.**
  - Open the page on a phone or a PC (`http://<console-ip>:8095`), open the sheet of a "Ready" game
    and press "Play". Repeat with a folder game, a `.pkg` and a `.p5z`.
  - Expected: the game starts on the console as from its tile; in the log,
    `<TID>: launch from the web UI` and `sceLncUtilLaunchApp: 0x0` (or `0x80940005`, which is also
    fine), and then `overlay MOUNTED` as usual. The sheet says "Starting ... on the console..." and
    "Play" stays off while the game runs.
  - From the PS5's own browser it works too: the console switches to the game.
  - If it does not start: note what the sheet says ("Could not start the game: ...") and the two
    `sceLncUtilLaunchApp` / `sceSystemServiceLaunchApp` lines of the log.
- [ ] **7.2. Play, when it should not.**
  - With a game open, press "Play" on another (or call `POST /api/launch/<TID>` with curl). Expected:
    the button is off; curl gets 409 `a game is running`, and nothing is launched.
  - With a game "Building" or in "Error": the button is off (409 `the game is not ready`).
  - With ShadowMountPlus running: off (409 `MaxPack does not mount now ...`).
  - From another website (a page that does `fetch('http://<ip>:8095/api/launch/<TID>', {method:
    'POST'})`): 403 and `web: refused` in the log; nothing is launched.
- [ ] **7.3. How a game exits, unexpected closes and "Copy report".**
  - Exit a game normally. Expected: the line `[launch] <TID> exit pid=... sandbox=...: exited with N`
    or `: killed by signal N` (the console usually closes the game with a signal), and no "crashed".
  - If a game closes by itself during the tests (or with one you know fails): in the log,
    `<TID>: crashed (eboot.bin, killed by signal 11), dump found at exit` (or `... ms after exit`) and
    the folder `/user/devlog/system/sce_coredumps.0/<TID>_<n>`; in `/data/maxpack/crashes.json`, the
    title with `at`, `process`, `folder` and `status`; on the sheet, the "Last crash" row and the red
    note. It is still there after restarting MaxPack or the console. If there was no dump, the log
    says `no crash dump in ...`.
  - On the sheet, press "Copy report" from the PC or the phone. Expected: "Report copied." and, when
    pasted, the report: MaxPack's version, **the console's firmware** (check it in Settings > System),
    the mounter, the game, source, status, ampr-emu-ng, the PlayGo stub, last crash, the row for
    `compatibility.md` (no game name or title ID in it: format, drive, firmware, MaxPack's version and
    the Fakelib mode, and `?` for the rest) and the last 40 log lines with its `TITLE_ID`.
  - From the PS5's browser: if it cannot copy, the text is shown selected below, to copy by hand.
    Also at `http://<console-ip>:8095/api/report/<TID>`.
  - If the firmware shows as `(unknown: please fill in)`, note it: the console did not give it.
- [ ] **7.4. Hide and show a game.**
  - On a game's sheet, "Hide from the library". Expected: it disappears from the library and the
    counters; next to the search box "Show hidden 1" appears; in `/data/maxpack/config.toml`,
    `hidden = ["<TID>"]`; in the log, `<TID>: hidden in the library`.
  - The game stays on the home screen and starts from its tile (it is still mounted and registered).
  - Press "Show hidden". Expected: it shows dimmed, with "Hidden" on its label; from its sheet,
    "Show in the library" brings it back.
  - Save the Settings with the game hidden and restart MaxPack. Expected: it stays hidden.
- [ ] **7.5. Search and filter by drive.**
  - Type part of a name (lowercase, with no accents) or of a `TITLE_ID` in "Search games". Expected:
    only the matches show; with none, "No games match "...""; Escape clears it.
  - Press a drive's pill (Internal, USB 1). Expected: it turns solid and only that drive's games
    show; another press shows them all. "Unmount" keeps working.
- [ ] **7.6. Rebuild cache, only once.**
  - Press "Rebuild cache" and confirm. Expected: the button stays off while it is queued, while it is
    built and while it is registered, and comes back when the game is "Ready"; in the log, a single
    `rebuild requested`.
- [ ] **7.7. The space before installing.**
  - In Install, pick a game bigger than a drive's free space.
  - Expected: that drive says "Does not fit: ... more needed" in red; if you pick it and press
    "Install", "Not enough space: ... more needed." appears and no copy starts (nothing new in
    `/data/maxpack/tmp/`). On the other drive, if it fits, it installs normally.
- [ ] **7.8. The page after rest mode.**
  - With MaxPack running, put the console in rest mode and wake it (wait a minute in rest mode).
  - Expected: the page answers again within a few seconds without sending the ELF again. If rest mode
    closed the port, in the log `web: accept on port 8095 failed: ...; binding it again` and `web UI
    on port 8095 again`; if it did not close it, nothing. A second copy is not started.
- [ ] **7.9. A game that launches an ELF on exit.**
  - With a `.pkg` (or a `.p5z`) whose game, on exit or from a menu, launches another `*.elf`
    executable (a launcher or a tool inside its folder).
  - Expected: in the cache (`/data/maxpack/caches/<TID>/`) that `*.elf` is there loose (never those in
    a `.pkg`'s `decrypted/`); at launch, `exec pid=... overlay already there` and `/data` and
    `/app0/kraken` (or `/app0/p5z`) are mounted again; the ELF starts and the console does not hang on
    exit.
- [ ] **7.10. The console's fonts inside the game.**
  - Launch any MaxPack game. Expected: in the log, `<TID> sandbox /preinst/common/font MOUNTED:
    /preinst/common/font -> /mnt/sandbox/<TID>_<n>/preinst/common/font` after the `/data` and USB
    lines; on exit, `cleanup done` and the console does not hang.
  - With a homebrew that writes Chinese, Japanese, Arabic or Thai with the system fonts (like
    ProsperoEden): expected, the text shows, with no empty boxes.
  - If MaxPack closes with a game open (a crash) and you load it again: the line `<TID> adopted
    <sandbox>: 1 layer(s), N data mount(s)` counts the fonts too, and exiting the game does not hang.
- [ ] **7.11. Excluded paths.**
  - With a folder game already registered in `/data/homebrew/<folder>`, add that path in Settings >
    "Excluded paths" and save. Expected: in `config.toml`, `exclude = ["/data/homebrew/<folder>"]`; on
    the next pass the game no longer shows in the library (neither as a duplicate nor with an error),
    but it stays on the home screen and starts from its tile; there is no `building cache` for it.
  - Restart the console with the path still excluded. Expected: the tile stays on the home screen,
    but the game no longer starts (MaxPack does not mount `/system_ex/app/<TID>` again for a game it
    does not scan; with a USB drive the same happens when you unplug and plug it in). When you remove
    the path from the excluded ones, the next pass mounts it and it starts again.
  - With the path excluded, press "Purge cache" in Settings. Expected: its cache is deleted and
    `/system_ex/app/<TID>` is unmounted (it is not among the games that are scanned); if it was a
    `.pkg` or a `.p5z`, its tile no longer starts until you remove the path from the excluded ones and
    it is rebuilt.
  - Exclude a folder that has an unreadable subfolder or a damaged `.p5z` inside. Expected: no error
    or notice about what is inside (it is not opened).
  - Type a relative path (`homebrew/x`) and save. Expected: "Save failed" with `exclude: expected an
    absolute path ...`, and nothing changes.
  - Remove the path and save. Expected: the game is back in the library on the next pass.
- [ ] **7.12. A game without MaxPack's libraries.**
  - Open the sheet of an **unpacked** folder game and turn off "MaxPack libraries at launch".
    Expected: in the log, `<TID>: overlay off` and, on the next pass, `<TID>: fakelib: overlay off
    (overlay_off)`; `/data/maxpack/caches/<TID>/fakelib/` is left empty; the sheet says "Runs without
    MaxPack's libraries: ..."; "Copy report" has `MaxPack overlay: off`.
  - Launch the game. Expected: `<TID> empty fakelib, nothing to overlay; not mounting` and no
    `overlay MOUNTED` or `sandbox ... MOUNTED` line for it; the game starts as registered (one that
    needs neither ampr-emu-ng nor the emulators). On exit, the console does not hang.
  - Turn it on again. Expected: `<TID>: overlay on`, `fakelib/` fills up again and at launch
    `overlay MOUNTED` appears as usual.
  - On the sheet of a `.pkg`, a `.p5z` or a zstd or LZ4 folder the switch does not show. With curl,
    `curl -X POST --data off http://<ip>:8095/api/overlay/<TID>` on one of them answers 409 with the
    reason (ampr-emu-ng's pkg or p5z mode, or the packs) and changes nothing.
  - In `/api/state`, each game carries `registered`, `mounted`, `source_present` and `overlay_off`: a
    ready, mounted game shows `registered: true` and `mounted: true`; after unplugging its USB drive,
    `source_present: false` until the pass removes it.
- [ ] **7.13. The fakelib mode of a game.**
  - Take a `.pkg` or `.p5z` game whose dump ships its own `fakelib/` (a backport with one works too). Open its sheet,
    set "Fakelib" to "Off". Expected: in the log, `<TID>: fakelib mode off` and, on the next pass,
    `<TID>: fakelib: mode off (only ampr-emu-ng)` followed by no `fakelib: added` line;
    `/data/maxpack/caches/<TID>/fakelib/` holds only `libSceAmpr.sprx`; `config.toml` has the title id in
    `fakelib_off`; `/api/state` shows
    `"fakelib_mode": "off"`; "Copy report" has `Fakelib mode: off (only ampr-emu-ng)`; if the sheet shows "PlayGo
    stub", it is greyed with a note that it has no effect while Fakelib is Off.
  - Launch it. Expected: `overlay MOUNTED` as usual, and the game reads its files (ampr-emu-ng's line on screen).
    Note whether it gets further than in "Full" (the main menu, past the add-on check).
  - Set it to "Emulators only". Expected: `fakelib: mode emulators (the game's and backport's fakelib not used)`, and
    `fakelib/` holds the bundled emulators the game needs (`fakelib: added ...`) and `libSceAmpr.sprx`, none of the
    dump's own libraries; the title id moved from `fakelib_off` to `fakelib_emulators`.
  - Set it back to "Full". Expected: no `fakelib: mode` line, `fakelib/` as before (the dump's libraries again), the
    title id in neither list. Other games' caches are not refreshed by any of this, nor by the update itself (no
    `refreshing fakelib only` line for them after installing this version).

## 8. The self-update

The secure-connection check and the update notice, with MaxPack alone and the console online. Until
`v1.0.0` is published there is no release to offer: 8.1 and 8.2 are what can be checked before it.

- [ ] **8.1. Check secure connections (it must pass before `v1.0.0` is tagged).**
  - In Settings, press "Check secure connections". Expected, within a minute: "Passed: this console
    refuses forged certificates."; under the row, `untrusted-root.badssl.com`, `wrong.host.badssl.com`
    and `expired.badssl.com` "refused, as expected" with a code (`0x80435060` or similar, maybe with
    `SSL 0x..`), and `badssl.com` and `api.github.com` "connected, as expected (HTTP ...)"; in the log,
    one `tls check:` line per site and `tls check: passed; the self-update is allowed`; in
    `/data/maxpack/tls_check.json`, `"passed":true`.
  - Note the five codes and the line `sceHttpsEnableOption ... failed` if the log has it.
  - If a bad site says "accepted, but it must be refused": the console does not check certificates.
    Stop: report it with the five lines; nothing may be released with the self-update until it is fixed.
  - If a bad site says "failed before the certificate check": note its code. Without internet that is
    expected (the result is "Could not finish" and nothing changes); with internet, the code is one
    `maxpack_logic::tls` does not know yet.
  - Restart MaxPack. Expected: Settings says "Passed on <date>" (the result is kept).
- [ ] **8.2. Nothing is installed before the check passes.**
  - Delete `/data/maxpack/tls_check.json` and restart MaxPack. Expected: Settings says "Not checked
    yet"; `curl -X POST -H 'Content-Type: application/json' --data '{"version":"v9.9.9"}'
    http://<console-ip>:8095/api/self_update` answers 400 `not_offered` (no release offered), and, when a
    newer release is offered, 409 `tls_unchecked` with nothing downloaded (no `update:` download line in
    the log).
  - With a newer release published: the notice says MaxPack checks the console's certificates first and
    its button is "Check secure connections"; once it passes, the notice offers "Update".
- [ ] **8.3. Update (once a release newer than the running one is published).**
  - Press "Update". Expected: the notice says it is downloading and checking against `SHA256SUMS`; in
    the log, `update: v... (N bytes) matches SHA256SUMS and went to elfldr`; the new copy takes port
    8095 and the page reloads with the new version.
  - Start an install in the Install tab and press "Update" while it copies. Expected: "the installer is
    copying a game; try again when it ends.", also when the install starts during the download.

## 9. 1.0.1: the ampr-emu-ng update is checked

With a 1.0.1 build, MaxPack alone and the console online. Since 1.0.1 the ampr-emu-ng update waits for
the secure-connection check and installs `libSceAmpr.sprx` only when it matches the release's
`SHA256SUMS`, as the self-update does. Note what `/data/maxpack/ampr/` holds first: a hand-placed
`libSceAmpr.sprx` is kept by the automatic update and moved to `/data/maxpack/libSceAmpr.sprx.hand`
by "Update ampr-emu-ng now" (it asks first).

> With a build that bundles `v0.5.1-maxpack.2` (section 11) while its release is not published, the latest
> release is the older `v0.5.1-maxpack.1`: "Update ampr-emu-ng now" refuses it ("Update failed: the latest
> release is older than the ampr-emu-ng MaxPack uses now, so nothing was installed: ..."; in the log
> `ampr: update: nothing installed (older): ...`), and 9.2's install does not happen until a newer release
> exists.

- [ ] **9.1. Refused before the secure-connection check.**
  - Delete `/data/maxpack/tls_check.json` and restart MaxPack. Expected: in Settings, under
    ampr-emu-ng, "ampr-emu-ng updates (at start or with this button) stay off until the
    secure-connection check below passes on this console."
  - Press "Update ampr-emu-ng now". Expected: "Update failed: the secure-connection check has not
    passed on this console (Settings > Secure connections).", at once (nothing is downloaded); in the
    log, `ampr: update: nothing installed (tls_unchecked): ...`;
    `/data/maxpack/ampr/libSceAmpr.sprx` and `/data/maxpack/ampr.bundled` unchanged. With curl,
    `curl -X POST http://<console-ip>:8095/api/ampr/update` answers 409 with
    `"code":"tls_unchecked"`.
  - Turn on "Auto-update ampr-emu-ng", save and restart MaxPack. Expected: the same
    `nothing installed (tls_unchecked)` line once at start, and no notice.
- [ ] **9.2. Accepted after it.**
  - Run "Check secure connections" until it says "Passed" (8.1). Expected: the note under ampr-emu-ng
    goes away.
  - Press "Update ampr-emu-ng now". Expected: on the ampr-emu-ng row and in the log,
    `installed ampr-emu-ng v0.5.1-maxpack.1 (716176 bytes, matches SHA256SUMS)` (or the newer release
    with its size), and the notice "ampr-emu-ng v0.5.1-maxpack.1 installed". `v0.5.1-maxpack.1` is
    byte-identical to the bundled copy, so Settings still shows it as bundled.
  - With "Auto-update ampr-emu-ng" on, restart MaxPack. Expected: `ampr: update: latest
    v0.5.1-maxpack.1 skipped (...: installed v0.5.1-maxpack.1 is not older)`.
- [ ] **9.3. A forged `SHA256SUMS` is refused (by the unit tests).**
  - MaxPack takes ampr-emu-ng only from DeLFuS77/ampr-emu-ng's releases on GitHub, with no local file
    or other address it can be pointed at, so a forged, missing or malformed `SHA256SUMS` cannot be
    served to it on the console. Those cases are tested on the PC, and by CI on every push:
    `cargo test -p maxpack-logic ampr` (a good sum, a wrong sum, the debug build's sum on the sprx's
    line, a missing line, a malformed file, a release without `SHA256SUMS`, and the gate closed).
  - The page's side, without a console: `node tools/mock_ui.js --tls passed --ampr-fail checksum`, then
    Settings > "Update ampr-emu-ng now". Expected: "Update failed: the download does not match the
    release's SHA256SUMS: nothing was installed."

## 10. 1.0.2: recommended settings

With a 1.0.2 build on firmware 13.60. The bundled list (`data/recommended.json`, version 2026112501) has two
entries, keyed by a hash of the title id: a game that gets Fakelib **Off** on firmware 13.60 and later (the
"Off game" below), and a game with the notice `long_first_boot` (the "notice game"). To find out whether a
game is in the list, run `node tools/recommended_key.js <TITLE_ID>` on the PC and look for its key in the
file. Keep which games they are in your own notes: this checklist, the issues and the log lines you paste
name no game.

- [ ] **10.1. The list at start.**
  - Restart MaxPack. Expected in the log: `recommended: v2026112501 (2 titles, bundled)`, then
    `recommended: refresh skipped until the next start: the secure connection check has not passed` if 8.1 has
    not passed on this console, or, once it has, `recommended: kept v2026112501 (download is not newer)`
    (`downloaded v<version> (<n> titles)` if a newer list was pushed to `main`).
  - With the check not passed, run "Check secure connections" until it passes. Expected: no `recommended:`
    download line in this run; restart MaxPack: now `kept` (or `downloaded`) as above.
  - Turn off "Download updated recommended settings", save and restart MaxPack. Expected:
    `recommended: refresh off`. Turn it on again.
- [ ] **10.2. A game with a bundled recommendation shows the tag.**
  - First, the Off game must have no setting chosen by hand: it is in none of `fakelib_emulators`,
    `fakelib_off`, `playgo_off` and `manual_settings` in `config.toml`. On a console upgraded from 1.0.1
    where it was set to Off by hand it shows as your choice (no tag): press "Back to the recommended
    setting" in its sheet first (10.4).
  - Expected at the first pass: `<TID>: ...; refreshing fakelib only` and
    `<TID>: fakelib: mode off (only ampr-emu-ng), recommended setting` (no refresh when its `fakelib/` was
    already Off by hand).
  - Open its sheet. Expected: Fakelib shows Off with "Recommended setting" beside it, and no tag beside
    "PlayGo stub" when the sheet shows that switch (the list sets only Fakelib for this game); "Copy report"
    has `Fakelib mode: off (only ampr-emu-ng) (recommended)` and a PlayGo line without `(recommended)`.
  - Start it. Expected: it reaches its menu, as with Off set by hand.
- [ ] **10.3. A hand change wins.**
  - In the Off game's sheet, set Fakelib to Full. Expected: the tags go and "Back to the recommended
    setting" shows; in the log `<TID>: fakelib mode full`, then at the next pass
    `<TID>: ...; refreshing fakelib only`; in `config.toml`, the title in `manual_settings`.
  - Restart MaxPack. Expected: still Full, no tag, the button still there.
- [ ] **10.4. Back to the recommended setting.**
  - Press it. Expected: in the log `<TID>: back to the recommended settings (fakelib mode off, PlayGo stub
    on, recommended)` and, at the next pass, the fakelib refresh with `mode off (only ampr-emu-ng),
    recommended setting`; the tags are back and the button is gone; `config.toml` lists the title in none
    of the four lists.
- [ ] **10.5. The notice at launch.**
  - Open the notice game's sheet. Expected: the notice above its fields, "It can take several minutes to
    start, with a black screen. Wait." in the page's language; no "Recommended setting" tag (the list sets
    neither Fakelib nor PlayGo for it).
  - Start it from the home screen. Expected: at once, a console notification with the same text in the
    console's language; in the log `<TID>: notice long_first_boot`. Wait: the game starts after a few
    minutes. Start it again: the notification shows again. Started with Play from the page: the same.
  - Turn off "System notifications", save, start it again. Expected: no notification, and in the log
    `<TID>: notice long_first_boot not shown (notify off)`. Turn them on again.
- [ ] **10.6. A list that does not check out is never used.**
  - Over FTP (binary mode), put a file with the text `x` as `/data/maxpack/recommended.json` and restart
    MaxPack. Expected: `recommended: cached copy ignored: not JSON: ...`, then
    `recommended: v2026112501 (2 titles, bundled)`, and the Off game still tagged.
  - Replace it with `{"format": 1, "version": 1, "titles": {}}` and restart. Expected:
    `recommended: cached copy ignored: v1 is not newer than the bundled v2026112501`. Delete the file.
  - A download that is too large, too deep, of the wrong types, with 4097 titles or `"format": 2` cannot be
    served to the console (it takes only the repository's file); those are tested on the PC:
    `cargo test -p maxpack-logic recommended`.

## 11. Next version: a `.p5z` that grew

With a build of this branch, MaxPack alone, a game you already played from its `.p5z` in section 6
(packed with `maxpack pack --p5z`, a few GB, more than one volume if you can) and a MaxPack for the PC
that has `maxpack add` and `maxpack update` (its branch `pc-ampliar`, or the release that brings them). On the PC, `maxpack info <game.p5z>` says how many
times a `.p5z` grew and how much room it does not use. This build carries ampr-emu-ng
`v0.5.1-maxpack.2`, whose launch notice says "v0.5.1" as `v0.5.1-maxpack.1`'s does
(upstream's version): only Settings tells the two apart. Before anything, Settings shows ampr-emu-ng as bundled, `v0.5.1-maxpack.2`; if it says
downloaded or put there by hand, delete `/data/maxpack/ampr/libSceAmpr.sprx` and restart MaxPack.

- [ ] **11.1. A DLC added with `maxpack add`.**
  - On the PC: `maxpack add "<game>.p5z" --dlc "<DLC dump>"` on a copy of that `.p5z` (one with no DLC).
    Copy it to the console in place of the old one (FTP in binary mode, or a USB drive).
  - Expected: `building cache` again (another `.p5z`), and the game shows as ready.
    `/data/maxpack/caches/<TID>/maxpack.image` has one line more per volume the add wrote, after the old
    ones, with the old lines as they were. In the cache, `dlc_emu.ini` and the DLC's folder (`addcont0/`, for
    example).
  - Start it. Expected: "ampr-emu-ng: v0.5.1 (p5z) running", the DLC shows up in the game, 10 minutes of
    play, a save and its load, and `cleanup done` on exit. If "ampr-emu-ng: p5z: ...; packed files not
    served" appears, stop and keep the log, `maxpack.image` and, on the PC, `maxpack info` of the file.
- [ ] **11.2. A new version with `maxpack update`.**
  - On the PC: a `.p5z` of one version of a game and the dump of a newer one (another `contentVersion`),
    then `maxpack update "<game>.p5z" "<newer dump>"`. The files that changed leave room in the old
    volumes, maybe a volume with nothing left in it: that is what this tests.
  - Copy it to the console. Expected: `building cache`; in `/api/state` the game's `version` is the
    new one; `maxpack.image` lists every volume, the old ones too.
  - Start it. Expected: the notice "(p5z) running", the game says it is the new version, the save from the
    old version loads, 10 minutes of play without errors, and `cleanup done`.
  - Grow it once more (a second `maxpack update`, or an add) and test again: the same.
- [ ] **11.3. The installer refuses it.**
  - In Install, pick the grown `.p5z` (put it under `/mnt/usb0/install/`, as in section 5) and install it.
  - Expected: the job ends with "This .p5z grew on the PC (a DLC or an update was added to it), so it cannot
    be installed. ..." in the page's language, and the engine's English detail under it. Nothing is left
    on the destination drive.
  - On the PC, `maxpack compact "<game>.p5z" -o "<compact>.p5z"`. Expected: the compact copy installs as in
    5.1 and starts.
  - Grow on the PC the very `.p5z` that compact copy was installed from (`maxpack add` or `maxpack update`
    on that file, in place, so it keeps its `archive_id`), put it back where it was installed from, and press
    Verify on the installed game. Expected: "<game> verified." with `0 damaged` (the installed loose files are
    no longer compared with a `.p5z` that grew; only the folder's packs are checked).
- [ ] **11.4. An older ampr-emu-ng never starts it half-way.**
  - Over FTP (binary mode), put the `libSceAmpr.sprx` of ampr-emu-ng's release `v0.5.1-maxpack.1` in
    `/data/maxpack/ampr/` in place of the bundled one, with no game running. ("Update ampr-emu-ng now" does
    not do it: it refuses a release older than the bundled one, 9.)
  - Expected, at the next pass: in the log `<TID> (...): fakelib refresh: this .p5z grew (index version 2) and
    needs ampr-emu-ng v0.5.1-maxpack.2 or later; ...; rebuilding`, then the build fails with
    `... not started with it`; the game shows Error and its sheet says "This .p5z grew on the PC ... needs a
    newer ampr-emu-ng ..." in the page's language; `/api/state` has `error_code` `old_ng`; one notice
    "<game> needs a newer ampr-emu-ng: see the game on MaxPack's page"; Play is off.
  - Start its tile from the home screen. Expected: the console refuses to start it (its folder has no game
    files any more); it must never reach the game's own screens.
  - A `.p5z` that never grew (another game, or the compact copy of 11.3) still builds and plays.
  - Delete the file you put there and restart MaxPack. Expected: `ampr: installed bundled ampr-emu-ng
    v0.5.1-maxpack.2`, then `building cache` of the grown game, and it starts.
- [ ] **11.4b. A copy in the middle of a build (optional).**
  - Delete the grown game's cache ("Rebuild cache") so that it builds again, and while the log says
    `building cache` for it (a large `.p5z` on USB gives time), put that `v0.5.1-maxpack.1` file into
    `/data/maxpack/ampr/` over FTP.
  - Expected: the build ends with `... not started with it` (the copy in `fakelib/` is the one checked), or,
    if the file arrived after the copy, the next pass refreshes the fakelib and stops as in 11.4. Never
    "ready" with that file in `/data/maxpack/caches/<TID>/fakelib/`.
- [ ] **11.5. An append cut short on the PC (optional).**
  - On the PC, run `maxpack update` on a copy of a `.p5z` and end the process from the Task Manager
    while it writes: a `<game>.p5z.maxpack-append` file stays next to it. Copy both to the console without
    running MaxPack on the PC first.
  - Expected: the console plays the `.p5z` as it was before that update (the engine reads it through
    that file, and never cuts it), or as it grew if the update had ended before the process was stopped;
    MaxPack on the PC settles it the next time it opens it.

## 12. Compress on the console (test build)

With a build of the branch `consola-comprimir`, MaxPack alone. This is a prototype that measures, not the feature:
it packs a game folder into a `.p5z` on the console, as `maxpack pack --p5z` does on the PC, and tells how fast
each level is, how much memory it takes, whether the console stays usable meanwhile, and whether the volumes are
the same as the PC's. It does not pause when a game starts and does not keep the console awake.

You need: a small game you have as a folder (a few GB, a plain dump: no `ampr_zstd.index` or
`ampr_assets.index` in it) and, for 12.6, a large one; MaxPack for the PC 1.0.7 or later (it has
`maxpack info --sums`) with the same engine as this build (MaxPack 1.0.7, 5463d75: an engine that packs differently gives other
volumes), and the same folder on the PC, byte for byte (copied over FTP in binary mode, or the other way round).
Close every game before starting a run: MaxPack refuses to start one while any game runs.

Where to look: the game's sheet (the row "Compress on the console (test)" at the bottom), the log
(`/data/maxpack/maxpack.log` or the page's log: every line of the job starts with `pack:`), and `/api/state`,
field `pack`.

- [ ] **12.1. The folder and the test build.**
  - Copy the small game's folder to `/data/homebrew` over FTP in binary mode, if it is not there yet. Wait until it
    shows as ready in the library.
  - Send this build's `maxpack.elf` (port 9021).
  - Expected: the folder game's sheet ends with "Compress on the console (test)", with Level (3, 9, 19) and
    Threads (4, 5). A `.p5z` game's sheet and a packed folder's do not have it.
- [ ] **12.2. Level 3.**
  - On the small game's sheet: Level 3, Threads 5, "Compress (test)". Write down the time it shows at the end.
  - Expected while it runs: "measuring the memory" for a second or two, then "loose files", then "packs", with
    the percentage, the MB/s and the time; the bar moves. In the log, first
    `pack: memory probe: N MiB in steps of 64 MiB, ...; stopped by ...` (keep that line: it is how much memory the
    payload gets, up to 1 GiB; the probe never takes the console's available memory under 1 GiB, and it runs only
    once per MaxPack start), a `pack:` line every minute, then one line per phase (`Copying the loose files: ...
    MB/s`, `Packing (zstd level 3): ... MB/s`), the payload's RSS at its peak, and `pack: done: <folder>.p5z
    (archive_id ..., N volumes)`.
  - If the probe misbehaves (while the page says "measuring the memory" the console shows a memory warning, an app
    or the home screen closes or freezes, or the page stops answering for more than a few seconds): press "Cancel
    the compression" if you still can, start no other run, and send `/data/maxpack/maxpack.log` (FTP) with what you
    saw. If the console needed a restart, say so; do not send this build again until the log has been read.
  - Expected at the end: "Done in ...", a box with the lines, and "Copy the lines". Next to the folder there is
    `<folder>.p5z` (and no `.maxpack-pack` file); the library lists it as a duplicate of the folder. The file takes
    that name only after its lines were read back from it: while the sheet says "reading the sums" it is still
    `<folder>.p5z.maxpack-pack`, and a run that fails there leaves it under that name (never as `<folder>.p5z`),
    with the sheet saying so; the next run of that folder removes it.
  - Press "Copy the lines" and keep them (paste them into a text file on the PC). They hold no game name.
  - While it runs, use the console: go to the home screen, open a menu or two, scroll the page. Note anything slow.
- [ ] **12.3. Levels 9 and 19, and 4 threads.**
  - MaxPack never overwrites a `.p5z`: before each run, delete `<folder>.p5z` over FTP (or move it somewhere
    else). If you start one with the file there, the sheet says it is there already and nothing starts.
  - Run level 9 with 5 threads, then level 9 with 4 threads, then level 3 with 4 threads. Level 19 with 5 threads
    once (it can take a long time; with 4 threads only if you have the time). Copy the lines of each run.
  - Expected: each run ends with "Done", and each run's lines say its level and threads (the lines that start with
    `#`). Two runs at the same level give the same `archive_id` and volume lines, whatever the threads. The
    memory probe does not run again: the log says `pack: memory probe: not run again (once per MaxPack start)`.
- [ ] **12.4. The same lines on the PC.**
  - On the PC, for each level you ran: `maxpack pack "<folder>" -o "<game>-L<level>.p5z" --p5z --level <level>`,
    then `maxpack info --sums "<game>-L<level>.p5z"` (MaxPack for the PC 1.0.7 or later). It prints `level N`,
    `archive_id <hex>` and one `ampr_zstd-NNN.pak <size> <sha256>` line per volume, as the console's lines do
    after their `#` notes.
  - Compare: save the console's lines of that level as `console.txt` and run, in Git Bash,
    `diff <(grep -v '^#' console.txt) <(maxpack info --sums "<game>-L<level>.p5z" | tr -d '\r')`.
  - Expected: no output from `diff` (the same `archive_id`, the same volume names and sizes, the same SHA-256).
    Send both sets of lines.
- [ ] **12.5. Play the `.p5z` the console made.**
  - Keep the last `.p5z` the console made. In Settings, add the game's folder to "Excluded paths" and save, so the
    `.p5z` is the library's copy (while the folder is listed, the folder wins).
  - Expected: the game builds again as a `.p5z` and shows as ready. Start it: the notice says "(p5z) running", it
    loads, and a save and its load work. Play 10 minutes.
  - Take the folder off "Excluded paths" when you are done: the folder is the game again.
- [ ] **12.6. A large game at level 3.**
  - Only if the small game went well. With the large game's folder (on the internal SSD or a USB drive with room
    for one more copy of it): level 3, Threads 5.
  - Expected: it ends with "Done". Note the time, the MB/s of the packs (the `pack:` line of each minute shows if
    the speed holds), the peak memory and how loud the fan gets.
  - Once, while it runs, start another game from the home screen (any game: one of MaxPack's library, an installed
    one or one another mounter started), play 2 minutes and close it. Expected: the job goes on (this test build
    does not pause); within 2 s the log says `pack: a game started while compressing (...)` with the speed before
    it, `pack: a game runs (...)` every 30 s, and `pack: the game exited (...)` with the speed while it ran. Note
    whether the game stutters or loads slowly.
  - Optional: leave the console alone until it goes into rest mode during a run, wake it, and note whether the job
    went on (the time and the `pack:` lines say it) or the page shows it failed.
- [ ] **12.7. Cancel and refusals.**
  - Start a run and press "Cancel the compression" during the packs. Expected: "Cancelled: nothing of the .p5z is
    left.", no `<folder>.p5z` and no `<folder>.p5z.maxpack-pack` next to the folder; in the log
    `pack: cancelled after ... s in the engine: cancelled`. If the sheet says instead that the `.maxpack-pack` file
    is still there, check it over FTP and note it.
  - With `<folder>.p5z` there, press "Compress (test)". Expected: the sheet says it is there already; the file is
    not touched.
  - The internal SSD keeps 2 GB free: with a folder game on `/data/homebrew` whose size plus 2 GB is more than the
    internal SSD's free space (fill it with a large file over FTP if you need to, and delete that file afterwards),
    press "Compress (test)". Expected: "It does not fit: N more is needed on the internal SSD (... 2 GB stay free
    there ...)", nothing is written next to the folder, and the log says `pack: ... refused: ... more is needed on
    the internal drive ...` with the folder's size and the free space. The same folder on a USB drive with only
    256 MB more free than its size does start.
  - A FAT32 USB drive (optional): put a folder game of more than 4 GB on it and press "Compress (test)". Expected:
    "That drive is FAT32, which keeps no file of 4 GB or more, and this folder is ...", nothing is written, and the
    log says `pack: ... refused: the drive is FAT32 ...` with `msdosfs` at the end of the line (if it shows another
    type for a FAT32 drive, note it). A folder of less than 4 GB on that drive starts as usual.
  - With a game running (start one from the home screen, also one MaxPack did not start), press "Compress (test)".
    Expected: "Close the game first." and nothing starts; in the log `pack: ... refused: a game is running (<TID>)`.
  - During a run, press Install or Verify in the Install tab. Expected: "A test compression is running: try again
    when it ends." and nothing starts.
  - During a run, open another folder game's sheet. Expected: it says another test compression is running, with its
    button off.

What passes: the `archive_id` and the volumes' SHA-256 are the PC's; level 9 goes over about 80 MB/s; the peak
memory stays under about 500 MB; and the console stays usable meanwhile. Send the lines of each run, the `pack:`
lines of the log, and what you noted (times, fan, menus, the game you started).
