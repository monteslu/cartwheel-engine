// A night sea with no ProceduralSky: the water is lit by the scene's moon and mirrors the backdrop the camera draws.
//
// samples/water_night: a WorldEnvironment that clears to black with no ambient light, a DirectionalLight3D (the moon)
// low ahead shining toward the camera, and an additive backdrop (the moon's disc, a halo, a band of glow along the
// horizon, a row of lights on a far shore) on a layer of its own that the camera draws (cull_layers 3). The water is
// set to reflectivity 1 with refraction off, the case where everything it shows is its reflection and the moon's
// light. Each case runs through tools/run-cart.mjs on native and romdev, GL and WebGPU, and reads the frame:
//   lit        the water takes the moon as its sun (debug water.sun_lit 1), and the moon path under the disc holds at
//              least GLINT_MIN bright pixels (luminance over 200): the glint and crest light of the moon
//   reflected  the reflected halo left of the moon path averages luminance REFL_MIN or more, and the reflections of the
//              shore lights (orange pixels under the horizon) number LIGHTS_MIN or more
//   backdrop   reflection off, background a dark blue: the water mirrors the background colour (the open water
//              averages BG_MIN or more in blue), not black
//   split      the same in a split screen of two views drawn in one pass (split.array 1): each view's water lit and its
//              halo reflected (on WebGPU a background colour no longer clears the views drawn before)
//   sky        a ProceduralSky scene (samples/water_see_through, default cull_layers) draws byte-identical with the
//              old paths forced (CW_TEST_WATER_SKY_ONLY=1, CW_TEST_WATER_REFL_LAYER0=1)
// CONTROLS, each of which must break its check or the gate could not see it:
//   CW_TEST_WATER_SKY_ONLY=1      only a ProceduralSky lights the water (the old path): 'lit' fails, and 'backdrop'
//                                 (no stand-in for the reflection) fails
//   CW_TEST_WATER_REFL_LAYER0=1   the reflection pass draws layer 0 only (the old path): 'reflected' fails, in the
//                                 split screen too
//   CW_TEST_VIEWS_BG_CLEAR=1      (WebGPU) the background colour cleared by every view of the one-pass split
//                                 screen, as before: the left view is wiped and 'split' fails
//   the night sea with the old paths forced must differ from its own frame (the 'sky' check can see a change)
//
//   node tests/water-night.mjs [--hosts native,romdev] [--gpus gl,webgpu]
import { spawnSync } from 'node:child_process';
import { mkdirSync, existsSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'water-night');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const HOSTS = opt('--hosts', 'native,romdev').split(',');
const GPUS = opt('--gpus', 'gl,webgpu').split(',');
process.env.CW_ROMDEV_AGENT = process.env.CW_ROMDEV_AGENT || 'water-night-gate';
const FRAMES = 60, GLINT_MIN = 1600, REFL_MIN = 40, LIGHTS_MIN = 40, BG_MIN = 30;
const OLD_LIGHT = { CW_TEST_WATER_SKY_ONLY: '1' }, OLD_LAYERS = { CW_TEST_WATER_REFL_LAYER0: '1' };
const R1 = ['set Root/Water reflectivity 1'];

function run(host, gpu, tag, { app = 'water_night', env = {}, cmds = R1 } = {}) {
  const shot = join(OUT, `${host}-${gpu}-${tag}.png`), st = join(OUT, `${host}-${gpu}-${tag}.json`);
  const args = ['tools/run-cart.mjs', '--host', host, '--gpu', gpu, '--app', app, '--frames', String(FRAMES), '--shot', shot, '--state', st,
    ...(Object.keys(env).length ? ['--env', JSON.stringify(env)] : []), ...(cmds.length ? ['--start-cmds', cmds.join(';')] : [])];
  const r = spawnSync(process.execPath, args, { cwd: ROOT, env: process.env, encoding: 'utf8', timeout: 600000 });
  if (r.status !== 0 || !existsSync(shot)) { console.log(`FAIL water-night ${host} ${gpu} ${tag}: exit ${r.status}: ${(r.stderr || r.stdout || '').slice(-600)}`); process.exit(1); }
  return { im: readImage(shot), st: JSON.parse(readFileSync(st, 'utf8')) };
}

// pixel boxes in 1920 x 1080 frame pixels; cx: the view's middle (a split screen's views are 960 wide)
const lum = (r, g, b) => 0.2126 * r + 0.7152 * g + 0.0722 * b;
function box(im, x0, y0, x1, y1, f) {
  let n = 0, hits = 0, sum = [0, 0, 0];
  for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) {
    const o = (y * im.w + x) * 4, r = im.rgba[o], g = im.rgba[o + 1], b = im.rgba[o + 2];
    sum[0] += r, sum[1] += g, sum[2] += b, n++;
    if (f(r, g, b)) hits++;
  }
  return { mean: sum.map((v) => v / n), hits };
}
function measure(im, cx = 960) {
  if (im.w !== 1920 || im.h !== 1080) { console.log(`FAIL water-night: frame is ${im.w} x ${im.h}, the boxes need 1920 x 1080`); process.exit(1); }
  const glint = box(im, cx - 80, 760, cx + 80, 1080, (r, g, b) => lum(r, g, b) > 200).hits;
  const halo = box(im, cx - 100, 630, cx - 60, 700, () => false).mean;
  // the shore lights' reflections: orange pixels in a strip under the horizon (the lights stand over it)
  const lights = box(im, cx - 300, 540, cx + 300, 600, (r, g, b) => r > 120 && r > 1.4 * b && r > 1.15 * g).hits;
  const side = box(im, cx - 360, 800, cx - 160, 1000, () => false).mean;
  return { glint, halo: lum(...halo), lights, sideBlue: side[2] };
}

