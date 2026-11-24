#!/usr/bin/env node
// Development only: serves src/ui.html with fixed data in place of the payload, to look at the page
// (Library with ShadowMountPlus images, Install, the "installer only" banner, an install in
// progress) without a console. Nothing here goes into maxpack.elf.
//
//   node tools/mock_ui.js [--port 8095] [--waiting other|kstuff|none] [--job on|paused|off]
//                         [--last done|failed|needs_confirm|none] [--empty]
//                         [--update v1.0.1|none] [--update-fail <code>|none]
//   then open http://localhost:8095/ and the Install tab.
//
// Defaults: --waiting other (ShadowMountPlus running: the banner and ShadowMountPlus connected in
// Install), --job on (a .p5z being copied, ~2 % a second), --last none.
// --empty: no games, images, found items or installs (the empty states).
// --update: a newer release found at start (state.update), so the page shows its notice with
// Update and Later; Update (POST /api/self_update) "installs" it: 3 s later the mock answers with
// that version and the page reloads. --update-fail answers Update with that error code instead
// (maxpack_logic::update::Fail: game_running, scan_running, install_running, no_elfldr, download,
// checksum, send...). Defaults: none.
// The same switches at run time, without a restart (the page picks them up on its next poll):
//   http://localhost:8095/mock?waiting=none&job=paused&last=needs_confirm&empty=0&update=v1.0.1&update_fail=checksum
// The page's POSTs answer as the payload would (install queues a job, pause pauses it and it ends
// as an unfinished install, a path with "replace" in it asks to confirm a replace, and so on).
// Library samples: a hidden game (PPSA02345, shown with "Show hidden"), a game with a recorded crash
// (PPSA01234, its sheet shows it and Copy report copies GET /api/report), Play (POST /api/launch:
// the game "runs" for 8 s, then exits), Hide/Show (POST /api/hidden), and Rebuild (rebuild_queued
// for 3 s), the overlay switch (POST /api/overlay: only "Plain Racer", a plain folder game, shows it;
// "Shell Title" has it off; "Packed Example", a .p5z, is still listed from before, so its sheet shows
// the switch to clear it; turning a non-plain game off answers 409 as the payload does), and the Excluded paths
// of Settings. Install: "Huge Example" does not fit on the internal SSD (the page says so per drive).
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
};
// The running version: the one an Update in the mock "installs" once it is done.
let version = '1.0.0';
// state.update as src/update.rs `available` gives it: a newer release, or null.
function updateValue() {
  if (!mock.update || mock.update === 'none' || mock.update.replace(/^v/, '') === version) return null;
  return { version: mock.update, size: 3456789 };
}
// maxpack_logic::update::Fail: the status of each code, and its English detail.
const UPDATE_FAILS = {
  not_offered: [400, 'not the newer release found at start'],
  game_running: [409, 'a game is running: close it first'],
  update_running: [409, 'an update is already running'],
  scan_running: [409, 'a scan or build is in progress; try again in a moment'],
  install_running: [409, 'the installer is copying a game; try again when it ends'],
  no_elfldr: [503, 'elfldr does not answer: 127.0.0.1:9021: connection refused'],
  download: [502, 'download failed: GET https://github.com/DeLFuS77/maxpack-ps5/releases/download/v1.0.1/maxpack.elf: HTTP status 404'],
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

// ---- the payload's own API (src/web.rs) ----
function games() {
  if (mock.empty) return [];
  const g = (tid, name, kind, packing, status, extra) => Object.assign({
    title_id: tid, title_name: name, version: '01.000.000', path: '/data/homebrew/' + tid, kind, packing,
    drive: '/data', size: kind === 'pkg' ? 38 * GB : 0, status, error: null, installed_copy: null, cache_size: 210 * MB,
    cache_built: BUILT, error_code: null, missing_bytes: null, backport: null, playgo: false, playgo_off: false,
    hidden: hidden.has(tid), crash: null, rebuild_queued: queued.has(tid),
    registered: status === 'ready', mounted: status === 'ready', source_present: true,
    overlay_off: kind === 'folder' && packing === 'plain' && overlayOff.has(tid), overlay_off_listed: overlayOff.has(tid),
  }, extra || {});
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
    // A dump whose title id is installed on the console: left alone, the sheet says why.
    g('PPSA06000', 'Installed Twin', 'folder', 'plain', 'error', { cache_built: null, cache_size: 0,
      error: 'PPSA06000 (/data/homebrew/PPSA06000): an installed copy of this title exists (/user/app/PPSA06000/app.pkg): MaxPack does not register or mount this copy',
      installed_copy: '/user/app/PPSA06000/app.pkg' }),
  ];
}

