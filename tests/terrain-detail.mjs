// Terrain3D's near-ground shading, measured from the picture (samples/terrain_detail: a flat field of 4 m flagstone
// tiles, cartwheel/terrain.h). Per host and backend:
//   1. uv rotation blended across control texels: with a quarter turn on every other 4 m block (a checkerboard), the
//      brightness step between neighbouring pixels that straddle a block's edge, or the lines half a texel either side
//      where the blend starts and ends, is no larger than between pixels that straddle a line through the blocks
//      (ratio under 1.15), from straight above and from a play camera. CONTROL: CW_TEST_TERRAIN_NEAREST_UV=1 (each
//      point's rotation from its nearest texel, as before) must show the seams (ratio over 1.3).
//   2. macro variation (macro_tint 0.35, macro_layer 0.6), top-down over 40 m: at Very Low (one plain read a layer,
//      so the tile repeats exactly) the luminance autocorrelation's peak at the 4 m period drops by a third or more;
//      at High (stochastic cells: no peak left to cut) the spread of 2 m patches' mean brightness at least doubles.
//      CONTROL: both off, the baseline each must move from.
//   3. blend_sharpness: flagstones meeting sand across an 8 m blend; the transition (the share of sand pixels per
//      column from 0.1 to 0.9) is wider at 0.4 than at 0.15 and narrower at 0.05; 0.15 set is the default pixel for
//      pixel. CONTROL: a sharpness on layer 5 (painted nowhere) changes nothing.
//   4. parallax: layers with a parallax depth of 0.08 m shift the picture (block matching against the flat one) by
//      2 px or more from a low camera and by less than half that from straight above; depth 0 is the flat picture
//      pixel for pixel. CONTROL: the same at Low (no parallax steps) shifts nothing.
//   node tests/terrain-detail.mjs [--hosts native,romdev] [--gpu gl,webgpu]   (romdev runs the dual cart, build/cart-wgpu)
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'terrain-detail');
mkdirSync(OUT, { recursive: true });
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const HOSTS = arg('--hosts', process.env.CW_TEST_GPU === 'software' ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
const ONLY = arg('--only', '1,2,3,4').split(',');
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

const FRAMES = 20;
function run(host, gpu, name, env) {
  const png = join(OUT, `${name}.${host}.${gpu}.png`);
  const a = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'terrain_detail', '--frames', String(FRAMES), '--gpu', gpu, '--shot', png];
  a.push('--env', JSON.stringify(env));
  const r = spawnSync('node', a, { encoding: 'utf8', env: process.env, maxBuffer: 1 << 26 });
  if (r.status !== 0) throw new Error(`${host} ${gpu} ${name}: ${((r.stderr || '') + (r.stdout || '')).trim().split('\n').slice(-3).join(' | ')}`);
  return readImage(png);
}

// ── the picture's geometry: the sample's cameras (fov 60, flat ground at y = 0) ──
const VIEWS = {
  play: { pos: [0, 8, 20], at: [0, 0, -10], up: [0, 1, 0] },
  top: { pos: [0, 34.641, 0], at: [0, 0, 0], up: [0, 0, -1] },
  graze: { pos: [0, 1.6, 6], at: [0, 0, -4], up: [0, 1, 0] },
};
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const norm = (a) => { const l = Math.hypot(...a); return [a[0] / l, a[1] / l, a[2] / l]; };
// world x, z of every pixel's ground point (NaN above the horizon), and its distance
function ground(img, view) {
  const v = VIEWS[view], f = norm(sub(v.at, v.pos)), r = norm(cross(f, v.up)), u = cross(r, f);
  const th = Math.tan(Math.PI / 6), asp = img.w / img.h;
  const X = new Float32Array(img.w * img.h), Z = new Float32Array(img.w * img.h), D = new Float32Array(img.w * img.h);
  for (let y = 0; y < img.h; y++)
    for (let x = 0; x < img.w; x++) {
      const nx = ((2 * (x + 0.5)) / img.w - 1) * th * asp, ny = (1 - (2 * (y + 0.5)) / img.h) * th;
      const d = [f[0] + r[0] * nx + u[0] * ny, f[1] + r[1] * nx + u[1] * ny, f[2] + r[2] * nx + u[2] * ny];
      const i = y * img.w + x;
      if (d[1] >= -1e-4) { X[i] = Z[i] = D[i] = NaN; continue; }
      const t = -v.pos[1] / d[1];
      X[i] = v.pos[0] + d[0] * t, Z[i] = v.pos[2] + d[2] * t, D[i] = t * Math.hypot(...d);
    }
  return { X, Z, D };
}
const lum = (img) => {
  const L = new Float32Array(img.w * img.h);
  for (let i = 0; i < L.length; i++) L[i] = 0.2126 * img.rgba[4 * i] + 0.7152 * img.rgba[4 * i + 1] + 0.0722 * img.rgba[4 * i + 2];
  return L;
};
const same = (a, b) => { for (let i = 0; i < a.rgba.length; i++) if (a.rgba[i] !== b.rgba[i]) return false; return true; };
const changed = (a, b) => { let n = 0; for (let i = 0; i < a.w * a.h; i++) if (a.rgba[4 * i] !== b.rgba[4 * i] || a.rgba[4 * i + 1] !== b.rgba[4 * i + 1] || a.rgba[4 * i + 2] !== b.rgba[4 * i + 2]) n++; return n / (a.w * a.h); };

