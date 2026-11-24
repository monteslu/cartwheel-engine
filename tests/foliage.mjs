// Foliage (cartwheel/foliage.h, TerrainScatter3D's foliage options), measured from the picture of samples/foliage_field
// (its fol.* debug fields: tests/foliage-runs.mjs), per host and backend, each check with a control that must fail:
//
//   wind      thin stems before a plain background, each tip tracked from the picture over four seconds: at strength
//             0.1 the tips travel 0.03 to 0.11 m and at 0.2 twice that (1.6x to 2.4x); with no gusts, neighbouring stems
//             sway out of phase (mean correlation of their tracks under 0.6). CONTROL: CW_TEST_FOLIAGE_LOCKSTEP=1 (every
//             stem in one phase) must correlate (over 0.9). Strength 0 with the part on (two_sided_lighting on the
//             front-sided stems) draws the picture with no foliage at all byte for byte; CONTROL: strength 0.1 changes it.
//   alpha     a field of alpha-tested grass cards under a slow pan (2 degrees a second): shimmer, the share of pixels that
//             change by more than 0.1 against the picture 11 frames before moved by the pan. smooth_alpha must cut it by a
//             fifth or more under MSAA 4x (alpha to coverage) and under TAA without MSAA (hashed alpha). CONTROL: the plain
//             alpha test (its numbers are the baseline).
//   twosided  two upright double-sided cards with straight-up normals, one seen from its front and one from its back:
//             with two_sided_lighting their brightness is within 0.02. CONTROL: without it the back is darker by 0.1+.
//   tuft      the built-in GRASS: per-instance colour variance above 0; with root_from_terrain every instance's colour
//             within a log ratio of 0.45 of the terrain's under it (the 0.25 jitter); and from 12 m above, the dirt disc's
//             red over green with grass is nearer the bare dirt's than without root_from_terrain. CONTROL: without
//             root_from_terrain the colours are far from the terrain's (log ratio above 1).
//   lod       a dolly 1 m a second over cards whose LOD 1 is a single card, through the switch at 14 m: the largest
//             single-frame change of a 20 x 8 pixel block of the rows 11 to 17 m away, the run less its reference (LOD 0
//             to the end), with fade_mode DITHER must be under 0.6 of SHRINK's. CONTROL: SHRINK (its number).
//
//   node tests/foliage.mjs [--hosts native,romdev] [--gpu gl,webgpu] [--only wind,alpha,...]
import { mkdirSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { runField } from './foliage-runs.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
mkdirSync(join(ROOT, 'out', 'foliage'), { recursive: true });
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const HOSTS = arg('--hosts', process.env.CW_TEST_GPU === 'software' ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
/* CI renders on a software GPU (llvmpipe and lavapipe, CW_TEST_GPU=software), where the whole set ran past a CI
 * share's 90 minutes (v0.71.1): the alpha and lod checks draw a dense field of alpha-tested cards (4 a square metre
 * out to 40 m) at 1080p, 2,600 frames a backend. There the pan runs ALPHA_FRAMES frames instead of 200 (the same checks
 * and controls over fewer frames) and the LOD dolly (1,800 frames a backend) runs on a real GPU only (--only lod runs
 * it on software too) */
const SOFTWARE = process.env.CW_TEST_GPU === 'software';
const ALPHA_FRAMES = +arg('--alpha-frames', SOFTWARE ? 110 : 200);
const ONLY = arg('--only', SOFTWARE ? 'wind,alpha,twosided,tuft' : 'wind,alpha,twosided,tuft,lod').split(',');
if (SOFTWARE && !ONLY.includes('lod')) console.log('skip lod: local only (a real GPU; the software GPU takes too long)');
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const diff = (a, b) => { let n = 0; for (let i = 0; i < a.rgba.length; i += 4) if (a.rgba[i] !== b.rgba[i] || a.rgba[i + 1] !== b.rgba[i + 1] || a.rgba[i + 2] !== b.rgba[i + 2]) n++; return n; };
const f3 = (x) => (typeof x === 'number' ? x.toFixed(4) : String(x));

for (const host of HOSTS)
  for (const gpu of GPUS) {
    const tag = `${host} ${gpu}`;
    const step = (what, fn) => { try { fn(); } catch (e) { check(false, `${tag}: ${what}: ${e.message}`); } };
    const run = (frames, env, name, o) => runField(host, gpu, frames, env, name, o);
    if (ONLY.includes('wind'))
      step('wind', () => {
        const W = { FOL_SCENE: 'wind', FOL_FROM: '60' };
        const a1 = run(330, { ...W, FOL_WIND: '0.1' }, 'wind1').fields, a2 = run(330, { ...W, FOL_WIND: '0.2' }, 'wind2').fields;
        check(a1['fol.tips'] >= 3 && a1['fol.tip_amp'] > 0.03 && a1['fol.tip_amp'] < 0.11,
          `${tag}: wind 0.1: ${a1['fol.tips']} tips travel ${f3(a1['fol.tip_amp'])} m (0.03 to 0.11)`);
        const ratio = a2['fol.tip_amp'] / Math.max(a1['fol.tip_amp'], 1e-6);
        check(ratio > 1.6 && ratio < 2.4, `${tag}: wind 0.2: ${f3(a2['fol.tip_amp'])} m, ${ratio.toFixed(2)}x strength 0.1's`);
        const p = run(330, { ...W, FOL_WIND: '0.1', FOL_GUST: '0' }, 'windphase').fields;
        const c = run(330, { ...W, FOL_WIND: '0.1', FOL_GUST: '0', CW_TEST_FOLIAGE_LOCKSTEP: '1' }, 'windlock').fields;
        check(p['fol.tips'] >= 3 && p['fol.tip_corr'] < 0.6, `${tag}: neighbouring stems out of phase (correlation ${f3(p['fol.tip_corr'])} < 0.6)`);
        check(c['fol.tip_corr'] > 0.9, `${tag}: CONTROL: in lockstep they correlate (${f3(c['fol.tip_corr'])} > 0.9)`);
        const none = readImage(run(90, W, 'wind-none').png);
        const zero = readImage(run(90, { ...W, FOL_WIND: '0', FOL_TWOSIDED: '1' }, 'wind-zero').png);
        const on = readImage(run(90, { ...W, FOL_WIND: '0.1' }, 'wind-on').png);
        const dz = diff(none, zero), don = diff(none, on);
        check(dz === 0, `${tag}: strength 0 with the foliage part on draws the picture without it byte for byte (${dz} pixels differ)`);
        check(don > 0, `${tag}: CONTROL: strength 0.1 changes it (${don} pixels)`);
      });
    if (ONLY.includes('alpha'))
      step('alpha', () => {
        const C = { FOL_SCENE: 'cards', FOL_CAM: 'pan' };
        for (const [mode, env] of [['MSAA 4x (alpha to coverage)', { FOL_MSAA: '4' }], ['TAA (hashed alpha)', { FOL_TAA: '1' }]]) {
          const plain = run(ALPHA_FRAMES, { ...C, ...env }, 'alpha-plain', { shot: false }).fields['fol.shimmer'];
          const smooth = run(ALPHA_FRAMES, { ...C, ...env, FOL_ALPHA: 'smooth' }, 'alpha-smooth', { shot: false }).fields['fol.shimmer'];
          check(plain > 0.005, `${tag}: CONTROL: ${mode}: the plain alpha test shimmers (${f3(plain)})`);
          check(smooth < plain * 0.8, `${tag}: ${mode}: smooth_alpha shimmer ${f3(smooth)} vs plain ${f3(plain)} (${((1 - smooth / plain) * 100).toFixed(0)}% less, 20% at least)`);
        }
      });
    if (ONLY.includes('twosided'))
      step('twosided', () => {
        const on = run(60, { FOL_SCENE: 'twosided', FOL_TWOSIDED: '1' }, 'two-on').fields;
        const off = run(60, { FOL_SCENE: 'twosided' }, 'two-off').fields;
        check(Math.abs(on['fol.front'] - on['fol.back']) < 0.02 && on['fol.front'] > 0.1,
          `${tag}: two_sided_lighting: front ${f3(on['fol.front'])}, back ${f3(on['fol.back'])} (within 0.02)`);
        check(off['fol.front'] - off['fol.back'] > 0.1, `${tag}: CONTROL: the flip darkens the back (front ${f3(off['fol.front'])}, back ${f3(off['fol.back'])})`);
      });
    if (ONLY.includes('tuft'))
      step('tuft', () => {
        const root = run(60, { FOL_SCENE: 'root', FOL_ROOT: '1' }, 'tuft-root').fields;
        const plain = run(60, { FOL_SCENE: 'root' }, 'tuft-plain').fields;
        const bare = run(60, { FOL_SCENE: 'root', FOL_NOGRASS: '1' }, 'tuft-bare').fields;
        check(plain['fol.instances'] > 100 && plain['fol.color_var'] > 1e-4, `${tag}: per-instance colour variance ${plain['fol.color_var']} over ${plain['fol.instances']} tufts`);
        check(root['fol.color_var'] > 0, `${tag}: with root_from_terrain still varied (${root['fol.color_var']})`);
        check(root['fol.root_err'] < 0.45, `${tag}: root colours within ${f3(root['fol.root_err'])} (log ratio, under 0.45) of the terrain's under them`);
        check(plain['fol.root_err'] > 1, `${tag}: CONTROL: without root_from_terrain ${f3(plain['fol.root_err'])} from the terrain's`);
        const dr = Math.abs(root['fol.disc_rg'] - bare['fol.disc_rg']), dp = Math.abs(plain['fol.disc_rg'] - bare['fol.disc_rg']);
        check(dr < dp, `${tag}: the grass on the dirt disc reads nearer the dirt with root_from_terrain (red/green ${f3(root['fol.disc_rg'])}, bare ${f3(bare['fol.disc_rg'])}, without ${f3(plain['fol.disc_rg'])})`);
      });
    if (ONLY.includes('lod'))
      step('lod', () => {
        const D = { FOL_SCENE: 'cards', FOL_CAM: 'dolly', FOL_TAA: '1' };
        const N = 600, B = 96 * 8;
        const band = (env, name) => run(N, { ...D, ...env }, name, { shot: false }).fields['fol.band'];
        const ref = band({ FOL_LODREF: '1' }, 'lod-ref');
        // (a WebGPU read lands a frame or more late: the last frames of a run may hold no picture; those are left out)
        const has = (x, t) => { for (let k = 0; k < B; k++) if (x[t * B + k]) return true; return false; };
        const jump = (a) => {
          let mx = 0;
          for (let t = 41; t < N; t++) {
            if (!has(a, t) || !has(a, t - 1) || !has(ref, t) || !has(ref, t - 1)) continue;
            for (let k = 0; k < B; k++) mx = Math.max(mx, Math.abs((a[t * B + k] - ref[t * B + k]) - (a[(t - 1) * B + k] - ref[(t - 1) * B + k])));
          }
          return mx;
        };
        const s = jump(band({}, 'lod-shrink')), d = jump(band({ FOL_FADE: 'dither' }, 'lod-dither'));
        check(s >= 12, `${tag}: CONTROL: SHRINK pops: a block of the LOD band jumps ${s}/255 in one frame`);
        check(d < s * 0.6, `${tag}: DITHER crossfades: the largest jump ${d}/255 (SHRINK ${s}/255)`);
      });
  }
console.log(fail ? `foliage: ${fail} failures` : 'ok   foliage');
process.exit(fail ? 1 : 0);
