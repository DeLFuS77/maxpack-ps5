#!/usr/bin/env node
// Development only: serves src/ui.html with fixed data in place of the payload, to look at the page
// (Library with ShadowMountPlus images, Install, the "installer only" banner, an install in
// progress) without a console. Nothing here goes into maxpack.elf.
//
//   node tools/mock_ui.js [--port 8095] [--waiting other|kstuff|none] [--job on|paused|off]
//                         [--last done|failed|needs_confirm|grown|none] [--empty]
//                         [--update v1.0.3|none] [--update-fail <code>|none]
//                         [--tls never|passed|broken] [--tls-result passed|broken|inconclusive]
//                         [--ampr-fail checksum|older|none] [--receive demo|failed|none] [--receive-delete-fail]
//   then open http://localhost:8095/ and the Install tab.
//
// Defaults: --waiting other (ShadowMountPlus running: the banner and ShadowMountPlus connected in
// Install), --job on (a .p5z being copied, ~2 % a second), --last none. --last grown: the last
// install refused a .p5z that grew (the engine's grown_p5z).
// --empty: no games, images, found items or installs (the empty states).
// --update: a newer release found at start (state.update), so the page shows its notice with
// Update and Later; Update (POST /api/self_update) "installs" it: 3 s later the mock answers with
// that version and the page reloads. --update-fail answers Update with that error code instead
// (maxpack_logic::update::Fail: game_running, scan_running, install_running, no_elfldr, download,
// checksum, send...). Defaults: none.
// --tls: the secure-connection check's record at start (state.tls_check, src/tls.rs): never run,
// passed, or broken (a forged certificate was accepted); until it is passed the update notice asks
// for the check instead of offering Update, and Update answers 409 tls_unchecked. --tls-result: what
// "Check secure connections" (POST /api/selftest/tls, 2 s) answers: passed (the record becomes
// passed), broken (it becomes broken) or inconclusive (no internet: the record stays). Defaults:
// --tls never, --tls-result passed.
// "Update ampr-emu-ng now" (POST /api/ampr/update, 1 s) answers 409 tls_unchecked until --tls is
// passed, as the payload (maxpack_logic::ampr); then it installs a newer release (Settings then shows it as
// downloaded), or, with --ampr-fail checksum, refuses the download as one that does not match the release's
// SHA256SUMS, or with --ampr-fail older, a release older than the bundled ampr-emu-ng. Default: none.
// The same switches at run time, without a restart (the page picks them up on its next poll):
//   http://localhost:8095/mock?waiting=none&job=paused&last=needs_confirm&empty=0&update=v1.0.3&update_fail=checksum&tls=never&tls_result=broken&ampr_fail=checksum
// The page's POSTs answer as the payload would (install queues a job, pause pauses it and it ends
// as an unfinished install, a path with "replace" in it asks to confirm a replace, and so on).
// Library samples: a hidden game (PPSA02345, shown with "Show hidden"), a game with a recorded crash
// (PPSA01234, its sheet shows it and Copy report copies GET /api/report), Play (POST /api/launch:
// the game "runs" for 8 s, then exits), Hide/Show (POST /api/hidden), and Rebuild (rebuild_queued
// for 3 s), the overlay switch (POST /api/overlay: only "Plain Racer", a plain folder game, shows it;
// "Shell Title" has it off; "Packed Example", a .p5z, is still listed from before, so its sheet shows
// the switch to clear it; turning a non-plain game off answers 409 as the payload does), the Fakelib selector
// (POST /api/fakelib: every game shows it; "Starfall Odyssey", a pkg, is set to Off, so its PlayGo switch is
// greyed; "Shell Title" has it greyed by its overlay off; a plain folder adds the "mounts nothing" line), and the Excluded paths
// of Settings. The recommended settings (src/recommended.rs): "Recommended Example", a pkg, gets Fakelib Off and a
// notice from the list (its sheet tags both values and shows the notice); "Chosen Example", a zstd folder, has a
// recommendation the user overrode by hand, so its sheet offers "Back to the recommended setting" (POST
// /api/recommended); a hand change of Fakelib or PlayGo (POST /api/fakelib, /api/playgo) makes a game's settings
// the user's, as the payload does. Install: "Huge Example" does not fit on the internal SSD (the page says so per drive);
// "Grown Example" in the found list grew on the PC, so the page says it is not installed and Install refuses it; a
// path added by hand with "grown" in its name is marked so too, and one with "newer" is refused as made by a newer
// MaxPack for the PC (newer_p5z), as the payload answers.
// "Grown Example", a .p5z that grew, is not started with the ampr-emu-ng in use (error_code old_ng: its sheet says why).
// That ampr-emu-ng is one put in /data/maxpack/ampr/ by hand, of a version MaxPack does not know (the only way a
// console gets one that does not play it: the bundled one does, and no update installs an older release), so
// Settings shows it as hand-placed and the game's error says its version is not known. Once "Update ampr-emu-ng
// now" has installed a newer release, the game is ready, as the console's next pass would make it.
// "Verify quickly" and "Verify fully" on a .p5z's sheet (POST /api/verify/<TID>, src/verify.rs): "Packed Example" shows an
// earlier OK, "Roomless Example" an earlier damaged result, and a quick verify of it names a volume and a file; a verify
// runs its quick phase, then (full) its full one (its Cancel works), and refuses as the payload does, so
// with the default --job on (an install running) it answers install_running: use --job off to watch one run. While it
// runs, the installer's install, verify and uninstall answer 409 console_busy.
// Every request passes the payload's checks (maxpack_logic::http, src/web.rs `refused`): Host must be
// an IPv4, an [IPv6] or localhost, with an optional port, so the page opened by a name
// (http://<pc-name>:8095/) gets 403 as on the console; and a POST from another site (an Origin that
// is not http(s)://<this Host>, or Sec-Fetch-Site: cross-site) gets 403. Opened at this PC's LAN IP
// (http://192.168.x.y:8095/) the "installer only" banner shows that address; on localhost or
// 127.0.0.1 it shows the http://<PS5-IP>:8095 placeholder (a loopback means nothing to a phone).
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const args = process.argv.slice(2);
function arg(name, def) {
  const i = args.indexOf('--' + name);
  return i >= 0 && args[i + 1] && !args[i + 1].startsWith('--') ? args[i + 1] : def;
}
const port = Number(arg('port', '8095'));
const mock = {
  waiting: arg('waiting', 'other'),
  job: arg('job', 'on'),
  last: arg('last', 'none'),
  empty: args.includes('--empty'),
  update: arg('update', 'none'),
  update_fail: arg('update-fail', 'none'),
  tls: arg('tls', 'never'),
  tls_result: arg('tls-result', 'passed'),
  ampr_fail: arg('ampr-fail', 'none'),
  receive: arg('receive', 'none'),
  receive_delete_fail: args.includes('--receive-delete-fail'),
};
// The running version: the one an Update in the mock "installs" once it is done.
let version = '1.0.2';
// What /data/maxpack/ampr/libSceAmpr.sprx is (GET /api/ampr, main::ampr_status): hand-placed, as the Grown Example
// needs, until "Update ampr-emu-ng now" installs a newer release.
let ampr = { kind: 'hand', version: null };
// state.update as src/update.rs `available` gives it: a newer release, or null.
function updateValue() {
  if (!mock.update || mock.update === 'none' || mock.update.replace(/^v/, '') === version) return null;
  return { version: mock.update, size: 3456789, tls_passed: mock.tls === 'passed' };
}
// state.tls_check as src/tls.rs `summary` gives it; tlsAt: when the record was written.
let tlsRunning = false, tlsAt = Math.floor(Date.now() / 1000) - 86400;
function tlsCheckValue() {
  const v = mock.tls === 'passed' || mock.tls === 'broken' ? { passed: mock.tls === 'passed', at: tlsAt, version } : {};
  return Object.assign(v, { running: tlsRunning });
}
// POST /api/selftest/tls's answer for mock.tls_result, probe by probe (maxpack_logic::tls::PROBES).
function tlsAnswer() {
  const r = mock.tls_result;
  const refused = (url) => r === 'broken' && url.includes('wrong.host')
    ? { url, expect: 'refused', check: 'accepted', status: 200 }
    : r === 'inconclusive'
      ? { url, expect: 'refused', check: 'unproven', code: '0x80436002', error: `sceHttpSendRequest failed for ${url}: 0x80436002` }
      : { url, expect: 'refused', check: 'ok', code: '0x80435060', ssl_detail: url.includes('expired') ? '0x28' : url.includes('wrong.host') ? '0x04' : '0x20',
        error: `sceHttpSendRequest failed for ${url}: 0x80435060` };
  const connects = (url) => r === 'inconclusive'
    ? { url, expect: 'connects', check: 'unproven', code: '0x80436002', error: `sceHttpSendRequest failed for ${url}: 0x80436002` }
    : { url, expect: 'connects', check: 'ok', status: 200 };
  const probes = [refused('https://untrusted-root.badssl.com/'), refused('https://wrong.host.badssl.com/'),
    refused('https://expired.badssl.com/'), connects('https://badssl.com/'), connects('https://api.github.com/')];
  const verdict = r === 'broken' ? 'broken' : r === 'inconclusive' ? 'inconclusive' : 'passed';
  return { verdict, probes, verify_option: 'set' };
}
// maxpack_logic::update::Fail: the status of each code, and its English detail.
const UPDATE_FAILS = {
  not_offered: [400, 'not the newer release found at start'],
  tls_unchecked: [409, 'the secure-connection check (Settings) has not passed on this console'],
  game_running: [409, 'a game is running: close it first'],
  update_running: [409, 'an update is already running'],
  scan_running: [409, 'a scan or build is in progress; try again in a moment'],
  install_running: [409, 'the installer is copying a game; try again when it ends'],
  no_elfldr: [503, 'elfldr does not answer: 127.0.0.1:9021: connection refused'],
  download: [502, 'download failed: GET https://github.com/DeLFuS77/maxpack-ps5/releases/download/v1.0.3/maxpack.elf: HTTP status 404'],
  checksum: [502, 'not the released maxpack.elf, not sent: SHA-256 0a1b... is not the 9f8e... that SHA256SUMS lists'],
  send: [502, 'sending it to elfldr failed: 3456789 bytes to 127.0.0.1:9021: broken pipe'],
};