// 1: mean brightness step across block edges over the mean across lines through the middle of the blocks, between
// horizontally and vertically adjacent pixels on ground within 45 m whose ground points are under 0.2 m apart. The
// splat puts control texel i's centre at i + 0.5 samples from the origin (world -128), so a point's nearest texel
// changes on whole samples: the block edges are x or z = 4k, the lines through the blocks 4k + 2
function seamRatio(img, view, at = 0) {
  const { X, Z, D } = ground(img, view), L = lum(img);
  let sS = 0, nS = 0, sN = 0, nN = 0;
  const cell = (w, off) => Math.floor((w + 128 - off) / 4);
  const pair = (i, j) => {
    if (!(D[i] < 45 && D[j] < 45)) return;
    if (Math.hypot(X[i] - X[j], Z[i] - Z[j]) > 0.2) return;
    const d = Math.abs(L[i] - L[j]);
    const seam = cell(X[i], at) !== cell(X[j], at) || cell(Z[i], at) !== cell(Z[j], at);
    const mid = cell(X[i], 2) !== cell(X[j], 2) || cell(Z[i], 2) !== cell(Z[j], 2);
    if (seam && !mid) sS += d, nS++;
    else if (mid && !seam) sN += d, nN++;
  };
  for (let y = 0; y < img.h - 1; y++)
    for (let x = 0; x < img.w - 1; x++) { const i = y * img.w + x; pair(i, i + 1); pair(i, i + img.w); }
  return { ratio: sS / nS / (sN / nN), n: Math.min(nS, nN) };
}

// 2: the autocorrelation's peak at the tile's period (4 m; top-down the ground is 27 px a metre) over the mean of
// lags round it, along x and along z, on the middle 900 x 900 pixels
function repeatPeak(img) {
  const L = lum(img), P = Math.round((img.h / 40) * 4), x0 = (img.w - 900) >> 1, y0 = (img.h - 900) >> 1, N = 900 - 2 * P;
  let mean = 0;
  for (let y = 0; y < 900; y++) for (let x = 0; x < 900; x++) mean += L[(y0 + y) * img.w + x0 + x];
  mean /= 900 * 900;
  const ac = (dx, dy) => {
    let s = 0, v = 0;
    for (let y = 0; y < N; y++)
      for (let x = 0; x < N; x++) {
        const a = L[(y0 + y) * img.w + x0 + x] - mean, b = L[(y0 + y + dy) * img.w + x0 + x + dx] - mean;
        s += a * b, v += a * a;
      }
    return s / v;
  };
  let peak = 0;
  for (const [ax, ay] of [[1, 0], [0, 1]]) {
    const at = (k) => ac(Math.round(ax * k * P), Math.round(ay * k * P));
    peak += at(1) - (at(0.6) + at(0.8) + at(1.2) + at(1.4)) / 4;
  }
  return peak / 2;
}

// 2b: how much 2 m x 2 m patches of ground differ in mean brightness (their standard deviation over the mean),
// top-down over the whole frame (71 x 40 m)
function patchSpread(img) {
  const L = lum(img), S = Math.round((img.h / 40) * 2), m = [];
  for (let py = 0; py + S <= img.h; py += S)
    for (let px = 0; px + S <= img.w; px += S) {
      let s = 0;
      for (let y = 0; y < S; y++) for (let x = 0; x < S; x++) s += L[(py + y) * img.w + px + x];
      m.push(s / (S * S));
    }
  const mean = m.reduce((p, q) => p + q) / m.length;
  return Math.sqrt(m.reduce((p, q) => p + (q - mean) ** 2, 0) / m.length) / mean;
}

