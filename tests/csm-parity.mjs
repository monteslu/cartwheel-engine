// The sun's cascaded shadow maps put shadows where a plain shadow map does:
// street_sun seen from above (NS_TOPCAM), its sun with four cascades against
// the same sun with three.c's one shadow map (NS_NO_CASCADES: the map moved
// up the light and widened to 120 m, so it covers everything in view), on GL
// and on WebGPU. The images must agree (at most 0.5% of pixels off by more
// than 16 levels: shadow edges, which the cascades resolve more finely).
// Control: the sun without shadows (NS_NO_SHADOW) against the cascades must
// FAIL the same comparison, or the comparison cannot see a shadow.
//
//   node tests/csm-parity.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
await takeSuiteLock('csm-parity');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate');
mkdirSync(OUT, { recursive: true });
let fail = 0;
const say = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} csm-parity: ${msg}`); if (!ok) fail++; };

function run(gpu, tag, env) {
  const shot = join(OUT, `csm-parity.${gpu}.${tag}.png`), state = join(OUT, `csm-parity.${gpu}.${tag}.json`);
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'street_sun',
    '--frames', '30', '--shot', shot, '--state', state], { env: { ...process.env, NS_TOPCAM: '1', ...env }, encoding: 'utf8', timeout: 300000 });
  if (r.status !== 0) throw new Error(`${tag}: run-cart failed\n${(r.stdout + r.stderr).slice(-2000)}`);
  return { img: readImage(shot), st: JSON.parse(readFileSync(state, 'utf8')) };
}
const same = (d) => d.comparable && d.gt16 <= 0.005;

for (const gpu of ['gl', 'webgpu']) {
  const csm = run(gpu, 'cascades', {}), ref = run(gpu, 'map', { NS_NO_CASCADES: '1' }), ctl = run(gpu, 'none', { NS_NO_SHADOW: '1' });
  say(csm.st['ns.cascades'] === 4, `${gpu}: the sun has ${csm.st['ns.cascades']} cascades (4)`);
  say(ref.st['ns.cascades'] === 0, `${gpu}: the reference sun has none (${ref.st['ns.cascades']})`);
  for (const r of [csm, ref, ctl]) say(r.st['cw.three.missing'] === 0 && r.st['cw.gpu.errors'] === 0,
    `${gpu}: no missing program state (${r.st['cw.three.missing']}) or GPU error (${r.st['cw.gpu.errors']})`);
  const d = diffImages(csm.img, ref.img);
  say(same(d), `${gpu}: cascades vs one map: ${(d.gt16 * 100).toFixed(3)}% of pixels off by more than 16 levels`);
  const dc = diffImages(csm.img, ctl.img);
  say(!same(dc), `${gpu}: control (no shadows) must differ: ${(dc.gt16 * 100).toFixed(2)}%${same(dc) ? ' CONTROL PASSED' : ''}`);
}
process.exit(fail ? 1 : 0);
