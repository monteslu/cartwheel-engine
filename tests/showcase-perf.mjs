// Showcase performance report: showcase_perf (the outdoor
// valley at 1920x1080, full quality: 193 lights, shadows, height fog,
// GTAO, TAA, bloom, SMAA) through its five time-of-day segments of 150 frames
// (morning, golden hour, dusk, night, the running cycle), natively.
// showcase_perf is GL_ONLY (it reads the frame back with GL itself), so it has
// no dual cart, and there is no GL-only cart: it has no romdev, browser or
// wasmcart-native legs.
//
// The app measures itself: frame ms is render
// end to render end with a 1-pixel read after each frame (the whole frame's
// CPU and GPU work, not a pipelined submission rate), averaged per segment
// after 30 settling frames, plus the worst frame; GPU ms per pass come from
// the pipeline's timer queries (16-frame averages).
//
// Checks: every segment averages under the budget
// (60 fps: 16.7 ms); the night segment has all 128 lamps on. Control: the same budget check against an impossible budget
// (0.05 ms) must fail, or the check cannot fail at all.
//
//   node tests/showcase-perf.mjs [--budget-ms 16.7]
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, writeFileSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
testEnv(process.env); // what it starts renders offscreen and cannot reach the desktop

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'perf');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes('--' + k) ? argv[argv.indexOf('--' + k) + 1] : d);
const HOSTS = opt('hosts', 'native').split(',');
if (HOSTS.some((h) => h !== 'native')) { console.error('showcase-perf: native only (showcase_perf is GL_ONLY: no cart)'); process.exit(2); }
const BUDGET = +opt('budget-ms', 1000 / 60);
const APP = 'showcase_perf', SEGS = 5, SEG = 150, FRAMES = SEGS * SEG;
const SEG_NAMES = ['morning', 'golden', 'dusk', 'night', 'cycle'];

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

const runners = { native: runNative };
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
