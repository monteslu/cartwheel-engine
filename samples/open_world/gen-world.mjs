// open_world's world: a 32 x 32 grid of 512 m cells (16.4 km square) of
// generated terrain, a road across all of it and props along it, one
// zstd-compressed file per cell. Everything here is made by this script (no
// third-party content) and is a pure function of the constants below, so
// every run writes the same bytes.
//
//   node samples/open_world/gen-world.mjs <out dir> [--cells N]
//
// Writes <out>/world/c_<x>_<z>.cell.zst, <out>/world/road.bin and copies
// the scene (samples/open_world/assets/*) beside them; <out>/stamp last.
//
// A cell file (little-endian), compressed whole with zstd:
//   0   "CWCL"
//   4   u32 version (1)
//   8   i32 cx, i32 cz
//   16  u32 n: samples per side (cell quads + 1, shared with the neighbours)
//   20  f32 spacing (m), f32 min_height, f32 max_height
//   32  u32 prop count, u32 reserved
//   40  heights: n * n u16 steps (min..max), each row delta-coded (the first
//       sample raw, then the difference from the previous one, mod 65536)
//       control: n * n u8: base layer (bits 0-1), overlay (2-3), blend (4-7, /15)
//       props: count x 32 bytes {u32 kind, f32 x, f32 z (cell-local metres),
//       f32 yaw, f32 sx, f32 sy, f32 sz, u32 seed}
// road.bin: u32 count, then count x {f64 x, f64 y, f64 z}: the road's centre
// line in world metres (y = the road surface), 8 m apart.
import { writeFileSync, mkdirSync, readdirSync, copyFileSync, statSync } from 'node:fs';
import { zstdCompressSync, constants } from 'node:zlib';
import { dirname, join } from 'node:path';
import { Worker, isMainThread, workerData, parentPort } from 'node:worker_threads';
import { availableParallelism } from 'node:os';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const args = isMainThread ? process.argv.slice(2) : workerData.args;
const OUT = args[0];
if (!OUT) { console.error('usage: gen-world.mjs <out dir> [--cells N]'); process.exit(2); }
const ci = args.indexOf('--cells');
export const CELLS = ci >= 0 ? +args[ci + 1] : 32;
export const QUADS = 256, SPACING = 2, CELL_M = QUADS * SPACING; // 512 m
export const N = QUADS + 1;
export const ORIGIN = -(CELLS * CELL_M) / 2;
export const MIN_H = -40, MAX_H = 560;
const STEP = (MAX_H - MIN_H) / 65535;

// ── noise: value noise on an integer lattice, hashed with Math.imul ──────
function hash2(x, z, seed) {
  let h = Math.imul(x | 0, 0x85ebca77) ^ Math.imul(z | 0, 0xc2b2ae3d) ^ Math.imul(seed, 0x9e3779b1);
  h ^= h >>> 15; h = Math.imul(h, 0x2c1b3c6d); h ^= h >>> 12; h = Math.imul(h, 0x297a2d39); h ^= h >>> 15;
  return (h >>> 8) / 16777216;
}
function vnoise(x, z, seed) {
  const ix = Math.floor(x), iz = Math.floor(z);
  let u = x - ix, v = z - iz;
  u = u * u * (3 - 2 * u); v = v * v * (3 - 2 * v);
  const a = hash2(ix, iz, seed), b = hash2(ix + 1, iz, seed), c = hash2(ix, iz + 1, seed), d = hash2(ix + 1, iz + 1, seed);
  return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}
function fbm(x, z, oct, seed) {
  let s = 0, a = 0.5, n = 0, f = 1;
  for (let o = 0; o < oct; o++) { s += vnoise(x * f, z * f, seed + o) * a; n += a; a *= 0.5; f *= 2.03; }
  return s / n;
}
function ridged(x, z, oct, seed) {
  let s = 0, a = 0.5, n = 0, f = 1;
  for (let o = 0; o < oct; o++) { const r = 1 - Math.abs(vnoise(x * f, z * f, seed + o) * 2 - 1); s += r * r * a; n += a; a *= 0.5; f *= 2.1; }
  return s / n;
}
const smooth = (e0, e1, x) => { const t = Math.min(1, Math.max(0, (x - e0) / (e1 - e0))); return t * t * (3 - 2 * t); };

