// Volumetric fog heals from a broken cell: fog_shafts_perf (the volumetric room, the camera turning).
// The grid stores half floats and blends every frame with the previous ones (temporal history, fetched
// with a linear filter): a cell that once holds an infinity or a NaN would be carried into every later
// frame and spread to its neighbours until the whole view is flat fog. The grid bounds what each cell
// stores (a NaN becomes 0, light is clamped to 60000 and extinction to 64) and blends in at most 8 times
// what a cell holds now, so a broken cell fades out in a frame or two.
// Checks, on GL and WebGPU, each against the run with nothing unusual, at frame FRAMES (143 frames later):
//   - CW_TEST_VFOG_POISON=1: an infinity put into the left quarter of the cells on frames 5 to 7 has
//     faded out (share of pixels off by more than 16 levels under 1 %), and on frame 7 it does show
//     (the poison is real). Control (must fail): the same with CW_TEST_VFOG_UNBOUNDED=1 stays broken.
//   - FOG_SHAFTS_HOT=1: the spot light flares to 100000 for frames 5 to 7 and goes back: no trace later.
//   - FOG_SHAFTS_HOT=nan: the spot light's position is not a number for frames 5 to 7: no trace later.
//   node tests/fog-flare.mjs [--gl | --webgpu]
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
testEnv(process.env);
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'fog-flare');
mkdirSync(OUT, { recursive: true });
const FRAMES = 150;

function shot(gpu, name, frames, env) {
  const png = join(OUT, `${name}.${gpu}.png`);
  const r = spawnSync('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--app', 'fog_shafts_perf', '--frames', String(frames),
    '--gpu', gpu, '--shot', png, '--state', join(OUT, `${name}.${gpu}.json`)], { env: { ...process.env, ...env }, encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`${gpu} ${name}: ${(r.stderr || '').trim().split('\n').slice(-2).join(' | ')}`);
  return readImage(png);
}
const pct = (d) => (d.gt16 * 100).toFixed(3);

let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const gpus = process.argv.includes('--gl') ? ['gl'] : process.argv.includes('--webgpu') ? ['webgpu'] : ['gl', 'webgpu'];
for (const gpu of gpus) {
  const calm = shot(gpu, 'calm', FRAMES, {});
  const P = { CW_TEST_VFOG_POISON: '1' };
  const shows = diffImages(shot(gpu, 'calm7', 7, {}), shot(gpu, 'poison7', 7, P));
  check(shows.gt16 > 0.05, `${gpu}: the poisoned cells show on frame 7 (${pct(shows)} % of pixels off by more than 16)`);
  const healed = diffImages(calm, shot(gpu, 'poison', FRAMES, P));
  check(healed.gt16 < 0.01, `${gpu}: ${FRAMES - 7} frames later the poison has faded out (${pct(healed)} %)`);
  const ctl = diffImages(calm, shot(gpu, 'poison_unbounded', FRAMES, { ...P, CW_TEST_VFOG_UNBOUNDED: '1' }));
  check(ctl.gt16 >= 0.05, `${gpu}: control, without the bound the poison stays (${pct(ctl)} %)`);
  for (const [v, what] of [['1', 'a flare to 100000'], ['nan', 'a light at a NaN position']]) {
    const d = diffImages(calm, shot(gpu, `hot_${v}`, FRAMES, { FOG_SHAFTS_HOT: v }));
    check(d.gt16 < 0.01, `${gpu}: ${FRAMES - 7} frames after ${what} the fog shows no trace (${pct(d)} %)`);
  }
}
console.log(fail ? `fog-flare: ${fail} check(s) failed` : 'fog-flare: ok');
process.exit(fail ? 1 : 0);
