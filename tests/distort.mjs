// Distortion buffer, scene colour copy and Shockwave3D: fx_distort on GL and WebGPU.
// Each backend renders four frames: ON, OFF (every strength 0), and the same two with the
// pole moved behind the haze (the control for the depth rule). Checks, per backend:
//   - the haze, the lens and the ring change the picture (ON differs from OFF inside their footprints);
//   - far from every source (sky strip, floor strip, left edge) ON equals OFF exactly: nothing leaks;
//   - the pole in front of the haze is identical ON and OFF (it stays sharp), while the same
//     region differs when the pole stands behind the haze (the control: the check can fail);
//   - GL and WebGPU agree (share of pixels differing by more than 16 levels under 0.5 %).
//   node tests/distort.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
testEnv(process.env);
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'distort');
mkdirSync(OUT, { recursive: true });
const FRAMES = 90;

function shot(gpu, name, env) {
  const png = join(OUT, `${name}.${gpu}.png`);
  const r = spawnSync('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--app', 'fx_distort', '--frames', String(FRAMES),
    '--gpu', gpu, '--shot', png, '--state', join(OUT, `${name}.${gpu}.json`)], { env: { ...process.env, ...env }, encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`${gpu} ${name}: ${(r.stderr || '').trim().split('\n').slice(-2).join(' | ')}`);
  return readImage(png);
}
// pixels in the region (fractions of the image) whose max channel difference exceeds `level`
function diffIn(a, b, [x0, y0, x1, y1], level = 0) {
  let n = 0;
  for (let y = Math.floor(y0 * a.h); y < Math.floor(y1 * a.h); y++)
    for (let x = Math.floor(x0 * a.w); x < Math.floor(x1 * a.w); x++) {
      const i = (y * a.w + x) * 4;
      if (Math.max(Math.abs(a.rgba[i] - b.rgba[i]), Math.abs(a.rgba[i + 1] - b.rgba[i + 1]), Math.abs(a.rgba[i + 2] - b.rgba[i + 2])) > level) n++;
    }
  return n;
}
const R = { haze: [0.43, 0.25, 0.58, 0.58], ring: [0.62, 0.15, 0.98, 0.72], lens: [0.2, 0.3, 0.42, 0.7],
  sky: [0, 0, 1, 0.08], floor: [0, 0.88, 1, 1], edge: [0, 0.1, 0.06, 0.8], pole: [0.405, 0.3, 0.425, 0.7] };

let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const ons = {};
for (const gpu of ['gl', 'webgpu']) {
  const on = shot(gpu, 'on', {}), off = shot(gpu, 'off', { FXD_OFF: '1' });
  const pon = shot(gpu, 'pole_on', { FXD_POLE_BACK: '1' }), poff = shot(gpu, 'pole_off', { FXD_POLE_BACK: '1', FXD_OFF: '1' });
  ons[gpu] = on;
  for (const k of ['haze', 'ring', 'lens']) {
    const n = diffIn(on, off, R[k], 16);
    check(n > 1500, `${gpu}: ${k} bends the picture (${n} pixels differ by more than 16 levels)`);
  }
  for (const k of ['sky', 'floor', 'edge']) {
    const n = diffIn(on, off, R[k], 2);
    check(n === 0, `${gpu}: nothing leaks into the ${k} (${n} pixels differ)`);
  }
  const front = diffIn(on, off, R.pole, 2), back = diffIn(pon, poff, R.pole, 2);
  check(front === 0, `${gpu}: the pole in front of the haze stays sharp (${front} pixels differ)`);
  check(back > 100, `${gpu}: CONTROL, the same pixels bend when the pole is behind the haze (${back} pixels): the check can see`);
}
let big = 0;
const a = ons.gl, b = ons.webgpu;
for (let i = 0; i < a.rgba.length; i += 4)
  if (Math.max(Math.abs(a.rgba[i] - b.rgba[i]), Math.abs(a.rgba[i + 1] - b.rgba[i + 1]), Math.abs(a.rgba[i + 2] - b.rgba[i + 2])) > 16) big++;
const share = big / (a.w * a.h);
check(share < 0.005, `GL and WebGPU agree (${(share * 100).toFixed(3)} % of pixels differ by more than 16 levels)`);
console.log(fail ? `distort: ${fail} failures` : 'ok   distort');
process.exit(fail ? 1 : 0);
