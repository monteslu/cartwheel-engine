// Generates action2d's music and sound effects (CC0: synthesized here from
// code; no recorded or third-party audio). Music is encoded to Ogg Vorbis with
// ffmpeg; effects stay 16-bit mono WAV (small, decoded once at load).
//   node games/action2d/make-audio.mjs      (needs ffmpeg with libvorbis)
import { writeFileSync, mkdirSync, rmSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, 'assets', 'audio');
const TMP = join(resolve(HERE, '../..'), 'out', 'action2d-audio');
for (const d of [OUT, TMP]) mkdirSync(d, { recursive: true });
const RATE = 44100, TAU = Math.PI * 2;
let seed = 99;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296) * 2 - 1;

function wav(path, ch, data) {
  const n = data.length, buf = Buffer.alloc(44 + n * 2);
  buf.write('RIFF', 0); buf.writeUInt32LE(36 + n * 2, 4); buf.write('WAVE', 8);
  buf.write('fmt ', 12); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20); buf.writeUInt16LE(ch, 22);
  buf.writeUInt32LE(RATE, 24); buf.writeUInt32LE(RATE * ch * 2, 28); buf.writeUInt16LE(ch * 2, 32); buf.writeUInt16LE(16, 34);
  buf.write('data', 36); buf.writeUInt32LE(n * 2, 40);
  for (let i = 0; i < n; i++) buf.writeInt16LE(Math.max(-32767, Math.min(32767, Math.round(data[i] * 32767))), 44 + i * 2);
  writeFileSync(path, buf);
}
const sq = (p, duty = 0.5) => (p - Math.floor(p) < duty ? 1 : -1);
const tri = (p) => { const f = p - Math.floor(p); return 4 * Math.abs(f - 0.5) - 1; };
const midi = (n) => 440 * 2 ** ((n - 69) / 12);

// ── effects ──────────────────────────────────────────────────────────────
function sfx(name, seconds, fn) {
  const n = Math.round(seconds * RATE), out = new Float32Array(n);
  let phase = 0;
  for (let i = 0; i < n; i++) { const t = i / RATE; out[i] = fn(t, i, (f) => (phase += f / RATE)); }
  // 3 ms fade-out so nothing clicks
  for (let i = 0; i < Math.min(n, 132); i++) out[n - 1 - i] *= i / 132;
  wav(join(OUT, name + '.wav'), 1, out);
}
sfx('swing', 0.16, (t) => rnd() * 0.5 * Math.exp(-t * 18) * (0.4 + 0.6 * Math.sin(TAU * 900 * t * (1 - t * 3))));
sfx('hit', 0.14, (t, i, ph) => (sq(ph(220 - t * 900)) * 0.35 + rnd() * 0.25) * Math.exp(-t * 22));
sfx('hurt', 0.3, (t, i, ph) => sq(ph(420 - t * 900), 0.3) * 0.35 * Math.exp(-t * 8));
sfx('pickup', 0.28, (t, i, ph) => sq(ph(t < 0.08 ? 660 : t < 0.16 ? 880 : 1320), 0.25) * 0.28 * Math.exp(-t * 5));
sfx('gem', 0.22, (t, i, ph) => tri(ph(1320 + Math.sin(t * 80) * 200)) * 0.4 * Math.exp(-t * 9));
sfx('die', 0.4, (t, i, ph) => (sq(ph(300 * Math.exp(-t * 6)), 0.5) * 0.3 + rnd() * 0.2 * Math.exp(-t * 10)) * Math.exp(-t * 5));
sfx('shoot', 0.2, (t, i, ph) => sq(ph(880 * Math.exp(-t * 9)), 0.25) * 0.25 * Math.exp(-t * 9));
sfx('door', 0.6, (t, i, ph) => (tri(ph(70 + Math.sin(t * 40) * 6)) * 0.5 + rnd() * 0.15) * Math.min(1, t * 20) * Math.exp(-t * 3.5));
sfx('move', 0.05, (t, i, ph) => sq(ph(1200), 0.5) * 0.15 * Math.exp(-t * 60));
sfx('select', 0.18, (t, i, ph) => sq(ph(t < 0.06 ? 880 : 1760), 0.5) * 0.2 * Math.exp(-t * 14));
sfx('roar', 1.0, (t, i, ph) => (sq(ph(70 + Math.sin(t * 30) * 12), 0.4) * 0.35 + rnd() * 0.25) * Math.min(1, t * 8) * Math.exp(-t * 2.2));
sfx('boom', 0.9, (t, i, ph) => (Math.sin(ph(90 * Math.exp(-t * 3))) * 0.6 + rnd() * 0.4 * Math.exp(-t * 4)) * Math.exp(-t * 3));
function jingle(name, notes, step, wave = 0.5) {
  sfx(name, notes.length * step + 0.6, (t, i, ph) => {
    const k = Math.min(notes.length - 1, Math.floor(t / step)), local = t - k * step;
    const n = notes[k];
    const env = k === notes.length - 1 ? Math.exp(-local * 2.5) : Math.exp(-local * 6);
    return n ? (sq(ph(midi(n)), wave) * 0.22 + tri(t * midi(n - 12)) * 0.2) * env : 0;
  });
}
jingle('victory', [67, 72, 76, 79, 76, 79, 84], 0.13);
jingle('gameover', [64, 62, 60, 59, 55, 52], 0.2, 0.3);

