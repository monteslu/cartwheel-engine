// A reversed depth buffer (cw_app_desc.reversed_depth, CW_REVERSED_DEPTH=1) against forward
// depth on WebGPU: every sample that draws 3D, with shadows, sky, fog, water, particles, trails,
// decals, impostors, screen-space effects and viewmodels between them, must draw the same picture
// with no program missing and no GPU error. Depth only changes how distance is stored; a pixel
// that moves is a pass still assuming forward depth. Control: CW_TEST_BREAK_REVERSED=1 leaves
// the engine's own WGSL on forward depth under a reversed buffer, and must fail.
//
//   node tests/reversed-depth.mjs [app ...]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { readImage } from '../tools/lib/png.mjs';
await enterGpuSandbox();
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'reversed');
mkdirSync(OUT, { recursive: true });
const APPS = ['scene3d', 'gi_room', 'gi_probe_check', 'gi_shadow', 'lightmap_room', 'lightmap_yard', 'decal_walls', 'fog_shafts', 'fog_valley',
  'fx_pipeline', 'fx_boom_water', 'fx_distort', 'fx_explosion', 'fx_lit_decals', 'fx_mesh_decals', 'fx_decals', 'fx_particles', 'fx_bursts',
  'gltf_scene', 'night_street', 'street_sun', 'sky_day', 'physical_materials', 'post_stack', 'rim_light', 'showcase', 'msaa_hdr', 'character_skin',
  'cloth_demo', 'csg_greybox', 'arena_game', 'terrain_demo', 'open_world', 'vis_city', 'vis_impostor', 'water_boat', 'water_ripples', 'water_shore',
  'viewmodel_demo', 'anim_skinned', 'post_synth'];
const want = process.argv.slice(2);
const apps = want.length ? want : APPS;
const TOL = 16, MAX_SHARE = 0.002;
// the fountain bounces off the scene's depth: rounding in the distance grows bounce by bounce (with
// collide_depth off this sample is 0.000%)
const ALLOW = { fx_particles: 0.01 };
let failed = false;
const report = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failed = true; };
function run(app, tag, env, host = 'native') {
  const shot = join(OUT, `${app}.${tag}.png`), state = join(OUT, `${app}.${tag}.json`);
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--gpu', 'webgpu', '--app', app, '--frames', '120', '--shot', shot, '--state', state],
    { encoding: 'utf8', env: { ...process.env, ...env } });
  if (r.status !== 0) return { err: (r.stderr || r.stdout).trim().split('\n').slice(-2).join(' | ').slice(0, 300) };
  return { img: readImage(shot), state: JSON.parse(readFileSync(state, 'utf8')) };
}
function share(a, b) {
  if (a.w !== b.w || a.h !== b.h) return 1;
  let n = 0;
  for (let i = 0; i < a.rgba.length; i += 4) if (Math.abs(a.rgba[i] - b.rgba[i]) > TOL || Math.abs(a.rgba[i + 1] - b.rgba[i + 1]) > TOL || Math.abs(a.rgba[i + 2] - b.rgba[i + 2]) > TOL) n++;
  return n / (a.w * a.h);
}
for (const app of apps) {
  const f = run(app, 'forward', {}), r = run(app, 'reversed', { CW_REVERSED_DEPTH: '1' });
  if (f.err || r.err) { report(false, `${app}: ${f.err || r.err}`); continue; }
  const d = share(f.img, r.img);
  const missing = r.state['cw.three.missing'] || 0, errs = r.state['cw.gpu.errors'] || 0;
  report(d <= (ALLOW[app] ?? MAX_SHARE) && !missing && !errs, `${app}: ${(d * 100).toFixed(3)}% of pixels over ${TOL} levels apart, ${missing} programs missing, ${errs} GPU errors`);
}
// what it is for: walls a metre apart 100 km out. Forward depth must show the far wall through the near
// one (the control), reversed depth never, on the native host and in a cart
const redOverGreen = (img) => {
  let red = 0, green = 0;
  for (let y = 0; y < img.h; y++) for (let x = 0; x < img.w; x++) {
    const i = (y * img.w + x) * 4;
    if (img.rgba[i] > img.rgba[i + 1] + 40) red++; else if (img.rgba[i + 1] > img.rgba[i] + 40) green++;
  }
  return { red, green };
};
if (!want.length || want.includes('far_view')) {
  const rev = run('far_view', 'far.reversed', {}), fwd = run('far_view', 'far.forward', { FAR_VIEW_FORWARD: '1' }), cart = run('far_view', 'far.cart', {}, 'romdev');
  if (rev.err || fwd.err || cart.err) report(false, `far_view: ${rev.err || fwd.err || cart.err}`);
  else {
    const a = redOverGreen(rev.img), b = redOverGreen(fwd.img), c = redOverGreen(cart.img);
    // (the far wall stands above the near one: its top edge is red on any depth)
    report(a.green > 1000 && a.red < a.green * 0.25, `far_view reversed: the near wall solid (red ${a.red}, green ${a.green})`);
    report(c.green > 1000 && c.red < c.green * 0.25, `far_view reversed in a cart (romdev): the near wall solid (red ${c.red}, green ${c.green})`);
    report(b.red > b.green, `control: far_view on forward depth shows the far wall through it (red ${b.red}, green ${b.green})`);
  }
}
if (!want.length) {
  const c = run('fx_pipeline', 'broken', { CW_REVERSED_DEPTH: '1', CW_TEST_BREAK_REVERSED: '1' });
  const f = readImage(join(OUT, 'fx_pipeline.forward.png'));
  const d = c.err ? 1 : share(f, c.img);
  report(d > MAX_SHARE, `control: the engine's WGSL left on forward depth moves pixels (${(d * 100).toFixed(3)}%)`);
}
process.exit(failed ? 1 : 0);
