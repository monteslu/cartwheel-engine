// Writes samples/character_skin/assets/character.glb: a small procedural character for the skin
// shading, glTF material and morph target sample. Everything here is generated (no source art).
//
//   node samples/character_skin/make-character.mjs [out.glb]
//
// One skeleton (Root, Spine, Chest, Neck, Head, ShoulderL/R, ArmL/R, HandL/R), skinned meshes:
//   Face   the head, neck and ears (thin, for light through them), material "Skin"
//          (extras skin_shading), morph targets smile, jawOpen, browUp (extras.targetNames)
//   Mouth  a dark inner mouth that opens with jawOpen and widens with smile, material "MouthInside"
//   Eyes   two eyes, material "Eye" (KHR_materials_clearcoat, KHR_materials_specular), morph target blink
//   Body   torso and arms, material "Knit" (KHR_materials_sheen); hands, material "Skin" again
//   Belt   a waist band, material "Leather" (KHR_materials_clearcoat, KHR_materials_ior)
//   Hair   a cap of hair, material "Hair" (KHR_materials_sheen, KHR_materials_specular)
// Animations: "talk" (head nods, jawOpen keyed through a glTF weights channel) and "idle".
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = process.argv[2] || join(dirname(fileURLToPath(import.meta.url)), 'assets', 'character.glb');

// ── skeleton ─────────────────────────────────────────────────────────────
const BONES = [
  ['Root', -1, [0, 0, 0]], ['Spine', 0, [0, 0.95, 0]], ['Chest', 1, [0, 0.3, 0]], ['Neck', 2, [0, 0.22, 0]],
  ['Head', 3, [0, 0.12, 0]], ['ShoulderL', 2, [0.14, 0.16, 0]], ['ArmL', 5, [0.075, -0.02, 0]], ['HandL', 6, [0, -0.5, 0]],
  ['ShoulderR', 2, [-0.14, 0.16, 0]], ['ArmR', 8, [-0.075, -0.02, 0]], ['HandR', 9, [0, -0.5, 0]],
];
const B = Object.fromEntries(BONES.map((b, i) => [b[0], i]));
const world = BONES.map(() => [0, 0, 0]);
BONES.forEach(([, p, t], i) => { world[i] = p < 0 ? t : world[p].map((v, k) => v + t[k]); });

// ── mesh building ────────────────────────────────────────────────────────
class Mesh {
  constructor() { this.p = []; this.n = []; this.j = []; this.w = []; this.i = []; this.targets = {}; }
  vert(p, n, joints, weights) {
    this.p.push(...p); const l = Math.hypot(...n) || 1; this.n.push(n[0] / l, n[1] / l, n[2] / l);
    const j = [0, 0, 0, 0], w = [0, 0, 0, 0];
    joints.forEach((x, k) => { j[k] = x; w[k] = weights[k]; });
    const s = w.reduce((a, b) => a + b, 0); this.j.push(...j); this.w.push(...w.map((x) => x / s));
    return this.p.length / 3 - 1;
  }
  tri(a, b, c) { this.i.push(a, b, c); }
  count() { return this.p.length / 3; }
}
// a deformed sphere: f(theta, phi) -> {p, n, joints, weights}; rows x cols quads
function sphere(m, rows, cols, f) {
  const base = m.count();
  for (let r = 0; r <= rows; r++) for (let c = 0; c <= cols; c++) {
    const v = f(r / rows, c / cols); m.vert(v.p, v.n, v.j, v.w);
  }
  for (let r = 0; r < rows; r++) for (let c = 0; c < cols; c++) {
    const a = base + r * (cols + 1) + c, b = a + cols + 1;
    m.tri(a, b, a + 1); m.tri(a + 1, b, b + 1);
  }
  return base;
}
const lerp = (a, b, t) => a + (b - a) * t;
const smooth = (a, b, x) => { const t = Math.min(1, Math.max(0, (x - a) / (b - a))); return t * t * (3 - 2 * t); };