// the landscape before the road: plains, rolling hills, a mountain range to the north-east, lakes
function baseHeight(x, z) {
  const c = fbm(x / 5200, z / 5200, 3, 11);                 // continental: lowlands vs uplands
  const hills = fbm(x / 900, z / 900, 4, 31);
  const range = smooth(0.15, 0.55, (x + z) / 16000 + (fbm(x / 3000, z / 3000, 2, 51) - 0.5) * 0.6);
  const mount = ridged(x / 1700, z / 1700, 4, 71);
  let h = 30 + (c - 0.4) * 160 + (hills - 0.5) * 70 * (0.4 + c) + range * mount * mount * 420;
  h += (fbm(x / 140, z / 140, 2, 91) - 0.5) * 6;              // detail
  return h;
}

// ── the road: a meandering line from west to east, its height smoothed ──
function makeRoad() {
  const x0 = ORIGIN + 300, x1 = -ORIGIN - 300, pts = [];
  const ctrl = [];
  const span = x1 - x0, nc = Math.max(4, Math.round(span / 900));
  for (let i = 0; i <= nc; i++) {
    const x = x0 + (span * i) / nc;
    const z = (hash2(i, 7, 1234) - 0.5) * Math.min(5200, -ORIGIN * 0.9) + Math.sin(i * 0.9) * 600;
    ctrl.push([x, z]);
  }
  // Catmull-Rom through the control points, then resampled every 8 m
  const dense = [];
  for (let i = 0; i < ctrl.length - 1; i++) {
    const p0 = ctrl[Math.max(0, i - 1)], p1 = ctrl[i], p2 = ctrl[i + 1], p3 = ctrl[Math.min(ctrl.length - 1, i + 2)];
    for (let k = 0; k < 64; k++) {
      const t = k / 64, t2 = t * t, t3 = t2 * t;
      const f = (a, b, c, d) => 0.5 * (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (-a + 3 * b - 3 * c + d) * t3);
      dense.push([f(p0[0], p1[0], p2[0], p3[0]), f(p0[1], p1[1], p2[1], p3[1])]);
    }
  }
  dense.push(ctrl[ctrl.length - 1]);
  let acc = 0;
  pts.push(dense[0]);
  for (let i = 1; i < dense.length; i++) {
    const a = dense[i - 1], b = dense[i], l = Math.hypot(b[0] - a[0], b[1] - a[1]);
    acc += l;
    while (acc >= 8) {
      acc -= 8;
      const u = 1 - acc / l;
      pts.push([a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u]);
    }
  }
  // the surface: the ground smoothed along the road over +-200 m, never below the lakes' level
  const g = pts.map(([x, z]) => Math.max(baseHeight(x, z), 4));
  let y = g;
  for (let pass = 0; pass < 3; pass++) {
    const out = new Array(y.length);
    for (let i = 0; i < y.length; i++) {
      let s = 0, n = 0;
      for (let k = -25; k <= 25; k++) { const j = Math.min(y.length - 1, Math.max(0, i + k)); s += y[j]; n++; }
      out[i] = s / n;
    }
    y = out;
  }
  return pts.map(([x, z], i) => [x, y[i], z]);
}

const ROAD_HALF = 5, ROAD_EDGE = 18;

// the road segments near a rectangle, for the per-sample distance
function segmentsNear(road, x0, z0, x1, z1, pad) {
  const out = [];
  for (let i = 0; i < road.length - 1; i++) {
    const a = road[i], b = road[i + 1];
    if (Math.max(a[0], b[0]) < x0 - pad || Math.min(a[0], b[0]) > x1 + pad) continue;
    if (Math.max(a[2], b[2]) < z0 - pad || Math.min(a[2], b[2]) > z1 + pad) continue;
    out.push(i);
  }
  return out;
}

function roadAt(road, segs, x, z) {
  let best = Infinity, by = 0, bi = -1;
  for (const i of segs) {
    const a = road[i], b = road[i + 1];
    const ex = b[0] - a[0], ez = b[2] - a[2], l2 = ex * ex + ez * ez;
    let u = l2 > 0 ? ((x - a[0]) * ex + (z - a[2]) * ez) / l2 : 0;
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    const qx = a[0] + ex * u - x, qz = a[2] + ez * u - z, d2 = qx * qx + qz * qz;
    if (d2 < best) { best = d2; by = a[1] + (b[1] - a[1]) * u; bi = i; }
  }
  return { d: Math.sqrt(best), y: by, i: bi };
}

function quant(h) {
  const c = Math.min(MAX_H, Math.max(MIN_H, h));
  return Math.min(65535, Math.floor((c - MIN_H) / STEP));
}

