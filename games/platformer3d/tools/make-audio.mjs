// Regenerates platformer3d's audio (all synthesized here: no third-party
// content). Music is written as WAV and encoded with ffmpeg (libopus /
// libvorbis); sound effects stay WAV.
//
//   node games/platformer3d/tools/make-audio.mjs
import { writeFileSync, mkdirSync, rmSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const D = join(dirname(fileURLToPath(import.meta.url)), '..', 'assets', 'audio');
const OUT = join(dirname(fileURLToPath(import.meta.url)), '..', '..', '..', 'out', 'platformer3d-audio');
mkdirSync(D, { recursive: true });
mkdirSync(OUT, { recursive: true });
const SR = 48000;

// a tiny deterministic noise source (so reruns write identical files)
let seed = 0x1234567;
const noise = () => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return ((seed >>> 8) / 8388608) - 1; };

function wav(path, L, R) {
  const n = L.length, ch = R ? 2 : 1;
  const b = Buffer.alloc(44 + n * ch * 2);
  b.write('RIFF', 0); b.writeUInt32LE(36 + n * ch * 2, 4); b.write('WAVE', 8); b.write('fmt ', 12);
  b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(ch, 22); b.writeUInt32LE(SR, 24);
  b.writeUInt32LE(SR * ch * 2, 28); b.writeUInt16LE(ch * 2, 32); b.writeUInt16LE(16, 34); b.write('data', 36);
  b.writeUInt32LE(n * ch * 2, 40);
  for (let i = 0; i < n; i++)
    for (let c = 0; c < ch; c++) {
      const v = Math.max(-1, Math.min(1, (c ? R : L)[i]));
      b.writeInt16LE(Math.round(v * 32767), 44 + (i * ch + c) * 2);
    }
  writeFileSync(path, b);
}

const midi = (m) => 440 * Math.pow(2, (m - 69) / 12);
const env = (t, a, d, s, len, r) => {
  if (t < 0) return 0;
  if (t < a) return t / a;
  if (t < a + d) return 1 - (1 - s) * ((t - a) / d);
  if (t < len) return s;
  if (t < len + r) return s * (1 - (t - len) / r);
  return 0;
};
// band-limited-ish oscillators (polyBLEP saw / square)
const blep = (t, dt) => {
  if (t < dt) { t /= dt; return t + t - t * t - 1; }
  if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
  return 0;
};
function osc(kind, phase, dt) {
  const p = phase % 1;
  if (kind === 'sine') return Math.sin(2 * Math.PI * p);
  if (kind === 'tri') return 1 - 4 * Math.abs(p - 0.5);
  if (kind === 'saw') return 2 * p - 1 - blep(p, dt);
  if (kind === 'sq') return (p < 0.5 ? 1 : -1) + blep(p, dt) - blep((p + 0.5) % 1, dt);
  return 0;
}

