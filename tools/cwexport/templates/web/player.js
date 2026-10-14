// cartwheel web export: runs the game (game.wasm, its code, and game.wasc,
// the rest) on wasmcart's web host (CartHostWeb) with WebGPU where the browser
// has it, else WebGL2; keyboard, gamepads, mouse / touch and Web Audio.
//
// The game uses threads, which need SharedArrayBuffer, which browsers only
// give a cross-origin isolated page: the server must send
//   Cross-Origin-Opener-Policy: same-origin
//   Cross-Origin-Embedder-Policy: require-corp
// (_headers does it on Netlify / Cloudflare Pages; serve.mjs locally; see
// README.txt for other servers). Without them this page says so and stops.
import { CartHostWeb } from './wasmcart/CartHostWeb.js';

const canvas = document.getElementById('screen');
const CART = 'game.wasc';
// the cart's code: fetched at once, beside game.wasc, and compiled as it
// streams in (the browser keeps the compiled code for the next visit)
const CODE = fetch('game.wasm');

// ── the loading panel (index.html #msg): one bar from the request to the
// first playable frame. The download fills it to 60 % (a real percentage when
// the server sends the file's Content-Length, any static host; indeterminate
// otherwise), then the steps after it, each measured: compiling the game,
// instantiating it, its init, its first frame, and a game's own warm-up
// (cartwheel's cw_app_desc.warmup: textures, scene, shaders), which it reports
// in the debug field cw.loading. A step that stalls shows its elapsed time.
// window.cwLoadTimes: when each step started (ms since the page started).
const panel = document.getElementById('msg');
const barEl = document.getElementById('bar'), stageEl = document.getElementById('stage'), amountEl = document.getElementById('amount');
const progressEl = panel.querySelector('[role=progressbar]');
const mb = (n) => (n / 1048576).toFixed(n < 10485760 ? 1 : 0);
const STEPS = { download: [0, 60, 'Loading'], compile: [60, 70, 'Compiling'], instantiate: [70, 74, 'Instantiating'],
  init: [74, 80, 'Starting'], frame: [80, 82, 'Starting'], warmup: [82, 100, 'Preparing'] };
const WARM_LABELS = ['Preparing', 'Loading assets', 'Making textures', 'Building meshes', 'Compiling shaders', 'Building the scene'];
window.cwLoadTimes = {};
let step = null, stepAt = 0, stepLabel = '';
function setBar(pct, label, amount) {
  pct = Math.max(0, Math.min(100, Math.floor(pct)));
  barEl.style.width = pct + '%';
  progressEl.setAttribute('aria-valuenow', String(pct));
  stageEl.textContent = `${label}… ${pct}%`;
  amountEl.textContent = amount || '';
}
function enter(name, frac = 0, label) {
  if (step !== name) {
    step = name;
    stepAt = performance.now();
    window.cwLoadTimes[name] = Math.round(stepAt);
  }
  const [a, b, l] = STEPS[name];
  stepLabel = label || l;
  panel.className = 'working';
  setBar(a + (b - a) * frac, stepLabel);
}
// a step that takes a while says how long it has taken (it runs on, it is not stuck)
setInterval(() => {
  if (!step || step === 'download' || panel.classList.contains('failed')) return;
  const s = (performance.now() - stepAt) / 1000;
  if (s >= 1.5) amountEl.textContent = `${s.toFixed(0)} s`;
}, 250);
function showProgress(loaded, total) {
  if (step !== 'download') enter('download');
  if (total) {
    panel.className = '';
    setBar((loaded / total) * 60, 'Loading', `${mb(loaded)} / ${mb(total)} MB`);
  } else {
    panel.className = 'indeterminate';
    progressEl.removeAttribute('aria-valuenow');
    stageEl.textContent = 'Loading…';
    amountEl.textContent = `${mb(loaded)} MB`;
  }
}
document.getElementById('retry').addEventListener('click', () => location.reload());

function fail(title, html) {
  document.getElementById('msg-title').textContent = title;
  document.getElementById('msg-body').innerHTML = html;
  panel.className = 'failed';
  panel.setAttribute('role', 'alert');
  window.cwStatus = { ok: false, error: title };
  throw new Error(title);
}