function makeCell(cx, cz, road) {
  const wx0 = ORIGIN + cx * CELL_M, wz0 = ORIGIN + cz * CELL_M;
  const segs = segmentsNear(road, wx0, wz0, wx0 + CELL_M, wz0 + CELL_M, ROAD_HALF + ROAD_EDGE + 4);
  // heights: the base on a 8 m grid (bilinear between), plus 2 m detail, then the road cut
  const C = QUADS / 4 + 1, coarse = new Float64Array(C * C);
  for (let j = 0; j < C; j++) for (let i = 0; i < C; i++) coarse[j * C + i] = baseHeight(wx0 + i * 8, wz0 + j * 8);
  const h = new Float64Array(N * N), rd = new Float64Array(N * N).fill(Infinity);
  for (let j = 0; j < N; j++)
    for (let i = 0; i < N; i++) {
      const fi = i / 4, fj = j / 4, ii = Math.min(C - 2, Math.floor(fi)), jj = Math.min(C - 2, Math.floor(fj));
      const u = fi - ii, v = fj - jj;
      const a = coarse[jj * C + ii], b = coarse[jj * C + ii + 1], c = coarse[(jj + 1) * C + ii], d = coarse[(jj + 1) * C + ii + 1];
      const x = wx0 + i * SPACING, z = wz0 + j * SPACING;
      let y = a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v + (vnoise(x / 9, z / 9, 101) - 0.5) * 0.6;
      if (segs.length) {
        const r = roadAt(road, segs, x, z);
        rd[j * N + i] = r.d;
        const k = r.d <= ROAD_HALF ? 1 : r.d >= ROAD_HALF + ROAD_EDGE ? 0 : 1 - smooth(ROAD_HALF, ROAD_HALF + ROAD_EDGE, r.d);
        y = y + (r.y - y) * k;
      }
      h[j * N + i] = y;
    }
  const q = new Uint16Array(N * N);
  for (let k = 0; k < N * N; k++) q[k] = quant(h[k]);
  // control: grass, rock on steep ground, dirt on the road and near water, snow up high
  const ctl = new Uint8Array(N * N);
  for (let j = 0; j < N; j++)
    for (let i = 0; i < N; i++) {
      const at = (ii, jj) => h[Math.min(N - 1, Math.max(0, jj)) * N + Math.min(N - 1, Math.max(0, ii))];
      const dx = (at(i + 1, j) - at(i - 1, j)) / (2 * SPACING), dz = (at(i, j + 1) - at(i, j - 1)) / (2 * SPACING);
      const ny = 1 / Math.sqrt(dx * dx + dz * dz + 1), y = h[j * N + i];
      const w = [0, Math.min(1, Math.max(0, (0.86 - ny) / 0.1)), 0, 0];
      w[2] = Math.min(1, Math.max(0, (6 - y) / 5)) * (1 - w[1]);
      const r = rd[j * N + i];
      if (r < ROAD_HALF + 1.5) w[2] = Math.max(w[2], r < ROAD_HALF - 0.5 ? 1 : (ROAD_HALF + 1.5 - r) / 2);
      w[3] = Math.min(1, Math.max(0, (y - 330) / 40)) * (ny > 0.8 ? 1 : 0.3);
      w[0] = Math.max(0, 1 - w[1] - w[2]) * (1 - w[3]);
      let b0 = 0;
      for (let k = 1; k < 4; k++) if (w[k] > w[b0]) b0 = k;
      let b1 = b0 === 0 ? 1 : 0;
      for (let k = 0; k < 4; k++) if (k !== b0 && w[k] > w[b1]) b1 = k;
      const s = w[b0] + w[b1], blend = s > 0 ? Math.round((w[b1] / s) * 15) : 0;
      ctl[j * N + i] = b0 | (b1 << 2) | (blend << 4);
    }
  // props: houses beside the road, stone towers on high ground, boulders
  const props = [];
  const rnd = (k) => hash2(cx * 97 + k, cz * 89 - k, 4242);
  if (segs.length) {
    for (let k = 0; k < 6; k++) {
      const s = segs[Math.floor(rnd(k) * segs.length)], a = road[s], b = road[s + 1];
      const ex = b[0] - a[0], ez = b[2] - a[2], l = Math.hypot(ex, ez) || 1;
      const side = rnd(k + 20) < 0.5 ? -1 : 1, off = ROAD_HALF + ROAD_EDGE + 6 + rnd(k + 40) * 20;
      const x = a[0] - (ez / l) * off * side, z = a[2] + (ex / l) * off * side;
      if (x < wx0 + 8 || z < wz0 + 8 || x > wx0 + CELL_M - 8 || z > wz0 + CELL_M - 8) continue;
      props.push([0, x - wx0, z - wz0, Math.atan2(ex, ez), 6 + rnd(k + 60) * 6, 4 + rnd(k + 70) * 3, 8 + rnd(k + 80) * 6]);
    }
  }
  for (let k = 0; k < 3; k++) {
    const x = 20 + rnd(k + 100) * (CELL_M - 40), z = 20 + rnd(k + 120) * (CELL_M - 40);
    const ii = Math.round(x / SPACING), jj = Math.round(z / SPACING), y = h[jj * N + ii];
    if (rd[jj * N + ii] < 40) continue;
    if (y > 120 && rnd(k + 140) < 0.7) props.push([1, x, z, rnd(k + 160) * 6.283, 3, 14 + rnd(k + 180) * 10, 3]);
    else props.push([2, x, z, rnd(k + 160) * 6.283, 2 + rnd(k + 200) * 3, 1.5 + rnd(k + 220) * 2, 2 + rnd(k + 240) * 3]);
  }
  // the bytes
  const head = 40, body = N * N * 2 + N * N, out = Buffer.alloc(head + body + props.length * 32);
  out.write('CWCL', 0);
  out.writeUInt32LE(1, 4); out.writeInt32LE(cx, 8); out.writeInt32LE(cz, 12); out.writeUInt32LE(N, 16);
  out.writeFloatLE(SPACING, 20); out.writeFloatLE(MIN_H, 24); out.writeFloatLE(MAX_H, 28);
  out.writeUInt32LE(props.length, 32);
  let o = head;
  for (let j = 0; j < N; j++) {
    let prev = 0;
    for (let i = 0; i < N; i++) { const v = q[j * N + i]; out.writeUInt16LE(i ? (v - prev) & 0xffff : v, o); o += 2; prev = v; }
  }
  for (let k = 0; k < N * N; k++) out[o++] = ctl[k];
  props.forEach((p, k) => {
    out.writeUInt32LE(p[0], o); for (let m = 1; m < 7; m++) out.writeFloatLE(p[m], o + 4 * m);
    out.writeUInt32LE((Math.imul(cx * 131 + cz, 0x9e3779b1) + k) >>> 0, o + 28); o += 32;
  });
  return zstdCompressSync(out, { params: { [constants.ZSTD_c_compressionLevel]: 15, [constants.ZSTD_c_checksumFlag]: 0 } });
}