// ── a note renderer into a stereo buffer ─────────────────────────────
function mixNote(L, R, start, note) {
  const { freq, len, kind = 'saw', vol = 0.2, pan = 0, a = 0.005, d = 0.1, s = 0.6, r = 0.08, detune = 0, slide = 0, lp = 0 } = note;
  const n = Math.floor((len + r) * SR);
  const i0 = Math.floor(start * SR);
  const gl = Math.cos((pan + 1) * Math.PI / 4), gr = Math.sin((pan + 1) * Math.PI / 4);
  let ph1 = 0, ph2 = 0.37, y = 0;
  const k = lp ? 1 - Math.exp(-2 * Math.PI * lp / SR) : 1;
  for (let i = 0; i < n && i0 + i < L.length; i++) {
    const t = i / SR;
    const f = freq * (1 + slide * t);
    const dt1 = f / SR, dt2 = f * (1 + detune) / SR;
    ph1 += dt1; ph2 += dt2;
    let v = osc(kind, ph1, dt1);
    if (detune) v = 0.5 * (v + osc(kind, ph2, dt2));
    y += k * (v - y);
    const e = env(t, a, d, s, len, r) * vol;
    L[i0 + i] += y * e * gl;
    R[i0 + i] += y * e * gr;
  }
}
function kick(L, R, start, vol = 0.9) {
  const i0 = Math.floor(start * SR), n = Math.floor(0.35 * SR);
  let ph = 0;
  for (let i = 0; i < n && i0 + i < L.length; i++) {
    const t = i / SR;
    ph += (45 + 110 * Math.exp(-t * 28)) / SR;
    const v = Math.sin(2 * Math.PI * ph) * Math.exp(-t * 9) * vol;
    L[i0 + i] += v; R[i0 + i] += v;
  }
}
function snare(L, R, start, vol = 0.35) {
  const i0 = Math.floor(start * SR), n = Math.floor(0.22 * SR);
  let y = 0;
  for (let i = 0; i < n && i0 + i < L.length; i++) {
    const t = i / SR;
    const nz = noise();
    y += 0.5 * (nz - y);
    const v = ((nz - y) * 0.8 * Math.exp(-t * 18) + Math.sin(2 * Math.PI * 190 * t) * 0.5 * Math.exp(-t * 30)) * vol;
    L[i0 + i] += v; R[i0 + i] += v;
  }
}
function hat(L, R, start, vol = 0.12, open = false) {
  const i0 = Math.floor(start * SR), n = Math.floor((open ? 0.18 : 0.05) * SR);
  let y = 0;
  for (let i = 0; i < n && i0 + i < L.length; i++) {
    const t = i / SR;
    const nz = noise();
    y += 0.7 * (nz - y);
    const v = (nz - y) * Math.exp(-t * (open ? 16 : 70)) * vol;
    L[i0 + i] += v * 0.8; R[i0 + i] += v;
  }
}
// a simple stereo feedback delay as "space"
function delay(L, R, time, fb, wet) {
  const dn = Math.floor(time * SR);
  for (let i = dn; i < L.length; i++) {
    const l = L[i - dn], r = R[i - dn];
    L[i] += r * fb * wet; R[i] += l * fb * wet;
  }
}
function normalize(L, R, peak) {
  let m = 0;
  for (let i = 0; i < L.length; i++) m = Math.max(m, Math.abs(L[i]), Math.abs(R[i]));
  const g = peak / m;
  for (let i = 0; i < L.length; i++) { L[i] *= g; R[i] *= g; }
}
// make the loop seamless: fold the tail past the loop length onto the start
function foldTail(L, R, loopLen) {
  const n = Math.floor(loopLen * SR);
  for (let i = n; i < L.length; i++) { L[i - n] += L[i]; R[i - n] += R[i]; }
  return [L.slice(0, n), R.slice(0, n)];
}

