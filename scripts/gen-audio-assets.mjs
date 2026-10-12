// Generates the audio samples' test sounds: synthesized here (CC0, no
// recorded or commercial audio), written as WAV, then encoded with ffmpeg
// (FLAC, MP3, Ogg Vorbis, Ogg Opus). The tracker module is written byte by
// byte. The outputs are committed; re-run only to change them.
//
//   node scripts/gen-audio-assets.mjs        (needs ffmpeg with libvorbis, libopus, libmp3lame)
import { writeFileSync, mkdirSync, copyFileSync, rmSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const MIX = join(ROOT, 'samples/audio_mix/assets');
const PLAY = join(ROOT, 'samples/audio_play/assets');
const TMP = join(ROOT, 'out/audio-gen');
for (const d of [MIX, PLAY, TMP]) mkdirSync(d, { recursive: true });

let seed = 12345;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296) * 2 - 1;

function wav(path, rate, channels, data /* Float32Array interleaved */) {
  const n = data.length, buf = Buffer.alloc(44 + n * 2);
  buf.write('RIFF', 0); buf.writeUInt32LE(36 + n * 2, 4); buf.write('WAVE', 8);
  buf.write('fmt ', 12); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20); buf.writeUInt16LE(channels, 22);
  buf.writeUInt32LE(rate, 24); buf.writeUInt32LE(rate * channels * 2, 28); buf.writeUInt16LE(channels * 2, 32); buf.writeUInt16LE(16, 34);
  buf.write('data', 36); buf.writeUInt32LE(n * 2, 40);
  for (let i = 0; i < n; i++) buf.writeInt16LE(Math.max(-32767, Math.min(32767, Math.round(data[i] * 32767))), 44 + i * 2);
  writeFileSync(path, buf);
}

function synth(rate, seconds, channels, fn) {
  const frames = Math.round(rate * seconds), out = new Float32Array(frames * channels);
  for (let i = 0; i < frames; i++) for (let c = 0; c < channels; c++) out[i * channels + c] = fn(i / rate, c, i);
  return out;
}

function ffmpeg(input, output, args) {
  const r = spawnSync('ffmpeg', ['-y', '-loglevel', 'error', '-i', input, ...args, '-map_metadata', '-1', '-fflags', '+bitexact', output], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`ffmpeg ${output}: ${r.stderr}`);
}

const TAU = Math.PI * 2;
// kick: 48 kHz mono WAV, a falling sine with a fast decay
// (phase is the integral of 50 + 120 e^(-30 t) Hz)
wav(join(MIX, 'kick.wav'), 48000, 1, synth(48000, 0.3, 1, (t) =>
  0.9 * Math.sin(TAU * (50 * t + (120 / 30) * (1 - Math.exp(-t * 30)))) * Math.exp(-t * 9)));
// hat: 44.1 kHz mono WAV (exercises load-time resampling), noise burst
wav(join(MIX, 'hat.wav'), 44100, 1, synth(44100, 0.12, 1, (t) => 0.5 * rnd() * Math.exp(-t * 40)));
// engine: 48 kHz mono WAV, a seamless loop (whole cycles) for the 3D voice
wav(join(MIX, 'engine.wav'), 48000, 1, synth(48000, 0.5, 1, (t) => {
  const saw = (x) => 2 * (x - Math.floor(x + 0.5));
  return 0.35 * saw(110 * t) + 0.2 * Math.sin(TAU * 220 * t) + 0.1 * saw(55 * t);
}));
// blip: FLAC from a 48 kHz mono tone
wav(join(TMP, 'blip.wav'), 48000, 1, synth(48000, 0.25, 1, (t) => 0.5 * Math.sign(Math.sin(TAU * 880 * t)) * Math.exp(-t * 10) * 0.6));
ffmpeg(join(TMP, 'blip.wav'), join(MIX, 'blip.flac'), ['-c:a', 'flac']);
// zap: MP3 from a 44.1 kHz mono sweep
wav(join(TMP, 'zap.wav'), 44100, 1, synth(44100, 0.4, 1, (t) => 0.5 * Math.sin(TAU * (1200 * t - 1200 * t * t)) * Math.exp(-t * 5)));
ffmpeg(join(TMP, 'zap.wav'), join(MIX, 'zap.mp3'), ['-c:a', 'libmp3lame', '-b:a', '96k']);
// music: Ogg Vorbis, 44.1 kHz stereo, 4 s arpeggio loop
const notes = [261.63, 329.63, 392.0, 523.25, 392.0, 329.63, 293.66, 349.23];
wav(join(TMP, 'music.wav'), 44100, 2, synth(44100, 4, 2, (t, c) => {
  const step = Math.floor(t * 4) % notes.length, lt = t - Math.floor(t * 4) / 4;
  const f = notes[step] * (c ? 1.003 : 1);
  const lead = 0.25 * Math.sin(TAU * f * t) * Math.exp(-lt * 6);
  const bass = 0.2 * Math.sin(TAU * (notes[Math.floor(t) % 4 * 2] / 2) * t);
  return lead + bass;
}));
ffmpeg(join(TMP, 'music.wav'), join(MIX, 'music.ogg'), ['-c:a', 'libvorbis', '-q:a', '3']);
// ambience: Ogg Opus, 48 kHz stereo, 3 s of filtered noise and a slow pad
let lpL = 0, lpR = 0;
wav(join(TMP, 'ambience.wav'), 48000, 2, synth(48000, 3, 2, (t, c) => {
  const n = rnd();
  if (c) lpR += 0.02 * (n - lpR); else lpL += 0.02 * (n - lpL);
  return 0.6 * (c ? lpR : lpL) + 0.12 * Math.sin(TAU * (c ? 196 : 196.5) * t) * (0.5 + 0.5 * Math.sin(TAU * 0.5 * t));
}));
ffmpeg(join(TMP, 'ambience.wav'), join(MIX, 'ambience.opus'), ['-c:a', 'libopus', '-b:a', '64k']);

