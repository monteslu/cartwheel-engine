// TerrainScatter3D fade_mode DITHER's LOD crossfade per instance: never two LODs of one instance in one place, and what
// it costs against SHRINK. samples/foliage_field's grain scene (a meadow's GRASS: lod_distance 7, lod_blend 3, fade 6,
// wind, under TAA) with the camera running forward 4 m a second while turning (run), so instances cross the band on
// every frame; each measured frame the sample reads back every drawn instanced mesh (FOL_XF=1, xfade.* fields):
//   - coincident: no instance's root is held by two LODs' instance matrices in any frame (xfade.double 0). Two LODs of
//     one tuft at full size share blades a few millimetres apart: they fight for the depth test (a game's coplanar
//     check found 150 such pairs in its meadow). CONTROL: CW_TEST_SCATTER_XFADE=overlap (0.76.2 to 0.78.2: the next
//     LOD grew in inside each instance before the old one shrank away) must hold over 20 such instances in a frame.
//   - draws: the instances drawn with their roots in the crossfade band (lod_distance give or take lod_blend / 2),
//     where SHRINK draws each once, within 1% of SHRINK's; every instance drawn, and the instanced draws, within 1% and
//     10% of SHRINK's (the same layer with fade_mode SHRINK; DITHER drops an instance once it has shrunk away over the
//     fade, so it draws fewer). CONTROL: overlap draws over 5% more instances in the band than SHRINK.
//   - coverage: the band's instances' widths times heights (as scales), summed (how much of the band the grass covers: each instance
//     shrinks about its swap), within 10% of SHRINK's. CONTROL: CW_TEST_SCATTER_XFADE=swap:1:1 (each instance shrinking
//     to nothing across the whole band) covers under 0.9 of it.
//   - spacing: no instance in the band narrower than 0.79 of the layer's scale_min (the swap lowers an instance but
//     keeps 0.8 of its width: a tuft's blades a centimetre apart at their roots drawn a fifth as far apart are closer
//     than the depth buffer tells apart, a game's coplanar check found 28 pairs). CONTROL: CW_TEST_SCATTER_XFADE=
//     swap:0.8:0.15:1 (the same dip, uniform) narrows some under it.
//   - GPU time (a real GPU, native, --timing or by default off software): the pipeline's GPU ms of the field scene
//     (the GRASS field, 7 tufts a square metre out to 45 m, TAA) with DITHER within 5% and 0.1 ms of SHRINK's, median
//     of interleaved rounds.
//   node tests/foliage-xfade.mjs [--hosts native,romdev] [--gpu gl,webgpu] [--frames N] [--timing|--no-timing]
import { runField } from './foliage-runs.mjs';

const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const SOFTWARE = process.env.CW_TEST_GPU === 'software';
const HOSTS = arg('--hosts', SOFTWARE ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
const FRAMES = +arg('--frames', SOFTWARE ? 90 : 150);
const TIMING = process.argv.includes('--timing') || (!SOFTWARE && !process.argv.includes('--no-timing'));
const ROUNDS = +arg('--rounds', 3);
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}
let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const BASE = { FOL_SCENE: 'grain', FOL_TAA: '1', FOL_WIND: '0.07', FOL_SHROOT: '1', FOL_PAN: '10', FOL_FROM: '40', FOL_CAM: 'run', FOL_XF: '1' };
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)]; };

