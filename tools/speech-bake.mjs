#!/usr/bin/env node
// Voices a game's lines ahead of time and packs them into one speech pack (cartwheel/speech.h, a .cwspeech asset).
// The voices are Kokoro-82M v1.0 (hexgrad, Apache-2.0) in its ONNX build (onnx-community/Kokoro-82M-v1.0-ONNX,
// Apache-2.0), run from node by kokoro-js (Apache-2.0) on the CPU. No Python. A dev tool: nothing of the model ships,
// only the audio it speaks.
//
//   npm install --prefix <dir> kokoro-js@1.2.1             (once; the weights download on the first run)
//   node tools/speech-bake.mjs out/lines.json -o assets/main.cwspeech --kokoro <dir>/node_modules [--cache <dir>] [--asr]
//
// lines.json is what tools/story-compile.mjs --lines writes: [{key, who, voice, speed, text}], or a game's own list in
// the same shape (key from story-compile.mjs's lineKey(who, text)). Takes are cached by voice, speed and text in
// --cache (default out/speech-cache under the working directory), so a rerun speaks only what changed. --tone voices
// every line with a synthetic stand-in instead of the model (a buzz shaped by the text's syllables), for tests and for
// a build machine without the model; the pack says which it holds. --asr transcribes each take back with Whisper
// base.en (onnx-community/whisper-base.en, MIT; a check only) and reports the words that differ.
//
// The pack: "CWSP", u32 version (1), u32 count, u32 envelope rate (Hz), u32 flags (1: tone stand-ins), then count
// entries sorted by key of (u32 key, u32 sample rate, u32 frames, u32 audio offset, u32 audio bytes, u32 envelope
// offset, u32 envelope frames), then the data. Audio is IMA ADPCM (4 bits a sample, mono, low nibble first, the
// predictor starting at 0 and step index 0). The envelope is two bytes a frame at the envelope rate: how open the mouth
// is (0..255, from the loudness, eased) and its shape (0 round .. 255 wide, from how bright the sound is). The game
// reads both by time (cw_speech_mouth), so lips move the same on every host whether or not audio plays.
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';

export const ENV_RATE = 100;
const STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
  157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
  1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767];
const IDX = [-1, -1, -1, -1, 2, 4, 6, 8];

/** IMA ADPCM, the decoder's own prediction tracked so errors never accumulate */
export function adpcmEncode(f32) {
  const out = Buffer.alloc((f32.length + 1) >> 1);
  let pred = 0, idx = 0;
  for (let i = 0; i < f32.length; i++) {
    const v = Math.max(-32768, Math.min(32767, Math.round(f32[i] * 32767)));
    const step = STEP[idx];
    let diff = v - pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    let d = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; d += step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; d += step >> 1; }
    if (diff >= step >> 2) { code |= 1; d += step >> 2; }
    pred += code & 8 ? -d : d;
    pred = Math.max(-32768, Math.min(32767, pred));
    idx = Math.max(0, Math.min(88, idx + IDX[code & 7]));
    out[i >> 1] |= i & 1 ? code << 4 : code;
  }
  return out;
}
export function adpcmDecode(buf, n) {
  const o = new Float32Array(n); let pred = 0, idx = 0;
  for (let i = 0; i < n; i++) {
    const code = i & 1 ? buf[i >> 1] >> 4 : buf[i >> 1] & 15, step = STEP[idx];
    let diff = step >> 3;
    if (code & 4) diff += step; if (code & 2) diff += step >> 1; if (code & 1) diff += step >> 2;
    pred += code & 8 ? -diff : diff; pred = Math.max(-32768, Math.min(32767, pred));
    idx = Math.max(0, Math.min(88, idx + IDX[code & 7]));
    o[i] = pred / 32768;
  }
  return o;
}

/** the mouth from the sound: per 1/ENV_RATE s, openness from the loudness against the line's loud parts (eased open
 * fast, closed a little slower, as a jaw moves), shape from the zero-crossing rate (bright sounds spread the lips) */
