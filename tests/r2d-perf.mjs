// The 2D renderer's call budget and timings: samples/r2d_stress case by case natively on GL and WebGPU at 1080p
// (tools/r2d-perf.mjs). Fails when a case's WebGPU calls go over its budget (the weak-host budget: about 8000 WebGPU
// calls a frame for a whole game on a Vega 8 libretro box, so the 2D renderer's share stays a few hundred however many
// sprites or lights), or a lit case draws more than its few draws. Timings are reported, never compared.
// Control: the same check against budgets of one call must fail.
import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const CASES = 'sprites100k,lit500,shadow500,soft64,volumetric16,tiles,text,post,inst200k,instkept200k,sidescroll';
const run = (extra) =>
  spawnSync('node', [join(ROOT, 'tools/r2d-perf.mjs'), '--sizes', '1920x1080', '--frames', '30', ...extra], {
    encoding: 'utf8', timeout: 900000,
  });
const r = run(['--cases', CASES, '--check', '--json', join(ROOT, 'out/r2d-perf/gate.json')]);
process.stdout.write(r.stdout);
let ok = r.status === 0;
// lit runs: a fixed handful of draws whatever the light count
const rows = JSON.parse(readFileSync(join(ROOT, 'out/r2d-perf/gate.json'), 'utf8'));
for (const row of rows) {
  if (row.failed) continue;
  const max = row.case === 'lit500' || row.case.startsWith('inst') ? (row.case === 'lit500' ? 3 : 1) : row.case.startsWith('shadow') || row.case === 'soft64' || row.case === 'volumetric16' ? 16 : 0;
  if (max && row.draws > max) { console.log(`FAIL ${row.case} ${row.gpu}: ${row.draws} draws (at most ${max})`); ok = false; }
}
console.log(ok ? 'ok   r2d-perf: every case within its WebGPU call budget' : 'FAIL r2d-perf');
const c = run(['--control', '--cases', 'lit500', '--gpus', 'webgpu', '--json', join(ROOT, 'out/r2d-perf/control.json')]);
const controlOk = c.status === 0 && /control failed as it must/.test(c.stdout);
console.log(controlOk ? 'ok   control: budgets of one call fail' : 'CONTROL PASSED');
process.exit(ok && controlOk ? 0 : 1);
