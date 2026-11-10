// tools/check-art.mjs, with its controls: a cart with every picture right passes (and prints the cover note), and
// carts with one thing wrong each must fail with exactly that problem: a 16:9 cover, an RGBA logo with no transparent
// pixel, a truncated PNG, no title, a JPEG named .png, a small icon, a missing key, a screenshot with no label, and a cart with
// no art at all. A palette logo whose transparency is in tRNS passes. The carts are written here, as
// wasmcart-pack lays them out (stored entries are enough for the reader).
//
//   node tests/check-art.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { deflateSync, crc32 } from 'node:zlib';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const TOOL = join(ROOT, 'tools', 'check-art.mjs');
const OUT = join(ROOT, 'out', 'check-art');
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
const problems = [];

/** a PNG: colour type ct at 8 bits, every pixel px(x, y) -> its samples; extra chunks before IDAT */
function png(w, h, ct, px, extra = []) {
  const ch = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[ct];
  const raw = Buffer.alloc((w * ch + 1) * h);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) raw.set(px(x, y), y * (w * ch + 1) + 1 + x * ch);
  const chunk = (type, data) => {
    const b = Buffer.alloc(12 + data.length);
    b.writeUInt32BE(data.length, 0);
    b.write(type, 4, 'latin1');
    data.copy(b, 8);
    b.writeUInt32BE(crc32(b.subarray(4, 8 + data.length)), 8 + data.length);
    return b;
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = ct;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr),
    ...extra.map(([t, d]) => chunk(t, d)), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
/** a JPEG's structure: start, a baseline frame header, a scan header, a few bytes of scan, the end marker */
function jpeg(w, h) {
  const sof = [0xff, 0xc0, 0, 17, 8, h >> 8, h & 255, w >> 8, w & 255, 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1];
  const sos = [0xff, 0xda, 0, 12, 3, 1, 0, 2, 0x11, 3, 0x11, 0, 63, 0];
  return Buffer.from([0xff, 0xd8, ...sof, ...sos, 1, 2, 3, 4, 0xff, 0xd9]);
}
const rgb = (w, h) => png(w, h, 2, () => [40, 90, 160]);
// a wordmark: a solid bar across the middle third, clear above and below
const logo = (w, h) => png(w, h, 6, (x, y) => (y > h / 3 && y < (2 * h) / 3 ? [255, 255, 255, 255] : [0, 0, 0, 0]));

/** a stored zip (the central directory is all the reader needs) */
function zip(path, files) {
  const locals = [], central = [];
  let off = 0;
  for (const [name, data] of Object.entries(files)) {
    const n = Buffer.from(name), crc = crc32(data);
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0); lh.writeUInt16LE(20, 4); lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(data.length, 18); lh.writeUInt32LE(data.length, 22); lh.writeUInt16LE(n.length, 26);
    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0); ch.writeUInt16LE(20, 4); ch.writeUInt16LE(20, 6); ch.writeUInt32LE(crc, 16);
    ch.writeUInt32LE(data.length, 20); ch.writeUInt32LE(data.length, 24); ch.writeUInt16LE(n.length, 28); ch.writeUInt32LE(off, 42);
    locals.push(lh, n, data);
    central.push(ch, n);
    off += 30 + n.length + data.length;
  }
  const cd = Buffer.concat(central), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(central.length / 2, 8); end.writeUInt16LE(central.length / 2, 10);
  end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(off, 16);
  writeFileSync(path, Buffer.concat([...locals, cd, end]));
}

const GOOD = {
  art: { icon: 'art/icon.png', cover: 'art/cover.jpg', background: 'art/background.png', logo: 'art/logo.png', title: 'art/title.png',
    screenshots: [{ path: 'art/shot-1.png', label: 'A flat test picture' }] },
  files: { 'art/icon.png': rgb(256, 256), 'art/cover.jpg': jpeg(600, 900), 'art/background.png': rgb(1920, 1080),
    'art/logo.png': logo(1280, 320), 'art/title.png': rgb(1920, 1080), 'art/shot-1.png': rgb(1920, 1080) },
};
/** write <name>.wasc: the good cart with changes to its art and files */
function cart(name, { art = {}, files = {}, noArt = false, noTitle = false } = {}) {
  const a = { ...GOOD.art, ...art };
  for (const k of Object.keys(a)) if (a[k] === undefined) delete a[k];
  const manifest = { name, ...(noTitle ? {} : { title: 'Check Art: ' + name }), version: '1.0.0', abi: 4, entry: 'cart.wasm', ...(noArt ? {} : { art: a }) };
  const f = { 'manifest.json': Buffer.from(JSON.stringify(manifest)), 'cart.wasm': Buffer.from([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]), ...GOOD.files, ...files };
  zip(join(OUT, `${name}.wasc`), f);
}
const run = (...a) => spawnSync('node', [TOOL, ...a], { encoding: 'utf8' });

