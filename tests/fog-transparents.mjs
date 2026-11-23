// Transparent and additive surfaces take a HeightFog at their own distance (src/render/fog.c, environment_internal.h):
// samples/fog_transparents, a plain reaching 400 m under a sky the fog leaves alone, the horizon across the middle of
// the picture, and standing on the ground 38 m away (each from 0 to 4 m high, so it crosses the horizon) a lit
// alpha-blended panel (Glass), an additive panel (Add), additive GPU particle flames (Fire) and alpha-blended GPU
// particles (Smoke). Every run is the same deterministic frame, its post chain plain (LINEAR tone curve, no bloom),
// so pictures add up in linear light; each configuration is drawn four ways: subjects shown or hidden (FT_HIDE=1),
// fog on or off (FT_FOG=0). Over a band above the horizon (over the sky) and one below it (over ground 80 to 400 m
// away), for each subject:
//   Add, Fire    additive: the light it adds with fog is the light it adds without, times the fog's transmittance at
//                its own distance, T = exp(-optical depth) of the height fog (the fog pass's formula), within 6%
//                (12% for the particles) above and below the horizon alike
//   Glass        alpha-blended: background x (1 - a) + (its colour x T + the fog's light) x a, its colour from the run
//                without fog, within 3/255 (mean over the band)
//   Smoke        alpha-blended particles of a known colour: the same, the coverage taken from the run without fog
// Volumetric fog (FT_VFOG=1, Medium and up): the transmittance comes from the grid, so it is held to be the same
// above and below the horizon (within 12%), and to fog at all (under 0.95).
// Every preset for the analytic fog (Very Low and Low run it inside the tonemap pass when nothing draws after it),
// a split screen of two views (FT_MODE=split) and the world as a Backdrop of an empty scene (FT_MODE=backdrop: the
// world's own fog), natively and as the dual cart on romdev, GL and WebGPU.
// CONTROLS that must fail: the fog over everything by the depth behind it, as before (CW_TEST_FOG_OVER_TRANSPARENTS=1:
// the horizon line, washed below and crisp above), in each mode; and three.c materials without their own fog
// (CW_TEST_FOG_NO_HFOG=1: the panels drawn unfogged).
//   node tests/fog-transparents.mjs [--hosts native,romdev] [--gpu gl,webgpu] [--presets verylow,...] [--quick]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'fog-transparents');
mkdirSync(OUT, { recursive: true });
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const HOSTS = arg('--hosts', process.env.CW_TEST_GPU === 'software' ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
const PRESETS = arg('--presets', 'verylow,low,medium,high,ultra').split(',');
const QUICK = process.argv.includes('--quick');
const FRAMES = 90;
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

function run(host, gpu, name, env, preset) {
  const png = join(OUT, `${name}.${host}.${gpu}.png`), st = join(OUT, `${name}.${host}.${gpu}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'fog_transparents', '--frames', String(FRAMES),
    '--gpu', gpu, '--shot', png, '--state', st, '--env', JSON.stringify(env), '--start-cmds', `quality ${preset}${process.env.FT_EXTRA_CMDS ? ';' + process.env.FT_EXTRA_CMDS : ''}`], { encoding: 'utf8', env: process.env });
  if (r.status !== 0) throw new Error(`${host} ${gpu} ${name}: ${((r.stderr || '') + (r.stdout || '')).trim().split('\n').slice(-3).join(' | ')}`);
  const s = JSON.parse(readFileSync(st, 'utf8'));
  return { img: readImage(png), s: s.fields || s };
}
const v = (r, k) => { const x = r.s[k]; return typeof x === 'object' && x !== null && 'value' in x ? x.value : x; };

const LIN = new Float64Array(256);
for (let i = 0; i < 256; i++) { const c = i / 255; LIN[i] = c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; }
const SRGB = (x) => { x = Math.min(1, Math.max(0, x)); return 255 * (x <= 0.0031308 ? 12.92 * x : 1.055 * x ** (1 / 2.4) - 0.055); };

// the world (samples/fog_transparents/assets/world.cwscene)
const CAM = [0, 1.6, 0], FOV = 50, Z = -38;
const FOG = { density: 0.02, falloff: 0.05, base: 0, color: [0.42, 0.36, 0.3] };
const SUBJECTS = {
  Glass: { x: -12, half: 0.9, below: [0.4, 1.0], above: [2.3, 3.1] },
  Add: { x: -4, half: 0.9, below: [0.4, 1.0], above: [2.3, 3.1] },
  Fire: { x: 4, half: 1.0, below: [0.4, 1.0], above: [2.2, 2.8] },
  Smoke: { x: 12, half: 1.0, below: [0.4, 1.0], above: [2.2, 2.8] },
};
const SMOKE = [0.05, 0.12, 0.2];
/** the fog's transmittance to a point (the fog pass's analytic height fog) */
function trans(p) {
  const r = [p[0] - CAM[0], p[1] - CAM[1], p[2] - CAM[2]], l = Math.hypot(...r), vy = r[1] / l;
  const a = FOG.density * Math.exp(-FOG.falloff * (CAM[1] - FOG.base)), k = FOG.falloff * vy * l;
  const tau = a * l * (Math.abs(k) > 1e-4 ? (1 - Math.exp(-k)) / k : 1 - 0.5 * k);
  return Math.exp(-tau);
}
/** the pixels of a world rectangle on the plane z = Z (x0..x1, y0..y1), in a view (rectangle of the image) */
function pixels(view, x0, x1, y0, y1) {
  const t = Math.tan((FOV / 2) * Math.PI / 180), asp = view.w / view.h, d = -Z;
  const col = (x) => view.x + (x / d / (t * asp) * 0.5 + 0.5) * view.w, row = (y) => view.y + (0.5 - (y - CAM[1]) / d / t * 0.5) * view.h;
  const out = [];
  for (let py = Math.ceil(row(y1)); py <= Math.floor(row(y0)); py++)
    for (let px = Math.ceil(col(x0)); px <= Math.floor(col(x1)); px++) {
      const ndx = ((px + 0.5 - view.x) / view.w - 0.5) * 2, ndy = (0.5 - (py + 0.5 - view.y) / view.h) * 2;
      out.push({ px, py, p: [ndx * t * asp * d, CAM[1] + ndy * t * d, Z] });
    }
  return out;
}
const at = (img, px, py, k) => LIN[img.rgba[4 * (py * img.w + px) + k]];

/** one band of one subject: the measures the checks read */
function measure(img, view, name, band) {
  const S = SUBJECTS[name], ys = S[band];
  const px = pixels(view, S.x - S.half, S.x + S.half, ys[0], ys[1]);
  const [shown, hidden, shown0, hidden0] = img;
  let cf = 0, c0 = 0, tsum = 0, errSum = 0, n = 0, aSum = 0;
  for (const q of px) {
    const T = trans(q.p), f = 1 - T;
    // the particle flames: only where they add enough light to measure against 8-bit steps (over 0.03 a channel,
    // about 4 steps at the sky's level, picked on the run without fog so the pick cannot favour a result)
    let p0 = 0;
    for (let k = 0; k < 3; k++) p0 += at(shown0, q.px, q.py, k) - at(hidden0, q.px, q.py, k);
    if (name === 'Fire' && p0 < 0.09) continue;
    tsum += T;
    n++;
    for (let k = 0; k < 3; k++) {
      cf += at(shown, q.px, q.py, k) - at(hidden, q.px, q.py, k);
      c0 += at(shown0, q.px, q.py, k) - at(hidden0, q.px, q.py, k);
    }
    if (name === 'Glass' || name === 'Smoke') {
      // the coverage: 0.5 for the panel; for the smoke, from the run without fog (colour known)
      let a = 0.5;
      if (name === 'Smoke') {
        let best = 0, kb = 2;
        for (let k = 0; k < 3; k++) { const d = Math.abs(SMOKE[k] - at(hidden0, q.px, q.py, k)); if (d > best) best = d, kb = k; }
        a = Math.min(1, Math.max(0, (at(shown0, q.px, q.py, kb) - at(hidden0, q.px, q.py, kb)) / (SMOKE[kb] - at(hidden0, q.px, q.py, kb))));
      }
      aSum += a;
      let worst = 0;
      for (let k = 0; k < 3; k++) {
        const own = at(shown0, q.px, q.py, k) - at(hidden0, q.px, q.py, k) * (1 - a); // colour x coverage, unfogged
        const want = at(hidden, q.px, q.py, k) * (1 - a) + own * T + a * FOG.color[k] * f;
        worst = Math.max(worst, Math.abs(SRGB(want) - shown.rgba[4 * (q.py * shown.w + q.px) + k]));
      }
      errSum += worst;
    }
  }
  return { n, ratio: c0 > 1e-6 ? cf / c0 : NaN, T: n ? tsum / n : 1, err: n ? errSum / n : 0, cover: n ? aSum / n : 0, c0 };
}

let fail = 0, ctlFail = 0, ctlRuns = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };

/** the checks of one configuration; returns the problems (empty: it holds) */
function judge(imgs, viewsOf, vfog, few = false) {
  const bad = [], views = viewsOf(imgs[0]);
  for (const [vi, view] of views.entries())
    for (const name of Object.keys(SUBJECTS)) {
      const above = measure(imgs, view, name, 'above'), below = measure(imgs, view, name, 'below');
      const tag = `${views.length > 1 ? `view ${vi} ` : ''}${name}`;
      if (name !== 'Fire' && (!above.n || !below.n)) { bad.push(`${tag}: no pixels`); continue; }
      if (vfog) {
        if (name !== 'Add' && name !== 'Fire') continue;
        const ra = above.ratio, rb = below.ratio, d = Math.abs(ra - rb) / Math.max(ra, rb);
        if (!(d <= 0.12 && ra < 0.95 && rb < 0.95)) bad.push(`${tag}: transmittance above ${ra.toFixed(3)}, below ${rb.toFixed(3)} (apart ${(100 * d).toFixed(1)}%, at most 12%, both under 0.95)`);
        continue;
      }
      let measured = 0;
      for (const [band, m] of [['above', above], ['below', below]]) {
        // (Very Low and Low draw a share of the particles, those emitted over part of their life: a band can hold too
        // little light to measure against 8-bit steps, a mean under 0.01 a channel; every band counts on Medium and up)
        if (name === 'Fire' && m.n < 40) continue;
        if (few && name === 'Smoke' && Math.abs(m.c0) < 0.03 * m.n) continue;
        measured++;
        if (name === 'Add' || name === 'Fire') {
          // (Very Low and Low draw into an RG11B10 float scene, whose 6-bit mantissa rounds each faint additive particle
          // as it blends: a pile of particles each fogged to half their light loses about a fifth of it, brighter pixels
          // less; the old path, the fog by the sky behind them, still breaks the wider bound)
          const tol = name === 'Fire' ? (few ? 0.3 : 0.12) : 0.06, d = Math.abs(m.ratio - m.T) / m.T;
          if (!(d <= tol)) bad.push(`${tag} ${band}: its light x ${m.ratio.toFixed(3)}, the fog at its distance x ${m.T.toFixed(3)} (${(100 * d).toFixed(1)}% apart, at most ${100 * tol}%)`);
        } else if (!(m.err <= 3)) bad.push(`${tag} ${band}: ${m.err.toFixed(2)}/255 from the blend fogged at its distance (at most 3; coverage ${m.cover.toFixed(2)})`);
      }
      if (!measured) bad.push(`${tag}: drew nothing in either band`);
    }
  return bad;
}

/** the views of a picture (its size is the host's: 1280 x 720 natively, the cart's host size on romdev) */
const VIEWS = {
  plain: (img) => [{ x: 0, y: 0, w: img.w, h: img.h }],
  split: (img) => [{ x: 0, y: 0, w: img.w / 2, h: img.h }, { x: img.w / 2, y: 0, w: img.w / 2, h: img.h }],
};
const configs = [];
for (const p of PRESETS) configs.push({ tag: `analytic-${p}`, preset: p, env: {}, views: VIEWS.plain, control: p === 'high' || p === 'verylow' });
for (const p of PRESETS.filter((p) => p !== 'verylow' && p !== 'low')) configs.push({ tag: `vfog-${p}`, preset: p, env: { FT_VFOG: '1' }, views: VIEWS.plain, vfog: true, control: p === 'high' });
configs.push({ tag: 'split', preset: 'high', env: { FT_MODE: 'split' }, views: VIEWS.split, control: true });
configs.push({ tag: 'backdrop', preset: 'high', env: { FT_MODE: 'backdrop' }, views: VIEWS.plain, control: true });
configs.push({ tag: 'backdrop-vfog', preset: 'high', env: { FT_MODE: 'backdrop', FT_VFOG: '1' }, views: VIEWS.plain, vfog: true });

for (const host of HOSTS)
  for (const gpu of GPUS)
    for (const c of configs) {
      if (QUICK && host === 'romdev' && !['analytic-high', 'analytic-verylow', 'vfog-high', 'split', 'backdrop'].includes(c.tag)) continue;
      const tag = `${host} ${gpu} ${c.tag}`;
      let imgs, shown;
      try {
        shown = run(host, gpu, `${c.tag}-shown`, c.env, c.preset);
        imgs = [shown.img, run(host, gpu, `${c.tag}-hidden`, { ...c.env, FT_HIDE: '1' }, c.preset).img,
          run(host, gpu, `${c.tag}-shown0`, { ...c.env, FT_FOG: '0' }, c.preset).img,
          run(host, gpu, `${c.tag}-hidden0`, { ...c.env, FT_FOG: '0', FT_HIDE: '1' }, c.preset).img];
      } catch (e) {
        check(false, `${tag}: ${e.message}`);
        continue;
      }
      check(v(shown, 'ft.trans') === 1 && v(shown, 'ft.fog') === 1, `${tag}: the fog drew, then the transparent queue after it (ft.fog ${v(shown, 'ft.fog')}, ft.trans ${v(shown, 'ft.trans')})`);
      const bad = judge(imgs, c.views, c.vfog, c.preset === 'verylow' || c.preset === 'low');
      check(!bad.length, `${tag}: every subject fogged at its own distance${bad.length ? '\n       ' + bad.join('\n       ') : ''}`);
      if (!c.control) continue;
      for (const [cname, cenv] of [['over', { CW_TEST_FOG_OVER_TRANSPARENTS: '1' }], ['nohfog', { CW_TEST_FOG_NO_HFOG: '1' }]]) {
        if (cname === 'nohfog' && c.tag !== 'analytic-high') continue;
        let cb;
        try {
          const s = run(host, gpu, `${c.tag}-ctl-${cname}`, { ...c.env, ...cenv }, c.preset).img;
          const h = run(host, gpu, `${c.tag}-ctl-${cname}-hidden`, { ...c.env, ...cenv, FT_HIDE: '1' }, c.preset).img;
          cb = judge([s, h, imgs[2], imgs[3]], c.views, c.vfog, c.preset === 'verylow' || c.preset === 'low');
        } catch (e) {
          check(false, `${tag} CONTROL ${cname}: ${e.message}`);
          continue;
        }
        ctlRuns++;
        if (!cb.length) ctlFail++;
        check(cb.length > 0, `${tag}: CONTROL ${cname} (${Object.keys(cenv)[0]}) breaks it (${cb.length} problems: ${cb[0] || 'none'})`);
      }
    }
console.log(fail ? `fog-transparents: ${fail} FAILED` : `fog-transparents: all held (${ctlRuns} controls failed as they must)`);
process.exit(fail ? 1 : 0);
