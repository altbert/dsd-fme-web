#!/usr/bin/env node
// Retakes the README screenshots (docs/screenshots/) from a dsd-fme instance on a simulated receiver
// with made-up demo data: no dongle, nothing from your config, no real traffic. See README.md here.
//
//   node tools/screenshots/shoot.mjs [--out DIR] [--dsd PATH] [--chromium CMD] [--rate SPS] [--shots LIST] [--keep]
import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '..', '..');
const SHOTS = ['desktop', 'activity', 'map', 'phone'];
const { values: opt } = parseArgs({ options: {
  out: { type: 'string', default: path.join(repo, 'build', 'screenshots') },
  dsd: { type: 'string', default: path.join(repo, 'build', 'dsd-fme') },
  chromium: { type: 'string' },
  rate: { type: 'string', default: '250000' },
  shots: { type: 'string', default: SHOTS.join(',') },
  keep: { type: 'boolean', default: false },
  help: { type: 'boolean', short: 'h', default: false },
} });
if (opt.help) {
  console.log(`node tools/screenshots/shoot.mjs [options]
  --out DIR       where the WebP files go (default build/screenshots; docs/screenshots replaces the README's)
  --dsd PATH      dsd-fme binary (default build/dsd-fme)
  --chromium CMD  Chromium or Chrome (default: the first of chromium, chromium-browser, google-chrome-stable,
                  google-chrome on the PATH, or $CHROME)
  --rate SPS      RTL sample rate (default 250000; 0 keeps dsd-fme's ~1 MSPS, where the channel band is thin)
  --shots LIST    any of ${SHOTS.join(',')} (default all)
  --keep          keep the temporary directory (dsd-fme log, PNGs)`);
  process.exit(0);
}

// The demo band, all made up: the tuned repeater, an analog simplex and a bursty M17 hotspot 50 kHz to
// either side, a carrier, and one bookmark outside the span. At 0.25 MSPS the 12 kHz channel band is
// wide enough to see. Stations stay between -rate/4 and +rate/2 of the tuned frequency: dsd-fme tunes
// the dongle rate/4 up, so the left quarter of the spectrum shows the wrapped top of the capture
const TUNED = 433475000, BW_KHZ = 12;
const STATIONS = '433475000:m17:30,433425000:fm:24,433525000:m17burst:22,433575000:cw:16';
const BURST_HZ = 433525000 - TUNED; // the spectrum shots wait for this one to be on the air
const BOOKMARKS = { version: 1, bookmarks: [
  { freq: 433475000, name: 'Demo repeater', group: 'Demo', mode: 'dmr', mod: '' },
  { freq: 433425000, name: 'Analog simplex', group: 'Demo', mode: 'analog', mod: '' },
  { freq: 433525000, name: 'M17 hotspot', group: 'Demo', mode: 'm17', mod: '' },
  { freq: 851012500, name: 'Trunked control channel', group: 'Examples', mode: 'p25p1', mod: '' },
] };

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fail = (msg) => { throw new Error(msg); };
const which = (cmd) => { const r = spawnSync('sh', ['-c', 'command -v "$1"', 'sh', cmd]); return r.status === 0 ? r.stdout.toString().trim() : ''; };
const kids = [];
const alive = (k) => k.exitCode === null && k.signalCode === null;
let tmp, dsd, chrome, page, logFile;

async function stop(k, grace = 0) {
  if (!k || !alive(k)) return;
  const done = new Promise((r) => k.once('exit', r));
  const exited = (ms) => Promise.race([done.then(() => true), sleep(ms).then(() => false)]);
  if (grace && await exited(grace)) return;
  k.kill('SIGTERM');
  if (await exited(5000)) return;
  k.kill('SIGKILL');
  await done;
}

let shutting;
function shutdown() {
  return shutting ||= (async () => {
    try { page?.close(); } catch {}
    try { // Browser.close also ends Chromium when its command is a wrapper script
      const [port, wsPath] = fs.readFileSync(path.join(tmp, 'chrome', 'DevToolsActivePort'), 'utf8').split('\n');
      const ws = new WebSocket(`ws://127.0.0.1:${port}${wsPath}`);
      await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
      ws.send(JSON.stringify({ id: 1, method: 'Browser.close' }));
    } catch {}
    await stop(chrome, 2000);
    await stop(dsd);
    if (tmp && !opt.keep) fs.rmSync(tmp, { recursive: true, force: true, maxRetries: 3 });
    else if (tmp) console.log('kept', tmp);
  })();
}
process.once('SIGINT', async () => { await shutdown(); process.exit(130); });

async function waitFor(what, fn, ms = 30000) {
  for (const end = Date.now() + ms; ;) {
    try { const v = await fn(); if (v) return v; } catch {}
    for (const k of kids) if (!alive(k)) fail(`${k === dsd ? 'dsd-fme' : 'Chromium'} exited while waiting for ${what}`);
    if (Date.now() > end) fail(`timed out waiting for ${what}`);
    await sleep(200);
  }
}

