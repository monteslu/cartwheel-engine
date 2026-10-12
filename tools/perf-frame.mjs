// Per-frame cost of a game, measured honestly on each host: CPU time per
// frame split into its phases, GPU time where the host can time it, and the
// frame-to-frame time of a real loop that never waits for the GPU.
//
//   node tools/perf-frame.mjs [--apps platformer3d_auto,action2d_auto] [--hosts native,chromium]
//        [--gpus gl,webgpu] [--window 600] [--end N] [--runs 1]
//        [--gpu-timers] [--profile] [--count] [--trace] [--build build] [--json out/perf-frame/report.json]
//
// Every run plays the game's own autopilot (<game>_auto) on the fixed 1/60 s
// clock, one simulation tick per rendered frame, and measures the last
// --window frames before --end (default: the game's gate length, so the
// window is gameplay). The same frames, the same scene, on every host.
//
// What is measured (all ms per frame, averaged over the window):
//   wall      frame start to the next frame start. Native: the engine's loop
//             (CW_FRAMES: flat out, swap with no vsync). Chromium: the
//             exported web player's real requestAnimationFrame loop, run
//             with vsync and the frame-rate limit off, nothing waiting for the
//             GPU: the wall time is the throughput of the slowest stage
//             (renderer CPU, GPU process, or GPU).
//   Native WebGPU runs skip Dawn's validation (CW_WGPU_VALIDATION=0), as a
//   player's run does; test runs keep it.
//   input/sim/update/render/present/audio
//             the engine's own phase timers (debug command "perf",
//             src/debug/perf.c). In a cart, render includes every wasm->JS
//             import it makes (the GL / WebGPU glue and the browser's API
//             entry points).
//   frame_cpu Chromium: the main thread's time in the player's host.runFrame
//             (wc_render and every import it calls, the host's own work).
//   gpu       native, with --gpu-timers: the pipeline's per-pass GPU timers
//             (GL timer queries, WebGPU timestamps; CW_GPU_TIMERS=1), summed.
//             Off by default: the timers' polling costs the CPU every frame,
//             and a player's run has none. Chromium WebGPU: timestamp writes
//             the page adds to every pass (--enable-webgpu-developer-features
//             so they are not quantized). Chromium GL: none (carts and test
//             pages never issue GL timer queries in Chromium,
//             src/render/pipeline_gl.c says why); see --trace.
//   --profile (Chromium) the V8 sampling profiler over the window: each
//             sample's leaf is wasm (split three.c / engine+game / libc),
//             JS glue (webgl_imports.js, the WebGPU glue, CartHostWeb.js),
//             a browser API entry (gl.* / GPU* methods: the binding and the
//             command encoding), GC, or other; and its phase (the wasm
//             function under cw__frame: sim, render, ...). Also counts calls
//             per import (a separate instrumented pass).
//   --trace   (Chromium) a Chrome trace over the window: busy ms per frame of
//             the renderer main thread and the GPU process threads (where
//             ANGLE and Dawn decode and drive the GPU).
//
// Holds the GPU suite lock and runs inside the GPU sandbox like every GPU
// test; Chromium runs on the machine's test GPU (tools/lib/chromium.mjs).
import { spawn, spawnSync } from 'node:child_process';
import { createServer } from 'node:http';
import { readFileSync, writeFileSync, mkdirSync, existsSync, rmSync, cpSync, statSync } from 'node:fs';
import { join, resolve, dirname, extname, normalize, posix } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from './lib/gpu-sandbox.mjs';
import { takeSuiteLock } from './lib/suite-lock.mjs';
import { wgpuPin, glPin, scrubDisplay } from './lib/wgpu-pin.mjs';
import { launchChromium } from './lib/chromium.mjs';
import { killTree } from './lib/gpu-guard.mjs';
import { tempDir } from './lib/tmp.mjs';
import { splitCart, wcFiles } from './cwexport/web.mjs';
import { workersOverride } from './lib/workers.mjs';