const GB = 1024 * 1024 * 1024;
const MB = 1024 * 1024;
// Fixed, so the page does not ask for the cards' art again on every poll.
const BUILT = Math.floor(Date.now() / 1000) - 3600;
// The page's switches and actions, as the payload keeps them.
const hidden = new Set(['PPSA02345']);
const overlayOff = new Set(['PPSA06100', 'PPSA07100']);
// The fakelib mode of each title not in "full" (fakelib_emulators, fakelib_off).
const fakelibMode = new Map([['PPSA10001', 'off']]);
// playgo_off, and manual_settings: the titles whose settings the user chose.
const playgoOff = new Set();
const manual = new Set(['PPSA11002']);
// The recommended settings (data/recommended.json, keyed here by title id for the mock only).
const recommended = new Map([
  ['PPSA11001', { fakelib: 'off', notice: 'long_first_boot' }],
  ['PPSA11002', { fakelib: 'emulators', playgo_off: true }],
]);
// maxpack_logic::recommended::effective: the user's when any of the four lists names the title, else the list's.
function settingsOf(tid) {
  const rec = recommended.get(tid) || {};
  const has = rec.fakelib != null || rec.playgo_off != null;
  const user = manual.has(tid) || fakelibMode.has(tid) || playgoOff.has(tid);
  return {
    fakelib_mode: user ? fakelibMode.get(tid) || 'full' : rec.fakelib || 'full',
    playgo_off: user ? playgoOff.has(tid) : !!rec.playgo_off,
    settings_source: user ? 'user' : has ? 'recommended' : 'default',
    recommended: has ? { fakelib: rec.fakelib || null, playgo_off: rec.playgo_off == null ? null : rec.playgo_off } : null,
    notice: rec.notice || null,
  };
}
// maxpack_logic::recommended::hand_choice: before a hand change, the list's values become the user's.
function handChoice(tid) {
  const s = settingsOf(tid);
  if (s.settings_source === 'recommended') {
    if (s.fakelib_mode === 'full') fakelibMode.delete(tid); else fakelibMode.set(tid, s.fakelib_mode);
    if (s.playgo_off) playgoOff.add(tid); else playgoOff.delete(tid);
  }
  manual.add(tid);
}
const queued = new Set();
let running = null; // { tid, until }
function runningNow() {
  if (running && Date.now() > running.until) running = null;
  return running ? [running.tid] : [];
}

function waitingValue() {
  if (mock.waiting === 'other') return 'other:shadowmountplus.elf';
  if (mock.waiting === 'kstuff') return 'kstuff';
  return null;
}

// ---- "Verify quickly" and "Verify fully" on a .p5z's sheet (src/verify.rs) ----
// The kept results by path (verified.json): "Packed Example" was verified OK a day ago, "Roomless Example" found
// damaged. A verify of this run (state.verify) runs as verifyStart says; "Roomless Example" is found damaged again.
const verified = new Map([
  ['/mnt/usb0/homebrew/Packed Example.p5z', { kind: 'full', at: BUILT - 82800, ok: true, files_ok: 1532, files_bad: 0, bad: [] }],
  ['/mnt/usb0/homebrew/Roomless Example.p5z', { kind: 'full', at: BUILT - 7200, ok: false, files_ok: 2101, files_bad: 3, bad: [] }],
]);
let verifyJob = null, verifyTimer = null;
// src/jobs.rs: a console job runs (a verify, a receive from the PC).
function consoleBusy() { return verifyRunning() || recvUnder(); }
function withVerified(g) { g.verified = g.kind === 'p5z' ? verified.get(g.path) || null : null; return g; }
function verifyRunning() { return !!verifyJob && verifyJob.state === 'running'; }
function verifyEnd(state, error) {
  clearInterval(verifyTimer);
  Object.assign(verifyJob, { state, cancelling: false, error: error || null, mbps: null });
}
// maxpack_logic::verify::gate: in the library, a .p5z, no other job of the engine, no game running.
function verifyStart(tid, body) {
  if (body !== 'quick' && body !== 'full') return [400, 'bad_request', 'expected the body quick or full'];
  const g = games().find((x) => x.title_id === tid);
  if (!g) return [404, 'not_found', 'not a game of the library'];
  if (g.kind !== 'p5z') return [409, 'not_p5z', 'only a .p5z is verified on the console'];
  if (currentJob()) return [409, 'install_running', 'the installer is running a job; try again when it ends'];
  if (verifyRunning()) return [409, 'verify_running', 'a .p5z is being verified on the console; try again when it ends'];
  if (recvUnder()) return [409, 'receive_running', 'a receive from the PC is under way; try again when it ends'];
  if (runningNow().length) return [409, 'game_running', 'a game is running: close it first'];
  // The quick phase reads the .p5z (10 % a second), the full one unpacks more (5 % a second); a damaged copy stops
  // after the quick one, as the payload does.
  const start = Date.now(), damaged = /Roomless/.test(g.path), at = () => Math.floor(Date.now() / 1000);
  verifyJob = { state: 'running', cancelling: false, title_id: tid, path: g.path, kind: body, phase: 'quick', done: 0,
    total: g.size, mbps: null, elapsed_ms: 0, error: null };
  verifyTimer = setInterval(() => {
    const j = verifyJob, step = j.phase === 'quick' ? 0.1 : 0.05;
    j.elapsed_ms = Date.now() - start;
    if (j.cancelling) return verifyEnd('cancelled');
    j.done = Math.min(j.total, j.done + Math.round(j.total * step));
    j.mbps = j.total * step / 1e6;
    if (j.done < j.total) return;
    if (j.phase === 'quick' && (body === 'quick' || damaged)) {
      verified.set(g.path, { kind: 'quick', at: at(), ok: !damaged, files_ok: damaged ? 36 : 38, files_bad: damaged ? 2 : 0,
        bad: damaged ? [{ what: 'volume', name: 'ampr_zstd-002.pak' }, { what: 'file', name: 'sce_sys/param.json' }] : [] });
      return verifyEnd('done');
    }
    if (j.phase === 'quick') return Object.assign(j, { phase: 'full', done: 0, total: Math.round(g.size * 1.6), mbps: null });
    verified.set(g.path, { kind: 'full', at: at(), ok: true, files_ok: 1532, files_bad: 0, bad: [] });
    verifyEnd('done');
  }, 1000);
  return null;
}
// maxpack_logic::verify::report_line: the report's line, which names no file of the game (volume names only).
function verifiedLine(r) {
  if (!r) return 'never';
  const at = new Date(r.at * 1000).toISOString().replace('T', ' ').replace(/\.\d+Z$/, 'Z'), all = r.files_ok + r.files_bad;
  const unit = r.kind === 'quick' ? 'volumes and files' : 'files';
  if (r.ok) return `${r.kind}, OK on ${at} (${all} ${unit})`;
  const vols = r.bad.filter((b) => b.what === 'volume').map((b) => b.name), files = r.bad.length - vols.length;
  return `${r.kind}, damaged on ${at}: ${r.files_bad} of ${all} ${unit} fail` + (vols.length ? '; volumes: ' + vols.join(', ') : '') +
    (files ? '; files: ' + files : '');
}

