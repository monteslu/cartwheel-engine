// GPU particle perf report (M3.4): fx_perf (100 000 particles at 1280x720,
// MODERN + bloom: depth collision, stretched sparks in curl noise, 20k alpha
// smoke sorted every frame, an attractor swarm) for 300 frames on each host.
// Reported: frame interval, CPU+GPU frame work (update start to a GPU sync
// after the frame), the pipeline's GPU ms and the particles pass's GPU ms.
// Checks: every host simulates exactly 100 000 alive particles; natively and
// in Chromium the frame work fits the 60 fps budget (16.7 ms). The control:
// the same budget check against an impossible budget (0.05 ms) must fail,
// or the measurement is not measuring anything.
//
//   node tests/fx-perf.mjs [--hosts native,chromium,romdev]
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
testEnv(process.env); // what it starts renders offscreen and cannot reach the desktop
process.env.CW_GPU_TIMERS = '1'; // the native runs' GPU ms column (the pipeline's timers are off unless asked for)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'perf');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const hosts = argv.includes('--hosts') ? argv[argv.indexOf('--hosts') + 1].split(',') : ['native', 'chromium', 'romdev'];
const FRAMES = 300, BUDGET = 1000 / 60, COUNT = 100000;

function run(host) {
  return new Promise((res) => {
    const state = join(OUT, `fx.${host}.json`);
    const p = spawn('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'fx_perf', '--frames', String(FRAMES),
      '--state', state, '--shot', join(OUT, `fx.${host}.png`)], { stdio: ['ignore', 'pipe', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => {
      if (code !== 0) return res({ host, error: err.trim().split('\n').slice(-2).join(' | ') });
      res({ host, state: JSON.parse(readFileSync(state, 'utf8')) });
    });
  });
}

const within = (s, budget) => s['perf.frames'] > 0 && s['perf.cpu_ms'] > 0 && s['perf.cpu_ms'] < budget;

const results = [];
for (const h of hosts) results.push(await run(h));
const pad = (s, n) => String(s).padEnd(n);
const fmt = (v) => (typeof v === 'number' ? v.toFixed(2) : '-');
const rows = [['alive', 'perf.alive'], ['draw calls', 'perf.draws'], ['sort passes', 'perf.sort_passes'],
  ['frame ms', 'perf.frame_ms'], ['frame max ms', 'perf.frame_max_ms'], ['cpu+gpu ms', 'perf.cpu_ms'],
  ['gpu ms', 'perf.gpu_ms'], ['particles gpu', 'perf.gpu_particles']];
const lines = [`fx_perf: ${COUNT} GPU particles, 1280x720, ${FRAMES} frames (averages after 60 warm-up frames)`,
  pad('', 15) + results.map((r) => pad(r.host, 12)).join('')];
for (const [label, key] of rows)
  lines.push(pad(label, 15) + results.map((r) => pad(r.error ? '(failed)' : key.includes('ms') || key.includes('gpu') ? fmt(r.state[key]) : r.state[key], 12)).join(''));
console.log(lines.join('\n'));
writeFileSync(join(OUT, 'fx-report.txt'), lines.join('\n') + '\n');

let fail = 0;
for (const r of results) {
  if (r.error) { if (r.host === 'native' || r.host === 'chromium') { fail++; console.log(`FAIL ${r.host}: ${r.error}`); } else console.log(`note ${r.host}: ${r.error}`); continue; }
  if (r.state['perf.alive'] !== COUNT || r.state['perf.capacity'] !== COUNT) { fail++; console.log(`FAIL ${r.host}: ${r.state['perf.alive']} alive of ${r.state['perf.capacity']}, expected ${COUNT}`); }
  if ((r.host === 'native' || r.host === 'chromium') && !within(r.state, BUDGET)) {
    fail++;
    console.log(`FAIL ${r.host}: frame work ${fmt(r.state['perf.cpu_ms'])} ms is over the 60 fps budget`);
  }
  if (r.host === 'native' && within(r.state, 0.05)) { fail++; console.log('FAIL CONTROL: an impossible budget passed: the timing measures nothing'); }
}
console.log(fail ? `fx-perf: ${fail} failures` : `ok   fx-perf (${COUNT} particles on every host; native and Chromium within 16.7 ms)`);
process.exit(fail ? 1 : 0);