/* game.wasc with progress: streamed when the browser can, Content-Length for
 * the percentage unless it is the compressed size (Content-Encoding) */
async function download(url) {
  let res;
  try {
    res = await fetch(url);
  } catch (e) {
    fail('The game could not be downloaded', `<p>${String(e.message || e).replace(/</g, '&lt;')}</p><p>Check the connection and try again.</p>`);
  }
  if (!res.ok) fail('The game could not be downloaded', `<p>The server answered <code>HTTP ${res.status}</code> for <code>${url}</code>.</p>`);
  const encoded = (res.headers.get('content-encoding') || 'identity') !== 'identity';
  const total = encoded ? 0 : +res.headers.get('content-length') || 0;
  if (!res.body || !res.body.getReader) {
    showProgress(0, 0);
    return new Uint8Array(await res.arrayBuffer());
  }
  const reader = res.body.getReader(), chunks = [];
  let loaded = 0;
  showProgress(0, total);
  for (;;) {
    let part;
    try {
      part = await reader.read();
    } catch (e) {
      fail('The download was interrupted', `<p>${String(e.message || e).replace(/</g, '&lt;')}</p><p>Check the connection and try again.</p>`);
    }
    if (part.done) break;
    chunks.push(part.value);
    loaded += part.value.length;
    showProgress(loaded, total && loaded <= total ? total : 0);
  }
  const bytes = new Uint8Array(loaded);
  let at = 0;
  for (const c of chunks) { bytes.set(c, at); at += c.length; }
  return bytes;
}

// coi_shim = true in the export preset ships coi-sw.js: on a host that cannot
// send the isolation headers, a service worker adds them; the page reloads
// once to be served through it. (A host that sends them never gets here.)
const COI_SHIM = '@COI_SHIM@' === 'on';
if (!window.crossOriginIsolated && COI_SHIM && 'serviceWorker' in navigator && location.protocol !== 'file:') {
  let again = false;
  try { again = sessionStorage.getItem('cw-coi-reloaded') === '1'; } catch {}
  if (!again) {
    try {
      await navigator.serviceWorker.register('coi-sw.js');
      await navigator.serviceWorker.ready;
      try { sessionStorage.setItem('cw-coi-reloaded', '1'); } catch {}
      location.reload();
      await new Promise(() => {}); // the reload takes over
    } catch { /* no service worker here: the message below explains */ }
  }
}
try { if (window.crossOriginIsolated) sessionStorage.removeItem('cw-coi-reloaded'); } catch {}
if (!window.crossOriginIsolated) {
  const local = location.protocol === 'file:';
  fail('This page is not cross-origin isolated, so the game cannot start',
    `<p>The game runs on several threads, which needs <code>SharedArrayBuffer</code>. Browsers only allow it on a
     <em>cross-origin isolated</em> page, and the server that sent this one did not isolate it.</p>
     ${local ? '<p>It was opened from a file (<code>file://</code>), which can never be isolated: serve the folder instead, e.g. <code>node serve.mjs</code> in it, then open the address it prints.</p>' : ''}
     <p>The server must send these two headers with every file of the game:</p>
     <p><code>Cross-Origin-Opener-Policy: same-origin</code><br><code>Cross-Origin-Embedder-Policy: require-corp</code></p>
     <p>Netlify and Cloudflare Pages read the <code>_headers</code> file shipped here; README.txt shows nginx, Apache and others.
     Some hosts (GitHub Pages, for one) cannot set headers at all: export with <code>coi_shim = true</code> in the
     preset to ship a service worker that isolates the page there${COI_SHIM ? ' (this build has it, but the browser did not let it isolate the page: private browsing, or service workers turned off)' : ''}.</p>`);
}
// (probed on a throwaway canvas: the first getContext fixes a canvas's attributes for good)
if (!document.createElement('canvas').getContext('webgl2'))
  fail('WebGL 2 is not available', '<p>This game renders with WebGL 2. Turn on hardware acceleration in the browser settings, or try another browser.</p>');