// ---- the payload's own API (src/web.rs) ----
function games() {
  if (mock.empty) return [];
  // src/verify.rs `last`: a .p5z's last verify (by path), else null.
  // src/web.rs game_json: with the bundled emulators off, no PlayGo stub is ever added (playgo null, playgo_off false).
  const emus = (x) => settings.add_emulators ? x : Object.assign(x, { playgo: null, playgo_off: false });
  const g = (tid, name, kind, packing, status, extra) => emus(withVerified(Object.assign({
    title_id: tid, title_name: name, version: '01.000.000', path: '/data/homebrew/' + tid, kind, packing,
    drive: '/data', size: kind === 'pkg' ? 38 * GB : 0, status, error: null, installed_copy: null, cache_size: 210 * MB,
    cache_built: BUILT, error_code: null, missing_bytes: null, backport: null, playgo: false,
    hidden: hidden.has(tid), crash: null, rebuild_queued: queued.has(tid),
    registered: status === 'ready', mounted: status === 'ready', source_present: true,
    overlay_off: kind === 'folder' && packing === 'plain' && overlayOff.has(tid), overlay_off_listed: overlayOff.has(tid),
  }, settingsOf(tid), extra || {})));
  return [
    g('PPSA01234', 'Astro Example', 'pkg', null, 'ready', { path: '/mnt/usb0/homebrew/astro.pkg', drive: '/mnt/usb0',
      crash: { at: BUILT + 600, process: 'eboot.bin', status: 'killed by signal 11' } }),
    g('PPSA02345', 'Folder Quest', 'folder', 'zstd', 'ready'),
    g('PPSA03456', 'Plain Racer', 'folder', 'plain', 'building', { cache_built: null, cache_size: 0 }),
    g('PPSA04567', 'Lz4 Tactics', 'folder', 'lz4', 'pending'),
    // A plain folder game with MaxPack's overlay turned off (overlay_off): it runs as registered.
    g('PPSA06100', 'Shell Title', 'folder', 'plain', 'ready'),
    // A .p5z in a scanned root: ready, and one short of space on the internal SSD (shows the size missing).
    g('PPSA07100', 'Packed Example', 'p5z', null, 'ready', { path: '/mnt/usb0/homebrew/Packed Example.p5z', drive: '/mnt/usb0', size: 21 * GB }),
    g('PPSA07200', 'Roomless Example', 'p5z', null, 'error', { path: '/mnt/usb0/homebrew/Roomless Example.p5z', drive: '/mnt/usb0',
      size: 33 * GB, cache_built: null, cache_size: 0, error_code: 'no_space', missing_bytes: Math.round(3.84 * GB),
      error: 'PPSA07200 (/mnt/usb0/homebrew/Roomless Example.p5z): no space' }),
    g('PPSA05678', 'Broken Sample', 'pkg', null, 'error', { error: 'PPSA05678 (/data/homebrew/broken.pkg): bad header' }),
    // A .p5z that grew on the PC, and a hand-placed ampr-emu-ng in /data/maxpack/ampr, whose version is not known
    // (cache::OldNg); ready once an update put a release in its place (the install wakes the scan, which rebuilds it).
    ampr.kind === 'hand'
      ? g('PPSA00000', 'Grown Example', 'p5z', null, 'error', { path: '/mnt/usb0/homebrew/Grown Example.p5z', drive: '/mnt/usb0',
        size: 24 * GB, cache_built: null, cache_size: 0, error_code: 'old_ng',
        error: 'PPSA00000 (/mnt/usb0/homebrew/Grown Example.p5z): cache build: this .p5z grew (index version 2) and needs ampr-emu-ng v0.5.1-maxpack.2 or later; the version of the ampr-emu-ng in use is not known (/data/maxpack/ampr/libSceAmpr.sprx): not started with it' })
      : g('PPSA00000', 'Grown Example', 'p5z', null, 'ready', { path: '/mnt/usb0/homebrew/Grown Example.p5z', drive: '/mnt/usb0', size: 24 * GB }),
    // A pkg whose fakelib mode is Off: its fakelib/ holds only ampr-emu-ng.
    g('PPSA10001', 'Starfall Odyssey', 'pkg', null, 'ready', { path: '/data/homebrew/Starfall Odyssey.pkg', playgo: true }),
    // The recommended settings: Fakelib Off and a notice from the list; and a recommendation the user overrode.
    g('PPSA11001', 'Recommended Example', 'pkg', null, 'ready', { path: '/data/homebrew/Recommended Example.pkg', playgo: true }),
    g('PPSA11002', 'Chosen Example', 'folder', 'zstd', 'ready', { playgo: true }),
    // A dump whose title id is installed on the console: left alone, the sheet says why.
    g('PPSA06000', 'Installed Twin', 'folder', 'plain', 'error', { cache_built: null, cache_size: 0,
      error: 'PPSA06000 (/data/homebrew/PPSA06000): an installed copy of this title exists (/user/app/PPSA06000/app.pkg): MaxPack does not register or mount this copy',
      installed_copy: '/user/app/PPSA06000/app.pkg' }),
  ];
}

function apiState(req) {
  return {
    version, update: updateValue(), tls_check: tlsCheckValue(), busy: false, paused: runningNow().length > 0, waiting: waitingValue(), running: runningNow(),
    games: games(),
    // Settings > "Add the bundled emulators" (src/web.rs api_state): the reset line names the PlayGo stub only with it on.
    add_emulators: settings.add_emulators,
    images: mock.empty ? [] : [
      { path: '/data/homebrew/Sample Game [PPSA06789].exfat', kind: 'exfat', title_id: 'PPSA06789' },
      { path: '/mnt/usb0/homebrew/misc/another.ffpkg', kind: 'ffpkg' },
    ],
    unplayable: mock.empty ? [] : [
      { path: '/mnt/usb0/homebrew/Broken Pack.p5z', code: 'damaged', detail: 'volume 2 is incomplete' },
      { path: '/mnt/usb0/homebrew/Huge Pack.p5z', code: 'too_many_volumes', detail: '300' },
      // Made by a newer MaxPack for the PC (its index version 3): listed as such, never as damaged.
      { path: '/mnt/usb0/homebrew/Future Pack [PPSA05557].p5z', code: 'newer', detail: '3' },
    ],
    drives: [
      { mount: '/mnt/usb0', label: 'USB 1', kind: 'usb', total: 1000 * GB, free: 412 * GB, ejectable: true },
      { mount: '/data', label: 'Internal', kind: 'internal', total: 825 * GB, free: 311 * GB, ejectable: false },
    ],
    cache: { path: '/data/maxpack/caches', size: 2 * GB, free: 311 * GB, low_space: false, ampr_present: true },
    verify: verifyJob,
    receive: recvState(),
    receive_left: leftState(),
    pairing: { on: pairing.on, code: isLoopback(req) && pairing.code ? pairing.code : null, here: isLoopback(req) },
    log: ['2026-11-23 20:00:00Z [1000] web UI on port 8095', '2026-11-23 20:00:02Z [3000] installer: ready in 412 ms'],
  };
}

const settings = {
  scanpath: [], exclude: ['/data/homebrew/old-copies'], inject_ampr: true, add_emulators: true, ampr_auto_update: false,
  recommended_update: true, backports: true,
  scan_interval_seconds: 10, stability_wait_seconds: 10, auto_remove_missing: true,
  auto_remove_delay_seconds: 300, low_space_gb: 20, notify: true, log: true, debug: false,
};

// ---- the installer (/api/inst/<x>: the engine's /api/<x>, vendor/maxpack src/web/mp_api.h) ----
const found = [
  { path: '/mnt/usb0/Astro Example.p5z', kind: 'p5z', packed: true, title_id: 'PPSA07890', version: '01.002.000',
    name: 'Astro Example', p5z_size: 21 * GB, install_size: 24 * GB, original_size: 41 * GB },
  { path: '/mnt/usb0/homebrew/PPSA08901', kind: 'folder', packed: false, title_id: 'PPSA08901', version: '',
    name: 'Folder Quest', p5z_size: 12 * GB, install_size: 12 * GB, original_size: 12 * GB },
  { path: '/mnt/usb0/Sample Game [PPSA06789].exfat', kind: 'image', packed: false, title_id: 'PPSA06789',
    version: '01.000.000', name: 'Example Image', p5z_size: 30 * GB, install_size: 30 * GB, original_size: 30 * GB },
  // Larger than the internal SSD's free space (311 GB): fits only on the USB drive.
  { path: '/mnt/usb0/Huge Example.p5z', kind: 'p5z', packed: true, title_id: 'PPSA08000', version: '01.000.000',
    name: 'Huge Example', p5z_size: 280 * GB, install_size: 330 * GB, original_size: 400 * GB },
  // A .p5z that grew on the PC: the payload marks it (src/web.rs, maxpack_logic::inst::mark_grown), so the page says at
  // once that it is not installed and refuses Install for it.
  { path: '/mnt/usb0/homebrew/Grown Example.p5z', kind: 'p5z', packed: true, title_id: 'PPSA00000', version: '01.001.000',
    name: 'Grown Example', p5z_size: 24 * GB, install_size: 26 * GB, original_size: 40 * GB, grown: true },
];
let installed = [
  { title_id: 'PPSA09012', name: 'Installed Example', version: '01.000.000', dir: '/data/homebrew/PPSA09012',
    kind: 'p5z', smp_pending: false, smp_why: '' },
  { title_id: 'PPSA09123', name: 'Waiting Example', version: '', dir: '/mnt/usb0/homebrew/PPSA09123',
    kind: 'folder', smp_pending: true, smp_why: 'escaneo aplazado: game_active' },
  // An image: MaxPack does not mount it; its pending note shows even without ShadowMountPlus.
  { title_id: 'PPSA06790', name: 'Image Example', version: '01.000.000', dir: '/data/homebrew/Image Example [PPSA06790].exfat',
    kind: 'image', smp_pending: true, smp_why: 'ShadowMountPlus no responde' },
];
let pending = [
  { title_id: 'PPSA09234', source: '/mnt/usb1/Half Done.p5z', dest: '/data', bytes: 7 * GB },
];
let extraJob = null; // a job the page queued (install), else the fixed one of --job
let lastExtra = null;
let jobStart = Date.now();

