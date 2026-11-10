// The quality range gate: each app's GPU time at every graphics preset (cartwheel/quality.h), natively on GL and
// WebGPU at 1080p, from the pipeline's per-pass GPU timers (CW_GPU_TIMERS=1, the "pipeline" debug command after the
// last frame; each pass's 16-frame average, summed). Pass, per app and backend:
//   Very Low at most 0.3 x High, Low at most 0.45 x High (a scene's own shares where it has them),
//   each preset no dearer than the one above it, Ultra at least High (10% slack each: GPU timings vary run to run).
// The control runs the same apps with CW_TEST_QUALITY_FLAT=1 (every preset draws High's settings) and must fail.
// Runs inside the GPU suite (the machine's test GPU, never the desktop): node tools/with-gpu-suite.mjs quality-range
// node tools/quality-range.mjs ...
// --host romdev: the same range from the built cart (build/cart-wgpu/carts/<app>.wasc) through romdev's
// frame({op:'timing'}) on the server's pinned test GPU (tools/lib/romdev-timing.mjs), each preset a fresh
// deterministic load run to the same frame. The cost is romdev's GPU time of the last 60 frames before frame
// --frames, measured by the host around the cart's work (WebGPU: the union of every pass's timestamps; GL: a
// GL_TIME_ELAPSED query around each frame, as a cart never times GL passes), the same measure on both backends. It
// tracked the native pass sums within about 10% at every preset of night_street on the 890M (GL and WebGPU,
// 2026-11-10). The pipeline's own pass times (WebGPU) are kept in the report beside it (their 16-frame averages at one
// frame swing more than the host's 60-frame mean). The host's time includes the frame's fixed GPU costs (UI, the
// final copy) that the pass sums leave out and no preset cuts, so on romdev Very Low and Low may each be
// --romdev-slack (0.05) more of High than the scene's native share. The control asks every preset for High
// (`quality high`), what CW_TEST_QUALITY_FLAT does natively. The cart draws at its own size: --lines must be its height.
// A run romdev reports as noisy (contention.noisy: another process on the GPU, a playtest window) counts as a
// failed rep. Native runs use the GPU suite; romdev runs need no sandbox (the server is pinned).
//
//   node tools/quality-range.mjs [--host native|romdev] [--romdev-slack 0.05] [--apps open_world,terrain_demo] [--gpus gl,webgpu] [--frames 400] [--lines 1080]
//        [--project <a games tree built with this engine>] [--scenes <file.json>] [--json out/quality-range/report.json]
//        [--no-control] [--reps 2] [--max-very-low 0.3] [--max-low 0.45] [--slack 0.1]
// With neither --apps nor --scenes it runs tests/quality-range-engine.json.
//
// --scenes: [{ "name", "app", "frames", "env": {}, "startCmds": [], "input": "<json input script, project relative>",
// "maxVeryLow", "maxLow", "gpus" }] for apps that need input or commands to reach play (a games tree keeps its list there),
// each with its own shares of High where its floor is known (a scene bound by pixels a preset cannot cut has a higher one),
// and "gpus" (optional) the backends to measure it on: one whose timers cannot see the scene's cost (a 2D game's app pass is
// untimed on WebGPU) is left out.
import { spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync, existsSync, mkdirSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from './lib/gpu-sandbox.mjs';
import { wgpuPin, glPin, scrubDisplay } from './lib/wgpu-pin.mjs';
import { romdevTiming, scheduleFrom, timingScale, scaleMs, inBand } from './lib/romdev-timing.mjs';
const HOST = process.argv.includes('--host') ? process.argv[process.argv.indexOf('--host') + 1] : 'native';
if (!['native', 'romdev'].includes(HOST)) { console.error('--host native|romdev'); process.exit(2); }
if (HOST === 'native') await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes('--' + k) ? argv[argv.indexOf('--' + k) + 1] : d);
const PROJ = resolve(opt('project', ROOT));
const GPUS = opt('gpus', 'gl,webgpu').split(',');
const FRAMES = +opt('frames', '400'), LINES = +opt('lines', '1080'), REPS = +opt('reps', '2');
const MAX_VL_ARG = +opt('max-very-low', '0.3'), MAX_L_ARG = +opt('max-low', '0.45'), SLACK = +opt('slack', '0.1');
const ROMDEV_SLACK = HOST === 'romdev' ? +opt('romdev-slack', '0.05') : 0;
const PRESETS = ['verylow', 'low', 'medium', 'high', 'ultra'];
const OUT = join(PROJ, 'out', 'quality-range');
mkdirSync(OUT, { recursive: true });
const JSON_OUT = resolve(opt('json', join(OUT, 'report.json')));
// the engine's list: samples whose High costs the 890M 1.5 ms or more, each with the share of High its Very Low and Low
// measured (plus a margin); the scenes bound by pixels a preset cannot cut (terrain, open ground) have high ones. The
// first is the control's.
const DEFAULT_SCENES = join(ROOT, 'tests/quality-range-engine.json');
let scenes = opt('apps') && !opt('scenes') ? opt('apps').split(',').map((a) => ({ name: a, app: a })) : JSON.parse(readFileSync(resolve(opt('scenes', DEFAULT_SCENES)), 'utf8'));
if (opt('apps') && opt('scenes')) scenes = scenes.filter((s) => opt('apps').split(',').includes(s.name));

