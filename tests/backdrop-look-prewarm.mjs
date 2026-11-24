// A Backdrop world or a look shown for the first time costs no frame (samples/style_switch): the dual cart through
// romdev on GL and WebGPU, every frame timed with frame({op:'timing'}) (window 1: wall, CPU and GPU per frame). The
// sample shows two worlds (fire, a stage in volumetric haze) and three looks (ASCII, half blocks with a palette, a
// one-bit dither at the DISPLAY stage) for the first time after the warm-up, then does every change again with all of
// them shown once (sw.event 0..5, then 6..11). Each change's frames (its first tick and the next 5 frames) are held
// to the run's usual frame (the median of the frames outside every change): no frame there may cost more than LIMIT_MS
// (3 ms) of CPU over it, the first showing or the second. A fixed CPU budget rather than a ratio: the usual frame's
// CPU is under 1.5 ms, so 1.5x of it is noise, while 3 ms more still leaves a 60 Hz frame well inside its 16.7 ms.
// Wall time, on the change's first two frames (+0 and +1: where every first-use cost landed before the prewarm, CPU
// and wall alike; later frames of a change on a shared GPU carry other processes' work, which the CPU time of the
// cart's own host does not): a first showing's worst may cost no more than RATIO (1.5) x the usual frame, or than the
// worst frame of the same change shown the second time (the same pictures drawn with everything built: a cross-fade
// draws two worlds, which is GPU work every time, not a first-use cost), or than the three frames just before the change (another process's load on
// a shared GPU lasts for frames, a first-use cost lands on the change's own), plus SLACK_MS; judged only when no other
// process used the GPU during the backend's runs (romdev's contention report), else reported. A frame's cost is its
// least over RUNS runs (the runs are deterministic, so frame i is the same work each time; another process only adds),
// which drops a shared machine's noise.
// Nothing of a prewarm is seen. In a test (a deterministic load) the prewarm runs in the first frame, a frame of the game:
// that frame is the same picture, pixel for pixel, as the same frame without the prewarm (CW_TEST_NO_PREWARM=1);
// control CW_TEST_PREWARM_SHOW=1 (the worlds' pictures blended at 1, the looks drawn after the pass's real run) must
// change it. As a player's host runs it (no seed: the warm-up spread over frames under the loading screen) the frame
// whose render ran the prewarms (sw.prewarms first above 0) shows the loading screen alone: at least 97% of its pixels
// are the loading screen's background colour; control: a frame of play judged the same way must fail.
// Control: CW_TEST_NO_PREWARM=1 (the warm-up without the prewarm of backdrops and looks, the engine before it) must
// show a first-showing change at least CONTROL_MS of CPU over the usual frame, on each backend.
// The sea (sw.event 6 and 13): a night sea world (Water3D) that lights the scene (environment) fades in while SSR and
// ssr_backdrop turn on, the passes the sample gives cw_post_prewarm. Its first showing is held as the others are, and the
// control without the prewarm must cost it at least CONTROL_MS too. Its environment's first capture and filter are the
// prewarm's: its first two frames cost the GPU at most GPU_SLACK_MS over its second showing's; control
// CW_TEST_NO_ENV_PREWARM=1 (the capture left to its first shown frame, as before) must cost more.
// Run: node tests/backdrop-look-prewarm.mjs [--gpu gl|webgpu] [--dump]   (127.0.0.1:7331; ROMDEV_URL overrides)
import { request as httpRequest } from 'node:http';
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { diffImages, readImage } from '../tools/lib/png.mjs';
import { wgpuPin } from '../tools/lib/wgpu-pin.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..'), OUT = join(ROOT, 'out/gate/backdrop-look-prewarm');
const CART = process.env.SW_CART || join(ROOT, 'build/cart-wgpu/carts/style_switch.wasc');
const BASE = new URL(process.env.ROMDEV_URL || 'http://127.0.0.1:7331');
const FRAMES = 960;
const EV = 7; // changes a round (sw.event 0..6, then 7..13 the same again)
const LIMIT_MS = 3; // a change's frames over the usual frame, CPU ms
const RATIO = 1.5; // a first showing's wall time over the usual frame's, times
const SLACK_MS = 1.5; // or over its second showing's, ms
const CONTROL_MS = 8; // without the prewarm a first showing costs at least this much CPU over the usual frame
const RUNS = 3;
const AFTER = 4; // frames after a change's first frame that are its frames too (with the first: frames +0 .. +5)
const NAMES = ['fire in', 'ASCII', 'half blocks', 'one bit (DISPLAY)', 'stage in', 'plain', 'sea in + SSR'];
const SEA = 6; // the sea's change: a world that lights the scene, shown with SSR and ssr_backdrop turned on
// a world that lights the scene filters its environment on the first frame of every showing (the filter of a finished
// capture, as every env_every frames of a fade: a usual cost, about 2.5 ms of CPU on WebGPU on the 890M): its second
// showing may cost that much more than the usual frame (its first showing is held to LIMIT_MS: the prewarm filtered it)
const ENV_FILTER_MS = 2;
const GPU_SLACK_MS = 0.25; // the sea's first showing's GPU time over its second showing's (frames +0, +1): with the prewarm about 1.5 ms under it (its first frame filters nothing), the old path 0.6 to 1.6 ms over
const FIELDS = ['sw.tick', 'sw.event', 'sw.fire_renders', 'sw.stage_renders', 'sw.sea_renders', 'cw.three.missing'];
mkdirSync(OUT, { recursive: true });
const arg = (n) => (process.argv.includes(n) ? process.argv[process.argv.indexOf(n) + 1] : undefined);
const gpus = arg('--gpu') ? [arg('--gpu')] : ['gl', 'webgpu'];
const dump = process.argv.includes('--dump');
const noisyOk = process.argv.includes('--noisy-ok'), RETRIES = 6, WAIT_S = 20;
// a backend whose runs found another process on the GPU (any attempt, retried or kept): its wall times are reported,
// not judged (the CPU time of the cart's own host is judged either way: it is where a first-use cost lands)
const busy = { gl: false, webgpu: false };