// head: an ellipsoid at the Head bone, the neck below, ears on the sides
const HEAD_C = [0, world[B.Head][1] + 0.1, 0.01], HEAD_R = [0.095, 0.125, 0.11];
const face = new Mesh(), faceSmile = [], faceJaw = [], faceBrow = [];
const frontZ = (sy, sx = 0) => { const sz = Math.sqrt(Math.max(0, 1 - sy * sy - sx * sx)); const low = smooth(0.0, -0.8, sy); return HEAD_C[2] + sz * HEAD_R[2] * (1 + 0.08 * low * sz); };
sphere(face, 40, 48, (v, u) => {
  const th = v * Math.PI, ph = u * Math.PI * 2 + Math.PI; // the seam at the back
  const sx = Math.sin(th) * Math.sin(ph), sy = Math.cos(th), sz = Math.sin(th) * Math.cos(ph);
  // jaw narrower, chin forward
  const low = smooth(0.0, -0.8, sy);
  const k = [1 - 0.18 * low, 1, 1 + 0.08 * low * Math.max(0, sz)];
  const p = [HEAD_C[0] + sx * HEAD_R[0] * k[0], HEAD_C[1] + sy * HEAD_R[1], HEAD_C[2] + sz * HEAD_R[2] * k[2]];
  // a nose: a bump on the front middle
  const nose = Math.exp(-((sx / 0.18) ** 2 + ((sy + 0.05) / 0.25) ** 2)) * Math.max(0, sz) ** 4;
  p[2] += 0.035 * nose;
  const n = [sx / HEAD_R[0], sy / HEAD_R[1], sz / HEAD_R[2]];
  const jw = smooth(0.1, -0.5, sy);
  return { p, n, j: [B.Head, B.Neck], w: [1 - 0.15 * smooth(-0.6, -1, sy), 0.15 * smooth(-0.6, -1, sy)], jw, sx, sy, sz };
});
// morph targets of the head: deltas per vertex
for (let i = 0; i < face.count(); i++) {
  const x = face.p[3 * i] - HEAD_C[0], y = face.p[3 * i + 1] - HEAD_C[1], z = face.p[3 * i + 2] - HEAD_C[2];
  const sx = x / HEAD_R[0], sy = y / HEAD_R[1], front = Math.max(0, z / HEAD_R[2]);
  // smile: the mouth corners and cheeks lift and widen
  const corner = Math.exp(-(((Math.abs(sx) - 0.35) / 0.25) ** 2 + ((sy + 0.42) / 0.22) ** 2)) * front;
  faceSmile.push(Math.sign(sx) * 0.012 * corner, 0.014 * corner, 0.004 * corner);
  // jawOpen: the face below the mouth line drops and tucks back
  const below = smooth(-0.38, -0.6, sy) * smooth(-0.2, 0.4, z / HEAD_R[2]);
  faceJaw.push(0, -0.03 * below, -0.008 * below);
  // browUp: the brow band lifts
  const brow = Math.exp(-(((sy - 0.25) / 0.14) ** 2)) * front;
  faceBrow.push(0, 0.008 * brow, 0);
}
// neck: a tube from the chest to the head (no targets: zero deltas)
function tube(m, y0, y1, r0, r1, z0, rings, segs, jw, cx = 0) {
  const base = m.count();
  for (let r = 0; r <= rings; r++) for (let s = 0; s <= segs; s++) {
    const t = r / rings, a = s / segs * Math.PI * 2, rad = lerp(r0, r1, t);
    const y = lerp(y0, y1, t);
    const { j, w } = jw(t);
    m.vert([cx + Math.sin(a) * rad, y, z0 + Math.cos(a) * rad * 0.9], [Math.sin(a), 0, Math.cos(a)], j, w);
  }
  for (let r = 0; r < rings; r++) for (let s = 0; s < segs; s++) {
    const a = base + r * (segs + 1) + s, b = a + segs + 1;
    m.tri(a, a + 1, b); m.tri(a + 1, b + 1, b);
  }
  return base;
}
const pad = (arr, m, from) => { while (arr.length < m.count() * 3) arr.push(0); };
tube(face, world[B.Chest][1] + 0.12, HEAD_C[1] - 0.06, 0.055, 0.05, 0, 6, 24,
  (t) => ({ j: [B.Chest, B.Neck, B.Head], w: [1 - smooth(0, 0.4, t), smooth(0, 0.4, t) * (1 - smooth(0.6, 1, t)), smooth(0.6, 1, t)] }));
