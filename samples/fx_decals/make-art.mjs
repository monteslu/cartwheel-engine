// Generates the fx_decals sample's decal images (CC0: made here, from code).
//   node samples/fx_decals/make-art.mjs
// Writes assets/decals/scorch.png (a dark burn mark), arrow.png (a painted
// arrow), cracks.png (branching cracks), sheet.png (two paint splats side by
// side: a decal sheet for `region`) and ring.png (a glow ring for ADD).
import { join, dirname } from 'node:path';
import { mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { writePng } from '../../tools/lib/png.mjs';

const DIR = join(dirname(fileURLToPath(import.meta.url)), 'assets', 'decals');
mkdirSync(DIR, { recursive: true });
// A stateless position hash (no cycle); it loses low bits in doubles, but changing it
// int-math-ok: changing it would change the committed art. New code: Math.imul or tools/lib/rng.mjs.
const hash = (x, y, s = 0) => { let h = (x * 374761393 + y * 668265263 + s * 2147483647) >>> 0; h = ((h ^ (h >>> 13)) * 1274126177) >>> 0; return ((h ^ (h >>> 16)) >>> 0) / 4294967296; };
const smooth = (t) => t * t * (3 - 2 * t);
// value noise on a lattice of `cell` pixels
function noise(x, y, cell, seed) {
  const gx = Math.floor(x / cell), gy = Math.floor(y / cell), fx = smooth(x / cell - gx), fy = smooth(y / cell - gy);
  const a = hash(gx, gy, seed), b = hash(gx + 1, gy, seed), c = hash(gx, gy + 1, seed), d = hash(gx + 1, gy + 1, seed);
  return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}
const clamp = (v, a = 0, b = 1) => Math.max(a, Math.min(b, v));
function image(w, h, fn) {
  const px = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const [r, g, b, a] = fn(x, y);
      const i = 4 * (y * w + x);
      px[i] = Math.round(clamp(r) * 255); px[i + 1] = Math.round(clamp(g) * 255);
      px[i + 2] = Math.round(clamp(b) * 255); px[i + 3] = Math.round(clamp(a) * 255);
    }
  return px;
}

// scorch: a dark brown blot, ragged edge, darker core (MULTIPLY: rgb is the
// factor, alpha the strength)
writePng(join(DIR, 'scorch.png'), 128, 128, image(128, 128, (x, y) => {
  const dx = (x + 0.5) / 64 - 1, dy = (y + 0.5) / 64 - 1, r = Math.hypot(dx, dy);
  const n = noise(x, y, 12, 1) * 0.6 + noise(x, y, 5, 2) * 0.4;
  const a = clamp((0.85 - r - (n - 0.5) * 0.5) * 3);
  const core = clamp(1 - r * 1.4);
  return [0.25 - core * 0.18, 0.18 - core * 0.14, 0.12 - core * 0.1, a];
}));

// arrow: a yellow painted arrow pointing up, slightly worn
writePng(join(DIR, 'arrow.png'), 128, 128, image(128, 128, (x, y) => {
  const u = (x + 0.5) / 128, v = (y + 0.5) / 128;
  const head = v < 0.45 && Math.abs(u - 0.5) < (v - 0.08) * 1.05;
  const shaft = v >= 0.42 && v < 0.92 && Math.abs(u - 0.5) < 0.12;
  const worn = noise(x, y, 4, 3) > 0.82 ? 0.3 : 1;
  return [1, 0.82, 0.1, (head || shaft) && v > 0.08 ? 0.95 * worn : 0];
}));

// cracks: random-walk branches, dark lines
{
  const w = 128, h = 128, a = new Float32Array(w * h);
  const walk = (x, y, ang, len, width, seed) => {
    for (let i = 0; i < len; i++) {
      ang += (hash(i, seed, 7) - 0.5) * 0.9;
      x += Math.cos(ang); y += Math.sin(ang);
      for (let oy = -2; oy <= 2; oy++)
        for (let ox = -2; ox <= 2; ox++) {
          const px = Math.round(x + ox), py = Math.round(y + oy);
          if (px < 0 || py < 0 || px >= w || py >= h) continue;
          const d = Math.hypot(ox, oy), v = clamp(width * (1 - i / len) + 0.6 - d);
          a[py * w + px] = Math.max(a[py * w + px], v);
        }
      if (hash(i, seed, 9) < 0.04 && len > 20) walk(x, y, ang + (hash(i, seed, 11) - 0.5) * 2.4, len * 0.5, width * 0.7, seed * 31 + i);
    }
  };
  for (let k = 0; k < 5; k++) walk(64, 64, k * 1.3 + 0.2, 60, 1.6, k + 1);
  writePng(join(DIR, 'cracks.png'), w, h, image(w, h, (x, y) => [0.08, 0.07, 0.06, a[y * w + x]]));
}

// sheet: two splats side by side (red left, blue right), each 128x128
writePng(join(DIR, 'sheet.png'), 256, 128, image(256, 128, (x, y) => {
  const cell = x < 128 ? 0 : 1, lx = x % 128;
  const dx = (lx + 0.5) / 64 - 1, dy = (y + 0.5) / 64 - 1, r = Math.hypot(dx, dy);
  const ang = Math.atan2(dy, dx);
  const lobes = 0.62 + 0.18 * Math.sin(ang * 7 + cell * 2) + (noise(lx, y, 6, 5 + cell) - 0.5) * 0.3;
  const a = clamp((lobes - r) * 6);
  return cell ? [0.15, 0.35, 1, a] : [1, 0.12, 0.1, a];
}));

// ring: a soft glowing ring (ADD)
writePng(join(DIR, 'ring.png'), 128, 128, image(128, 128, (x, y) => {
  const r = Math.hypot((x + 0.5) / 64 - 1, (y + 0.5) / 64 - 1);
  const g = Math.exp(-((r - 0.7) ** 2) / 0.006);
  return [0.3 * g, 0.9 * g, 1 * g, g];
}));
console.log('wrote', DIR);