function post(tool, args, session) {
  const body = JSON.stringify(args);
  return new Promise((ok, fail) => {
    const req = httpRequest({ host: BASE.hostname, port: BASE.port || 80, path: `/tool/${tool}`, method: 'POST', agent: false,
      headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body), 'x-romdev-session': session,
        'x-romdev-agent': process.env.CW_ROMDEV_AGENT || 'cartwheel-backdrop-look-prewarm' } }, (res) => {
      let text = '';
      res.setEncoding('utf8');
      res.on('data', (c) => { text += c; });
      res.on('end', () => {
        clearTimeout(timer);
        let b;
        try { b = JSON.parse(text); } catch { b = text; }
        if (res.statusCode < 200 || res.statusCode >= 300 || (b && b.isError)) fail(new Error(`romdev ${tool}: ${typeof b === 'string' ? b : JSON.stringify(b).slice(0, 1500)}`));
        else ok(b);
      });
    });
    const timer = setTimeout(() => req.destroy(new Error(`romdev ${tool}: no reply in 900 s`)), 900000);
    req.on('error', (e) => { clearTimeout(timer); fail(new Error(`romdev at ${BASE.origin} ${tool}: ${e.message} (is the server running?)`)); });
    req.end(body);
  });
}

/** one deterministic load, every frame timed */
async function run(gpu, label, env, accept) {
  const session = `cw-bd-look-prewarm-${process.pid}-${label}-${gpu}`;
  const pin = wgpuPin();
  const first = Object.keys(env).length ? `env ${Object.entries(env).map(([k, v]) => `${k}=${v}`).join(' ')}` : undefined;
  const loaded = await post('loadMedia', { platform: 'wasmcart', path: CART, deterministicSeed: 1, deterministicStepMs: 1000 / 60,
    ...(first ? { firstCommand: first } : {}), gpuPower: pin.power, wgpu: gpu === 'webgpu', ...(gpu === 'webgpu' ? { gpuTiming: true } : {}) }, session);
  try {
    if (first) {
      const fr = loaded.firstCommandReply;
      if (fr === undefined) throw new Error('romdev: firstCommand was not delivered (romdev too old for it)');
      if (fr && typeof fr === 'object' && typeof fr.error === 'string') throw new Error(`romdev: '${first}' answered: ${fr.error}`);
    }
    if (loaded.gpu !== gpu) throw new Error(`romdev: the cart runs on ${loaded.gpu}, not ${gpu}`);
    pin.check('romdev', gpu === 'webgpu' ? loaded.gpuAdapter && (loaded.gpuAdapter.device || loaded.gpuAdapter.description) : loaded.glRenderer);
    const t = await post('frame', { op: 'timing', frames: FRAMES, warmup: 0, window: 1, debugFields: FIELDS }, session);
    t.noisy = !!(t.contention && t.contention.noisy);
    if (t.noisy && label.startsWith('prewarm')) busy[gpu] = true;
    if (t.noisy && !accept) {
      console.log(`     (${label} ${gpu}: a noisy run, ${(t.contention.reasons || []).join('; ')}: again in ${WAIT_S} s)`);
      throw new Error('noisy');
    }
    if (t.gpu && t.gpu.onPinnedGpu === false) throw new Error(`romdev: GPU work went off the test GPU: ${JSON.stringify(t.gpu.workByDeviceMs)}`);
    await post('frame', { op: 'screenshot', path: join(OUT, `${label}.${gpu}.png`) }, session);
    return t;
  } finally {
    await post('host', { op: 'shutdown' }, session).catch(() => {});
  }
}
async function runRetry(gpu, label, env) {
  for (let i = 0; ; i++) {
    try { return await run(gpu, label, env, noisyOk || i >= RETRIES); } catch (e) {
      if (e.message !== 'noisy' || i >= RETRIES) throw e;
      await new Promise((r) => setTimeout(r, WAIT_S * 1000));
    }
  }
}