export function envelope(a, sr) {
  const hop = Math.round(sr / ENV_RATE), n = Math.ceil(a.length / hop);
  const rms = new Float32Array(n), zcr = new Float32Array(n);
  for (let f = 0; f < n; f++) {
    let e = 0, z = 0, c = 0, last = 0;
    for (let i = f * hop; i < Math.min(a.length, (f + 1) * hop); i++) { e += a[i] * a[i]; c++; const s = a[i] > 0 ? 1 : 0; if (s !== last) z++; last = s; }
    rms[f] = c ? Math.sqrt(e / c) : 0; zcr[f] = c ? z / c : 0;
  }
  const sorted = Array.from(rms).sort((x, y) => x - y), loud = sorted[Math.floor(sorted.length * 0.9)] || 1e-4;
  const out = Buffer.alloc(n * 2);
  let open = 0, shape = 0.5;
  for (let f = 0; f < n; f++) {
    const t = Math.min(1, Math.max(0, (rms[f] / loud - 0.08) / 0.92));
    const target = Math.pow(t, 0.8);
    open += (target - open) * (target > open ? 0.6 : 0.35);
    if (rms[f] > loud * 0.1) shape += (Math.min(1, Math.max(0, (zcr[f] - 0.03) * 7)) - shape) * 0.3;
    out[f * 2] = Math.round(Math.min(1, open) * 255); out[f * 2 + 1] = Math.round(shape * 255);
  }
  return out;
}

function trim(a, sr) {
  let s = 0, e = a.length - 1;
  while (s < a.length && Math.abs(a[s]) < 0.01) s++;
  while (e > s && Math.abs(a[e]) < 0.01) e--;
  if (s >= e) return a;
  return a.slice(Math.max(0, s - Math.round(0.03 * sr)), Math.min(a.length, e + Math.round(0.12 * sr)));
}
function normalise(a) {
  let pk = 1e-4; for (const x of a) pk = Math.max(pk, Math.abs(x));
  const g = Math.min(4, 0.7 / pk); const o = new Float32Array(a.length);
  for (let i = 0; i < a.length; i++) o[i] = a[i] * g;
  return o;
}
/** a stand-in voice: a buzz with two formants, a syllable envelope from the text's vowels, about 13 characters a
 * second (the pace Kokoro speaks at); deterministic */
export function tone(text, speed = 1, sr = 24000) {
  const secs = Math.max(0.6, text.length / (13 * speed)), n = Math.round(secs * sr), a = new Float32Array(n);
  const syl = Math.max(1, (text.match(/[aeiouy]+/gi) || []).length), f0 = 120 + (text.length % 7) * 12;
  let ph = 0;
  for (let i = 0; i < n; i++) {
    const t = i / sr, k = t / secs, s = Math.sin(Math.PI * ((k * syl) % 1));
    ph += (2 * Math.PI * f0 * (1 + 0.05 * Math.sin(2 * Math.PI * 3 * t))) / sr;
    const buzz = Math.sin(ph) + 0.5 * Math.sin(ph * 3.1) + 0.25 * Math.sin(ph * 7.3);
    a[i] = 0.35 * buzz * s * s * Math.min(1, k * 20, (1 - k) * 20);
  }
  return a;
}