// ── input ──────────────────────────────────────────────────────────────
// KeyboardEvent.code -> USB HID usage (the wasmcart keyboard ABI's scancodes)
const KEYS = { Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44, Minus: 45, Equal: 46, BracketLeft: 47,
  BracketRight: 48, Backslash: 49, Semicolon: 51, Quote: 52, Backquote: 53, Comma: 54, Period: 55, Slash: 56,
  CapsLock: 57, Insert: 73, Home: 74, PageUp: 75, Delete: 76, End: 77, PageDown: 78, ArrowRight: 79, ArrowLeft: 80,
  ArrowDown: 81, ArrowUp: 82, ControlLeft: 224, ShiftLeft: 225, AltLeft: 226, MetaLeft: 227, ControlRight: 228,
  ShiftRight: 229, AltRight: 230, MetaRight: 231 };
for (let i = 0; i < 26; i++) KEYS['Key' + String.fromCharCode(65 + i)] = 4 + i;
for (let i = 1; i <= 9; i++) KEYS['Digit' + i] = 29 + i;
KEYS.Digit0 = 39;
for (let i = 1; i <= 12; i++) KEYS['F' + i] = 57 + i;

// standard-mapping gamepad button index -> wasmcart pad bit
const PAD_BITS = { 0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 9: 6, 8: 7, 12: 8, 13: 9, 14: 10, 15: 11, 10: 12, 11: 13, 16: 14 };
const axis = (v) => Math.round(Math.max(-1, Math.min(1, Math.abs(v) < 0.08 ? 0 : v)) * 32767);
function readPads() {
  const out = [];
  for (const gp of (navigator.getGamepads ? navigator.getGamepads() : [])) {
    if (!gp || !gp.connected || out.length >= 4) continue;
    let buttons = 0;
    gp.buttons.forEach((b, i) => { if (b.pressed && i in PAD_BITS) buttons |= 1 << PAD_BITS[i]; });
    out.push({ connected: true, name: gp.id, buttons, leftX: axis(gp.axes[0] || 0), leftY: axis(gp.axes[1] || 0),
      rightX: axis(gp.axes[2] || 0), rightY: axis(gp.axes[3] || 0),
      leftTrigger: Math.round((gp.buttons[6]?.value || 0) * 32767), rightTrigger: Math.round((gp.buttons[7]?.value || 0) * 32767) });
  }
  return out;
}

