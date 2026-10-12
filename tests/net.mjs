// networking across processes and hosts (the cross-host in-process run
// is the gates.json "net-arena" gate). Every check has a control that must
// be caught:
//   1. native pair, CW_NET_DESYNC=1: one peer miscounts a score once; the
//      in-band hash check must report it and the sync hashes must differ.
//   2. native <-> native over UDP loopback (two processes, real clock,
//      80 ms latency / 40 ms jitter / 10% loss injected on top): both
//      confirm the same hash at tick 600 with 0 desyncs. Controls: one side
//      miscounts (CW_NET_DESYNC on player 1), and rollback disabled
//      (CW_NET_NO_ROLLBACK on both): both must be caught.
//   3. cart <-> cart over wasmcart peer connections through tools/cwrelay.mjs:
//      two romdev sessions, two wasmcart-native (wcnative) players, and two
//      web-host carts in one Chromium page (the browser's WebSocket).
//      Control: one of the two carts is net_arena_peer_desync.
//   4. rollback cost per frame (printed, never a pass/fail: the machine is shared).
//
//   node tests/net.mjs [--only pair,udp,romdev,wcnative,chromium,perf]
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, readFileSync, mkdirSync, rmSync } from 'node:fs';
import { createSocket } from 'node:dgram';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { wcnativePlayer } from '../tools/lib/wcnative.mjs';
import { startRelay } from '../tools/cwrelay.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'net');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const only = argv.includes('--only') ? argv[argv.indexOf('--only') + 1].split(',') : null;
const want = (k) => !only || only.includes(k);
const EXE = join(ROOT, 'build/native/samples/net_arena');
const ASSETS = join(ROOT, 'samples/r2d_assets');
const SYNC_TICK = 600, WANT_CHECKED = SYNC_TICK / 10 + 1;
let failed = 0;
const ok = (cond, what) => { console.log(`${cond ? 'ok  ' : 'FAIL'} ${what}`); if (!cond) failed++; return cond; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const short = (s) => ['net.a.hash', 'net.a.desyncs', 'net.a.checked', 'net.a.rollbacks', 'net.local_player'].map((k) => `${k.replace('net.a.', '')}=${s[k]}`).join(' ');

function nativeEnv(extra) {
  return { ...process.env, SDL_VIDEODRIVER: 'offscreen', SDL_AUDIODRIVER: 'dummy', CW_ASSETS: ASSETS, ...extra };
}
function runPair(env, frames = 800) {
  const dump = join(OUT, `pair-${process.pid}-${Math.random().toString(36).slice(2)}.json`);
  const r = spawnSync(EXE, [], { env: nativeEnv({ CW_FRAMES: String(frames), CW_DEBUG_DUMP: dump, ...env }), encoding: 'utf8', timeout: 120000 });
  if (r.status !== 0) throw new Error(`net_arena exited ${r.status}: ${r.stderr.slice(-800)}`);
  const s = JSON.parse(readFileSync(dump, 'utf8'));
  rmSync(dump);
  return s;
}
async function freeUdpPort() {
  const s = createSocket('udp4');
  await new Promise((r) => s.bind(0, '127.0.0.1', r));
  const p = s.address().port;
  await new Promise((r) => s.close(r));
  return p;
}

// ── 1. the in-process pair: the desync control ─────────────────────────
if (want('pair')) {
  const good = runPair({});
  ok(good['net.match'] === 1 && good['net.a.desyncs'] === 0 && good['net.a.checked'] >= WANT_CHECKED,
    `pair: both peers confirm ${good['net.a.hash']} at tick ${SYNC_TICK}, ${good['net.a.checked']} hashes compared, 0 desyncs`);
  const bad = runPair({ CW_NET_DESYNC: '1' });
  ok(bad['net.match'] === 0 && bad['net.a.desyncs'] > 0 && bad['net.b.desyncs'] > 0,
    `pair control (one peer miscounts at tick 300): caught, ${bad['net.a.desyncs']} desyncs, hashes ${bad['net.a.hash']} vs ${bad['net.b.hash']}`);
}

// ── 2. native <-> native over UDP ──────────────────────────────────────
async function udpRun(tag, env0, env1) {
  const [p0, p1] = [await freeUdpPort(), await freeUdpPort()];
  const runs = [[p0, p1, env0], [p1, p0, env1]].map(([mine, theirs, extra], i) => {
    const dump = join(OUT, `udp-${tag}-${i}.json`);
    rmSync(dump, { force: true });
    const child = spawn(EXE, [], { env: nativeEnv({ CW_NET_UDP: String(mine), CW_NET_PEER: `127.0.0.1:${theirs}`, CW_NET_PLAYER: String(i),
      CW_DEBUG_DUMP: dump, ...extra }), stdio: ['ignore', 'pipe', 'pipe'] });
    let log = '';
    child.stdout.on('data', (d) => (log += d));
    child.stderr.on('data', (d) => (log += d));
    const done = new Promise((r) => child.on('close', (code) => r(code)));
    return { child, dump, done, get log() { return log; } };
  });
  const timer = setTimeout(() => runs.forEach((r) => r.child.kill('SIGTERM')), 90000);
  await Promise.all(runs.map((r) => r.done));
  clearTimeout(timer);
  return runs.map((r) => {
    if (!existsSync(r.dump)) throw new Error(`udp ${tag}: no state dump\n${r.log.slice(-1500)}`);
    return JSON.parse(readFileSync(r.dump, 'utf8'));
  });
}
if (want('udp')) {
  const [a, b] = await udpRun('good', {}, {});
  console.log(`     udp p0: ${short(a)}\n     udp p1: ${short(b)}`);
  ok(a['net.done'] === 1 && b['net.done'] === 1 && a['net.a.hash'] === b['net.a.hash'] && a['net.a.hash'] !== 0 &&
     a['net.a.desyncs'] === 0 && b['net.a.desyncs'] === 0 && a['net.a.checked'] >= WANT_CHECKED && b['net.a.checked'] >= WANT_CHECKED,
    `udp: two processes confirm the same state at tick ${SYNC_TICK} under 80 ms / 40 ms jitter / 10% loss (${a['net.a.rollbacks']} + ${b['net.a.rollbacks']} rollbacks)`);
  const [c, d] = await udpRun('desync', {}, { CW_NET_DESYNC: '1' });
  ok(c['net.a.desyncs'] > 0 && d['net.a.desyncs'] > 0 && c['net.a.hash'] !== d['net.a.hash'],
    `udp control (player 1 miscounts): caught on both sides (${c['net.a.desyncs']} / ${d['net.a.desyncs']} desyncs)`);
  const [e, f] = await udpRun('norollback', { CW_NET_NO_ROLLBACK: '1' }, { CW_NET_NO_ROLLBACK: '1' });
  ok(e['net.a.desyncs'] > 0 && f['net.a.desyncs'] > 0 && e['net.a.hash'] !== f['net.a.hash'],
    `udp control (rollback disabled under latency): caught (${e['net.a.desyncs']} / ${f['net.a.desyncs']} desyncs)`);
}

// ── 3. cart <-> cart over wasmcart peers, through the relay ────────────
const cartPath = (name) => join(ROOT, 'build/cart/carts', `${name}.wasc`);
async function romdevCall(tool, a, session) {
  const res = await fetch(`http://127.0.0.1:${process.env.ROMDEV_PORT || 7331}/tool/${tool}`, { method: 'POST',
    headers: { 'Content-Type': 'application/json', 'x-romdev-session': session }, body: JSON.stringify(a) });
  const text = await res.text();
  let body;
  try { body = JSON.parse(text); } catch { body = text; }
  if (!res.ok || body?.isError) throw new Error(`romdev ${tool}: ${typeof body === 'string' ? body : JSON.stringify(body).slice(0, 1500)}`);
  if (body && Array.isArray(body.content)) {
    const t = body.content.find((x) => x.type === 'text');
    try { return JSON.parse(t.text); } catch { return t ? t.text : body; }
  }
  return body;
}
async function romdevPair(tag, carts) {
  const relay = await startRelay({ port: 47731 });
  const sessions = carts.map((_, i) => `cartwheel-net-${tag}-${i}-${process.pid}`);
  const state = async (s) => {
    const st = await romdevCall('wasm', { op: 'debugState' }, s);
    return Object.fromEntries((st.fields || []).map((f) => [f.name, f.values ?? f.value]));
  };
  try {
    // one after the other: the first takes slot 0, the second slot 1
    for (let i = 0; i < carts.length; i++) {
      await romdevCall('loadMedia', { platform: 'wasmcart', path: cartPath(carts[i]), deterministicSeed: 1, deterministicStepMs: 1000 / 60 }, sessions[i]);
      await sleep(100);
    }
    let out = [];
    for (let i = 0; i < 400; i++) { // interleave: each session steps 4 frames, the event loop delivers between calls
      for (const s of sessions) await romdevCall('frame', { op: 'step', frames: 4 }, s);
      if (i % 10 === 9) {
        out = await Promise.all(sessions.map(state));
        if (out.every((o) => o['net.done'] === 1)) break;
      }
    }
    return out;
  } finally {
    for (const s of sessions) await romdevCall('host', { op: 'shutdown' }, s).catch(() => {});
    await relay.close();
  }
}
if (want('romdev')) {
  const [a, b] = await romdevPair('good', ['net_arena_peer', 'net_arena_peer']);
  console.log(`     romdev A: ${short(a)}\n     romdev B: ${short(b)}`);
  ok(a['net.done'] === 1 && b['net.done'] === 1 && a['net.a.hash'] === b['net.a.hash'] && a['net.a.hash'] !== 0 &&
     a['net.a.desyncs'] === 0 && b['net.a.desyncs'] === 0 && a['net.local_player'] !== b['net.local_player'],
    `romdev: two carts over wc_peer + cwrelay confirm the same state at tick ${SYNC_TICK} (${a['net.a.checked']} hashes compared)`);
  const [c, d] = await romdevPair('desync', ['net_arena_peer', 'net_arena_peer_desync']);
  ok(c['net.a.desyncs'] > 0 && d['net.a.desyncs'] > 0 && c['net.a.hash'] !== d['net.a.hash'],
    `romdev control (one cart miscounts): caught (${c['net.a.desyncs']} / ${d['net.a.desyncs']} desyncs)`);
}

function wcnativeExe() {
  return wcnativePlayer(); // the headless-capable player (tools/lib/wcnative.mjs)
}
async function wcnativePair(tag, carts) {
  const exe = wcnativeExe();
  const relay = await startRelay({ port: 47731 });
  const procs = [];
  try {
    for (let i = 0; i < carts.length; i++) {
      const shot = join(OUT, `wcn-${tag}-${i}.ppm`);
      rmSync(shot, { force: true });
      const p = spawn(exe, [cartPath(carts[i]), '--fixed-step', String(1000 / 60), '--shot', '1300', shot, '--uncapped'],
        { env: testEnv({ ...process.env, SDL_AUDIODRIVER: 'dummy' }), stdio: ['ignore', 'pipe', 'pipe'] });
      p.log = '';
      p.stdout.on('data', (x) => (p.log += x));
      p.stderr.on('data', (x) => (p.log += x));
      p.shot = shot;
      procs.push(p);
      await sleep(300); // join order = slot order
    }
    const t0 = Date.now();
    while (Date.now() - t0 < 120000 && !procs.every((p) => existsSync(p.shot) || p.exitCode !== null)) await sleep(200);
    await sleep(400);
  } finally {
    for (const p of procs) p.kill('SIGTERM');
    await relay.close();
  }
  await sleep(300);
  return procs.map((p) => {
    const m = /sync tick (\d+) hash ([0-9a-f]+) \(local\), (\d+) hashes compared, (\d+) desyncs, (\d+) rollbacks/.exec(p.log);
    const slot = /assigned us player (\d+)/.exec(p.log);
    return m ? { hash: m[2], checked: +m[3], desyncs: +m[4], rollbacks: +m[5], slot: slot ? +slot[1] : -1, shot: existsSync(p.shot) }
      : { error: p.log.split('\n').slice(-6).join(' | ') };
  });
}
if (want('wcnative')) {
  const [a, b] = await wcnativePair('good', ['net_arena_peer', 'net_arena_peer']);
  console.log(`     wcnative A: ${JSON.stringify(a)}\n     wcnative B: ${JSON.stringify(b)}`);
  ok(!a.error && !b.error && a.hash === b.hash && a.desyncs === 0 && b.desyncs === 0 && a.checked >= WANT_CHECKED && a.slot !== b.slot && a.shot && b.shot,
    `wcnative: two wasmcart-run players over wc_peer + cwrelay confirm the same state at tick ${SYNC_TICK}`);
  const [c, d] = await wcnativePair('desync', ['net_arena_peer', 'net_arena_peer_desync']);
  ok(!c.error && !d.error && c.desyncs > 0 && d.desyncs > 0 && c.hash !== d.hash,
    `wcnative control (one cart miscounts): caught (${c.desyncs} / ${d.desyncs} desyncs)`);
}

// Chromium: two CartHostWeb instances in one page, each dialing the relay
// with the browser's own WebSocket (the web host's wc_peer_open), stepped in
// turn with an event-loop yield between frames, as rAF would.
async function chromiumPair(tag, carts) {
  const { createServer } = await import('node:http');
  const { launchChromium } = await import('../tools/lib/chromium.mjs');
  const WC = join(ROOT, 'deps/wasmcart');
  const page = `<!doctype html><html><body>
<script type=importmap>{"imports":{"fflate":"/wc/node_modules/fflate/esm/browser.js"}}</script>
<script type=module>
import { CartHostWeb } from '/wc/src/CartHostWeb.js';
function debugState(host) {
  const ex = host.instance.exports, dv = new DataView((host.memory || ex.memory).buffer), st = {};
  const str = (q) => { let s = ''; for (let b; (b = dv.getUint8(q)); q++) s += String.fromCharCode(b); return s; };
  for (let p = ex.wc_debug_state(); dv.getUint32(p, true); p += 16) {
    const vp = dv.getUint32(p + 4, true), type = dv.getUint8(p + 8), len = dv.getUint32(p + 12, true);
    if (type > 7) continue;
    const rd = [(q) => dv.getUint8(q), (q) => dv.getInt8(q), (q) => dv.getUint16(q, true), (q) => dv.getInt16(q, true),
      (q) => dv.getUint32(q, true), (q) => dv.getInt32(q, true), (q) => dv.getFloat32(q, true), (q) => dv.getFloat64(q, true)][type];
    const size = [1, 1, 2, 2, 4, 4, 4, 8][type];
    st[str(dv.getUint32(p, true))] = len === 1 ? rd(vp) : Array.from({ length: len }, (_, i) => rd(vp + i * size));
  }
  return st;
}
window.run = async (carts) => {
  const hosts = [];
  const pads = [{ connected: true, buttons: 0 }, { connected: false }, { connected: false }, { connected: false }];
  for (let i = 0; i < carts.length; i++) {
    const h = new CartHostWeb();
    const canvas = document.createElement('canvas');
    document.body.appendChild(canvas);
    await h.load('/cart/' + carts[i] + '.wasc', { glBackend: () => canvas.getContext('webgl2', { antialias: false }) });
    let k = 0;
    h._writeTime = ((orig) => function () { orig.call(this, k * 1000 / 60, 1000 / 60, k); })(h._writeTime);
    h.frame = () => { k++; };
    hosts.push(h);
    for (let f = 0; f < 10; f++) { await h.runFrame(pads); h.frame(); await new Promise((r) => setTimeout(r, 10)); } /* joins first: slot order */
  }
  for (let f = 0; f < 4000; f++) {
    for (const h of hosts) { await h.runFrame(pads); h.frame(); }
    await new Promise((r) => setTimeout(r, 0));
    if (f % 30 === 29 && hosts.every((h) => debugState(h)['net.done'] === 1)) break;
  }
  return hosts.map(debugState);
};
window.ready = true;
</script></body></html>`;
  const server = createServer((req, res) => {
    const url = decodeURIComponent(req.url.split('?')[0]);
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    if (url === '/') { res.setHeader('Content-Type', 'text/html'); res.end(page); return; }
    const file = url.startsWith('/cart/') ? cartPath(url.slice(6).replace(/\.wasc$/, '')) : url.startsWith('/wc/') ? join(WC, url.slice(4)) : null;
    if (!file || !existsSync(file)) { res.statusCode = 404; res.end(); return; }
    res.setHeader('Content-Type', file.endsWith('.js') ? 'text/javascript' : file.endsWith('.wasm') ? 'application/wasm' : 'application/octet-stream');
    res.end(readFileSync(file));
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const relay = await startRelay({ port: 47731 });
  const { chromium } = await import(join(WC, 'node_modules', 'playwright', 'index.mjs'));
  const { browser, verify } = await launchChromium(chromium, OUT);
  try {
    const p = await browser.newPage();
    const logs = [];
    p.on('console', (m) => logs.push(m.text()));
    p.on('pageerror', (e) => logs.push('pageerror: ' + e.message));
    await p.goto(`http://127.0.0.1:${server.address().port}/`);
    await p.waitForFunction('window.ready === true');
    await verify(p);
    return await p.evaluate((c) => window.run(c), carts).catch((e) => { throw new Error(`${e.message}\n${logs.slice(-10).join('\n')}`); });
  } finally {
    await browser.close();
    server.close();
    await relay.close();
  }
}
if (want('chromium')) {
  const [a, b] = await chromiumPair('good', ['net_arena_peer', 'net_arena_peer']);
  console.log(`     chromium A: ${short(a)}\n     chromium B: ${short(b)}`);
  ok(a['net.done'] === 1 && b['net.done'] === 1 && a['net.a.hash'] === b['net.a.hash'] && a['net.a.hash'] !== 0 &&
     a['net.a.desyncs'] === 0 && b['net.a.desyncs'] === 0 && a['net.local_player'] !== b['net.local_player'],
    `chromium: two web-host carts over the browser's WebSocket + cwrelay confirm the same state at tick ${SYNC_TICK}`);
  const [c, d] = await chromiumPair('desync', ['net_arena_peer', 'net_arena_peer_desync']);
  ok(c['net.a.desyncs'] > 0 && d['net.a.desyncs'] > 0 && c['net.a.hash'] !== d['net.a.hash'],
    `chromium control (one cart miscounts): caught (${c['net.a.desyncs']} / ${d['net.a.desyncs']} desyncs)`);
}

// ── 4. rollback cost per frame ─────────────────────────────────────────
if (want('perf')) {
  console.log('     rollback cost per frame (native pair, peer A; restore + re-simulation, and the per-tick snapshot):');
  const cases = [
    ['80 ms / 40 jitter / 10% loss', {}],
    ['160 ms / 60 jitter / 15% loss', { CW_NET_LATENCY_MS: '160', CW_NET_JITTER_MS: '60', CW_NET_LOSS: '0.15' }],
    ['80 ms + 300 ballast nodes', { CW_NET_BALLAST: '300' }],
    ['160 ms + 1000 ballast nodes', { CW_NET_LATENCY_MS: '160', CW_NET_JITTER_MS: '60', CW_NET_LOSS: '0.15', CW_NET_BALLAST: '1000' }],
  ];
  for (const [label, env] of cases) {
    const s = runPair(env);
    console.log(`     ${label.padEnd(30)} rollbacks ${String(s['net.a.rollbacks']).padStart(4)} (deepest ${String(s['net.a.max_depth']).padStart(2)}, ` +
      `${String(s['net.a.resim']).padStart(5)} ticks re-sim)  rollback ${s['net.a.rb_ms'].toFixed(3)} ms/frame avg, ` +
      `${s['net.a.rb_ms_max'].toFixed(3)} max  snapshot ${s['net.a.save_ms'].toFixed(3)} ms/frame  ${s['net.state_bytes']} B` +
      `  match ${s['net.match']}`);
    if (!s['net.match']) ok(false, `perf case "${label}" did not converge`);
  }
}

process.exit(failed ? 1 : 0);