let fail = 0;
const ok = (c, msg) => { console.log(`${c ? 'ok  ' : 'FAIL'} water-night ${msg}`); if (!c) fail++; };
const mustFail = (c, msg) => { console.log(`${c ? 'ok  ' : 'FAIL'} water-night control fails as it must: ${msg}`); if (!c) fail++; };
const f0 = (v) => v.toFixed(0);
const same = (a, b) => a.im.rgba.length === b.im.rgba.length && Buffer.compare(Buffer.from(a.im.rgba), Buffer.from(b.im.rgba)) === 0;

for (const host of HOSTS) for (const gpu of GPUS) {
  const t = `${host} ${gpu}`;
  const nw = run(host, gpu, 'night'), m = measure(nw.im);
  console.log(`water-night ${t}: glint ${m.glint}, halo ${f0(m.halo)}, lights ${m.lights}, sun_lit ${nw.st['water.sun_lit']}`);
  ok(nw.st['water.sun_lit'] === 1 && m.glint >= GLINT_MIN, `${t}: lit by the moon (sun_lit ${nw.st['water.sun_lit']}, moon path ${m.glint} bright px >= ${GLINT_MIN})`);
  ok(m.halo >= REFL_MIN && m.lights >= LIGHTS_MIN, `${t}: the backdrop reflected (halo ${f0(m.halo)} >= ${REFL_MIN}, shore lights ${m.lights} px >= ${LIGHTS_MIN})`);

  const ol = run(host, gpu, 'old-light', { env: OLD_LIGHT }), mo = measure(ol.im);
  mustFail(!(ol.st['water.sun_lit'] === 1 && mo.glint >= GLINT_MIN), `${t}: sky-only light, sun_lit ${ol.st['water.sun_lit']}, moon path ${mo.glint} bright px`);
  const oy = run(host, gpu, 'old-layers', { env: OLD_LAYERS }), my = measure(oy.im);
  mustFail(!(my.halo >= REFL_MIN && my.lights >= LIGHTS_MIN), `${t}: reflection of layer 0 only, halo ${f0(my.halo)}, shore lights ${my.lights} px`);

  const BG = ['set Root/Water reflection false', 'set Root/Env background color(0.02, 0.04, 0.12, 1)', 'set Root/Water reflectivity 1'];
  const bg = measure(run(host, gpu, 'backdrop', { cmds: BG }).im), bo = measure(run(host, gpu, 'backdrop-old', { cmds: BG, env: OLD_LIGHT }).im);
  ok(bg.sideBlue >= BG_MIN, `${t}: no planar reflection: the water mirrors the background (open water blue ${f0(bg.sideBlue)} >= ${BG_MIN})`);
  mustFail(!(bo.sideBlue >= BG_MIN), `${t}: sky-only path, open water blue ${f0(bo.sideBlue)}`);

  const sp = run(host, gpu, 'split', { env: { WN_SPLIT: '1' } }), sl = measure(sp.im, 480), sr = measure(sp.im, 1440);
  ok(sp.st['split.array'] === 1, `${t}: split screen in one pass (split.array ${sp.st['split.array']})`);
  for (const [k, v] of [['left', sl], ['right', sr]])
    ok(v.glint >= GLINT_MIN / 2 && v.halo >= REFL_MIN, `${t}: split ${k} view lit (${v.glint} bright px >= ${GLINT_MIN / 2}) and reflecting the backdrop (halo ${f0(v.halo)} >= ${REFL_MIN})`);
  const so = run(host, gpu, 'split-old-layers', { env: { WN_SPLIT: '1', ...OLD_LAYERS } });
  const sol = measure(so.im, 480), sor = measure(so.im, 1440);
  mustFail(!(sol.halo >= REFL_MIN) && !(sor.halo >= REFL_MIN), `${t}: split, reflection of layer 0 only, halo ${f0(sol.halo)} / ${f0(sor.halo)}`);
  if (gpu === 'webgpu') {
    // (WebGPU: a background colour was a clear of the whole target, so the second view wiped the first)
    const sc = measure(run(host, gpu, 'split-bg-clear', { env: { WN_SPLIT: '1', CW_TEST_VIEWS_BG_CLEAR: '1' } }).im, 480);
    mustFail(!(sc.glint >= GLINT_MIN / 2 && sc.halo >= REFL_MIN), `${t}: split, the background cleared by every view, left view ${sc.glint} bright px, halo ${f0(sc.halo)}`);
  }

  const sky = run(host, gpu, 'sky', { app: 'water_see_through', cmds: [] });
  const skyOld = run(host, gpu, 'sky-old', { app: 'water_see_through', cmds: [], env: { ...OLD_LIGHT, ...OLD_LAYERS } });
  ok(same(sky, skyOld), `${t}: a ProceduralSky scene draws byte-identical to the old paths`);
  const both = run(host, gpu, 'night-old', { env: { ...OLD_LIGHT, ...OLD_LAYERS } });
  mustFail(!same(nw, both), `${t}: the night sea with the old paths differs from its own frame`);
}
process.exit(fail ? 1 : 0);
