#!/usr/bin/env node
// Makes samples/video_cutscene/assets/clip.mpg: a 4 second, 320x180, 30 fps
// MPEG-1 clip with MP2 audio, drawn and synthesized here (no third-party
// footage). Needs ffmpeg (mpeg1video + mp2 encoders) at tool time only; the
// engine decodes with pl_mpeg.
//
//   node samples/video_cutscene/make-clip.mjs
//
// The picture: a sky gradient that shifts through the clip, a sun crossing
// it, a ball bouncing on the ground, the frame number in big segment digits
// (so a frame picked from the wrong time is easy to see) and a colour bar
// (red, green, blue, white) that checks the YUV to RGB conversion. The
// sound: one short note per 1/3 second climbing a scale, and a click on
// every whole second.
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const DIR = path.dirname(new URL(import.meta.url).pathname);
const OUT = path.join(DIR, 'assets/clip.mpg');
const W = 320, H = 180, FPS = 30, SECONDS = 4, FRAMES = FPS * SECONDS, RATE = 44100;

const px = new Uint8Array(W * H * 3 * FRAMES);
const put = (f, x, y, r, g, b) => {
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  const o = ((f * H + y) * W + x) * 3;
  px[o] = r; px[o + 1] = g; px[o + 2] = b;
};
const rect = (f, x0, y0, w, h, c) => { for (let y = y0; y < y0 + h; y++) for (let x = x0; x < x0 + w; x++) put(f, x, y, ...c); };
// seven-segment digits: segments a b c d e f g
const SEG = ['1111110', '0110000', '1101101', '1111001', '0110011', '1011011', '1011111', '1110000', '1111111', '1111011'];
const digit = (f, x, y, d, c) => {
  const s = SEG[d], t = 4, l = 18;
  if (s[0] === '1') rect(f, x + t, y, l, t, c);
  if (s[1] === '1') rect(f, x + t + l, y + t, t, l, c);
  if (s[2] === '1') rect(f, x + t + l, y + 2 * t + l, t, l, c);
  if (s[3] === '1') rect(f, x + t, y + 2 * (t + l), l, t, c);
  if (s[4] === '1') rect(f, x, y + 2 * t + l, t, l, c);
  if (s[5] === '1') rect(f, x, y + t, t, l, c);
  if (s[6] === '1') rect(f, x + t, y + t + l, l, t, c);
};

for (let f = 0; f < FRAMES; f++) {
  const k = f / (FRAMES - 1);
  for (let y = 0; y < H; y++) {
    const v = y / H;
    const top = [40 + 120 * k, 80 + 60 * (1 - k), 170 - 90 * k], bot = [230, 170 + 40 * k, 120];
    for (let x = 0; x < W; x++) put(f, x, y, ...top.map((t, i) => Math.round(t + (bot[i] - t) * v)));
  }
  // sun
  const sx = 20 + 280 * k, sy = 60 - 30 * Math.sin(Math.PI * k);
  for (let y = -14; y <= 14; y++) for (let x = -14; x <= 14; x++) if (x * x + y * y <= 196) put(f, Math.round(sx + x), Math.round(sy + y), 255, 230, 90);
  // ground and a bouncing ball
  rect(f, 0, 150, W, 30, [60, 120, 50]);
  const bx = 40 + 6 * f % 240, by = 140 - Math.round(60 * Math.abs(Math.sin(f * Math.PI / 20)));
  for (let y = -10; y <= 10; y++) for (let x = -10; x <= 10; x++) if (x * x + y * y <= 100) put(f, bx + x, by + y, 220, 40, 60);
  // frame number
  const s = String(f).padStart(3, '0');
  for (let i = 0; i < 3; i++) digit(f, 220 + i * 30, 100, Number(s[i]), [255, 255, 255]);
  // colour bars
  [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 255]].forEach((c, i) => rect(f, 8 + i * 24, 8, 24, 16, c));
}

const pcm = new Int16Array(RATE * SECONDS * 2);
const scale = [262, 294, 330, 349, 392, 440, 494, 523];
for (let i = 0; i < RATE * SECONDS; i++) {
  const t = i / RATE, n = Math.floor(t * 3), tn = t - n / 3;
  let v = 0.3 * Math.sin(2 * Math.PI * scale[n % 8] * t) * Math.exp(-tn * 8);
  const ts = t - Math.floor(t);
  if (ts < 0.004) v += 0.5 * Math.sin(2 * Math.PI * 2000 * ts);
  const s = Math.round(Math.max(-1, Math.min(1, v)) * 32767);
  pcm[2 * i] = s;
  pcm[2 * i + 1] = s;
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'cw-clip-'));
const vid = path.join(tmp, 'v.rgb'), aud = path.join(tmp, 'a.pcm');
fs.writeFileSync(vid, px);
fs.writeFileSync(aud, Buffer.from(pcm.buffer));
fs.mkdirSync(path.dirname(OUT), { recursive: true });
const r = spawnSync('ffmpeg', ['-y', '-hide_banner', '-loglevel', 'error',
  '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', `${W}x${H}`, '-r', String(FPS), '-i', vid,
  '-f', 's16le', '-ar', String(RATE), '-ac', '2', '-i', aud,
  '-c:v', 'mpeg1video', '-q:v', '4', '-g', '15', '-bf', '2',
  '-c:a', 'mp2', '-b:a', '128k', '-f', 'mpeg', '-fflags', '+bitexact', '-flags', '+bitexact', OUT], { stdio: 'inherit' });
fs.rmSync(tmp, { recursive: true, force: true });
if (r.status !== 0) process.exit(r.status ?? 1);
console.log(`${path.relative(process.cwd(), OUT)}: ${fs.statSync(OUT).size} bytes, ${FRAMES} frames`);
