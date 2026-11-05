// The quality range gate: each app's GPU time at every graphics preset (cartwheel/quality.h), natively on GL and
// WebGPU at 1080p, from the pipeline's per-pass GPU timers (CW_GPU_TIMERS=1, the "pipeline" debug command after the
// last frame). Pass, per app and backend:
//   Very Low at most 0.3 x High, Low at most 0.45 x High,
//   each preset no dearer than the one above it (5% slack), Ultra at least High (5% slack).
// The control runs the same apps with CW_TEST_QUALITY_FLAT=1 (every preset draws High's settings) and must fail.
// Runs inside the GPU suite (the machine's test GPU, never the desktop): node tools/with-gpu-suite.mjs quality-range
// node tools/quality-range.mjs ...
//
//   node tools/quality-range.mjs [--apps open_world,terrain_demo] [--gpus gl,webgpu] [--frames 400] [--lines 1080]
//        [--project <a games tree built with this engine>] [--scenes <file.json>] [--json out/quality-range/report.json]
//        [--no-control] [--reps 2] [--max-very-low 0.3] [--max-low 0.45]
//
// --scenes: [{ "name", "app", "frames", "env": {}, "startCmds": [], "input": "<json input script, project relative>",
// "maxVeryLow", "maxLow" }] for apps that need input or commands to reach play (a games tree keeps its list there), each
// with its own shares of High where its floor is known (a scene bound by pixels a preset cannot cut has a higher one).
import { spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync, existsSync, mkdirSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from './lib/gpu-sandbox.mjs';
import { wgpuPin, glPin, scrubDisplay } from './lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes('--' + k) ? argv[argv.indexOf('--' + k) + 1] : d);
const PROJ = resolve(opt('project', ROOT));
const GPUS = opt('gpus', 'gl,webgpu').split(',');
const FRAMES = +opt('frames', '400'), LINES = +opt('lines', '1080'), REPS = +opt('reps', '2');
const MAX_VL_ARG = +opt('max-very-low', '0.3'), MAX_L_ARG = +opt('max-low', '0.45');
const PRESETS = ['verylow', 'low', 'medium', 'high', 'ultra'];
const OUT = join(PROJ, 'out', 'quality-range');
mkdirSync(OUT, { recursive: true });
const JSON_OUT = resolve(opt('json', join(OUT, 'report.json')));
const DEFAULT_APPS = 'open_world,terrain_demo,night_street,lightmap_yard,fx_explosion,platformer3d_auto';
let scenes = opt('scenes') ? JSON.parse(readFileSync(resolve(opt('scenes')), 'utf8')) : opt('apps', DEFAULT_APPS).split(',').map((a) => ({ name: a, app: a }));
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
    const run = { ms: pipe.gpu_ms, passes: Object.fromEntries(pipe.passes.filter((p) => p.ran && p.gpu_ms > 0).map((p) => [p.name, +p.gpu_ms.toFixed(3)])),
      preset: q && q.preset, view: s['cw.view'], skipped: s['render.skipped'] || 0, culled: s['shadow.culled'] };
    if (!best || run.ms < best.ms) best = run;
  }
  return best;
}

/** the checks on one app and backend; [] when it passes */
function judge(r, sc) {
  const bad = [], H = r.high.ms;
  const MAX_VL = sc.maxVeryLow ?? MAX_VL_ARG, MAX_L = sc.maxLow ?? MAX_L_ARG;
  if (!(H > 0)) return ['no GPU time at High'];
  if (r.verylow.ms > MAX_VL * H) bad.push(`Very Low ${r.verylow.ms.toFixed(2)} ms is ${(r.verylow.ms / H).toFixed(2)} x High (at most ${MAX_VL})`);
  if (r.low.ms > MAX_L * H) bad.push(`Low ${r.low.ms.toFixed(2)} ms is ${(r.low.ms / H).toFixed(2)} x High (at most ${MAX_L})`);
  for (let i = 0; i + 1 < PRESETS.length - 1; i++) {
    const a = r[PRESETS[i]], b = r[PRESETS[i + 1]];
    if (a.ms > b.ms * 1.05) bad.push(`${PRESETS[i]} ${a.ms.toFixed(2)} ms is dearer than ${PRESETS[i + 1]} ${b.ms.toFixed(2)} ms`);
  }
  if (r.ultra.ms < H * 0.95) bad.push(`Ultra ${r.ultra.ms.toFixed(2)} ms is under High ${H.toFixed(2)} ms`);
  return bad;
}

function sweep(flat) {
  const report = {};
  let failures = 0;
  for (const sc of scenes)
    for (const gpu of GPUS) {
      const r = {};
      let err = null;
      for (const p of PRESETS) {
        r[p] = measure(sc, gpu, p, flat);
        if (r[p].error) { err = `${p}: ${r[p].error}`; break; }
      }
      const key = `${sc.name}/${gpu}`;
      if (err) {
        failures++;
        report[key] = { error: err };
        console.log(`FAIL ${key}${flat ? ' (control)' : ''}: ${err}`);
        continue;
      }
      const bad = judge(r, sc);
      report[key] = { ...Object.fromEntries(PRESETS.map((p) => [p, r[p]])), bad };
      if (bad.length) failures++;
      const row = PRESETS.map((p) => `${p} ${r[p].ms.toFixed(2)}`).join('  ');
      console.log(`${bad.length ? 'FAIL' : 'ok  '} ${key}${flat ? ' (control)' : ''}: ${row} ms (VL ${(r.verylow.ms / r.high.ms).toFixed(2)}, L ${(r.low.ms / r.high.ms).toFixed(2)} x High)${bad.length ? '\n       ' + bad.join('\n       ') : ''}`);
    }
  return { report, failures };
}

const main = sweep(false);
let control = null;
if (!argv.includes('--no-control')) {
  // the control: one app per backend is enough to show the gate can fail (every preset drawing High is flat)
  const keep = scenes;
  scenes = scenes.slice(0, 1);
  control = sweep(true);
  scenes = keep;
}
writeFileSync(JSON_OUT, JSON.stringify({ lines: LINES, frames: FRAMES, main: main.report, control: control && control.report }, null, 1));
let fail = main.failures;
if (control && control.failures === 0) {
  fail++;
  console.log('CONTROL PASSED: with every preset drawing High the gate still passed; it cannot see a flat range');
} else if (control) console.log('ok   control: every preset drawing High fails the range');
console.log(fail ? `quality-range: ${main.failures} failing app/backend pair(s)${control && control.failures === 0 ? ', and the control passed' : ''} (${JSON_OUT})` : `ok   quality-range (${JSON_OUT})`);
process.exit(fail ? 1 : 0);
