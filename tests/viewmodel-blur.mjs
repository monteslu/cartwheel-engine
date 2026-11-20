// Motion blur leaves the first person viewmodel sharp (src/render/pipeline_post.c: the velocity pass gives the
// viewmodel's pixels no motion, and the blur's depth weights keep what moves behind it off it).
//
// viewmodel_demo with its camera turning 0.04 rad a tick (VIEWMODEL_PAN=1), native, frame 30, per backend:
//   plain   no blur            blur    motion blur on            bare    no blur, the gun hidden
// The gun's pixels are where plain differs from bare by more than 48 levels, shrunk by two pixels. On them the
// blurred frame must be the plain one (at most 3 levels apart anywhere); away from them (the mask grown by four
// pixels) at least 1000 pixels must differ by more than 8 levels (the scene is mostly flat colour: its edges are
// what a blur changes), or the blur did not run.
// CONTROL: CW_TEST_VIEWMODEL_MOTION=camera (the gun's pixels take the camera's motion like the world's) must
// smear the gun: the first check must fail.
//
//   node tests/viewmodel-blur.mjs [--gpus gl,webgpu]
import { spawn } from 'node:child_process';
import { mkdirSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv, wgpuPin } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox();
await takeSuiteLock('viewmodel-blur.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'viewmodel-blur');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const gi = argv.indexOf('--gpus');
const GPUS = (gi >= 0 ? argv[gi + 1] : 'gl,webgpu').split(',');

function run(gpu, tag, extra) {
  const exe = join(ROOT, 'build/native/samples/viewmodel_demo');
  if (!existsSync(exe)) throw new Error('no native build of viewmodel_demo (scripts/build.sh viewmodel_demo)');
  const shot = join(OUT, `${gpu}-${tag}.png`);
  const env = { ...process.env, CW_GPU: gpu, CW_FRAMES: '30', CW_SCREENSHOT: shot, SDL_AUDIODRIVER: 'dummy', CW_RENDER_SCALE: '2', VIEWMODEL_PAN: '1',
    ...(gpu === 'webgpu' ? wgpuPin().env : {}), ...extra };
  return new Promise((res, rej) => {
    const p = spawn('timeout', ['-s', 'KILL', '300', exe], { env, stdio: ['ignore', 'ignore', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => (code !== 0 ? rej(new Error(`viewmodel_demo ${gpu} ${tag} exited ${code}: ${err.slice(-800)}`)) : res(readImage(shot))));
  });
}
const diff = (a, b, i) => Math.max(Math.abs(a.rgba[i * 4] - b.rgba[i * 4]), Math.abs(a.rgba[i * 4 + 1] - b.rgba[i * 4 + 1]), Math.abs(a.rgba[i * 4 + 2] - b.rgba[i * 4 + 2]));
/** a mask grown (r > 0) or shrunk (r < 0) by |r| pixels */
function grow(m, w, h, r) {
  const out = new Uint8Array(m.length), n = Math.abs(r), want = r > 0 ? 1 : 0;
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      let hit = false;
      for (let j = -n; j <= n && !hit; j++)
        for (let i = -n; i <= n; i++) {
          const xx = Math.min(w - 1, Math.max(0, x + i)), yy = Math.min(h - 1, Math.max(0, y + j));
          if (m[yy * w + xx] === want) { hit = true; break; }
        }
      out[y * w + x] = r > 0 ? (hit ? 1 : 0) : (hit ? 0 : 1);
    }
  return out;
}
/** the gun's worst difference from the plain frame, and the share of the world that differs */
function judge(blur, plain, gun, far) {
  let worst = 0, over = 0, n = 0, world = 0, moved = 0;
  for (let i = 0; i < gun.length; i++) {
    const d = diff(blur, plain, i);
    if (gun[i]) { n++; if (d > worst) worst = d; if (d > 3) over++; }
    else if (!far[i]) { world++; if (d > 8) moved++; }
  }
  return { worst, over: over / Math.max(n, 1), n, moved };
}

let fail = 0;
for (const gpu of GPUS) {
  const plain = await run(gpu, 'plain', {}), bare = await run(gpu, 'bare', { VIEWMODEL_HIDE: '1' });
  const blur = await run(gpu, 'blur', { VIEWMODEL_BLUR: '1' });
  const ctl = await run(gpu, 'control', { VIEWMODEL_BLUR: '1', CW_TEST_VIEWMODEL_MOTION: 'camera' });
  const w = plain.w, h = plain.h, raw = new Uint8Array(w * h);
  for (let i = 0; i < raw.length; i++) raw[i] = diff(plain, bare, i) > 48 ? 1 : 0;
  const gun = grow(raw, w, h, -2), far = grow(raw, w, h, 4);
  const j = judge(blur, plain, gun, far), c = judge(ctl, plain, gun, far);
  console.log(`${gpu}: the gun covers ${j.n} pixels; with motion blur its worst difference from the plain frame is ${j.worst} levels, and ${j.moved} pixels of the world differ by more than 8`);
  if (j.n < 2000) { console.log(`FAIL ${gpu}: the gun's mask is ${j.n} pixels: the run proves nothing`); fail++; }
  if (j.worst > 3) { console.log(`FAIL ${gpu}: the gun is blurred (${(j.over * 100).toFixed(2)}% of its pixels over 3 levels)`); fail++; }
  if (j.moved < 1000) { console.log(`FAIL ${gpu}: the world is not blurred`); fail++; }
  if (c.worst <= 3) { console.log(`FAIL ${gpu}: CONTROL PASSED: with the camera's motion on the gun's pixels it is still sharp`); fail++; }
  else console.log(`ok   ${gpu}: control (the gun's pixels take the camera's motion) blurs the gun: worst ${c.worst} levels, ${(c.over * 100).toFixed(1)}% of its pixels over 3`);
}
console.log(fail ? `FAIL viewmodel-blur (${fail})` : 'ok   viewmodel-blur');
process.exit(fail ? 1 : 0);
