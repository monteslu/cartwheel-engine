// Generates the visibility samples' tree model (CC0: made here, from code).
//   node samples/vis_city/project/gen.mjs
// Writes models/tree.glb: one mesh, two primitives (a tapered trunk and a
// lumpy two-lobed crown, ~2.7k triangles) with plain PBR materials. cwpack
// imports it with simplified LODs (roots.txt), which LOD selection and
// impostors exercise.
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const D = dirname(fileURLToPath(import.meta.url));
// A stateless position hash (no cycle); it loses low bits in doubles, but changing it
// int-math-ok: changing it would change the committed art. New code: Math.imul or tools/lib/rng.mjs.
const hash = (x, y, z) => { let h = (x * 374761393 + y * 668265263 + z * 1274126177) >>> 0; h = ((h ^ (h >>> 13)) * 1103515245) >>> 0; return ((h ^ (h >>> 16)) >>> 0) / 4294967296; };
const smooth = (t) => t * t * (3 - 2 * t);
function noise3(x, y, z) {
  const ix = Math.floor(x), iy = Math.floor(y), iz = Math.floor(z);
  const fx = smooth(x - ix), fy = smooth(y - iy), fz = smooth(z - iz);
  let v = 0;
  for (let k = 0; k < 8; k++) {
    const dx = k & 1, dy = (k >> 1) & 1, dz = (k >> 2) & 1;
    v += hash(ix + dx, iy + dy, iz + dz) * (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dz ? fz : 1 - fz);
  }
  return v;
}

// an icosphere (subdivided icosahedron) of radius 1
function icosphere(level) {
  const t = (1 + Math.sqrt(5)) / 2;
  let v = [[-1, t, 0], [1, t, 0], [-1, -t, 0], [1, -t, 0], [0, -1, t], [0, 1, t], [0, -1, -t], [0, 1, -t], [t, 0, -1], [t, 0, 1], [-t, 0, -1], [-t, 0, 1]]
    .map((p) => { const l = Math.hypot(...p); return p.map((c) => c / l); });
  let f = [[0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11], [1, 5, 9], [5, 11, 4], [11, 10, 2], [10, 7, 6], [7, 1, 8],
    [3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9], [4, 9, 5], [2, 4, 11], [6, 2, 10], [8, 6, 7], [9, 8, 1]];
  for (let l = 0; l < level; l++) {
    const mid = new Map(), nf = [];
    const m = (a, b) => {
      const k = a < b ? `${a},${b}` : `${b},${a}`;
      if (!mid.has(k)) { const p = v[a].map((c, i) => (c + v[b][i]) / 2), len = Math.hypot(...p); v.push(p.map((c) => c / len)); mid.set(k, v.length - 1); }
      return mid.get(k);
    };
    for (const [a, b, c] of f) { const ab = m(a, b), bc = m(b, c), ca = m(c, a); nf.push([a, ab, ca], [b, bc, ab], [c, ca, bc], [ab, bc, ca]); }
    f = nf;
  }
  return { v, f };
}

function normals(pos, idx) {
  const n = new Float32Array(pos.length);
  for (let i = 0; i < idx.length; i += 3) {
    const [a, b, c] = [idx[i] * 3, idx[i + 1] * 3, idx[i + 2] * 3];
    const e1 = [pos[b] - pos[a], pos[b + 1] - pos[a + 1], pos[b + 2] - pos[a + 2]];
    const e2 = [pos[c] - pos[a], pos[c + 1] - pos[a + 1], pos[c + 2] - pos[a + 2]];
    const fn = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]];
    for (const o of [a, b, c]) for (let k = 0; k < 3; k++) n[o + k] += fn[k];
  }
  for (let i = 0; i < n.length; i += 3) { const l = Math.hypot(n[i], n[i + 1], n[i + 2]) || 1; n[i] /= l; n[i + 1] /= l; n[i + 2] /= l; }
  return n;
}

// trunk: a tapered open cylinder, 12 segments, 4 rings
const trunkPos = [], trunkIdx = [];
const SEG = 12, RINGS = 4;
for (let r = 0; r <= RINGS; r++) {
  const y = (r / RINGS) * 1.8, rad = 0.16 - 0.07 * (r / RINGS);
  for (let s = 0; s < SEG; s++) { const a = (s / SEG) * Math.PI * 2; trunkPos.push(Math.cos(a) * rad, y, Math.sin(a) * rad); }
}
for (let r = 0; r < RINGS; r++)
  for (let s = 0; s < SEG; s++) {
    const a = r * SEG + s, b = r * SEG + ((s + 1) % SEG), c = a + SEG, d = b + SEG;
    trunkIdx.push(a, c, b, b, c, d);
  }

