// Phase 0 spikes: GPU ms per frame of an FFT ocean (3 x 256^2), a 512^2 ripple grid, a 64x96x64 fire
// step and its half-resolution ray march, and a 20 000 particle 3D MLS-MPM step, on WebGPU (the
// compute techniques are WebGPU only), measured with the pipeline's timestamp queries on the test GPU.
// Written to out/perf/spikes-report.txt. Checks: no WebGPU error, every spike measured and under a
// loose ceiling (10 ms each); control: an impossible 0.001 ms budget must fail.
//   node tests/spikes.mjs
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
const state = join(OUT, 'spikes.json');
const r = spawnSync('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--app', 'fx_spikes', '--frames', '240', '--gpu', 'webgpu', '--state', state],
  { env: process.env, encoding: 'utf8' });
if (r.status !== 0) { console.log(`FAIL run: ${(r.stderr || '').trim().split('\n').slice(-2).join(' | ')}`); process.exit(1); }
const s = JSON.parse(readFileSync(state, 'utf8'));
const names = [['fft', '3 cascades of 256x256 FFT ocean'], ['ripple', '512x512 height-field ripples, 2 steps'], ['fire_step', '64x96x64 fire and smoke step'],
  ['fire_march', 'that grid marched at 960x540, 64 steps, about a third of the frame'], ['mpm', '20 000 particle 3D MLS-MPM step']];
const ms = s['spike.ms'];
const lines = [`fx_spikes: GPU ms per frame on WebGPU (timestamp queries), uptime ${readFileSync('/proc/loadavg', 'utf8').trim()}`, ''];
let fail = 0;
names.forEach(([k, label], i) => {
  const d = s[`spike.dispatches.${k}`];
  lines.push(`${k.padEnd(11)} ${ms[i].toFixed(3).padStart(7)} ms  ${String(d).padStart(2)} dispatches  ${label}`);
  if (!(ms[i] > 0)) { fail++; lines.push(`FAIL ${k}: not measured`); }
  if (ms[i] > 10) { fail++; lines.push(`FAIL ${k}: over the 10 ms ceiling`); }
  if (ms[i] > 0 && ms[i] < 0.001) { fail++; lines.push(`FAIL CONTROL ${k}: an impossible 0.001 ms budget passed`); }
});
if (s['cw.gpu.errors']) { fail++; lines.push(`FAIL ${s['cw.gpu.errors']} WebGPU errors`); }
writeFileSync(join(OUT, 'spikes-report.txt'), lines.join('\n') + '\n');
console.log(lines.join('\n'));
console.log(fail ? `spikes: ${fail} failures` : 'ok   spikes');
process.exit(fail ? 1 : 0);