await enterGpuSandbox();
for (const sig of ['SIGINT', 'SIGTERM', 'SIGHUP'])
  process.once(sig, () => { killTree(); process.exit(128 + ({ SIGHUP: 1, SIGINT: 2, SIGTERM: 15 })[sig]); });

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'perf-frame');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 && argv[i + 1] && !argv[i + 1].startsWith('--') ? argv[i + 1] : d; };
const flag = (k) => argv.includes('--' + k);
const GAMES = ['platformer3d_auto', 'action2d_auto'];
const APPS = opt('apps', GAMES.join(',')).split(',').map((a) => (a.endsWith('_auto') || !GAMES.includes(a + '_auto') ? a : a + '_auto'));
const HOSTS = opt('hosts', 'native,chromium').split(',');
const GPUS = opt('gpus', 'gl,webgpu').split(',');
const WINDOW = +opt('window', 600);
const RUNS = +opt('runs', 1);
// --profile / --count / --trace each add their own Chromium pass after the clean one (they cost time,
// so the clean pass's numbers never carry them)
const MODES = ['clean', ...['count', 'profile', 'trace'].filter(flag)];
const VERBOSE = flag('verbose');
// --builds a,b: the build trees to measure (default build; an A/B tree from scripts/build-ab.sh),
// interleaved per app and backend so machine load hits both alike
const BUILDS = opt('builds', opt('build', 'build')).split(',').map((b) => resolve(ROOT, b));

// the gate's length per app: the window ends there (gameplay, not menus)
const gates = (() => {
  const g = JSON.parse(readFileSync(join(ROOT, 'tests/gates.json'), 'utf8'));
  const arr = Array.isArray(g) ? g : g.gates || Object.values(g);
  return Object.fromEntries(arr.filter((x) => x.app && x.frames).map((x) => [x.app, x.frames]));
})();
const endOf = (app) => +opt('end', 0) || Math.max(gates[app] || 0, WINDOW + 120);

await takeSuiteLock('perf-frame');

const log = (...a) => console.error(...a);
const r2 = (x) => (x == null || Number.isNaN(x) ? null : Math.round(x * 100) / 100);

// ── native ─────────────────────────────────────────────────────────────
function nativeExe(app, BUILD) {
  for (const d of ['games', 'samples', `samples/${app}`]) {
    const f = join(BUILD, 'native', d, app);
    if (existsSync(f) && statSync(f).isFile()) return f;
  }
  throw new Error(`no native build of ${app} (scripts/build.sh)`);
}

function runNative(app, gpu, BUILD) {
  const exe = nativeExe(app, BUILD), end = endOf(app);
  const env = { ...process.env, CW_FRAMES: String(end), SDL_AUDIODRIVER: 'dummy',
    CW_DEBUG_CMDS: `perf ${WINDOW};pipeline`, CW_WGPU_VALIDATION: '0' };
  if (flag('gpu-timers')) env.CW_GPU_TIMERS = '1';
  const pin = gpu === 'webgpu' ? wgpuPin() : null, gl = pin ? null : glPin();
  if (pin) Object.assign(env, pin.env, { CW_GPU: 'webgpu' });
  else { env.CW_GPU = 'gl'; if (gl) Object.assign(env, gl.env); }
  scrubDisplay(env);
  env.CW_ASSETS = existsSync(exe + '.assets') ? exe + '.assets' : '';
  if (!env.CW_ASSETS) delete env.CW_ASSETS;
  const t0 = Date.now();
  const r = spawnSync('timeout', ['-s', 'KILL', '600', exe], { env, encoding: 'utf8', maxBuffer: 1 << 26 });
  if (r.status !== 0) throw new Error(`native ${app} ${gpu} exited ${r.status}: ${(r.stderr || '').slice(-2000)}`);
  if (pin) pin.check('native', (/WebGPU adapter: (.*)/.exec(r.stderr) || [])[1]);
  else if (gl) gl.check('native', r.stderr);
  const lines = r.stdout.split('\n').filter((l) => l.startsWith('{'));
  const perf = JSON.parse(lines.find((l) => l.includes('"frames"')));
  const pipe = lines.find((l) => l.includes('"passes"'));
  const pipeline = pipe ? JSON.parse(pipe) : null;
  return { host: 'native', gpu, app, end, mode: 'clean', build: BUILD.split('/').pop(), secs: (Date.now() - t0) / 1000, perf, pipeline,
    gpu_ms: pipeline && pipeline.gpu_ms > 0 ? pipeline.gpu_ms : null };
}

