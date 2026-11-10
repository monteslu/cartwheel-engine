// A cart's archive with a new cart.wasm and everything else kept: the fast path of a dev cart's
// pack (tools/cart-version.mjs pack --reuse-assets). After a code change the assets, art and
// manifest are the same, but packing again deflates every asset (seconds for a cart with 100+ MB
// of models); this copies their entries byte for byte from the last archive and writes the one
// entry that changed, as wasmcart-pack writes it (yazl's layout: UTF-8 names, a data descriptor,
// no extra fields; deflated above wasmcart-pack's 4 MiB threshold). The entries, their order and
// their contents are those a fresh pack gives (tests/cart-repack.mjs checks it against one).
//
//   spliceWasm(oldWasc, wasmPath, outPath) -> true, or a string saying why it could not
//   readEntries(wasc) -> [{ name, data }] in archive order (decompressed), for checks
//   readNamedEntries(wasc, names) -> Map name -> data, only those entries (tools/check-art.mjs)
import { closeSync, openSync, readFileSync, statSync, writeSync } from 'node:fs';
import { deflateRawSync, inflateRawSync } from 'node:zlib';

// wasmcart-pack.js: a cart.wasm above this is deflated, at or below it stored
const WASM_COMPRESS_THRESHOLD = 4 * 1024 * 1024;

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
// yazl's dateToDosDateTime (local time)
function dosDateTime(d) {
  let date = d.getDate() & 0x1f;
  date |= ((d.getMonth() + 1) & 0xf) << 5;
  date |= ((d.getFullYear() - 1980) & 0x7f) << 9;
  let time = Math.floor(d.getSeconds() / 2);
  time |= (d.getMinutes() & 0x3f) << 5;
  time |= (d.getHours() & 0x1f) << 11;
  return { date, time };
}

/** the central directory of a zip: entries with their header fields, or a string saying why not */
function centralDirectory(buf) {
  const min = Math.max(0, buf.length - 22 - 0xffff);
  let eocd = -1;
  for (let i = buf.length - 22; i >= min; i--) if (buf.readUInt32LE(i) === 0x06054b50) { eocd = i; break; }
  if (eocd < 0) return 'no end of central directory record';
  const count = buf.readUInt16LE(eocd + 10), cdSize = buf.readUInt32LE(eocd + 12), cdOff = buf.readUInt32LE(eocd + 16);
  if (count === 0xffff || cdOff === 0xffffffff) return 'a zip64 archive';
  const entries = [];
  for (let p = cdOff, i = 0; i < count; i++) {
    if (buf.readUInt32LE(p) !== 0x02014b50) return 'a damaged central directory';
    const nameLen = buf.readUInt16LE(p + 28), extraLen = buf.readUInt16LE(p + 30), commentLen = buf.readUInt16LE(p + 32);
    const len = 46 + nameLen + extraLen + commentLen;
    entries.push({
      name: buf.toString('utf8', p + 46, p + 46 + nameLen), method: buf.readUInt16LE(p + 10), csize: buf.readUInt32LE(p + 20),
      offset: buf.readUInt32LE(p + 42), record: buf.subarray(p, p + len),
    });
    p += len;
  }
  return { entries, eocd, cdOff, cdSize };
}

/** every entry's name and decompressed bytes, in archive order */
export function readEntries(wasc) {
  const buf = readFileSync(wasc);
  const cd = centralDirectory(buf);
  if (typeof cd === 'string') throw new Error(`${wasc}: ${cd}`);
  return cd.entries.map((e) => {
    const nameLen = buf.readUInt16LE(e.offset + 26), extraLen = buf.readUInt16LE(e.offset + 28);
    const raw = buf.subarray(e.offset + 30 + nameLen + extraLen, e.offset + 30 + nameLen + extraLen + e.csize);
    return { name: e.name, data: e.method === 8 ? inflateRawSync(raw) : Buffer.from(raw) };
  });
}

/** the named entries' decompressed bytes (a Map; a name the archive lacks is left out), inflating
 *  nothing else: a cart's manifest and art without its assets */
