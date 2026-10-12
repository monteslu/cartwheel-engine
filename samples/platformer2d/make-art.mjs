// Generates the platformer2d sample's art (CC0: made here, from code).
//   node samples/platformer2d/make-art.mjs
// Writes assets/images/tiles.png (16x16 tiles), player.png (8 frames of
// 16x24) and coin.png (4 frames of 12x12).
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writePng } from '../../tools/lib/png.mjs';

const DIR = join(dirname(fileURLToPath(import.meta.url)), 'assets', 'images');
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

// ── tiles.png: 8 x 2 tiles of 16x16 ─────────────────────────────────────
{
  const c = canvas(128, 32);
  const tile = (id, fn) => { const ox = (id % 8) * 16, oy = Math.floor(id / 8) * 16; for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) { const col = fn(x, y); if (col) c.set(ox + x, oy + y, ...col); } };
  const dirt = (x, y, s) => { const n = hash(x, y, s); return [120 + n * 30 | 0, 78 + n * 20 | 0, 46 + n * 10 | 0]; };
  tile(0, (x, y) => y < 4 + (hash(x, 0, 3) < 0.3 ? 1 : 0) ? [70 + hash(x, y, 1) * 40 | 0, 170 + hash(x, y, 2) * 50 | 0, 60] : dirt(x, y, 0)); // grass top
  tile(1, (x, y) => dirt(x, y, 5));                                                                                                       // dirt
  tile(2, (x, y) => { const row = y >> 3, bx = (x + (row & 1) * 4) & 7; const m = (y & 7) === 7 || bx === 7; const n = hash(x, y, 9) * 25; return m ? [60, 60, 70] : [130 + n | 0, 130 + n | 0, 145 + n | 0]; }); // stone
  for (let f = 0; f < 2; f++) tile(3 + f, (x, y) => { const w = Math.sin((x + f * 4) * Math.PI / 4) * 1.5; return y < 3 + w ? [150, 200, 255, 200] : [40, 90 + y * 3, 200, 210]; }); // water A/B
  tile(5, (x, y) => { const e = Math.min(x, y, 15 - x, 15 - y); if (e === 0) return [70, 40, 20]; if (e < 2 || x === y || x === 15 - y) return [160, 100, 40]; return [200, 140, 70]; }); // crate
  tile(6, (x, y) => { const dx = (x - 7.5) / 7.5, dy = (y - 15) / 10; return dx * dx + dy * dy <= 1 ? [40, 120 + hash(x, y, 4) * 40 | 0, 50] : null; }); // bush
  tile(7, (x, y) => { if (x === 7 && y > 8) return [50, 140, 50]; const d = (x - 7) ** 2 + (y - 6) ** 2; return d <= 2 ? [250, 220, 60] : d <= 8 ? [230, 80, 120] : null; }); // flower
  tile(8, (x, y) => { const dx = (x - 7.5) / 8, dy = (y - 9) / 6; return dx * dx + dy * dy <= 1 || ((x - 4) / 4) ** 2 + ((y - 7) / 4) ** 2 <= 1 ? [255, 255, 255, 230] : null; }); // cloud
  tile(9, (x, y) => (x >= 7 && x <= 8 && y > 4) ? [90, 60, 30] : (y <= 6 && Math.abs(x - 7.5) < 3 + (6 - y) * 0.2 && y > 1) ? [255, 160 + (y * 15), 40] : null); // torch
  writePng(join(DIR, 'tiles.png'), c.w, c.h, c.px);
}

// ── player.png: idle x2, run x4, jump, fall (16x24 each) ────────────────
{
  const c = canvas(128, 24);
  for (let f = 0; f < 8; f++) {
    const ox = f * 16;
    const bob = f < 2 ? f : f < 6 ? ((f - 2) & 1) : 0;
    for (let y = 0; y < 24; y++) for (let x = 0; x < 16; x++) {
      const yy = y - bob;
      if (yy >= 2 && yy < 9 && x >= 4 && x < 12) c.set(ox + x, y, 240, 200, 160);                // head
      if (yy >= 2 && yy < 4 && x >= 3 && x < 13) c.set(ox + x, y, 200, 50, 50);                  // cap
      if (yy === 5 && x === 9) c.set(ox + x, y, 20, 20, 40);                                      // eye (faces +x)
      if (yy >= 9 && yy < 17 && x >= 4 && x < 12) c.set(ox + x, y, 50, 90, 200);                 // body
    }
    // legs
    const ph = f >= 2 && f < 6 ? (f - 2) * Math.PI / 2 : f === 6 ? 0.8 : f === 7 ? -0.6 : 0;
    const legs = [[6, Math.sin(ph) * 3], [10, -Math.sin(ph) * 3]];
    for (const [lx, sw] of legs) for (let t = 0; t < 7; t++) {
      const x = Math.round(lx + sw * t / 7), y = 17 + t;
      c.set(ox + x, y, 60, 40, 30); c.set(ox + x - 1, y, 60, 40, 30);
    }
  }
  writePng(join(DIR, 'player.png'), c.w, c.h, c.px);
}

// ── coin.png: 4 spin frames of 12x12 ────────────────────────────────────
{
  const c = canvas(48, 12);
  const widths = [5.5, 3.5, 1, 3.5];
  for (let f = 0; f < 4; f++) for (let y = 0; y < 12; y++) for (let x = 0; x < 12; x++) {
    const dx = (x - 5.5) / Math.max(widths[f], 0.8), dy = (y - 5.5) / 5.5;
    const d = dx * dx + dy * dy;
    if (d <= 1) c.set(f * 12 + x, y, d > 0.6 ? 200 : 255, d > 0.6 ? 150 : 215, 40);
  }
  writePng(join(DIR, 'coin.png'), c.w, c.h, c.px);
}
