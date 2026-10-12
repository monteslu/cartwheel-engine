// Generates the render2d samples' test art (CC0: made here, from code).
//   node samples/r2d_assets/make-art.mjs      (needs ImageMagick for the JPEG)
// Writes images/sheet.png, images/tiles.png, images/photo.jpg.
import { writeFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writePng } from '../../tools/lib/png.mjs';

const DIR = join(dirname(fileURLToPath(import.meta.url)), 'images');
// A stateless position hash (no cycle); it loses low bits in doubles, but changing it
// int-math-ok: changing it would change the committed art. New code: Math.imul or tools/lib/rng.mjs.
const hash = (x, y, s = 0) => { let h = (x * 374761393 + y * 668265263 + s * 2147483647) >>> 0; h = ((h ^ (h >>> 13)) * 1274126177) >>> 0; return ((h ^ (h >>> 16)) >>> 0) / 4294967296; };

function canvas(w, h) {
  const px = new Uint8Array(w * h * 4);
  const set = (x, y, r, g, b, a = 255) => {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const i = 4 * (y * w + x); px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a;
  };
  return { w, h, px, set };
}

// ── sheet.png: critter frames, nine-slice panel, alpha gem, arrow ──────────
{
  const c = canvas(128, 64);
  for (let f = 0; f < 4; f++) {
    const ox = f * 32;
    for (let y = 0; y < 32; y++) for (let x = 0; x < 32; x++) {
      const dx = (x - 15.5) / 11, dy = (y - 14) / 9;
      if (dx * dx + dy * dy <= 1) c.set(ox + x, y, 230 - f * 20, 120 + f * 25, 40, 255);           // body
      const nx = x - 26, ny = y - 13;
      if (nx >= 0 && nx < 5 && Math.abs(ny) <= 2 - nx / 3) c.set(ox + x, y, 250, 220, 60, 255);    // beak (faces +x)
      if ((x - 20) ** 2 + (y - 10) ** 2 <= 4) c.set(ox + x, y, 255, 255, 255, 255);                 // eye
      if ((x - 21) ** 2 + (y - 10) ** 2 <= 1) c.set(ox + x, y, 20, 20, 30, 255);
    }
    const legs = [[11, 4 * Math.sin(f * Math.PI / 2)], [20, -4 * Math.sin(f * Math.PI / 2)]];
    for (const [lx, sw] of legs) for (let t = 0; t < 8; t++) {
      const x = Math.round(lx + sw * t / 8), y = 22 + t;
      c.set(ox + x, y, 90, 50, 30); c.set(ox + x + 1, y, 90, 50, 30);
    }
  }
  // nine-slice panel at (0,32) 32x32: dark outline, 6px frame, light centre, marked corners
  for (let y = 0; y < 32; y++) for (let x = 0; x < 32; x++) {
    const e = Math.min(x, y, 31 - x, 31 - y);
    let col = e === 0 ? [20, 24, 40] : e < 6 ? [70 + e * 12, 90 + e * 10, 170] : [210, 220, 240];
    if (x < 6 && y < 6 && e > 0) col = [240, 80, 80];
    if (x > 25 && y < 6 && e > 0) col = [80, 220, 80];
    if (x < 6 && y > 25 && e > 0) col = [240, 200, 60];
    if (x > 25 && y > 25 && e > 0) col = [200, 80, 240];
    c.set(x, 32 + y, ...col, 255);
  }
  // alpha gem at (32,32) 16x16: radial alpha falloff
  for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) {
    const d = Math.abs(x - 7.5) + Math.abs(y - 7.5);
    if (d <= 8) c.set(32 + x, 32 + y, 80, 200, 255, Math.round(255 * (1 - d / 8.5)));
  }
  // arrow at (64,32) 32x32 pointing up, with a flag on its left (asymmetric both ways)
  for (let y = 0; y < 32; y++) for (let x = 0; x < 32; x++) {
    const shaft = x >= 14 && x <= 17 && y >= 8 && y <= 29;
    const head = y >= 2 && y < 12 && Math.abs(x - 15.5) <= (y - 2) * 0.9;
    const flag = x >= 4 && x < 14 && y >= 18 && y <= 23;
    if (shaft || head) c.set(64 + x, 32 + y, 250, 250, 250, 255);
    if (flag) c.set(64 + x, 32 + y, 240, 60, 90, 255);
  }
  writePng(join(DIR, 'sheet.png'), c.w, c.h, c.px);
}

// ── tiles.png: 16x16 tiles, 8 columns ───────────────────────────────────
{
  const T = 16, c = canvas(128, 64);
  const tile = (id, fn) => {
    const ox = (id % 8) * T, oy = Math.floor(id / 8) * T;
    for (let y = 0; y < T; y++) for (let x = 0; x < T; x++) { const v = fn(x, y); if (v) c.set(ox + x, oy + y, ...v); }
  };
  tile(0, (x, y) => { const n = hash(x, y, 1); return [50 + n * 30, 140 + n * 50, 50, 255]; });               // grass
  tile(1, (x, y) => { const n = hash(x, y, 2); return [120 + n * 30, 85 + n * 20, 50, 255]; });               // dirt
  tile(2, (x, y) => { const m = y % 8 === 7 || (x + (y >> 3) * 8) % 16 === 15; return m ? [70, 70, 80, 255] : [150 + hash(x, y, 3) * 30, 150, 160, 255]; }); // brick
  tile(3, (x, y) => (x === 3 && y >= 2 && y <= 13) || (y === 2 && x >= 3 && x <= 12) || (y === 7 && x >= 3 && x <= 9) ? [255, 230, 60, 255] : [40, 40, 90, 255]); // "F": shows any flip/rotation
  for (let f = 0; f < 4; f++) tile(4 + f, (x, y) => { const w = Math.sin((x + f * 4) / 16 * 2 * Math.PI) * 2; return Math.abs(y - 8 - w) < 1.5 ? [200, 230, 255, 255] : [30, 90 + y * 4, 200, 255]; }); // water, 4 frames
  tile(8, (x, y) => { const d = (x - 7.5) ** 2 + (y - 7.5) ** 2; return d < 6 ? [255, 240, 80, 255] : d < 20 && (Math.atan2(y - 7.5, x - 7.5) * 5 / Math.PI + 10) % 2 < 1.2 ? [240, 80, 160, 255] : null; }); // flower (transparent)
  tile(9, (x, y) => (x + y) % 6 === 0 ? [255, 255, 255, 160] : null);                                          // sparkle overlay
  for (let f = 0; f < 2; f++) tile(10 + f, (x, y) => [200 + f * 50, 60 + f * 80 + hash(x, y, f + 9) * 40, 20, 255]); // lava, 2 frames
  writePng(join(DIR, 'tiles.png'), c.w, c.h, c.px);
}

// ── photo.jpg: a smooth pattern through ImageMagick (exercises the JPEG path) ──
{
  const c = canvas(64, 64);
  for (let y = 0; y < 64; y++) for (let x = 0; x < 64; x++) {
    const d = Math.hypot(x - 32, y - 32) / 32;
    c.set(x, y, Math.round(255 * x / 63), Math.round(255 * (1 - d) ** 0.5 || 0), Math.round(255 * y / 63), 255);
  }
  const tmp = join(DIR, 'photo.tmp.png');
  writePng(tmp, c.w, c.h, c.px);
  execFileSync('convert', [tmp, '-quality', '92', join(DIR, 'photo.jpg')]);
  execFileSync('rm', [tmp]);
}
console.log('wrote', DIR);