const median = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[s.length >> 1] : NaN; };
const last = (w, f) => (w.fields && w.fields[f] ? w.fields[f].last : undefined);
const f2 = (x) => (Number.isFinite(x) ? x.toFixed(2) : String(x));

/** each change's frames against the run's usual frame (a frame's cost: its least over the runs) */
function analyse(runs, label, gpu) {
  const n = Math.min(...runs.map((t) => t.windows.length));
  const w = runs[0].windows.slice(0, n);
  const minOf = (k) => w.map((x, i) => Math.min(...runs.map((t) => t.windows[i][k] ?? NaN)));
  const wall = minOf('wallMs'), cpu = minOf('cpuMs'), gpuMs = minOf('gpuMs');
  const ev = w.map((x) => last(x, 'sw.event'));
  const starts = [];
  for (let i = 1; i < n; i++) if (ev[i] !== ev[i - 1] && ev[i] >= 0) starts.push({ e: ev[i], i });
  const inChange = new Set();
  for (const s of starts) for (let k = 0; k <= AFTER + 1 && s.i + k < n; k++) inChange.add(s.i + k);
  const usual = w.map((x, i) => i).filter((i) => i >= 10 && !inChange.has(i));
  const base = { wall: median(usual.map((i) => wall[i])), cpu: median(usual.map((i) => cpu[i])), gpu: median(usual.map((i) => gpuMs[i])) };
  const changes = starts.map((s) => {
    const fr = [];
    for (let k = 0; k <= AFTER + 1 && s.i + k < n; k++) fr.push(s.i + k);
    const worst = fr.reduce((m, i) => (cpu[i] > cpu[m] ? i : m), fr[0]);
    const worstWall = fr.slice(0, 2).reduce((m, i) => (wall[i] > wall[m] ? i : m), fr[0]);
    const before = Math.max(...[1, 2, 3].map((k) => s.i - k).filter((i) => i >= 0).map((i) => wall[i]));
    return { e: s.e, frame: w[s.i].from, cpu: cpu[worst] - base.cpu, cpuAt: worst - s.i, wall: wall[worstWall], wallAt: worstWall - s.i, wallAll: Math.max(...fr.map((i) => wall[i])), before, gpu: Math.max(...fr.map((i) => gpuMs[i])),
      gpu01: Math.max(...fr.slice(0, 2).map((i) => gpuMs[i])) };
  });
  const missing = last(w[n - 1], 'cw.three.missing');
  if (dump) writeFileSync(join(OUT, `${label}.${gpu}.json`), JSON.stringify({ base, changes, frames: w.map((x, i) => ({ f: x.from, e: ev[i], wall: wall[i], cpu: cpu[i], gpu: gpuMs[i] })) }, null, 1));
  return { base, changes, missing, noisy: runs.some((t) => t.noisy), fire: last(w[n - 1], 'sw.fire_renders'), stage: last(w[n - 1], 'sw.stage_renders'), sea: last(w[n - 1], 'sw.sea_renders') };
}