function currentJob() {
  if (extraJob) return extraJob;
  if (mock.job === 'off') return null;
  const total = 24 * GB;
  const frac = mock.job === 'paused' ? 0.37 : ((Date.now() - jobStart) / 1000 * 0.02) % 1;
  return { kind: 'install', title_id: 'PPSA07890', name: 'Astro Example', stage: frac > 0.9 ? 'comprobando' : 'copiando',
    done: Math.floor(total * frac), total, paused: mock.job === 'paused' };
}

function lastValue() {
  if (lastExtra) return lastExtra;
  switch (mock.last) {
    case 'done': return { kind: 'install', title_id: 'PPSA09012', code: 'done', detail: '' };
    case 'failed': return { kind: 'install', title_id: 'PPSA07890', code: 'io_error', detail: 'could not write /data/maxpack/tmp/PPSA07890/eboot.bin' };
    case 'grown': return { kind: 'install', title_id: 'PPSA00000', code: 'grown_p5z',
      detail: 'this .p5z grew (DLC or an update were added to it): play it from the .p5z, or make a compact copy of it on the PC (maxpack compact) to install it' };
    case 'needs_confirm': return { kind: 'install', title_id: 'PPSA07890', code: 'needs_confirm', detail: '',
      existing: [{ path: '/data/homebrew/PPSA07890', size: 39 * GB, from_maxpack: false }],
      source: '/mnt/usb0/Astro Example.p5z', dest: '/data' };
    default: return null;
  }
}

function instState() {
  const job = currentJob();
  return {
    ok: true, version: '0.0.1',
    drives: [{ root: '/data', internal: true, free: 311 * GB }, { root: '/mnt/usb0', internal: false, free: 412 * GB }],
    installed: mock.empty ? [] : installed,
    pending: mock.empty ? [] : pending,
    found: mock.empty ? [] : found,
    job, queue: job ? 1 : 0, last: lastValue(),
    smp: { alive: mock.waiting === 'other', version: mock.waiting === 'other' ? '1.7' : '' },
  };
}

function instPost(name, body) {
  const ok = { ok: true };
  const err = (code, detail) => ({ ok: false, code, detail: detail || '' });
  switch (name) {
    case 'scan': return ok;
    case 'probe': {
      const p = String(body.path || '');
      if (!p.startsWith('/data/') && !p.startsWith('/mnt/')) return err('bad_path', 'ruta no permitida');
      const leaf = p.split('/').pop();
      // As the payload answers: a .p5z made by a newer MaxPack for the PC (maxpack_logic::inst::newer_refusal), one that
      // grew marked (mark_grown).
      if (/newer/i.test(leaf) && /\.p5z$/i.test(p)) return err('newer_p5z', 'made by a newer MaxPack for the PC (index version 3): update MaxPack for the console to install or play it');
      return { ok: true, found: { path: p, kind: /\.p5z$/i.test(p) ? 'p5z' : /\.(exfat|ffpkg|ffpfs|ffpfsc)$/i.test(p) ? 'image' : 'folder',
        packed: false, title_id: 'PPSA09999', version: '', name: leaf.replace(/\.[^.]+$/, ''), p5z_size: 8 * GB,
        install_size: 9 * GB, original_size: 15 * GB, ...(/grown/i.test(leaf) && /\.p5z$/i.test(p) ? { grown: true } : {}) } };
    }
    case 'install': {
      const p = String(body.p5z || '');
      if (/replace/i.test(p) && !(body.replace || []).length)
        return { ok: false, code: 'needs_confirm', existing: [{ path: '/data/homebrew/PPSA09999', size: 14 * GB, from_maxpack: false }] };
      if (/full/i.test(p)) return { ok: false, code: 'no_space', need: 60 * GB, avail: 22 * GB };
      const f = found.find((x) => x.path === p) || { title_id: 'PPSA09999', name: p.split('/').pop() };
      extraJob = { kind: 'install', title_id: f.title_id, name: f.name, stage: 'copiando', done: 0, total: 9 * GB, paused: false };
      jobStart = Date.now();
      const tick = setInterval(() => {
        if (!extraJob || extraJob.paused) return clearInterval(tick);
        extraJob.done = Math.min(extraJob.total, extraJob.done + 300 * MB);
        if (extraJob.done >= extraJob.total) {
          clearInterval(tick);
          lastExtra = { kind: 'install', title_id: extraJob.title_id, code: 'done', detail: '' };
          extraJob = null;
        }
      }, 1000);
      return ok;
    }
    case 'pause': {
      const job = extraJob || (mock.job === 'on' ? currentJob() : null);
      if (job) {
        // Paused: it ends a moment later as an unfinished install, as in the engine.
        extraJob = Object.assign({}, job, { paused: true });
        setTimeout(() => {
          pending = pending.filter((x) => x.title_id !== job.title_id)
            .concat([{ title_id: job.title_id, source: '/mnt/usb0/' + job.name + '.p5z', dest: '/data', bytes: job.done }]);
          lastExtra = { kind: 'install', title_id: job.title_id, code: 'stopped', detail: '' };
          extraJob = null;
          mock.job = 'off';
        }, 1500);
      }
      return ok;
    }
    case 'verify': {
      lastExtra = { kind: 'verify', title_id: body.title_id, code: 'done', detail: '', files_ok: 1532, files_bad: 0 };
      return ok;
    }
    case 'uninstall': {
      const rec = installed.find((x) => x.title_id === body.title_id);
      if (!rec) return err('not_found', 'that game is not installed with MaxPack');
      if (!body.confirm) return err('needs_confirm', rec.dir);
      installed = installed.filter((x) => x !== rec);
      lastExtra = { kind: 'uninstall', title_id: rec.title_id, code: 'done', detail: '' };
      return ok;
    }
    case 'discard': {
      const before = pending.length;
      pending = pending.filter((x) => x.title_id !== body.title_id);
      return pending.length < before ? ok : err('not_found', 'there is no unfinished install of that game');
    }
    default: return null;
  }
}

