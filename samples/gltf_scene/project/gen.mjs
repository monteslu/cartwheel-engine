// Regenerates this sample's generated texture (no third-party content):
// node samples/gltf_scene/project/gen.mjs
import { writeFileSync, mkdirSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const D = dirname(fileURLToPath(import.meta.url));
const crcT = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc = (b) => { let c = 0xffffffff; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
function png(w, h, px) {
  const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const ih = Buffer.alloc(13); ih.writeUInt32BE(w, 0); ih.writeUInt32BE(h, 4); ih[8] = 8; ih[9] = 6;
  const raw = Buffer.alloc(h * (w * 4 + 1));
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const o = y * (w * 4 + 1) + 1 + x * 4; const c = px(x, y); raw.set([c[0], c[1], c[2], 255], o); }
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}
// stone tiles: a grid with per-tile tint and soft mortar lines
const tile = (x, y) => {
  const tx = x >> 6, ty = y >> 6, u = x & 63, v = y & 63;
  const edge = Math.min(u, v, 63 - u, 63 - v);
  const t = ((tx * 73856093) ^ (ty * 19349663)) & 255;
  const base = [120 + (t & 31), 112 + ((t >> 3) & 31), 100 + ((t >> 5) & 15)];
  const k = edge < 3 ? 0.45 + edge * 0.15 : 1 - ((x * 7 + y * 13) % 17) / 170;
  return base.map((c) => Math.round(c * k));
};
mkdirSync(join(D, 'textures'), { recursive: true });
writeFileSync(join(D, 'textures/ground.png'), png(512, 512, tile));