// ── level music: upbeat, 128 BPM, 16 bars in C major ─────────────────
function levelMusic() {
  const bpm = 128, beat = 60 / bpm, bars = 16, len = bars * 4 * beat;
  const L = new Float32Array(Math.ceil((len + 2) * SR)), R = new Float32Array(L.length);
  const prog = [[48, [60, 64, 67]], [43, [59, 62, 67]], [45, [60, 64, 69]], [41, [60, 65, 69]]]; // C G Am F
  const lead = [ // (beat offset in bar, midi, beats)
    [[0, 72, 1], [1, 74, 0.5], [1.5, 76, 1], [2.5, 79, 0.5], [3, 76, 1]],
    [[0, 74, 1.5], [1.5, 71, 0.5], [2, 74, 1], [3, 79, 1]],
    [[0, 76, 1], [1, 72, 0.5], [1.5, 76, 0.5], [2, 81, 1.5], [3.5, 79, 0.5]],
    [[0, 77, 1], [1, 76, 1], [2, 74, 1], [3, 72, 1]],
  ];
  const lead2 = [
    [[0, 79, 0.5], [0.5, 76, 0.5], [1, 79, 0.5], [1.5, 84, 1.5], [3, 83, 1]],
    [[0, 79, 1], [1, 74, 0.5], [1.5, 79, 0.5], [2, 83, 1], [3, 81, 1]],
    [[0, 81, 0.5], [0.5, 79, 0.5], [1, 76, 1], [2, 72, 1], [3, 76, 1]],
    [[0, 77, 0.5], [0.5, 79, 0.5], [1, 81, 1], [2, 79, 1.5], [3.5, 74, 0.5]],
  ];
  for (let bar = 0; bar < bars; bar++) {
    const t0 = bar * 4 * beat;
    const [root, chord] = prog[bar % 4];
    // bass: driving eighths, root / octave
    for (let e = 0; e < 8; e++) {
      const m = root - 12 + (e % 4 === 3 ? 7 : e % 2 ? 12 : 0);
      mixNote(L, R, t0 + e * beat / 2, { freq: midi(m), len: beat * 0.42, kind: 'saw', vol: 0.22, lp: 900, d: 0.08, s: 0.5 });
    }
    // chord stabs on the offbeats
    for (let b = 0; b < 4; b++)
      for (const [i, c] of chord.entries())
        mixNote(L, R, t0 + (b + 0.5) * beat, { freq: midi(c), len: beat * 0.3, kind: 'saw', detune: 0.006, vol: 0.06, pan: (i - 1) * 0.5, lp: 2600, d: 0.12, s: 0.3 });
    // pad underneath
    for (const [i, c] of chord.entries())
      mixNote(L, R, t0, { freq: midi(c - 12), len: 4 * beat - 0.05, kind: 'tri', vol: 0.05, pan: (i - 1) * 0.7, a: 0.3, d: 0.5, s: 0.8, r: 0.3 });
    // lead from bar 4 (second phrase uses the brighter melody)
    if (bar >= 4) {
      const ph = (bar >= 12 ? lead2 : lead)[bar % 4];
      for (const [o, m, b] of ph)
        mixNote(L, R, t0 + o * beat, { freq: midi(m), len: b * beat * 0.9, kind: 'sq', vol: 0.09, pan: 0.15, lp: 3500, d: 0.15, s: 0.55, r: 0.1 });
    }
    // drums
    for (let b = 0; b < 4; b++) {
      kick(L, R, t0 + b * beat, 0.75);
      if (b % 2 === 1) snare(L, R, t0 + b * beat, 0.32);
      hat(L, R, t0 + (b + 0.5) * beat, 0.1, bar % 2 === 1 && b === 3);
      hat(L, R, t0 + b * beat, 0.05);
    }
  }
  delay(L, R, beat * 0.75, 0.35, 0.5);
  const [l, r] = foldTail(L, R, len);
  normalize(l, r, 0.7);
  return [l, r];
}

// ── title music: calm arpeggios at 96 BPM ─────────────────────────────
function titleMusic() {
  const bpm = 96, beat = 60 / bpm, bars = 8, len = bars * 4 * beat;
  const L = new Float32Array(Math.ceil((len + 3) * SR)), R = new Float32Array(L.length);
  const prog = [[45, [57, 60, 64, 69]], [41, [57, 60, 65, 69]], [48, [55, 60, 64, 67]], [43, [55, 59, 62, 67]]]; // Am F C G
  for (let bar = 0; bar < bars; bar++) {
    const t0 = bar * 4 * beat;
    const [root, chord] = prog[bar % 4];
    mixNote(L, R, t0, { freq: midi(root - 12), len: 4 * beat - 0.1, kind: 'tri', vol: 0.3, a: 0.02, d: 0.6, s: 0.6, r: 0.2 });
    for (let s = 0; s < 16; s++) {
      const m = chord[[0, 1, 2, 3, 2, 1, 2, 3][s % 8]] + (s >= 8 ? 12 : 0);
      mixNote(L, R, t0 + s * beat / 4, { freq: midi(m), len: beat * 0.2, kind: 'sq', vol: 0.05, pan: s % 2 ? 0.4 : -0.4, lp: 2200, d: 0.1, s: 0.3, r: 0.15 });
    }
    for (const [i, c] of chord.entries())
      mixNote(L, R, t0, { freq: midi(c), len: 4 * beat - 0.1, kind: 'saw', detune: 0.004, vol: 0.025, pan: (i - 1.5) * 0.4, lp: 1200, a: 0.8, d: 1, s: 0.8, r: 0.6 });
    if (bar >= 4) for (let b = 0; b < 4; b++) { kick(L, R, t0 + b * beat, b % 2 ? 0 : 0.5); hat(L, R, t0 + (b + 0.5) * beat, 0.06); }
  }
  delay(L, R, beat * 0.5, 0.45, 0.6);
  const [l, r] = foldTail(L, R, len);
  normalize(l, r, 0.6);
  return [l, r];
}

