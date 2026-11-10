#!/usr/bin/env node
// check-art: a built cart's launcher art (wasmcart's manifest `art`, SPEC.md "art") against what a cartwheel game
// ships: every picture there, each a real PNG or JPEG of the right shape and at least the SPEC's size.
//
//   node tools/check-art.mjs <cart.wasc | dir of .wasc> [...]
//
// Problems, one line each (`<cart>: <problem>`), exit 1 on any:
//   - no `title` (the manifest's name for people; `name` is the cart's id);
//   - no art, or a key missing: icon, cover, background, logo, title and at least one screenshot;
//   - a path that is not in the archive, or a file that is not a whole PNG or JPEG (signature, header, every PNG
//     chunk's CRC and its image data inflating to the full size, a JPEG's frame, scan and end marker), or whose
//     extension names the other format;
//   - icon not square or under 256 x 256; cover not portrait 2:3 or under 600 x 900; background not 16:9 or under
//     1920 x 1080; logo not a PNG, not wider than tall, under 1280 wide and 720 tall, or without real transparency
//     (under 5% of its pixels at most half opaque: an RGBA file of solid pixels is not transparent); title and
//     screenshots under the cart's size (the manifest's width x height, else 1920 x 1080: a cartwheel cart takes the
//     host's size, so a 1080p capture is always possible) or, with a declared size, of another aspect; a screenshot
//     with no label (the SPEC's text for screen readers).
// Aspects are held to 1%.
//
// The cover must also show the game's name, and the background must have none; no text detector reads a picture
// here, so each cart with a cover prints `<cart>: cover: name not machine-checked`: look at the covers.
import { readdirSync, statSync } from 'node:fs';
import { basename, join, resolve } from 'node:path';
import { inflateSync, crc32 } from 'node:zlib';
import { readNamedEntries } from './lib/wasc-splice.mjs';

const KEYS = ['icon', 'cover', 'background', 'logo', 'title'];
const ASPECT_TOL = 0.01;
const LOGO_CLEAR_MIN = 0.05; // the share of a logo's pixels that must be at most half opaque

