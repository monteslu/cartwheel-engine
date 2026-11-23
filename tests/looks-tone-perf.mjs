// What the dither's tone options cost (cartwheel/look.h, src/render/looks.c): GPU ms of the looks_display pass
// (the look at the DISPLAY stage, every tone option on: hue, grey, the unsharp mask at 0.9, outlines, gamma,
// contrast) in the looks_onebit sample at 1920 x 1080, natively on GL and WebGPU. Before: the tone worked out in
// every screen pixel (CW_TEST_LOOK_TONE_EACH_PIXEL=1, the look to v0.65.1: a dither pixel of n x n screen pixels
// took its mean colour n^2 times and the unsharp mask nine times that). After: once a dither pixel in small
// targets. Each is measured with the mask and without it (sharpen 0, the other options on), so the mask's own
// cost is the difference. The rounds are interleaved (before, after, before...) and each figure is the median of
// the rounds, with the machine's load and the GPU's clock beside it (docs/guides/profiling.md). Build the sample
// as a release (scripts/build.sh, no DEV).
//
// Bound: at 2 and 6 dither pixels (1080 line) the mask costs no more than a quarter of what it did and the pass no
// more than it did. One dither pixel a screen pixel keeps the tone in the screen pixels (nothing to share; the small
// targets cost more there), so its two columns are the same program: within 15% or 0.05 ms of each other.
//
//   node tools/with-gpu-suite.mjs looks-tone-perf node tests/looks-tone-perf.mjs [--rounds 5] [--frames 240] [--gpus gl,webgpu]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { loadavg } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { gpuClock, clockLine } from '../tools/lib/gpu-clock.mjs';
await enterGpuSandbox();

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'looks-tone-perf');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : d);
const rounds = +opt('--rounds', 5), frames = opt('--frames', '240'), GPUS = opt('--gpus', 'gl,webgpu').split(',');
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.floor(s.length / 2)] : NaN; };
const CASES = [
  ['1 (output)', {}],
  ['2 (1080, Bayer)', { ONEBIT_PIXEL: '2', ONEBIT_OUTPX: '0', ONEBIT_PATTERN: 'bayer' }],
  ['6 (1080)', { ONEBIT_PIXEL: '6', ONEBIT_OUTPX: '0' }],
];

function run(gpu, env, tag) {
  const state = join(OUT, `${gpu}.${tag}.json`);
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'looks_onebit', '--frames', frames,
    '--env', JSON.stringify({ ONEBIT: 'look', ...env }), '--state', state], { encoding: 'utf8', env: { ...process.env, CW_GPU_TIMERS: '1' } });
  if (r.status !== 0) return { err: (r.stderr || r.stdout).trim().split('\n').slice(-2).join(' | ') };
  const st = JSON.parse(readFileSync(state, 'utf8'));
  const s = st.state || st;
  return { ms: s['onebit.gpu_ms'], small: s['look.tone_small'] };
}

const clk = gpuClock();
console.log(`looks_onebit, 1920 x 1080, GPU ms of the looks_display pass (median of ${rounds} interleaved rounds); load ${loadavg().map((v) => v.toFixed(1)).join(' ')}`);
console.log('gpu     pixel              before   after   | mask before   mask after');
let fails = 0;
for (const gpu of GPUS) for (const [label, env] of CASES) {
  const V = { before: [], after: [], beforeNo: [], afterNo: [] };
  let bad = false;
  for (let i = 0; i < rounds && !bad; i++) {
    for (const [k, e] of [['before', { CW_TEST_LOOK_TONE_EACH_PIXEL: '1' }], ['after', {}], ['beforeNo', { CW_TEST_LOOK_TONE_EACH_PIXEL: '1', ONEBIT_SHARPEN: '0' }], ['afterNo', { ONEBIT_SHARPEN: '0' }]]) {
      const r = run(gpu, { ...env, ...e }, `${label.split(' ')[0]}.${k}`);
      if (r.err || !(r.ms > 0)) { console.log(`FAIL ${gpu} ${label} ${k}: ${r.err || `no GPU time (${r.ms})`}`); fails++; bad = true; break; }
      if (r.small !== (k.startsWith('before') || !env.ONEBIT_PIXEL ? 0 : 1)) { console.log(`FAIL ${gpu} ${label} ${k}: small targets ${r.small}`); fails++; bad = true; break; }
      V[k].push(r.ms);
    }
  }
  if (bad) continue;
  const b = median(V.before), a = median(V.after), mb = b - median(V.beforeNo), ma = a - median(V.afterNo);
  const one = env.ONEBIT_PIXEL === undefined;
  const ok = one ? a <= Math.max(b * 1.15, b + 0.05) : a <= b && ma <= Math.max(mb * 0.25, 0.02);
  if (!ok) fails++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${gpu.padEnd(7)} ${label.padEnd(16)} ${b.toFixed(3).padStart(7)} ${a.toFixed(3).padStart(7)}   | ${mb.toFixed(3).padStart(8)} ${ma.toFixed(3).padStart(11)}   (rounds before/after: ${V.before.map((v, i) => `${v.toFixed(3)}/${V.after[i].toFixed(3)}`).join(' ')})`);
}
console.log(clockLine(await clk.stop()));
console.log(`load at the end ${loadavg().map((v) => v.toFixed(1)).join(' ')}`);
console.log(fails ? `FAIL looks-tone-perf (${fails})` : 'ok   looks-tone-perf');
process.exit(fails ? 1 : 0);
