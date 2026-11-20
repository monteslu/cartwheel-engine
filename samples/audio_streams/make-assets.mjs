// The audio_streams sample's sounds, made here so they are small and exactly known (16-bit mono WAV, 48 kHz):
//   step1..3.wav   three short footstep-like tones (a randomizer's sounds)
//   loop.wav       2 s of a rising tone whose sampler chunk ("smpl") loops frames 24000..71999
//   calm.wav       a 120 bpm clip of 8 beats (4 s): a soft pad, a tick on every beat
//   battle.wav     a 120 bpm clip of 8 beats: a brighter pad, a louder tick on every beat
//   sting.wav      one beat (0.5 s) of a falling tone: the filler between them
// Every sample is computed (no randomness), so the files are the same on every run.
//   node samples/audio_streams/make-assets.mjs
import { writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = join(dirname(fileURLToPath(import.meta.url)), 'assets');
const RATE = 48000;
function wav(name, samples, loop) {
  const n = samples.length, data = Buffer.alloc(n * 2);
  for (let i = 0; i < n; i++) data.writeInt16LE(Math.max(-32767, Math.min(32767, Math.round(samples[i] * 32767))), i * 2);
  const chunks = [];
  const fmt = Buffer.alloc(16);
  fmt.writeUInt16LE(1, 0); fmt.writeUInt16LE(1, 2); fmt.writeUInt32LE(RATE, 4); fmt.writeUInt32LE(RATE * 2, 8);
  fmt.writeUInt16LE(2, 12); fmt.writeUInt16LE(16, 14);
  chunks.push(['fmt ', fmt]);
  if (loop) { // smpl: 36 bytes of header, then one 24-byte loop (start, end inclusive)
    const s = Buffer.alloc(60);
    s.writeUInt32LE(Math.round(1e9 / RATE), 8); s.writeUInt32LE(60, 12); s.writeUInt32LE(1, 28);
    s.writeUInt32LE(0, 36); s.writeUInt32LE(0, 40); s.writeUInt32LE(loop[0], 44); s.writeUInt32LE(loop[1] - 1, 48);
    chunks.push(['smpl', s]);
  }
  chunks.push(['data', data]);
  const body = Buffer.concat(chunks.map(([id, b]) => { const h = Buffer.alloc(8); h.write(id, 0, 'ascii'); h.writeUInt32LE(b.length, 4); return Buffer.concat([h, b]); }));
  const head = Buffer.alloc(12); head.write('RIFF', 0, 'ascii'); head.writeUInt32LE(4 + body.length, 4); head.write('WAVE', 8, 'ascii');
  writeFileSync(join(OUT, name), Buffer.concat([head, body]));
}
const tone = (secs, f) => Array.from({ length: Math.round(secs * RATE) }, (_, i) => f(i / RATE, i));
const env = (t, len) => Math.min(1, t * 200) * Math.max(0, 1 - t / len);
[330, 415, 494].forEach((hz, k) => wav(`step${k + 1}.wav`, tone(0.12, (t) => 0.5 * env(t, 0.12) * Math.sin(2 * Math.PI * hz * t))));
// a rising tone: every frame differs from the one before, so a wrong loop point shows in the samples
wav('loop.wav', tone(2, (t) => 0.4 * Math.sin(2 * Math.PI * (200 * t + 60 * t * t))), [24000, 72000]);
const beat = 0.5;
const tick = (t, amp, hz) => { const b = t % beat; return amp * Math.max(0, 1 - b / 0.05) * Math.sin(2 * Math.PI * hz * b); };
wav('calm.wav', tone(4, (t) => 0.15 * Math.sin(2 * Math.PI * 220 * t) + tick(t, 0.3, 1000)));
wav('battle.wav', tone(4, (t) => 0.15 * Math.sign(Math.sin(2 * Math.PI * 165 * t)) * 0.6 + tick(t, 0.5, 1500)));
wav('sting.wav', tone(beat, (t) => 0.4 * Math.sin(2 * Math.PI * (880 - 600 * t) * t)));
console.log('audio_streams assets written');