const t0 = Date.now();
const road = makeRoad();
if (!isMainThread) {
  // a worker: the cells whose id % workers is its index
  let bytes = 0;
  for (let id = workerData.k; id < CELLS * CELLS; id += workerData.w) {
    const z = makeCell(id % CELLS, Math.floor(id / CELLS), road);
    bytes += z.length;
    writeFileSync(join(OUT, 'world', `c_${id % CELLS}_${Math.floor(id / CELLS)}.cell.zst`), z);
  }
  parentPort.postMessage(bytes);
} else {
  mkdirSync(join(OUT, 'world'), { recursive: true });
  const rb = Buffer.alloc(4 + road.length * 24);
  rb.writeUInt32LE(road.length, 0);
  road.forEach((p, i) => { rb.writeDoubleLE(p[0], 4 + i * 24); rb.writeDoubleLE(p[1], 12 + i * 24); rb.writeDoubleLE(p[2], 20 + i * 24); });
  writeFileSync(join(OUT, 'world', 'road.bin'), rb);
  const w = Math.max(1, Math.min(8, availableParallelism() - 1));
  const sizes = await Promise.all(Array.from({ length: w }, (_, k) => new Promise((res, rej) => {
    const wk = new Worker(new URL(import.meta.url), { workerData: { args, k, w } });
    wk.on('message', res);
    wk.on('error', rej);
  })));
  const bytes = sizes.reduce((a, b) => a + b, 0);
  // the scene and anything else in assets/
  const A = join(HERE, 'assets');
  for (const f of readdirSync(A)) if (statSync(join(A, f)).isFile()) copyFileSync(join(A, f), join(OUT, f));
  writeFileSync(join(OUT, 'stamp'), `${CELLS}\n`);
  console.log(`open_world: ${CELLS * CELLS} cells (${((CELLS * CELL_M) / 1000).toFixed(1)} km square), ${(bytes / 1048576).toFixed(1)} MB, road ${road.length} points, ${((Date.now() - t0) / 1000).toFixed(1)} s`);
}