const CW_BITS = { south: 0, east: 1, west: 2, north: 3, back: 4, guide: 5, start: 6, l3: 7, r3: 8, l: 9, r: 10, up: 11, down: 12, left: 13, right: 14 };
const inputFile = (script, f) => {
  writeFileSync(f, script.map((e) => [e.frame - 1, (e.buttons || []).reduce((m, n) => m | (1 << CW_BITS[n]), 0), ...['lx', 'ly', 'rx', 'ry', 'lt', 'rt'].map((a) => (e.axes || {})[a] || 0)].join(' ')).join('\n') + '\n');
  return f;
};
const exeOf = (app) => {
  const j = join(PROJ, 'build/native/apps', `${app}.json`);
  if (existsSync(j)) return JSON.parse(readFileSync(j, 'utf8')).binary;
  return join(PROJ, 'build/native/samples', app);
};
const lh = {}; // each app's logical height (cw.view), for the output size

/** one run: the pipeline's GPU ms (16-frame averages at the last frame), the best of REPS */
function measure(sc, gpu, preset, flat) {
  const exe = exeOf(sc.app);
  const tag = `${sc.name}-${gpu}-${preset}${flat ? '-flat' : ''}`;
  const state = join(OUT, `${tag}.json`), shot = join(OUT, `${tag}.png`);
  const env = { ...process.env, SDL_AUDIODRIVER: 'dummy', CW_GPU_TIMERS: '1', CW_QUALITY: preset, CW_FRAMES: String(sc.frames || FRAMES), CW_FRAME_HZ: '60',
    CW_WARMUP_SPREAD: '0', CW_DEBUG_CMDS: 'pipeline;quality', CW_DEBUG_DUMP: state, CW_SCREENSHOT: shot, CW_ASSETS: exe + '.assets', CW_PROJECT_ROOT: PROJ,
    CW_WGPU_VALIDATION: '0', CW_HOST_PREF: `${Math.round(LINES * 16 / 9)}x${LINES}`, ...(sc.env || {}) };
  if (flat) env.CW_TEST_QUALITY_FLAT = '1';
  else delete env.CW_TEST_QUALITY_FLAT;
  if (lh[sc.name]) { env.CW_RENDER_SCALE = String(LINES / lh[sc.name]); delete env.CW_HOST_PREF; }
  if (sc.startCmds) env.CW_START_CMDS = sc.startCmds.join(';');
  if (sc.input) env.CW_INPUT = inputFile(JSON.parse(readFileSync(join(PROJ, sc.input), 'utf8')), join(OUT, `${tag}.input`));
  if (gpu === 'webgpu') Object.assign(env, wgpuPin().env, { CW_GPU: 'webgpu' });
  else {
    env.CW_GPU = 'gl';
    const g = glPin();
    if (g) Object.assign(env, g.env);
  }
  scrubDisplay(env);
  let best = null;
  for (let rep = 0; rep < REPS; rep++) {
    rmSync(state, { force: true });
    const r = spawnSync(exe, [], { env, encoding: 'utf8', maxBuffer: 1 << 26, timeout: 600e3 });
    const replies = (r.stdout || '').split('\n').filter((l) => l.startsWith('{')).map((l) => { try { return JSON.parse(l); } catch { return null; } });
    const pipe = replies.find((j) => j && j.passes), q = replies.find((j) => j && j.quality);
    if (r.status !== 0 || !pipe) return { error: `exit ${r.status ?? r.signal}: ${(r.stderr || '').trim().split('\n').slice(-3).join(' | ')}` };
    const s = existsSync(state) ? JSON.parse(readFileSync(state, 'utf8')) : {};
    if (s['cw.view'] && !lh[sc.name]) lh[sc.name] = s['cw.view'][1];
    /* the frame's cost: every pass's 16-frame average summed (the pipeline's own total is the last frame's, which a pass that
     * draws every few frames, a probe capture or a sky that keeps changing, makes jump by whether that frame had it) */
    const avg = pipe.passes.filter((p) => p.ran && !p.fused && p.gpu_ms > 0).reduce((t, p) => t + p.gpu_ms, 0);
    const run = { ms: avg, passes: Object.fromEntries(pipe.passes.filter((p) => p.ran && p.gpu_ms > 0).map((p) => [p.name, +p.gpu_ms.toFixed(3)])),
      preset: q && q.preset, view: s['cw.view'], skipped: s['render.skipped'] || 0, culled: s['shadow.culled'] };
    if (!best || run.ms < best.ms) best = run;
  }
  return best;
}

