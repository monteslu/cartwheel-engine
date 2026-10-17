// Terrain under clustered lights and the sun's cascades: terrain_demo's
// lights run (TERRAIN_MODE=lights: dusk, seven lamps along the track, the sun
// casting four cascades), natively on GL and on WebGPU. The terrain's
// materials carry their own surface parts, so they must take the cluster and
// csm parts beside them (terrain_cluster_csm) or lamps and cascades would
// light everything but the ground.
//   - lamps: the sun without shadows (TL_SUN=none), LightingSettings
//     CLUSTERED against FORWARD (three.c shades the seven lamps itself): the
//     images must match (at most 0.1% of pixels off by more than 2 levels),
//     with all seven lamps clustered and the terrain's materials among those
//     carrying the part. Control: FORWARD without the nearest lamp against
//     CLUSTERED with all seven must FAIL the same comparison.
//   - cascades: looking straight down on the car (TERRAIN_TOPCAM), no lamps,
//     the sun's four cascades against three.c's one shadow map, 300 m wide
//     and moved back along the light so every caster upstream is in it
//     (TL_SUN=map): at most 0.5% of pixels off by more than 16 levels
//     (shadow edges, resolved more finely by the cascades). Control: the sun
//     without shadows against the cascades must FAIL the same comparison.
//
//   node tests/terrain-lights.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
await takeSuiteLock('terrain-lights');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate');
mkdirSync(OUT, { recursive: true });
const FRAMES = 40;
let fail = 0;
const say = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} terrain-lights: ${msg}`); if (!ok) fail++; };

function run(gpu, tag, env, cmds) {
  const shot = join(OUT, `terrain-lights.${gpu}.${tag}.png`), state = join(OUT, `terrain-lights.${gpu}.${tag}.json`);
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'terrain_demo',
    '--frames', String(FRAMES), '--shot', shot, '--state', state, ...(cmds ? ['--start-cmds', cmds] : [])],
  { env: { ...process.env, TERRAIN_MODE: 'lights', ...env }, encoding: 'utf8', timeout: 300000 });
  if (r.status !== 0) throw new Error(`${gpu} ${tag}: run-cart failed\n${(r.stdout + r.stderr).slice(-2000)}`);
  return { img: readImage(shot), st: JSON.parse(readFileSync(state, 'utf8')) };
}
const pct = (v) => `${(v * 100).toFixed(3)}%`;

for (const gpu of ['gl', 'webgpu']) {
  const cl = run(gpu, 'clustered', { TL_SUN: 'none' });
  const fw = run(gpu, 'forward', { TL_SUN: 'none', TL_FORWARD: '1' });
  const fw6 = run(gpu, 'forward6', { TL_SUN: 'none', TL_FORWARD: '1' }, 'free /root/World/Lamps/Lamp0');
  say(cl.st['terrain.clustered'] === 7, `${gpu}: ${cl.st['terrain.clustered']} lamps clustered (7)`);
  say(cl.st['terrain.lit_materials'] > 0, `${gpu}: ${cl.st['terrain.lit_materials']} materials carry the cluster part`);
  const d = diffImages(cl.img, fw.img), dc = diffImages(cl.img, fw6.img);
  say(d.comparable && d.gt2 <= 0.001, `${gpu}: lamps on the terrain, clustered vs three.c's lights: ${pct(d.gt2)} of pixels off by >2 (max ${d.max})`);
  say(dc.comparable && dc.gt2 > 0.001, `${gpu}: CONTROL the nearest lamp missing is seen: ${pct(dc.gt2)} of pixels off by >2`);

  const csm = run(gpu, 'cascades', { TERRAIN_TOPCAM: '1', TL_LAMPS: '0' });
  const map = run(gpu, 'map', { TERRAIN_TOPCAM: '1', TL_LAMPS: '0', TL_SUN: 'map' });
  const none = run(gpu, 'noshadow', { TERRAIN_TOPCAM: '1', TL_LAMPS: '0', TL_SUN: 'none' });
  say(csm.st['terrain.cascades'] === 4, `${gpu}: the sun has ${csm.st['terrain.cascades']} cascades (4)`);
  const e = diffImages(csm.img, map.img), en = diffImages(csm.img, none.img);
  say(e.comparable && e.gt16 <= 0.005, `${gpu}: cascades on the terrain vs one shadow map: ${pct(e.gt16)} of pixels off by >16`);
  say(en.comparable && en.gt16 > 0.005, `${gpu}: CONTROL no shadow is seen: ${pct(en.gt16)} of pixels off by >16`);
}
console.log(fail ? `terrain-lights: ${fail} failures` : 'ok   terrain-lights');
process.exit(fail ? 1 : 0);