export function readNamedEntries(wasc, names) {
  const buf = readFileSync(wasc);
  const cd = centralDirectory(buf);
  if (typeof cd === 'string') throw new Error(cd);
  const want = new Set(names), out = new Map();
  for (const e of cd.entries) {
    if (!want.has(e.name)) continue;
    const nameLen = buf.readUInt16LE(e.offset + 26), extraLen = buf.readUInt16LE(e.offset + 28);
    const raw = buf.subarray(e.offset + 30 + nameLen + extraLen, e.offset + 30 + nameLen + extraLen + e.csize);
    out.set(e.name, e.method === 8 ? inflateRawSync(raw) : Buffer.from(raw));
  }
  return out;
}

/** write oldWasc with its cart.wasm replaced by wasmPath to outPath: true, or why not */
export function spliceWasm(oldWasc, wasmPath, outPath) {
  let buf;
  try { buf = readFileSync(oldWasc); } catch (e) { return `cannot read ${oldWasc}: ${e.message}`; }
  const cd = centralDirectory(buf);
  if (typeof cd === 'string') return cd;
  const byOffset = [...cd.entries].sort((a, b) => a.offset - b.offset);
  const wasmEntry = cd.entries.find((e) => e.name === 'cart.wasm');
  if (!wasmEntry) return 'no cart.wasm in the old archive';
  // each entry's bytes run from its local header to the next one's (or the central directory)
  const span = new Map(byOffset.map((e, i) => [e, [e.offset, i + 1 < byOffset.length ? byOffset[i + 1].offset : cd.cdOff]]));

  const wasm = readFileSync(wasmPath);
  const compress = wasm.length > WASM_COMPRESS_THRESHOLD;
  const data = compress ? deflateRawSync(wasm) : wasm;
  const crc = crc32(wasm);
  const { date, time } = dosDateTime(statSync(wasmPath).mtime);
  const flags = (1 << 11) | (1 << 3); // UTF-8 name; crc and sizes in the data descriptor (as yazl's addFile)
  const name = Buffer.from('cart.wasm');
  const local = Buffer.alloc(30);
  local.writeUInt32LE(0x04034b50, 0);
  local.writeUInt16LE(20, 4);
  local.writeUInt16LE(flags, 6);
  local.writeUInt16LE(compress ? 8 : 0, 8);
  local.writeUInt16LE(time, 10);
  local.writeUInt16LE(date, 12);
  local.writeUInt16LE(name.length, 26);
  const desc = Buffer.alloc(16);
  desc.writeUInt32LE(0x08074b50, 0);
  desc.writeUInt32LE(crc, 4);
  desc.writeUInt32LE(data.length, 8);
  desc.writeUInt32LE(wasm.length, 12);

  const fd = openSync(outPath, 'w');
  try {
    let pos = 0;
    const put = (b) => { writeSync(fd, b); pos += b.length; };
    const newOffset = new Map();
    for (const e of byOffset) {
      newOffset.set(e, pos);
      if (e === wasmEntry) { put(local); put(name); put(data); put(desc); }
      else { const [a, b] = span.get(e); put(buf.subarray(a, b)); }
    }
    const cdStart = pos;
    for (const e of cd.entries) { // the central directory keeps its own order
      let rec;
      if (e === wasmEntry) {
        rec = Buffer.alloc(46);
        e.record.copy(rec, 0, 0, 46); // version made by, external attributes (the file's mode)
        rec.writeUInt16LE(20, 6);
        rec.writeUInt16LE(flags, 8);
        rec.writeUInt16LE(compress ? 8 : 0, 10);
        rec.writeUInt16LE(time, 12);
        rec.writeUInt16LE(date, 14);
        rec.writeUInt32LE(crc, 16);
        rec.writeUInt32LE(data.length, 20);
        rec.writeUInt32LE(wasm.length, 24);
        rec.writeUInt16LE(name.length, 28);
        rec.writeUInt16LE(0, 30);
        rec.writeUInt16LE(0, 32);
        rec = Buffer.concat([rec, name]);
      } else rec = Buffer.from(e.record);
      if (newOffset.get(e) > 0xfffffffe) throw new Error('the archive would need zip64');
      rec.writeUInt32LE(newOffset.get(e), 42);
      put(rec);
    }
    const eocd = Buffer.from(buf.subarray(cd.eocd));
    eocd.writeUInt32LE(pos - cdStart, 12);
    eocd.writeUInt32LE(cdStart, 16);
    put(eocd);
  } finally {
    closeSync(fd);
  }
  return true;
}