function apiState() {
  return {
    version, update: updateValue(), busy: false, paused: runningNow().length > 0, waiting: waitingValue(), running: runningNow(),
    games: games(),
    images: mock.empty ? [] : [
      { path: '/data/homebrew/Juego [PPSA06789].exfat', kind: 'exfat', title_id: 'PPSA06789' },
      { path: '/mnt/usb0/homebrew/varios/otro.ffpkg', kind: 'ffpkg' },
    ],
    unplayable: mock.empty ? [] : [
      { path: '/mnt/usb0/homebrew/Broken Pack.p5z', code: 'damaged', detail: 'el volumen 2 está incompleto' },
      { path: '/mnt/usb0/homebrew/Huge Pack.p5z', code: 'too_many_volumes', detail: '300' },
    ],
    drives: [
      { mount: '/mnt/usb0', label: 'USB 1', kind: 'usb', total: 1000 * GB, free: 412 * GB, ejectable: true },
      { mount: '/data', label: 'Internal', kind: 'internal', total: 825 * GB, free: 311 * GB, ejectable: false },
    ],
    cache: { path: '/data/maxpack/caches', size: 2 * GB, free: 311 * GB, low_space: false, ampr_present: true },
    log: ['2026-11-23 20:00:00Z [1000] web UI on port 8095', '2026-11-23 20:00:02Z [3000] installer: ready in 412 ms'],
  };
}

const settings = {
  scanpath: [], exclude: ['/data/homebrew/old-copies'], inject_ampr: true, add_emulators: true, ampr_auto_update: false, backports: true,
  scan_interval_seconds: 10, stability_wait_seconds: 10, auto_remove_missing: true,
  auto_remove_delay_seconds: 300, low_space_gb: 20, notify: true, log: true, debug: false,
};

