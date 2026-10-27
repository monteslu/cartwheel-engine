// Showcase performance report: showcase_perf (the outdoor
// valley at 1920x1080, full quality: 193 lights, shadows, height fog,
// GTAO, TAA, bloom, SMAA) through its five time-of-day segments of 150 frames
// (morning, golden hour, dusk, night, the running cycle), on every host.
//
// The cart measures itself, the same way on every host: frame ms is render
// end to render end with a 1-pixel read after each frame (the whole frame's
// CPU and GPU work, not a pipelined submission rate), averaged per segment
// after 30 settling frames, plus the worst frame; GPU ms per pass come from
// the pipeline's timer queries (16-frame averages). Hosts without the debug
// state (wasmcart-run) report through the cart's per-segment stderr line.
// romdev's frame stepping is also timed from outside (wall ms per stepped
// frame, HTTP included), since a deterministic host may virtualize clocks.
//
// Checks: every segment of every host that ran averages under the budget
// (60 fps: 16.7 ms); the night segment has all 128 lamps on. Control: the same budget check against an impossible budget
// (0.05 ms) must fail, or the check cannot fail at all.
//
//   node tests/showcase-perf.mjs [--hosts native,romdev,chromium,wcnative] [--budget-ms 16.7]
//
// Chromium runs on CW_CHROMIUM_GPU (a PCI address) when set, on that GPU alone (tools/lib/chromium.mjs);
// the report names the GPU it got.
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFileSync, mkdirSync, writeFileSync, existsSync, mkdtempSync } from 'node:fs';
import { join, resolve, dirname, extname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { wcnativePlayer } from '../tools/lib/wcnative.mjs';
import { launchChromium } from '../tools/lib/chromium.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv, wgpuPin } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
testEnv(process.env); // what it starts renders offscreen and cannot reach the desktop

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'perf');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes('--' + k) ? argv[argv.indexOf('--' + k) + 1] : d);
const HOSTS = opt('hosts', 'native,romdev,chromium,wcnative').split(',');
const BUDGET = +opt('budget-ms', 1000 / 60);
const APP = 'showcase_perf', SEGS = 5, SEG = 150, FRAMES = SEGS * SEG;
const SEG_NAMES = ['morning', 'golden', 'dusk', 'night', 'cycle'];
const cart = join(ROOT, 'build/cart/carts', `${APP}.wasc`);

// "showcase-perf seg=3 frame_ms=1.2 max_ms=2 gpu_ms=0.9 lamps=128 scene=0.5 ..."
function parseLines(text) {
  const segs = {};
  for (const line of text.split('\n')) {
    const m = line.match(/showcase-perf (.*)$/);
    if (!m) continue;
    const o = Object.fromEntries(m[1].trim().split(/\s+/).map((kv) => kv.split('=')).map(([k, v]) => [k, +v]));
    segs[o.seg] = o;
  }
  return segs;
}
// the debug state's perf fields into the same shape
function fromState(s) {
  const segs = {};
  for (let k = 0; k < SEGS; k++)
    if (s[`perf.seg${k}.frame`] > 0) segs[k] = { seg: k, frame_ms: s[`perf.seg${k}.frame`], max_ms: s[`perf.seg${k}.max`], cpu_ms: s[`perf.seg${k}.cpu`], gpu_ms: s[`perf.seg${k}.gpu`] };
  return segs;
}
const passesOf = (s) => Object.fromEntries(Object.entries(s).filter(([k, v]) => k.startsWith('gpu.') && k !== 'gpu.total' && v >= 0).map(([k, v]) => [k.slice(4), v]));

