// The first image into the decal atlas mid-run costs no frame (samples/decal_late): the dual cart through romdev on
// GL and WebGPU, every frame timed with frame({op:'timing'}) (window 1: wall, CPU and GPU per frame). At tick 60 two
// Decal3D nodes with the atlas's first image are stamped; the stamp's frame must cost no more than the run's usual
// frame by more than LIMIT_MS of wall time, and no later frame may either. The same run stamped at the start
// (DECAL_AT_LOAD=1, how a game worked around the hitch) draws the same last frame: a pixel diff of the two screenshots.
// The far mark samples the atlas's mip 2, so late.far_ok proves the new cell's mips are there.
// Controls: CW_TEST_DECAL_FULL_MIPS=1 (the engine's path before v0.46.2: the image loads at its first draw and every
// mip of the whole atlas is rebuilt then) must cost the stamp's frame at least CONTROL_MS more than the usual frame;
// CW_TEST_DECAL_NO_MIPS=1 (a new cell's mips left empty) must fail far_ok.
// Run: node tests/decal-first-image.mjs [--gpu gl|webgpu]   (127.0.0.1:7331; ROMDEV_URL overrides)
import { request as httpRequest } from 'node:http';
import { mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { diffImages, readImage } from '../tools/lib/png.mjs';
import { wgpuPin } from '../tools/lib/wgpu-pin.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..'), OUT = join(ROOT, 'out/gate/decal-first-image');
const CART = join(ROOT, 'build/cart-wgpu/carts/decal_late.wasc');
const BASE = new URL(process.env.ROMDEV_URL || 'http://127.0.0.1:7331');
const FRAMES = 130, STAMP_TICK = 60;
const LIMIT_MS = 1.5; // the stamp's frame (and any other) over the usual frame, CPU and wall time: the fix, preloaded
const DECODE_MS = 4; // the stamp's frame when the image is not preloaded: its decode lands there
const CONTROL_MS = { gl: 4, webgpu: 6 }; // the control's stamp frame over the usual frame, CPU time, at least
const RUNS = 3; // runs of the fixed path: a frame's cost is its least over them (below)
const FIELDS = ['late.tick', 'late.stamped', 'late.load_ms', 'late.decode_ms', 'late.atlas', 'late.visible', 'late.far_ok'];
mkdirSync(OUT, { recursive: true });
const gpus = process.argv.includes('--gpu') ? [process.argv[process.argv.indexOf('--gpu') + 1]] : ['gl', 'webgpu'];
// a shared GPU: a noisy run (another process on the test GPU) is run again after a wait, RETRIES times, then kept
// (--noisy-ok keeps the first); a noisy run's CPU time is judged (the hitch is CPU work in the cart: the decode and
// the mips), its wall time only reported
const noisyOk = process.argv.includes('--noisy-ok'), RETRIES = 6, WAIT_S = 20;

function post(tool, args, session) {
  const body = JSON.stringify(args);
  return new Promise((ok, fail) => {
    const req = httpRequest({ host: BASE.hostname, port: BASE.port || 80, path: `/tool/${tool}`, method: 'POST', agent: false,
      headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body), 'x-romdev-session': session,
        'x-romdev-agent': process.env.CW_ROMDEV_AGENT || 'cartwheel-decal-first-image' } }, (res) => {
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
    const timer = setTimeout(() => req.destroy(new Error(`romdev ${tool}: no reply in 600 s`)), 600000);
    req.on('error', (e) => { clearTimeout(timer); fail(new Error(`romdev at ${BASE.origin} ${tool}: ${e.message} (is the server running?)`)); });
    req.end(body);
  });
}