// ── audio ──────────────────────────────────────────────────────────────
// The game hands over a frame's worth of samples each frame. They go into a
// ring in shared memory that an AudioWorklet drains at the hardware's pace,
// so the sound never depends on frame timing. (Scheduling one buffer source
// per frame put a seam every 16.7 ms: rounding of each start time to the
// device's sample grid left tiny gaps or overlaps, a 60 Hz buzz, and every
// late frame re-synced with a gap.) Without AudioWorklet or shared memory it
// falls back to that scheduling.
const RING = 48000 * 2; // stereo frames: two seconds of headroom
let actx = null, audioAt = 0, ring = null, ringIdx = null, worklet = null, audioRate = 48000;
// The worklet plays the ring at the device's pace and never makes a hard cut:
// - it holds the queue near TARGET by resampling a hair faster or slower
//   (at most 0.5%, inaudible) instead of dropping or padding samples, so the
//   game's clock and the sound card's clock can drift apart forever;
// - the game's rate need not match the device's (44.1 kHz cards resample);
// - when the queue runs dry it fades the last sample out over 2 ms, and fades
//   back in once re-primed; a queue far too long fades out, skips ahead to
//   TARGET and fades back in. No step in the waveform, so no clicks or buzz.
const WORKLET = `
class CartRing extends AudioWorkletProcessor {
  constructor(o) {
    super();
    const p = o.processorOptions;
    this.buf = new Float32Array(p.buf);
    this.idx = new Int32Array(p.idx); // [write, read, underruns, skips]
    this.cap = p.cap;
    this.base = p.srcRate / sampleRate;           // source frames per output frame
    this.target = Math.round(p.srcRate * 0.08);   // aim for 80 ms queued
    this.prime = Math.round(p.srcRate * 0.06);    // start (again) at 60 ms
    this.high = Math.round(p.srcRate * 0.35);     // beyond this: fade, skip, fade
    this.fade = Math.max(32, Math.round(sampleRate * 0.002));
    this.rd = 0; this.primed = false; this.gain = 0; this.dir = 0; this.skip = false;
    this.lastL = 0; this.lastR = 0;
  }
  process(_, outs) {
    const L = outs[0][0], R = outs[0][1] || outs[0][0], n = L.length, idx = this.idx, cap = this.cap, b = this.buf;
    const wr = Atomics.load(idx, 0);
    let have = wr - this.rd; if (have < 0) have += cap;
    if (!this.primed && have >= this.prime) { this.primed = true; this.dir = 1; }
    if (this.primed && have > this.high && !this.skip) { this.skip = true; this.dir = -1; }
    // drift: speed up when the queue is long, slow down when short
    let err = (have - this.target) / this.target;
    err = err > 1 ? 1 : err < -1 ? -1 : err;
    const step = this.base * (1 + 0.005 * err);
    const dg = 1 / this.fade;
    for (let i = 0; i < n; i++) {
      if (this.dir > 0) { this.gain += dg; if (this.gain >= 1) { this.gain = 1; this.dir = 0; } }
      else if (this.dir < 0) {
        this.gain -= dg;
        if (this.gain <= 0) {
          this.gain = 0;
          if (this.skip) { // jump to TARGET behind the writer, then fade back in
            this.rd = wr - this.target; if (this.rd < 0) this.rd += cap;
            have = this.target; this.skip = false; this.dir = 1; Atomics.add(idx, 3, 1);
          } else this.dir = 0;
        }
      }
      if (this.primed && have >= 2) {
        const i0 = this.rd | 0, f = this.rd - i0, i1 = i0 + 1 === cap ? 0 : i0 + 1;
        this.lastL = b[2 * i0] + (b[2 * i1] - b[2 * i0]) * f;
        this.lastR = b[2 * i0 + 1] + (b[2 * i1 + 1] - b[2 * i0 + 1]) * f;
        this.rd += step; if (this.rd >= cap) this.rd -= cap;
        have -= step;
      } else if (this.primed) { // ran dry: fade what was playing, wait to re-prime
        this.primed = false; this.dir = -1; Atomics.add(idx, 2, 1);
      }
      L[i] = this.lastL * this.gain; R[i] = this.lastR * this.gain;
    }
    Atomics.store(idx, 1, this.rd | 0);
    return true;
  }
}
registerProcessor('cart-ring', CartRing);`;
async function startAudio(rate) {
  audioRate = rate;
  if (!actx) {
    try { actx = new AudioContext({ sampleRate: rate, latencyHint: 'interactive' }); } catch { actx = new AudioContext(); }
    if (actx.audioWorklet && typeof SharedArrayBuffer !== 'undefined' && window.crossOriginIsolated) {
      try {
        const sb = new SharedArrayBuffer(RING * 2 * 4), si = new SharedArrayBuffer(16);
        ring = new Float32Array(sb);
        ringIdx = new Int32Array(si);
        const url = URL.createObjectURL(new Blob([WORKLET], { type: 'text/javascript' }));
        await actx.audioWorklet.addModule(url);
        worklet = new AudioWorkletNode(actx, 'cart-ring', { numberOfInputs: 0, outputChannelCount: [2],
          processorOptions: { buf: sb, idx: si, cap: RING, srcRate: rate } });
        worklet.connect(actx.destination);
      } catch (e) { console.warn('audio worklet unavailable, scheduling buffers instead:', e); worklet = null; }
    }
  }
  if (actx.state === 'suspended') actx.resume();
}
function queueAudio(samples, f32, rate) {
  if (!actx || actx.state !== 'running' || !samples || !samples.length) return;
  const n = samples.length / 2, k = f32 ? 1 : 1 / 32768;
  if (worklet && rate === audioRate) {
    let wr = Atomics.load(ringIdx, 0);
    const rd = Atomics.load(ringIdx, 1), have = (wr - rd + RING) % RING;
    // the worklet keeps the queue short by itself; only a full ring drops a chunk
    if (have + n >= RING - 1) { window.cwStatus.audioOverruns = (window.cwStatus.audioOverruns || 0) + 1; return; }
    for (let i = 0; i < n; i++) { ring[2 * wr] = samples[2 * i] * k; ring[2 * wr + 1] = samples[2 * i + 1] * k; wr = wr + 1 === RING ? 0 : wr + 1; }
    Atomics.store(ringIdx, 0, wr);
    window.cwStatus.audioUnderruns = Atomics.load(ringIdx, 2);
    window.cwStatus.audioSkips = Atomics.load(ringIdx, 3);
    window.cwStatus.audioQueuedMs = Math.round(((wr - rd + RING) % RING) / rate * 1000);
    return;
  }
  const buf = actx.createBuffer(2, n, rate), l = buf.getChannelData(0), r = buf.getChannelData(1);
  for (let i = 0; i < n; i++) { l[i] = samples[2 * i] * k; r[i] = samples[2 * i + 1] * k; }
  const src = actx.createBufferSource();
  src.buffer = buf;
  src.connect(actx.destination);
  const now = actx.currentTime;
  if (audioAt < now || audioAt > now + 0.25) audioAt = now + 0.02; // late or far ahead: resync
  src.start(audioAt);
  audioAt += buf.duration;
}

