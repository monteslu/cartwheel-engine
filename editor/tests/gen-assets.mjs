// Generates the editor's test assets from code (CC0: made here, no outside art).
//   node editor/tests/gen-assets.mjs
// Writes:
//   editor/demo_project/textures/grid.png   a 64x64 grid texture (the demo
//                                           project's importable texture)
//   editor/tests/import/lantern.gltf        a small lantern: base, glass body,
//   editor/tests/import/lantern.bin         roof and ring; one textured material
//   editor/tests/import/lantern_paint.png   (external buffer and image, so the
//                                           FileSystem's dependency view has
//                                           something to show)
//   editor/tests/import/broken.gltf         a glTF whose accessor points past its
//                                           buffer: must fail to import, clearly
// The import test (editor_import) copies these into the project at run time.
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writePng } from '../../tools/lib/png.mjs';

const D = dirname(fileURLToPath(import.meta.url));
const DEMO = join(D, '..', 'demo_project');
const IMP = join(D, 'import');
mkdirSync(join(DEMO, 'textures'), { recursive: true });
mkdirSync(IMP, { recursive: true });

// ── grid.png ──────────────────────────────────────────────────────────────
{
  const w = 64, h = 64, px = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const i = (y * w + x) * 4, line = x % 16 === 0 || y % 16 === 0, check = ((x >> 3) + (y >> 3)) & 1;
    const v = line ? [235, 180, 60] : check ? [70, 76, 90] : [96, 104, 120];
    px.set([...v, 255], i);
  }
  writePng(join(DEMO, 'textures', 'grid.png'), w, h, px);
}

// ── lantern_paint.png: bands the UVs map onto the parts ──────────────────
{
  const w = 64, h = 64, px = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const i = (y * w + x) * 4, band = y >> 4; // 4 bands: base, glass, roof, ring
    let c = [[52, 60, 72], [255, 196, 64], [176, 48, 40], [210, 210, 200]][band];
    if (band === 1 && (x % 16 < 2)) c = [60, 50, 40]; // window frames
    if (band === 0 && ((x + y) & 7) === 0) c = [80, 90, 104];
    px.set([...c, 255], i);
  }
  writePng(join(IMP, 'lantern_paint.png'), w, h, px);
}

