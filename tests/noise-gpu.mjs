// The GPU ports of cw_noise (cw_noise_glsl / cw_noise_wgsl) agree with the
// CPU (cw_noise_2d / _3d) on every host and backend.
//
// samples/noise_demo draws sixteen noise settings three times each: the GPU
// port, the CPU's texels and the difference in 8-bit levels (red, 64 per
// level). Per host and backend this reads every texel of the difference rows
// (at the texel's centre, whatever size the host drew the window at) and
// requires: at most 0.5% of texels a level apart (float rounding lands either
// side of a .5 now and then) and at most 0.05% two or more levels apart (a
// cellular point exactly between two cells). The GPU and CPU rows must be
// real pictures (enough grey levels), or a black frame would pass.
// CONTROL: natively, NOISE_DEMO_BREAK=1 gives the GPU another seed; that
// run must fail the bounds on each backend.
//
//   node tests/noise-gpu.mjs [--hosts native,romdev,chromium] [--gpus gl,webgpu]
import { spawn } from 'node:child_process';
import { mkdirSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
await takeSuiteLock('noise-gpu.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'noise-gpu');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return (i >= 0 ? argv[i + 1] : d).split(','); };
const HOSTS = opt('--hosts', 'native,romdev,chromium');
const GPUS = opt('--gpus', 'gl,webgpu');
const FRAMES = 20, AW = 512, AH = 384, P = 64;
const MAX_ONE = 0.005, MAX_TWO = 0.0005;

function runCart(host, gpu, tag, env = {}) {
  const shot = join(OUT, `${host}-${gpu}-${tag}.png`);
  const args = ['tools/run-cart.mjs', '--host', host, '--gpu', gpu, '--app', 'noise_demo', '--frames', String(FRAMES), '--shot', shot];
  return new Promise((res) => {
    const p = spawn(process.execPath, args, { cwd: ROOT, env: { ...process.env, ...env }, stdio: ['ignore', 'pipe', 'pipe'] });
    let err = '';
    p.stdout.on('data', () => {});
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => res(code === 0 && existsSync(shot) ? { img: readImage(shot) } : { error: `exit ${code}: ${err.slice(-600)}` }));
  });
}

/** Per texel of the difference rows: the level; and the grey levels of the picture rows. */
function measure(img) {
  const sx = img.w / AW, sy = img.h / AH;
  const px = (ax, ay) => {
    const x = Math.min(img.w - 1, Math.floor((ax + 0.5) * sx)), y = Math.min(img.h - 1, Math.floor((ay + 0.5) * sy));
    return 4 * (y * img.w + x);
  };
  let one = 0, two = 0, n = 0;
  const greys = new Set();
  for (let band = 0; band < 2; band++)
    for (let ty = 0; ty < P; ty++)
      for (let ax = 0; ax < AW; ax++) {
        const d = img.rgba[px(ax, band * 3 * P + 2 * P + ty)];
        const lvl = Math.round(d / 64);
        n++;
        if (lvl >= 1) one++;
        if (lvl >= 2) two++;
        greys.add(img.rgba[px(ax, band * 3 * P + ty)]);
        greys.add(img.rgba[px(ax, band * 3 * P + P + ty)]);
      }
  return { one: one / n, two: two / n, greys: greys.size };
}

const verdict = (m) => m.one <= MAX_ONE && m.two <= MAX_TWO && m.greys >= 100;
const fmt = (m) => `${(m.one * 100).toFixed(3)}% a level apart, ${(m.two * 100).toFixed(3)}% two or more, ${m.greys} grey levels`;

let fail = 0;
for (const gpu of GPUS) {
  for (const host of HOSTS) {
    const r = await runCart(host, gpu, 'ref');
    if (r.error) { fail++; console.log(`FAIL noise-gpu ${host} ${gpu}: ${r.error}`); continue; }
    const m = measure(r.img);
    if (verdict(m)) console.log(`ok   noise-gpu ${host} ${gpu}: ${fmt(m)}`);
    else { fail++; console.log(`FAIL noise-gpu ${host} ${gpu}: ${fmt(m)}`); }
  }
  const c = await runCart('native', gpu, 'control', { NOISE_DEMO_BREAK: '1' });
  if (c.error) { fail++; console.log(`FAIL noise-gpu control ${gpu}: ${c.error}`); continue; }
  const m = measure(c.img);
  if (verdict(m)) { fail++; console.log(`FAIL noise-gpu ${gpu}: CONTROL PASSED (another GPU seed): ${fmt(m)}`); }
  else console.log(`ok   noise-gpu ${gpu} control fails as it must: ${fmt(m)}`);
}
process.exit(fail ? 1 : 0);
