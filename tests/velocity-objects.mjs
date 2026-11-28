// The GL velocity pass's moved meshes (src/render/velocity.c): their previous poses uploaded in one run before the
// draws into textures the engine owns, the velocity target's framebuffer with the scene's depth made once and kept,
// draw the same velocity buffer as the way before (CW_TEST_VELOCITY_LEGACY=1: three.c's data textures uploaded as each
// mesh is drawn, the framebuffer attached and checked every frame). Natively on GL, samples/motion_vectors with its
// crowd (MV_CROWD=1: 33 skinned walkers, 13 instanced swarms written every frame, 17 turning meshes; 63 meshes in the
// velocity target) at frame 75 (the velocity buffer on screen) and frame 200:
//   - the velocity target read back as floats after the pass (CW_TEST_VELOCITY_HASH=1: cw.velocity.hash) and the
//     picture equal, bit for bit, to the legacy way's;
//   - every mover drawn (cw.velocity.objects 63);
//   - CONTROL: camera motion only (CW_TEST_VELOCITY_OBJECTS=0) must fail the comparison: the hash sees the meshes;
//   - the same at both frames with the whole-scene render the pass used before t3_renderer_render_objects
//     (CW_TEST_VELOCITY_SCENE_RENDER=1);
// and samples/foliage_field with its DITHER fades and LOD crossfade (FOL_FADE=dither, TAA on) under a camera dollying
// through the LOD distance, every scatter cell's instance matrices rewritten each frame: the meshes drawn into the
// velocity target the same as with no grass at all (FOL_NOGRASS=1: the cells only rescale their instances, so they
// are still), at frames 60 and 120; CONTROL: with those writes counted as motion (CW_TEST_VELOCITY_SCALE_ONLY=0)
// cells are drawn and the velocity buffer differs.
// WebGPU is untouched. Native only.
//   node tests/velocity-objects.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'velocity-objects');
mkdirSync(OUT, { recursive: true });
{
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

function run(name, frames, env = {}, app = 'motion_vectors') {
  const png = join(OUT, `${name}.png`), st = join(OUT, `${name}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', 'gl', '--app', app, '--frames',
    String(frames), '--shot', png, '--state', st], { encoding: 'utf8', env: { ...process.env, MV_CROWD: '1', CW_TEST_VELOCITY_HASH: '1', ...env } });
  if (r.status !== 0) throw new Error(`${name}: ${((r.stderr || '') + (r.stdout || '')).trim().split('\n').slice(-3).join(' | ')}`);
  const s = JSON.parse(readFileSync(st, 'utf8'));
  return { png: readFileSync(png), hash: s['cw.velocity.hash'], objects: s['cw.velocity.objects'] };
}

let fail = 0;
const bad = (m) => { console.log('  FAIL ' + m); fail++; };
for (const frames of [75, 200]) {
  const now = run(`new.${frames}`, frames), old = run(`legacy.${frames}`, frames, { CW_TEST_VELOCITY_LEGACY: '1' });
  const off = run(`off.${frames}`, frames, { CW_TEST_VELOCITY_OBJECTS: '0' });
  console.log(`frame ${frames}: hash ${now.hash} (legacy ${old.hash}, camera motion only ${off.hash}), ${now.objects} meshes drawn`);
  if (typeof now.hash !== 'number' || now.hash === 0) bad(`frame ${frames}: no velocity hash in the state`);
  if (now.hash !== old.hash) bad(`frame ${frames}: the velocity buffer differs from the legacy way's`);
  if (!now.png.equals(old.png)) bad(`frame ${frames}: the picture differs from the legacy way's`);
  if (now.objects !== 63 || old.objects !== 63) bad(`frame ${frames}: ${now.objects} meshes drawn (legacy ${old.objects}), 63 expected`);
  if (off.hash === now.hash) bad(`CONTROL PASSED: frame ${frames} with camera motion only has the same velocity buffer`);
  const scene = run(`scene.${frames}`, frames, { CW_TEST_VELOCITY_SCENE_RENDER: '1' });
  if (scene.hash !== now.hash || !scene.png.equals(now.png)) bad(`frame ${frames}: the whole-scene render's velocity buffer or picture differs from the moved meshes' own pass`);
}
for (const frames of [60, 120]) {
  const fol = { FOL_FADE: 'dither', FOL_CAM: 'dolly', FOL_TAA: '1' };
  const now = run(`fol.${frames}`, frames, fol, 'foliage_field');
  const none = run(`fol-nograss.${frames}`, frames, { ...fol, FOL_NOGRASS: '1' }, 'foliage_field');
  const ctl = run(`fol-ctl.${frames}`, frames, { ...fol, CW_TEST_VELOCITY_SCALE_ONLY: '0' }, 'foliage_field');
  console.log(`foliage_field frame ${frames}: ${now.objects} meshes drawn (with no grass ${none.objects}; scatter writes as motion: ${ctl.objects}), hash ${now.hash} (scatter writes as motion ${ctl.hash})`);
  if (typeof now.hash !== 'number' || now.hash === 0) bad(`foliage_field frame ${frames}: no velocity hash in the state`);
  if (now.objects !== none.objects) bad(`foliage_field frame ${frames}: ${now.objects} meshes drawn into the velocity target, ${none.objects} with no grass: scatter cells drawn`);
  if (!(ctl.objects > 0) || ctl.hash === now.hash) bad(`CONTROL PASSED: foliage_field frame ${frames} with scatter writes as motion draws ${ctl.objects} meshes, the same buffer`);
}
console.log(fail ? `FAIL velocity-objects (${fail})` : 'ok   velocity-objects');
process.exit(fail ? 1 : 0);
