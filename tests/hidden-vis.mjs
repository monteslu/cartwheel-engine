// The visibility rule (cw_node_visible_in_tree, cartwheel/scene.h) for every node type that draws or runs per frame
// outside the mesh path: samples/hidden_vis puts each under one hidden Node3D (the 2D particles under a hidden
// Node2D). Per host and backend:
//   - hidden draws the picture absent draws (the groups freed), pixel for pixel, and three.c issues the same draws;
//   - hidden costs nothing: per type, its counter stays at absent's (no particle step or draw, no decal drawn or clipped,
//     no cascade, no fog volume or fog light, no shockwave or explosion time, no water surface, no probe capture,
//     no cloth step, no 2D particle, no SubViewport render, never on screen);
//   - the same with each node hidden by its own `visible` instead (HV_MODE=self, the groups shown);
//   - CONTROLS: the ancestor case with CW_TEST_HIDDEN_SELF_ONLY=1 (ancestors ignored, the old rule) and the own case
//     with CW_TEST_HIDDEN_PARENTS_ONLY=1 (a node's own `visible` ignored) must each fail the picture and every type's
//     check: each check can see its type;
//   - shown again at tick HV_SHOW (show) everything draws and runs, and resumes from the state it was hidden with:
//     the particles' and the shockwave's clocks equal those of a run shown from the start, shifted by the hidden ticks.
//   node tests/hidden-vis.mjs [--hosts native,romdev] [--gpu gl,webgpu]   (romdev runs the dual cart, build/cart-wgpu)
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'hidden-vis');
mkdirSync(OUT, { recursive: true });
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
// (CI, CW_TEST_GPU=software, has no romdev: native alone there)
const HOSTS = arg('--hosts', process.env.CW_TEST_GPU === 'software' ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
const FRAMES = 240, SHOW = 90;
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

function run(host, gpu, name, env, frames = FRAMES) {
  const png = join(OUT, `${name}.${host}.${gpu}.png`), st = join(OUT, `${name}.${host}.${gpu}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'hidden_vis', '--frames', String(frames),
    '--gpu', gpu, '--shot', png, '--state', st, '--env', JSON.stringify(env)], { encoding: 'utf8', env: process.env });
  if (r.status !== 0) throw new Error(`${host} ${gpu} ${name}: ${((r.stderr || '') + (r.stdout || '')).trim().split('\n').slice(-3).join(' | ')}`);
  const s = JSON.parse(readFileSync(st, 'utf8'));
  return { img: readImage(png), s: s.fields || s };
}
const v = (r, k) => { const x = r.s[`hv.${k}`]; return typeof x === 'object' && x !== null ? x.value : x; };
function diff(a, b) {
  let n = 0;
  for (let i = 0; i < a.rgba.length; i += 4)
    if (a.rgba[i] !== b.rgba[i] || a.rgba[i + 1] !== b.rgba[i + 1] || a.rgba[i + 2] !== b.rgba[i + 2]) n++;
  return n;
}

// per type: the counters a hidden node must leave where absent leaves them
const TYPES = [
  ['GPUParticles3D + Trail3D', ['p3_steps', 'p3_draws', 'flames_alive']],
  ['Decal3D (screen)', ['dec_drawn', 'dec_draws']],
  ['Decal3D (mesh)', ['dec_builds', 'dec_tris']],
  ['DirectionalLight3D shadow cascades', ['cascades']],
  ['FogVolume3D', ['fog_volumes']],
  ['shadowed lights in the volumetric fog', ['fog_shadowed']], ['lights in the volumetric fog (at its cap here, so no control)', ['fog_lights']],
  ['Shockwave3D', ['shock_age_ms']],
  ['Explosion3D', ['boom_flashes']],
  ['Water3D', ['water_surfaces']],
  ['ReflectionProbe3D + LightProbeGrid3D', ['gi_captures', 'gi_grid']],
  ['Cloth3D', ['cloth_hash']],
  ['GPUParticles2D', ['p2_alive']],
  ['SubViewport', ['sv_renders']],
  ['Backdrop', ['bd_renders']],
  ['VisibleOnScreenNotifier3D', ['on_screen']],
  ['three.c draws', ['draws']],
];
// shown again: what must have run (the counter moved past absent's)
const SHOWN = ['p3_steps', 'p3_draws', 'flames_alive', 'dec_drawn', 'dec_builds', 'cascades', 'fog_volumes', 'shock_age_ms',
  'boom_flashes', 'water_surfaces', 'gi_captures', 'cloth_hash', 'p2_alive', 'sv_renders', 'bd_renders', 'on_screen'];

let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
for (const host of HOSTS)
  for (const gpu of GPUS) {
    const tag = `${host} ${gpu}`;
    let absent, show, vis;
    try {
      absent = run(host, gpu, 'absent', { HV_MODE: 'absent' });
      show = run(host, gpu, 'show', { HV_MODE: 'show', HV_SHOW: String(SHOW) });
    } catch (e) {
      check(false, `${tag}: ${e.message}`);
      continue;
    }
    // hidden by an ancestor (the groups), then by each node's own `visible` (the groups shown); each with its control
    const CASES = [
      { what: 'hidden by an ancestor', mode: 'hidden', ctl: { CW_TEST_HIDDEN_SELF_ONLY: '1' }, ctlWhat: 'self-only visibility (ancestors ignored)' },
      { what: 'hidden by its own visible', mode: 'self', ctl: { CW_TEST_HIDDEN_PARENTS_ONLY: '1' }, ctlWhat: 'parents-only visibility (its own ignored)',
        noOwn: ['SubViewport', 'Backdrop'] }, // plain Nodes with no `visible` of their own: freed in this mode
    ];
    let hidden;
    for (const c of CASES) {
      let h, ctl;
      try {
        h = run(host, gpu, c.mode, { HV_MODE: c.mode });
        ctl = run(host, gpu, `${c.mode}-control`, { HV_MODE: c.mode, ...c.ctl });
      } catch (e) {
        check(false, `${tag}: ${c.what}: ${e.message}`);
        continue;
      }
      if (c.mode === 'hidden') hidden = h;
      const pd = diff(h.img, absent.img), cd = diff(ctl.img, absent.img);
      check(pd === 0, `${tag}: ${c.what}: draws the picture absent draws (${pd} pixels differ)`);
      check(cd > 0, `${tag}: CONTROL, ${c.what}: ${c.ctlWhat} changes the picture (${cd} pixels differ)`);
      check(v(h, 'p3_hidden') === 2 && v(h, 'dec_hidden') === 2 && v(h, 'water_hidden') === 1 && v(h, 'gi_hidden') === 2,
        `${tag}: ${c.what}: the stats count what is hidden (particles ${v(h, 'p3_hidden')}/2, decals ${v(h, 'dec_hidden')}/2, water ${v(h, 'water_hidden')}/1, probes ${v(h, 'gi_hidden')}/2)`);
      for (const [type, keys] of TYPES) {
        const same = keys.every((k) => v(h, k) === v(absent, k));
        const vals = keys.map((k) => `${k} ${v(h, k)} (absent ${v(absent, k)}, control ${v(ctl, k)})`).join(', ');
        check(same, `${tag}: ${c.what}: ${type} costs nothing: ${vals}`);
        if (type !== 'three.c draws' && !type.includes('no control') && !(c.noOwn || []).includes(type))
          check(!keys.every((k) => v(ctl, k) === v(absent, k)), `${tag}: CONTROL, ${c.what}: ${type}: ${c.ctlWhat} runs it`);
      }
    }
    if (hidden) {
      const sd = diff(show.img, hidden.img);
      check(sd > 0, `${tag}: shown again, it draws (${sd} pixels differ from hidden)`);
    }
    for (const k of SHOWN) check(v(show, k) !== v(absent, k) && v(show, k) !== 0, `${tag}: shown again, ${k} runs (${v(show, k)})`);
    // resumed, not restarted or fast-forwarded: the clocks match a run shown from the start, shifted by the hidden ticks
    try {
      const vis0 = run(host, gpu, 'visible-probe', { HV_MODE: 'visible' }, 8);
      const shift = v(show, 'shown_tick') - v(vis0, 'shown_tick');
      vis = run(host, gpu, 'visible', { HV_MODE: 'visible' }, FRAMES - shift);
      for (const k of ['flames_alive', 'shock_age_ms'])
        check(v(show, k) === v(vis, k), `${tag}: shown at tick ${v(show, 'shown_tick')}, ${k} resumes where it stopped (${v(show, k)}; shown from tick ${v(vis, 'shown_tick')}, ${shift} frames fewer: ${v(vis, k)})`);
    } catch (e) {
      check(false, `${tag}: ${e.message}`);
    }
  }
console.log(fail ? `hidden-vis: ${fail} failures` : 'ok   hidden-vis');
process.exit(fail ? 1 : 0);
