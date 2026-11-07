// The 2D renderer's second round, on GL and WebGPU (natively, the test GPU). Checks:
//   cull       depth culling (the opaque fast path) changes no pixel: r2d_hdr and r2d_stress's sidescroll drawn
//              with it and without it (CW_R2D_CULL=0) are identical. Control: CW_R2D_CULL=control (the test
//              without its half step, so opaque texels fail against their own depth) must change the frame.
//   lut        the identity lookup table leaves the picture within 2 levels (all but 0.1 % of pixels); the teal and
//              orange preset changes most of it (a table that does nothing fails this).
//   distort    a layer drawn above a distortion layer stays straight: r2d_hdr "front" (a lattice over the heat and
//              the shockwave) matches "frontnodistort" on the lattice, while off the lattice the distortion still
//              bends the picture. Control: CW_R2D_DIST_LATE=1 (every layer bent once at the end, as before) must
//              bend the lattice.
//   instances  sprite instances draw the sprites200k scene as the sprites do (r2d_stress inst200k against
//              sprites200k, the same tick): under 0.5 % of pixels past 16 levels. Control: the kept buffer's still
//              scene (instkept200k) must differ.
//   presets    r2d_stress sidescroll on Very Low and Low (a smaller light buffer, the small HDR canvas) stays near
//              High: under 3 % of pixels past 16 levels; and differs from High at all (the presets really scale).
//   cookie     light textures (r2d_hdr "cookie": the torches and the spot through a window's panes) change the lit
//              picture where those lights fall (a texture that does nothing fails this).
//   parity     GL against WebGPU for every frame above: under 0.5 % of pixels past 16 levels.
// Holds the GPU suite lock. Run: node tests/r2d-round2.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
if (!process.env.CW_SUITE_LOCK_HELD) {
  const r = spawnSync('node', [join(ROOT, 'tools/with-gpu-suite.mjs'), 'r2d-round2', 'node', fileURLToPath(import.meta.url), ...process.argv.slice(2)], { stdio: 'inherit' });
  process.exit(r.status ?? 1);
}
const OUT = join(ROOT, 'out', 'r2d-round2');
mkdirSync(OUT, { recursive: true });
let bad = 0;
const ok = (cond, msg) => { console.log(`${cond ? 'ok  ' : 'FAIL'} ${msg}`); if (!cond) bad++; };

