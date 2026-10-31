// Water3D with none of its options set still draws as it did before they were added.
//
// samples/water_ripples sets no foam, swell or grid option, and it has what the look is made of: a boat's wake, a
// splash and foam on the ripple grid over Gerstner waves. tests/water-default-look/water_ripples.png is its frame 260,
// rows 440 to 759 and columns 320 to 1119 (the wake), drawn by the engine at tag v0.25.4, the release before those
// options, natively on GL on the test GPU. This run must match it on GL and on WebGPU, on the test GPU and on a software
// renderer: those differ from each other by under 0.05% of these pixels over 16 levels. Reading the floor map and the
// ripple grid at a surface point's rest position (the fault in v0.26.0 to v0.28.4) moves 3.8% of them.
//
// CONTROL: the foam's lace a little lower (foam_lace 1.2, the default 1.4: 0.24% to 0.30%) must break the bound of 0.1%, or the gate
// could not see the foam.
//
// A change that means to change how water looks by default writes a new reference with --write and says so in the
// CHANGELOG.
//
//   node tests/water-default-look.mjs [--gpus gl,webgpu] [--exe <binary>] [--write]
import { spawnSync } from 'node:child_process';
import { mkdirSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, writePng, diffImages } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
await takeSuiteLock('water-default-look.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'water-default-look');
mkdirSync(OUT, { recursive: true });
const REF = join(ROOT, 'tests', 'water-default-look', 'water_ripples.png');
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const GPUS = opt('--gpus', 'gl,webgpu').split(',');
const EXE = opt('--exe', null);
const FRAMES = 260, X = 320, Y = 440, W = 800, H = 320, MAX_GT16 = 0.001;

function run(gpu, tag, cmds = []) {
  const shot = join(OUT, `${gpu}-${tag}.png`);
  const args = ['tools/run-cart.mjs', '--host', 'native', '--gpu', gpu, '--app', 'water_ripples', '--frames', String(FRAMES), '--shot', shot,
    ...(EXE ? ['--exe', resolve(EXE)] : []), ...(cmds.length ? ['--start-cmds', cmds.join(';')] : [])];
  const r = spawnSync(process.execPath, args, { cwd: ROOT, env: process.env, encoding: 'utf8' });
  if (r.status !== 0 || !existsSync(shot)) { console.log(`FAIL water-default-look ${gpu} ${tag}: exit ${r.status}: ${(r.stderr || '').slice(-600)}`); process.exit(1); }
  const img = readImage(shot);
  if (img.w !== 1920 || img.h !== 1080) { console.log(`FAIL water-default-look ${gpu} ${tag}: frame is ${img.w} x ${img.h}, the reference's crop needs 1920 x 1080`); process.exit(1); }
  const rgba = new Uint8Array(W * H * 4);
  for (let y = 0; y < H; y++) rgba.set(img.rgba.subarray(((Y + y) * img.w + X) * 4, ((Y + y) * img.w + X + W) * 4), y * W * 4);
  return { w: W, h: H, rgba };
}

if (argv.includes('--write')) {
  const c = run(GPUS[0], 'ref');
  writePng(REF, W, H, c.rgba);
  console.log(`wrote ${REF}`);
  process.exit(0);
}

const ref = readImage(REF);
const pct = (v) => `${(v * 100).toFixed(3)}%`;
let fail = 0;
for (const gpu of GPUS) {
  const d = diffImages(ref, run(gpu, 'default'));
  if (d.comparable && d.gt16 <= MAX_GT16) console.log(`ok   water-default-look ${gpu}: ${pct(d.gt16)} over 16 levels (bound ${pct(MAX_GT16)}), max ${d.max}`);
  else { fail++; console.log(`FAIL water-default-look ${gpu}: ${pct(d.gt16)} over 16 levels (bound ${pct(MAX_GT16)}), max ${d.max}: water with no option set no longer draws as the reference does`); }
  const c = diffImages(ref, run(gpu, 'control', ['set Root/Water foam_lace 1.2']));
  if (c.comparable && c.gt16 > MAX_GT16) console.log(`ok   water-default-look ${gpu} control fails as it must: ${pct(c.gt16)} with foam_lace 1.2`);
  else { fail++; console.log(`FAIL water-default-look ${gpu}: CONTROL PASSED: ${pct(c.gt16)} with foam_lace 1.2 is within the bound`); }
}
process.exit(fail ? 1 : 0);