for (const host of HOSTS)
  for (const gpu of GPUS) {
    const tag = `${host} ${gpu}`;
    try {
      const run = (env, name) => runField(host, gpu, FRAMES, { ...BASE, ...env }, `xfade-${name}`, { shot: false }).fields;
      const fix = run({ FOL_FADE: 'dither' }, 'fix'), ctl = run({ FOL_FADE: 'dither', CW_TEST_SCATTER_XFADE: 'overlap' }, 'control');
      const ref = run({ FOL_FADE: 'shrink' }, 'shrink'), thin = run({ FOL_FADE: 'dither', CW_TEST_SCATTER_XFADE: 'swap:1:1' }, 'thin');
      const narrow = run({ FOL_FADE: 'dither', CW_TEST_SCATTER_XFADE: 'swap:0.8:0.15:1' }, 'narrow');
      check([fix, ctl, ref].every((f) => f['render.skipped'] === 0 && f['xfade.inst'] > 500), `${tag}: measured (${fix['xfade.inst'].toFixed(0)} instances a frame), nothing skipped`);
      check(fix['xfade.double'] === 0, `${tag}: no instance drawn by two LODs in any frame (${fix['xfade.double']})`);
      check(ctl['xfade.double'] > 20, `${tag}: CONTROL: overlap draws ${ctl['xfade.double']} instances in two LODs at once (over 20)`);
      const ri = fix['xfade.inst'] / ref['xfade.inst'], rd = fix['xfade.draws'] / ref['xfade.draws'];
      const rb = fix['xfade.band'] / ref['xfade.band'], rc = ctl['xfade.band'] / ref['xfade.band'];
      check(ref['xfade.band'] > 100 && rb < 1.01, `${tag}: instances a frame in the crossfade band ${fix['xfade.band'].toFixed(0)}, ${rb.toFixed(4)}x SHRINK's ${ref['xfade.band'].toFixed(0)} (under 1.01)`);
      check(ri < 1.01, `${tag}: instances a frame ${fix['xfade.inst'].toFixed(0)}, ${ri.toFixed(4)}x SHRINK's ${ref['xfade.inst'].toFixed(0)} (under 1.01)`);
      check(rd < 1.1, `${tag}: instanced draws a frame ${fix['xfade.draws'].toFixed(1)}, ${rd.toFixed(3)}x SHRINK's ${ref['xfade.draws'].toFixed(1)} (under 1.1)`);
      const ra = fix['xfade.area'] / ref['xfade.area'], rt = thin['xfade.area'] / ref['xfade.area'];
      check(ra > 0.9, `${tag}: the grass covers ${ra.toFixed(3)} of SHRINK's in the crossfade band (over 0.9)`);
      check(rt < 0.9, `${tag}: CONTROL: shrinking to nothing across the band covers ${rt.toFixed(3)} of it (under 0.9)`);
      check(fix['xfade.min_wide'] > 0.79, `${tag}: the narrowest instance in the band ${fix['xfade.min_wide'].toFixed(3)} of scale_min (over 0.79)`);
      check(narrow['xfade.min_wide'] < 0.79, `${tag}: CONTROL: a uniform dip narrows one to ${narrow['xfade.min_wide'].toFixed(3)} (under 0.79)`);
      check(rc > 1.05, `${tag}: CONTROL: overlap draws ${rc.toFixed(4)}x SHRINK's instances in the crossfade band (over 1.05)`);
    } catch (e) { check(false, `${tag}: ${e.message}`); }
  }
if (TIMING && HOSTS.includes('native'))
  for (const gpu of GPUS) {
    const tag = `native ${gpu} timing`;
    try {
      const F = { FOL_SCENE: 'field', FOL_TAA: '1', FOL_FROM: '60' }, ms = { shrink: [], dither: [] };
      const prev = process.env.CW_GPU_TIMERS;
      process.env.CW_GPU_TIMERS = '1';
      for (let r = 0; r < ROUNDS; r++)
        for (const m of ['shrink', 'dither']) ms[m].push(runField('native', gpu, 240, { ...F, FOL_FADE: m }, `xfade-time-${m}`, { shot: false }).fields['fol.gpu_ms']);
      if (prev === undefined) delete process.env.CW_GPU_TIMERS; else process.env.CW_GPU_TIMERS = prev;
      const s = median(ms.shrink), d = median(ms.dither);
      check(s > 0 && d <= s * 1.05 + 0.1, `${tag}: the field's GPU ms with DITHER ${d.toFixed(3)} against SHRINK ${s.toFixed(3)} (within 5% and 0.1 ms; rounds ${ms.dither.map((x) => x.toFixed(2)).join(' ')} / ${ms.shrink.map((x) => x.toFixed(2)).join(' ')})`);
    } catch (e) { check(false, `${tag}: ${e.message}`); }
  }
console.log(fail ? `foliage-xfade: ${fail} failures` : 'ok   foliage-xfade');
process.exit(fail ? 1 : 0);