// ── lantern.gltf ─────────────────────────────────────────────────────────
const pos = [], nrm = [], uv = [], idx = [];
// a ring of n quads from (r0, y0) to (r1, y1) around Y, textured in band v0..v1
function lathe(r0, y0, r1, y1, n, v0, v1, cap) {
  const base = pos.length / 3;
  const ny = (r0 - r1) / Math.hypot(r1 - r0, y1 - y0), nr = (y1 - y0) / Math.hypot(r1 - r0, y1 - y0);
  for (let i = 0; i <= n; i++) {
    const a = (i / n) * Math.PI * 2, c = Math.cos(a), s = Math.sin(a);
    pos.push(r0 * c, y0, r0 * s, r1 * c, y1, r1 * s);
    nrm.push(nr * c, ny, nr * s, nr * c, ny, nr * s);
    uv.push(i / n, v0, i / n, v1);
  }
  for (let i = 0; i < n; i++) {
    const a = base + i * 2;
    idx.push(a, a + 1, a + 2, a + 1, a + 3, a + 2);
  }
  if (cap !== undefined) { // a flat disc at y = cap facing down (-1) or up (+1)
    const y = cap > 0 ? y1 : y0, r = cap > 0 ? r1 : r0, centre = pos.length / 3;
    pos.push(0, y, 0); nrm.push(0, cap, 0); uv.push(0.5, (v0 + v1) / 2);
    for (let i = 0; i <= n; i++) {
      const a = (i / n) * Math.PI * 2;
      pos.push(r * Math.cos(a), y, r * Math.sin(a)); nrm.push(0, cap, 0); uv.push(i / n, (v0 + v1) / 2);
    }
    for (let i = 0; i < n; i++) cap > 0 ? idx.push(centre, centre + 2 + i, centre + 1 + i) : idx.push(centre, centre + 1 + i, centre + 2 + i);
  }
}
const N = 12, band = (k) => [k / 4 + 0.02, (k + 1) / 4 - 0.02];
lathe(0.42, 0.0, 0.42, 0.18, N, ...band(0), -1); // base
lathe(0.42, 0.18, 0.30, 0.26, N, ...band(0));
lathe(0.30, 0.26, 0.30, 0.95, N, ...band(1)); // glass body
lathe(0.36, 0.95, 0.40, 1.02, N, ...band(3)); // ring
lathe(0.40, 1.02, 0.0, 1.38, N, ...band(2)); // roof
lathe(0.05, 1.38, 0.05, 1.5, N, ...band(3), 1); // finial
const f32 = (a) => Buffer.from(new Float32Array(a).buffer);
const u16 = (a) => Buffer.from(new Uint16Array(a).buffer);
const parts = [f32(pos), f32(nrm), f32(uv), u16(idx)];
const pad = (b) => Buffer.concat([b, Buffer.alloc((4 - (b.length % 4)) % 4)]);
const views = [];
let off = 0;
for (const p of parts) { views.push({ buffer: 0, byteOffset: off, byteLength: p.length }); off += pad(p).length; }
views[0].target = views[1].target = views[2].target = 34962;
views[3].target = 34963;
const bin = Buffer.concat(parts.map(pad));
const mn = [0, 1, 2].map((k) => Math.min(...pos.filter((_, i) => i % 3 === k)));
const mx = [0, 1, 2].map((k) => Math.max(...pos.filter((_, i) => i % 3 === k)));
const nv = pos.length / 3;
const gltf = {
  asset: { version: '2.0', generator: 'cartwheel editor/tests/gen-assets.mjs (CC0)' },
  scene: 0,
  scenes: [{ nodes: [0] }],
  nodes: [{ name: 'Lantern', mesh: 0 }],
  meshes: [{ name: 'Lantern', primitives: [{ attributes: { POSITION: 0, NORMAL: 1, TEXCOORD_0: 2 }, indices: 3, material: 0 }] }],
  materials: [{ name: 'Painted', pbrMetallicRoughness: { baseColorTexture: { index: 0 }, metallicFactor: 0.1, roughnessFactor: 0.6 },
    emissiveFactor: [0.35, 0.25, 0.05], emissiveTexture: { index: 0 } }],
  textures: [{ source: 0 }],
  images: [{ uri: 'lantern_paint.png' }],
  buffers: [{ uri: 'lantern.bin', byteLength: bin.length }],
  bufferViews: views,
  accessors: [
    { bufferView: 0, componentType: 5126, count: nv, type: 'VEC3', min: mn, max: mx },
    { bufferView: 1, componentType: 5126, count: nv, type: 'VEC3' },
    { bufferView: 2, componentType: 5126, count: nv, type: 'VEC2' },
    { bufferView: 3, componentType: 5123, count: idx.length, type: 'SCALAR' },
  ],
};
writeFileSync(join(IMP, 'lantern.bin'), bin);
writeFileSync(join(IMP, 'lantern.gltf'), JSON.stringify(gltf, null, 1) + '\n');

// ── broken.gltf: the POSITION accessor reads past the end of its buffer ──
const broken = JSON.parse(JSON.stringify(gltf));
broken.accessors[0].count = nv * 50;
broken.buffers[0].uri = 'lantern.bin';
broken.images = [];
broken.textures = [];
delete broken.materials[0].pbrMetallicRoughness.baseColorTexture;
delete broken.materials[0].emissiveTexture;
writeFileSync(join(IMP, 'broken.gltf'), JSON.stringify(broken, null, 1) + '\n');
console.log(`lantern: ${nv} vertices, ${idx.length / 3} triangles, bin ${bin.length} bytes`);