// ears: thin flattened discs on the sides (light glows through them)
for (const side of [-1, 1]) {
  sphere(face, 10, 16, (v, u) => {
    const th = v * Math.PI, ph = u * Math.PI * 2;
    const sx = Math.sin(th) * Math.sin(ph), sy = Math.cos(th), sz = Math.sin(th) * Math.cos(ph);
    const p = [side * (HEAD_R[0] + 0.004) + sx * 0.008, HEAD_C[1] + 0.0 + sy * 0.032, HEAD_C[2] - 0.005 + sz * 0.02];
    return { p, n: [sx / 0.008, sy / 0.032, sz / 0.02], j: [B.Head], w: [1] };
  });
}
pad(faceSmile, face); pad(faceJaw, face); pad(faceBrow, face);

// mouth: a dark ellipse just inside the lips, opening with the jaw
const mouth = new Mesh(), mouthSmile = [], mouthJaw = [], mouthBrow = [];
{
  const cy = HEAD_C[1] - 0.42 * HEAD_R[1], cz = frontZ(-0.42) + 0.0015;
  const c0 = mouth.vert([0, cy, cz], [0, 0, 1], [B.Head], [1]);
  mouthSmile.push(0, 0.002, 0); mouthJaw.push(0, -0.012, -0.004); mouthBrow.push(0, 0, 0);
  const N = 24;
  for (let s = 0; s < N; s++) {
    const a = s / N * Math.PI * 2, x = Math.cos(a) * 0.028, y = Math.sin(a) * 0.004;
    mouth.vert([x, cy + y, cz - 0.004 * (x / 0.028) ** 2], [0, 0, 1], [B.Head], [1]);
    const lower = Math.sin(a) < 0 ? 1 : 0;
    mouthSmile.push(Math.sign(x) * 0.008 * Math.abs(x / 0.028), 0.01 * (x / 0.028) ** 2, 0);
    mouthJaw.push(0, -0.026 * lower * Math.abs(Math.sin(a)), -0.006 * lower);
    mouthBrow.push(0, 0, 0);
  }
  for (let s = 0; s < N; s++) mouth.tri(c0, c0 + 1 + s, c0 + 1 + ((s + 1) % N));
}

// eyes: two spheres, morph target blink squashes them
const eyes = new Mesh(), eyeBlink = [];
for (const side of [-1, 1]) {
  const c = [side * 0.036, HEAD_C[1] + 0.012, frontZ(0.1, 0.38) - 0.008];
  sphere(eyes, 10, 14, (v, u) => {
    const th = v * Math.PI, ph = u * Math.PI * 2;
    const sx = Math.sin(th) * Math.sin(ph), sy = Math.cos(th), sz = Math.sin(th) * Math.cos(ph);
    return { p: [c[0] + sx * 0.016, c[1] + sy * 0.016, c[2] + sz * 0.016], n: [sx, sy, sz], j: [B.Head], w: [1] };
  });
}
for (let i = 0; i < eyes.count(); i++) {
  const y = eyes.p[3 * i + 1] - (HEAD_C[1] + 0.012);
  eyeBlink.push(0, -y * 0.9, 0);
}

// body: torso + arms (sheen knit), hands (skin), belt, hair
const body = new Mesh(), hands = new Mesh(), belt = new Mesh(), hair = new Mesh();
const yS = world[B.Spine][1], yC = world[B.Chest][1];
tube(body, yS - 0.1, yC + 0.16, 0.15, 0.17, 0, 10, 32,
  (t) => ({ j: [B.Spine, B.Chest], w: [1 - smooth(0.3, 0.7, t), smooth(0.3, 0.7, t)] }));