// tune.mod: a 4-channel ProTracker module, two looped 8-bit samples
function mod() {
  const sq = Buffer.alloc(32), sine = Buffer.alloc(64);
  for (let i = 0; i < 32; i++) sq.writeInt8(i < 16 ? 60 : -60, i);
  for (let i = 0; i < 64; i++) sine.writeInt8(Math.round(100 * Math.sin((TAU * i) / 64)), i);
  const samples = [sq, sine];
  const head = Buffer.alloc(20 + 31 * 30 + 2 + 128 + 4);
  head.write('cartwheel test', 0);
  samples.forEach((s, k) => {
    const o = 20 + k * 30;
    head.write(k ? 'sine' : 'square', o);
    head.writeUInt16BE(s.length / 2, o + 22); head.writeUInt8(0, o + 24); head.writeUInt8(48, o + 25);
    head.writeUInt16BE(0, o + 26); head.writeUInt16BE(s.length / 2, o + 28);
  });
  for (let k = 2; k < 31; k++) head.writeUInt16BE(1, 20 + k * 30 + 28);
  let o = 20 + 31 * 30;
  head.writeUInt8(2, o); head.writeUInt8(127, o + 1); // song length 2: patterns 0, 1
  head.writeUInt8(0, o + 2); head.writeUInt8(1, o + 3);
  head.write('M.K.', o + 130);
  const P = { C2: 428, D2: 381, E2: 340, G2: 285, A2: 254, C3: 214, C1: 856, G1: 570, A1: 508, F1: 640 };
  const cell = (period, smp, eff = 0, prm = 0) => [(smp & 0xf0) | (period >> 8), period & 0xff, ((smp & 0x0f) << 4) | eff, prm];
  const pat = (lead, bass) => {
    const b = Buffer.alloc(64 * 16);
    for (let r = 0; r < 64; r++) {
      const put = (ch, c) => c.forEach((v, i) => b.writeUInt8(v, r * 16 + ch * 4 + i));
      if (r === 0) put(3, cell(0, 0, 0xf, 3)); // speed 3
      if (r % 4 === 0) put(0, cell(P[lead[(r / 4) % lead.length]], 1));
      if (r % 16 === 0) put(1, cell(P[bass[(r / 16) % bass.length]], 2));
    }
    return b;
  };
  return Buffer.concat([head, pat(['C2', 'E2', 'G2', 'C3'], ['C1', 'G1', 'A1', 'F1']), pat(['A2', 'G2', 'E2', 'D2'], ['F1', 'G1', 'C1', 'C1']), ...samples]);
}
writeFileSync(join(MIX, 'tune.mod'), mod());

// the device sample streams the Vorbis music and plays the blip
copyFileSync(join(MIX, 'music.ogg'), join(PLAY, 'music.ogg'));
copyFileSync(join(MIX, 'blip.flac'), join(PLAY, 'blip.flac'));
rmSync(TMP, { recursive: true, force: true });
console.log('ok');