// crown: two displaced icospheres (a big lobe and a side lobe)
const crownPos = [], crownIdx = [];
for (const [cx, cy, cz, R, seed] of [[0, 2.5, 0, 1.15, 1], [0.55, 2.05, 0.35, 0.7, 2]]) {
  const { v, f } = icosphere(3);
  const base = crownPos.length / 3;
  for (const p of v) {
    const n = noise3(p[0] * 2.2 + seed * 7, p[1] * 2.2, p[2] * 2.2) * 0.6 + noise3(p[0] * 5 + seed, p[1] * 5, p[2] * 5) * 0.4;
    const s = R * (0.82 + 0.36 * n);
    crownPos.push(cx + p[0] * s, cy + p[1] * s * 0.9, cz + p[2] * s);
  }
  for (const [a, b, c] of f) crownIdx.push(base + a, base + c, base + b);
}
// icosphere faces wind clockwise seen from outside with this index order: flip back
for (let i = 0; i < crownIdx.length; i += 3) { const t = crownIdx[i + 1]; crownIdx[i + 1] = crownIdx[i + 2]; crownIdx[i + 2] = t; }

const prims = [
  { pos: new Float32Array(trunkPos), idx: new Uint16Array(trunkIdx), mat: 0 },
  { pos: new Float32Array(crownPos), idx: new Uint16Array(crownIdx), mat: 1 },
];
for (const p of prims) p.nrm = normals(p.pos, p.idx);

// glb
const chunks = [];
let offset = 0;
const views = [], accessors = [];
const addView = (arr, target) => {
  const buf = Buffer.from(arr.buffer, arr.byteOffset, arr.byteLength);
  const pad = (4 - (buf.length % 4)) % 4;
  chunks.push(buf, Buffer.alloc(pad));
  views.push({ buffer: 0, byteOffset: offset, byteLength: buf.length, target });
  offset += buf.length + pad;
  return views.length - 1;
};
const meshPrims = [];
for (const p of prims) {
  const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < p.pos.length; i += 3) for (let k = 0; k < 3; k++) { min[k] = Math.min(min[k], p.pos[i + k]); max[k] = Math.max(max[k], p.pos[i + k]); }
  const pv = addView(p.pos, 34962), nv = addView(p.nrm, 34962), iv = addView(p.idx, 34963);
  accessors.push({ bufferView: pv, componentType: 5126, count: p.pos.length / 3, type: 'VEC3', min, max });
  accessors.push({ bufferView: nv, componentType: 5126, count: p.nrm.length / 3, type: 'VEC3' });
  accessors.push({ bufferView: iv, componentType: 5123, count: p.idx.length, type: 'SCALAR' });
  const a = accessors.length;
  meshPrims.push({ attributes: { POSITION: a - 3, NORMAL: a - 2 }, indices: a - 1, material: p.mat });
}
const gltf = {
  asset: { version: '2.0', generator: 'cartwheel vis_city gen.mjs (CC0)' },
  scene: 0, scenes: [{ nodes: [0] }], nodes: [{ mesh: 0, name: 'Tree' }],
  meshes: [{ name: 'Tree', primitives: meshPrims }],
  materials: [
    { name: 'Bark', pbrMetallicRoughness: { baseColorFactor: [0.32, 0.2, 0.11, 1], metallicFactor: 0, roughnessFactor: 0.9 } },
    { name: 'Leaves', pbrMetallicRoughness: { baseColorFactor: [0.16, 0.4, 0.13, 1], metallicFactor: 0, roughnessFactor: 0.85 } },
  ],
  buffers: [{ byteLength: offset }], bufferViews: views, accessors,
};
let json = Buffer.from(JSON.stringify(gltf));
json = Buffer.concat([json, Buffer.alloc((4 - (json.length % 4)) % 4, 0x20)]);
const bin = Buffer.concat(chunks);
const header = Buffer.alloc(12), jh = Buffer.alloc(8), bh = Buffer.alloc(8);
header.write('glTF', 0); header.writeUInt32LE(2, 4); header.writeUInt32LE(12 + 8 + json.length + 8 + bin.length, 8);
jh.writeUInt32LE(json.length, 0); jh.write('JSON', 4);
bh.writeUInt32LE(bin.length, 0); bh.write('BIN\0', 4);
mkdirSync(join(D, 'models'), { recursive: true });
writeFileSync(join(D, 'models', 'tree.glb'), Buffer.concat([header, jh, json, bh, bin]));
console.log('wrote models/tree.glb:', (trunkIdx.length + crownIdx.length) / 3, 'triangles');