export function pack(entries, flags = 0) {
  entries.sort((x, y) => x.key - y.key);
  for (let i = 1; i < entries.length; i++) if (entries[i].key === entries[i - 1].key) throw new Error(`two lines share key ${entries[i].key}`);
  const head = Buffer.alloc(20 + entries.length * 28);
  head.write('CWSP', 0); head.writeUInt32LE(1, 4); head.writeUInt32LE(entries.length, 8); head.writeUInt32LE(ENV_RATE, 12); head.writeUInt32LE(flags, 16);
  let off = 0; const data = [];
  entries.forEach((e, i) => {
    const o = 20 + i * 28;
    head.writeUInt32LE(e.key, o); head.writeUInt32LE(e.rate, o + 4); head.writeUInt32LE(e.frames, o + 8);
    head.writeUInt32LE(off, o + 12); head.writeUInt32LE(e.adpcm.length, o + 16); data.push(e.adpcm); off += e.adpcm.length;
    head.writeUInt32LE(off, o + 20); head.writeUInt32LE(e.env.length / 2, o + 24); data.push(e.env); off += e.env.length;
  });
  return Buffer.concat([head, ...data]);
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const a = process.argv.slice(2);
  const arg = (k) => { const i = a.indexOf(k); return i >= 0 ? a[i + 1] : null; };
  const input = a[0];
  const out = arg('-o');
  if (!input || !out) { console.error('usage: speech-bake.mjs <lines.json> -o <out.cwspeech> [--kokoro <node_modules>] [--cache <dir>] [--tone] [--asr]'); process.exit(2); }
  const lines = JSON.parse(readFileSync(input, 'utf8'));
  const useTone = a.includes('--tone');
  const cache = resolve(arg('--cache') || join(process.cwd(), 'out', 'speech-cache'));
  mkdirSync(cache, { recursive: true });
  const mods = arg('--kokoro') || process.env.KOKORO_JS_MODULES;
  let tts = null;
  const SR = 24000;
  const entries = [];
  let spoken = 0, total = 0;
  for (const [n, L] of lines.entries()) {
    if (!L.voice && !useTone) throw new Error(`${L.who}: no voice= on the cast line (needed for "${L.text}")`);
    const tag = createHash('sha1').update(`${useTone ? 'tone' : 'kokoro-v1.0'}|${L.voice}|${L.speed}|${L.text}`).digest('hex').slice(0, 24);
    const f = join(cache, tag + '.f32');
    let pcm;
    if (existsSync(f)) pcm = new Float32Array(readFileSync(f).buffer.slice(0));
    else {
      if (useTone) pcm = tone(L.text, L.speed || 1, SR);
      else {
        if (!tts) {
          if (!mods || !existsSync(join(mods, 'kokoro-js'))) throw new Error('kokoro-js not found: npm install --prefix <dir> kokoro-js@1.2.1, then --kokoro <dir>/node_modules');
          const K = await import(pathToFileURL(join(mods, 'kokoro-js', 'dist', 'kokoro.js')).href);
          tts = await K.KokoroTTS.from_pretrained('onnx-community/Kokoro-82M-v1.0-ONNX', { dtype: 'fp32', device: 'cpu' });
        }
        const r = await tts.generate(L.text, { voice: L.voice, speed: L.speed || 1 });
        if (r.sampling_rate !== SR) throw new Error(`Kokoro spoke at ${r.sampling_rate} Hz, expected ${SR}`);
        pcm = normalise(trim(Float32Array.from(r.audio), SR));
      }
      writeFileSync(f, Buffer.from(pcm.buffer));
      spoken++;
      console.log(`${n + 1}/${lines.length} ${L.who}: ${L.text.slice(0, 70)}`);
    }
    total += pcm.length / SR;
    entries.push({ key: L.key >>> 0, rate: SR, frames: pcm.length, adpcm: adpcmEncode(pcm), env: envelope(pcm, SR), pcm, L });
  }
  const bin = pack(entries, useTone ? 1 : 0);
  mkdirSync(dirname(resolve(out)), { recursive: true });
  writeFileSync(out, bin);
  console.log(`${entries.length} lines (${spoken} spoken now), ${total.toFixed(1)} s of speech, ${(bin.length / 1024).toFixed(0)} KiB${useTone ? ', tone stand-ins' : ''}`);
  if (a.includes('--asr')) {
    const T = await import(pathToFileURL(join(mods, '@huggingface', 'transformers', 'dist', 'transformers.node.mjs')).href);
    const asr = await T.pipeline('automatic-speech-recognition', 'onnx-community/whisper-base.en', { dtype: 'fp32', device: 'cpu' });
    const words = (s) => s.toLowerCase().replace(/[^a-z0-9' ]+/g, ' ').split(/\s+/).filter(Boolean);
    let bad = 0;
    for (const e of entries) {
      const pcm16k = new Float32Array(Math.floor(e.frames * 16000 / SR));
      const dec = adpcmDecode(e.adpcm, e.frames);
      for (let i = 0; i < pcm16k.length; i++) pcm16k[i] = dec[Math.floor(i * SR / 16000)];
      const heard = (await asr(pcm16k)).text.trim();
      const w = words(e.L.text), h = words(heard);
      const miss = w.filter((x) => !h.includes(x));
      if (miss.length > Math.max(1, w.length * 0.2)) { bad++; console.log(`ASR MISS ${e.L.who}: "${e.L.text}" heard "${heard}"`); }
    }
    console.log(`asr: ${entries.length - bad}/${entries.length} lines heard as written`);
    if (bad) process.exitCode = 1;
  }
}