// ── music: a tiny tracker (square lead, triangle bass, noise drums) ───────
function music(name, bpm, bars, chords, lead, opts = {}) {
  const beat = 60 / bpm, len = bars * 4 * beat, n = Math.round(len * RATE);
  const out = new Float32Array(n * 2);
  const add = (t0, dur, fn, pan = 0) => {
    const a = Math.round(t0 * RATE), b = Math.min(n, Math.round((t0 + dur) * RATE));
    let p = 0;
    for (let i = a; i < b; i++) {
      const t = (i - a) / RATE, v = fn(t, (f) => (p += f / RATE));
      out[2 * i] += v * (1 - pan) * 0.5 + v * 0.5; out[2 * i + 1] += v * (1 + pan) * 0.5 + v * 0.5;
    }
  };
  for (let bar = 0; bar < bars; bar++) {
    const ch = chords[bar % chords.length];
    const t0 = bar * 4 * beat;
    // bass: root on eighths, octave bounce
    for (let e = 0; e < 8; e++) {
      const note = ch[0] - 24 + (e % 2 ? 12 : 0);
      add(t0 + e * beat / 2, beat / 2 * 0.9, (t, ph) => tri(ph(midi(note))) * 0.32 * Math.min(1, t * 200) * Math.exp(-t * 3));
    }
    // arpeggio: chord tones on sixteenths, quiet
    for (let s = 0; s < 16; s++) {
      const note = ch[s % ch.length] + (s >= 8 ? 12 : 0);
      add(t0 + s * beat / 4, beat / 4 * 0.8, (t, ph) => sq(ph(midi(note)), 0.125) * 0.06 * Math.exp(-t * 10), -0.4);
    }
    // drums
    for (let q = 0; q < 4; q++) {
      const tq = t0 + q * beat;
      if (q % 2 === 0 || opts.fourFloor) add(tq, 0.25, (t, ph) => Math.sin(ph(55 + 90 * Math.exp(-t * 30))) * 0.5 * Math.exp(-t * 10));
      if (q % 2 === 1) add(tq, 0.18, (t) => rnd() * 0.22 * Math.exp(-t * 18));
      add(tq + beat / 2, 0.05, (t) => rnd() * 0.08 * Math.exp(-t * 60), 0.5);
    }
  }
  // lead: [bar, beat, length in beats, midi]
  for (const [bar, b, l, note] of lead)
    add((bar * 4 + b) * beat, l * beat * 0.95, (t, ph) => {
      const vib = t > 0.15 ? Math.sin(t * 34) * 0.006 : 0;
      return sq(ph(midi(note) * (1 + vib)), 0.25) * 0.11 * Math.min(1, t * 120) * Math.exp(-t * 1.2);
    }, 0.3);
  let peak = 0;
  for (const v of out) peak = Math.max(peak, Math.abs(v));
  for (let i = 0; i < out.length; i++) out[i] *= 0.8 / peak;
  wav(join(TMP, name + '.wav'), 2, out);
  const r = spawnSync('ffmpeg', ['-y', '-loglevel', 'error', '-i', join(TMP, name + '.wav'), '-c:a', 'libvorbis', '-q:a', '3',
    '-map_metadata', '-1', '-fflags', '+bitexact', join(OUT, name + '.ogg')], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error('ffmpeg: ' + r.stderr);
}
// A minor crypt theme: Am F G Em | Am F E E
const Am = [57, 60, 64], F = [53, 57, 60], G = [55, 59, 62], Em = [52, 55, 59], E = [52, 56, 59], Dm = [50, 53, 57];
music('crypt', 112, 8, [Am, F, G, Em, Am, F, E, E], [
  [0, 0, 1.5, 69], [0, 1.5, 0.5, 71], [0, 2, 2, 72], [1, 0, 1, 69], [1, 1, 1, 65], [1, 2, 2, 69],
  [2, 0, 1.5, 67], [2, 1.5, 0.5, 69], [2, 2, 2, 71], [3, 0, 3, 64], [4, 0, 1, 76], [4, 1, 1, 74], [4, 2, 2, 72],
  [5, 0, 1, 72], [5, 1, 1, 69], [5, 2, 2, 65], [6, 0, 2, 68], [6, 2, 2, 71], [7, 0, 4, 69],
]);
// The boss: faster, D minor with a chromatic climb
music('boss', 150, 8, [Dm, Dm, [51, 55, 58], [51, 55, 58], Dm, [52, 55, 58], [53, 57, 60], [52, 56, 59]], [
  [0, 0, 0.5, 74], [0, 0.5, 0.5, 74], [0, 1, 1, 77], [0, 2, 0.5, 74], [0, 2.5, 1.5, 72], [1, 0, 2, 74], [1, 2, 2, 69],
  [2, 0, 0.5, 75], [2, 0.5, 0.5, 75], [2, 1, 1, 79], [2, 2, 0.5, 75], [2, 2.5, 1.5, 74], [3, 0, 4, 70],
  [4, 0, 1, 74], [4, 1, 1, 76], [4, 2, 1, 77], [4, 3, 1, 79], [5, 0, 2, 81], [5, 2, 2, 79], [6, 0, 2, 77], [6, 2, 2, 81], [7, 0, 4, 80],
], { fourFloor: true });
rmSync(TMP, { recursive: true, force: true });
console.log('wrote', OUT);