function shot(app, gpu, name, env, frames, extra = []) {
  const png = join(OUT, `${name}.${gpu}.png`);
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', app, '--frames', String(frames), '--gpu', gpu, '--shot', png, ...extra],
    { env: { ...process.env, ...env }, encoding: 'utf8', timeout: 300000 });
  if (r.status !== 0) throw new Error(`${app} ${name} ${gpu}: ${(r.stderr || '').trim().split('\n').slice(-3).join(' | ')}`);
  return readImage(png);
}
const hdr = (gpu, mode, env = {}) => shot('r2d_hdr', gpu, `hdr-${mode}${Object.keys(env).map((k) => '-' + env[k]).join('')}`, { CW_R2D_HDR: mode, ...env }, 120);
const stress = (gpu, c, env = {}) => shot('r2d_stress', gpu, `stress-${c}${Object.keys(env).map((k) => '-' + env[k]).join('')}`, { CW_STRESS: c, CW_STRESS_SIZE: '1920x1080', ...env }, 30);
// max channel difference per pixel over the pixels where pick(x, y) holds: [pixels past `level`, pixels looked at]
function diffWhere(a, b, pick, level) {
  let n = 0, m = 0;
  for (let y = 0; y < a.h; y++)
    for (let x = 0; x < a.w; x++) {
      if (!pick(x, y)) continue;
      m++;
      const i = (y * a.w + x) * 4;
      if (Math.max(Math.abs(a.rgba[i] - b.rgba[i]), Math.abs(a.rgba[i + 1] - b.rgba[i + 1]), Math.abs(a.rgba[i + 2] - b.rgba[i + 2])) > level) n++;
    }
  return [n, m];
}
// r2d_hdr's lattice (samples/r2d_hdr "front"), 2 px inside its bars, in frame pixels (canvas 1280x720 logical)
function onLattice(img) {
  const s = img.w / 1280, W = 1280, H = 720;
  const bars = [];
  for (let i = 0; i < 9; i++) bars.push([W * 0.05 + i * W * 0.1, H * 0.3, 3, H * 0.5]);
  for (let j = 0; j < 4; j++) bars.push([W * 0.05, H * (0.32 + 0.15 * j), W * 0.8, 3]);
  return (x, y) => bars.some(([bx, by, bw, bh]) => x >= bx * s + 1 && x < (bx + bw) * s - 1 && y >= by * s + 1 && y < (by + bh) * s - 1);
}
const parity = [];
for (const gpu of ['gl', 'webgpu']) {
  // cull
  const h = hdr(gpu, 'default'), h0 = hdr(gpu, 'default', { CW_R2D_CULL: '0' }), hc = hdr(gpu, 'default', { CW_R2D_CULL: 'control' });
  let d = diffImages(h, h0);
  ok(d.any === 0, `${gpu} cull: r2d_hdr with culling equals it without (${(d.any * 100).toFixed(4)} % of pixels differ, max ${d.max})`);
  d = diffImages(hc, h0);
  ok(d.gt16 > 0.01, `${gpu} cull control: without the half step the frame loses opaque texels (${(d.gt16 * 100).toFixed(2)} % past 16 levels)`);
  const s = stress(gpu, 'sidescroll'), s0 = stress(gpu, 'sidescroll', { CW_R2D_CULL: '0' });
  d = diffImages(s, s0);
  ok(d.any === 0, `${gpu} cull: sidescroll with culling equals it without (${(d.any * 100).toFixed(4)} % differ, max ${d.max})`);
  // lut
  const li = hdr(gpu, 'lutid'), lt = hdr(gpu, 'lut');
  d = diffImages(li, h);
  ok(d.gt2 < 0.001, `${gpu} lut: the identity table keeps the picture (${(d.gt2 * 100).toFixed(4)} % past 2 levels, max ${d.max})`);
  d = diffImages(lt, h);
  ok(d.gt2 > 0.5, `${gpu} lut control: the teal and orange table changes the picture (${(d.gt2 * 100).toFixed(1)} % past 2 levels)`);
  // cookie
  const ck = hdr(gpu, 'cookie');
  d = diffImages(ck, h);
  ok(d.gt16 > 0.02, `${gpu} cookie: the window panes shape the torches and the spot (${(d.gt16 * 100).toFixed(1)} % past 16 levels)`);
  // distort
  const f = hdr(gpu, 'front'), fn = hdr(gpu, 'frontnodistort'), fl = hdr(gpu, 'front', { CW_R2D_DIST_LATE: '1' });
  const lat = onLattice(f);
  const [on, onN] = diffWhere(f, fn, lat, 16);
  ok(onN > 1000 && on / onN < 0.005, `${gpu} distort: the lattice above the distortion layer stays straight (${on} of ${onN} lattice pixels past 16 levels)`);
  const [off, offN] = diffWhere(f, fn, (x, y) => !lat(x, y) && y > f.h * 0.3 && y < f.h * 0.8, 16);
  ok(off > 2000, `${gpu} distort: off the lattice the picture still bends (${off} of ${offN} pixels past 16 levels)`);
  const [late] = diffWhere(fl, fn, lat, 16);
  ok(late > on * 4 && late > 200, `${gpu} distort control: bending every layer at the end bends the lattice (${late} lattice pixels past 16 levels)`);
  // instances
  const sp = stress(gpu, 'sprites200k'), ins = stress(gpu, 'inst200k'), ink = stress(gpu, 'instkept200k');
  d = diffImages(ins, sp);
  ok(d.gt16 < 0.005, `${gpu} instances: 200k instances draw the sprites' picture (${(d.gt16 * 100).toFixed(3)} % past 16 levels)`);
  d = diffImages(ink, sp);
  ok(d.gt16 > 0.05, `${gpu} instances control: the still kept buffer's picture differs (${(d.gt16 * 100).toFixed(1)} % past 16 levels)`);
  // presets
  const sl = stress(gpu, 'sidescroll', { CW_QUALITY: 'low' }), sv = stress(gpu, 'sidescroll', { CW_QUALITY: 'verylow' });
  for (const [name, img] of [['Low', sl], ['Very Low', sv]]) {
    d = diffImages(img, s);
    ok(d.gt16 < 0.03 && d.any > 0.01, `${gpu} presets: sidescroll on ${name} near High (${(d.gt16 * 100).toFixed(2)} % past 16 levels, ${(d.any * 100).toFixed(1)} % differ)`);
  }
  parity.push({ h, li, lt, ck, f, fn, s, ins, sl, sv });
}
const [g, w] = parity;
for (const k of Object.keys(g)) {
  const d = diffImages(g[k], w[k]);
  ok(d.gt16 < 0.005, `parity ${k}: GL against WebGPU (${(d.gt16 * 100).toFixed(3)} % past 16 levels)`);
}
console.log(bad ? `${bad} check(s) failed` : 'all checks passed');
process.exit(bad ? 1 : 0);