// shoulders cap
sphere(body, 8, 32, (v, u) => {
  if (v > 0.5) v = 0.5;
  const th = v * Math.PI, ph = u * Math.PI * 2;
  const sx = Math.sin(th) * Math.sin(ph), sy = Math.cos(th), sz = Math.sin(th) * Math.cos(ph);
  return { p: [sx * 0.17, yC + 0.16 + sy * 0.06, sz * 0.17 * 0.9], n: [sx, sy, sz], j: [B.Chest], w: [1] };
});
for (const [arm, sh, hand, side] of [['ArmL', 'ShoulderL', 'HandL', 1], ['ArmR', 'ShoulderR', 'HandR', -1]]) {
  const a0 = world[B[arm]], x = a0[0];
  tube(body, a0[1] + 0.02, a0[1] - 0.46, 0.05, 0.038, 0, 12, 16,
    (t) => ({ j: [B[sh], B[arm]], w: [1 - smooth(0, 0.2, t), smooth(0, 0.2, t)] }), x);
  const hc = world[B[hand]];
  sphere(hands, 10, 14, (v, u) => {
    const th = v * Math.PI, ph = u * Math.PI * 2;
    const sx = Math.sin(th) * Math.sin(ph), sy = Math.cos(th), sz = Math.sin(th) * Math.cos(ph);
    return { p: [hc[0] + sx * 0.035, hc[1] - 0.04 + sy * 0.06, sz * 0.025], n: [sx / 0.035, sy / 0.06, sz / 0.025], j: [B[hand], B[arm]], w: [0.9, 0.1] };
  });
}
tube(belt, yS - 0.06, yS + 0.0, 0.158, 0.16, 0, 1, 32, () => ({ j: [B.Spine], w: [1] }));
sphere(hair, 18, 40, (v, u) => {
  const ph = u * Math.PI * 2, front = Math.cos(ph);
  const cut = front > 0 ? lerp(0.05, 0.42, front) : lerp(0.05, -0.45, -front); // lowest sy the hair reaches
  const th = v * Math.acos(cut);
  const sx = Math.sin(th) * Math.sin(ph), sy = Math.cos(th), sz = Math.sin(th) * Math.cos(ph);
  const back = 1 + 0.06 * Math.max(0, -sz);
  return { p: [HEAD_C[0] + sx * (HEAD_R[0] + 0.012) * back, HEAD_C[1] + 0.012 + sy * (HEAD_R[1] + 0.01), HEAD_C[2] - 0.006 + sz * (HEAD_R[2] + 0.012) * back],
    n: [sx, sy, sz], j: [B.Head], w: [1] };
});