// ── sound effects ───────────────────────────────────────────────────
function sfx(seconds, fn) {
  const L = new Float32Array(Math.ceil(seconds * SR));
  for (let i = 0; i < L.length; i++) L[i] = fn(i / SR, i);
  return L;
}
const sweep = (f0, f1, dur, kind = 'sq', vol = 0.5, decay = 6) => {
  let ph = 0;
  return sfx(dur, (t) => {
    const f = f0 * Math.pow(f1 / f0, Math.min(1, t / dur));
    ph += f / SR;
    return osc(kind, ph, f / SR) * vol * Math.exp(-t * decay) * Math.min(1, t * 400);
  });
};
function seq(notes, kind, vol, noteLen, decay) {
  const total = notes.length * noteLen + 0.4;
  const L = new Float32Array(Math.ceil(total * SR)), R = new Float32Array(L.length);
  notes.forEach((m, i) => mixNote(L, R, i * noteLen, { freq: midi(m), len: noteLen * 0.9, kind, vol, d: decay, s: 0.3, r: 0.2 }));
  return L.map((v, i) => (v + R[i]) * 0.7);
}

const effects = {
  jump: sweep(300, 720, 0.18, 'sq', 0.35, 10),
  double_jump: sweep(500, 1300, 0.2, 'sq', 0.3, 9),
  land: (() => { let y = 0; return sfx(0.12, (t) => { y += 0.08 * (noise() - y); return y * 6 * Math.exp(-t * 40) + Math.sin(2 * Math.PI * 80 * t) * 0.4 * Math.exp(-t * 30); }); })(),
  dash: (() => { let y = 0; return sfx(0.3, (t) => { const k = 0.05 + 0.4 * Math.min(1, t / 0.15); y += k * (noise() - y); return y * 0.9 * Math.sin(Math.PI * Math.min(1, t / 0.3)); }); })(),
  coin: seq([88, 95], 'sq', 0.25, 0.07, 0.2),
  gem: seq([84, 88, 91, 96, 100], 'tri', 0.45, 0.05, 0.25),
  hurt: sweep(420, 90, 0.45, 'saw', 0.4, 4),
  checkpoint: seq([72, 76, 79, 84], 'tri', 0.5, 0.09, 0.4),
  goal: seq([72, 76, 79, 84, 79, 84, 88], 'sq', 0.2, 0.11, 0.3),
  bump: sweep(160, 90, 0.12, 'tri', 0.6, 25),
  // loopable ambiences for 3D voices (whole cycles at their frequencies)
  whir: (() => { const f = 75; return sfx(1, (t) => 0.35 * (osc('saw', t * f, f / SR) * 0.5 + Math.sin(2 * Math.PI * f * 2 * t) * 0.3) * (0.8 + 0.2 * Math.sin(2 * Math.PI * 4 * t))); })(),
  portal: (() => sfx(2, (t) => 0.3 * (Math.sin(2 * Math.PI * 220 * t) * 0.4 + Math.sin(2 * Math.PI * 330 * t) * 0.3 + Math.sin(2 * Math.PI * 441 * t) * 0.15) * (0.75 + 0.25 * Math.sin(2 * Math.PI * 1 * t))))(),
};
for (const [name, L] of Object.entries(effects)) wav(join(D, `${name}.wav`), L);

// music: WAV in out/, encoded into assets/audio
const enc = (name, [l, r], args, ext) => {
  const w = join(OUT, `${name}.wav`);
  wav(w, l, r);
  const dst = join(D, `${name}.${ext}`);
  rmSync(dst, { force: true });
  const p = spawnSync('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-i', w, ...args, '-map_metadata', '-1', '-fflags', '+bitexact', '-flags:a', '+bitexact', dst], { stdio: 'inherit' });
  if (p.status) throw new Error(`ffmpeg failed for ${name}`);
};
enc('music_level', levelMusic(), ['-c:a', 'libopus', '-b:a', '80k'], 'opus');
enc('music_title', titleMusic(), ['-c:a', 'libvorbis', '-q:a', '4'], 'ogg');
console.log('audio written to', D);