// ── save data ──────────────────────────────────────────────────────────
// The cart's save region (cw_app_desc.save) lives in localStorage, keyed by
// the page's path, so each exported game keeps its own and two games on one
// site never share. Loaded before the cart's init; written back whenever the
// region changes (checked twice a second) and when the page is hidden or
// closed. A never-saved cart (an all-zero region) writes nothing.
const SAVE_KEY = 'cartwheel-save:' + location.pathname.replace(/[^/]*$/, '') + CART;
function loadSave() {
  try {
    const b64 = localStorage.getItem(SAVE_KEY);
    if (!b64) return undefined;
    const bin = atob(b64), out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  } catch { return undefined; } // private mode or blocked storage: start fresh
}
let lastSave = null;
function persistSave() {
  try {
    const sav = host.getSaveData && host.getSaveData();
    if (!sav) return;
    if (lastSave && lastSave.length === sav.length && lastSave.every((b, i) => b === sav[i])) return;
    if (!lastSave && !localStorage.getItem(SAVE_KEY) && !sav.some((b) => b !== 0)) { lastSave = sav; return; }
    let bin = '';
    for (let i = 0; i < sav.length; i += 0x8000) bin += String.fromCharCode.apply(null, sav.subarray(i, i + 0x8000));
    localStorage.setItem(SAVE_KEY, btoa(bin));
    lastSave = sav;
    if (window.cwStatus) window.cwStatus.saveBytes = sav.length;
  } catch { /* storage full or blocked: the game keeps running */ }
}
function startSaving() {
  setInterval(persistSave, 500);
  addEventListener('visibilitychange', () => { if (document.visibilityState === 'hidden') persistSave(); });
  addEventListener('pagehide', persistSave);
}

// ── run ────────────────────────────────────────────────────────────────
const host = new CartHostWeb();
try {
  // depth + stencil, and the canvas sized to the cart (fit), let wasmcart
  // present straight into the canvas instead of blitting a redirect FBO
  const bytes = await download(CART);
  // wasmcart's host reports its own load steps through _drawProgress (0.1
  // compile, 0.6 instantiate, 0.95 the cart's init): follow them
  const hostProgress = host._drawProgress ? host._drawProgress.bind(host) : null;
  host._drawProgress = (ctx, p, label) => {
    if (hostProgress) hostProgress(ctx, p, label);
    enter(p >= 0.95 ? 'init' : p >= 0.6 ? 'instantiate' : 'compile');
  };
  enter('compile');
  // WebGPU draws straight into the canvas (wgpuCanvas); without WebGPU the
  // cart runs on WebGL2 (the factory makes the context only then)
  await host.load(bytes, { wasm: CODE, wgpuCanvas: canvas,
    glBackend: () => canvas.getContext('webgl2', { antialias: false, depth: true, stencil: true }), audioSampleRate: 48000,
    saveData: loadSave() });
} catch (e) {
  if (window.cwStatus && window.cwStatus.ok === false) throw e; /* already shown (the download) */
  fail('The game could not start', `<p>${String(e.message || e).replace(/</g, '&lt;')}</p>`);
}
startSaving();
const info = host.getInfo();
const rate = info.audioSampleRate >= 8000 ? info.audioSampleRate : 48000;

