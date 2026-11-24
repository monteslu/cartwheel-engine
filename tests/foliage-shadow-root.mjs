// One shadow sample per tuft (cartwheel/foliage.h shadow_at_root, TerrainScatter3D shadow_at_root), measured from the
// picture of samples/foliage_field's shadow scene (shadow.* debug fields): the GRASS field on flat ground under TAA, the
// sun low from one side behind a wall, whose shadow edge (two cascades over 40 m) runs straight out from the camera
// through the grass. The camera holds still for 70 frames; each pixel's temporal standard deviation of brightness is
// averaged over the ground within 0.6 m of the edge (5 to 12 m ahead), over 1.4 to 2.6 m into the lit side and as far
// into the shadow. The edge's excess noise: the edge band's less the mean of the other two (what the edge adds to the
// grass's own flicker). Per host and backend, with wind 0.08 and with none (the jitter alone):
//   shadow_at_root keeps the excess under 0.3 of the per-fragment shadow's. CONTROL: shadow_at_root off, whose excess
//   must show the speckled edge (over 0.002 with wind, 0.0003 without).
//   node tests/foliage-shadow-root.mjs [--hosts native,romdev] [--gpu gl,webgpu]
import { runField } from './foliage-runs.mjs';

const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const SOFTWARE = process.env.CW_TEST_GPU === 'software';
const HOSTS = arg('--hosts', SOFTWARE ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}
let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const f5 = (x) => (typeof x === 'number' ? x.toFixed(5) : String(x));
const excess = (f) => f['shadow.edge'] - (f['shadow.lit'] + f['shadow.dark']) / 2;

for (const host of HOSTS)
  for (const gpu of GPUS) {
    const tag = `${host} ${gpu}`;
    for (const [what, env, floor] of [['wind 0.08', { FOL_WIND: '0.08' }, 0.002], ['no wind', {}, 0.0003]]) {
      try {
        const S = { FOL_SCENE: 'shadow', FOL_TAA: '1', ...env };
        const off = runField(host, gpu, 100, S, 'shroot-off', { shot: false }).fields;
        const on = runField(host, gpu, 100, { ...S, FOL_SHROOT: '1' }, 'shroot-on', { shot: false }).fields;
        const eo = excess(off), en = excess(on);
        check(off['shadow.n'] >= 60 && off['render.skipped'] === 0 && on['render.skipped'] === 0, `${tag}: ${what}: ${off['shadow.n']} frames measured, nothing skipped`);
        check(eo > floor, `${tag}: ${what}: CONTROL: a shadow per fragment speckles the edge (excess ${f5(eo)}: edge ${f5(off['shadow.edge'])}, lit ${f5(off['shadow.lit'])}, shadow ${f5(off['shadow.dark'])}; over ${floor})`);
        check(en < 0.3 * eo, `${tag}: ${what}: shadow_at_root: excess ${f5(en)} (edge ${f5(on['shadow.edge'])}), under 0.3 of ${f5(eo)}`);
      } catch (e) { check(false, `${tag}: ${what}: ${e.message}`); }
    }
  }
console.log(fail ? `foliage-shadow-root: ${fail} failures` : 'ok   foliage-shadow-root');
process.exit(fail ? 1 : 0);
