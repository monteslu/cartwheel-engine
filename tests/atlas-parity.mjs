// Clustered lights' shadows (the shadow atlas) fall where three.c's own
// shadow maps put them: night_street from above with only its four
// floodlights and two street lamps, all casting shadows over crates
// (NS_PARITY=3), with LightingSettings.shadow_atlas on (the lights clustered,
// their shadows tiles of the atlas) and off (NS_NO_ATLAS: three.c's own maps,
// as three.js r186 draws them), natively on GL and on WebGPU. The images must
// match (no pixel off by more than 2 levels, at most 0.1% off at all).
// Control: the same lights without shadows (NS_PARITY=4) must differ by more
// than 16 levels on at least 0.1% of the pixels, or the check cannot see a shadow.
//
//   node tests/atlas-parity.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
await takeSuiteLock('atlas-parity');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate');
mkdirSync(OUT, { recursive: true });
let fail = 0;
const say = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} atlas-parity: ${msg}`); if (!ok) fail++; };

function run(gpu, tag, env) {
  const shot = join(OUT, `atlas-parity.${gpu}.${tag}.png`), state = join(OUT, `atlas-parity.${gpu}.${tag}.json`);
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'night_street',
    '--frames', '30', '--shot', shot, '--state', state], { env: { ...process.env, NS_TOPCAM: '1', ...env }, encoding: 'utf8', timeout: 300000 });
  if (r.status !== 0) throw new Error(`${tag}: run-cart failed\n${(r.stdout + r.stderr).slice(-2000)}`);
  return { img: readImage(shot), st: JSON.parse(readFileSync(state, 'utf8')) };
}
const same = (d) => d.comparable && d.max <= 2 && d.any <= 0.001;

for (const gpu of ['gl', 'webgpu']) {
  const atlas = run(gpu, 'atlas', { NS_PARITY: '3' }), own = run(gpu, 'own', { NS_PARITY: '3', NS_NO_ATLAS: '1' });
  const ctl = run(gpu, 'none', { NS_PARITY: '4' });
  say(atlas.st['ns.clustered'] === 6 && atlas.st['ns.shadowed'] === 6, `${gpu}: 6 lights clustered (${atlas.st['ns.clustered']}), 6 with atlas shadows (${atlas.st['ns.shadowed']})`);
  say(own.st['ns.clustered'] === 0 && own.st['ns.forward'] === 6, `${gpu}: without the atlas three.c draws all 6 (${own.st['ns.forward']})`);
  for (const r of [atlas, own, ctl]) say(r.st['cw.three.missing'] === 0 && r.st['cw.gpu.errors'] === 0,
    `${gpu}: no missing program state (${r.st['cw.three.missing']}) or GPU error (${r.st['cw.gpu.errors']})`);
  const d = diffImages(atlas.img, own.img);
  say(same(d), `${gpu}: atlas vs three.c's maps: ${(d.any * 100).toFixed(4)}% of pixels differ, max ${d.max} levels`);
  const dc = diffImages(atlas.img, ctl.img);
  say(dc.gt16 >= 0.001, `${gpu}: control (no shadows) must differ: ${(dc.gt16 * 100).toFixed(3)}% off by more than 16${dc.gt16 < 0.001 ? ' CONTROL PASSED' : ''}`);
}
process.exit(fail ? 1 : 0);