/** one run through romdev: a fresh deterministic load, the preset, the pipeline's timers, and the last 60 frames
 *  before frame FRAMES timed; the best of REPS */
const romdevSized = {};
async function measureRomdev(sc, gpu, preset, flat) {
  const cart = join(PROJ, 'build/cart-wgpu/carts', `${sc.app}.wasc`);
  if (!existsSync(cart)) return { error: `no cart ${cart} (build the cart tree)` };
  const frames = sc.frames || FRAMES;
  const env = Object.entries(sc.env || {});
  const commands = [...(env.length ? [`env ${env.map(([k, v]) => `${k}=${v}`).join(' ')}`] : []), 'test_run', `quality ${flat ? 'high' : preset}`, 'pipeline', ...(sc.startCmds || [])];
  const at = 1 + commands.length; // the load runs frame 1, each request one more
  const MEASURED = 60;
  const warmup = frames - at - MEASURED;
  if (warmup < 16) return { error: `--frames ${frames} leaves no warmup after ${at} setup frames` };
  // a cart that draws another size than --lines (a 720p sample) is loaded with no seed and the host's preferred size,
  // as natively it gets CW_RENDER_SCALE: a deterministic run keeps its own size
  // (one with a fixed cw_app_desc.render_scale ignores a host's preference: it is measured at its own size, and says so;
  // its presets must still order by cost, its shares of High are not checked)
  const sized = romdevSized[sc.name] === 'pref' ? { seed: null, preferredSize: { width: Math.round(LINES * 16 / 9), height: LINES } } : {};
  let best = null, lastErr = null, noisy = 0;
  for (let rep = 0; rep < REPS; rep++) {
    let t;
    if (noisy) await new Promise((r) => setTimeout(r, 5000)); // a noisy run is run again, after a pause
    try {
      t = await romdevTiming({ cart, gpu, commands, ...sized, warmup, frames: MEASURED, window: MEASURED, passTimers: true, debugFields: ['render.skipped', 'shadow.culled'],
        ...(sc.input ? { input: scheduleFrom(JSON.parse(readFileSync(join(PROJ, sc.input), 'utf8')), at) } : {}) });
    } catch (e) { lastErr = e.message; continue; }
    if (t.size && t.size.height !== LINES && !romdevSized[sc.name]) {
      romdevSized[sc.name] = 'pref';
      return measureRomdev(sc, gpu, preset, flat);
    }
    if (t.size && t.size.height !== LINES && romdevSized[sc.name] === 'pref') {
      romdevSized[sc.name] = 'own';
      console.log(`note ${sc.name}: the cart draws ${t.size.width}x${t.size.height} whatever the host prefers (a fixed render scale): measured at that size`);
      return measureRomdev(sc, gpu, preset, flat);
    }
    if (t.contention && t.contention.noisy) {
      lastErr = `noisy: ${t.contention.reasons.join('; ')}`;
      if (noisy++ < 5) rep--; // up to five more tries for noise, beside the reps
      continue;
    }
    const pt = (t.passTimers || []).at(-1) || {};
    const passes = Object.fromEntries(Object.entries(pt.passes || {}).filter(([, p]) => p.ran && p.gpuMs > 0).map(([n, p]) => [n, +p.gpuMs.toFixed(3)]));
    const ms = t.gpuMs && t.gpuMs.mean;
    if (!(ms > 0)) { lastErr = `no GPU time: ${JSON.stringify(t.gpuMs)}`; continue; }
    const run = { ms, passes, passSumMs: pt.totalGpuMs, preset: (t.setup || []).map((x) => x.reply && x.reply.preset).find(Boolean),
      skipped: (t.fields && t.fields['render.skipped'] && t.fields['render.skipped'].max) || 0, clockMhz: t.gpu && t.gpu.clockMhz && t.gpu.clockMhz.mean,
      gpu: t.gpuSeen, size: t.size, ...(romdevSized[sc.name] === 'pref' ? { seeded: false } : {}),
      // a timing scale set for this backend (tools/lib/romdev-timing.mjs): the reference-GPU equivalent beside the raw ms
      ...(t.timingScale ? { scaledMs: t.scaledMs ? t.scaledMs.mean : null, timingScale: t.timingScale, ...(t.scaleNote ? { scaleNote: t.scaleNote } : {}) } : {}) };
    if (!best || run.ms < best.ms) best = run;
  }
  return best || { error: lastErr || 'no run' };
}