/** a PNG, fully decoded far enough to know it is whole and how much of it is transparent */
function readPng(b) {
  let off = 8, ihdr = null, plte = null, trns = null, end = false;
  const idat = [];
  while (off + 12 <= b.length) {
    const len = b.readUInt32BE(off), type = b.toString('latin1', off + 4, off + 8);
    if (off + 12 + len > b.length) return { error: `the ${type} chunk runs past the end of the file (truncated)` };
    const data = b.subarray(off + 8, off + 8 + len);
    if (crc32(b.subarray(off + 4, off + 8 + len)) !== b.readUInt32BE(off + 8 + len)) return { error: `the ${type} chunk's CRC is wrong` };
    if (type === 'IHDR') ihdr = { w: data.readUInt32BE(0), h: data.readUInt32BE(4), depth: data[8], ct: data[9], interlace: data[12] };
    else if (type === 'PLTE') plte = data;
    else if (type === 'tRNS') trns = data;
    else if (type === 'IDAT') idat.push(data);
    else if (type === 'IEND') { end = true; break; }
    off += 12 + len;
  }
  if (!ihdr) return { error: 'no IHDR' };
  if (!end) return { error: 'no IEND (truncated)' };
  const { w, h, depth, ct, interlace } = ihdr;
  const channels = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[ct];
  if (!channels || ![1, 2, 4, 8, 16].includes(depth) || (ct === 3 && !plte)) return { error: `colour type ${ct} at depth ${depth} is not a valid PNG` };
  let raw;
  try { raw = inflateSync(Buffer.concat(idat)); } catch (e) { return { error: `its image data does not inflate (${e.message})` }; }
  const bits = channels * depth, bpp = Math.max(1, bits >> 3);
  const passes = interlace ? [[0, 0, 8, 8], [4, 0, 8, 8], [0, 4, 4, 8], [2, 0, 4, 4], [0, 2, 2, 4], [1, 0, 2, 2], [0, 1, 1, 2]] : [[0, 0, 1, 1]];
  const maxv = (1 << Math.min(depth, 8)) - 1;
  // the alpha of one pixel in [0, 1], from the unfiltered row
  const sample = (row, x, c) => {
    if (depth === 16) return row[(x * channels + c) * 2];
    if (depth === 8) return row[x * channels + c];
    const bit = (x * channels + c) * depth;
    return (row[bit >> 3] >> (8 - depth - (bit & 7))) & maxv;
  };
  const key = trns && (ct === 0 || ct === 2) ? Array.from({ length: channels }, (_, c) => trns.readUInt16BE(c * 2)) : null;
  const alphaOf = (row, x) => {
    if (ct === 6 || ct === 4) return sample(row, x, channels - 1) / (depth === 16 ? 255 : maxv);
    if (ct === 3) { const i = sample(row, x, 0); return trns && i < trns.length ? trns[i] / 255 : 1; }
    if (key) {
      for (let c = 0; c < channels; c++) {
        const v = depth === 16 ? (row[(x * channels + c) * 2] << 8) | row[(x * channels + c) * 2 + 1] : sample(row, x, c);
        if (v !== key[c]) return 1;
      }
      return 0;
    }
    return 1;
  };
  let p = 0, clear = 0;
  for (const [x0, y0, dx, dy] of passes) {
    const pw = Math.ceil((w - x0) / dx), ph = Math.ceil((h - y0) / dy);
    if (pw <= 0 || ph <= 0) continue;
    const stride = Math.ceil((pw * bits) / 8);
    let prev = new Uint8Array(stride);
    for (let y = 0; y < ph; y++) {
      if (p + 1 + stride > raw.length) return { error: 'its image data is shorter than the picture (truncated)' };
      const f = raw[p], src = raw.subarray(p + 1, p + 1 + stride), cur = new Uint8Array(stride);
      if (f > 4) return { error: `row filter ${f} is not a PNG filter` };
      for (let i = 0; i < stride; i++) {
        const a = i >= bpp ? cur[i - bpp] : 0, u = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
        let v = src[i];
        if (f === 1) v += a;
        else if (f === 2) v += u;
        else if (f === 3) v += (a + u) >> 1;
        else if (f === 4) { const q = a + u - c, pa = Math.abs(q - a), pb = Math.abs(q - u), pc = Math.abs(q - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? u : c; }
        cur[i] = v & 255;
      }
      for (let x = 0; x < pw; x++) if (alphaOf(cur, x) <= 0.5) clear++;
      prev = cur;
      p += 1 + stride;
    }
  }
  return { type: 'png', w, h, clear: clear / (w * h) };
}

/** a JPEG's size, and that it has a frame, a scan and its end marker */
function readJpeg(b) {
  let o = 2, frame = null;
  while (o + 4 <= b.length) {
    if (b[o] !== 0xff) return { error: `no marker at byte ${o}` };
    const m = b[o + 1];
    if (m === 0xff) { o++; continue; }
    if (m === 0xd8 || m === 0x01 || (m >= 0xd0 && m <= 0xd7)) { o += 2; continue; }
    const len = b.readUInt16BE(o + 2);
    if (m >= 0xc0 && m <= 0xcf && m !== 0xc4 && m !== 0xc8 && m !== 0xcc) frame = { w: b.readUInt16BE(o + 7), h: b.readUInt16BE(o + 5) };
    if (m === 0xda) {
      if (!frame) return { error: 'a scan before any frame header' };
      // the entropy-coded data runs to the end marker; a file cut short has none (progressive files hold several scans)
      const tail = b.subarray(Math.max(o, b.length - 1024));
      let e = -1;
      for (let i = tail.length - 2; i >= 0; i--) if (tail[i] === 0xff && tail[i + 1] === 0xd9) { e = i; break; }
      if (e < 0) return { error: 'no end-of-image marker (truncated)' };
      return { type: 'jpeg', w: frame.w, h: frame.h, clear: 0 };
    }
    o += 2 + len;
  }
  return { error: 'no scan (truncated)' };
}

/** the picture's format, size and transparency, or { error } for a file that is not a whole PNG or JPEG */
export function readPicture(b) {
  if (!b || b.length < 8) return { error: 'empty' };
  if (b.readUInt32BE(0) === 0x89504e47 && b.readUInt32BE(4) === 0x0d0a1a0a) return readPng(b);
  if (b[0] === 0xff && b[1] === 0xd8) return readJpeg(b);
  return { error: 'not a PNG or JPEG' };
}

const aspectOff = (w, h, want) => Math.abs(w / h - want) / want > ASPECT_TOL;
const safePath = (p) => typeof p === 'string' && p.length > 0 && !p.startsWith('/') && !p.split(/[\\/]/).includes('..');

/**
 * Problems with a cart's art, and the notes a person must check by eye.
 * @param {object | null} manifest
 * @param {(path: string) => Buffer | undefined} read
 * @returns {{ problems: string[], notes: string[] }}
 */
export function checkArt(manifest, read) {
  const problems = [], notes = [];
  const art = manifest && manifest.art;
  if (!manifest) return { problems: ['no manifest.json'], notes };
  if (typeof manifest.title !== 'string' || !manifest.title.trim()) problems.push('title: missing (the game\'s name as people read it: project.cwproj `title`, cw_add_app TITLE)');
  if (!art || typeof art !== 'object') return { problems: [...problems, 'no art in the manifest'], notes };
  const minShot = manifest.width > 0 && manifest.height > 0 ? { w: manifest.width, h: manifest.height, declared: true } : { w: 1920, h: 1080 };
  const shots = Array.isArray(art.screenshots) ? art.screenshots : [];
  const items = [];
  for (const k of KEYS) {
    if (typeof art[k] !== 'string') problems.push(`${k}: missing`);
    else items.push([k, art[k]]);
  }
  if (!shots.length) problems.push('screenshots: none (at least one)');
  shots.forEach((s, i) => {
    const what = `screenshot ${i + 1}`;
    if (!s || typeof s.path !== 'string') { problems.push(`${what}: no path`); return; }
    if (!s.label || typeof s.label !== 'string') problems.push(`${what}: no label (the text screen readers and captions use)`);
    items.push([what, s.path]);
  });
  for (const [k, path] of items) {
    if (!safePath(path)) { problems.push(`${k}: "${path}" is not a path inside the cart`); continue; }
    const b = read(path);
    if (!b) { problems.push(`${k}: ${path} is not in the cart`); continue; }
    const pic = readPicture(b);
    if (pic.error) { problems.push(`${k}: ${path} is not a whole PNG or JPEG: ${pic.error}`); continue; }
    const ext = /\.png$/i.test(path) ? 'png' : /\.jpe?g$/i.test(path) ? 'jpeg' : null;
    if (ext && ext !== pic.type) problems.push(`${k}: ${path} is a ${pic.type.toUpperCase()} named .${ext === 'png' ? 'png' : 'jpg'}`);
    const { w, h } = pic, size = `${w} x ${h}`;
    if (k === 'icon') {
      if (w !== h) problems.push(`icon: ${path} is ${size}, not square`);
      else if (w < 256) problems.push(`icon: ${path} is ${size}, under 256 x 256`);
    } else if (k === 'cover') {
      if (aspectOff(w, h, 2 / 3)) problems.push(`cover: ${path} is ${size}, not portrait 2:3`);
      else if (w < 600 || h < 900) problems.push(`cover: ${path} is ${size}, under 600 x 900`);
    } else if (k === 'background') {
      if (aspectOff(w, h, 16 / 9)) problems.push(`background: ${path} is ${size}, not 16:9`);
      else if (w < 1920 || h < 1080) problems.push(`background: ${path} is ${size}, under 1920 x 1080`);
    } else if (k === 'logo') {
      if (pic.type !== 'png') problems.push(`logo: ${path} is a JPEG; a logo is a PNG with a transparent background`);
      else if (pic.clear < LOGO_CLEAR_MIN) problems.push(`logo: ${path} has no real transparency (${(pic.clear * 100).toFixed(1)}% of its pixels at most half opaque, under ${LOGO_CLEAR_MIN * 100}%)`);
      if (w <= h) problems.push(`logo: ${path} is ${size}, not wider than tall`);
      else if (w < 1280 && h < 720) problems.push(`logo: ${path} is ${size}, under 1280 wide and 720 tall`);
    } else {
      if (minShot.declared && aspectOff(w, h, minShot.w / minShot.h)) problems.push(`${k}: ${path} is ${size}, not the cart's ${minShot.w} x ${minShot.h} aspect`);
      else if (w < minShot.w || h < minShot.h) problems.push(`${k}: ${path} is ${size}, under ${minShot.declared ? "the cart's " : ''}${minShot.w} x ${minShot.h}`);
    }
  }
  if (typeof art.cover === 'string') notes.push('cover: name not machine-checked');
  return { problems, notes };
}

/** a .wasc's manifest and its art files (nothing else is inflated) */
function openCart(path) {
  let m;
  const first = readNamedEntries(path, ['manifest.json']).get('manifest.json');
  if (!first) return { manifest: null, read: () => undefined };
  try { m = JSON.parse(first.toString('utf8')); } catch (e) { throw new Error(`manifest.json is not JSON: ${e.message}`); }
  const art = m && m.art && typeof m.art === 'object' ? m.art : {};
  const paths = [...KEYS.map((k) => art[k]), ...(Array.isArray(art.screenshots) ? art.screenshots.map((s) => s && s.path) : [])].filter((p) => typeof p === 'string');
  const files = readNamedEntries(path, paths);
  return { manifest: m, read: (p) => files.get(p) };
}

if (import.meta.url === `file://${process.argv[1]}` || process.argv[1]?.endsWith('check-art.mjs')) {
  const args = process.argv.slice(2);
  if (!args.length || args.includes('--help') || args.includes('-h')) {
    console.log('usage: node tools/check-art.mjs <cart.wasc | dir of .wasc> [...]');
    process.exit(args.length ? 0 : 2);
  }
  const carts = [];
  for (const a of args) {
    const p = resolve(a);
    const st = statSync(p, { throwIfNoEntry: false });
    if (!st) { console.log(`${a}: not found`); process.exitCode = 1; continue; }
    if (st.isDirectory()) carts.push(...readdirSync(p).filter((n) => /\.wasc$/i.test(n)).sort().map((n) => join(p, n)));
    else carts.push(p);
  }
  let bad = 0;
  for (const c of carts) {
    const name = basename(c).replace(/\.wasc$/i, '');
    let r;
    try {
      const { manifest, read } = openCart(c);
      r = checkArt(manifest, read);
    } catch (e) {
      r = { problems: [`cannot read the cart: ${e.message}`], notes: [] };
    }
    for (const p of r.problems) console.log(`${name}: ${p}`);
    if (r.problems.length) bad++;
    else console.log(`${name}: art ok`);
    for (const n of r.notes) console.log(`${name}: ${n}`);
  }
  if (carts.length > 1) console.log(`${carts.length - bad} of ${carts.length} carts' art ok`);
  if (bad) process.exitCode = 1;
}
