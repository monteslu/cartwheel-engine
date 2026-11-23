// What Terrain3D's near-ground options cost (cartwheel/terrain.h): the pipeline's GPU ms in samples/terrain_detail
// at 1920 x 1080 from its play camera (flagstones over the lower two thirds of the frame, 10 to 80 m away),
// natively on GL and WebGPU. Each option against the same frame without it:
//   rotation blend   a quarter turn on every other 4 m block, read across the texels (the shipping shading) against
//                    the nearest texel's rotation (CW_TEST_TERRAIN_NEAREST_UV=1, the seams)
//   macro tint       macro_tint 0.35;   macro layer   macro_layer 0.6;   both
//   parallax         depth 0.08 m on the layer, at Medium (4 steps), High (8) and Ultra (16), each against the
//                    same preset flat
// The rounds are interleaved (every case once, then again) and each figure is the minimum and the median of the
// rounds, with the machine's load and the GPU's clock beside them (docs/guides/profiling.md). A report: it fails
// only on a run that fails. Build the sample as a release (scripts/build.sh, no DEV).
//
//   node tools/with-gpu-suite.mjs terrain-detail-perf node tests/terrain-detail-perf.mjs [--rounds 3] [--frames 360] [--gpus gl,webgpu]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { loadavg } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { gpuClock, clockLine } from '../tools/lib/gpu-clock.mjs';
await enterGpuSandbox();

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'terrain-detail-perf');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : d);
const rounds = +opt('--rounds', 3), frames = opt('--frames', '360'), GPUS = opt('--gpus', 'gl,webgpu').split(',');
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)]; };
const P = { TD_PARALLAX: '0.08' };
// [label, the option's env, its baseline's env]
const CASES = [
  ['rotation blend (High)', { TD_PAINT: 'rot' }, { TD_PAINT: 'rot', CW_TEST_TERRAIN_NEAREST_UV: '1' }],
  ['macro tint (High)', { TD_MACRO_TINT: '0.35' }, {}],
  ['macro layer (High)', { TD_MACRO_LAYER: '0.6' }, {}],
  ['macro both (High)', { TD_MACRO_TINT: '0.35', TD_MACRO_LAYER: '0.6' }, {}],
  ['parallax (Medium)', { TD_QUALITY: 'medium', ...P }, { TD_QUALITY: 'medium' }],
  ['parallax (High)', P, {}],
  ['parallax (Ultra)', { TD_QUALITY: 'ultra', ...P }, { TD_QUALITY: 'ultra' }],
];

function run(gpu, env, tag) {
  const state = join(OUT, `${gpu}.${tag}.json`);
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'terrain_detail', '--frames', frames,
    '--env', JSON.stringify(env), '--state', state], { encoding: 'utf8', env: { ...process.env, CW_GPU_TIMERS: '1' } });
  if (r.status !== 0) return { err: (r.stderr || r.stdout).trim().split('\n').slice(-2).join(' | ') };
  const st = JSON.parse(readFileSync(state, 'utf8'));
  const s = st.state || st;
  return { ms: s['td.gpu_ms'], frames: s['td.frames'] };
}

const clk = gpuClock();
console.log(`terrain_detail, 1920 x 1080, play camera, pipeline GPU ms (min / median of ${rounds} interleaved rounds); load ${loadavg().map((v) => v.toFixed(1)).join(' ')}`);
let fails = 0;
for (const gpu of GPUS) {
  const V = CASES.map(() => ({ on: [], off: [] }));
  for (let i = 0; i < rounds; i++)
    CASES.forEach(([label, on, off], c) => {
      for (const [k, env] of [['off', off], ['on', on]]) {
        const r = run(gpu, env, `${c}.${k}`);
        if (r.err || !(r.ms > 0)) { console.log(`FAIL ${gpu} ${label} ${k}: ${r.err || `no GPU time (${r.ms})`}`); fails++; continue; }
        V[c][k].push(r.ms);
      }
    });
  CASES.forEach(([label], c) => {
    const { on, off } = V[c];
    if (!on.length || !off.length) return;
    const mn = Math.min(...on) - Math.min(...off), md = median(on) - median(off);
    console.log(`${gpu.padEnd(7)} ${label.padEnd(24)} off ${Math.min(...off).toFixed(3)} / ${median(off).toFixed(3)}   on ${Math.min(...on).toFixed(3)} / ${median(on).toFixed(3)}   cost ${mn.toFixed(3)} / ${md.toFixed(3)} ms`);
  });
}
console.log(clockLine(await clk.stop()));
console.log(`load at the end ${loadavg().map((v) => v.toFixed(1)).join(' ')}`);
console.log(fails ? `FAIL terrain-detail-perf (${fails})` : 'ok   terrain-detail-perf');
process.exit(fails ? 1 : 0);
