// Explosion and water GPU cost: fx_boom_water at every quality preset, with one view and with four
// (split screen), on GL and WebGPU, with the whole explosion, without its distortion, and without
// the explosion at all (the water-only baseline). Reported: the pipeline's GPU milliseconds per frame
// (the passes' own GPU timers, averaged over the last frames: the numbers do not depend on how busy the
// CPU is) and the passes the explosion work adds. Written to out/perf/boom-report.txt.
// Checks: every run reached the scene; GPU ms stays under a loose ceiling (20 ms: this is a report,
// not a tight gate); the control, an impossible 0.05 ms budget, must fail.
//
//   node tests/boom-perf.mjs [--presets low,high] [--views 1,4] [--gpus gl,webgpu] [--frames 240]
import { spawnSync } from 'node:child_process';
import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
testEnv(process.env);
process.env.CW_GPU_TIMERS = '1';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'perf');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1].split(',') : d);
const presets = opt('--presets', ['low', 'medium', 'high', 'ultra']);
const views = opt('--views', ['1', '4']);
const gpus = opt('--gpus', ['gl', 'webgpu']);
const FRAMES = +opt('--frames', ['240'])[0];
const CEILING = 20, PASS = ['scene', 'particles', 'distort_sources', 'distort', 'bloom', 'tonemap', 'ssao', 'aa', 'height_fog'];

function run(gpu, preset, nv, fx) {
  const tag = `${gpu}.${preset}.${nv}.${fx}`;
  const state = join(OUT, `boom.${tag}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--app', 'fx_boom_water', '--frames', String(FRAMES),
    '--gpu', gpu, '--state', state, '--shot', join(OUT, `boom.${tag}.png`)],
  { env: { ...process.env, BOOM_PRESET: preset, BOOM_VIEWS: nv, BOOM_FX: fx }, encoding: 'utf8' });
  if (r.status !== 0) return { tag, error: (r.stderr || '').trim().split('\n').slice(-2).join(' | ') };
  const s = JSON.parse(readFileSync(state, 'utf8'));
  return { tag, total: s['boom.gpu_total'], pass: s['boom.pass_ms'], alive: s['boom.alive'], tick: s['boom.tick'] };
}
const f = (v) => (typeof v === 'number' ? v.toFixed(2) : '-');
const lines = [`fx_boom_water, 1920x1080, ${FRAMES} frames: GPU ms per frame (pipeline timers), 890M`,
  `uptime: ${readFileSync('/proc/loadavg', 'utf8').trim()}`, '',
  'gpu     preset  views | water only | + explosion | - distortion | explosion adds | distort passes | particles pass'];
let fail = 0;
const rows = [];
for (const gpu of gpus) for (const preset of presets) for (const nv of views) {
  const base = run(gpu, preset, nv, 'none'), full = run(gpu, preset, nv, 'all'), nod = run(gpu, preset, nv, 'nodistort');
  for (const r of [base, full, nod]) if (r.error) { fail++; console.log(`FAIL ${r.tag}: ${r.error}`); }
  if (full.error || base.error || nod.error) continue;
  const dp = (r, n) => r.pass[PASS.indexOf(n)];
  const row = { gpu, preset, nv, base: base.total, full: full.total, nod: nod.total, pass: full.pass };
  rows.push(row);
  lines.push(`${gpu.padEnd(7)} ${preset.padEnd(7)} ${nv.padEnd(5)} | ${f(base.total).padStart(11)} | ${f(full.total).padStart(11)} | ${f(nod.total).padStart(12)} | ${f(full.total - base.total).padStart(14)} | ${f(dp(full, 'distort_sources') + dp(full, 'distort')).padStart(14)} | ${f(dp(full, 'particles')).padStart(15)}`);
  console.log(lines[lines.length - 1]);
  if (!(full.total > 0) || full.alive < 100) { fail++; console.log(`FAIL ${full.tag}: no GPU time or the explosion is not alive (${full.alive})`); }
  if (full.total > CEILING) { fail++; console.log(`FAIL ${full.tag}: ${f(full.total)} ms is over the ${CEILING} ms ceiling`); }
  if (full.total < 0.05) { fail++; console.log(`FAIL CONTROL ${full.tag}: an impossible 0.05 ms budget passed: the timing measures nothing`); }
}
lines.push('', 'per pass, whole explosion (ms): ' + PASS.join(', '));
for (const r of rows) lines.push(`${r.gpu} ${r.preset} x${r.nv}: ${r.pass.map(f).join(', ')}`);
writeFileSync(join(OUT, 'boom-report.txt'), lines.join('\n') + '\n');
console.log(fail ? `boom-perf: ${fail} failures` : `ok   boom-perf (${rows.length} configurations, report in out/perf/boom-report.txt)`);
process.exit(fail ? 1 : 0);