// ---- the installer (/api/inst/<x>: the engine's /api/<x>, vendor/maxpack src/web/mp_api.h) ----
const found = [
  { path: '/mnt/usb0/Astro Example.p5z', kind: 'p5z', packed: true, title_id: 'PPSA07890', version: '01.002.000',
    name: 'Astro Example', p5z_size: 21 * GB, install_size: 24 * GB, original_size: 41 * GB },
  { path: '/mnt/usb0/homebrew/PPSA08901', kind: 'folder', packed: false, title_id: 'PPSA08901', version: '',
    name: 'Folder Quest', p5z_size: 12 * GB, install_size: 12 * GB, original_size: 12 * GB },
  { path: '/mnt/usb0/Juego [PPSA06789].exfat', kind: 'image', packed: false, title_id: 'PPSA06789',
    version: '01.000.000', name: 'Imagen de ejemplo', p5z_size: 30 * GB, install_size: 30 * GB, original_size: 30 * GB },
  // Larger than the internal SSD's free space (311 GB): fits only on the USB drive.
  { path: '/mnt/usb0/Huge Example.p5z', kind: 'p5z', packed: true, title_id: 'PPSA08000', version: '01.000.000',
    name: 'Huge Example', p5z_size: 280 * GB, install_size: 330 * GB, original_size: 400 * GB },
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
    case 'failed': return { kind: 'install', title_id: 'PPSA07890', code: 'io_error', detail: 'no se pudo escribir /data/maxpack/tmp/PPSA07890/eboot.bin' };
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
      return { ok: true, found: { path: p, kind: /\.p5z$/i.test(p) ? 'p5z' : /\.(exfat|ffpkg|ffpfs|ffpfsc)$/i.test(p) ? 'image' : 'folder',
        packed: false, title_id: 'PPSA09999', version: '', name: leaf.replace(/\.[^.]+$/, ''), p5z_size: 8 * GB,
        install_size: 9 * GB, original_size: 15 * GB } };
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
      if (!rec) return err('not_found', 'ese juego no está instalado con MaxPack');
      if (!body.confirm) return err('needs_confirm', rec.dir);
      installed = installed.filter((x) => x !== rec);
      lastExtra = { kind: 'uninstall', title_id: rec.title_id, code: 'done', detail: '' };
      return ok;
    }
    case 'discard': {
      const before = pending.length;
      pending = pending.filter((x) => x.title_id !== body.title_id);
      return pending.length < before ? ok : err('not_found', 'no hay ninguna instalación a medias de ese juego');
    }
    default: return null;
  }
}

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
  let chunks = '';
  req.on('data', (c) => { chunks += c; });
  req.on('end', () => {
    const why = refusal(req);
    if (why) {
      console.log(`refused ${req.method} ${req.url} (${why}): Host ${req.headers.host}, Origin ${req.headers.origin}`);
      if (p.startsWith('/api/inst/')) return json(res, 403, { ok: false, code: 'forbidden', detail: REFUSED[why][0] });
      return send(res, 403, 'text/plain; charset=utf-8', REFUSED[why].join('\n') + '\n');
    }
    if (req.method === 'GET' && p === '/') {
      const html = fs.readFileSync(path.join(root, 'src/ui.html'), 'utf8').replace(/__VERSION__/g, '1.0.0-mock');
      return send(res, 200, 'text/html; charset=utf-8', html);
    }
    if (req.method === 'GET' && p === '/app-icon.png') return send(res, 200, 'image/png', fs.readFileSync(path.join(root, 'ps5/icon0.png')));
    if (p === '/mock') {
      for (const k of ['waiting', 'job', 'last', 'update', 'update_fail']) if (url.searchParams.has(k)) mock[k] = url.searchParams.get(k);
      if (url.searchParams.has('empty')) mock.empty = url.searchParams.get('empty') === '1';
      if (url.searchParams.has('job')) { extraJob = null; jobStart = Date.now(); }
      if (url.searchParams.has('last')) lastExtra = null;
      return json(res, 200, mock);
    }
    if (req.method === 'GET' && p === '/api/state') return json(res, 200, apiState());
    if (req.method === 'GET' && p === '/api/settings') return json(res, 200, settings);
    if (req.method === 'GET' && p === '/api/ampr') return json(res, 200, { kind: 'bundled', version: 'v0.5.0', updating: false });
    if (req.method === 'GET' && p.startsWith('/api/sizes/')) return json(res, 200, { game: 38 * GB, dlc: 0, save: 120 * MB, cache: 210 * MB });
    if (req.method === 'GET' && p.startsWith('/api/report/')) {
      const g = games().find((x) => x.title_id === p.slice('/api/report/'.length));
      if (!g) return send(res, 404, 'text/plain', 'not found');
      // The shape of maxpack_logic::report::text, with fixed values.
      const kinds = { pkg: ['pkg', 'pkg'], p5z: ['p5z', 'p5z'], plain: ['folder', 'carpeta'], zstd: ['folder (zstd)', 'carpeta zstd'], lz4: ['folder (lz4)', 'carpeta lz4'] };
      const [en, es] = kinds[g.kind === 'folder' ? g.packing : g.kind];
      const text = [`MaxPack 1.0.0: game report`, 'Firmware: 13.60', 'Mounting: MaxPack mounts the games',
        `Game: ${g.title_name} (${g.title_id}), version ${g.version}`, `Source: ${g.path} (${en}, ${g.drive})`,
        `Status: ${g.status}${g.error ? ` (${g.error})` : ''}`].concat(g.hidden ? ['Hidden in the library: yes'] : [],
        ['Backport: none', 'ampr-emu-ng: bundled v0.5.0-maxpack.3', 'PlayGo stub: on',
          `Last crash: ${g.crash ? `2026-11-23 20:10:00Z in ${g.crash.process}, ${g.crash.status} (console dump folder ${g.title_id}_1795166239)` : 'none recorded'}`,
          '', 'Row for docs/compatibility.md (fill in the result and the date):',
          `| ${g.title_id} | ${g.title_name} | ${g.version} | ${es} | 13.60 | MaxPack 1.0.0 | ? | ? |`, '',
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
      if (runningNow().length) return fail('game_running');
      if (UPDATE_FAILS[mock.update_fail]) return fail(mock.update_fail);
      // Sent: the "new copy" answers with its version a little later, as a real take-over does.
      setTimeout(() => { version = v.replace(/^v/, ''); }, 3000);
      return json(res, 200, {});
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
      try { body = chunks ? JSON.parse(chunks) : {}; } catch (e) { return json(res, 400, { ok: false, code: 'bad_json', detail: 'el cuerpo no es JSON válido' }); }
      if (!/^application\/json/i.test(req.headers['content-type'] || ''))
        return json(res, 403, { ok: false, code: 'forbidden', detail: 'petición rechazada: solo se acepta la página de MaxPack' });
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
  console.log(`mock UI on http://localhost:${port}/ (waiting=${mock.waiting}, job=${mock.job}, last=${mock.last}${mock.empty ? ', empty' : ''}, update=${mock.update}, update_fail=${mock.update_fail})`);
  console.log(`switch at run time: http://localhost:${port}/mock?waiting=none&job=paused&last=needs_confirm`);
  // The addresses to try the banner's URL with (see the top of the file).
  const lan = Object.values(require('os').networkInterfaces()).flat().filter((a) => a && a.family === 'IPv4' && !a.internal);
  for (const a of lan) console.log(`banner shows this address: http://${a.address}:${port}/`);
  console.log(`opened by a name, every request answers 403: http://${require('os').hostname()}:${port}/`);
});
