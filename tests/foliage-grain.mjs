// The grain of grass fades under TAA in motion (TerrainScatter3D fade_mode DITHER, cartwheel/foliage.h fades), measured
// from the picture of samples/foliage_field's grain scene (grain.* debug fields): the GRASS layer as a third-person
// game's meadow (visible_range 16, fade 6, lod_distance 7, lod_blend 3, wind 0.07, shadow_at_root) under TAA, the sun
// low behind a wall whose shadow edge runs out from the camera, the camera running forward 4 m a second while turning
// 10 degrees a second (run), or turning alone (pan). Each picture's ground pixels 5.5 to 8.5 m (the LOD crossfade) and
// 10 to 15 m (the fade) from the camera are found by casting them onto the ground with the camera that drew it; in them
// the spatial high frequency (|brightness - its 3 x 3 mean|) and its change from the picture before (temporal), on the
// shadow's edge (lit blades against shadowed ground), in the shadow and in the light. Each against the same run with
// fade_mode SHRINK (no dither at all): what the dither adds.
//   The per-instance dither adds under 0.35 of what the per-pixel screen-door added, every measure, and so does an
//   instanced mesh's own fade (the shader's per-instance path: CW_TEST_FOLIAGE_PIXEL_DITHER=instance, the scatter's
//   bands drawn by the material). CONTROL: CW_TEST_FOLIAGE_PIXEL_DITHER=1, the screen-door of 0.76.1 and before, must add grain
//   (edge spatial over 0.0006, edge temporal over 0.0008, light spatial over 0.001).
//   node tests/foliage-grain.mjs [--hosts native,romdev] [--gpu gl,webgpu] [--cams run,pan] [--frames N]
//   A layer a scene file holds (FOL_FILE=1: the field scene's GRASS layer with DITHER fades as main.cwscene makes it
//   on frame 0, where cw_test_env answers nothing, no option set on it later): the control switch must still reach it,
//   so CW_TEST_FOLIAGE_PIXEL_DITHER=1 changes the picture (over 0.2% of its pixels by more than 8/255; 0.77.0 read the
//   switch once when the layer was made and drew the two byte for byte alike).
import { readImage } from '../tools/lib/png.mjs';
import { runField } from './foliage-runs.mjs';

const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const SOFTWARE = process.env.CW_TEST_GPU === 'software';
const HOSTS = arg('--hosts', SOFTWARE ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
const CAMS = arg('--cams', 'run,pan').split(',').filter((c) => c && c !== '-'); // (--cams - : the file leg alone)
const FRAMES = +arg('--frames', SOFTWARE ? 90 : 120);
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}
let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const f5 = (x) => (typeof x === 'number' ? x.toFixed(5) : String(x));
const KEYS = ['edge_hf', 'edge_t', 'dark_hf', 'dark_t', 'lit_hf', 'lit_t'];
const BASE = { FOL_SCENE: 'grain', FOL_TAA: '1', FOL_WIND: '0.07', FOL_FADE: 'dither', FOL_SHROOT: '1', FOL_PAN: '10', FOL_FROM: '40' };
const VARIANTS = { fix: {}, shader: { CW_TEST_FOLIAGE_PIXEL_DITHER: 'instance' }, control: { CW_TEST_FOLIAGE_PIXEL_DITHER: '1' }, ref: { FOL_FADE: 'shrink' } };

for (const host of HOSTS)
  for (const gpu of GPUS)
    for (const cam of CAMS) {
      const tag = `${host} ${gpu} ${cam}`;
      try {
        const R = {};
        for (const [v, env] of Object.entries(VARIANTS)) R[v] = runField(host, gpu, FRAMES, { ...BASE, FOL_CAM: cam, ...env }, `grain-${cam}-${v}`, { shot: false }).fields;
        const n = Math.min(...Object.values(R).map((f) => f['grain.n']));
        check(n >= 40 && Object.values(R).every((f) => f['render.skipped'] === 0), `${tag}: ${n} pictures measured a run, nothing skipped`);
        const add = (v, k) => R[v]['grain.' + k] - R.ref['grain.' + k];
        console.log(`     ${tag}: against plain lit grass (edge / light): spatial ${['fix', 'control', 'ref'].map((v) => `${v} ${(R[v]['grain.edge_hf'] / R[v]['grain.lit_hf']).toFixed(3)}`).join(', ')}; temporal ${['fix', 'control', 'ref'].map((v) => `${v} ${(R[v]['grain.edge_t'] / R[v]['grain.lit_t']).toFixed(3)}`).join(', ')}`);
        check(add('control', 'edge_hf') > 0.0006 && add('control', 'edge_t') > 0.0008 && add('control', 'lit_hf') > 0.001,
          `${tag}: CONTROL: the per-pixel screen-door adds grain (edge spatial ${f5(add('control', 'edge_hf'))}, temporal ${f5(add('control', 'edge_t'))}, light spatial ${f5(add('control', 'lit_hf'))})`);
        for (const v of ['fix', 'shader'])
          for (const k of KEYS) {
            const c = add('control', k), a = add(v, k);
            check(a < 0.35 * c, `${tag}: ${v === 'fix' ? 'scatter per instance' : 'shader per instance'}: ${k} adds ${f5(a)} (${f5(R[v]['grain.' + k])} against ${f5(R.ref['grain.' + k])}), under 0.35 of the screen-door's ${f5(c)}`);
          }
      } catch (e) { check(false, `${tag}: ${e.message}`); }
    }
/** the share of pixels two pictures differ in by more than 8/255 on some channel */
function changed(a, b) {
  const A = readImage(a), B = readImage(b);
  if (A.w !== B.w || A.h !== B.h) return 1;
  let n = 0;
  for (let i = 0; i < A.w * A.h; i++)
    for (let k = 0; k < 3; k++) if (Math.abs(A.rgba[4 * i + k] - B.rgba[4 * i + k]) > 8) { n++; break; }
  return n / (A.w * A.h);
}
for (const host of HOSTS)
  for (const gpu of GPUS) {
    const tag = `${host} ${gpu} file`;
    try {
      const env = { FOL_FILE: '1', FOL_TAA: '1' };
      const fix = runField(host, gpu, 60, env, 'grain-file-fix'), ctl = runField(host, gpu, 60, { ...env, CW_TEST_FOLIAGE_PIXEL_DITHER: '1' }, 'grain-file-control');
      check(fix.fields['render.skipped'] === 0 && ctl.fields['render.skipped'] === 0, `${tag}: nothing skipped`);
      const c = changed(fix.png, ctl.png);
      check(c > 0.002, `${tag}: a layer from the scene file takes the control switch: CW_TEST_FOLIAGE_PIXEL_DITHER=1 changes ${(100 * c).toFixed(3)}% of the pixels (over 0.2%)`);
    } catch (e) { check(false, `${tag}: ${e.message}`); }
  }
console.log(fail ? `foliage-grain: ${fail} failures` : 'ok   foliage-grain');
process.exit(fail ? 1 : 0);