/* The canvas fills the space the page really has: the visual viewport on a
 * phone (it shrinks and grows with the URL bar and keyboard; innerHeight
 * lags it), less the safe-area insets body pads, letterboxed to the cart's
 * aspect. Refitted on every resize, rotation and viewport change, and when
 * the cart changes its resolution. */
let fitKey = '';
function fit() {
  const { width: w, height: h } = host.getInfo();
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  const vv = window.visualViewport;
  const cs = getComputedStyle(document.body);
  const padX = parseFloat(cs.paddingLeft) + parseFloat(cs.paddingRight), padY = parseFloat(cs.paddingTop) + parseFloat(cs.paddingBottom);
  const aw = (vv ? vv.width : innerWidth) - padX, ah = (vv ? vv.height : innerHeight) - padY;
  const key = `${w}x${h}@${aw}x${ah}`;
  if (key === fitKey) return;
  fitKey = key;
  const s = Math.max(0.01, Math.min(aw / w, ah / h));
  canvas.style.width = Math.floor(w * s) + 'px';
  canvas.style.height = Math.floor(h * s) + 'px';
  if (vv && (vv.offsetLeft || vv.offsetTop)) window.scrollTo(0, 0);
}
addEventListener('resize', fit);
addEventListener('orientationchange', () => setTimeout(fit, 100));
if (window.visualViewport) { visualViewport.addEventListener('resize', fit); visualViewport.addEventListener('scroll', fit); }
// browsers that ignore user-scalable=no (iOS Safari) still get no pinch or double-tap zoom
for (const ev of ['gesturestart', 'gesturechange', 'dblclick']) document.addEventListener(ev, (e) => e.preventDefault(), { passive: false });
document.addEventListener('touchmove', (e) => { if (e.touches.length > 1) e.preventDefault(); }, { passive: false });
fit();

/* On a touch-first phone the first tap also takes the page fullscreen and
 * locks landscape where the browser allows it (Android Chrome; iOS ignores
 * both), so the URL bar stops eating the game. Once only: a player who exits
 * fullscreen is left alone. */
const coarsePointer = matchMedia('(pointer: coarse)').matches && !matchMedia('(any-pointer: fine)').matches;
let triedFullscreen = false;
function phoneFullscreen() {
  if (!coarsePointer || triedFullscreen || document.fullscreenElement) return;
  triedFullscreen = true;
  const el = document.documentElement;
  if (!el.requestFullscreen) return;
  el.requestFullscreen({ navigationUI: 'hide' }).then(() => {
    if (screen.orientation && screen.orientation.lock) screen.orientation.lock('landscape').catch(() => {});
  }).catch(() => {});
}
const gesture = () => { startAudio(rate); phoneFullscreen(); };
addEventListener('keydown', (e) => {
  gesture();
  const sc = KEYS[e.code];
  if (sc === undefined) return;
  e.preventDefault();
  if (!e.repeat) host.keyDown(sc);
});
addEventListener('keyup', (e) => {
  const sc = KEYS[e.code];
  if (sc === undefined) return;
  e.preventDefault();
  host.keyUp(sc);
});
addEventListener('blur', () => host.blur && host.blur());
addEventListener('focus', () => host.focus && host.focus());

