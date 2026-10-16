#!/usr/bin/env node
// Builds a .cwhrtf (cartwheel's HRTF table, see src/audio/hrtf.c) from the MIT
// Media Lab KEMAR measurements (Bill Gardner and Keith Martin, 1994), the
// diffuse-field equalized set: deps/kemar (scripts/fetch-deps.sh) holds
// elev<E>/H<E>e<AAA>a.wav, 128 frames of 16-bit stereo at 44.1 kHz per
// position, right hemisphere only (azimuth 0..180, 90 = the listener's right;
// channel 0 the left ear, channel 1 the right ear).
//
//   node tools/hrtf/import-kemar.mjs [deps/kemar] [out.cwhrtf] [--rate 48000] [--taps 64]
//
// Steps: mirror to the left hemisphere (ears swapped): 710 positions; add a
// synthesized nadir (the mean of the -40 degree ring: KEMAR has nothing
// lower) so every direction falls in a triangle; resample to the mix rate
// (Kaiser-windowed sinc); measure each ear's onset (the ITD is the
// difference) and make each response minimum phase (real cepstrum), so the
// FIR holds only the spectrum and the delay is applied separately; keep
// `taps` taps with a short fade; normalize so the mean over directions of
// (left energy + right energy) is 1 (an equal-power pan's sum); quantize
// taps to int16 and delays to 1/256 sample, so the file's bytes do not
// depend on the last bit of a double.
//
// The triangulation is built from the measurement rings, not a convex hull:
// adjacent elevation rings are zipped by exact rational azimuth comparisons,
// so it is the same on every machine (a hull of the grid has coplanar quads
// whose diagonal would depend on rounding).
import fs from 'node:fs';
import path from 'node:path';

const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf(name);
  if (i < 0) return def;
  const v = args[i + 1];
  args.splice(i, 2);
  return Number(v);
};
const RATE = opt('--rate', 48000);
const TAPS = opt('--taps', 64);
const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../..');
const SRC = args[0] ?? path.join(ROOT, 'deps/kemar');
const OUT = args[1] ?? path.join(ROOT, 'src/audio/hrtf/kemar.cwhrtf');
const SRC_RATE = 44100;

/** @param {string} file @returns {[Float64Array, Float64Array]} */
function readWav(file) {
  const b = fs.readFileSync(file);
  if (b.toString('ascii', 0, 4) !== 'RIFF' || b.toString('ascii', 8, 12) !== 'WAVE') throw new Error(`${file}: not a WAV`);
  let off = 12, fmt = null, data = null;
  while (off + 8 <= b.length) {
    const id = b.toString('ascii', off, off + 4), len = b.readUInt32LE(off + 4);
    if (id === 'fmt ') fmt = { ch: b.readUInt16LE(off + 10), rate: b.readUInt32LE(off + 12), bits: b.readUInt16LE(off + 22) };
    if (id === 'data') data = b.subarray(off + 8, off + 8 + len);
    off += 8 + len + (len & 1);
  }
  if (!fmt || !data || fmt.ch !== 2 || fmt.bits !== 16 || fmt.rate !== SRC_RATE) throw new Error(`${file}: expected 16-bit stereo ${SRC_RATE} Hz`);
  const n = data.length / 4, l = new Float64Array(n), r = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    l[i] = data.readInt16LE(i * 4) / 32768;
    r[i] = data.readInt16LE(i * 4 + 2) / 32768;
  }
  return [l, r];
}

// ── rings ────────────────────────────────────────────────────────────────
/** @typedef {{ el: number, k: number, n: number, l: Float64Array, r: Float64Array }} Pos  azimuth = 360 k / n */
/** @type {Map<number, Pos[]>} elevation -> positions sorted by azimuth (clockwise from the front) */
const rings = new Map();
for (const dir of fs.readdirSync(SRC).filter((d) => /^elev-?\d+$/.test(d))) {
  const el = Number(dir.slice(4));
  const files = fs.readdirSync(path.join(SRC, dir)).filter((f) => /^H-?\d+e\d+a\.wav$/.test(f));
  files.sort((a, b) => Number(a.match(/e(\d+)a/)[1]) - Number(b.match(/e(\d+)a/)[1]));
  // file names round the azimuth; the measurement grid is 360/n exactly. A ring
  // with a 180 degree position has n = 2 (files - 1); one without (50 degrees:
  // 8 degree steps, 45 positions) has n = 2 files - 1
  const has180 = Number(files[files.length - 1].match(/e(\d+)a/)[1]) === 180;
  const n = files.length === 1 ? 1 : has180 ? 2 * (files.length - 1) : 2 * files.length - 1;
  const ring = [];
  files.forEach((f, k) => {
    const [l, r] = readWav(path.join(SRC, dir, f));
    ring.push({ el, k, n, l, r });
  });
  for (let k = has180 ? files.length - 2 : files.length - 1; k >= 1 && n > 1; k--) ring.push({ el, k: n - k, n, l: ring[k].r, r: ring[k].l }); // mirror
  rings.set(el, ring);
}
const elevs = [...rings.keys()].sort((a, b) => a - b);
if (elevs.length !== 14 || [...rings.values()].reduce((s, r) => s + r.length, 0) !== 710)
  throw new Error(`${SRC}: expected the 710-position KEMAR set (got ${elevs.length} rings)`);

