// What foliage costs (cartwheel/foliage.h, samples/foliage_field): GPU ms of a 1920 x 1080 frame of a dense field, on
// GL and WebGPU, natively on the test GPU, with the option against without it, rounds interleaved, each line the median
// of the rounds with the machine's load and the GPU clock beside it. A report, not a gate.
//   - wind: the GRASS field (7 tufts a square metre, 45 m) from the play camera, wind 0.08 against none
//   - smooth_alpha: the alpha-tested card field, under MSAA 4x (alpha to coverage) and under TAA (hashed alpha),
//     against the plain alpha test
//   - DITHER: the GRASS field with dithered fades and LOD crossfades against SHRINK
//   node tools/with-gpu-suite.mjs foliage-perf node tests/foliage-perf.mjs [--rounds 3] [--frames 240]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { loadavg } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { gpuClock, clockLine } from '../tools/lib/gpu-clock.mjs';
await enterGpuSandbox();

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'foliage-perf');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : d);
const rounds = +opt('--rounds', 3), frames = opt('--frames', '240');
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.floor(s.length / 2)] : NaN; };

function run(gpu, env, name) {
  const state = join(OUT, `perf.${gpu}.${name}.json`);
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'foliage_field', '--frames', frames,
    '--state', state, '--env', JSON.stringify({ FOL_FROM: '60', ...env })], { encoding: 'utf8', env: { ...process.env, CW_GPU_TIMERS: '1' } });
  if (r.status !== 0) return { err: (r.stderr || r.stdout).trim().split('\n').slice(-2).join(' | ') };
  const st = JSON.parse(readFileSync(state, 'utf8'));
  return { ms: st['fol.gpu_ms'] };
}

const CASES = [
  ['wind', { FOL_SCENE: 'field' }, { FOL_SCENE: 'field', FOL_WIND: '0.08' }],
  ['smooth_alpha, MSAA 4x', { FOL_SCENE: 'cards', FOL_MSAA: '4' }, { FOL_SCENE: 'cards', FOL_MSAA: '4', FOL_ALPHA: 'smooth' }],
  ['smooth_alpha, TAA', { FOL_SCENE: 'cards', FOL_TAA: '1' }, { FOL_SCENE: 'cards', FOL_TAA: '1', FOL_ALPHA: 'smooth' }],
  ['fade_mode DITHER', { FOL_SCENE: 'field', FOL_TAA: '1' }, { FOL_SCENE: 'field', FOL_TAA: '1', FOL_FADE: 'dither' }],
  ['root_from_terrain', { FOL_SCENE: 'field' }, { FOL_SCENE: 'field', FOL_ROOT: '1' }],
];
const clk = gpuClock();
console.log(`foliage_field, 1920 x 1080, GPU ms a frame (median of ${rounds} interleaved rounds); load ${loadavg().map((v) => v.toFixed(1)).join(' ')}`);
console.log('gpu     case                     without   with   difference');
let failed = false;
for (const gpu of (opt('--gpu', 'gl,webgpu')).split(','))
  for (const [name, a, b] of CASES) {
    const x = [], y = [];
    for (let i = 0; i < rounds; i++) {
      const p = run(gpu, a, 'a'), q = run(gpu, b, 'b');
      if (p.err || q.err) { console.log(`${gpu} ${name}: ${p.err || q.err}`); failed = true; break; }
      x.push(p.ms); y.push(q.ms);
    }
    if (x.length) console.log(`${gpu.padEnd(7)} ${name.padEnd(24)} ${median(x).toFixed(2).padStart(7)}  ${median(y).toFixed(2).padStart(6)}   ${(median(y) - median(x)).toFixed(2).padStart(8)}   (rounds: ${x.map((v, i) => `${v.toFixed(2)}/${y[i].toFixed(2)}`).join(' ')})`);
  }
console.log(clockLine(await clk.stop()));
console.log(`load at the end ${loadavg().map((v) => v.toFixed(1)).join(' ')}`);
process.exit(failed ? 1 : 0);