// pointers: the mouse is slot 0, touches 1..9 (wasmcart pointer ABI), in cart pixels
const touchSlot = new Map();
function cartXY(e) {
  const r = canvas.getBoundingClientRect();
  return [Math.round((e.clientX - r.left) * canvas.width / r.width), Math.round((e.clientY - r.top) * canvas.height / r.height)];
}
function slotOf(e, down) {
  if (e.pointerType === 'mouse') return 0;
  if (!touchSlot.has(e.pointerId) && down) {
    for (let s = 1; s <= 9; s++) if (![...touchSlot.values()].includes(s)) { touchSlot.set(e.pointerId, s); break; }
  }
  return touchSlot.get(e.pointerId);
}
// A press must be seen by at least one frame: a tap whose down and up land
// between two frames (a quick tap, or the first tap while the audio starts)
// would otherwise never reach the game. Such a release waits for the next frame.
let framesRun = 0;
const downAt = new Map(), lateUps = [];
canvas.addEventListener('pointerdown', (e) => {
  gesture();
  canvas.focus();
  canvas.setPointerCapture(e.pointerId);
  const s = slotOf(e, true), [x, y] = cartXY(e);
  if (s !== undefined) {
    host.setPointer(s, x, y, e.buttons || 1, true);
    downAt.set(s, framesRun);
  }
  e.preventDefault();
});
canvas.addEventListener('pointermove', (e) => {
  const s = slotOf(e, false), [x, y] = cartXY(e);
  if (s !== undefined) host.setPointer(s, x, y, e.buttons, true);
});
const up = (e) => {
  const s = slotOf(e, false), [x, y] = cartXY(e);
  if (s === undefined) return;
  const release = () => {
    host.setPointer(s, x, y, e.buttons, e.pointerType === 'mouse');
    if (e.pointerType !== 'mouse') touchSlot.delete(e.pointerId); /* the slot frees with its release */
  };
  if (downAt.get(s) === framesRun) lateUps.push(release);
  else release();
};
canvas.addEventListener('pointerup', up);
canvas.addEventListener('pointercancel', up);
canvas.addEventListener('pointerleave', (e) => { if (e.pointerType === 'mouse') host.setPointer(0, 0, 0, 0, false); });
canvas.addEventListener('wheel', (e) => { host.wheel(Math.round(-e.deltaX), Math.round(-e.deltaY)); e.preventDefault(); }, { passive: false });
canvas.addEventListener('contextmenu', (e) => e.preventDefault());

// ── the debug channel (cartwheel/debug.h), for tools: the same requests the
//    editor and romdev send, through the cart's mailbox fields ───────────
function debugFields() {
  const ex = host.instance.exports;
  if (!ex.wc_debug_state) return null;
  const dv = new DataView((host.memory || ex.memory).buffer);
  const str = (q) => { let s = ''; for (let b; (b = dv.getUint8(q)); q++) s += String.fromCharCode(b); return s; };
  const f = {};
  for (let p = ex.wc_debug_state(); ; p += 16) {
    const name = dv.getUint32(p, true);
    if (!name) break;
    f[str(name)] = { ptr: dv.getUint32(p + 4, true), type: dv.getUint8(p + 8), len: dv.getUint32(p + 12, true) };
  }
  return { dv, f };
}
let pending = null;
window.cwDebug = {
  /** One request ("status", "tree /root 1", ...); resolves with the parsed reply. */
  command(text) {
    const d = debugFields();
    if (!d || !d.f['dbg.cmd']) return Promise.reject(new Error('this game has no debug channel (cw_app_desc.debug)'));
    if (pending) return Promise.reject(new Error('one request at a time'));
    const bytes = new TextEncoder().encode(text);
    const cmd = d.f['dbg.cmd'];
    if (bytes.length >= cmd.len) return Promise.reject(new Error('request too long'));
    for (let i = 0; i < bytes.length; i++) d.dv.setUint8(cmd.ptr + i, bytes[i]);
    d.dv.setUint8(cmd.ptr + bytes.length, 0);
    const seq = d.dv.getUint32(d.f['dbg.cmd_seq'].ptr, true) + 1;
    d.dv.setUint32(d.f['dbg.cmd_seq'].ptr, seq, true);
    return new Promise((res, rej) => { pending = { seq, res, rej, deadline: performance.now() + 10000 }; });
  },
  /** Every named debug field's current value (numbers, arrays, hex for bytes). */
  fields() {
    const d = debugFields();
    if (!d) return null;
    const rd = [(q) => d.dv.getUint8(q), (q) => d.dv.getInt8(q), (q) => d.dv.getUint16(q, true), (q) => d.dv.getInt16(q, true),
      (q) => d.dv.getUint32(q, true), (q) => d.dv.getInt32(q, true), (q) => d.dv.getFloat32(q, true), (q) => d.dv.getFloat64(q, true)];
    const size = [1, 1, 2, 2, 4, 4, 4, 8];
    const out = {};
    for (const [k, v] of Object.entries(d.f)) {
      if (v.type === 8) continue; // byte blocks (the mailbox itself)
      out[k] = v.len === 1 ? rd[v.type](v.ptr) : Array.from({ length: v.len }, (_, i) => rd[v.type](v.ptr + i * size[v.type]));
    }
    return out;
  },
};
function pollDebug() {
  if (!pending) return;
  const d = debugFields();
  if (d.dv.getUint32(d.f['dbg.reply_seq'].ptr, true) === pending.seq) {
    const n = d.dv.getUint32(d.f['dbg.reply_len'].ptr, true), r = d.f['dbg.reply'].ptr;
    const bytes = new Uint8Array(n); // TextDecoder refuses views of shared memory: copy out
    bytes.set(new Uint8Array(d.dv.buffer, r, n));
    const text = new TextDecoder().decode(bytes);
    const p = pending;
    pending = null;
    try { p.res(JSON.parse(text)); } catch { p.res(text); }
  } else if (performance.now() > pending.deadline) {
    const p = pending;
    pending = null;
    p.rej(new Error('no reply in 10 s'));
  }
}