// ── glTF writer ──────────────────────────────────────────────────────────
const bin = [], views = [], accessors = [];
let binLen = 0;
function view(bytes, target) {
  const pad4 = (4 - (binLen % 4)) % 4;
  if (pad4) { bin.push(new Uint8Array(pad4)); binLen += pad4; }
  views.push({ buffer: 0, byteOffset: binLen, byteLength: bytes.byteLength, ...(target ? { target } : {}) });
  bin.push(new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength)); binLen += bytes.byteLength;
  return views.length - 1;
}
function acc(arr, type, ct, target, minmax) {
  const n = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT4: 16 }[type];
  const T = ct === 5126 ? Float32Array : ct === 5123 ? Uint16Array : ct === 5125 ? Uint32Array : Uint8Array;
  const data = new T(arr);
  const a = { bufferView: view(data, target), componentType: ct, count: arr.length / n, type };
  if (minmax) {
    a.min = []; a.max = [];
    for (let k = 0; k < n; k++) { let lo = Infinity, hi = -Infinity; for (let i = k; i < arr.length; i += n) { lo = Math.min(lo, arr[i]); hi = Math.max(hi, arr[i]); } a.min.push(lo); a.max.push(hi); }
  }
  accessors.push(a); return accessors.length - 1;
}
const meshes = [];
function addMesh(name, m, material, targets) {
  const prim = {
    attributes: { POSITION: acc(m.p, 'VEC3', 5126, 34962, true), NORMAL: acc(m.n, 'VEC3', 5126, 34962),
      JOINTS_0: acc(m.j, 'VEC4', 5121, 34962), WEIGHTS_0: acc(m.w, 'VEC4', 5126, 34962) },
    indices: acc(m.i, 'SCALAR', m.count() > 65535 ? 5125 : 5123, 34963), material,
  };
  const mesh = { name, primitives: [prim] };
  if (targets) {
    prim.targets = targets.map(([, d]) => ({ POSITION: acc(d, 'VEC3', 5126, undefined, true) }));
    mesh.weights = targets.map(() => 0);
    mesh.extras = { targetNames: targets.map(([n]) => n) };
  }
  meshes.push(mesh); return meshes.length - 1;
}
const materials = [
  { name: 'Skin', pbrMetallicRoughness: { baseColorFactor: [0.80, 0.53, 0.42, 1], metallicFactor: 0, roughnessFactor: 0.55 },
    extensions: { KHR_materials_specular: { specularFactor: 0.5 } },
    extras: { skin_shading: { scatter: [1.0, 0.35, 0.22], wrap: 0.55, translucency: 0.45, distortion: 0.25, power: 3 } } },
  { name: 'MouthInside', pbrMetallicRoughness: { baseColorFactor: [0.18, 0.04, 0.05, 1], metallicFactor: 0, roughnessFactor: 0.6 } },
  { name: 'Eye', pbrMetallicRoughness: { baseColorFactor: [0.06, 0.035, 0.02, 1], metallicFactor: 0, roughnessFactor: 0.3 },
    extensions: { KHR_materials_clearcoat: { clearcoatFactor: 1, clearcoatRoughnessFactor: 0.03 }, KHR_materials_specular: { specularFactor: 1, specularColorFactor: [1, 1, 1] } } },
  { name: 'Knit', pbrMetallicRoughness: { baseColorFactor: [0.10, 0.22, 0.42, 1], metallicFactor: 0, roughnessFactor: 0.95 },
    extensions: { KHR_materials_sheen: { sheenColorFactor: [0.45, 0.55, 0.8], sheenRoughnessFactor: 0.8 }, KHR_materials_specular: { specularFactor: 0.2 } } },
  { name: 'Leather', pbrMetallicRoughness: { baseColorFactor: [0.12, 0.06, 0.03, 1], metallicFactor: 0, roughnessFactor: 0.6 },
    extensions: { KHR_materials_clearcoat: { clearcoatFactor: 0.6, clearcoatRoughnessFactor: 0.2 }, KHR_materials_ior: { ior: 1.45 } } },
  { name: 'Hair', pbrMetallicRoughness: { baseColorFactor: [0.05, 0.03, 0.02, 1], metallicFactor: 0, roughnessFactor: 0.7 },
    extensions: { KHR_materials_sheen: { sheenColorFactor: [0.35, 0.25, 0.18], sheenRoughnessFactor: 0.4 }, KHR_materials_specular: { specularFactor: 0.6 } } },
];
const MI = Object.fromEntries(materials.map((m, i) => [m.name, i]));
const mFace = addMesh('Face', face, MI.Skin, [['smile', faceSmile], ['jawOpen', faceJaw], ['browUp', faceBrow]]);
const mMouth = addMesh('Mouth', mouth, MI.MouthInside, [['smile', mouthSmile], ['jawOpen', mouthJaw], ['browUp', mouthBrow]]);
const mEyes = addMesh('Eyes', eyes, MI.Eye, [['blink', eyeBlink]]);
const mBody = addMesh('Body', body, MI.Knit);
const mHands = addMesh('Hands', hands, MI.Skin);
const mBelt = addMesh('Belt', belt, MI.Leather);
const mHair = addMesh('Hair', hair, MI.Hair);

// nodes: bones, then one node per skinned mesh at the root
const nodes = BONES.map(([name, , t]) => ({ name, translation: t, children: [] }));
BONES.forEach(([, p], i) => { if (p >= 0) nodes[p].children.push(i); });
nodes.forEach((n) => { if (!n.children.length) delete n.children; });
// inverse bind matrices: translations only (the rest pose has no rotation)
const ibm = [];
for (const w of world) ibm.push(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -w[0], -w[1], -w[2], 1);
const skins = [{ name: 'Rig', joints: BONES.map((_, i) => i), skeleton: 0, inverseBindMatrices: acc(ibm, 'MAT4', 5126) }];
const meshNodes = [[mFace, 'Face'], [mMouth, 'Mouth'], [mEyes, 'Eyes'], [mBody, 'Body'], [mHands, 'Hands'], [mBelt, 'Belt'], [mHair, 'Hair']]
  .map(([mesh, name]) => { nodes.push({ name, mesh, skin: 0 }); return nodes.length - 1; });

