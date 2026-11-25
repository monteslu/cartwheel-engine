// Regenerates texture_color's images (all made here): node samples/texture_color/project/gen.mjs
// Each is a flat mid grey (128): sampled as colour (sRGB) it draws as 128 on screen, as data as about 188.
import { writeFileSync, mkdirSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const D = dirname(fileURLToPath(import.meta.url));
const put = (p, data) => { mkdirSync(dirname(join(D, p)), { recursive: true }); writeFileSync(join(D, p), data); };
const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc = (b) => { let c = 0xffffffff; for (const x of b) c = crcTable[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
function png(w, h, v) {
  const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  const raw = Buffer.alloc(h * (w * 4 + 1));
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const o = y * (w * 4 + 1) + 1 + x * 4; raw[o] = raw[o + 1] = raw[o + 2] = v; raw[o + 3] = 255; }
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}
// a PNG kept as it is: data unless the game says colour (cw_res_texture_set_srgb)
put('textures/grey.png', png(16, 16, 128));
put('textures/grey_claimed.png', png(16, 16, 128));
// KTX2 from cwpack: sRGB unless the sidecar says texture.color = linear
put('textures/grey_k.png', png(16, 16, 128));
put('textures/grey_k.png.cwimport', 'texture.format = ktx2\ntexture.mode = uastc\ntexture.color = srgb\n');
put('textures/grey_kl.png', png(16, 16, 128));
put('textures/grey_kl.png.cwimport', 'texture.format = ktx2\ntexture.mode = uastc\ntexture.color = linear\n');
// KTX2 with no texture.color: cwpack's default, sRGB
put('textures/grey_kd.png', png(16, 16, 128));
put('textures/grey_kd.png.cwimport', 'texture.format = ktx2\n');