/** a deterministic load's first frame (where a test's prewarm runs): the load runs it (its first request is answered
 * there), so it is captured before any step; every load sends a first request (a harmless one without a switch) */
async function firstFrame(gpu, label, env) {
  const session = `cw-bd-look-prewarm-first-${process.pid}-${label}-${gpu}`;
  const first = `env SW_FIRST=1 ${Object.entries(env).map(([k, v]) => `${k}=${v}`).join(' ')}`.trim();
  const loaded = await post('loadMedia', { platform: 'wasmcart', path: CART, deterministicSeed: 1, deterministicStepMs: 1000 / 60, gpuPower: wgpuPin().power,
    wgpu: gpu === 'webgpu', firstCommand: first }, session);
  try {
    if (loaded.firstCommandReply === undefined) throw new Error('romdev: firstCommand was not delivered (romdev too old for it)');
    const shot = join(OUT, `first-${label}.${gpu}.png`);
    await post('frame', { op: 'screenshot', path: shot }, session);
    const r = await post('wasm', { op: 'read', name: 'sw.prewarms' }, session);
    return { shot, prewarms: r.value };
  } finally {
    await post('host', { op: 'shutdown' }, session).catch(() => {});
  }
}
/** a load with no seed (the warm-up spread under the loading screen): the frame whose render ran the prewarms, and a
 * frame of play 120 frames on */
async function spreadFrames(gpu) {
  const session = `cw-bd-look-prewarm-spread-${process.pid}-${gpu}`;
  await post('loadMedia', { platform: 'wasmcart', path: CART, gpuPower: wgpuPin().power, wgpu: gpu === 'webgpu' }, session);
  try {
    const st = await post('frame', { op: 'step', frames: 900, stopWhen: { field: 'sw.prewarms', atLeast: 1 }, fields: ['sw.prewarms', 'sw.tick'] }, session);
    const at = join(OUT, `spread-prewarm.${gpu}.png`), play = join(OUT, `spread-play.${gpu}.png`);
    await post('frame', { op: 'screenshot', path: at }, session);
    await post('frame', { op: 'step', frames: 120 }, session);
    await post('frame', { op: 'screenshot', path: play }, session);
    return { stopped: st.stopped, prewarms: st.fields ? st.fields['sw.prewarms'] : undefined, tick: st.fields ? st.fields['sw.tick'] : undefined, at, play };
  } finally {
    await post('host', { op: 'shutdown' }, session).catch(() => {});
  }
}
/** the share of pixels that are the engine's loading screen background (0.063, 0.075, 0.102 in sRGB: 16, 19, 26, within 2) */
function loadingShare(path) {
  const im = readImage(path);
  let n = 0;
  for (let i = 0; i < im.w * im.h; i++) {
    const r = im.rgba[i * 4], g = im.rgba[i * 4 + 1], b = im.rgba[i * 4 + 2];
    if (Math.abs(r - 16) <= 2 && Math.abs(g - 19) <= 2 && Math.abs(b - 26) <= 2) n++;
  }
  return n / (im.w * im.h);
}