// animations
const quat = (ax, ang) => { const s = Math.sin(ang / 2), l = Math.hypot(...ax); return [ax[0] / l * s, ax[1] / l * s, ax[2] / l * s, Math.cos(ang / 2)]; };
function clip(name, len, n, tracks) {
  const times = Array.from({ length: n + 1 }, (_, i) => (len * i) / n);
  const input = acc(times, 'SCALAR', 5126, undefined, true);
  const samplers = [], channels = [];
  for (const { node, path, f, size } of tracks) {
    const out = times.flatMap((t) => f(t));
    const type = size === 4 ? 'VEC4' : size === 3 ? 'VEC3' : 'SCALAR';
    samplers.push({ input, output: acc(out, type, 5126), interpolation: 'LINEAR' });
    channels.push({ sampler: samplers.length - 1, target: { node, path } });
  }
  return { name, samplers, channels };
}
const T = Math.PI * 2;
const animations = [
  clip('talk', 2, 48, [
    { node: B.Head, path: 'rotation', f: (t) => quat([1, 0.2 * Math.sin(T * t / 2), 0], 0.08 * Math.sin(T * t * 1.5)) },
    { node: B.Neck, path: 'rotation', f: (t) => quat([0, 1, 0], 0.12 * Math.sin(T * t / 2)) },
    // jawOpen keyed per syllable; smile and browUp left to code (weights: one value per target)
    { node: meshNodes[0], path: 'weights', f: (t) => [0, Math.max(0, Math.sin(T * t * 2)) ** 0.7 * 0.9, 0] },
    { node: meshNodes[1], path: 'weights', f: (t) => [0, Math.max(0, Math.sin(T * t * 2)) ** 0.7 * 0.9, 0] },
  ]),
  clip('idle', 3, 36, [
    { node: B.Chest, path: 'rotation', f: (t) => quat([1, 0, 0], 0.03 * Math.sin(T * t / 3)) },
    { node: B.ArmL, path: 'rotation', f: (t) => quat([0, 0, 1], 0.12 + 0.05 * Math.sin(T * t / 3)) },
    { node: B.ArmR, path: 'rotation', f: (t) => quat([0, 0, 1], -0.12 - 0.05 * Math.sin(T * t / 3)) },
  ]),
];

const gltf = {
  asset: { version: '2.0', generator: 'cartwheel samples/character_skin/make-character.mjs' },
  extensionsUsed: ['KHR_materials_sheen', 'KHR_materials_clearcoat', 'KHR_materials_specular', 'KHR_materials_ior'],
  scene: 0, scenes: [{ name: 'Character', nodes: [0, ...meshNodes] }],
  nodes, meshes, materials, skins, animations,
  buffers: [{ byteLength: 0 }], bufferViews: views, accessors,
};
let binBuf = Buffer.concat(bin.map((b) => Buffer.from(b)));
const binPad = (4 - (binBuf.length % 4)) % 4;
binBuf = Buffer.concat([binBuf, Buffer.alloc(binPad)]);
gltf.buffers[0].byteLength = binBuf.length;
let json = Buffer.from(JSON.stringify(gltf));
json = Buffer.concat([json, Buffer.alloc((4 - (json.length % 4)) % 4, 0x20)]);
const header = Buffer.alloc(12), jh = Buffer.alloc(8), bh = Buffer.alloc(8);
header.writeUInt32LE(0x46546c67, 0); header.writeUInt32LE(2, 4); header.writeUInt32LE(12 + 8 + json.length + 8 + binBuf.length, 8);
jh.writeUInt32LE(json.length, 0); jh.writeUInt32LE(0x4e4f534a, 4);
bh.writeUInt32LE(binBuf.length, 0); bh.writeUInt32LE(0x004e4942, 4);
mkdirSync(dirname(OUT), { recursive: true });
writeFileSync(OUT, Buffer.concat([header, jh, json, bh, binBuf]));
const verts = [face, mouth, eyes, body, hands, belt, hair].reduce((s, m) => s + m.count(), 0);
console.log(`${OUT}: ${verts} vertices, ${meshes.length} meshes, ${materials.length} materials, ${animations.length} clips`);