// 3: the transition's width in metres, top-down: per column the share of pixels that are sand (warm: red over blue by
// more than halfway from the flagstones' mean to the sand's), smoothed over half a metre, from where it passes 0.1 to
// where it passes 0.9
function transition(img) {
  const { w, h, rgba } = img, warm = (i) => rgba[4 * i] - rgba[4 * i + 2];
  const ppm = h / 40, wx = (x) => (x + 0.5 - w / 2) / ppm;
  let a = 0, na = 0, b = 0, nb = 0;
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const v = wx(x);
      if (v > -20 && v < -8) a += warm(y * w + x), na++;
      if (v > 14 && v < 24) b += warm(y * w + x), nb++;
    }
  const cut = (a / na + b / nb) / 2, share = new Float32Array(w);
  for (let x = 0; x < w; x++) { let s = 0; for (let y = 0; y < h; y++) s += warm(y * w + x) > cut ? 1 : 0; share[x] = s / h; }
  const r = Math.round(ppm / 4), sm = (x) => { let s = 0, n = 0; for (let k = -r; k <= r; k++) if (x + k >= 0 && x + k < w) s += share[x + k], n++; return s / n; };
  let lo = null, hi = null;
  for (let x = 0; x < w; x++) {
    const v = wx(x), p = sm(x);
    if (v < -8 || v > 14) continue;
    if (lo === null && p > 0.1) lo = v;
    if (p > 0.9) { hi = v; break; }
  }
  return hi - lo;
}

// 4: the median shift (pixels) that best matches 24 x 24 blocks of b to a, searched 10 px each way, over textured
// blocks in the picture's lower half
function shift(a, b) {
  const A = lum(a), B = lum(b), W = a.w, S = 24, R = 10, out = [];
  for (let by = Math.floor(a.h / 2) + R; by + S + R < a.h; by += 48)
    for (let bx = R; bx + S + R < W; bx += 48) {
      let m = 0, v = 0;
      for (let y = 0; y < S; y++) for (let x = 0; x < S; x++) m += A[(by + y) * W + bx + x];
      m /= S * S;
      for (let y = 0; y < S; y++) for (let x = 0; x < S; x++) v += (A[(by + y) * W + bx + x] - m) ** 2;
      if (v / (S * S) < 25) continue; // a flat block finds no match
      let best = Infinity, bd = 0;
      for (let dy = -R; dy <= R; dy++)
        for (let dx = -R; dx <= R; dx++) {
          let s = 0;
          for (let y = 0; y < S && s < best; y++) for (let x = 0; x < S; x++) s += Math.abs(A[(by + y) * W + bx + x] - B[(by + y + dy) * W + bx + x + dx]);
          if (s < best) best = s, bd = Math.hypot(dx, dy);
        }
      out.push(bd);
    }
  out.sort((p, q) => p - q);
  return out.length ? out[out.length >> 1] : 0;
}