function runNative() {
  return new Promise((res) => {
    const state = join(OUT, `${APP}.native.json`);
    const exe = join(ROOT, 'build/native/samples', APP);
    const p = spawn(exe, [], { env: { ...process.env, CW_FRAMES: String(FRAMES), CW_DEBUG_DUMP: state, CW_ASSETS: exe + '.assets',
      SDL_AUDIODRIVER: 'dummy', CW_GPU_TIMERS: '1' }, stdio: ['ignore', 'ignore', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => {
      if (code !== 0 || !existsSync(state)) return res({ error: `exit ${code}: ${err.trim().split('\n').slice(-2).join(' | ')}` });
      const s = JSON.parse(readFileSync(state, 'utf8'));
      res({ state: s, segs: fromState(s), passes: passesOf(s), gpu: 'native GLES (default EGL device)' });
    });
  });
}

async function romdevCall(tool, a, session) {
  const r = await fetch(`http://127.0.0.1:7331/tool/${tool}`, { method: 'POST',
    headers: { 'Content-Type': 'application/json', 'x-romdev-session': session }, body: JSON.stringify(a) });
  const text = await r.text();
  let body;
  try { body = JSON.parse(text); } catch { body = text; }
  if (!r.ok || (body && body.isError)) throw new Error(`romdev ${tool}: ${String(typeof body === 'string' ? body : JSON.stringify(body)).slice(0, 400)}`);
  if (body && Array.isArray(body.content)) {
    const t = body.content.find((c) => c.type === 'text');
    try { return JSON.parse(t.text); } catch { return t ? t.text : body; }
  }
  return body;
}

async function runRomdev() {
  const session = `cartwheel-showcase-perf-${process.pid}`;
  try {
    await romdevCall('loadMedia', { platform: 'wasmcart', gpuPower: wgpuPin().power, path: cart, deterministicSeed: 1, deterministicStepMs: 1000 / 60 }, session);
    // load ran frame 1; step each segment's remainder and time it
    const outside = {};
    let at = 1;
    for (let k = 0; k < SEGS; k++) {
      const settle = k * SEG + 30 - at;
      if (settle > 0) { await romdevCall('frame', { op: 'step', frames: settle }, session); at += settle; }
      const n = (k + 1) * SEG - at, t0 = performance.now();
      await romdevCall('frame', { op: 'step', frames: n }, session);
      outside[k] = (performance.now() - t0) / n;
      at += n;
    }
    const st = await romdevCall('wasm', { op: 'debugState' }, session);
    const s = {};
    for (const f of st.fields || []) s[f.name] = f.values ?? f.value;
    const segs = fromState(s);
    for (let k = 0; k < SEGS; k++) segs[k] = { ...(segs[k] || { seg: k }), outside_ms: outside[k] };
    return { state: s, segs, passes: passesOf(s), gpu: 'romdev wasmcart host' };
  } catch (e) {
    return { error: e.message.split('\n')[0] };
  } finally {
    await romdevCall('host', { op: 'shutdown' }, session).catch(() => {});
  }
}

const MIME = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.html': 'text/html', '.wasc': 'application/octet-stream' };
async function runChromium() {
  const WC = join(ROOT, 'deps/wasmcart');
  const page = `<!doctype html><html><body><canvas id=c></canvas>
<script type=importmap>{"imports":{"fflate":"/wc/node_modules/fflate/esm/browser.js"}}</script>
<script type=module>
import { CartHostWeb } from '/wc/src/CartHostWeb.js';
window.run = async (frames) => {
  const host = new CartHostWeb();
  const canvas = document.getElementById('c');
  let lost = -1, k = 0;
  canvas.addEventListener('webglcontextlost', () => { lost = k; });
  await host.load('/cart.wasc', { glBackend: () => canvas.getContext('webgl2', { antialias: false, preserveDrawingBuffer: false }) });
  const info = host.getInfo();
  canvas.width = info.width; canvas.height = info.height;
  const step = 1000 / 60;
  host._writeTime = ((orig) => function () { orig.call(this, k * step, step, k); })(host._writeTime);
  const pads = [{ connected: true, buttons: 0 }, { connected: false }, { connected: false }, { connected: false }];
  for (k = 0; k < frames; k++) {
    await host.runFrame(pads);
    await new Promise((r) => setTimeout(r, 0));
  }
  const gl = host.getGlContext();
  const e = gl.getExtension('WEBGL_debug_renderer_info');
  const state = {};
  const ex = host.instance.exports;
  const dv = new DataView(host.memory ? host.memory.buffer : ex.memory.buffer);
  const str = (q) => { let s = ''; for (let b; (b = dv.getUint8(q)); q++) s += String.fromCharCode(b); return s; };
  for (let p = ex.wc_debug_state(); ; p += 16) {
    const name = dv.getUint32(p, true); if (!name) break;
    const vp = dv.getUint32(p + 4, true), type = dv.getUint8(p + 8), len = dv.getUint32(p + 12, true);
    const rd = (q) => (type === 6 ? dv.getFloat32(q, true) : type === 5 ? dv.getInt32(q, true) : type === 4 ? dv.getUint32(q, true) : null);
    state[str(name)] = len === 1 ? rd(vp) : Array.from({ length: len }, (_, i) => rd(vp + i * 4));
  }
  return { state, lost, renderer: e ? gl.getParameter(e.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER) };
};
window.ready = true;
</script></body></html>`;
  const server = createServer((req, res) => {
    const url = decodeURIComponent(req.url.split('?')[0]);
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    if (url === '/') { res.setHeader('Content-Type', 'text/html'); res.end(page); return; }
    const file = url === '/cart.wasc' ? cart : url.startsWith('/wc/') ? join(WC, url.slice(4)) : null;
    if (!file || !existsSync(file)) { res.statusCode = 404; res.end(); return; }
    res.setHeader('Content-Type', MIME[extname(file)] || 'application/octet-stream');
    res.end(readFileSync(file));
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const { chromium } = await import(join(WC, 'node_modules', 'playwright', 'index.mjs'));
  const { browser, verify } = await launchChromium(chromium, OUT);
  try {
    const pg = await browser.newPage({ viewport: { width: 1960, height: 1120 } });
    await pg.goto(`http://127.0.0.1:${server.address().port}/`);
    await pg.waitForFunction('window.ready === true');
    await verify(pg);
    const r = await pg.evaluate((f) => window.run(f), FRAMES);
    if (r.lost >= 0) return { error: `WebGL context lost at frame ${r.lost}` };
    return { state: r.state, segs: fromState(r.state), passes: passesOf(r.state), gpu: r.renderer };
  } catch (e) {
    return { error: String(e.message).split('\n')[0] };
  } finally {
    await browser.close();
    server.close();
  }
}

async function runWcnative() {
  let exe;
  try { exe = wcnativePlayer(); } catch (e) { return { error: e.message }; } // tools/lib/wcnative.mjs
  const ppm = join(mkdtempSync(join(ROOT, 'out', 'wcn-')), 'shot.ppm');
  const child = spawn(exe, [cart, '--fixed-step', String(1000 / 60), '--shot', String(FRAMES - 1), ppm, '--uncapped'],
    { env: { ...process.env, SDL_AUDIODRIVER: 'dummy' }, stdio: ['ignore', 'ignore', 'pipe'] });
  let err = '';
  child.stderr.on('data', (d) => (err += d));
  const deadline = Date.now() + 300000;
  while (!existsSync(ppm) && child.exitCode === null && Date.now() < deadline) await new Promise((r) => setTimeout(r, 100));
  await new Promise((r) => setTimeout(r, 300));
  child.kill('SIGTERM');
  const segs = parseLines(err);
  if (!Object.keys(segs).length) {
    const why = err.split('\n').filter((l) => /failed|error/i.test(l)).slice(0, 2).join(' | ') || err.trim().split('\n').slice(-2).join(' | ');
    return { error: `no perf lines (wasmcart-run needs a display for its EGL window): ${why}` };
  }
  // wasmcart-run stubs the timer queries: no per-pass GPU times there
  for (const k in segs) delete segs[k].gpu_ms;
  return { segs, passes: Object.fromEntries(Object.entries(segs[SEGS - 1] || {}).filter(([k]) => !/^(seg|frame_ms|max_ms|cpu_ms|gpu_ms|lamps)$/.test(k))),
    gpu: 'wasmcart-native (EGL default device)', lines: segs };
}

const runners = { native: runNative, romdev: runRomdev, chromium: runChromium, wcnative: runWcnative };
const results = {};
for (const h of HOSTS) results[h] = await runners[h]();

// ── report ──
const pad = (s, n) => String(s).padEnd(n);
const num = (v, d = 2) => (typeof v === 'number' && v >= 0 ? v.toFixed(d) : '-');
const hs = HOSTS.filter((h) => !results[h].error);
const lines = [`showcase_perf 1920x1080 full quality, ${SEGS} segments x ${SEG} frames (averages after 30 settling frames), budget ${BUDGET.toFixed(1)} ms`,
  'frame ms avg/worst = render end to render end with a GPU sync each frame; fps from the average; c = CPU ms before the sync; g = GPU ms (timer queries)'];
for (const h of HOSTS) if (results[h].gpu) lines.push(`  ${h}: ${results[h].gpu}`);
lines.push(pad('segment', 10) + hs.map((h) => pad(h, 40)).join(''));
for (let k = 0; k < SEGS; k++)
  lines.push(pad(SEG_NAMES[k], 10) + hs.map((h) => {
    const s = results[h].segs[k] || {};
    const fps = s.frame_ms > 0 ? (1000 / s.frame_ms).toFixed(0) : '-';
    return pad(`${num(s.frame_ms)}/${num(s.max_ms)} ${fps}fps c${num(s.cpu_ms)} g${num(s.gpu_ms)}${s.outside_ms ? ` o${num(s.outside_ms)}` : ''}`, 40);
  }).join(''));
const passNames = [...new Set(hs.flatMap((h) => Object.keys(results[h].passes || {})))];
lines.push('GPU ms per pass (last frame\'s 16-frame average, the running cycle):');
lines.push(pad('pass', 20) + hs.map((h) => pad(h, 12)).join(''));
for (const p of passNames) lines.push(pad(p, 20) + hs.map((h) => pad(num((results[h].passes || {})[p], 3), 12)).join(''));
for (const h of HOSTS) if (results[h].error) lines.push(`${h}: did not run: ${results[h].error}`);
if (results.romdev && !results.romdev.error) lines.push('romdev: o = wall ms per frame stepped through the tool route, measured outside the cart');
console.log(lines.join('\n'));
writeFileSync(join(OUT, 'showcase-report.txt'), lines.join('\n') + '\n');

// ── checks ──
let fail = 0;
const budgetFails = (r, budget) => {
  const out = [];
  for (let k = 0; k < SEGS; k++) {
    const s = r.segs[k];
    if (!s || !(s.frame_ms > 0)) out.push(`${SEG_NAMES[k]}: no frame time`);
    else if (s.frame_ms > budget) out.push(`${SEG_NAMES[k]}: ${s.frame_ms.toFixed(2)} ms > ${budget.toFixed(2)}`);
  }
  return out;
};
for (const h of HOSTS) {
  const r = results[h];
  if (r.error) { fail++; console.log(`FAIL ${h}: ${r.error}`); continue; }
  const bf = budgetFails(r, BUDGET);
  if (bf.length) { fail++; console.log(`FAIL ${h}: over budget: ${bf.join('; ')}`); }
  const lamps = r.state ? r.state['sc.lamps'] : (r.segs[3] || {}).lamps;
  if (lamps !== 128) { fail++; console.log(`FAIL ${h}: night has ${lamps} lamps on, expected 128`); }
}
// control: an impossible budget must fail on the reference host
const ref = results[hs[0]];
if (ref && !budgetFails(ref, 0.05).length) { fail++; console.log('FAIL control: an impossible 0.05 ms budget passed: the budget check cannot fail'); }
console.log(fail ? `showcase-perf: ${fail} failures` : `ok   showcase-perf (every segment under ${BUDGET.toFixed(1)} ms on ${hs.join(', ')}; control fails as it must)`);
process.exit(fail ? 1 : 0);
