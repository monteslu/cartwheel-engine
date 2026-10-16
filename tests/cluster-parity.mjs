// Clustered lights shade as three.c's own lights do: night_street with 7 omni
// and 7 spot lights, all shadowless (NS_PARITY=1), rendered with
// LightingSettings CLUSTERED and with FORWARD (three.c lights all fourteen
// per material), natively on GL and on WebGPU. The two images must match
// (no pixel off by more than 2 levels, at most 0.1% off at all), and the
// clustered run must really have clustered all fourteen.
// Control: FORWARD with one omni light fewer (NS_PARITY=2) against CLUSTERED
// must FAIL the same comparison, or the comparison cannot see a light.
//
//   node tests/cluster-parity.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
await takeSuiteLock('cluster-parity');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate');
mkdirSync(OUT, { recursive: true });
let fail = 0;
const say = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} cluster-parity: ${msg}`); if (!ok) fail++; };

function run(gpu, parity, forward) {
  const tag = `cluster-parity.${gpu}.p${parity}${forward ? 'f' : 'c'}`;
  const shot = join(OUT, `${tag}.png`), state = join(OUT, `${tag}.json`);
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'night_street',
    '--frames', '30', '--shot', shot, '--state', state], {
    env: { ...process.env, NS_PARITY: String(parity), NS_FORWARD: forward ? '1' : '0' }, encoding: 'utf8', timeout: 300000 });
  if (r.status !== 0) throw new Error(`${tag}: run-cart failed\n${(r.stdout + r.stderr).slice(-2000)}`);
  return { img: readImage(shot), st: JSON.parse(readFileSync(state, 'utf8')) };
}
const same = (d) => d.comparable && d.max <= 2 && d.any <= 0.001;

for (const gpu of ['gl', 'webgpu']) {
  const c = run(gpu, 1, false), f = run(gpu, 1, true), ctl = run(gpu, 2, true);
  say(c.st['ns.clustered'] === 14 && c.st['ns.spots'] === 7, `${gpu}: clustered ${c.st['ns.clustered']} lights (${c.st['ns.spots']} spot), expected 14 (7)`);
  say(f.st['ns.clustered'] === 0, `${gpu}: FORWARD clusters nothing (${f.st['ns.clustered']})`);
  for (const r of [c, f, ctl]) say(r.st['cw.three.missing'] === 0, `${gpu}: no program state missing (${r.st['cw.three.missing']})`);
  const d = diffImages(c.img, f.img);
  say(same(d), `${gpu}: CLUSTERED vs FORWARD: ${(d.any * 100).toFixed(4)}% of pixels differ, max ${d.max} levels`);
  const dc = diffImages(c.img, ctl.img);
  say(!same(dc), `${gpu}: control (one light fewer) must differ: ${(dc.gt16 * 100).toFixed(2)}% of pixels off by more than 16${same(dc) ? ' CONTROL PASSED' : ''}`);
}
process.exit(fail ? 1 : 0);