// ---- Receive games from the PC (src/receive.rs): Settings' switch and code, and the PC's /api/receive/* ----
// The switch starts off (on with --receive); the code goes only to a page opened at localhost or 127.0.0.1, as the
// payload gives it only to the console's own screen, and only such a page turns it on or makes a new code (403
// console_only elsewhere). The PC's routes answer as the payload's, so maxpack send can be
// tried against the mock: the switch (403 receive_off), the code and its lockout (401 bad_code; 5 wrong in a row from
// one address, 429 locked for 60 s), the drives "internal" (/data, ufs) and "usb0" (/mnt/usb0, exfatfs), start,
// data (counted and dropped: nothing is written), status, finish (a verify of 3 s; a name with "damaged" in it fails
// on a volume and a file), cancel, and discard (the contract's v3: what a send left of a file deleted without a
// start; a start refused for space says what of that file is there). --receive demo|failed|paused: a receive that runs by itself (5 % a second), or
// one paused at 40 %, so the banner shows without a PC; at run time /mock?receive=demo. A receive with nothing from the
// PC for 10 minutes pauses (src/receive.rs, reason idle), as the banner's Stop does (reason stopped): it keeps its
// token, its status says paused with the reason, data and finish answer 409 paused, and a start of the same file goes
// on with that token; Delete deletes what arrived.
const pairing = { on: mock.receive !== 'none', code: '' };
let recvJob = null, recvSeq = 0, recvTimer = null;
const recvLock = new Map();
const RECV_DRIVES = [
  { id: 'internal', mount: '/data', free: 311 * GB, total: 825 * GB, fs: 'ufs' },
  { id: 'usb0', mount: '/mnt/usb0', free: 412 * GB, total: 1000 * GB, fs: 'exfatfs' },
];
const received = new Set();
// The files sends left (src/receive.rs scan_leftovers): one from an earlier, abandoned send; a paused or failed
// receive's file joins them.
let leftovers = [{ path: '/mnt/usb0/homebrew/Old Send [PPSA05555].p5z.maxpack-recv', name: 'Old Send [PPSA05555].p5z', size: 7 * GB }];
function leftState() {
  const l = leftovers.slice();
  const kept = recvJob && (recvJob.state === 'paused' || recvJob.state === 'failed' || (recvJob.state === 'cancelled' && recvJob.error));
  if (kept && !l.some((x) => x.path === recvJob.partial))
    l.push({ path: recvJob.partial, name: recvJob.name, size: recvJob.have });
  return l;
}
function mockCode() { return String(Math.floor(Math.random() * 1e6)).padStart(6, '0'); }
if (pairing.on) pairing.code = mockCode();
function isLoopback(req) { return ['127.0.0.1', '::1', '::ffff:127.0.0.1'].includes(req.socket.remoteAddress); }
// maxpack_logic::receive::idle: 10 minutes with nothing from the PC pauses a receive.
function recvSweep() {
  if (recvJob && recvJob.state === 'receiving' && !recvJob.demo && Date.now() - (recvJob.last || recvJob.started) >= 600000) {
    Object.assign(recvJob, { state: 'paused', reason: 'idle', mbps: null });
  }
}
function recvUnder() { recvSweep(); return !!recvJob && (recvJob.state === 'receiving' || recvJob.state === 'verifying'); }
// state.receive (src/receive.rs `json`): never the token.
function recvState() {
  recvSweep();
  if (!recvJob) return null;
  const j = recvJob;
  return { seq: j.seq, state: j.state, reason: j.state === 'paused' ? j.reason : null, name: j.name, drive: j.mount, path: j.partial, dest: j.dest, size: j.size, have: j.have,
    from: j.from, mbps: recvUnder() ? j.mbps : null, cancelling: false, verify: j.verify, error: j.error, elapsed_ms: Date.now() - j.started };
}
// The status the PC polls (maxpack_logic::receive::status_json).
// Each bad item as the PC's maxpack verify --quick --json gives it: a volume by number, a file by path.
function recvStatus(j) {
  return { have: j.have, size: j.size, state: j.state, ...(j.state === 'paused' ? { reason: j.reason } : {}), error: j.error, verify: j.verify && { bytes: j.verify.bytes, total: j.verify.total,
    nbad: j.verify.nbad, bad: j.verify.bad.map((b) => Object.assign(b.what === 'volume' ? { volume: b.volume } : { path: b.name },
      { why: 'sha256', expected: 'aa'.repeat(32), found: 'bb'.repeat(32) })) } };
}
const recvNo = (code, status, detail, extra) => [status, Object.assign({ error: code, detail }, extra || {})];
function recvNewJob(name, size, drive, have) {
  const d = RECV_DRIVES.find((x) => x.id === drive);
  recvJob = { id: require('crypto').randomBytes(16).toString('hex'), seq: ++recvSeq, name, size, drive, mount: d.mount,
    partial: `${d.mount}/homebrew/${name}.maxpack-recv`, dest: `${d.mount}/homebrew/${name}`, have, from: have,
    state: 'receiving', verify: null, error: null, mbps: null, started: Date.now() };
  return recvJob;
}
// The verify after finish: 3 s, then done (the name joins the drive's files) or failed.
function recvVerify(j) {
  j.state = 'verifying';
  j.verify = { bytes: 0, total: j.size, nbad: 0, bad: [] };
  let step = 0;
  clearInterval(recvTimer);
  recvTimer = setInterval(() => {
    if (j.state !== 'verifying') return clearInterval(recvTimer);
    j.verify.bytes = Math.min(j.size, Math.round(j.size * ++step / 3));
    j.mbps = j.size / 3 / 1e6;
    if (step < 3) return;
    clearInterval(recvTimer);
    if (/damaged/i.test(j.name) || j.demoFail) {
      j.verify.nbad = 2;
      j.verify.bad = [{ what: 'volume', volume: 2, name: 'ampr_zstd-002.pak' }, { what: 'file', volume: null, name: 'sce_sys/param.json' }];
      j.state = 'failed';
      j.error = '2 of 40 volumes and files do not match: volume ampr_zstd-002.pak (sha256) and 1 more';
    } else {
      j.state = 'done';
      received.add(j.dest);
    }
  }, 1000);
}
// --receive demo|failed (and /mock?receive=): a receive of 18 GB that arrives 5 % a second by itself.
function recvDemo(kind) {
  clearInterval(recvTimer);
  if (kind === 'paused') {
    pairing.on = true;
    if (!pairing.code) pairing.code = mockCode();
    Object.assign(recvNewJob('Demo Paused [PPSA09876].p5z', 18 * GB, 'usb0', Math.round(18 * GB * 0.4)), { state: 'paused', reason: 'idle', demo: true });
    return;
  }
  if (kind !== 'demo' && kind !== 'failed') { recvJob = null; return; }
  pairing.on = true;
  if (!pairing.code) pairing.code = mockCode();
  const j = recvNewJob(kind === 'failed' ? 'Demo Damaged [PPSA09876].p5z' : 'Demo Game [PPSA09876].p5z', 18 * GB, 'usb0', 0);
  j.demoFail = kind === 'failed';
  j.demo = true;
  recvTimer = setInterval(() => {
    if (j.state !== 'receiving') return clearInterval(recvTimer);
    j.have = Math.min(j.size, j.have + Math.round(j.size * 0.05));
    j.mbps = j.size * 0.05 / 1e6;
    if (j.have >= j.size) recvVerify(j);
  }, 1000);
}
// maxpack_logic::receive::gate: the switch, then the lockout and the code.
function recvGate(req) {
  if (!pairing.on) return recvNo('receive_off', 403, 'receiving games from the PC is off on the console (Settings)');
  const key = req.socket.remoteAddress, now = Date.now(), t = recvLock.get(key) || { wrong: 0, until: 0 };
  if (t.until > now) return recvNo('locked', 429, 'too many wrong codes', { retry_after: Math.ceil((t.until - now) / 1000) });
  const code = req.headers['x-maxpack-code'];
  if (code === undefined) return recvNo('bad_code', 401, 'missing or wrong pairing code (X-MaxPack-Code)');
  if (code === pairing.code) { recvLock.delete(key); return null; }
  t.wrong += 1;
  if (t.wrong >= 5) Object.assign(t, { wrong: 0, until: now + 60000 });
  recvLock.set(key, t);
  return recvNo('bad_code', 401, 'missing or wrong pairing code (X-MaxPack-Code)');
}
// maxpack_logic::receive::valid_name.
function recvNameOk(name) {
  return Buffer.byteLength(name) <= 200 && /^[^/\\:*?"<>|\u0000-\u001f\u007f-\u009f]+\.p5z$/i.test(name) && !/\.\./.test(name) && !name.startsWith('.')
    && !/[­؜᠎​-‏‪-‮⁠-⁤⁦-⁯﻿]/.test(name);
}
// maxpack_logic::receive::valid_name and start_gate, in their order.
function recvStart(body) {
  let b;
  try { b = JSON.parse(body); } catch (e) { return recvNo('bad_request', 400, 'expected {"name","size","drive"}'); }
  if (!b || typeof b.name !== 'string' || typeof b.size !== 'number' || typeof b.drive !== 'string')
    return recvNo('bad_request', 400, 'expected {"name","size","drive"}');
  const { name, size, drive } = b;
  if (!recvNameOk(name)) return recvNo('bad_name', 400, 'not a plain .p5z file name');
  if (size < 96) return recvNo('bad_size', 400, 'a .p5z is at least 96 bytes');
  const d = RECV_DRIVES.find((x) => x.id === drive);
  if (!d) return recvNo('no_drive', 404, 'no such drive on the console');
  const dest = `${d.mount}/homebrew/${name}`;
  if (received.has(dest) || games().some((g) => g.path === dest)) return recvNo('exists', 409, 'that .p5z is already there');
  if (recvJob && recvJob.state === 'receiving' && recvJob.name === name && recvJob.drive === drive && recvJob.size === size)
    return [200, { id: recvJob.id, have: recvJob.have }];
  // Paused in this run: receiving again with its token, from where it was.
  if (recvJob && recvJob.state === 'paused' && recvJob.name === name && recvJob.drive === drive && recvJob.size === size) {
    if (currentJob() || verifyRunning()) return recvNo('busy', 409, 'another job runs; try again when it ends');
    Object.assign(recvJob, { state: 'receiving', reason: null, from: recvJob.have, last: Date.now(), demo: false });
    return [200, { id: recvJob.id, have: recvJob.have }];
  }
  if (recvUnder()) return recvNo('busy', 409, 'a receive from the PC is under way; try again when it ends');
  if (currentJob()) return recvNo('busy', 409, 'the installer is running a job; try again when it ends');
  if (verifyRunning()) return recvNo('busy', 409, 'a .p5z is being verified on the console; try again when it ends');
  // maxpack_logic::receive::start_gate's space check: what is still to come from where a file of that name there
  // would resume (its length less 64 MiB), and the margin. A refusal says what is there (have) and what is short
  // (need; missing as before the contract's v3).
  const there = leftState().find((x) => x.path === `${d.mount}/homebrew/${name}.maxpack-recv`);
  const have = there ? there.size : 0;
  const need = size - Math.max(0, have - 64 * MB) + 256 * MB - d.free;
  if (need > 0) return recvNo('no_space', 507, 'not enough space on the drive', { need, missing: need, have });
  clearInterval(recvTimer);
  const j = recvNewJob(name, size, drive, 0);
  return [200, { id: j.id, have: j.have }];
}
// A paused receive keeps its token; a request with the token counts as the PC being there.
function recvById(url) {
  recvSweep();
  const id = url.searchParams.get('id');
  const j = recvJob && recvJob.id === id ? recvJob : null;
  if (j) j.last = Date.now();
  return j;
}
// POST /api/receive/discard (the contract's v3, maxpack_logic::receive::discard_gate): what a send left of a file,
// deleted without a start; a receive of it under way is busy, a paused, failed or cancelled one ends (its token goes).
function recvDiscard(body) {
  let b;
  try { b = JSON.parse(body); } catch (e) { return recvNo('bad_request', 400, 'expected {"name","drive"}'); }
  if (!b || typeof b.name !== 'string' || (b.drive !== undefined && typeof b.drive !== 'string'))
    return recvNo('bad_request', 400, 'expected {"name","drive"}');
  const name = b.name, drive = b.drive === undefined ? 'internal' : b.drive;
  if (!recvNameOk(name)) return recvNo('bad_name', 400, 'not a plain .p5z file name');
  const d = RECV_DRIVES.find((x) => x.id === drive);
  if (!d) return recvNo('no_drive', 404, 'no such drive on the console');
  const p = `${d.mount}/homebrew/${name}.maxpack-recv`;
  const same = recvJob && recvJob.partial === p ? recvJob : null;
  if (same && (same.state === 'receiving' || same.state === 'verifying')) return recvNo('busy', 409, 'a receive from the PC is under way; try again when it ends');
  const there = leftState().find((x) => x.path === p);
  leftovers = leftovers.filter((x) => x.path !== p);
  if (same && same.state !== 'done') { clearInterval(recvTimer); recvJob = null; }
  return [200, there ? { deleted: true, bytes: there.size } : { deleted: false }];
}
// --receive-delete-fail (or /mock?receive_delete_fail=1): the delete fails, so the file stays and error says why
// (src/receive.rs cancel: a cancelled receive's error is only that).
function recvCancel(j) {
  if (j.state === 'done') return recvNo('state', 409, 'the receive is done');
  if (j.state !== 'cancelled') Object.assign(j, { state: 'cancelled', mbps: null,
    error: mock.receive_delete_fail ? 'Permission denied (os error 13)' : null });
  clearInterval(recvTimer);
  return [200, recvStatus(j)];
}
// The PC's routes; null for a request that is not one of them.
function recvRoute(req, url, body, bytes) {
  const p = url.pathname;
  if (!p.startsWith('/api/receive/')) return null;
  const no = recvGate(req);
  if (no) return no;
  if (req.method === 'GET' && p === '/api/receive/drives')
    return [200, { drives: RECV_DRIVES.map((d) => ({ id: d.id, free: d.free, total: d.total, fs: d.fs })) }];
  if (req.method === 'POST' && p === '/api/receive/start') return recvStart(body);
  if (req.method === 'POST' && p === '/api/receive/discard') return recvDiscard(body);
  const j = recvById(url);
  if (!j) return recvNo('unknown_id', 404, 'no receive with that id: start again');
  if (req.method === 'PUT' && p === '/api/receive/data') {
    const offset = Number(url.searchParams.get('offset'));
    if (j.state === 'paused') return recvNo('paused', 409, 'the receive is paused: start it again to go on', { reason: j.reason, have: j.have });
    if (j.state !== 'receiving') return recvNo('state', 409, `the receive is ${j.state}`);
    if (bytes > 64 * MB) return recvNo('too_large', 413, 'at most 64 MiB per request');
    if (offset !== j.have) return recvNo('offset', 409, `the data must go on at byte ${j.have}`, { have: j.have });
    if (offset + bytes > j.size) return recvNo('past_size', 400, 'past the size given at start');
    const t = (Date.now() - (j.lastPut || j.started)) / 1000;
    j.have += bytes;
    j.mbps = t > 0 ? bytes / t / 1e6 : null;
    j.lastPut = Date.now();
    return [200, { have: j.have }];
  }
  if (req.method === 'GET' && p === '/api/receive/status') return [200, recvStatus(j)];
  if (req.method === 'POST' && p === '/api/receive/finish') {
    if (j.state === 'paused') return recvNo('paused', 409, 'the receive is paused: start it again to go on', { reason: j.reason, have: j.have });
    if (j.state !== 'receiving') return recvNo('state', 409, `the receive is ${j.state}`);
    if (j.have !== j.size) return recvNo('incomplete', 409, `only ${j.have} bytes arrived`, { have: j.have });
    recvVerify(j);
    return [202, recvStatus(j)];
  }
  if (req.method === 'POST' && p === '/api/receive/cancel') return recvCancel(j);
  return recvNo('not_found', 404, 'no such route');
}
if (mock.receive === 'demo' || mock.receive === 'failed') recvDemo(mock.receive);

// ---- server ----
function send(res, status, type, body) {
  res.writeHead(status, { 'Content-Type': type, 'Cache-Control': 'no-store' });
  res.end(body);
}
const json = (res, status, v) => send(res, status, 'application/json', JSON.stringify(v));
// maxpack_logic::http::host_ok: an IPv4 (four groups of 0-255), an [IPv6] or localhost, with an
// optional :port (0-65535).
function hostOk(h) {
  const m = /^(?:\[([0-9A-Fa-f:.]*)\]|([^:\[\]]*))(?::(\d+))?$/.exec(h || '');
  if (!m || (m[3] !== undefined && Number(m[3]) > 65535)) return false;
  if (m[1] !== undefined) return m[1].indexOf(':') >= 0;
  const g = m[2].split('.');
  return m[2] === 'localhost' || g.length === 4 && g.every((x) => /^\d{1,3}$/.test(x) && Number(x) <= 255);
}
// maxpack_logic::http::same_origin: no Sec-Fetch-Site cross-site, and an Origin (when sent) that is
// http(s):// and this very Host.
function sameOrigin(host, origin, fetchSite) {
  if (fetchSite && fetchSite.toLowerCase() === 'cross-site') return false;
  if (origin === undefined) return true;
  const o = origin.toLowerCase(), h = String(host).toLowerCase();
  return o === 'http://' + h || o === 'https://' + h;
}
// maxpack_logic::http::check, and src/web.rs `refused`'s answers.
function refusal(req) {
  const host = req.headers.host;
  if (!hostOk(host)) return 'host';
  if (req.method !== 'GET' && req.method !== 'HEAD' && !sameOrigin(host, req.headers.origin, req.headers['sec-fetch-site']))
    return 'cross';
  return null;
}
const REFUSED = {
  host: ['MaxPack: abre esta página con la dirección IP de la consola (http://<IP de la PS5>:8095), no con un nombre.',
    "MaxPack: open this page at the console's IP address (http://<PS5 IP>:8095), not by a name."],
  cross: ['MaxPack: petición rechazada: no viene de la página de MaxPack.',
    'MaxPack: request refused: it did not come from the MaxPack page.'],
};

http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const p = url.pathname;
  // The receive's data is counted, never kept (up to 64 MiB a request).
  const counted = req.method === 'PUT' && url.pathname === '/api/receive/data';
  let chunks = '', bytes = 0;
  req.on('data', (c) => { if (counted) bytes += c.length; else chunks += c; });
  req.on('end', () => {
    const why = refusal(req);
    if (why) {
      console.log(`refused ${req.method} ${req.url} (${why}): Host ${req.headers.host}, Origin ${req.headers.origin}`);
      if (p.startsWith('/api/inst/')) return json(res, 403, { ok: false, code: 'forbidden', detail: REFUSED[why][0] });
      return send(res, 403, 'text/plain; charset=utf-8', REFUSED[why].join('\n') + '\n');
    }
    if (req.method === 'GET' && p === '/') {
      const html = fs.readFileSync(path.join(root, 'src/ui.html'), 'utf8').replace(/__VERSION__/g, '1.0.2-mock');
      return send(res, 200, 'text/html; charset=utf-8', html);
    }
    if (req.method === 'GET' && p === '/app-icon.png') return send(res, 200, 'image/png', fs.readFileSync(path.join(root, 'ps5/icon0.png')));
    if (p === '/mock') {
      for (const k of ['waiting', 'job', 'last', 'update', 'update_fail', 'tls', 'tls_result', 'ampr_fail']) if (url.searchParams.has(k)) mock[k] = url.searchParams.get(k);
      if (url.searchParams.has('empty')) mock.empty = url.searchParams.get('empty') === '1';
      if (url.searchParams.has('receive_delete_fail')) mock.receive_delete_fail = url.searchParams.get('receive_delete_fail') === '1';
      if (url.searchParams.has('job')) { extraJob = null; jobStart = Date.now(); }
      if (url.searchParams.has('last')) lastExtra = null;
      if (url.searchParams.has('receive')) recvDemo(url.searchParams.get('receive'));
      return json(res, 200, mock);
    }
    if (req.method === 'GET' && p === '/api/state') return json(res, 200, apiState(req));
    const rx = recvRoute(req, url, chunks, bytes);
    if (rx) return json(res, rx[0], rx[1]);
    // Settings > "Receive games from the PC" (src/receive.rs set_on, renew, cancel).
    if (req.method === 'POST' && p === '/api/pairing') {
      if (chunks !== 'on' && chunks !== 'off') return send(res, 400, 'text/plain', 'expected on or off');
      // maxpack_logic::receive::pairing_allowed: on (and a new code) only from the console's own browser.
      if (chunks === 'on' && !isLoopback(req)) return json(res, 403, { code: 'console_only', detail: "only from MaxPack's page on the console itself" });
      if (chunks === 'off' && recvUnder()) return send(res, 409, 'text/plain', 'a game is arriving from the PC: stop it first');
      pairing.on = chunks === 'on';
      if (pairing.on && !pairing.code) pairing.code = mockCode();
      return send(res, 204, 'text/plain', '');
    }
    if (req.method === 'POST' && p === '/api/pairing/new') {
      if (!isLoopback(req)) return json(res, 403, { code: 'console_only', detail: "only from MaxPack's page on the console itself" });
      pairing.code = mockCode();
      return send(res, 204, 'text/plain', '');
    }
    if (req.method === 'POST' && p === '/api/pairing/stop') {
      if (!recvUnder()) return json(res, 409, { error: 'state', detail: 'no receive is under way' });
      clearInterval(recvTimer);
      Object.assign(recvJob, { state: 'paused', reason: 'stopped', mbps: null });
      return json(res, 200, recvStatus(recvJob));
    }
    if (req.method === 'POST' && p === '/api/pairing/left') {
      if (!leftState().some((x) => x.path === chunks)) return send(res, 404, 'text/plain', 'not a file a send left');
      leftovers = leftovers.filter((x) => x.path !== chunks);
      if (recvJob && recvJob.partial === chunks) Object.assign(recvJob, { state: 'cancelled', error: null });
      return send(res, 204, 'text/plain', '');
    }
    if (req.method === 'POST' && p === '/api/pairing/cancel') {
      if (!recvJob) return json(res, 404, { error: 'unknown_id', detail: 'no receive' });
      const c = recvCancel(recvJob);
      return json(res, c[0], c[1]);
    }
    if (req.method === 'GET' && p === '/api/settings') return json(res, 200, settings);
    if (req.method === 'GET' && p === '/api/ampr') return json(res, 200, { ...ampr, updating: false });
    if (req.method === 'GET' && p.startsWith('/api/sizes/')) return json(res, 200, { game: 38 * GB, dlc: 0, save: 120 * MB, cache: 210 * MB });
    if (req.method === 'GET' && p.startsWith('/api/report/')) {
      const g = games().find((x) => x.title_id === p.slice('/api/report/'.length));
      if (!g) return send(res, 404, 'text/plain', 'not found');
      // The shape of maxpack_logic::report::text, with fixed values.
      const kinds = { pkg: ['pkg', 'pkg'], p5z: ['p5z', 'p5z'], plain: ['folder', 'plain folder'], zstd: ['folder (zstd)', 'zstd folder'], lz4: ['folder (lz4)', 'LZ4 folder'] };
      const [en, format] = kinds[g.kind === 'folder' ? g.packing : g.kind];
      // maxpack_logic::report::compat_row: no name, no title id; drive_text and the Fakelib mode in the page's words.
      const drive = { '/data': 'internal SSD', '/mnt/ext1': 'M.2 SSD (console slot)', '/mnt/ext0': 'USB SSD or HDD? (extended storage)' }[g.drive]
        || (/^\/mnt\/usb\d+$/.test(g.drive) ? 'USB SSD or HDD?' : g.drive);
      const mode = g.overlay_off ? 'none (MaxPack libraries at launch off)' : { full: 'Full', emulators: 'Emulators only', off: 'Off' }[g.fakelib_mode];
      // maxpack_logic::report: " (recommended)" only after a value the list itself sets.
      const rec = g.settings_source === 'recommended' && g.recommended ? g.recommended : {};
      const fromList = rec.fakelib != null ? ' (recommended)' : '';
      const playgoFromList = rec.playgo_off != null ? ' (recommended)' : '';
      const text = [`MaxPack 1.0.2: game report`, 'Firmware: 13.60', 'Mounting: MaxPack mounts the games',
        `Game: ${g.title_name} (${g.title_id}), version ${g.version}`, `Source: ${g.path} (${en}, ${g.drive})`,
        `Status: ${g.status}${g.error ? ` (${g.error})` : ''}`].concat(g.kind === 'p5z' ? [`Verified on the console: ${verifiedLine(g.verified)}`] : [],
        g.hidden ? ['Hidden in the library: yes'] : [],
        ['Backport: none'],
        // maxpack_logic::report::text: a fakelib mode other than full.
        // " (recommended)" after the two values the recommended settings gave.
        g.fakelib_mode === 'emulators' ? [`Fakelib mode: emulators only (the game's and backport's fakelib not used)${fromList}`] :
          g.fakelib_mode === 'off' ? [`Fakelib mode: off (${g.kind === 'folder' && g.packing === 'plain' ? 'empty: nothing at launch' : 'only ampr-emu-ng'})${fromList}`] :
            fromList ? [`Fakelib mode: full${fromList}`] : [],
        [`ampr-emu-ng: ${ampr.kind}${ampr.version ? ' ' + ampr.version : ''}`,
          `PlayGo stub: ${g.fakelib_mode === 'off' ? 'not added (fakelib mode off)' : (g.playgo_off ? 'off' : 'on') + playgoFromList}`,
          `Last crash: ${g.crash ? `2026-11-23 20:10:00Z in ${g.crash.process}, ${g.crash.status} (console dump folder ${g.title_id}_1795166239)` : 'none recorded'}`,
          '', 'Row for docs/compatibility.md (fill in each ?; it names no game):',
          `| ? | ${format} | ${g.kind === 'p5z' ? '?' : '—'} | ${drive} | 13.60 | MaxPack 1.0.2 | ${mode} | ? | ? |  |`, '',
          `maxpack.log, last 40 lines with ${g.title_id}:`, `2026-11-23 20:00:05Z [5000] ${g.title_id}: cache fresh`]).join('\n') + '\n';
      return send(res, 200, 'text/plain; charset=utf-8', text);
    }
    if (req.method === 'POST' && p.startsWith('/api/launch/')) {
      const g = games().find((x) => x.title_id === p.slice('/api/launch/'.length));
      if (!g) return send(res, 404, 'text/plain', 'not found');
      if (runningNow().length) return send(res, 409, 'text/plain', 'a game is running');
      if (waitingValue()) return send(res, 409, 'text/plain', 'MaxPack does not mount now (another mounter runs, or no kstuff)');
      if (g.status !== 'ready') return send(res, 409, 'text/plain', 'the game is not ready');
      running = { tid: g.title_id, until: Date.now() + 8000 };
      return send(res, 204, 'text/plain', '');
    }
    if (req.method === 'POST' && p === '/api/self_update') {
      let v = null;
      try { v = JSON.parse(chunks).version; } catch (e) { /* below */ }
      if (typeof v !== 'string') return send(res, 400, 'text/plain', 'expected {"version":"<tag>"}');
      const fail = (code) => json(res, UPDATE_FAILS[code][0], { code, detail: UPDATE_FAILS[code][1] });
      const offered = updateValue();
      if (!offered || offered.version !== v) return fail('not_offered');
      if (mock.tls !== 'passed') return fail('tls_unchecked');
      if (runningNow().length) return fail('game_running');
      if (UPDATE_FAILS[mock.update_fail]) return fail(mock.update_fail);
      // Sent: the "new copy" answers with its version a little later, as a real take-over does.
      setTimeout(() => { version = v.replace(/^v/, ''); }, 3000);
      return json(res, 200, {});
    }
    if (req.method === 'POST' && p === '/api/ampr/update') {
      // maxpack_logic::ampr::Fail: the self-update's gate first, then the SHA256SUMS check.
      if (mock.tls !== 'passed') return json(res, 409, { code: 'tls_unchecked', detail: UPDATE_FAILS.tls_unchecked[1] });
      return setTimeout(() => mock.ampr_fail === 'checksum'
        ? json(res, 502, { code: 'checksum', detail: 'not the released libSceAmpr.sprx, not installed: v0.5.1-maxpack.3: SHA256SUMS lists no libSceAmpr.sprx' })
        : mock.ampr_fail === 'older'
        ? json(res, 409, { code: 'older', detail: 'not installed: the latest release v0.5.1-maxpack.1 is older than the ampr-emu-ng MaxPack carries, v0.5.1-maxpack.2 (bundled v0.5.1-maxpack.2); a .p5z that grew would no longer start with it' })
        : (ampr = { kind: 'downloaded', version: 'v0.5.1-maxpack.3' },
          send(res, 200, 'text/plain', 'installed ampr-emu-ng v0.5.1-maxpack.3 (720048 bytes, matches SHA256SUMS)')), 1000);
    }
    if (req.method === 'POST' && p === '/api/selftest/tls') {
      if (tlsRunning) return send(res, 409, 'text/plain', 'a secure-connection check is already running');
      tlsRunning = true;
      // The probes take a moment on the console: the page shows "Checking..." meanwhile.
      setTimeout(() => {
        tlsRunning = false;
        const a = tlsAnswer();
        // maxpack_logic::tls::after_run: a pass or a broken run replaces the record, inconclusive keeps it.
        if (a.verdict !== 'inconclusive') { mock.tls = a.verdict; tlsAt = Math.floor(Date.now() / 1000); }
        json(res, 200, Object.assign(a, { passed: mock.tls === 'passed' }));
      }, 2000);
      return;
    }
    if (req.method === 'POST' && p.startsWith('/api/hidden/')) {
      const tid = p.slice('/api/hidden/'.length);
      if (chunks !== 'on' && chunks !== 'off') return send(res, 400, 'text/plain', 'expected on or off');
      if (chunks === 'on') hidden.add(tid); else hidden.delete(tid);
      return send(res, 204, 'text/plain', '');
    }
    if (req.method === 'POST' && p.startsWith('/api/overlay/')) {
      const g = games().find((x) => x.title_id === p.slice('/api/overlay/'.length));
      if (!g) return send(res, 404, 'text/plain', 'not found');
      if (chunks !== 'on' && chunks !== 'off') return send(res, 400, 'text/plain', 'expected on or off');
      // maxpack_logic::overlay::needed: only a plain folder game can run without it.
      if (chunks === 'off' && !(g.kind === 'folder' && g.packing === 'plain'))
        return send(res, 409, 'text/plain', 'not a plain folder game');
      if (chunks === 'off') overlayOff.add(g.title_id); else overlayOff.delete(g.title_id);
      return send(res, 204, 'text/plain', '');
    }
    if (req.method === 'POST' && p.startsWith('/api/fakelib/')) {
      const g = games().find((x) => x.title_id === p.slice('/api/fakelib/'.length));
      if (!g) return send(res, 404, 'text/plain', 'not found');
      if (!['full', 'emulators', 'off'].includes(chunks)) return send(res, 400, 'text/plain', 'expected full, emulators or off');
      handChoice(g.title_id);
      if (chunks === 'full') fakelibMode.delete(g.title_id); else fakelibMode.set(g.title_id, chunks);
      return send(res, 204, 'text/plain', '');
    }
    if (req.method === 'POST' && p.startsWith('/api/playgo/')) {
      const g = games().find((x) => x.title_id === p.slice('/api/playgo/'.length));
      if (!g) return send(res, 404, 'text/plain', 'not found');
      if (chunks !== 'on' && chunks !== 'off') return send(res, 400, 'text/plain', 'expected on or off');
      handChoice(g.title_id);
      if (chunks === 'off') playgoOff.add(g.title_id); else playgoOff.delete(g.title_id);
      return send(res, 204, 'text/plain', '');
    }
    if (req.method === 'POST' && p.startsWith('/api/recommended/')) {
      const g = games().find((x) => x.title_id === p.slice('/api/recommended/'.length));
      if (!g) return send(res, 404, 'text/plain', 'not found');
      if (chunks !== 'reset') return send(res, 400, 'text/plain', 'expected reset');
      // maxpack_logic::recommended::reset: out of the four lists.
      [manual, playgoOff].forEach((l) => l.delete(g.title_id));
      fakelibMode.delete(g.title_id);
      return send(res, 204, 'text/plain', '');
    }
    if (req.method === 'POST' && p === '/api/verify/cancel') {
      if (!verifyRunning()) return send(res, 409, 'text/plain', 'no verify is running');
      verifyJob.cancelling = true;
      return send(res, 204, 'text/plain', '');
    }
    if (req.method === 'POST' && p.startsWith('/api/verify/')) {
      const no = verifyStart(p.slice('/api/verify/'.length), chunks);
      return no ? json(res, no[0], { code: no[1], detail: no[2] }) : send(res, 204, 'text/plain', '');
    }
    if (req.method === 'POST' && p.startsWith('/api/rebuild/')) {
      const tid = p.slice('/api/rebuild/'.length);
      queued.add(tid);
      setTimeout(() => queued.delete(tid), 3000);
      return send(res, 204, 'text/plain', '');
    }
    if (req.method === 'GET' && p === '/api/inst/state') return json(res, 200, instState());
    if (req.method === 'GET' && p === '/api/inst/icon') {
      const which = url.searchParams.get('p5z') || '';
      if (/\.p5z$/i.test(which)) return send(res, 200, 'image/png', fs.readFileSync(path.join(root, 'ps5/icon0.png')));
      return send(res, 404, 'text/plain', '');
    }
    if (req.method === 'POST' && p.startsWith('/api/inst/')) {
      let body = {};
      try { body = chunks ? JSON.parse(chunks) : {}; } catch (e) { return json(res, 400, { ok: false, code: 'bad_json', detail: 'the body is not valid JSON' }); }
      if (!/^application\/json/i.test(req.headers['content-type'] || ''))
        return json(res, 403, { ok: false, code: 'forbidden', detail: 'request refused: only the MaxPack page is accepted' });
      // src/web.rs: a job of the installer waits for the console's own (maxpack_logic::job::blocks_installer).
      if (['install', 'verify', 'uninstall'].includes(p.slice('/api/inst/'.length)) && consoleBusy())
        return json(res, 409, { ok: false, code: 'console_busy', detail: 'a .p5z is being verified or a game is arriving from the PC' });
      const out = instPost(p.slice('/api/inst/'.length), body);
      return out ? json(res, 200, out) : send(res, 404, 'text/plain', 'not found');
    }
    if (req.method === 'POST' && p === '/api/settings') {
      let v = {};
      try { v = JSON.parse(chunks); } catch (e) { return send(res, 400, 'text/plain', 'settings: bad JSON'); }
      // A few of maxpack_logic::roots::never_root's paths, enough to see the page's message.
      const bad = (v.scanpath || []).find((r) => /^\/(system[^/]*|user(?!\/data(\/|$))|mnt\/sandbox|data\/maxpack)(\/|$)|^\/+$/.test(r));
      if (bad !== undefined) return json(res, 400, { code: 'forbidden_root', path: bad, detail: `scanpath ${bad} is never scanned` });
      Object.assign(settings, v);
      return send(res, 204, 'text/plain', '');
    }
    if (req.method === 'POST' && p.startsWith('/api/')) return send(res, 204, 'text/plain', '');
    return send(res, 404, 'text/plain', 'not found');
  });
}).listen(port, () => {
  console.log(`mock UI on http://localhost:${port}/ (waiting=${mock.waiting}, job=${mock.job}, last=${mock.last}${mock.empty ? ', empty' : ''}, update=${mock.update}, update_fail=${mock.update_fail}, tls=${mock.tls}, tls_result=${mock.tls_result}, ampr_fail=${mock.ampr_fail}, receive=${mock.receive})`);
  console.log(`switch at run time: http://localhost:${port}/mock?waiting=none&job=paused&last=needs_confirm`);
  // The addresses to try the banner's URL with (see the top of the file).
  const lan = Object.values(require('os').networkInterfaces()).flat().filter((a) => a && a.family === 'IPv4' && !a.internal);
  for (const a of lan) console.log(`banner shows this address: http://${a.address}:${port}/`);
  console.log(`opened by a name, every request answers 403: http://${require('os').hostname()}:${port}/`);
});