// nadir: the mean of the lowest ring (no measurement below -40 degrees)
{
  const low = rings.get(elevs[0]);
  const len = low[0].l.length, l = new Float64Array(len), r = new Float64Array(len);
  for (const p of low) for (let i = 0; i < len; i++) { l[i] += p.l[i] / low.length; r[i] += p.r[i] / low.length; }
  rings.set(-90, [{ el: -90, k: 0, n: 1, l, r }]);
  elevs.unshift(-90);
}

// ── DSP ──────────────────────────────────────────────────────────────────
function besselI0(x) {
  let s = 1, t = 1;
  for (let k = 1; k < 40; k++) { t *= (x / (2 * k)) ** 2; s += t; }
  return s;
}
/** Band-limited resample (cutoff at the lower Nyquist). */
function resample(x, from, to) {
  if (from === to) return Float64Array.from(x);
  const ratio = to / from, n = Math.ceil(x.length * ratio), out = new Float64Array(n);
  const fc = Math.min(1, ratio), half = 32, beta = 8, i0b = besselI0(beta);
  for (let j = 0; j < n; j++) {
    const t = j / ratio;
    let s = 0;
    for (let i = Math.ceil(t - half); i <= Math.floor(t + half); i++) {
      if (i < 0 || i >= x.length) continue;
      const d = t - i, w = besselI0(beta * Math.sqrt(Math.max(0, 1 - (d / half) ** 2))) / i0b;
      const sinc = d === 0 ? 1 : Math.sin(Math.PI * fc * d) / (Math.PI * fc * d);
      s += x[i] * fc * sinc * w;
    }
    out[j] = s;
  }
  return out;
}
/** In-place radix-2 complex FFT; inverse when inv. */
function fft(re, im, inv) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = (2 * Math.PI / len) * (inv ? 1 : -1), wr = Math.cos(ang), wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < len / 2; k++) {
        const a = i + k, b = a + len / 2;
        const tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr; im[a] += ti;
        const ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
  if (inv) for (let i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
}
/** Minimum-phase version of h (same magnitude response), via the real cepstrum. */
function minimumPhase(h) {
  const N = 2048, re = new Float64Array(N), im = new Float64Array(N);
  re.set(h);
  fft(re, im, false);
  for (let i = 0; i < N; i++) { re[i] = Math.log(Math.max(Math.hypot(re[i], im[i]), 1e-9)); im[i] = 0; }
  fft(re, im, true); // real cepstrum
  for (let i = 1; i < N / 2; i++) { re[i] *= 2; re[N - i] = 0; }
  for (let i = 0; i < N; i++) im[i] = 0;
  fft(re, im, false);
  for (let i = 0; i < N; i++) { const m = Math.exp(re[i]); const a = im[i]; re[i] = m * Math.cos(a); im[i] = m * Math.sin(a); }
  fft(re, im, true);
  return re;
}
/** Onset in (fractional) samples: where |h| first reaches 20% of its peak. */
function onset(h) {
  let pk = 0;
  for (const v of h) pk = Math.max(pk, Math.abs(v));
  const th = pk * 0.2;
  for (let i = 0; i < h.length; i++) {
    if (Math.abs(h[i]) >= th) {
      if (i === 0) return 0;
      const a = Math.abs(h[i - 1]), b = Math.abs(h[i]);
      return i - 1 + (th - a) / (b - a);
    }
  }
  return 0;
}

// ── directions, filters, delays ──────────────────────────────────────────
const dirs = [], filt = [], delays = [];
const index = new Map(); // "el/k/n" -> direction index
for (const el of elevs) {
  for (const p of rings.get(el)) {
    const l = resample(p.l, SRC_RATE, RATE), r = resample(p.r, SRC_RATE, RATE);
    const ol = onset(l), or = onset(r), base = Math.min(ol, or);
    const ml = minimumPhase(l), mr = minimumPhase(r);
    const fl = new Float64Array(TAPS), fr = new Float64Array(TAPS), fade = Math.max(4, TAPS >> 3);
    for (let i = 0; i < TAPS; i++) {
      const w = i < TAPS - fade ? 1 : 0.5 + 0.5 * Math.cos(Math.PI * (i - (TAPS - fade) + 1) / (fade + 1));
      fl[i] = ml[i] * w;
      fr[i] = mr[i] * w;
    }
    const az = (2 * Math.PI * p.k) / p.n, er = (Math.PI * el) / 180;
    // listener space: x right, y up, -z forward; azimuth clockwise from the front
    dirs.push([Math.cos(er) * Math.sin(az), Math.sin(er), -Math.cos(er) * Math.cos(az)]);
    filt.push([fl, fr]);
    delays.push(el === -90 ? [0, 0] : [ol - base, or - base]);
    index.set(`${el}/${p.k}/${p.n}`, dirs.length - 1);
  }
}
// normalize: mean (El + Er) over the measured directions = 1
let esum = 0;
for (const [fl, fr] of filt) for (let i = 0; i < TAPS; i++) esum += fl[i] * fl[i] + fr[i] * fr[i];
const norm = 1 / Math.sqrt(esum / filt.length);
let peak = 0;
for (const [fl, fr] of filt) for (let i = 0; i < TAPS; i++) { fl[i] *= norm; fr[i] *= norm; peak = Math.max(peak, Math.abs(fl[i]), Math.abs(fr[i])); }
// the tap scale is a power of two, so int16 * scale is exact in float
const scale = 2 ** Math.ceil(Math.log2(peak / 32767));

// ── triangles: zip adjacent rings ────────────────────────────────────────
const tris = [];
for (let e = 0; e + 1 < elevs.length; e++) {
  const A = rings.get(elevs[e]), B = rings.get(elevs[e + 1]);
  const ia = A.map((p) => index.get(`${p.el}/${p.k}/${p.n}`)), ib = B.map((p) => index.get(`${p.el}/${p.k}/${p.n}`));
  const ka = A.map((p) => p.k), kb = B.map((p) => p.k), na = A[0].n, nb = B[0].n;
  if (A.length === 1 || B.length === 1) { // a pole: fan
    const pole = A.length === 1 ? ia[0] : ib[0], ring = A.length === 1 ? ib : ia, up = A.length === 1;
    for (let i = 0; i < ring.length; i++) {
      const a = ring[i], b = ring[(i + 1) % ring.length];
      tris.push(up ? [pole, b, a] : [pole, a, b]);
    }
    continue;
  }
  // walk both rings once around; advance the one whose next azimuth is smaller.
  // azimuth k/n compared as k1*n2 vs k2*n1 (exact); the last step wraps by +n
  let i = 0, j = 0;
  const az = (k, n, wrap) => (k + (wrap ? n : 0));
  while (i < A.length || j < B.length) {
    const ni = i + 1, nj = j + 1;
    const nextA = ni <= A.length ? az(ka[ni % A.length], na, ni === A.length) * nb : Infinity;
    const nextB = nj <= B.length ? az(kb[nj % B.length], nb, nj === B.length) * na : Infinity;
    if (nextA <= nextB && ni <= A.length) {
      tris.push([ia[i % A.length], ia[ni % A.length], ib[j % B.length]]);
      i = ni;
    } else {
      tris.push([ia[i % A.length], ib[nj % B.length], ib[j % B.length]]);
      j = nj;
    }
  }
}
// orient every triangle outward (counter-clockwise seen from outside) and check the cover
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
for (const t of tris) {
  const [a, b, c] = t.map((i) => dirs[i]);
  if (dot(cross(sub(b, a), sub(c, a)), a) < 0) [t[1], t[2]] = [t[2], t[1]];
}
// Euler: a closed triangulated sphere has F = 2V - 4
if (tris.length !== 2 * dirs.length - 4) throw new Error(`triangulation: ${tris.length} triangles for ${dirs.length} vertices`);

// ── write ────────────────────────────────────────────────────────────────
// header (little endian, 32 bytes):
//   "CWHRTF01" | u32 rate | u32 taps | u32 directions | u32 triangles | f32 tap scale | u32 0
// per direction (12 bytes): i16 x, y, z (unit vector * 32767) | i16 left delay, right delay (1/256 sample) | i16 0
// taps: per direction, left then right, `taps` i16 each (value = i16 * tap scale)
// triangles: u16 a, b, c each (counter-clockwise seen from outside)
const nd = dirs.length;
const buf = Buffer.alloc(32 + nd * 12 + nd * 2 * TAPS * 2 + tris.length * 6);
buf.write('CWHRTF01', 0, 'ascii');
buf.writeUInt32LE(RATE, 8);
buf.writeUInt32LE(TAPS, 12);
buf.writeUInt32LE(nd, 16);
buf.writeUInt32LE(tris.length, 20);
buf.writeFloatLE(scale, 24);
let o = 32;
const q = (v, s, lim = 32767) => Math.max(-lim, Math.min(lim, Math.round(v * s)));
for (let d = 0; d < nd; d++) {
  for (let c = 0; c < 3; c++) buf.writeInt16LE(q(dirs[d][c], 32767), o + c * 2);
  buf.writeInt16LE(q(delays[d][0], 256), o + 6);
  buf.writeInt16LE(q(delays[d][1], 256), o + 8);
  o += 12;
}
for (let d = 0; d < nd; d++)
  for (const f of filt[d])
    for (let i = 0; i < TAPS; i++, o += 2) buf.writeInt16LE(q(f[i], 1 / scale), o);
for (const t of tris) for (let c = 0; c < 3; c++, o += 2) buf.writeUInt16LE(t[c], o);
fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, buf);
const maxItd = Math.max(...delays.map(([a, b]) => Math.max(a, b)));
console.log(`${path.relative(process.cwd(), OUT)}: ${nd} directions, ${tris.length} triangles, ${TAPS} taps at ${RATE} Hz, max ITD ${(maxItd / RATE * 1e3).toFixed(3)} ms, ${buf.length} bytes`);
