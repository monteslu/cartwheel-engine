// Minimal PNG read/write (8-bit RGB/RGBA in, RGBA out) and PPM read, enough
// for golden frames. No dependencies.
import { readFileSync, writeFileSync } from 'node:fs';
import { inflateSync, deflateSync, crc32 } from 'node:zlib';

export function readImage(path) {
  const buf = readFileSync(path);
  if (buf[0] === 0x50 && buf[1] === 0x36) return readPpm(buf, path);
  return readPng(buf, path);
}

function readPpm(buf, path) {
  // P6\n<w> <h>\n255\n<rgb...>, comments not supported (our writers emit none)
  const head = buf.toString('latin1', 0, 64).split(/\s+/);
  const w = +head[1], h = +head[2];
  const off = buf.indexOf(0x0a, buf.indexOf(0x0a, buf.indexOf(0x0a) + 1) + 1) + 1;
  if (!(w > 0 && h > 0)) throw new Error(`${path}: bad PPM header`);
  const rgba = new Uint8Array(w * h * 4);
  for (let i = 0; i < w * h; i++) {
    rgba[4 * i] = buf[off + 3 * i]; rgba[4 * i + 1] = buf[off + 3 * i + 1]; rgba[4 * i + 2] = buf[off + 3 * i + 2]; rgba[4 * i + 3] = 255;
  }
  return { w, h, rgba };
}

function readPng(buf, path) {
  if (buf.readUInt32BE(0) !== 0x89504e47) throw new Error(`${path}: not a PNG`);
  let off = 8, w = 0, h = 0, ct = 0, depth = 0;
  const idat = [];
  while (off < buf.length) {
    const len = buf.readUInt32BE(off), type = buf.toString('latin1', off + 4, off + 8);
    const data = buf.subarray(off + 8, off + 8 + len);
    if (type === 'IHDR') { w = data.readUInt32BE(0); h = data.readUInt32BE(4); depth = data[8]; ct = data[9]; }
    else if (type === 'IDAT') idat.push(data);
    off += 12 + len;
  }
  if (depth !== 8 || (ct !== 2 && ct !== 6)) throw new Error(`${path}: need 8-bit RGB/RGBA (got depth ${depth} type ${ct})`);
  const bpp = ct === 6 ? 4 : 3, stride = w * bpp;
  const raw = inflateSync(Buffer.concat(idat));
  const rgba = new Uint8Array(w * h * 4);
  let prev = new Uint8Array(stride);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)];
    const row = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    const cur = new Uint8Array(stride);
    for (let i = 0; i < stride; i++) {
      const a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
      let v = row[i];
      if (f === 1) v += a;
      else if (f === 2) v += b;
      else if (f === 3) v += (a + b) >> 1;
      else if (f === 4) { const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
      cur[i] = v & 255;
    }
    for (let x = 0; x < w; x++) {
      for (let k = 0; k < 3; k++) rgba[4 * (y * w + x) + k] = cur[x * bpp + k];
      rgba[4 * (y * w + x) + 3] = bpp === 4 ? cur[x * bpp + 3] : 255;
    }
    prev = cur;
  }
  return { w, h, rgba };
}

export function writePng(path, w, h, rgba, { flipY = false } = {}) {
  const stride = w * 4;
  const raw = Buffer.alloc((stride + 1) * h);
  for (let y = 0; y < h; y++) {
    const sy = flipY ? h - 1 - y : y;
    raw[y * (stride + 1)] = 0;
    Buffer.from(rgba.buffer, rgba.byteOffset + sy * stride, stride).copy(raw, y * (stride + 1) + 1);
  }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type, 'latin1'), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td) >>> 0);
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  writeFileSync(path, Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0)),
  ]));
}

// Compare two images. Returns share of differing pixels at thresholds, max
// channel delta, and the count of unique colours in each (a filtering bug or
// a black frame shows up as a very different count).
export function diffImages(a, b) {
  if (a.w !== b.w || a.h !== b.h) return { comparable: false, reason: `size ${a.w}x${a.h} vs ${b.w}x${b.h}` };
  let any = 0, gt2 = 0, gt16 = 0, max = 0;
  const n = a.w * a.h;
  for (let i = 0; i < n; i++) {
    let d = 0;
    for (let k = 0; k < 3; k++) d = Math.max(d, Math.abs(a.rgba[4 * i + k] - b.rgba[4 * i + k]));
    if (d > 0) any++;
    if (d > 2) gt2++;
    if (d > 16) gt16++;
    if (d > max) max = d;
  }
  return { comparable: true, pixels: n, any: any / n, gt2: gt2 / n, gt16: gt16 / n, max, colorsA: uniqueColors(a), colorsB: uniqueColors(b) };
}

export function uniqueColors(img) {
  const s = new Set();
  for (let i = 0; i < img.w * img.h; i++) s.add((img.rgba[4 * i] << 16) | (img.rgba[4 * i + 1] << 8) | img.rgba[4 * i + 2]);
  return s.size;
}