let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const note = (msg) => console.log(`     ${msg}`);
const table = (a) => a.changes.map((c) => `${String(c.e).padStart(2)} ${(NAMES[c.e % EV] + (c.e >= EV ? ' (again)' : '')).padEnd(26)} +${f2(c.cpu).padStart(6)} ms CPU (frame +${c.cpuAt}), ${f2(c.wall).padStart(6)} ms wall (frame +${c.wallAt}), GPU ${f2(c.gpu)} ms (+0/+1 ${f2(c.gpu01)})`).join('\n     ');
// the sea's first showing's GPU time on its first two frames over its second showing's (its environment's first capture
// is the prewarm's: the first shown frame draws only its share of the next capture, as the second showing's does)
const seaGpu = (a) => {
  const c1 = a.changes.find((x) => x.e === SEA), c2 = a.changes.find((x) => x.e === SEA + EV);
  return c1 && c2 ? { first: c1.gpu01, again: c2.gpu01, over: c1.gpu01 - c2.gpu01 } : null;
};
for (const gpu of gpus) {
  const runs = [];
  for (let i = 0; i < RUNS; i++) runs.push(await runRetry(gpu, `prewarm${i ? i + 1 : ''}`, {}));
  const a = analyse(runs, 'prewarm', gpu);
  note(`${gpu}: usual frame ${f2(a.base.wall)} ms wall / ${f2(a.base.cpu)} ms CPU / ${f2(a.base.gpu)} ms GPU (the least of ${RUNS} runs per frame); each change's worst frame over it:\n     ${table(a)}`);
  check(a.changes.length === 2 * EV, `${gpu}: the run made all ${2 * EV} changes (${a.changes.length}; fire ${a.fire} pictures, stage ${a.stage}, sea ${a.sea})`);
  check(!a.missing, `${gpu}: no program state missing (${a.missing || 0})`);
  for (const c of a.changes) {
    const name = `${NAMES[c.e % EV]}${c.e >= EV ? ' again' : ' first'}`;
    const lim = LIMIT_MS + (c.e === SEA + EV ? ENV_FILTER_MS : 0);
    check(c.cpu <= lim, `${gpu}: ${name}: at most ${lim} ms of CPU over the usual frame (+${f2(c.cpu)} ms)`);
    if (c.e >= EV) continue;
    const again = a.changes.find((x) => x.e === c.e + EV);
    const bound = Math.max(RATIO * a.base.wall, (again ? again.wallAll : 0) + SLACK_MS, c.before + SLACK_MS);
    if (a.noisy || busy[gpu]) note(`${gpu}: ${name}: wall not judged (another process used the GPU during these runs): ${f2(c.wall)} ms, bound ${f2(bound)}`);
    else check(c.wall <= bound, `${gpu}: ${name}: its first two frames at most ${RATIO}x the usual frame or its second showing or the 3 frames before it + ${SLACK_MS} ms of wall time (${f2(c.wall)} ms; usual ${f2(a.base.wall)}, second showing ${again ? f2(again.wallAll) : '-'}, before ${f2(c.before)})`);
  }
  // nothing of a prewarm is seen
  const fw = await firstFrame(gpu, 'prewarm', {}), fn = await firstFrame(gpu, 'none', { CW_TEST_NO_PREWARM: '1' });
  const fs = await firstFrame(gpu, 'show', { CW_TEST_PREWARM_SHOW: '1' });
  const d0 = diffImages(readImage(fw.shot), readImage(fn.shot)), d1 = diffImages(readImage(fs.shot), readImage(fn.shot));
  check(fw.prewarms > 0 && !(fn.prewarms > 0), `${gpu}: the first frame of a test ran the prewarms (${fw.prewarms} prewarm renders; ${fn.prewarms} without it)`);
  check(d0.comparable && d0.max === 0, `${gpu}: and is the same picture as without the prewarm (${(d0.gt2 * 100).toFixed(3)}% of pixels over 2, max ${d0.max})`);
  check(d1.comparable && d1.gt2 > 0.01, `${gpu} CONTROL: the prewarm shown (CW_TEST_PREWARM_SHOW=1) changes it (${(d1.gt2 * 100).toFixed(2)}% of pixels over 2)`);
  const sp = await spreadFrames(gpu);
  const shareAt = loadingShare(sp.at), sharePlay = loadingShare(sp.play);
  check(sp.stopped && sp.prewarms > 0 && !(sp.tick > 0), `${gpu}: unseeded, the warm-up's last render ran the prewarms before any tick (${sp.prewarms} prewarm renders, tick ${sp.tick})`);
  check(shareAt >= 0.97, `${gpu}: and that frame shows the loading screen alone (${(shareAt * 100).toFixed(2)}% of pixels its background)`);
  check(sharePlay < 0.97, `${gpu} CONTROL: a frame of play judged the same way fails (${(sharePlay * 100).toFixed(2)}%)`);
  const ctl = analyse([await runRetry(gpu, 'control-no-prewarm', { CW_TEST_NO_PREWARM: '1' })], 'control', gpu);
  note(`${gpu} control (no prewarm): usual frame ${f2(ctl.base.cpu)} ms CPU:\n     ${table(ctl)}`);
  const worstFirst = Math.max(...ctl.changes.filter((c) => c.e < EV).map((c) => c.cpu));
  check(worstFirst >= CONTROL_MS, `${gpu} CONTROL: without the prewarm a first showing costs at least ${CONTROL_MS} ms of CPU over the usual frame (+${f2(worstFirst)} ms)`);
  const ctlSea = ctl.changes.find((c) => c.e === SEA);
  check(ctlSea && ctlSea.cpu >= CONTROL_MS, `${gpu} CONTROL: without the prewarm the sea's first showing with SSR costs at least ${CONTROL_MS} ms of CPU over the usual frame (+${ctlSea ? f2(ctlSea.cpu) : '-'} ms)`);
  // the environment's first capture made in the warm-up: the sea's first shown frames cost the GPU what its second
  // showing's do (control CW_TEST_NO_ENV_PREWARM=1: the capture left to its first shown frame, as before)
  const sg = seaGpu(a);
  if (a.noisy || busy[gpu]) note(`${gpu}: the sea's GPU not judged (another process used the GPU): first ${sg ? f2(sg.first) : '-'} ms, again ${sg ? f2(sg.again) : '-'}`);
  else check(sg && sg.over <= GPU_SLACK_MS, `${gpu}: the sea's first two frames cost the GPU at most ${GPU_SLACK_MS} ms over its second showing's (${sg ? `${f2(sg.first)} vs ${f2(sg.again)} ms` : 'no change'})`);
  const envRuns = [];
  for (let i = 0; i < RUNS; i++) envRuns.push(await runRetry(gpu, `control-no-env-prewarm${i ? i + 1 : ''}`, { CW_TEST_NO_ENV_PREWARM: '1' }));
  const ce = seaGpu(analyse(envRuns, 'control-env', gpu));
  check(ce && ce.over > GPU_SLACK_MS, `${gpu} CONTROL: with the first capture on its first shown frame (CW_TEST_NO_ENV_PREWARM=1) the sea's first two frames cost the GPU more than ${GPU_SLACK_MS} ms over its second showing's (${ce ? `${f2(ce.first)} vs ${f2(ce.again)} ms, +${f2(ce.over)}` : 'no change'})`);
}
console.log(fail ? `backdrop-look-prewarm: ${fail} failure(s)` : 'ok   backdrop-look-prewarm');
process.exit(fail ? 1 : 0);
