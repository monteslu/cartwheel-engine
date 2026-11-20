// Generates the tiles2d sample's tile set (CC0: made here, from code).
//   node samples/tiles2d/make-art.mjs
// Writes assets/images/tiles.png: 16x16 tiles, 16 columns. Ids 1..11 are the side-scroller's (their collision
// shapes are the scene's tile_shapes). The top-down room's terrains, each in its kind's tile order
// (cartwheel/scene2d.h "terrains"): floor 12..15 (single, 4 variants), wall 16..62 and 63..109 (blob47, 2
// variants), moss 112..127 (corner16), fence 128..143 (edge16).
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writePng } from '../../tools/lib/png.mjs';
import { rng } from '../../tools/lib/rng.mjs';

const DIR = join(dirname(fileURLToPath(import.meta.url)), 'assets', 'images');
const T = 16, COLS = 16, W = COLS * T, H = 9 * T;
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

// ── the room's terrains ──────────────────────────────────────────────────────────────────────────────────
// floor: flagstones, four variants (a crack or a chip here and there)
for (let v = 0; v < 4; v++) {
  const g = grain(40 + v), r = rng(90 + v), marks = [];
  for (let i = 0; i < v; i++) marks.push([2 + Math.floor(r() * 11), 2 + Math.floor(r() * 11)]);
  tile(12 + v, (x, y) => {
    const seam = x === 0 || y === 0;
    const c = mix([66, 62, 74], [84, 80, 94], g(x, y));
    if (seam) return mix(c, [40, 38, 48], 0.7);
    return marks.some(([mx, my]) => Math.abs(x - mx) + Math.abs(y - my) <= 1) ? mix(c, [44, 42, 52], 0.6) : c;
  });
}
// the 47 blob masks in ascending order: a corner only counts between two set sides
const reduce = m => { if (!((m & 1) && (m & 4))) m &= ~2; if (!((m & 4) && (m & 16))) m &= ~8; if (!((m & 16) && (m & 64))) m &= ~32; if (!((m & 64) && (m & 1))) m &= ~128; return m; };
const BLOB = []; for (let m = 0; m < 256; m++) if (reduce(m) === m) BLOB.push(m);
if (BLOB.length !== 47) throw new Error('blob set');
// wall: a raised stone top seen from above, with a dark rim 3 pixels wide wherever no wall continues
for (let v = 0; v < 2; v++) {
  const g = grain(60 + v);
  BLOB.forEach((m, i) => tile(16 + v * 47 + i, (x, y) => {
    const n = m & 1, ne = m & 2, e = m & 4, se = m & 8, s2 = m & 16, sw = m & 32, w = m & 64, nw = m & 128;
    const L = x < 3, R = x > 12, U = y < 3, D = y > 12;
    let rim = (U && !n) || (D && !s2) || (L && !w) || (R && !e);
    rim = rim || (U && R && !ne) || (D && R && !se) || (D && L && !sw) || (U && L && !nw);
    if (rim) {
      const outer = (y === 0 && !n) || (y === 15 && !s2) || (x === 0 && !w) || (x === 15 && !e);
      return outer ? [30, 34, 46] : (D && !s2) ? [70, 76, 96] : [98, 106, 130];
    }
    const brick = (y + v * 3) % 6 === 5 || (x + ((y + v * 3) / 6 | 0) * 5) % 10 === 9;
    return brick ? [120, 128, 150] : mix([146, 154, 178], [170, 178, 200], g(x, y));
  }));
}
// moss: corner16. A pixel is moss where the bilinear blend of the four corners passes a half (the corner bits:
// north-east 1, south-east 2, south-west 4, north-west 8), with a ragged edge
{
  const g = grain(77);
  for (let k = 1; k < 16; k++) tile(112 + k, (x, y) => {
    const u = (x + 0.5) / 16, w = (y + 0.5) / 16;
    const ne = k & 1 ? 1 : 0, se = k & 2 ? 1 : 0, sw = k & 4 ? 1 : 0, nw = k & 8 ? 1 : 0;
    const val = nw * (1 - u) * (1 - w) + ne * u * (1 - w) + sw * (1 - u) * w + se * u * w + (g(x, y) - 0.5) * 0.12;
    if (val <= 0.5) return null;
    return val < 0.58 ? [38, 92, 60, 255] : mix([52, 128, 76], [78, 160, 92], g(y, x));
  });
}
// fence: edge16. A post, and a rail toward every side that continues (north 1, east 2, south 4, west 8)
for (let k = 0; k < 16; k++) tile(128 + k, (x, y) => {
  const post = x >= 5 && x <= 10 && y >= 5 && y <= 10;
  const rail = ((k & 1) && x >= 7 && x <= 8 && y < 5) || ((k & 4) && x >= 7 && x <= 8 && y > 10) ||
               ((k & 8) && y >= 7 && y <= 8 && x < 5) || ((k & 2) && y >= 7 && y <= 8 && x > 10);
  if (post) return (x === 5 || y === 5) ? [214, 170, 96] : (x === 10 || y === 10) ? [96, 62, 34] : [168, 118, 64];
  return rail ? [140, 96, 52] : null;
});
writePng(join(DIR, 'tiles.png'), W, H, px);
console.log('wrote', join(DIR, 'tiles.png'));