// a game's buzz requests (cw_platform_vibrate: the debug field cw.vibrate,
// a request count then ms) become navigator.vibrate where the browser has it
let vibPtr = 0, vibSeq = 0, vibLook = 0;
function pollVibrate() {
  if (!navigator.vibrate) return;
  if (!vibPtr) {
    if (vibLook++ % 30) return; // the field appears once the game is up: look now and then
    const d = debugFields(), f = d && d.f['cw.vibrate'];
    if (!f) return;
    vibPtr = f.ptr;
    vibSeq = d.dv.getUint32(vibPtr, true);
  }
  const dv = new DataView((host.memory || host.instance.exports.memory).buffer);
  const seq = dv.getUint32(vibPtr, true);
  if (seq === vibSeq) return;
  vibSeq = seq;
  const ms = dv.getUint32(vibPtr + 4, true);
  if (ms) try { navigator.vibrate(ms); } catch {}
}

window.cwStatus = { ok: true, frames: 0, width: info.width, height: info.height };
let shown = false;
enter('frame');
async function frame() {
  fit(); // a no-op unless the cart or the viewport changed size
  const result = await host.runFrame(readPads());
  if (!shown) {
    /* a game's warm-up (cw.loading = done, total, stage) carries the bar on;
     * then the panel fades over a frame already on the canvas (no black gap) */
    const f = window.cwDebug.fields(), l = f && f['cw.loading'];
    if (l && l[1] > 0 && l[0] < l[1]) enter('warmup', l[0] / l[1], WARM_LABELS[l[2]] || 'Preparing');
    else {
      shown = true;
      window.cwLoadTimes.ready = Math.round(performance.now());
      requestAnimationFrame(() => { panel.classList.add('done'); panel.setAttribute('aria-hidden', 'true'); });
    }
  }
  framesRun++;
  for (const release of lateUps.splice(0)) release();
  window.cwStatus.frames = host.frameCount;
  if (result) {
    if (result.audio) queueAudio(result.audio, info.audioIsF32, rate);
    if (result.width !== canvas.width || result.height !== canvas.height) fit();
  }
  try {
    pollVibrate();
    pollDebug();
  } catch (e) { // a tool's request must never stop the game
    const p = pending;
    pending = null;
    if (p) p.rej(e);
  }
  requestAnimationFrame(frame);
}
// a phone or tablet: tell the game before its first frame, so it starts with
// touch prompts / controls (cartwheel's cw_platform_touch_primary). A game
// without the debug channel just never hears it.
const coarse = matchMedia('(pointer: coarse)').matches && !matchMedia('(any-pointer: fine)').matches;
if (coarse) window.cwDebug.command('touch_primary 1').catch(() => {});
canvas.focus();
requestAnimationFrame(frame);
