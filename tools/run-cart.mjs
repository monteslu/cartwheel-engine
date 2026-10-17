// Run one cartwheel app on one host for N frames on the fixed 1/60 s clock,
// with an optional input script (pads 0-3, keys, pointer), and capture the last frame and the
// named debug state. The same request on every host is how cartwheel proves
// native and cart builds agree.
//
//   node tools/run-cart.mjs --host native|romdev|chromium|wcnative --app hello
//        --frames 120 [--input script.json] [--shot out.png] [--state out.json]
//
// Frame N means "after N engine frames": the app's frame counter reads N.
//   native    build/native/samples/<app>, CW_FRAMES / CW_SCREENSHOT / CW_DEBUG_DUMP / CW_INPUT
//   romdev    the MCP server's HTTP tool route (127.0.0.1:7331), deterministic load
//             (its load runs frame 1, so N-1 steps follow); one session per run
//   chromium  wasmcart's CartHostWeb in headless Chromium (Playwright), served
//             cross-origin isolated (threads need SharedArrayBuffer); the host's
//             clock is pinned to frame*1000/60. CW_CHROMIUM_GPU=<PCI address>
//             runs it on that GPU alone (tools/lib/chromium.mjs),
//             CW_CHROMIUM_ARGS adds flags; it never falls back to SwiftShader.
//             --gl-audit [file] checks every draw for out-of-range GPU reads
//             and query misuse (tools/lib/gl-audit.js); --gl-audit-errors
//             also reads glGetError after every call (slow); --init-script
//             <file> runs a page script before the cart (window.__cwDump is
//             written to out/init-script-dump.json)
//   wcnative  wasmcart-native's wasmcart-run --fixed-step --shot (no input, no state); XAUTHORITY
//             is filled in from /run/user/<uid> when the shell has none (it would wait on the display).
//             CW_WORKERS gives its cart the job pool a native run gets (tools/lib/gpu-sandbox.mjs)
//
// --warmup-spread: the app's warm-up spread over frames under the loading screen, as a player
// gets it (native, chromium, wcnative; test runs otherwise do it all in frame 1).
//
// --log FILE: the run's log (native: the app's stderr; wcnative: the player's), what a cart
// that reports no state (wcnative) can still say. CW_RUN_TIMEOUT_MS bounds wcnative runs too.
//
// --allow-skipped: a run where three.c skipped draws (debug field render.skipped) does not fail
// (every run fails on one otherwise: a test that skips on purpose says so).
//
// --gpu gl|webgpu (default gl): the graphics backend (cartwheel/gpu.h). webgpu runs the native
// build with CW_GPU=webgpu and the DUAL carts of build/cart-wgpu (gl + WebGPU imports), and holds
// every host to the test GPU (tools/lib/wgpu-pin.mjs): a run that lands on another adapter fails
// before it draws (romdev, chromium) or as soon as the adapter is known (native, wcnative). It also
// fails on any WebGPU error the engine counted (debug field cw.gpu.errors).
//
// Input script (JSON): [{ "frame": 30, "buttons": ["south"], "axes": { "lx": 1 } }, ...]
// Each entry sets the WHOLE input state from that frame on. Frame numbers are
// 1-based engine frames (>= 1; romdev's load already ran frame 0). Optional:
//   "keys":    ["space", "d", 79]       keys held (SDL scancodes / names below)
//   "mouse":   { "x": 320, "y": 180, "buttons": ["left", "right"] }   pointer slot 0
//   "touches": [{ "id": 1, "x": 100, "y": 280 }]   fingers, wasmcart pointer slots 1-9
//              (pointer positions are the app's LOGICAL pixels, cw_window_size; a cart's
//              host gets them scaled to its framebuffer, cw.view, cw_app_desc.render_scale)
//   "wheel":   1  or  { "x": 0, "y": -2 }   notches, +y up, THIS frame only
//   "pad2":    { "buttons": ["south"], "axes": { "lx": 1 } }   pad 1 (a second player; absent: not connected)
//   "pad3", "pad4": the same for pads 2 and 3 (the third and fourth players)
//   "text":    "hello"   text typed THIS frame (CW_EVENT_TEXT; the cart must have
//              text input started, as a focused text field does)
// Absent keys/mouse/touches mean none held. Per host: native writes them into
// the CW_INPUT file; chromium calls CartHostWeb keyDown/keyUp/setPointer/wheel;
// romdev uses input({op:'pointer'}) and input({op:'wheel'}) but has no
// wasmcart keyboard op, so a script with keys fails there (gates skip romdev).
import { spawn, spawnSync } from 'node:child_process';
import { createServer } from 'node:http';
import { readFileSync, writeFileSync, mkdtempSync, existsSync, realpathSync, statSync, copyFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname, extname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, writePng } from './lib/png.mjs';
import { launchChromium } from './lib/chromium.mjs';
import { wgpuPin, glPin, scrubDisplay } from './lib/wgpu-pin.mjs';
import { tempDir } from './lib/tmp.mjs';
import { enterGpuSandbox, showCartWorkers } from './lib/gpu-sandbox.mjs';
import { workersOverride } from './lib/workers.mjs';
import { killTree } from './lib/gpu-guard.mjs';
import { wcnativePlayer, wcnativeWgpuPlayer } from './lib/wcnative.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
// a killed run-cart stops its player too (a wasmcart-run waiting on --shot ran on uncapped as an orphan)
for (const sig of ['SIGINT', 'SIGTERM', 'SIGHUP'])
  process.once(sig, () => { killTree(); process.exit(128 + ({ SIGHUP: 1, SIGINT: 2, SIGTERM: 15 })[sig]); });

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
// CW_PROJECT_ROOT: run the apps of another tree built with this engine (a games
// repository: its build/ and out/ sit where this checkout's do); default this checkout
const PROJ = process.env.CW_PROJECT_ROOT ? resolve(process.env.CW_PROJECT_ROOT) : ROOT;
const args = Object.fromEntries(process.argv.slice(2).reduce((acc, a, i, all) => {
  if (a.startsWith('--')) acc.push([a.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
  return acc;
}, []));
const host = args.host, app = args.app, frames = +args.frames || 60;
// --start-cmds "a;b": debug-channel requests posted at load, request i answered at the
// start of frame i + 2 on every host (a cart host runs frame 1 on load): how a test
// says what a real host would (touch_primary 1 for a phone). A request written
// "@N cmd" is answered at the start of frame N instead (N >= 2; native, chromium,
// wcnative): a mid-run change such as a graphics preset
const startCmds = typeof args['start-cmds'] === 'string' ? args['start-cmds'].split(';').filter(Boolean) : [];
// [k, text]: request posted before loop step k (0-based; answered at the start of frame k + 1)
const startPlan = startCmds.map((c, i) => {
  const m = /^@(\d+)\s*(.*)$/.exec(c);
  if (m && +m[1] < 2) throw new Error(`--start-cmds: "${c}": a request runs at frame 2 or later`);
  return m ? [+m[1] - 1, m[2]] : [i + 1, c];
});
const timedCmds = startCmds.some((c) => c.startsWith('@'));
// --frame-hz: the virtual render rate (default 60); fixed ticks stay at the app's rate
const frameHz = +args['frame-hz'] || 60;
// --warmup-spread: the warm-up spread over frames under the loading screen, as a player gets it
// (test runs otherwise do it all in frame 1); the frames it takes vary with the machine's speed,
// so the state's ticks say where the game is
const spread = !!args['warmup-spread'];
const gpu = args.gpu === 'webgpu' || args.gpu === 'wgpu' ? 'webgpu' : 'gl';
if (args.gpu && args.gpu !== true && !['gl', 'webgpu', 'wgpu'].includes(args.gpu)) { console.error('--gpu gl|webgpu'); process.exit(2); }
const pin = gpu === 'webgpu' ? wgpuPin() : null;
if (spread && host === 'romdev') { console.error('--warmup-spread: romdev loads deterministically (no spread)'); process.exit(2); }
if (!host || !app) { console.error('usage: run-cart.mjs --host <native|romdev|chromium|wcnative> --app <name> --frames N'); process.exit(2); }
// (a project built in the tree, samples/<app>/ with its own CMakeLists.txt, has its executable inside that directory)
const isFile = (p) => existsSync(p) && statSync(p).isFile();
// (an app of a CW_PROJECT_DIR build sits under build/native/project/: its apps/<app>.json names it)
const appBinary = () => { try { return JSON.parse(readFileSync(join(PROJ, 'build/native/apps', `${app}.json`), 'utf8')).binary; } catch { return null; } };
const nativeExe = args.exe ? resolve(args.exe) : ['samples', 'tests/unit', 'games', 'editor', `samples/${app}`].map((d) => join(PROJ, 'build/native', d, app)).find(isFile) ||
  appBinary() || join(PROJ, 'build/native/samples', app);
// --dual-gl (romdev, --gpu gl): the DUAL cart of build/cart-wgpu, told to render with GL (loadMedia wgpu:false)
const dualGl = !!args['dual-gl'];
if (dualGl && (host !== 'romdev' || gpu !== 'gl')) { console.error('--dual-gl is a romdev --gpu gl run'); process.exit(2); }
const cart = args.cart ? resolve(args.cart) : join(PROJ, gpu === 'webgpu' || dualGl ? 'build/cart-wgpu/carts' : 'build/cart/carts', `${app}.wasc`);
const script = args.input ? JSON.parse(readFileSync(args.input, 'utf8')) : [];
const OUT = join(PROJ, 'out');

// cw_pad_button bits (native) and wasmcart WC_BTN bits (cart hosts)
const CW_BITS = { south: 0, east: 1, west: 2, north: 3, back: 4, guide: 5, start: 6, l3: 7, r3: 8, l: 9, r: 10, up: 11, down: 12, left: 13, right: 14 };
const WC_BITS = { south: 0, east: 1, west: 2, north: 3, l: 4, r: 5, start: 6, back: 7, up: 8, down: 9, left: 10, right: 11, l3: 12, r3: 13, guide: 14 };
const mask = (names, bits) => (names || []).reduce((m, n) => {
  if (!(n in bits)) throw new Error(`unknown button ${n}`);
  return m | (1 << bits[n]);
}, 0);
const AX = ['lx', 'ly', 'rx', 'ry', 'lt', 'rt'];
for (const s of script) if (!(s.frame >= 1)) throw new Error('input frames start at 1');

// SDL scancodes (USB HID usage ids), the codes wasmcart's keyboard ABI uses
const KEYS = { return: 40, enter: 40, escape: 41, backspace: 42, tab: 43, space: 44, minus: 45, equals: 46,
  comma: 54, period: 55, slash: 56, right: 79, left: 80, down: 81, up: 82, lctrl: 224, lshift: 225, lalt: 226,
  rctrl: 228, rshift: 229, ralt: 230 };
for (let i = 0; i < 26; i++) KEYS[String.fromCharCode(97 + i)] = 4 + i;
for (let i = 1; i <= 9; i++) KEYS[String(i)] = 29 + i;
KEYS['0'] = 39;
for (let i = 1; i <= 12; i++) KEYS['f' + i] = 57 + i;
const MOUSE_BITS = { left: 0, right: 1, middle: 2 };
const scancodes = (keys) => (keys || []).map((k) => {
  const c = typeof k === 'number' ? k : KEYS[k];
  if (!(c >= 0 && c < 256)) throw new Error(`unknown key ${k} (names: ${Object.keys(KEYS).join(' ')}, or a scancode < 256)`);
  return c;
});
const mouseMask = (m) => (m && m.buttons ? mask(m.buttons, MOUSE_BITS) : 0);
const wheelOf = (w) => (w == null ? null : typeof w === 'number' ? { x: 0, y: w } : { x: w.x || 0, y: w.y || 0 });
for (const s of script) {
  scancodes(s.keys);
  for (const t of s.touches || []) if (!(t.id >= 1 && t.id <= 9)) throw new Error('touch ids are 1..9 (pointer slot 0 is the mouse)');
  for (const p of [s.mouse, ...(s.touches || [])].filter(Boolean))
    if (!Number.isInteger(p.x) || !Number.isInteger(p.y)) throw new Error('pointer x/y are integer logical pixels');
}

function result(obj) { console.log(JSON.stringify({ ...obj, gpu, ...(adapterSeen ? { adapter: adapterSeen } : {}) })); }
let adapterSeen = null;

// a run's own report: draws three.c could not make (render.skipped: a state the
// program table lacks, a program refused as incomplete, an extension with no
// hook) fail it on every host and backend, since every host skipping the same
// draws agrees with itself (--allow-skipped: a test that skips on purpose);
// for WebGPU, the backend it used and the errors it counted
function checkWgpuState(where, state) {
  if (state && state['render.skipped'] && !args['allow-skipped'])
    throw new Error(`${where}: three.c skipped ${state['render.skipped']} draw(s) (see the log: render3d names the reason)`);
  if (!pin || !state) return;
  if (state['cw.gpu.api'] !== undefined && state['cw.gpu.api'] !== 2) throw new Error(`${where}: ran on ${state['cw.gpu.api'] === 1 ? 'GL' : 'no GPU API'}, not WebGPU`);
  if (state['cw.gpu.errors']) throw new Error(`${where}: ${state['cw.gpu.errors']} WebGPU error(s) (see the log)`);
}

async function runNative() {
  // SDL's dummy audio driver: the same SDL device + callback path, without
  // depending on the desktop sound server (it intermittently refused to open,
  // flaking audio gates). CW_REAL_AUDIO=1 keeps the system device.
  const env = { ...process.env, CW_FRAMES: String(frames), CW_FRAME_HZ: String(frameHz), CW_WARMUP_SPREAD: spread ? '1' : '0',
    ...(process.env.CW_REAL_AUDIO ? {} : { SDL_AUDIODRIVER: process.env.SDL_AUDIODRIVER || 'dummy' }) };
  if (pin) Object.assign(env, pin.env, { CW_GPU: 'webgpu' });
  else env.CW_GPU = 'gl';
  // Test runs never open a window on the desktop (hundreds of short-lived X11
  // windows per suite went through the user's Xwayland, which aborted once
  // mid-suite, 2026-10-06): SDL's offscreen driver (GL: EGL surfaceless;
  // WebGPU: no surface at all), and no DISPLAY / WAYLAND_DISPLAY / XAUTHORITY
  // so an attempt to reach the desktop fails loudly. CW_TEST_WINDOW=1 keeps a
  // real window.
  const gl = pin ? null : glPin();
  if (gl) Object.assign(env, gl.env);
  if (!process.env.CW_TEST_WINDOW) scrubDisplay(env);
  else delete env.SDL_VIDEODRIVER;
  if (args.shot) env.CW_SCREENSHOT = resolve(args.shot);
  if (args.state) env.CW_DEBUG_DUMP = resolve(args.state);
  if (startCmds.length) env.CW_START_CMDS = startCmds.join(';');
  if (script.length) {
    const f = join(tempDir(OUT, 'input-'), 'input.txt');
    // the platform applies an entry at the START of that engine frame (index frame-1)
    const line = (s) => {
      const parts = [s.frame - 1, mask(s.buttons, CW_BITS), ...AX.map((a) => (s.axes || {})[a] || 0)];
      const keys = scancodes(s.keys);
      if (keys.length) parts.push('k=' + keys.join(','));
      if (s.mouse) parts.push(`m=${s.mouse.x},${s.mouse.y},${mouseMask(s.mouse)}`);
      const w = wheelOf(s.wheel);
      if (w) parts.push(`w=${w.x},${w.y}`);
      for (const t of s.touches || []) parts.push(`t${t.id}=${t.x},${t.y}`);
      if (s.text) parts.push('x=' + Buffer.from(s.text, 'utf8').toString('hex'));
      for (const [k, f] of [['pad2', 'p'], ['pad3', 'p2'], ['pad4', 'p3']])
        if (s[k]) parts.push(`${f}=${mask(s[k].buttons, CW_BITS)},${(s[k].axes || {}).lx || 0},${(s[k].axes || {}).ly || 0}`);
      return parts.join(' ');
    };
    writeFileSync(f, script.map(line).join('\n') + '\n');
    env.CW_INPUT = f;
  }
  // the build links each app's asset dir beside it (<exe>.assets); samples/<app>/assets otherwise
  env.CW_ASSETS = args.assets || (existsSync(nativeExe + '.assets') ? nativeExe + '.assets' : join(ROOT, 'samples', app, 'assets'));
  // CW_RUN_TIMEOUT_MS: how long one native run may take (default 2 minutes; software rendering in CI is slower)
  const timeout = +(process.env.CW_RUN_TIMEOUT_MS || 120000);
  const r = spawnSync(nativeExe, [], { env, encoding: 'utf8', timeout });
  // a run stopped by the timeout may still exit 0 (the engine quits cleanly on SIGTERM): that is a failure
  if (r.error) throw new Error(`native: ${r.error.code === 'ETIMEDOUT' ? `timed out after ${timeout} ms (CW_RUN_TIMEOUT_MS)` : r.error.message}: ${r.stderr || ''}`);
  if (r.status !== 0) throw new Error(`native exited ${r.status ?? r.signal}: ${r.stderr}`);
  if (args.log) writeFileSync(resolve(args.log), r.stderr || '');
  if (gl) gl.check('native', r.stderr);
  if (pin) {
    const m = /WebGPU adapter: (.*)/.exec(r.stderr || '');
    adapterSeen = pin.check('native', m && m[1]);
  }
  if (args.state) checkWgpuState('native', JSON.parse(readFileSync(resolve(args.state), 'utf8')));
  result({ host, frames, shot: args.shot, state: args.state });
}

async function romdevCall(tool, a, session) {
  const res = await fetch(`http://127.0.0.1:7331/tool/${tool}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'x-romdev-session': session },
    body: JSON.stringify(a),
  });
  const text = await res.text();
  let body;
  try { body = JSON.parse(text); } catch { body = text; }
  if (!res.ok || (body && body.isError)) throw new Error(`romdev ${tool}: ${typeof body === 'string' ? body : JSON.stringify(body).slice(0, 2000)}`);
  // tool route returns either the raw result or an MCP-style {content:[{text}]}
  if (body && Array.isArray(body.content)) {
    const t = body.content.find((c) => c.type === 'text');
    try { return JSON.parse(t.text); } catch { return t ? t.text : body; }
  }
  return body;
}

async function runRomdev() {
  const session = `cartwheel-run-${app}-${process.pid}`;
  const loaded = await romdevCall('loadMedia', { platform: 'wasmcart', path: cart, deterministicSeed: +(args.seed || 1), deterministicStepMs: 1000 / frameHz,
    ...(pin ? { gpuPower: pin.power } : {}), ...(dualGl ? { wgpu: false } : {}) }, session);
  if (dualGl && loaded && loaded.gpu !== 'gl') {
    await romdevCall('host', { op: 'shutdown' }, session).catch(() => {});
    throw new Error(`romdev: the dual cart runs on ${loaded && loaded.gpu}, not GL (loadMedia wgpu:false needs romdev 65237621+)`);
  }
  if (pin) { /* before any frame beyond the load's: the adapter, then the API */
    const ga = loaded && loaded.gpuAdapter;
    try {
      adapterSeen = pin.check('romdev', ga && (ga.device || ga.description));
      if (loaded.gpu !== 'webgpu') throw new Error(`romdev: the cart runs on ${loaded.gpu}, not WebGPU`);
    } catch (e) {
      await romdevCall('host', { op: 'shutdown' }, session).catch(() => {});
      throw e;
    }
  }
  let at = 1; // engine frames already run
  // input scripts are in the cart's LOGICAL pixels (as natively); the host's
  // pointer is in framebuffer pixels (cw_app_desc.render_scale): cw.view = [lw, lh, picture w, h, x, y]
  let sx = 1, sy = 1, ox = 0, oy = 0; // cw.view [4], [5]: the picture's origin (pixel_scale bars)
  if (script.some((s) => s.mouse || (s.touches || []).length)) {
    const st = await romdevCall('wasm', { op: 'debugState' }, session);
    const v = (st.fields || []).find((f) => f.name === 'cw.view');
    const vv = v && (v.values ?? v.value);
    if (Array.isArray(vv) && vv[0] > 0 && vv[1] > 0) { sx = vv[2] / vv[0]; sy = vv[3] / vv[1]; ox = vv[4] || 0; oy = vv[5] || 0; }
  }
  const fbx = (x) => Math.round(ox + x * sx), fby = (y) => Math.round(oy + y * sy);
  if (timedCmds) throw new Error('romdev: --start-cmds "@N cmd" (a request at a frame) is not supported here; skip romdev for such gates');
  for (const text of startCmds) { /* answered at the start of the frame the request runs */
    await romdevCall('wasm', { op: 'command', text }, session);
    at++;
  }
  const romdevPad = (s) => {
    const p = {};
    for (const b of s.buttons || []) p[b === 'back' ? 'select' : b === 'l' ? 'l' : b] = true;
    if (s.axes) p.axes = s.axes;
    const ports = [p];
    for (const k of ['pad2', 'pad3', 'pad4']) {
      if (!s[k]) {
        // romdev's ports are all connected: a pad unplugged before a later one cannot be said
        if (['pad2', 'pad3', 'pad4'].slice(ports.length).some((j) => s[j]))
          throw new Error(`romdev: frame ${s.frame} unplugs ${k} while a later pad stays (its input ports are all connected); skip romdev for such gates`);
        break;
      }
      const q = {};
      for (const b of s[k].buttons || []) q[b === 'back' ? 'select' : b] = true;
      if (s[k].axes) q.axes = s[k].axes;
      ports.push(q);
    }
    return ports;
  };
  if (script.some((s) => s.text))
    throw new Error('romdev: text input cannot be injected (no wasmcart text op); skip romdev for text gates');
  if (script.some((s) => scancodes(s.keys).length))
    throw new Error('romdev: keyboard input cannot be injected (its input tool has no wasmcart key op); skip romdev for keyboard gates');
  let mouse = null, touches = new Map();
  for (const s of script) {
    if (s.frame > frames) break; // entries after the last frame never apply (as natively)
    // the entry applies from engine frame s.frame (1-based) on: frames 1..s.frame-1 run before it
    const before = s.frame - 1;
    if (before > at) {
      await romdevCall('frame', { op: 'step', frames: before - at }, session); at = before;
    }
    // pointer and wheel ops reset the held pad in romdev's host: send them first, the pad last
    if (s.mouse || mouse) {
      const m = s.mouse || mouse, b = mouseMask(s.mouse);
      await romdevCall('input', { op: 'pointer', id: 0, x: fbx(m.x), y: fby(m.y), left: !!(b & 1), right: !!(b & 2), active: !!s.mouse }, session);
      mouse = s.mouse || null;
    }
    const now = new Map((s.touches || []).map((t) => [t.id, t]));
    for (const [id, t] of new Map([...touches, ...now]))
      await romdevCall('input', { op: 'pointer', id, x: fbx(t.x), y: fby(t.y), left: now.has(id), active: now.has(id) }, session);
    touches = now;
    const w = wheelOf(s.wheel);
    if (w && (w.x || w.y)) await romdevCall('input', { op: 'wheel', dx: Math.round(w.x * 120), dy: Math.round(w.y * 120) }, session);
    await romdevCall('input', { op: 'set', ports: romdevPad(s) }, session);
  }
  // One call for the rest of the run. romdev gives a WebGPU cart an event-loop
  // turn between every two stepped frames, so asynchronous results (buffer
  // maps, error scopes) arrive between frames as on every other host; stepping
  // one frame per call for that cost thousands of calls a run
  if (frames > at) await romdevCall('frame', { op: 'step', frames: frames - at }, session);
  if (args.shot) await romdevCall('frame', { op: 'screenshot', path: resolve(args.shot) }, session);
  if (args.state) {
    const st = await romdevCall('wasm', { op: 'debugState' }, session);
    const o = {};
    for (const f of st.fields || []) o[f.name] = f.values ?? f.value;
    writeFileSync(resolve(args.state), JSON.stringify(o) + '\n');
    checkWgpuState('romdev', o);
  }
  await romdevCall('host', { op: 'shutdown' }, session).catch(() => {});
  result({ host, frames, shot: args.shot, state: args.state });
}

const MIME = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.html': 'text/html', '.wasc': 'application/octet-stream', '.wasm': 'application/wasm' };

async function runChromium() {
  const WC = join(ROOT, gpu === 'webgpu' ? 'deps/wasmcart-wgpu' : 'deps/wasmcart'); // the WebGPU tier's host for dual carts
  const page_html = `<!doctype html><html><body><canvas id=c></canvas>
<script type=importmap>{"imports":{"fflate":"/wc/node_modules/fflate/esm/browser.js"}}</script>
<script type=module>
import { CartHostWeb } from '/wc/src/CartHostWeb.js';
window.run = async (cfg) => {
  const host = new CartHostWeb();
  const canvas = document.getElementById('c');
  await host.load('/cart.wasc', { glBackend: () => canvas.getContext('webgl2', { antialias: false, preserveDrawingBuffer: true }),
    ...(cfg.gpu === 'webgpu' ? { adapterOptions: { powerPreference: cfg.power } } : { wgpu: false }) });
  const adapter = host.getGpuAdapterInfo ? host.getGpuAdapterInfo() : null;
  if (cfg.gpu === 'webgpu' && !host.usesWgpu) throw new Error('the cart did not run on WebGPU (usesWgpu false)');
  // a browser may withhold the device name (privacy): then the bubblewrap pin and the DRM
  // devices the browser holds (verified by the runner after the run) say which GPU it was
  const named = adapter ? String(adapter.device || adapter.description || '').toLowerCase() : '';
  if (cfg.gpu === 'webgpu' && (!adapter || /swiftshader|cpu|llvmpipe|lavapipe/.test(JSON.stringify(adapter).toLowerCase()) || (named && !named.includes(cfg.match))))
    throw new Error('WebGPU adapter ' + JSON.stringify(adapter) + ' is not the test GPU (' + cfg.match + '): refusing');
  let gframe = null;
  const info = host.getInfo();
  canvas.width = info.width; canvas.height = info.height;
  // pin the clock: frame k (0-based) runs at time k*step
  const step = 1000 / cfg.frameHz;
  let k = 0;
  host._writeTime = ((orig) => function () { orig.call(this, k * step, step, k); })(host._writeTime);
  // pad 0 present and neutral from the start, as romdev and native test runs report it
  const pads = [{ connected: true, buttons: 0 }, { connected: false }, { connected: false }, { connected: false }];
  let si = 0;
  let keys = new Set(), mouse = null, touches = new Map();
  // --start-cmds: request i goes into the cart's dbg.cmd mailbox before frame i + 2
  // input scripts are in the cart's LOGICAL pixels (as natively); the host's
  // pointer is in framebuffer pixels (cw_app_desc.render_scale): cw.view = [lw, lh, picture w, h, x, y]
  let sx = 1, sy = 1, ox = 0, oy = 0;
  {
    const ex = host.instance.exports;
    if (ex.wc_debug_state) {
      const dv = new DataView((host.memory || ex.memory).buffer);
      const str = (q) => { let s = ''; for (let b; (b = dv.getUint8(q)); q++) s += String.fromCharCode(b); return s; };
      for (let p = ex.wc_debug_state(); ; p += 16) {
        const name = dv.getUint32(p, true);
        if (!name) break;
        if (str(name) !== 'cw.view') continue;
        const n = dv.getUint32(p + 12, true), vp = dv.getUint32(p + 4, true);
        const v = [0, 1, 2, 3, 4, 5].map((i) => (i < n ? dv.getInt32(vp + 4 * i, true) : 0));
        if (v[0] > 0 && v[1] > 0) { sx = v[2] / v[0]; sy = v[3] / v[1]; ox = v[4]; oy = v[5]; }
      }
    }
  }
  const fbx = (x) => Math.round(ox + x * sx), fby = (y) => Math.round(oy + y * sy);
  const postCmd = (text) => {
    const ex = host.instance.exports;
    if (!ex.wc_debug_state) throw new Error('--start-cmds: the cart has no debug channel');
    const dv = new DataView((host.memory || ex.memory).buffer);
    const str = (q) => { let s = ''; for (let b; (b = dv.getUint8(q)); q++) s += String.fromCharCode(b); return s; };
    const f = {};
    for (let p = ex.wc_debug_state(); ; p += 16) {
      const name = dv.getUint32(p, true);
      if (!name) break;
      f[str(name)] = { ptr: dv.getUint32(p + 4, true) };
    }
    const bytes = new TextEncoder().encode(text);
    for (let i = 0; i < bytes.length; i++) dv.setUint8(f['dbg.cmd'].ptr + i, bytes[i]);
    dv.setUint8(f['dbg.cmd'].ptr + bytes.length, 0);
    dv.setUint32(f['dbg.cmd_seq'].ptr, dv.getUint32(f['dbg.cmd_seq'].ptr, true) + 1, true);
  };
  for (k = 0; k < cfg.frames; k++) {
    // a cart spreads its warm-up over frames (a loading screen) unless told otherwise:
    // a test does it all in frame 1, as native and romdev test runs do
    if (k === 0 && !cfg.spread && host.instance.exports.wc_debug_state) postCmd('warmup_spread 0');
    for (const [at, text] of cfg.startPlan) if (at === k) postCmd(text);
    while (si < cfg.script.length && cfg.script[si].frame - 1 <= k) {
      const s = cfg.script[si++];
      const nk = new Set(s.scancodes);
      for (const c of keys) if (!nk.has(c)) host.keyUp(c);
      for (const c of nk) if (!keys.has(c)) host.keyDown(c);
      keys = nk;
      if (s.mouse) host.setPointer(0, fbx(s.mouse.x), fby(s.mouse.y), s.mouseMask, true);
      else if (mouse) host.setPointer(0, fbx(mouse.x), fby(mouse.y), 0, false);
      mouse = s.mouse || null;
      const nt = new Map((s.touches || []).map((t) => [t.id, t]));
      for (const [id, t] of touches) if (!nt.has(id)) host.setPointer(id, fbx(t.x), fby(t.y), 0, false);
      for (const [id, t] of nt) host.setPointer(id, fbx(t.x), fby(t.y), 1, true);
      touches = nt;
      if (s.wheelXY) host.wheel(Math.round(s.wheelXY.x * 120), Math.round(s.wheelXY.y * 120));
      if (s.text) host.textInput(s.text);
      const ax = s.axes || {};
      const st = (v) => Math.round(Math.max(-1, Math.min(1, v || 0)) * 32767);
      ['pad2', 'pad3', 'pad4'].forEach((k, i) => {
        const a2 = (s[k] && s[k].axes) || {};
        pads[i + 1] = s[k] ? { connected: true, buttons: s.masks[i], leftX: st(a2.lx), leftY: st(a2.ly), rightX: 0, rightY: 0, leftTrigger: 0, rightTrigger: 0 }
                           : { connected: false };
      });
      pads[0] = { connected: true, buttons: s.mask, leftX: st(ax.lx), leftY: st(ax.ly), rightX: st(ax.rx), rightY: st(ax.ry),
        leftTrigger: Math.round((ax.lt || 0) * 32767), rightTrigger: Math.round((ax.rt || 0) * 32767) };
    }
    await host.runFrame(pads);
    /* a WebGPU frame is read in the task that drew it */
    if (cfg.gpu === 'webgpu' && k === cfg.frames - 1) gframe = await host.readGpuFrame();
    // yield to the event loop between frames, as a page driven by rAF does:
    // wasi-thread workers start, and receive messages, only when it runs
    await new Promise((r) => setTimeout(r, 0));
  }
  let w, h, px;
  if (gframe) {
    w = gframe.width;
    h = gframe.height;
    px = new Uint8Array(gframe.data.buffer || gframe.data);
  } else {
    const gl = host.getGlContext();
    w = gl.drawingBufferWidth;
    h = gl.drawingBufferHeight;
    px = new Uint8Array(w * h * 4);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
  }
  let state = null;
  const ex = host.instance.exports;
  if (ex.wc_debug_state) {
    state = {};
    const mem = host.memory ? host.memory.buffer : ex.memory.buffer;
    const dv = new DataView(mem);
    let p = ex.wc_debug_state();
    const str = (q) => { let s = ''; for (let b; (b = dv.getUint8(q)); q++) s += String.fromCharCode(b); return s; };
    for (;; p += 16) {
      const name = dv.getUint32(p, true); if (!name) break;
      const vp = dv.getUint32(p + 4, true), type = dv.getUint8(p + 8), len = dv.getUint32(p + 12, true);
      const rd = [(q) => dv.getUint8(q), (q) => dv.getInt8(q), (q) => dv.getUint16(q, true), (q) => dv.getInt16(q, true),
        (q) => dv.getUint32(q, true), (q) => dv.getInt32(q, true), (q) => dv.getFloat32(q, true), (q) => dv.getFloat64(q, true)][type];
      const size = [1, 1, 2, 2, 4, 4, 4, 8][type];
      if (type === 8) { let hex = ''; for (let i = 0; i < len; i++) hex += dv.getUint8(vp + i).toString(16).padStart(2, '0'); state[str(name)] = hex; }
      else state[str(name)] = len === 1 ? rd(vp) : Array.from({ length: len }, (_, i) => rd(vp + i * size));
    }
  }
  // the pixels as base64: a 1920x1080 frame as a JSON number array took seconds to cross
  let b = '';
  for (let i = 0; i < px.length; i += 0x8000) b += String.fromCharCode.apply(null, px.subarray(i, i + 0x8000));
  return { w, h, px64: btoa(b), state, crossOriginIsolated, topDown: !!gframe, adapter };
};
window.ready = true;
</script></body></html>`;
  const server = createServer((req, res) => {
    const url = decodeURIComponent(req.url.split('?')[0]);
    let file = null, body = null;
    if (url === '/' ) body = page_html;
    else if (url === '/cart.wasc') file = cart;
    else if (url.startsWith('/wc/')) file = join(WC, url.slice(4));
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    if (body) { res.setHeader('Content-Type', 'text/html'); res.end(body); return; }
    if (!file || !existsSync(file)) { res.statusCode = 404; res.end(); return; }
    res.setHeader('Content-Type', MIME[extname(file)] || 'application/octet-stream');
    res.end(readFileSync(file));
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const { chromium } = await import(join(WC, 'node_modules', 'playwright', 'index.mjs'));
  // CW_CHROMIUM_GPU pins the browser to one GPU (tools/lib/chromium.mjs)
  const { browser, verify } = await launchChromium(chromium, OUT, gpu === 'webgpu' ? ['--enable-unsafe-webgpu', '--enable-features=Vulkan'] : []);
  try {
    const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
    const logs = [];
    page.on('console', (m) => logs.push(m.text()));
    page.on('pageerror', (e) => logs.push('pageerror: ' + e.message));
    // --init-script <file>: a page script run before the cart (its window.__cwDump lands in out/init-script-dump.json)
    if (typeof args['init-script'] === 'string') await page.addInitScript({ path: resolve(args['init-script']) });
    await workersOverride(page);
    // --gl-audit [file]: check every draw for out-of-range GPU reads and query misuse (tools/lib/gl-audit.js)
    if (args['gl-audit-errors']) await page.addInitScript(() => { window.__glAuditErrors = true; });
    if (args['gl-audit'] || args['gl-audit-errors']) await page.addInitScript({ path: join(ROOT, 'tools/lib/gl-audit.js') });
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.waitForFunction('window.ready === true');
    const pinned = await verify(page);
    if (pinned && args.verbose) console.error(`chromium pinned to ${process.env.CW_CHROMIUM_GPU || process.env.CW_CHROMIUM_RENDER_NODE}: ${pinned}`);
    const cfg = { gpu, spread, startPlan, match: pin ? pin.match : '', power: pin ? pin.power : '', frames, frameHz, script: script.map((s) => ({ ...s, mask: mask(s.buttons, WC_BITS), masks: ['pad2', 'pad3', 'pad4'].map((k) => (s[k] ? mask(s[k].buttons, WC_BITS) : 0)), scancodes: scancodes(s.keys),
      mouseMask: mouseMask(s.mouse), wheelXY: wheelOf(s.wheel) })) };
    const r = await page.evaluate((c) => window.run(c), cfg).catch((e) => { throw new Error(`${e.message}\n${logs.join('\n')}`); });
    if (!r.crossOriginIsolated) throw new Error('page is not cross-origin isolated: threads cannot run');
    if (pin) {
      const named = r.adapter && (r.adapter.device || r.adapter.description);
      if (named) adapterSeen = pin.check('chromium', named);
      else {
        // name withheld: the browser must be pinned (bubblewrap) and, now that WebGPU has
        // opened its device, hold DRM fds of the test GPU only
        if (!pinned) throw new Error('chromium: WebGPU adapter name withheld and the browser is not GPU-pinned: refusing');
        await verify(page);
        adapterSeen = `${r.adapter.vendor} ${r.adapter.architecture} (name withheld; DRM pinned to ${pin.pci})`;
      }
    }
    if (args.shot) writePng(resolve(args.shot), r.w, r.h, new Uint8Array(Buffer.from(r.px64, 'base64')), { flipY: !r.topDown });
    if (args.state) writeFileSync(resolve(args.state), JSON.stringify(r.state) + '\n');
    checkWgpuState('chromium', r.state);
    if (args.verbose) console.error(logs.join('\n'));
    if (args['init-script']) {
      const dump = await page.evaluate(() => window.__cwDump || null);
      if (dump) writeFileSync(join(OUT, 'init-script-dump.json'), JSON.stringify(dump));
    }
    if (args['gl-audit'] || args['gl-audit-errors']) {
      const audit = await page.evaluate(() => window.__glAudit);
      if (typeof args['gl-audit'] === 'string') writeFileSync(resolve(args['gl-audit']), JSON.stringify(audit, null, 1) + '\n');
      console.error(`gl-audit: ${audit.draws} draws, ${audit.findings} findings ${JSON.stringify(audit.kinds)}`);
    }
    result({ host, frames, shot: args.shot, state: args.state });
  } finally {
    await browser.close();
    server.close();
  }
}

async function runWcnative() {
  // beside the checkout, or beside the main checkout when this is a git worktree under worktrees/
  // WebGPU: a wasmcart-run built with WebGPU (a wgpu/ directory beside it); CW_WCNATIVE_WGPU names one
  // the main wasmcart-native build, or --wcnative; throws when there is none (tools/lib/wcnative.mjs)
  const player = args.wcnative || (pin ? wcnativeWgpuPlayer() : wcnativePlayer());
  if (!existsSync(player)) throw new Error(`wcnative: ${player} does not exist`);
  if (script.length) throw new Error('wcnative: input scripts are not supported by wasmcart-run');
  const wdir = tempDir(OUT, 'wcn-');
  const ppm = join(wdir, 'shot.ppm');
  // wasmcart-run keeps a cart's save data in <cart>.sav beside the cart and
  // never sets the deterministic flag, so a run would start from the last
  // run's save: run a copy in the fresh directory instead (no save there)
  const runCart = join(wdir, 'cart' + extname(cart));
  copyFileSync(cart, runCart);
  // --shot does not exit on its own: wait for the file, then stop the player.
  // SDL_AUDIODRIVER=dummy and --uncapped keep a desktop audio daemon and a
  // throttled vsync from setting the pace.
  // no window, no desktop: SDL's offscreen driver (Mesa's surfaceless EGL) and no display variables
  const glp = pin ? null : glPin();
  const wenv = { ...process.env, SDL_AUDIODRIVER: 'dummy', ...(pin ? pin.env : { WASMCART_NO_WGPU: '1', ...(glp ? glp.env : {}) }) };
  if (!process.env.CW_TEST_WINDOW) scrubDisplay(wenv);
  else delete wenv.SDL_VIDEODRIVER;
  // warmup_spread 0 before frame 1: the whole warm-up in that frame, as on the other test hosts
  const cmdArgs = [...(spread ? [] : ['--debug-cmd', '0', 'warmup_spread 0']), ...startPlan.flatMap(([k, c]) => ['--debug-cmd', String(k), c])];
  showCartWorkers(); // CW_WORKERS: the cart's job pool as natively (tools/lib/gpu-sandbox.mjs)
  const child = spawn(player, [runCart, '--fixed-step', String(1000 / frameHz), '--shot', String(frames - 1), ppm, '--uncapped', ...cmdArgs],
    { env: wenv, stdio: ['ignore', 'ignore', 'pipe'] });
  let stderr = '';
  child.stderr.on('data', (d) => (stderr += d));
  const deadline = Date.now() + +(process.env.CW_RUN_TIMEOUT_MS || 120000);
  while (!existsSync(ppm) && child.exitCode === null && Date.now() < deadline) await new Promise((r) => setTimeout(r, 100));
  await new Promise((r) => setTimeout(r, 300)); // let the writer finish the file
  child.kill('SIGTERM');
  if (!existsSync(ppm)) throw new Error(`wcnative: no shot: ${stderr.slice(-2000)}`);
  if (pin) {
    const m = /WebGPU on (.*)/.exec(stderr);
    adapterSeen = pin.check('wcnative', m && m[1]);
    if (!/via WebGPU/.test(stderr)) throw new Error('wcnative: the cart did not render via WebGPU');
    if (/\[error\] WebGPU/.test(stderr)) throw new Error(`wcnative: WebGPU errors: ${stderr.split('\n').filter((l) => /\[error\] WebGPU/.test(l)).slice(0, 3).join(' | ')}`);
  }
  else if (glp) glp.check('wcnative', stderr);
  if (args.log) writeFileSync(resolve(args.log), stderr);
  if (args.shot) { const img = readImage(ppm); writePng(resolve(args.shot), img.w, img.h, img.rgba); }
  rmSync(wdir, { recursive: true, force: true });
  result({ host, frames, shot: args.shot });
}

// The native build on ANGLE instead of the system GLES: the GL stack Windows,
// macOS and iOS builds ship. CW_ANGLE_DIR = a directory holding an ANGLE
// libEGL.so + libGLESv2.so that export the GL entry points (a standard ANGLE
// build; Chromium's own copy does not export them). ANGLE_DEFAULT_PLATFORM
// picks its backend (default gl).
async function runAngle() {
  const dir = process.env.CW_ANGLE_DIR;
  if (!dir) throw new Error('angle: set CW_ANGLE_DIR to a directory with ANGLE libEGL.so and libGLESv2.so');
  const links = tempDir(OUT, 'angle-');
  const { symlinkSync } = await import('node:fs');
  for (const [name, target] of [['libGLESv2.so.2', 'libGLESv2.so'], ['libGLESv2.so', 'libGLESv2.so'], ['libEGL.so.1', 'libEGL.so'], ['libEGL.so', 'libEGL.so']])
    symlinkSync(join(dir, target), join(links, name));
  process.env.LD_LIBRARY_PATH = [links, dir, dirname(dir), process.env.LD_LIBRARY_PATH].filter(Boolean).join(':');
  process.env.SDL_VIDEO_EGL_DRIVER = join(links, 'libEGL.so.1');
  process.env.SDL_VIDEO_GL_DRIVER = join(links, 'libGLESv2.so.2');
  process.env.SDL_VIDEO_X11_FORCE_EGL = '1';
  process.env.ANGLE_DEFAULT_PLATFORM ||= 'gl';
  return runNative();
}

const runners = { native: runNative, romdev: runRomdev, chromium: runChromium, wcnative: runWcnative, angle: runAngle };
if (!runners[host]) { console.error(`unknown host ${host}`); process.exit(2); }
runners[host]().catch((e) => { console.error(e.message); process.exit(1); });
