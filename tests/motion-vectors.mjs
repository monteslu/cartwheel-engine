// Object motion vectors (src/render/velocity.c): TAA on fast-moving things, measured against a supersampled
// picture of the same frame.
//
// Cases (native, per backend): post_stack's motion_taa segment (a bar spinning in front of the room at 0.21 rad
// a frame) and the motion_vectors sample's three movers: a turning mesh (its world matrix), a skinned character
// (bone matrices) and an instanced swarm (instance matrices). Runs of one frame:
//   ref   no TAA, rendered at 6 x 6 the size and box-filtered down in linear light: the frame as it should look
//   bare  the same without the movers: where it differs from ref (grown by two pixels) is what a mover covers
//   plain no TAA at the frame's own size
//   on    TAA, the velocity target holding each mover's own motion
//   off   TAA with only the camera's motion in the velocity target (CW_TEST_VELOCITY_OBJECTS=0), as before
//         this pass drew objects: the history under a mover is read from where the camera says it was
// The error is the mean absolute difference from ref, in display levels, over a mover's pixels. With the
// movers' motion it must be at most RATIO of the error with the camera's motion alone. The CONTROL is that
// same check with the "on" run made as "off" (camera motion only in both): it must fail, or the test could
// not see the motion vectors at all. A wrong vector (the sign turned, the previous pose not the previous
// one) sends the history somewhere else on the mover and fails the same way.
// TAA must also be no worse than no TAA on every mover: the "on" error at most the "plain" one. Its CONTROL is
// TAA without its moving-pixel terms (CW_TEST_TAA_STILL_ONLY=1: the jittered frame and a full-weight history on
// movers too, as TAA was before them), which must fail it.
// Still picture: the yard without the movers under TAA (MV_REF=3) must be as sharp as TAA without the moving-pixel
// terms: its error against the 36-sample still picture at most STILL_SLACK over that one's, and its mean gradient at
// least STILL_GRAD of it. Its CONTROL treats every pixel as moving (CW_TEST_TAA_STILL_ONLY=soft: the unjittered
// frame and the low history weight on still pixels too) and must fail. Crops land in out/gate/motion-vectors/.
//
//   node tests/motion-vectors.mjs [--gpus gl,webgpu]
import { spawn } from 'node:child_process';
import { mkdirSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, writePng } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv, wgpuPin } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox();
await takeSuiteLock('motion-vectors.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'motion-vectors');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const gi = argv.indexOf('--gpus');
const GPUS = (gi >= 0 ? argv[gi + 1] : 'gl,webgpu').split(',');
const SS = 6, W = 640, H = 360, RATIO = 0.8, STILL_SLACK = 1.02, STILL_GRAD = 0.98;
// crops (at 640 x 360) bound each mover's sweep; the mask inside them is the mover's own pixels
const CASES = [
  { app: 'post_stack', frame: 496, ref: 'POST_MOTION_REF', name: 'bar', crop: { x: 250, y: 5, w: 216, h: 216 } },
  { app: 'motion_vectors', frame: 60, ref: 'MV_REF', name: 'mesh', crop: { x: 90, y: 50, w: 150, h: 150 } },
  { app: 'motion_vectors', frame: 60, ref: 'MV_REF', name: 'skinned', crop: { x: 270, y: 80, w: 140, h: 190 } },
  { app: 'motion_vectors', frame: 60, ref: 'MV_REF', name: 'instanced', crop: { x: 385, y: 45, w: 185, h: 185 } },
];

const shots = new Map();
function run(c, gpu, tag, extra) {
  const key = `${c.app}-${gpu}-${tag}`;
  if (!shots.has(key)) shots.set(key, run1(c, gpu, key, extra));
  return shots.get(key);
}
function run1(c, gpu, key, extra) {
  const exe = join(ROOT, 'build/native/samples', c.app);
  if (!existsSync(exe)) throw new Error(`no native build of ${c.app} (scripts/build.sh ${c.app})`);
  const shot = join(OUT, `${key}.png`);
  const env = { ...process.env, CW_GPU: gpu, CW_FRAMES: String(c.frame), CW_SCREENSHOT: shot, SDL_AUDIODRIVER: 'dummy',
    ...(gpu === 'webgpu' ? wgpuPin().env : {}), ...extra };
  return new Promise((res, rej) => {
    const p = spawn('timeout', ['-s', 'KILL', '300', exe], { env, stdio: ['ignore', 'ignore', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => (code !== 0 ? rej(new Error(`${key} exited ${code}: ${err.slice(-800)}`)) : res(((i) => ({ width: i.w, height: i.h, data: i.rgba }))(readImage(shot)))));
  });
}
const toLin = new Float64Array(256), enc = (v) => Math.round(255 * (v <= 0.0031308 ? v * 12.92 : 1.055 * v ** (1 / 2.4) - 0.055));
for (let i = 0; i < 256; i++) { const c = i / 255; toLin[i] = c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; }
/** box-filter an image down by n in linear light */
function boxDown(img, n) {
  const w = img.width / n, h = img.height / n, out = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++)
      for (let c = 0; c < 4; c++) {
        let s = 0;
        for (let j = 0; j < n; j++) for (let i = 0; i < n; i++) { const v = img.data[((y * n + j) * img.width + x * n + i) * 4 + c]; s += c === 3 ? v / 255 : toLin[v]; }
        out[(y * w + x) * 4 + c] = c === 3 ? 255 : enc(s / (n * n));
      }
  return { width: w, height: h, data: out };
}
function crop(img, CROP) {
  const out = new Uint8Array(CROP.w * CROP.h * 4);
  for (let y = 0; y < CROP.h; y++) out.set(img.data.subarray(((CROP.y + y) * img.width + CROP.x) * 4, ((CROP.y + y) * img.width + CROP.x + CROP.w) * 4), y * CROP.w * 4);
  return { width: CROP.w, height: CROP.h, data: out };
}
/** mean absolute difference (levels) and the share of pixels more than 16 levels off, over the crop */
function error(a, b, mask) {
  let sum = 0, big = 0, n = 0;
  for (let i = 0; i < a.data.length; i += 4) {
    if (!mask[i >> 2]) continue;
    n++;
    const d = Math.max(Math.abs(a.data[i] - b.data[i]), Math.abs(a.data[i + 1] - b.data[i + 1]), Math.abs(a.data[i + 2] - b.data[i + 2]));
    sum += (Math.abs(a.data[i] - b.data[i]) + Math.abs(a.data[i + 1] - b.data[i + 1]) + Math.abs(a.data[i + 2] - b.data[i + 2])) / 3;
    if (d > 16) big++;
  }
  return { mae: sum / n, gt16: big / n, n };
}

let fail = 0;
for (const gpu of GPUS) for (const c of CASES) {
  const CROP = c.crop, tag = `${gpu} ${c.name}`;
  const big = await run(c, gpu, 'ref-big', { [c.ref]: '1', CW_RENDER_SCALE: String(SS) });
  if (big.width !== W * SS || big.height !== H * SS) throw new Error(`the reference is ${big.width}x${big.height}, not ${W * SS}x${H * SS}`);
  const down = async (k, im) => { const d = shots.get(k) ?? boxDown(await im, SS); shots.set(k, d); return d; };
  const ref = crop(await down(`${c.app}-${gpu}-ref`, big), CROP);
  const bare = crop(await down(`${c.app}-${gpu}-bare`, run(c, gpu, 'ref-bare', { [c.ref]: '2', CW_RENDER_SCALE: String(SS) })), CROP);
  // the bar's pixels: where it changes the reference, grown by two pixels
  const hit = new Uint8Array(CROP.w * CROP.h), mask = new Uint8Array(CROP.w * CROP.h);
  for (let i = 0; i < hit.length; i++) hit[i] = Math.abs(ref.data[i * 4] - bare.data[i * 4]) + Math.abs(ref.data[i * 4 + 1] - bare.data[i * 4 + 1]) + Math.abs(ref.data[i * 4 + 2] - bare.data[i * 4 + 2]) > 6 ? 1 : 0;
  for (let y = 0; y < CROP.h; y++)
    for (let x = 0; x < CROP.w; x++)
      for (let j = -2; j <= 2 && !mask[y * CROP.w + x]; j++)
        for (let i = -2; i <= 2; i++) {
          const xx = x + i, yy = y + j;
          if (xx >= 0 && yy >= 0 && xx < CROP.w && yy < CROP.h && hit[yy * CROP.w + xx]) { mask[y * CROP.w + x] = 1; break; }
        }
  const covered = mask.reduce((a, b) => a + b, 0);
  if (covered < 500) throw new Error(`${tag}: the mover covers ${covered} pixels of the crop: the mask is wrong`);
  const plain = crop(await run(c, gpu, 'plain', { [c.ref]: '1', CW_RENDER_SCALE: '1' }), CROP);
  const on = crop(await run(c, gpu, 'on', { CW_RENDER_SCALE: '1' }), CROP);
  const off = crop(await run(c, gpu, 'off', { CW_TEST_VELOCITY_OBJECTS: '0', CW_RENDER_SCALE: '1' }), CROP);
  const old = crop(await run(c, gpu, 'still-only', { CW_TEST_TAA_STILL_ONLY: '1', CW_RENDER_SCALE: '1' }), CROP);
  for (const [n, im] of [['ref', ref], ['plain', plain], ['on', on], ['off', off], ['still-only', old]]) writePng(join(OUT, `${gpu}-${c.name}-crop-${n}.png`), im.width, im.height, im.data);
  const eOn = error(on, ref, mask), eOff = error(off, ref, mask), ePlain = error(plain, ref, mask), eOld = error(old, ref, mask);
  const f = (e) => `${e.mae.toFixed(3)} levels, ${(e.gt16 * 100).toFixed(2)}% over 16`;
  console.log(`${tag}: error against the ${SS * SS}-sample reference over the mover (${covered} pixels): its own motion ${f(eOn)}; camera motion only ${f(eOff)}; no TAA ${f(ePlain)}`);
  const check = (e) => e.mae <= RATIO * eOff.mae;
  if (!check(eOn)) { console.log(`FAIL ${tag}: with the mover's motion the error is ${eOn.mae.toFixed(3)}, over ${RATIO} of ${eOff.mae.toFixed(3)}`); fail++; }
  if (check(eOff)) { console.log(`FAIL ${tag}: CONTROL PASSED: camera motion only passes the check`); fail++; }
  else console.log(`ok   ${tag}: ${(eOn.mae / eOff.mae).toFixed(2)} of the camera-only error; the control (camera motion only in both runs) fails`);
  if (eOn.mae > ePlain.mae) { console.log(`FAIL ${tag}: TAA ${eOn.mae.toFixed(3)} is over no TAA ${ePlain.mae.toFixed(3)}`); fail++; }
  else console.log(`ok   ${tag}: TAA ${eOn.mae.toFixed(3)} at or under no TAA ${ePlain.mae.toFixed(3)} (${(eOn.mae / ePlain.mae).toFixed(3)})`);
  if (eOld.mae <= ePlain.mae) { console.log(`FAIL ${tag}: CONTROL PASSED: TAA without its moving-pixel terms (${eOld.mae.toFixed(3)}) is at or under no TAA`); fail++; }
  else console.log(`ok   ${tag}: the control (TAA without its moving-pixel terms, ${eOld.mae.toFixed(3)}) is over no TAA`);
}

// the still picture: the yard without movers under TAA, against the 36-sample picture of it
/** mean absolute difference over the whole frame, and the mean gradient (sharpness) */
function stillStats(img, ref) {
  let e = 0, g = 0, n = 0, m = 0;
  for (let y = 0; y < img.height - 1; y++)
    for (let x = 0; x < img.width - 1; x++) {
      const i = (y * img.width + x) * 4;
      for (let c = 0; c < 3; c++) {
        e += Math.abs(img.data[i + c] - ref.data[i + c]); n++;
        g += Math.abs(img.data[i + c] - img.data[i + 4 + c]) + Math.abs(img.data[i + c] - img.data[i + img.width * 4 + c]); m++;
      }
    }
  return { mae: e / n, grad: g / m };
}
for (const gpu of GPUS) {
  const c = CASES.find((k) => k.app === 'motion_vectors');
  const bare = shots.get(`${c.app}-${gpu}-bare`);
  if (!bare) throw new Error(`no still reference for ${gpu}`);
  const now = stillStats(await run(c, gpu, 'still', { MV_REF: '3', CW_RENDER_SCALE: '1' }), bare);
  const was = stillStats(await run(c, gpu, 'still-old', { MV_REF: '3', CW_TEST_TAA_STILL_ONLY: '1', CW_RENDER_SCALE: '1' }), bare);
  const soft = stillStats(await run(c, gpu, 'still-soft', { MV_REF: '3', CW_TEST_TAA_STILL_ONLY: 'soft', CW_RENDER_SCALE: '1' }), bare);
  const sharp = (s) => s.mae <= was.mae * STILL_SLACK && s.grad >= was.grad * STILL_GRAD;
  const f = (s) => `error ${s.mae.toFixed(3)}, gradient ${s.grad.toFixed(3)}`;
  console.log(`${gpu} still: TAA ${f(now)}; TAA without the moving-pixel terms ${f(was)}; every pixel taken as moving ${f(soft)}`);
  if (!sharp(now)) { console.log(`FAIL ${gpu} still: TAA softens the still picture`); fail++; }
  else console.log(`ok   ${gpu} still: as sharp as before (error within ${STILL_SLACK}x, gradient at least ${STILL_GRAD}x)`);
  if (sharp(soft)) { console.log(`FAIL ${gpu} still: CONTROL PASSED: every pixel taken as moving passes the sharpness check`); fail++; }
  else console.log(`ok   ${gpu} still: the control (every pixel taken as moving) fails it`);
}
console.log(fail ? `FAIL motion-vectors (${fail})` : 'ok   motion-vectors');
process.exit(fail ? 1 : 0);
