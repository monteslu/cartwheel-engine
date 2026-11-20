// The cascaded sun keeps its specular highlight: gi_room's chrome ball, lit by the sun through three
// cascades (GI_SUN_CASCADES=3), against the same sun with three.c's one shadow map fitted over the room
// (GI_SUN_FIT=1: the map moved up the sun's ray and widened so the ceiling is a caster), on GL and on
// WebGPU, with the ceiling taken away (GI_NO_CEILING=1) so the sun reaches the ball. The highlight on
// the ball (a 40 x 40 patch at native scale) must be there under cascades (its brightest pixel at least
// MIN_PEAK) and match the one map's (mean difference at most MAX_MEAN levels).
// Control: with the ceiling in place the ball's highlight point is in the ceiling's shadow (correctly,
// under cascades and under the fitted map alike), so the presence check must FAIL there, or it cannot
// see a missing highlight. (The room's default sun map starts at the light and leaves the ceiling out
// of it, so it lights that point: it is not a reference.)
//
//   node tests/csm-specular.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, writePng } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
await takeSuiteLock('csm-specular');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'csm-specular');
mkdirSync(OUT, { recursive: true });
const MIN_PEAK = 240, MAX_MEAN = 12;
let fail = 0;
const say = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} csm-specular: ${msg}`); if (!ok) fail++; };

function run(gpu, tag, env) {
  const shot = join(OUT, `${gpu}.${tag}.png`), state = join(OUT, `${gpu}.${tag}.json`);
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'gi_room',
    '--frames', '30', '--shot', shot, '--state', state], { env: { ...process.env, ...env }, encoding: 'utf8', timeout: 300000 });
  if (r.status !== 0) throw new Error(`${gpu} ${tag}: run-cart failed\n${(r.stdout + r.stderr).slice(-2000)}`);
  return { img: readImage(shot), st: JSON.parse(readFileSync(state, 'utf8')) };
}
// the sun's highlight on the chrome ball, in the frame's own pixels (the frame is 3 x 640 x 360 natively)
const patch = (img) => {
  const s = img.w / 640, r = { x: Math.round(880 / 3 * s), y: Math.round(450 / 3 * s), w: Math.round(40 / 3 * s), h: Math.round(40 / 3 * s) };
  return r;
};
const peak = (img) => {
  const r = patch(img);
  let m = 0;
  for (let y = r.y; y < r.y + r.h; y++) for (let x = r.x; x < r.x + r.w; x++) {
    const i = (y * img.w + x) * 4;
    m = Math.max(m, (img.rgba[i] + img.rgba[i + 1] + img.rgba[i + 2]) / 3);
  }
  return m;
};
const mean = (a, b) => {
  const r = patch(a);
  let s = 0, n = 0;
  for (let y = r.y; y < r.y + r.h; y++) for (let x = r.x; x < r.x + r.w; x++) for (let c = 0; c < 3; c++) {
    const i = (y * a.w + x) * 4 + c;
    s += Math.abs(a.rgba[i] - b.rgba[i]);
    n++;
  }
  return s / n;
};
// a side by side crop of the ball at native scale, for a look
function crop(file, imgs) {
  const s = imgs[0].w / 640, x0 = Math.round(820 / 3 * s), y0 = Math.round(380 / 3 * s), w = Math.round(300 / 3 * s), h = w;
  const W = (w + 2) * imgs.length, d = new Uint8Array(W * h * 4);
  imgs.forEach((im, k) => {
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) for (let c = 0; c < 4; c++)
      d[(y * W + k * (w + 2) + x) * 4 + c] = im.rgba[((y0 + y) * im.w + x0 + x) * 4 + c];
  });
  writePng(file, W, h, d);
}

for (const gpu of ['gl', 'webgpu']) {
  const csm = run(gpu, 'cascades', { GI_NO_CEILING: '1', GI_SUN_CASCADES: '3' });
  const map = run(gpu, 'map', { GI_NO_CEILING: '1', GI_SUN_FIT: '1' });
  const ctl = run(gpu, 'ceiling', { GI_SUN_CASCADES: '3' });
  for (const [t, r] of [['cascades', csm], ['map', map], ['ceiling', ctl]])
    say(r.st['cw.three.missing'] === 0 && r.st['cw.gpu.errors'] === 0,
      `${gpu} ${t}: no missing program state (${r.st['cw.three.missing']}) or GPU error (${r.st['cw.gpu.errors']})`);
  const pc = peak(csm.img), pm = peak(map.img), pk = peak(ctl.img), m = mean(csm.img, map.img);
  say(pm >= MIN_PEAK, `${gpu}: the one map shows the highlight (peak ${pm.toFixed(0)} >= ${MIN_PEAK})`);
  say(pc >= MIN_PEAK, `${gpu}: cascades show the highlight (peak ${pc.toFixed(0)} >= ${MIN_PEAK})`);
  say(m <= MAX_MEAN, `${gpu}: cascades vs one map over the highlight: mean ${m.toFixed(2)} levels (<= ${MAX_MEAN})`);
  say(pk < MIN_PEAK, `${gpu}: control (the ceiling shadows the point) must fail the presence check: peak ${pk.toFixed(0)}${pk >= MIN_PEAK ? ' CONTROL PASSED' : ''}`);
  crop(join(OUT, `${gpu}.ball.png`), [map.img, csm.img, ctl.img]);
}
process.exit(fail ? 1 : 0);