// ── chromium: the exported web player ──────────────────────────────────
// The site cwexport's web exporter writes (tools/cwexport/web.mjs), assembled
// from the build's cart: index.html + player.js + wasmcart's web host. For
// WebGPU the WebGPU tier's host and the dual cart (build/cart-wgpu), drawing
// straight into the page's canvas (wgpuCanvas). Two lines of player.js change,
// both for measuring: the host is reachable (window.__cwHost) and its clock is
// pinned to one fixed tick per rendered frame (window.__cwPinClock).
async function buildSite(app, gpu, BUILD) {
  const site = tempDir(OUT, `site-${gpu}-${app}-`); // removed when the tool exits
  mkdirSync(join(site, 'wasmcart'), { recursive: true });
  const T = join(ROOT, 'tools/cwexport/templates/web');
  const WC = join(ROOT, gpu === 'webgpu' ? 'deps/wasmcart-wgpu' : 'deps/wasmcart');
  const fill = (t) => t.replaceAll('@TITLE@', app).replaceAll('@VERSION@', 'perf').replaceAll('@COI_SHIM@', 'off');
  writeFileSync(join(site, 'index.html'), fill(readFileSync(join(T, 'index.html'), 'utf8')));
  let player = fill(readFileSync(join(T, 'player.js'), 'utf8'));
  const hostLine = 'const host = new CartHostWeb();';
  if (!player.includes(hostLine)) throw new Error('player.js changed: no host line to hook');
  player = player.replace(hostLine, `${hostLine}
window.__cwHost = host;
{ const P = window.__perf, rf = host.runFrame;
  host.runFrame = function (...a) { // a GL / WebGPU cart's frame is synchronous: wc_render and everything it calls
    if (!P || !P.armed) return rf.apply(this, a);
    P.rafStarts.push(performance.now());
    try { return rf.apply(this, a); } finally { P.rafEnds.push(performance.now()); } }; }
if (window.__cwPinClock) { let k = 0; const step = 1000 / 60;
  host._writeTime = ((orig) => function () { orig.call(this, k * step, step, k); k++; })(host._writeTime); }`);
  if (gpu === 'webgpu') {
    // the player hands the dual cart its canvas for WebGPU (wgpuCanvas); measure on the discrete GPU
    const load = 'await host.load(bytes, { wasm: CODE, wgpuCanvas: canvas,';
    if (!player.includes(load)) throw new Error('player.js changed: no host.load line to hook');
    player = player.replace(load, "await host.load(bytes, { wasm: CODE, wgpuCanvas: canvas, adapterOptions: { powerPreference: 'high-performance' },");
  }
  // the warm-up runs in the first frame, as in a native test run (CW_FRAMES), so frame N is the same
  // game moment on every host (spreading it shows a loading bar; per-frame cost is unchanged)
  const touch = "if (coarse) window.cwDebug.command('touch_primary 1')";
  if (!player.includes(touch)) throw new Error('player.js changed: no touch_primary line');
  player = player.replace(touch, "window.cwDebug.command('warmup_spread 0').catch(() => {});\n" + touch);
  writeFileSync(join(site, 'player.js'), player);
  // what the web export ships of wasmcart (the files CartHostWeb reaches), its fflate import made relative
  for (const f of wcFiles(WC)) {
    const fflate = posix.relative(posix.dirname(f), 'fflate.js').replace(/^(?!\.)/, './');
    mkdirSync(dirname(join(site, 'wasmcart', f)), { recursive: true });
    writeFileSync(join(site, 'wasmcart', f), readFileSync(join(WC, 'src', f), 'utf8').replace(/(from\s+|import\()(['"])fflate\2/g, `$1$2${fflate}$2`));
  }
  cpSync(join(WC, 'node_modules/fflate/esm/browser.js'), join(site, 'wasmcart/fflate.js'));
  const cart = join(BUILD, gpu === 'webgpu' ? 'cart-wgpu/carts' : 'cart/carts', `${app}.wasc`);
  if (!existsSync(cart)) throw new Error(`no cart ${cart} (scripts/build.sh)`);
  await splitCart(cart, join(site, 'game.wasm'), join(site, 'game.wasc')); // as the web export ships it
  return site;
}

function serve(site) {
  const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasc': 'application/octet-stream', '.wasm': 'application/wasm', '.png': 'image/png' };
  const server = createServer((req, res) => {
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    res.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
    let p = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    if (p.endsWith('/')) p += 'index.html';
    const f = normalize(join(site, p));
    if (!f.startsWith(site) || !existsSync(f)) { res.statusCode = 404; res.end(); return; }
    res.setHeader('Content-Type', TYPES[extname(f)] || 'application/octet-stream');
    res.setHeader('Content-Length', statSync(f).size);
    res.end(readFileSync(f));
  });
  return new Promise((r) => server.listen(0, '127.0.0.1', () => r(server)));
}

// Page instrumentation (an init script): the frame loop's timing, and for
// WebGPU, timestamp writes on every pass.
const INIT = `(() => {
  window.__cwPinClock = true;
  const P = window.__perf = { rafStarts: [], rafEnds: [], gpu: { total: 0, frames: 0, passes: {}, spans: 0, groups: 0 }, armed: false, counts: null };
  if (!window.__cwGpuTime || !self.GPUAdapter) return;
  const rd = GPUAdapter.prototype.requestDevice;
  GPUAdapter.prototype.requestDevice = async function (desc = {}) {
    const feats = [...(desc.requiredFeatures || [])];
    if (this.features.has('timestamp-query') && !feats.includes('timestamp-query')) feats.push('timestamp-query');
    const dev = await rd.call(this, { ...desc, requiredFeatures: feats });
    if (dev.features.has('timestamp-query')) install(dev);
    else P.gpu.unavailable = true;
    return dev;
  };
  function install(dev) {
    const MAXQ = 1024;
    const qs = dev.createQuerySet({ type: 'timestamp', count: MAXQ });
    const res = dev.createBuffer({ size: MAXQ * 8, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC });
    const free = [];
    let n = 0, labels = [];
    const add = (orig, kind) => function (desc = {}) {
      if (!desc.timestampWrites && n + 2 <= MAXQ && this.__dev === dev) {
        desc = { ...desc, timestampWrites: { querySet: qs, beginningOfPassWriteIndex: n, endOfPassWriteIndex: n + 1 } };
        labels.push(desc.label || kind); n += 2;
      }
      return orig.call(this, desc);
    };
    const ce = dev.createCommandEncoder;
    dev.createCommandEncoder = function (d) { const e = ce.call(this, d); e.__dev = dev; return e; };
    const E = GPUCommandEncoder.prototype;
    E.beginRenderPass = add(E.beginRenderPass, 'render');
    E.beginComputePass = add(E.beginComputePass, 'compute');
    const sub = dev.queue.submit;
    dev.queue.submit = function (cbs) {
      if (!n) return sub.call(this, cbs);
      const count = n, names = labels;
      n = 0; labels = [];
      const rb = free.pop() || dev.createBuffer({ size: MAXQ * 8, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
      const enc = ce.call(dev);
      enc.resolveQuerySet(qs, 0, count, res, 0);
      enc.copyBufferToBuffer(res, 0, rb, 0, count * 8);
      sub.call(this, [...cbs, enc.finish()]);
      rb.mapAsync(GPUMapMode.READ).then(() => {
        const t = new BigInt64Array(rb.getMappedRange(0, count * 8));
        if (P.armed) {
          let lo = null, hi = null;
          for (let i = 0; i < count; i += 2) {
            const a = t[i], b = t[i + 1];
            if (b <= a || a === 0n) continue;
            const ms = Number(b - a) / 1e6;
            P.gpu.total += ms;
            P.gpu.passes[names[i / 2]] = (P.gpu.passes[names[i / 2]] || 0) + ms;
            if (lo === null || a < lo) lo = a;
            if (hi === null || b > hi) hi = b;
          }
          if (lo !== null) { P.gpu.spans += Number(hi - lo) / 1e6; P.gpu.groups++; }
        }
        rb.unmap(); free.push(rb);
      }).catch(() => {});
    };
  }
})();`;

// call counting per import (a separate pass: wrapping every import costs time)
const COUNT_INIT = `(() => {
  const inst = WebAssembly.instantiate, Inst = WebAssembly.Instance;
  const C = window.__importCounts = {};
  window.__countArmed = false;
  const wrap = (imports) => {
    if (!imports || typeof imports !== 'object') return imports;
    const out = {};
    for (const [mod, fns] of Object.entries(imports)) {
      out[mod] = {};
      for (const [name, f] of Object.entries(fns || {})) {
        if (typeof f !== 'function') { out[mod][name] = f; continue; }
        const key = mod + '.' + name;
        out[mod][name] = function (...a) { if (window.__countArmed) C[key] = (C[key] || 0) + 1; return f.apply(this, a); };
      }
    }
    return out;
  };
  WebAssembly.instantiate = function (src, imports) { return inst.call(this, src, wrap(imports)); };
  WebAssembly.Instance = function (m, imports) { return new Inst(m, wrap(imports)); };
  WebAssembly.Instance.prototype = Inst.prototype;
})();`;

function classifyProfile(profile) {
  const byId = new Map(profile.nodes.map((n) => [n.id, n]));
  const parent = new Map();
  for (const n of profile.nodes) for (const c of n.children || []) parent.set(c, n.id);
  const isWasm = (cf) => cf.url.startsWith('wasm://') || /^\$?wasm-function/.test(cf.functionName);
  const GLUE = /webgl_imports\.js|\/wgpu\/|CartHostWeb\.js|abi\.js/;
  const leafKind = (cf) => {
    const fn = cf.functionName;
    if (fn === '(garbage collector)') return 'gc';
    if (fn === '(idle)') return 'idle';
    if (fn === '(program)') return 'program';
    if (fn === '(root)') return 'other';
    if (isWasm(cf)) return /^(t3_|t3__|three_|_t3)/.test(fn) || /three/.test(fn) ? 'wasm_three' : /^(cw_|cw__)/.test(fn) ? 'wasm_engine' : 'wasm_other';
    if (!cf.url) return 'api'; // a builtin: the browser's API entry (gl.drawElements, GPUQueue.writeBuffer, ...)
    if (GLUE.test(cf.url)) return 'glue';
    if (/player\.js/.test(cf.url)) return 'player';
    return 'js_other';
  };
  const phaseOf = (id) => {
    // the wasm function called by cw__frame names the phase
    let prev = null;
    for (let cur = id; cur != null; cur = parent.get(cur)) {
      const cf = byId.get(cur).callFrame;
      if (cf.functionName === 'cw__frame') return prev ? byId.get(prev).callFrame.functionName : 'cw__frame';
      if (cf.functionName === 'wc_render') return prev ? 'wc_render>' + byId.get(prev).callFrame.functionName : 'wc_render';
      prev = cur;
    }
    return 'outside';
  };
  const kinds = {}, phases = {}, fnSelf = {}, apiSelf = {};
  const dt = profile.timeDeltas;
  for (let i = 0; i < profile.samples.length; i++) {
    const ms = (dt[i + 1] ?? dt[i] ?? 0) / 1000;
    const node = byId.get(profile.samples[i]);
    const k = leafKind(node.callFrame);
    kinds[k] = (kinds[k] || 0) + ms;
    if (k === 'idle' || k === 'program') continue;
    const ph = phaseOf(node.id);
    phases[ph] ??= {};
    phases[ph][k] = (phases[ph][k] || 0) + ms;
    const name = (k.startsWith('wasm') ? '' : k + ':') + node.callFrame.functionName;
    fnSelf[name] = (fnSelf[name] || 0) + ms;
    if (k === 'api' || k === 'glue') apiSelf[node.callFrame.functionName] = (apiSelf[node.callFrame.functionName] || 0) + ms;
  }
  return { kinds, phases, fnSelf, apiSelf };
}

function traceBusy(events, frames) {
  // thread names, then the top-level task durations per thread
  const names = new Map(), procs = new Map();
  for (const e of events) {
    if (e.ph === 'M' && e.name === 'thread_name') names.set(e.pid + ':' + e.tid, e.args.name);
    if (e.ph === 'M' && e.name === 'process_name') procs.set(e.pid, e.args.name);
  }
  const busy = {};
  for (const e of events) {
    if (e.ph !== 'X' || !e.dur) continue;
    if (!/toplevel/.test(e.cat || '')) continue;
    if (!/RunTask|ThreadControllerImpl|ThreadPool_RunTask|TaskQueueManager/.test(e.name)) continue;
    const key = `${procs.get(e.pid) || e.pid}/${names.get(e.pid + ':' + e.tid) || e.tid}`;
    busy[key] = (busy[key] || 0) + e.dur / 1000;
  }
  const out = {};
  for (const [k, v] of Object.entries(busy)) if (v / frames > 0.05) out[k] = r2(v / frames);
  return out;
}

async function runChromium(app, gpu, BUILD, mode) {
  const PROFILE = mode === 'profile', TRACE = mode === 'trace', COUNT = mode === 'count';
  const end = endOf(app), start = end - WINDOW;
  const site = await buildSite(app, gpu, BUILD);
  const server = await serve(site);
  const WC = join(ROOT, 'deps/wasmcart');
  const { chromium } = await import(join(WC, 'node_modules', 'playwright', 'index.mjs'));
  const extra = ['--disable-gpu-vsync', '--disable-frame-rate-limit', '--autoplay-policy=no-user-gesture-required'];
  if (gpu === 'webgpu') extra.push('--enable-unsafe-webgpu', '--enable-features=Vulkan', '--enable-webgpu-developer-features');
  const { browser, verify } = await launchChromium(chromium, OUT, extra);
  const result = { host: 'chromium', gpu, app, end, mode, build: BUILD.split('/').pop() };
  try {
    const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
    const logs = [];
    page.on('console', (m) => logs.push(m.text()));
    page.on('pageerror', (e) => logs.push('pageerror: ' + e.message));
    if (gpu === 'webgpu') await page.addInitScript('window.__cwGpuTime = true;');
    await page.addInitScript(INIT);
    await workersOverride(page);
    if (COUNT) await page.addInitScript(COUNT_INIT);
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.waitForFunction(() => window.cwStatus && (window.cwStatus.ok === false || window.cwStatus.frames > 0), null, { timeout: 120000 });
    const st = await page.evaluate(() => window.cwStatus);
    if (!st.ok) throw new Error(`player failed: ${st.error}\n${logs.join('\n')}`);
    result.renderer = await verify(page);
    if (gpu === 'webgpu') {
      const ad = await page.evaluate(() => window.__cwHost.getGpuAdapterInfo && window.__cwHost.getGpuAdapterInfo());
      if (!ad) throw new Error('no WebGPU adapter info');
      result.adapter = ad;
      const usesWgpu = await page.evaluate(() => window.__cwHost.usesWgpu);
      if (!usesWgpu) throw new Error('the cart did not run on WebGPU');
    }
    await page.keyboard.press('Shift'); // a gesture: the player starts its audio (the autopilot ignores devices)
    const at = (f) => page.waitForFunction((n) => window.__cwHost.frameCount >= n, f, { timeout: 600000, polling: 50 });
    await at(start);
    let cdp = null;
    if (PROFILE) {
      cdp = await page.context().newCDPSession(page);
      await cdp.send('Profiler.enable');
      await cdp.send('Profiler.setSamplingInterval', { interval: 100 });
    }
    if (TRACE) await browser.startTracing(page, { categories: ['toplevel'] }); // (more categories: hundreds of MB a run)
    if (cdp) await cdp.send('Profiler.start');
    await page.evaluate(() => window.cwDebug.command('perf reset'));
    const f0 = await page.evaluate(() => { window.__perf.armed = true; window.__countArmed = true; return window.__cwHost.frameCount; });
    await at(end);
    const f1 = await page.evaluate(() => { window.__perf.armed = false; window.__countArmed = false; return window.__cwHost.frameCount; });
    const perfReply = await page.evaluate(() => window.cwDebug.command('perf'));
    let profile = null, trace = null;
    if (cdp) profile = (await cdp.send('Profiler.stop')).profile;
    if (TRACE) trace = JSON.parse((await browser.stopTracing()).toString());
    // let the last timestamp readbacks land
    await page.waitForTimeout(300);
    const frames = f1 - f0;
    const P = await page.evaluate(() => ({ s: window.__perf.rafStarts, e: window.__perf.rafEnds, gpu: window.__perf.gpu, counts: window.__importCounts || null, st: window.cwStatus }));
    const intervals = P.s.slice(1).map((t, i) => t - P.s[i]);
    const cpu = P.e.map((t, i) => t - P.s[i]);
    const avg = (a) => a.reduce((x, y) => x + y, 0) / Math.max(1, a.length);
    const pct = (a, q) => { const s = [...a].sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(s.length * q))]; };
    result.frames = frames;
    result.wall = { avg: r2(avg(intervals)), p50: r2(pct(intervals, 0.5)), p95: r2(pct(intervals, 0.95)), max: r2(Math.max(...intervals)) };
    result.frame_cpu = { avg: r2(avg(cpu)), p50: r2(pct(cpu, 0.5)), p95: r2(pct(cpu, 0.95)) };
    result.perf = perfReply;
    if (gpu === 'webgpu') {
      const g = P.gpu;
      result.gpu_ms = g.unavailable ? null : r2(g.total / frames);
      result.gpu_span_ms = g.groups ? r2(g.spans / frames) : null;
      result.gpu_passes = Object.fromEntries(Object.entries(g.passes).sort((a, b) => b[1] - a[1]).slice(0, 20).map(([k, v]) => [k, r2(v / frames)]));
    }
    result.audio = { underruns: P.st.audioUnderruns ?? null, overruns: P.st.audioOverruns ?? null };
    if (P.counts) {
      const tot = Object.values(P.counts).reduce((a, b) => a + b, 0);
      result.import_calls_per_frame = Math.round(tot / frames);
      result.import_calls = Object.fromEntries(Object.entries(P.counts).sort((a, b) => b[1] - a[1]).slice(0, 30).map(([k, v]) => [k, Math.round((v / frames) * 10) / 10]));
    }
    if (profile) {
      const c = classifyProfile(profile);
      const per = (o) => Object.fromEntries(Object.entries(o).sort((a, b) => b[1] - a[1]).map(([k, v]) => [k, r2(v / frames)]));
      result.profile = { kinds: per(c.kinds), phases: Object.fromEntries(Object.entries(c.phases).map(([k, v]) => [k, per(v)])),
        top: Object.fromEntries(Object.entries(per(c.fnSelf)).slice(0, 40)), api: Object.fromEntries(Object.entries(per(c.apiSelf)).slice(0, 30)) };
      writeFileSync(join(OUT, `${app}-${gpu}-${result.build}.cpuprofile`), JSON.stringify(profile));
    }
    if (trace) {
      result.trace_busy = traceBusy(trace.traceEvents || trace, frames);
    }
    if (VERBOSE) log(logs.join('\n'));
  } finally {
    await browser.close().catch(() => {});
    server.close();
  }
  return result;
}

// ── report ─────────────────────────────────────────────────────────────
function row(r) {
  const p = r.perf || {};
  const a = (k) => (p[k] ? r2(p[k].avg) : null);
  const wall = r.host === 'native' ? a('frame') : r.wall && r.wall.avg;
  return { app: r.app.replace(/_auto$/, ''), host: r.host, gpu: r.gpu, build: r.build, wall, p95: r.host === 'native' ? (p.frame ? r2(p.frame.p95) : null) : r.wall && r.wall.p95,
    frame_cpu: r.frame_cpu ? r.frame_cpu.avg : null, input: a('input'), sim: a('sim'), update: a('update'), render: a('render'),
    present: a('present'), audio: a('audio'), gpu_ms: r.gpu_ms != null ? r2(r.gpu_ms) : null, draws: p.draw_calls ?? null, tris: p.triangles ?? null };
}

const results = [];
for (const app of APPS)
  for (const gpu of GPUS)
    for (const host of HOSTS)
      for (const mode of host === 'native' ? ['clean'] : MODES)
        for (let k = 0; k < RUNS; k++)
          for (const BUILD of BUILDS) {
            if (mode !== 'clean' && BUILD !== BUILDS[0]) continue; // the extra passes look at the first build
            const b = BUILD.split('/').pop();
            log(`${app} ${host} ${gpu} ${b}${mode !== 'clean' ? ' ' + mode : ''}${RUNS > 1 ? ' run ' + (k + 1) : ''}...`);
            try {
              const r = host === 'native' ? runNative(app, gpu, BUILD) : await runChromium(app, gpu, BUILD, mode);
              results.push(r);
              log('  ' + JSON.stringify(mode === 'clean' ? row(r) : { count: r.import_calls_per_frame, top: r.import_calls, profile: r.profile && r.profile.kinds, phases: r.profile && r.profile.phases, trace: r.trace_busy }));
            } catch (e) {
              log(`  FAILED: ${e.message.split('\n').slice(0, 6).join('\n')}`);
              results.push({ app, host, gpu, build: b, mode, error: e.message });
            }
          }

const rows = results.filter((r) => !r.error && r.mode === 'clean').map(row);
const cols = ['app', 'host', 'gpu', 'build', 'wall', 'p95', 'frame_cpu', 'input', 'sim', 'update', 'render', 'present', 'audio', 'gpu_ms', 'draws', 'tris'];
console.log(cols.join('\t'));
for (const r of rows) console.log(cols.map((c) => r[c] ?? '-').join('\t'));
const load = readFileSync('/proc/loadavg', 'utf8').split(' ').slice(0, 3).join(' ');
const jsonOut = opt('json', join(OUT, 'report.json'));
writeFileSync(jsonOut, JSON.stringify({ when: new Date().toISOString(), load, window: WINDOW, rows, results }, null, 1));
log(`load ${load}; full results: ${jsonOut}`);
process.exit(results.some((r) => r.error) ? 1 : 0);
