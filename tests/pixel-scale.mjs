// cw_app_desc.pixel_scale and crisp UI (src/platform/pixel.c) on native and
// Chromium, GL and WebGPU: the pixel_n2 / pixel_n3 apps (tests/unit/
// pixel_scale.c) at an output of 1000x563, a multiple of neither 2 nor 3, so
// the picture is centred with bars.
//
// Per host, GPU API, N (2, 3) and crisp UI (off, on: the crisp_ui debug command):
//   - geometry: the frame is 1000x563; cw.view says the picture is
//     floor(1000/N)*N x floor(563/N)*N at ((1000 - w) / 2, (563 - h) / 2); every
//     pixel outside it is black (the bars)
//   - nearest-neighbour: crisp UI off, every N x N block of the picture is one
//     colour (the whole frame, 3D, post and the 2D text, is pixelated); on,
//     the blocks outside the UI are still one colour and the text's blocks are
//     not (it draws at the picture's resolution)
//   - pointer: the input script puts the mouse at logical (200, 50); the app
//     sees (200, 50) (debug field pointer) and its magenta marker, a 4x4
//     logical square centred there, is centred at the picture's origin +
//     (200, 50) * (picture width / 320) in the frame. On Chromium the pointer
//     goes through the host in framebuffer pixels, so the bars are crossed.
// CONTROL: pixel_n2 with CW_PIXEL_SCALE=1 (native) must fail the block check.
//
//   node tests/pixel-scale.mjs [--hosts native,chromium] [--gpus gl,webgpu]
import { spawn } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { armGuard } from '../tools/lib/gpu-guard.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
await takeSuiteLock('pixel-scale.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'pixel-scale');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : d);
const hosts = opt('--hosts', 'native,chromium').split(',');
const gpus = opt('--gpus', 'gl,webgpu').split(',');
const OUT_W = 1000, OUT_H = 563, LW = 320, LH = 180;
const POINTER = [200, 50];
const TEXT = [10, 138, 170, 160]; // logical rect around the UI text line
const FRAMES = 20;
const script = join(OUT, 'pointer.json');
writeFileSync(script, JSON.stringify([{ frame: 2, mouse: { x: POINTER[0], y: POINTER[1] } }]));

function run(host, gpu, app, tag, { crisp = false, env = {} } = {}) {
  const shot = join(OUT, `${tag}.png`), state = join(OUT, `${tag}.json`);
  const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--gpu', gpu, '--app', app, '--frames', String(FRAMES),
    '--shot', shot, '--state', state, '--input', script, ...(crisp ? ['--start-cmds', 'crisp_ui 1'] : [])];
  return new Promise((res) => {
    const p = spawn('node', args, { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...env } });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.stdout.on('data', () => {});
    p.on('close', (code) => {
      if (code !== 0) return res({ error: `run failed: ${err.trim().split('\n').slice(-2).join(' | ')}` });
      res({ img: readImage(shot), state: JSON.parse(readFileSync(state, 'utf8')) });
    });
  });
}

const px = (img, x, y) => { const i = 4 * (y * img.w + x); return [img.rgba[i], img.rgba[i + 1], img.rgba[i + 2]]; };
const same = (a, b) => a[0] === b[0] && a[1] === b[1] && a[2] === b[2];
const field = (state, name) => state[name] ?? (state.fields || []).find((f) => f.name === name)?.values;

/** blocks of the picture that are not one colour: [count inside rect, count outside rect] (rect in frame pixels) */
function blocks(img, n, view, rect) {
  let inside = 0, outside = 0;
  for (let by = 0; by < view.h / n; by++) for (let bx = 0; bx < view.w / n; bx++) {
    const x0 = view.x + bx * n, y0 = view.y + by * n, c = px(img, x0, y0);
    let uniform = true;
    for (let y = y0; y < y0 + n && uniform; y++) for (let x = x0; x < x0 + n; x++) if (!same(px(img, x, y), c)) { uniform = false; break; }
    if (uniform) continue;
    const inRect = rect && x0 + n > rect[0] && x0 < rect[2] && y0 + n > rect[1] && y0 < rect[3];
    if (inRect) inside++;
    else outside++;
  }
  return [inside, outside];
}