/** the checks on one app and backend; [] when it passes */
function judge(r, sc) {
  const bad = [], H = r.high.ms;
  const MAX_VL = (sc.maxVeryLow ?? MAX_VL_ARG) + ROMDEV_SLACK, MAX_L = (sc.maxLow ?? MAX_L_ARG) + ROMDEV_SLACK;
  if (!(H > 0)) return ['no GPU time at High'];
  // a cart measured at its own size (romdev, a fixed render scale) has no share of High at --lines to hold: its
  // presets must still order by cost
  const ownSize = r.high.size && r.high.size.height !== LINES;
  if (ownSize) bad.note = `shares not checked: the cart draws ${r.high.size.width}x${r.high.size.height} on romdev, its shares are for ${LINES} lines`;
  if (!ownSize && r.verylow.ms > MAX_VL * H) bad.push(`Very Low ${r.verylow.ms.toFixed(2)} ms is ${(r.verylow.ms / H).toFixed(2)} x High (at most ${MAX_VL})`);
  if (!ownSize && r.low.ms > MAX_L * H) bad.push(`Low ${r.low.ms.toFixed(2)} ms is ${(r.low.ms / H).toFixed(2)} x High (at most ${MAX_L})`);
  for (let i = 0; i + 1 < PRESETS.length - 1; i++) {
    const a = r[PRESETS[i]], b = r[PRESETS[i + 1]];
    if (a.ms > b.ms * (1 + SLACK)) bad.push(`${PRESETS[i]} ${a.ms.toFixed(2)} ms is dearer than ${PRESETS[i + 1]} ${b.ms.toFixed(2)} ms`);
  }
  if (r.ultra.ms < H * (1 - SLACK)) bad.push(`Ultra ${r.ultra.ms.toFixed(2)} ms is under High ${H.toFixed(2)} ms`);
  return bad;
}