// the cart that must pass
cart('good');
let r = run(join(OUT, 'good.wasc'));
if (r.status !== 0 || r.stdout !== 'good: art ok\ngood: cover: name not machine-checked\n') problems.push(`the good cart: exit ${r.status}\n${r.stdout}${r.stderr}`);

// a palette logo, transparent through tRNS, passes too
cart('palette', { files: { 'art/logo.png': png(1280, 320, 3, (x, y) => [y > 106 && y < 213 ? 1 : 0], [['PLTE', Buffer.from([0, 0, 0, 255, 255, 255])], ['tRNS', Buffer.from([0])]]) } });
r = run(join(OUT, 'palette.wasc'));
if (r.status !== 0) problems.push(`a palette logo with tRNS transparency failed:\n${r.stdout}`);

// the controls that must fail, each with exactly its problem
const truncated = rgb(1920, 1080);
const controls = {
  widecover: [{ files: { 'art/cover.jpg': jpeg(1920, 1080) } }, 'cover: art/cover.jpg is 1920 x 1080, not portrait 2:3'],
  smallcover: [{ files: { 'art/cover.jpg': jpeg(400, 600) } }, 'cover: art/cover.jpg is 400 x 600, under 600 x 900'],
  opaquelogo: [{ files: { 'art/logo.png': png(1280, 320, 6, () => [255, 255, 255, 255]) } },
    'logo: art/logo.png has no real transparency (0.0% of its pixels at most half opaque, under 5%)'],
  smalllogo: [{ files: { 'art/logo.png': logo(1000, 300) } }, 'logo: art/logo.png is 1000 x 300, under 1280 wide and 720 tall'],
  jpeglogo: [{ art: { logo: 'art/logo.jpg' }, files: { 'art/logo.jpg': jpeg(1280, 320) } }, 'logo: art/logo.jpg is a JPEG; a logo is a PNG with a transparent background'],
  truncated: [{ files: { 'art/title.png': truncated.subarray(0, truncated.length - 40) } }, /^title: art\/title\.png is not a whole PNG or JPEG: /],
  misnamed: [{ files: { 'art/icon.png': jpeg(256, 256) } }, 'icon: art/icon.png is a JPEG named .png'],
  smallicon: [{ files: { 'art/icon.png': rgb(128, 128) } }, 'icon: art/icon.png is 128 x 128, under 256 x 256'],
  tallicon: [{ files: { 'art/icon.png': rgb(256, 300) } }, 'icon: art/icon.png is 256 x 300, not square'],
  narrowbg: [{ files: { 'art/background.png': rgb(1440, 1080) } }, 'background: art/background.png is 1440 x 1080, not 16:9'],
  smallshot: [{ files: { 'art/shot-1.png': rgb(1280, 720) } }, 'screenshot 1: art/shot-1.png is 1280 x 720, under 1920 x 1080'],
  nolabel: [{ art: { screenshots: [{ path: 'art/shot-1.png' }] } }, 'screenshot 1: no label (the text screen readers and captions use)'],
  noshots: [{ art: { screenshots: undefined } }, 'screenshots: none (at least one)'],
  nobackground: [{ art: { background: undefined } }, 'background: missing'],
  notincart: [{ art: { title: 'art/gone.png' } }, 'title: art/gone.png is not in the cart'],
  notapicture: [{ files: { 'art/background.png': Buffer.from('GIF89a, not a PNG') } }, 'background: art/background.png is not a whole PNG or JPEG: not a PNG or JPEG'],
  noart: [{ noArt: true }, 'no art in the manifest'],
  notitle: [{ noTitle: true }, "title: missing (the game's name as people read it: project.cwproj `title`, cw_add_app TITLE)"],
};
for (const [name, [opts, want]] of Object.entries(controls)) {
  cart(name, opts);
  r = run(join(OUT, `${name}.wasc`));
  const lines = r.stdout.split('\n').filter((l) => l && !l.endsWith('name not machine-checked')).map((l) => l.slice(name.length + 2));
  const match = lines.length === 1 && (typeof want === 'string' ? lines[0] === want : want.test(lines[0]));
  if (r.status !== 1 || !match) problems.push(`control ${name}: exit ${r.status}, wanted exactly "${want}", got:\n${r.stdout}`);
}

// a directory: every cart, a line each, and the count
r = run(OUT);
const total = 2 + Object.keys(controls).length;
if (r.status !== 1 || !r.stdout.includes(`2 of ${total} carts' art ok`)) problems.push(`the directory run: exit ${r.status}\n${r.stdout}`);

if (problems.length) {
  for (const p of problems) console.log(`FAIL ${p}`);
  process.exit(1);
}
console.log(`ok   check-art: the good carts pass, ${Object.keys(controls).length} controls fail with their problem`);