function check(r, n, crisp) {
  const problems = [];
  const { img, state } = r;
  if (img.w !== OUT_W || img.h !== OUT_H) return [`the frame is ${img.w}x${img.h}, not ${OUT_W}x${OUT_H}`];
  const sw = Math.floor(OUT_W / n), sh = Math.floor(OUT_H / n);
  const view = { w: sw * n, h: sh * n, x: (OUT_W - sw * n) >> 1, y: (OUT_H - sh * n) >> 1 };
  const v = field(state, 'cw.view') || [];
  if (v[2] !== view.w || v[3] !== view.h || (v[4] || 0) !== view.x || (v[5] || 0) !== view.y)
    problems.push(`cw.view ${JSON.stringify(v)}, expected the picture ${view.w}x${view.h} at ${view.x},${view.y}`);
  let bar = 0;
  for (let y = 0; y < OUT_H; y++) for (let x = 0; x < OUT_W; x++) {
    if (x >= view.x && x < view.x + view.w && y >= view.y && y < view.y + view.h) continue;
    const c = px(img, x, y);
    if (c[0] | c[1] | c[2]) bar++;
  }
  if (bar) problems.push(`${bar} bar pixels are not black`);
  const s = view.w / LW; // frame pixels per logical unit
  const text = TEXT.map((t, i) => (i % 2 ? view.y : view.x) + t * s);
  const marker = [view.x + (POINTER[0] - 3) * s, view.y + (POINTER[1] - 3) * s, view.x + (POINTER[0] + 3) * s, view.y + (POINTER[1] + 3) * s];
  const [inText, outside] = blocks(img, n, view, text);
  const [inMarker] = blocks(img, n, view, marker);
  const stray = outside - inMarker;
  if (!crisp && inText + outside) problems.push(`crisp UI off: ${inText + outside} ${n}x${n} blocks are not one colour (${inText} in the text)`);
  if (crisp && stray > 0) problems.push(`crisp UI on: ${stray} ${n}x${n} blocks outside the UI are not one colour`);
  if (crisp && inText < 20) problems.push(`crisp UI on: only ${inText} blocks in the text are finer than ${n}x${n} (the text is not at the picture's resolution)`);
  const p = field(state, 'pointer') || [];
  if (!(Math.abs(p[0] - POINTER[0]) < 0.6 && Math.abs(p[1] - POINTER[1]) < 0.6)) problems.push(`the app sees the pointer at ${JSON.stringify(p)}, not ${POINTER}`);
  let mx0 = 1e9, my0 = 1e9, mx1 = -1, my1 = -1;
  for (let y = 0; y < OUT_H; y++) for (let x = 0; x < OUT_W; x++) {
    const c = px(img, x, y);
    if (c[0] > 200 && c[1] < 60 && c[2] > 200) { mx0 = Math.min(mx0, x); my0 = Math.min(my0, y); mx1 = Math.max(mx1, x); my1 = Math.max(my1, y); }
  }
  const want = [view.x + POINTER[0] * s, view.y + POINTER[1] * s];
  const got = [(mx0 + mx1 + 1) / 2, (my0 + my1 + 1) / 2];
  const tol = crisp ? 1.5 : n + 0.5;
  if (mx1 < 0) problems.push('no pointer marker in the frame');
  else if (Math.abs(got[0] - want[0]) > tol || Math.abs(got[1] - want[1]) > tol)
    problems.push(`the marker is centred at ${got.map((g) => g.toFixed(1))}, expected ${want.map((g) => g.toFixed(1))} (+-${tol})`);
  return problems;
}

const guard = armGuard('pixel-scale.mjs');
await guard.gate('pixel-scale');
const failed = [];
let total = 0;
const report = (name, problems, detail = '') => {
  total++;
  console.log(`${problems.length ? 'FAIL' : 'ok  '} pixel-scale ${name}${problems.length ? ': ' + problems.join('; ') : detail}`);
  if (problems.length) failed.push(name);
};
for (const host of hosts) for (const gpu of gpus) for (const n of [2, 3]) for (const crisp of [false, true]) {
  const name = `${host} ${gpu} N=${n} crisp UI ${crisp ? 'on' : 'off'}`;
  const r = await run(host, gpu, `pixel_n${n}`, `${host}-${gpu}-n${n}-${crisp ? 'crisp' : 'plain'}`, { crisp });
  report(name, r.error ? [r.error] : check(r, n, crisp));
}
// CONTROL: no pixel scale, the block check must see non-uniform blocks
{
  const r = await run('native', 'gl', 'pixel_n2', 'control', { env: { CW_PIXEL_SCALE: '1' } });
  const problems = [];
  if (r.error) problems.push(r.error);
  else {
    const [a, b] = blocks(r.img, 2, { x: 0, y: 0, w: 1000, h: 562 }, null);
    if (a + b < 100) problems.push(`CONTROL passed: an unscaled frame has only ${a + b} non-uniform 2x2 blocks`);
  }
  report('CONTROL native gl, pixel scale 1: blocks are not uniform', problems);
}
guard.finish();
console.log(`pixel-scale: ${total - failed.length}/${total} pass` + (failed.length ? `; failed: ${failed.join(', ')}` : '') + ` (frames in ${OUT})`);
process.exit(failed.length ? 1 : 0);
