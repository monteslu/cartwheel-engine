// Regenerates the resource suite's generated assets (all made here, no
// third-party content): node tests/unit/res_project/gen.mjs
import { writeFileSync, mkdirSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const D = dirname(fileURLToPath(import.meta.url));
const put = (p, data) => { mkdirSync(dirname(join(D, p)), { recursive: true }); writeFileSync(join(D, p), data); };

const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc = (b) => { let c = 0xffffffff; for (const x of b) c = crcTable[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
function png(w, h, px) {
  const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  const raw = Buffer.alloc(h * (w * 4 + 1));
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const o = y * (w * 4 + 1) + 1 + x * 4; const c = px(x, y); raw[o] = c[0]; raw[o + 1] = c[1]; raw[o + 2] = c[2]; raw[o + 3] = 255; }
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}
const checker = (n, a, b) => (x, y) => (((x >> n) ^ (y >> n)) & 1 ? a : b);
put('textures/checker.png', png(64, 64, checker(3, [230, 80, 40], [250, 230, 200])));
put('textures/checker_k.png', png(64, 64, checker(2, [40, 120, 230], [220, 240, 250])));
put('textures/small.png', png(16, 16, checker(1, [20, 200, 90], [10, 30, 20])));

// a 32x32-cell wavy grid (LODs have something to remove), external .bin + image
const N = 32, pos = [], uv = [], nrm = [], idx = [];
for (let j = 0; j <= N; j++) for (let i = 0; i <= N; i++) {
  const x = i / N - 0.5, z = j / N - 0.5;
  pos.push(x, 0.05 * Math.sin(x * 6) * Math.cos(z * 6), z); nrm.push(0, 1, 0); uv.push(i / N, j / N);
}
for (let j = 0; j < N; j++) for (let i = 0; i < N; i++) {
  const a = j * (N + 1) + i, b = a + 1, c = a + N + 1, d = c + 1;
  idx.push(a, c, b, b, c, d);
}
const f32 = (a) => Buffer.from(new Float32Array(a).buffer), u16 = (a) => Buffer.from(new Uint16Array(a).buffer);
const P = f32(pos), Nn = f32(nrm), U = f32(uv), I = u16(idx);
const bin = Buffer.concat([P, Nn, U, I]);
let mn = [1e9, 1e9, 1e9], mx = [-1e9, -1e9, -1e9];
for (let k = 0; k < pos.length; k += 3) for (let c = 0; c < 3; c++) { const v = Math.fround(pos[k + c]); mn[c] = Math.min(mn[c], v); mx[c] = Math.max(mx[c], v); }
const gltf = {
  asset: { version: '2.0', generator: 'cartwheel res suite gen.mjs' },
  scene: 0, scenes: [{ nodes: [0] }], nodes: [{ name: 'Quad', mesh: 0 }],
  meshes: [{ name: 'quad', primitives: [{ attributes: { POSITION: 0, NORMAL: 1, TEXCOORD_0: 2 }, indices: 3, material: 0 }] }],
  materials: [{ name: 'checker', pbrMetallicRoughness: { baseColorTexture: { index: 0 }, metallicFactor: 0, roughnessFactor: 0.8 } }],
  textures: [{ source: 0 }], images: [{ uri: '../textures/checker.png' }],
  buffers: [{ uri: 'quad.bin', byteLength: bin.length }],
  bufferViews: [
    { buffer: 0, byteOffset: 0, byteLength: P.length, target: 34962 },
    { buffer: 0, byteOffset: P.length, byteLength: Nn.length, target: 34962 },
    { buffer: 0, byteOffset: P.length + Nn.length, byteLength: U.length, target: 34962 },
    { buffer: 0, byteOffset: P.length + Nn.length + U.length, byteLength: I.length, target: 34963 },
  ],
  accessors: [
    { bufferView: 0, componentType: 5126, count: pos.length / 3, type: 'VEC3', min: mn, max: mx },
    { bufferView: 1, componentType: 5126, count: pos.length / 3, type: 'VEC3' },
    { bufferView: 2, componentType: 5126, count: pos.length / 3, type: 'VEC2' },
    { bufferView: 3, componentType: 5123, count: idx.length, type: 'SCALAR' },
  ],
};
put('models/quad.gltf', JSON.stringify(gltf, null, 1) + '\n');
put('models/quad.bin', bin);
// the same quad and image, sampled clamped and nearest: a texture of its own
put('models/quad_clamp.gltf', JSON.stringify({ ...gltf, textures: [{ source: 0, sampler: 0 }], samplers: [{ magFilter: 9728, minFilter: 9728, wrapS: 33071, wrapT: 33071 }] }, null, 1) + '\n');
// the same image as data (an occlusion map) where quad.gltf shows it as colour
put('models/quad_data.gltf', JSON.stringify({ ...gltf, materials: [{ name: 'checker_ao', pbrMetallicRoughness: { metallicFactor: 0, roughnessFactor: 0.8 }, occlusionTexture: { index: 0 } }] }, null, 1) + '\n');

// 0.25 s of 440 Hz, 22050 Hz mono 16-bit
const rate = 22050, frames = Math.floor(rate / 4), data = Buffer.alloc(frames * 2);
for (let i = 0; i < frames; i++) data.writeInt16LE(Math.round(8000 * Math.sin((2 * Math.PI * 440 * i) / rate)), i * 2);
const hdr = Buffer.alloc(44);
hdr.write('RIFF', 0); hdr.writeUInt32LE(36 + data.length, 4); hdr.write('WAVE', 8); hdr.write('fmt ', 12);
hdr.writeUInt32LE(16, 16); hdr.writeUInt16LE(1, 20); hdr.writeUInt16LE(1, 22); hdr.writeUInt32LE(rate, 24);
hdr.writeUInt32LE(rate * 2, 28); hdr.writeUInt16LE(2, 32); hdr.writeUInt16LE(16, 34); hdr.write('data', 36); hdr.writeUInt32LE(data.length, 40);
put('audio/beep.wav', Buffer.concat([hdr, data]));

put('text/hello.txt', 'hello cartwheel\n');
put('text/hello.txt.cwimport', '# a sidecar: this file keeps its UID if it moves\nuid = 0x1234abcd5678ef00\n');
put('data/config.json', '{"speed": 4.5, "name": "cartwheel", "list": [1, 2, 3]}\n');
put('data/bad.json', '{"speed": 4.5, "name": }\n');
put('textures/checker_k.png.cwimport', 'texture.format = ktx2\ntexture.mode = uastc\n');
// async-order files: a later request decodes faster than an earlier one
for (let i = 0; i < 8; i++) put(`order/${'abcdefgh'[i]}.ord`, String((8 - i) * 200000) + '\n');

// 1 s, 44.1 kHz stereo: 440 Hz left, 660 Hz right (imported to Ogg Opus by its sidecar)
{
  const rate = 44100, n = rate, data = Buffer.alloc(n * 4);
  for (let i = 0; i < n; i++) {
    data.writeInt16LE(Math.round(9830 * Math.sin((2 * Math.PI * 440 * i) / rate)), i * 4);
    data.writeInt16LE(Math.round(9830 * Math.sin((2 * Math.PI * 660 * i) / rate)), i * 4 + 2);
  }
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8); h.write('fmt ', 12);
  h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(2, 22); h.writeUInt32LE(rate, 24);
  h.writeUInt32LE(rate * 4, 28); h.writeUInt16LE(4, 32); h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(data.length, 40);
  put('audio/tune.wav', Buffer.concat([h, data]));
  put('audio/tune.wav.cwimport', 'audio.format = opus\naudio.bitrate = 96000\n');
}

// scenes for the preload test: UIDs are FNV-1a 64 of resource paths
{
  const fnv = (str) => { let h = 0xcbf29ce484222325n; for (const c of Buffer.from(str)) { h ^= BigInt(c); h = (h * 0x100000001b3n) & 0xffffffffffffffffn; } return h.toString(16).padStart(16, '0'); };
  put('levels/test.cwscene', `[scene format=1]\n[ext_scene id=1 path="levels/sub.cwscene"]\n\n[node name="Level" type="Node3D"]\n\n[node name="Quad" parent="." type="MeshInstance3D"]\nmesh = res(0x${fnv('models/quad.gltf#mesh/0/0')})\n\n[node name="Sub" parent="." instance=1]\n`);
  put('levels/sub.cwscene', `[scene format=1]\n\n[node name="Crate" type="MeshInstance3D"]\nmesh = res(0x${fnv('models/box.glb')})\n`);
}
