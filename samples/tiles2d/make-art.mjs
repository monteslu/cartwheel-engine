// Generates the tiles2d sample's tile set (CC0: made here, from code).
//   node samples/tiles2d/make-art.mjs
// Writes assets/images/tiles.png: 16x16 tiles, 8 columns. Ids 1..11 are the side-scroller's (their collision
// shapes are the scene's tile_shapes).
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writePng } from '../../tools/lib/png.mjs';
import { rng } from '../../tools/lib/rng.mjs';

const DIR = join(dirname(fileURLToPath(import.meta.url)), 'assets', 'images');
const T = 16, COLS = 8, W = COLS * T, H = 8 * T;
const px = new Uint8Array(W * H * 4);
const set = (x, y, c) => { const i = 4 * (y * W + x); px[i] = c[0]; px[i + 1] = c[1]; px[i + 2] = c[2]; px[i + 3] = c[3] ?? 255; };
// one noise table per material, so a tile and its slope share a grain
const grain = seed => { const r = rng(seed), g = []; for (let i = 0; i < T * T; i++) g.push(r()); return (x, y) => g[(y & 15) * T + (x & 15)]; };
const gStone = grain(11), gDirt = grain(12), gGrass = grain(13), gWood = grain(14);
const mix = (a, b, t) => a.map((v, i) => Math.round(v + (b[i] - v) * t));
const stone = (x, y) => {
  const seam = y % 8 === 7 || (x + (y >> 3) * 8) % 16 === 15;
  return seam ? [58, 62, 78] : mix([104, 110, 132], [136, 142, 164], gStone(x, y));
};
const dirt = (x, y) => mix([92, 62, 44], [124, 88, 58], gDirt(x, y));
const grass = (x, y) => mix([58, 150, 70], [96, 196, 92], gGrass(x, y));
// earth with a grass skin `depth` pixels deep under the surface line top(x)
const earth = top => (x, y) => { const t = top(x); if (y < t) return null; return y - t < 3 ? grass(x, y) : y - t < 4 ? mix(grass(x, y), dirt(x, y), 0.5) : dirt(x, y); };
const tile = (id, fn) => {
  const ox = (id % COLS) * T, oy = Math.floor(id / COLS) * T;
  for (let y = 0; y < T; y++) for (let x = 0; x < T; x++) { const c = fn(x, y); if (c) set(ox + x, oy + y, c); }
};
tile(1, stone);
tile(2, earth(() => 0));                       // grass on top
tile(3, dirt);
tile(4, (x, y) => { const m = y % 4 === 3 || (x + (y >> 2) * 4) % 8 === 7; return m ? [70, 40, 36] : mix([150, 74, 60], [176, 92, 70], gStone(y, x)); }); // brick
tile(5, earth(x => 15 - x));                   // slope 0 16: rises to the right
tile(6, earth(x => x));                        // slope 16 0
tile(7, earth(x => 15 - (x >> 1)));            // slope 0 8
tile(8, earth(x => 7 - (x >> 1)));             // slope 8 16
tile(9, (x, y) => y > 4 ? null : y === 0 ? [222, 176, 104] : y === 4 || x % 8 === 7 ? [92, 60, 34] : mix([168, 118, 64], [196, 144, 84], gWood(x, y))); // one-way plank
tile(10, (x, y) => y < 8 ? null : stone(x, y)); // half block
tile(11, (x, y) => Math.abs(x - 7.5) <= (15.5 - y) / 2 ? mix([150, 156, 176], [210, 214, 228], y / 15) : null); // a hanging wedge
writePng(join(DIR, 'tiles.png'), W, H, px);
console.log('wrote', join(DIR, 'tiles.png'));