/** one deterministic load, every frame timed, the last frame's screenshot */
async function run(gpu, label, env, accept) {
  const session = `cw-decal-first-image-${process.pid}-${label}-${gpu}`;
  const pin = wgpuPin();
  const shot = join(OUT, `${label}.${gpu}.png`);
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
    if (t.noisy && !accept) {
      console.log(`     (${label} ${gpu}: a noisy run, ${(t.contention.reasons || []).join('; ')}: again in ${WAIT_S} s)`);
      throw new Error('noisy'); // a fresh load: the stamp is at a fixed tick of a run
    }
    if (t.noisy) console.log(`     (${label} ${gpu}: a noisy run, ${(t.contention.reasons || []).join('; ')}: wall times are not judged)`);
    if (t.gpu && t.gpu.onPinnedGpu === false) throw new Error(`romdev: GPU work went off the test GPU: ${JSON.stringify(t.gpu.workByDeviceMs)}`);
    await post('frame', { op: 'screenshot', path: shot }, session);
    return { ...t, shot };
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

/** the frames around the stamp against the run's usual frame. Several runs of one variant: a frame's cost is its
 * minimum over them (the runs are deterministic, so frame i is the same work each time; another process's load only
 * adds), which keeps the repeatable cost and drops the noise of a shared machine. */
function analyse(runs) {
  const n = Math.min(...runs.map((t) => t.windows.length));
  const w = runs[0].windows.slice(0, n);
  const minOf = (k) => w.map((x, i) => Math.min(...runs.map((t) => t.windows[i][k])));
  const wall = minOf('wallMs'), cpu = minOf('cpuMs');
  const stamp = w.findIndex((x) => last(x, 'late.stamped') >= 0);
  if (stamp < 0) throw new Error('the run never stamped the image');
  const usual = w.map((x, i) => i).filter((i) => i >= 3 && (i < stamp - 1 || i > stamp + 2));
  const base = { wall: median(usual.map((i) => wall[i])), cpu: median(usual.map((i) => cpu[i])) };
  const at = [stamp, stamp + 1].filter((i) => i < n); // the stamp's frame and the next (a read-back that lands a frame later)
  const spike = { wall: Math.max(...at.map((i) => wall[i])) - base.wall, cpu: Math.max(...at.map((i) => cpu[i])) - base.cpu };
  const others = w.map((x, i) => i).filter((i) => i >= 3 && i !== stamp && i !== stamp + 1);
  const worstOf = (series, b) => others.reduce((m, i) => (series[i] - b > m.over ? { over: series[i] - b, frame: w[i].from } : m), { over: -1e9, frame: -1 });
  const worst = { wall: worstOf(wall, base.wall), cpu: worstOf(cpu, base.cpu) };
  const end = w[n - 1];
  return { stamp, stampFrame: w[stamp].from, base, spike, worst, noisy: runs.some((t) => t.noisy), runs: runs.length,
    tick: last(end, 'late.tick'), atlas: last(end, 'late.atlas'), visible: last(end, 'late.visible'), farOk: last(end, 'late.far_ok'),
    loadMs: Math.min(...runs.map((t) => last(t.windows[n - 1], 'late.load_ms'))), decodeMs: Math.min(...runs.map((t) => last(t.windows[n - 1], 'late.decode_ms'))) };
}
const f2 = (x) => (Number.isFinite(x) ? x.toFixed(2) : String(x));

let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const note = (msg) => console.log(`     ${msg}`);
for (const gpu of gpus) {
  const show = (a) => `usual frame ${f2(a.base.wall)} ms wall / ${f2(a.base.cpu)} ms CPU; the stamp's frame (frame ${a.stampFrame}) +${f2(a.spike.wall)} ms wall, +${f2(a.spike.cpu)} ms CPU; the image into the atlas ${f2(a.loadMs)} ms (its decode ${f2(a.decodeMs)} ms)`;
  // the image preloaded (cw_decal_preload at tick 1), the stamp at tick 60: the frame must cost as any other
  const preRuns = [];
  for (let i = 0; i < RUNS; i++) preRuns.push(await runRetry(gpu, `preload${i ? i + 1 : ''}`, { DECAL_PRELOAD: '1' }));
  const pre = analyse(preRuns);
  note(`${gpu} preloaded (the least of ${pre.runs} runs per frame): ${show(pre)}; worst other frame +${f2(pre.worst.cpu.over)} ms CPU (frame ${pre.worst.cpu.frame}), +${f2(pre.worst.wall.over)} ms wall (frame ${pre.worst.wall.frame})`);
  check(pre.tick >= STAMP_TICK + 10 && pre.atlas === 2 && pre.visible === 3, `${gpu}: the run stamped the atlas's first image at tick ${STAMP_TICK} (atlas ${pre.atlas} images, ${pre.visible} decals visible, ${pre.tick} ticks)`);
  check(pre.spike.cpu <= LIMIT_MS, `${gpu}: preloaded, the stamp's frame costs at most ${LIMIT_MS} ms of CPU over the usual frame (+${f2(pre.spike.cpu)} ms)`);
  check(pre.worst.cpu.over <= LIMIT_MS, `${gpu}: no other frame does either (worst +${f2(pre.worst.cpu.over)} ms CPU at frame ${pre.worst.cpu.frame})`);
  if (pre.noisy) note(`${gpu}: wall time not judged (a noisy run): the stamp's frame +${f2(pre.spike.wall)} ms, the worst other +${f2(pre.worst.wall.over)} ms`);
  else {
    check(pre.spike.wall <= LIMIT_MS, `${gpu}: and at most ${LIMIT_MS} ms of wall time (+${f2(pre.spike.wall)} ms)`);
    check(pre.worst.wall.over <= LIMIT_MS, `${gpu}: no other frame does either (worst +${f2(pre.worst.wall.over)} ms wall at frame ${pre.worst.wall.frame})`);
  }
  check(pre.farOk === 1, `${gpu}: the far mark (the atlas's mip 2) darkens the floor (far_ok ${pre.farOk})`);
  // not preloaded: the stamp's frame decodes the image (a 128 x 128 PNG) and uploads its cell, nothing more
  const lateRuns = [];
  for (let i = 0; i < 2; i++) lateRuns.push(await runRetry(gpu, `late${i ? i + 1 : ''}`, {}));
  const late = analyse(lateRuns);
  note(`${gpu} not preloaded (the least of ${late.runs} runs per frame): ${show(late)}`);
  check(late.spike.cpu <= DECODE_MS, `${gpu}: not preloaded, the stamp's frame costs its image's decode and no more: at most ${DECODE_MS} ms of CPU over the usual frame (+${f2(late.spike.cpu)} ms; the image ${f2(late.loadMs)} ms, its decode ${f2(late.decodeMs)} ms)`);
  check(late.farOk === 1, `${gpu}: not preloaded, the far mark darkens the floor too (far_ok ${late.farOk})`);

  const atLoad = await runRetry(gpu, 'at-load', { DECAL_AT_LOAD: '1' });
  const al = analyse([atLoad]);
  const d = diffImages(readImage(preRuns[0].shot), readImage(atLoad.shot));
  const d2 = diffImages(readImage(preRuns[0].shot), readImage(lateRuns[0].shot));
  note(`${gpu} at load: ${show(al)}; last frame against late: ${(d.gt2 * 100).toFixed(3)}% of pixels differ by more than 2, max ${d.max}`);
  check(d.comparable && d.gt2 < 0.001 && d.max <= 24, `${gpu}: stamped at the start or at tick ${STAMP_TICK}, the last frame is the same picture (${(d.gt2 * 100).toFixed(3)}% over 2, max ${d.max})`);
  check(d2.comparable && d2.gt2 < 0.001 && d2.max <= 24, `${gpu}: preloaded or decoded at the stamp, the same picture too (${(d2.gt2 * 100).toFixed(3)}% over 2, max ${d2.max})`);

  const ctl = analyse([await runRetry(gpu, 'control-full-mips', { CW_TEST_DECAL_FULL_MIPS: '1' })]);
  note(`${gpu} control (the whole atlas's mips at the first draw): ${show(ctl)}`);
  check(ctl.spike.cpu >= CONTROL_MS[gpu], `${gpu} CONTROL: the old path costs the stamp's frame at least ${CONTROL_MS[gpu]} ms more CPU (+${f2(ctl.spike.cpu)} ms CPU, +${f2(ctl.spike.wall)} ms wall)`);

  const nm = analyse([await runRetry(gpu, 'control-no-mips', { CW_TEST_DECAL_NO_MIPS: '1' })]);
  check(nm.farOk === 0, `${gpu} CONTROL: a new cell without its mips leaves the far mark off the floor (far_ok ${nm.farOk})`);
}
console.log(fail ? `decal-first-image: ${fail} failure(s)` : 'ok   decal-first-image');
process.exit(fail ? 1 : 0);