async function sweep(flat) {
  const report = {};
  let failures = 0, errors = 0; // errors: a run that could not measure (a control failing that way proves nothing)
  for (const sc of scenes)
    for (const gpu of GPUS.filter((g) => !sc.gpus || sc.gpus.includes(g))) {
      const r = {};
      let err = null;
      for (const p of PRESETS) {
        r[p] = HOST === 'romdev' ? await measureRomdev(sc, gpu, p, flat) : measure(sc, gpu, p, flat);
        if (r[p].error) { err = `${p}: ${r[p].error}`; break; }
      }
      const key = `${sc.name}/${gpu}`;
      if (err) {
        failures++;
        errors++;
        report[key] = { error: err };
        console.log(`FAIL ${key}${flat ? ' (control)' : ''}: ${err}`);
        continue;
      }
      const bad = judge(r, sc);
      report[key] = { ...Object.fromEntries(PRESETS.map((p) => [p, r[p]])), bad };
      if (bad.length) failures++;
      const row = PRESETS.map((p) => `${p} ${r[p].ms.toFixed(2)}`).join('  ');
      // with a timing scale for the backend (romdev runs on another test GPU): the reference-GPU row beside the raw one
      // ('-' where a preset's time lies outside the fit's band); the shares and the order are judged on the raw ms
      const sc_ = HOST === 'romdev' ? timingScale(gpu) : null;
      const scaled = sc_ ? `\n       scaled to the reference GPU (from ${sc_.gpuName}, ${sc_.residual === null ? 'residual unknown' : 'residual ' + (sc_.residual * 100).toFixed(0) + '%'}${sc_.unreliable ? ', UNRELIABLE' : ''}${sc_.validMs ? ', - outside its band of ' + sc_.validMs.join(' to ') + ' ms' : ''}): ${PRESETS.map((p) => { const v = scaleMs(r[p].ms, sc_); return `${p} ${inBand(v, sc_) ? v.toFixed(2) : '-'}`; }).join('  ')} ms` : '';
      console.log(`${bad.length ? 'FAIL' : 'ok  '} ${key}${flat ? ' (control)' : ''}: ${row} ms (VL ${(r.verylow.ms / r.high.ms).toFixed(2)}, L ${(r.low.ms / r.high.ms).toFixed(2)} x High)${scaled}${bad.length ? '\n       ' + bad.join('\n       ') : ''}${bad.note ? '\n       (' + bad.note + ')' : ''}`);
    }
  return { report, failures, errors };
}

const main = await sweep(false);
let control = null;
if (!argv.includes('--no-control')) {
  // the control: one app per backend is enough to show the gate can fail (every preset drawing High is flat)
  const keep = scenes;
  // (on romdev, the first whose cart draws --lines: a cart measured at its own size has no shares to fail)
  scenes = scenes.filter((sc) => romdevSized[sc.name] !== 'own').slice(0, 1);
  control = await sweep(true);
  scenes = keep;
}
writeFileSync(JSON_OUT, JSON.stringify({ host: HOST, lines: LINES, frames: FRAMES, main: main.report, control: control && control.report }, null, 1));
let fail = main.failures;
if (control && control.errors) {
  fail++;
  console.log('CONTROL COULD NOT RUN: a control that fails by not measuring proves nothing');
} else if (control && control.failures === 0) {
  fail++;
  console.log('CONTROL PASSED: with every preset drawing High the gate still passed; it cannot see a flat range');
} else if (control) console.log('ok   control: every preset drawing High fails the range');
console.log(fail ? `quality-range: ${main.failures} failing app/backend pair(s)${control && control.failures === 0 ? ', and the control passed' : ''} (${JSON_OUT})` : `ok   quality-range (${JSON_OUT})`);
process.exit(fail ? 1 : 0);