let fail = 0;
const check = (name, ok, d) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}: ${d}`); if (!ok) fail++; };

for (const host of HOSTS)
  for (const gpu of GPUS) {
    const tag = `${host} ${gpu}`;
    try {
      if (ONLY.includes('1')) {
        for (const view of ['top', 'play']) {
          const img = run(host, gpu, `rot-${view}`, { TD_PAINT: 'rot', TD_VIEW: view });
          // on the edge, and on the lines where the blend between the two rotations starts and ends (the texel centres
          // either side, half a sample away) and between
          const at = [-0.5, -0.25, 0, 0.25, 0.5].map((o) => seamRatio(img, view, o).ratio);
          const ctl = seamRatio(run(host, gpu, `rot-${view}-nearest`, { TD_PAINT: 'rot', TD_VIEW: view, CW_TEST_TERRAIN_NEAREST_UV: '1' }), view);
          check(`${tag} 1 rotation blended, ${view}`, Math.max(...at) < 1.15, `step ratio across the band ${at.map((r) => r.toFixed(2)).join(' ')}`);
          check(`${tag} 1 CONTROL nearest texel's rotation shows the seams, ${view}`, ctl.ratio > 1.3, `ratio ${ctl.ratio.toFixed(3)}`);
        }
      }
      if (ONLY.includes('2')) {
        for (const q of ['verylow', 'high']) {
          const a = run(host, gpu, `macro-off-${q}`, { TD_VIEW: 'top', TD_QUALITY: q });
          const b = run(host, gpu, `macro-on-${q}`, { TD_VIEW: 'top', TD_QUALITY: q, TD_MACRO_TINT: '0.35', TD_MACRO_LAYER: '0.6' });
          const off = repeatPeak(a), on = repeatPeak(b), soff = patchSpread(a), son = patchSpread(b);
          const d = `repeat peak ${off.toFixed(3)} off (the CONTROL baseline) -> ${on.toFixed(3)} on; 2 m patches' spread ${(soff * 100).toFixed(1)}% -> ${(son * 100).toFixed(1)}%`;
          // Very Low reads each layer once, plainly: the tile repeats exactly and the peak is the measure. Higher
          // presets break the repeat already (stochastic cells: no peak to cut), and what is left of the wallpaper
          // is that every patch of ground averages the same: the patches' spread is the measure there
          if (q === 'verylow') check(`${tag} 2 macro variation cuts the repeat, ${q}`, off > 0.3 && on < off * 0.67, d);
          else check(`${tag} 2 macro variation varies the ground patch to patch, ${q}`, son > soff * 2, d);
        }
      }
      if (ONLY.includes('3')) {
        const base = run(host, gpu, 'pair', { TD_PAINT: 'pair', TD_VIEW: 'top' });
        const set = run(host, gpu, 'pair-015', { TD_PAINT: 'pair', TD_VIEW: 'top', TD_SHARP: '0.15' });
        const ctl = run(host, gpu, 'pair-l5', { TD_PAINT: 'pair', TD_VIEW: 'top', TD_SHARP5: '0.6' });
        const w05 = transition(run(host, gpu, 'pair-005', { TD_PAINT: 'pair', TD_VIEW: 'top', TD_SHARP: '0.05' }));
        const w15 = transition(base), w40 = transition(run(host, gpu, 'pair-040', { TD_PAINT: 'pair', TD_VIEW: 'top', TD_SHARP: '0.4' }));
        check(`${tag} 3 0.15 set is the default pixel for pixel`, same(base, set), same(base, set) ? 'identical' : `${(changed(base, set) * 100).toFixed(3)}% of pixels differ`);
        check(`${tag} 3 sharpness sets the transition's width`, w05 < w15 - 0.15 && w40 > w15 + 0.3, `width ${w05.toFixed(2)} m at 0.05, ${w15.toFixed(2)} at 0.15, ${w40.toFixed(2)} at 0.4`);
        const c = changed(base, ctl);
        check(`${tag} 3 CONTROL a sharpness on an unpainted layer changes nothing`, c === 0, `${(c * 100).toFixed(3)}% of pixels differ`);
      }
      if (ONLY.includes('4')) {
        const flat = run(host, gpu, 'pom-off-graze', { TD_VIEW: 'graze' });
        const zero = run(host, gpu, 'pom-0-graze', { TD_VIEW: 'graze', TD_PARALLAX: '0' });
        const on = run(host, gpu, 'pom-on-graze', { TD_VIEW: 'graze', TD_PARALLAX: '0.08' });
        const tflat = run(host, gpu, 'pom-off-top', { TD_VIEW: 'top' }), ton = run(host, gpu, 'pom-on-top', { TD_VIEW: 'top', TD_PARALLAX: '0.08' });
        const lflat = run(host, gpu, 'pom-off-low', { TD_VIEW: 'graze', TD_QUALITY: 'low' }), lon = run(host, gpu, 'pom-on-low', { TD_VIEW: 'graze', TD_QUALITY: 'low', TD_PARALLAX: '0.08' });
        const sg = shift(flat, on), st = shift(tflat, ton), sl = shift(lflat, lon);
        check(`${tag} 4 depth 0 is the flat picture pixel for pixel`, same(flat, zero), same(flat, zero) ? 'identical' : 'differs');
        check(`${tag} 4 parallax shifts texels with the view angle`, sg >= 2 && st < sg / 2, `median shift ${sg.toFixed(1)} px from the low camera, ${st.toFixed(1)} px from straight above`);
        check(`${tag} 4 CONTROL at Low (no steps) nothing shifts`, sl === 0 && same(lflat, lon), `median shift ${sl.toFixed(1)} px, ${same(lflat, lon) ? 'identical' : 'differs'}`);
      }
    } catch (e) {
      check(`${tag} runs`, false, e.message);
    }
  }
process.exit(fail ? 1 : 0);
