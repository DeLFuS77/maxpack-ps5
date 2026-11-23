#!/usr/bin/env node
// Development only: serves src/ui.html with fixed data in place of the payload, to look at the page
// (Library with ShadowMountPlus images, Install, the "installer only" banner, an install in
// progress) without a console. Nothing here goes into maxpack.elf.
//
//   node tools/mock_ui.js [--port 8095] [--waiting other|kstuff|none] [--job on|paused|off]
//                         [--last done|failed|needs_confirm|none] [--empty]
//   then open http://localhost:8095/ and the Install tab.
//
// Defaults: --waiting other (ShadowMountPlus running: the banner and ShadowMountPlus connected in
// Install), --job on (a .p5z being copied, ~2 % a second), --last none.
// --empty: no games, images, found items or installs (the empty states).
// The same switches at run time, without a restart (the page picks them up on its next poll):
//   http://localhost:8095/mock?waiting=none&job=paused&last=needs_confirm&empty=0
// The page's POSTs answer as the payload would (install queues a job, pause pauses it and it ends
// as an unfinished install, a path with "replace" in it asks to confirm a replace, and so on).
// /api/inst/* checks Host as the engine does (host_ok: an IPv4, an [IPv6] or localhost), so the
// banner's address can be checked from another device: opened at this PC's LAN IP
// (http://192.168.x.y:8095/) the banner shows that address; opened by a name (http://<pc-name>:8095/)
// it shows http://<PS5-IP>:8095 and Install answers 403, as on the console; on localhost or
// 127.0.0.1 it shows the placeholder too (a loopback means nothing to a phone).
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
};

const GB = 1024 * 1024 * 1024;
const MB = 1024 * 1024;
// Fixed, so the page does not ask for the cards' art again on every poll.
const BUILT = Math.floor(Date.now() / 1000) - 3600;

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
    drive: '/data', size: kind === 'pkg' ? 38 * GB : 0, status, error: null, cache_size: 210 * MB,
    cache_built: BUILT, backport: null, playgo: false, playgo_off: false,
  }, extra || {});
  return [
    g('PPSA01234', 'Astro Example', 'pkg', null, 'ready', { path: '/mnt/usb0/homebrew/astro.pkg', drive: '/mnt/usb0' }),
    g('PPSA02345', 'Folder Quest', 'folder', 'zstd', 'ready'),
    g('PPSA03456', 'Plain Racer', 'folder', 'plain', 'building', { cache_built: null, cache_size: 0 }),
    g('PPSA04567', 'Lz4 Tactics', 'folder', 'lz4', 'pending'),
    g('PPSA05678', 'Broken Sample', 'pkg', null, 'error', { error: 'PPSA05678 (/data/homebrew/broken.pkg): bad header' }),
  ];
}

function apiState() {
  return {
    version: '0.3.1', busy: false, paused: false, waiting: waitingValue(), running: [],
    games: games(),
    images: mock.empty ? [] : [
      { path: '/data/homebrew/Juego [PPSA06789].exfat', kind: 'exfat', title_id: 'PPSA06789' },
      { path: '/mnt/usb0/homebrew/varios/otro.ffpkg', kind: 'ffpkg' },
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
  scanpath: [], inject_ampr: true, add_emulators: true, ampr_auto_update: false, backports: true,
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
// The engine's host_ok (vendor/maxpack src/web/mp_api.c): an IP, an [IPv6] or localhost, with a port or not.
function hostOk(h) {
  if (!h) return false;
  if (h[0] === '[') return h.indexOf(']') >= 0;
  const name = h.split(':')[0];
  return name === 'localhost' || /^[0-9.]*$/.test(name) && name.split('.').length === 4;
}

http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const p = url.pathname;
  let chunks = '';
  req.on('data', (c) => { chunks += c; });
  req.on('end', () => {
    if (req.method === 'GET' && p === '/') {
      const html = fs.readFileSync(path.join(root, 'src/ui.html'), 'utf8').replace(/__VERSION__/g, '0.3.1-mock');
      return send(res, 200, 'text/html; charset=utf-8', html);
    }
    if (req.method === 'GET' && p === '/app-icon.png') return send(res, 200, 'image/png', fs.readFileSync(path.join(root, 'ps5/icon0.png')));
    if (p === '/mock') {
      for (const k of ['waiting', 'job', 'last']) if (url.searchParams.has(k)) mock[k] = url.searchParams.get(k);
      if (url.searchParams.has('empty')) mock.empty = url.searchParams.get('empty') === '1';
      if (url.searchParams.has('job')) { extraJob = null; jobStart = Date.now(); }
      if (url.searchParams.has('last')) lastExtra = null;
      return json(res, 200, mock);
    }
    if (req.method === 'GET' && p === '/api/state') return json(res, 200, apiState());
    if (req.method === 'GET' && p === '/api/settings') return json(res, 200, settings);
    if (req.method === 'GET' && p === '/api/ampr') return json(res, 200, { kind: 'bundled', version: 'v0.5.0', updating: false });
    if (req.method === 'GET' && p.startsWith('/api/sizes/')) return json(res, 200, { game: 38 * GB, dlc: 0, save: 120 * MB, cache: 210 * MB });
    if (p.startsWith('/api/inst/') && !hostOk(req.headers.host))
      return json(res, 403, { ok: false, code: 'forbidden', detail: 'petición rechazada: solo se acepta la página de MaxPack' });
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
    if (req.method === 'POST' && p.startsWith('/api/')) return send(res, 204, 'text/plain', '');
    return send(res, 404, 'text/plain', 'not found');
  });
}).listen(port, () => {
  console.log(`mock UI on http://localhost:${port}/ (waiting=${mock.waiting}, job=${mock.job}, last=${mock.last}${mock.empty ? ', empty' : ''})`);
  console.log(`switch at run time: http://localhost:${port}/mock?waiting=none&job=paused&last=needs_confirm`);
  // The addresses to try the banner's URL with (see the top of the file).
  const lan = Object.values(require('os').networkInterfaces()).flat().filter((a) => a && a.family === 'IPv4' && !a.internal);
  for (const a of lan) console.log(`banner shows this address: http://${a.address}:${port}/`);
  console.log(`banner shows the placeholder, Install answers 403: http://${require('os').hostname()}:${port}/`);
});