function freePort() {
  return new Promise((res, rej) => {
    const s = net.createServer().on('error', rej);
    s.listen(0, '127.0.0.1', () => { const { port } = s.address(); s.close(() => res(port)); });
  });
}

// the rtlsdr_* functions a binary imports (--undefined-only) or defines (--defined-only); null without nm
function rtlSyms(file, flag) {
  const r = spawnSync('nm', ['-D', flag, file]);
  return r.status === 0 ? new Set(r.stdout.toString().match(/\brtlsdr_\w+/g) || []) : null;
}

// A page over the DevTools protocol
async function openPage(port) {
  const tgt = await (await fetch(`http://127.0.0.1:${port}/json/new?about:blank`, { method: 'PUT' })).json();
  const ws = new WebSocket(tgt.webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  let id = 0;
  const pend = new Map(), errors = [];
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
    else if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
    else if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') errors.push(m.params.args.map((a) => a.value ?? a.description).join(' '));
  };
  const cmd = (method, params = {}) => new Promise((res, rej) => {
    const i = ++id;
    pend.set(i, (m) => (m.error ? rej(new Error(`${method}: ${m.error.message}`)) : res(m.result)));
    ws.send(JSON.stringify({ id: i, method, params }));
  });
  await cmd('Runtime.enable');
  await cmd('Page.enable');
  return {
    errors,
    async viewport(width, height, scale = 1, mobile = false) {
      await cmd('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: scale, mobile });
      await cmd('Emulation.setTouchEmulationEnabled', { enabled: mobile, maxTouchPoints: 5 });
    },
    go: (url) => cmd('Page.navigate', { url }),
    async eval(expression) {
      const r = await cmd('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
      if (r.exceptionDetails) fail('in the page: ' + (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
      return r.result.value;
    },
    async snap(file) { fs.writeFileSync(file, Buffer.from((await cmd('Page.captureScreenshot', { format: 'png' })).data, 'base64')); },
    async key(key, code, keyCode) { for (const type of ['keyDown', 'keyUp']) await cmd('Input.dispatchKeyEvent', { type, key, code, windowsVirtualKeyCode: keyCode }); },
    close: () => ws.close(),
  };
}

// dB the bursty station's strongest bin stands over the span's median, in the newest frame
const burstLevel = () => page.eval(`(() => { if (!lastFFT) return -99; const {bins, rate, vals} = lastFFT;
  const k = Math.round(bins/2 + ${BURST_HZ}/rate*bins), w = Math.max(2, Math.round(3000/rate*bins));
  let m = -999; for (let i = Math.max(0, k-w); i <= Math.min(bins-1, k+w); i++) m = Math.max(m, vals[i]);
  const s = Array.from(vals).sort((a, b) => a - b); return m - s[bins >> 1]; })()`);

// waits for the bursty station to go off, come back on and stay on a few seconds, so it shows in the
// spectrum trace and starts a segment in the waterfall
async function onAir(secs = 4) {
  const until = async (ok) => {
    for (const end = Date.now() + 30000; Date.now() < end; await sleep(100)) if (ok(await burstLevel())) return;
    console.warn('  the bursty station never changed state; taking the shot anyway');
  };
  await until((db) => db < 6);
  await until((db) => db > 15);
  await sleep(secs * 1000);
}

async function main() {
  if (typeof WebSocket !== 'function') fail('needs Node 22 or newer (built-in WebSocket)');
  const want = new Set(opt.shots.split(',').map((s) => s.trim()).filter(Boolean));
  const bad = [...want].filter((s) => !SHOTS.includes(s));
  if (bad.length || !want.size) fail(`--shots takes any of ${SHOTS.join(', ')}`);
  const bin = path.resolve(opt.dsd), out = path.resolve(opt.out), rate = Math.max(0, Math.round(+opt.rate || 0));
  if (!fs.existsSync(bin)) fail(`no dsd-fme at ${bin}: build it first (cmake -B build && cmake --build build)`);
  const browser = opt.chromium || process.env.CHROME || ['chromium', 'chromium-browser', 'google-chrome-stable', 'google-chrome'].map(which).find(Boolean);
  if (!browser) fail('no Chromium or Chrome found: pass --chromium');

  tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'dsdfme-shots-'));
  const so = path.join(tmp, 'libfakertl.so');
  const cc = spawnSync(process.env.CC || 'cc', ['-O2', '-shared', '-fPIC', '-o', so, path.join(here, 'fakertl.c'), '-lm'], { stdio: 'inherit' });
  if (cc.status !== 0) fail('building fakertl.c failed');
  const need = rtlSyms(bin, '--undefined-only'), have = rtlSyms(so, '--defined-only');
  if (!need || !have) console.warn('nm not found: not checking that fakertl.c stands in for every librtlsdr call');
  else if (!need.size) fail(`${bin} doesn't load librtlsdr (built without it, or linked statically), so the simulated receiver can't stand in`);
  else {
    const missing = [...need].filter((s) => !have.has(s));
    if (missing.length) fail(`dsd-fme calls ${missing.join(', ')}, which fakertl.c doesn't define yet: add them there`);
  }

  // dsd-fme in the temporary directory, which also holds its bookmarks and config (the Files tab lists ./WAV)
  const bm = path.join(tmp, 'bookmarks.json');
  fs.writeFileSync(bm, JSON.stringify(BOOKMARKS, null, 2));
  const webPort = await freePort();
  logFile = path.join(tmp, 'dsd-fme.log');
  const log = fs.openSync(logFile, 'w');
  dsd = spawn(bin, ['-i', `rtl:0:${TUNED / 1e6}M:0:0:${BW_KHZ}`, '-fs', '-o', 'null', '--web', `127.0.0.1:${webPort}`, '--web-bookmarks', bm], {
    cwd: tmp, stdio: ['ignore', log, log],
    env: { ...process.env, LD_PRELOAD: so, XDG_CONFIG_HOME: path.join(tmp, 'config'), FAKERTL_STATIONS: STATIONS },
  });
  kids.push(dsd);
  fs.closeSync(log);
  chrome = spawn(browser, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${path.join(tmp, 'chrome')}`, '--no-first-run',
    '--no-default-browser-check', '--hide-scrollbars', '--mute-audio', 'about:blank'], { stdio: 'ignore' });
  kids.push(chrome);
  const url = `http://127.0.0.1:${webPort}/`;
  await waitFor('the dsd-fme web server', async () => (await fetch(url)).ok);
  const cdpPort = await waitFor('Chromium', () => +fs.readFileSync(path.join(tmp, 'chrome', 'DevToolsActivePort'), 'utf8').split('\n')[0]);

  page = await openPage(cdpPort);
  await page.viewport(1440, 900);
  await page.go(url);
  await waitFor('the spectrum', () => page.eval(`typeof handleJSON === 'function' && !!lastFFT`));
  if (rate) {
    // dsd-fme picks the nearest rate it can run, 252 kSPS for 0.25 MSPS
    await page.eval(`send({cmd: 'rtl', rate: ${rate}})`);
    await waitFor('the new sample rate', () => page.eval(`!!lastFFT && Math.abs(lastFFT.rate - ${rate}) < ${rate * 0.05}`));
  }
  console.log(await page.eval(fs.readFileSync(path.join(here, 'demo_page.js'), 'utf8')));
  await sleep(16000); // fills the waterfall at the new rate

  const png = (name) => path.join(tmp, name + '.png'), taken = [];
  const shot = async (name) => { await page.snap(png(name)); taken.push(name); console.log('took', name); };
  if (want.has('desktop')) { await onAir(); await shot('desktop'); }
  if (want.has('activity')) {
    await page.eval(`(() => { const c = document.getElementById('act-count').closest('section, .card');
      window.scrollTo(0, c.getBoundingClientRect().top + scrollY - 58); })()`);
    await sleep(1200);
    await shot('activity');
  }
  if (want.has('map')) {
    await page.eval(`window.scrollTo(0, 0); document.getElementById('btn-map').click()`);
    await sleep(6000); // OpenStreetMap tiles
    await shot('map');
    await page.key('Escape', 'Escape', 27);
    await sleep(800);
  }
  if (want.has('phone')) {
    await page.viewport(390, 844, 2, true);
    await sleep(1500);
    await page.eval('window.scrollTo(0, 0)');
    await onAir();
    await shot('phone');
  }
  if (page.errors.length) console.warn('page errors:\n  ' + page.errors.join('\n  '));

  fs.mkdirSync(out, { recursive: true });
  const magick = which('magick') || which('convert');
  for (const name of taken) {
    if (!magick) { fs.copyFileSync(png(name), path.join(out, `web-${name}.png`)); continue; }
    const webp = path.join(out, `web-${name}.webp`);
    const r = spawnSync(magick, [png(name), '-strip', '-quality', '88', '-define', 'webp:method=6', webp], { stdio: 'inherit' });
    if (r.status !== 0) fail(`converting ${name} to WebP failed`);
    // -strip leaves only the image: no EXIF, XMP or color profile
    const b = fs.readFileSync(webp), chunks = [];
    for (let i = 12; i + 8 <= b.length; i += 8 + b.readUInt32LE(i + 4) + (b.readUInt32LE(i + 4) & 1)) chunks.push(b.toString('latin1', i, i + 4));
    if (chunks.some((c) => !['VP8 ', 'VP8L', 'VP8X', 'ALPH'].includes(c))) fail(`${webp} has metadata chunks: ${chunks.join(' ')}`);
  }
  if (!magick) console.warn('ImageMagick not found: wrote PNGs instead of WebP');
  console.log(`wrote ${taken.map((n) => `web-${n}.${magick ? 'webp' : 'png'}`).join(', ')} to ${out}`);
}

let code = 0;
try { await main(); }
catch (e) {
  console.error('shoot.mjs: ' + e.message);
  if (logFile && fs.existsSync(logFile)) console.error('dsd-fme log, last lines:\n' + fs.readFileSync(logFile, 'utf8').split('\n').slice(-15).join('\n'));
  code = 1;
}
await shutdown();
process.exit(code);
