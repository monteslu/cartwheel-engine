// SpringBone3D skirt mode (cartwheel/ik.h) on samples/anim_robe: a person in a long robe (eight chains of four bones,
// skirt RING) and one in a long coat open at the front (five chains of three, skirt OPEN) run at 5.4 m/s, turn half
// round at speed, stop, turn on the spot and run and stop again (900 ticks). Per host and backend:
//   (a) legs: no leg (thigh and shin capsules placed from the bones) goes more than PEN_MAX through either garment's
//       surface (its joints and the panels between neighbouring chains) in the runs, at the stands and in the turn on
//       the spot (rb.pen_*); CONTROL: chains on their own (ROBE_SKIRT=OFF, the old way) let the legs through;
//   (b) settle and rest: after each of the three stops every tip is within 1 cm of where it hangs, for good, within
//       SETTLE_MAX ticks (rb.settle), and at rest each tip's offset strays from its 5-tick running mean by at most
//       JIT_MAX mm (rb.jit_max); CONTROL: ROBE_SKIRT=OFF misses both;
//   (c) no flip: in the runs no tip strays more than SWING_MAX m from its animated place (rb.swing_run); CONTROL:
//       ROBE_SKIRT=OFF throws one further;
//   (d) determinism: both models' pose hashes after each 60-tick window (rb.hash_win) are the same bits on every
//       host and backend;
//   (e) cost: the modifier phase per character (rb.us, the host's clock), reported; on a real GPU host with eight
//       people (ROBE_CROWD=8) the quietest tick must be under COST_MAX us a character natively (CI's shared runners
//       report it only).
//   node tests/robe-skirt.mjs [--hosts native,romdev] [--gpu gl,webgpu]   (romdev runs the dual cart, build/cart-wgpu)
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'robe-skirt');
mkdirSync(OUT, { recursive: true });
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const SOFTWARE = process.env.CW_TEST_GPU === 'software';
// (CI, CW_TEST_GPU=software, has no romdev: native alone there)
const HOSTS = arg('--hosts', SOFTWARE ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
const PEN_MAX = 0.01, SETTLE_MAX = 60, JIT_MAX = 1, SWING_MAX = 0.6, COST_MAX = 40; // the sample's own oks use the same
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

function run(host, gpu, name, env) {
  const st = join(OUT, `${name}.${host}.${gpu}.json`);
  const a = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'anim_robe', '--frames', '960', '--gpu', gpu,
    '--shot', join(OUT, `${name}.${host}.${gpu}.png`), '--state', st];
  if (env) a.push('--env', JSON.stringify(env));
  const r = spawnSync('node', a, { encoding: 'utf8', env: process.env });
  if (r.status !== 0) throw new Error(`${host} ${gpu} ${name}: ${((r.stderr || '') + (r.stdout || '')).trim().split('\n').slice(-3).join(' | ')}`);
  const j = JSON.parse(readFileSync(st, 'utf8'));
  return j.state || j;
}

let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const f = (v) => JSON.stringify(v, (k, x) => (typeof x === 'number' ? Number(x.toFixed(4)) : x));
const max = (a) => Math.max(...a);
const legsOk = (s) => max([...s['rb.pen_run'], ...s['rb.pen_stand'], ...s['rb.pen_turn']]) <= PEN_MAX;
const settleOk = (s) => s['rb.settle'].every((t) => t >= 0 && t <= SETTLE_MAX);
const restOk = (s) => max(s['rb.jit_max']) <= JIT_MAX;
const flipOk = (s) => max(s['rb.swing_run']) <= SWING_MAX;
let refHash = null;
for (const host of HOSTS)
  for (const gpu of GPUS) {
    const tag = `${host} ${gpu}`;
    try {
      const s = run(host, gpu, 'main');
      check(s['rb.tick'] === 960, `${tag}: ran 960 ticks`);
      check(legsOk(s), `${tag}: (a) legs through the robe, coat (m): run ${f(s['rb.pen_run'])}, stands ${f(s['rb.pen_stand'])}, turn ${f(s['rb.pen_turn'])} (<= ${PEN_MAX})`);
      check(settleOk(s), `${tag}: (b) settled after the stops in ${f(s['rb.settle'])} ticks (robe x3, coat x3; <= ${SETTLE_MAX})`);
      check(restOk(s), `${tag}: (b) rest jitter max ${f(s['rb.jit_max'])} mm, rms ${f(s['rb.jit_rms'])} mm (<= ${JIT_MAX})`);
      check(flipOk(s), `${tag}: (c) tips off their animated places in the runs ${f(s['rb.swing_run'])} m (<= ${SWING_MAX})`);
      const h = JSON.stringify(s['rb.hash_win']);
      if (!refHash) refHash = { tag, h };
      check(h === refHash.h && !s['rb.hash_win'].includes(0), `${tag}: (d) pose hashes per 60-tick window ${h === refHash.h ? 'equal to' : 'DIFFER from'} ${refHash.tag}`);
      console.log(`     ${tag}: (e) modifier phase ${f(s['rb.us'])} us a character on average, ${f(s['rb.us_min'])} on the quietest tick (two people)`);
      const off = run(host, gpu, 'off', { ROBE_SKIRT: 'OFF' });
      check(!legsOk(off), `${tag}: CONTROL (a): chains on their own let the legs ${f(max([...off['rb.pen_run'], ...off['rb.pen_stand'], ...off['rb.pen_turn']]))} m into the garments`);
      check(!flipOk(off), `${tag}: CONTROL (c): chains on their own throw a tip ${f(off['rb.swing_run'])} m`);
      check(JSON.stringify(off['rb.hash_win']) !== h, `${tag}: CONTROL (d): the hashes see skirt mode off`);
      check(!settleOk(off) && !restOk(off), `${tag}: CONTROL (b): chains on their own settle in ${f(off['rb.settle'])} ticks (999: never), rest jitter ${f(off['rb.jit_max'])} mm`);
      if (host === 'native' && gpu === GPUS[0]) {
        const crowd = run(host, gpu, 'crowd', { ROBE_CROWD: '8' });
        const ok = SOFTWARE || crowd['rb.us_min'] <= COST_MAX;
        check(ok, `${tag}: (e) eight people: ${f(crowd['rb.us'])} us a character on average, ${f(crowd['rb.us_min'])} on the quietest tick${SOFTWARE ? ' (reported: CI runners are shared)' : ` (<= ${COST_MAX})`}`);
      }
    } catch (e) {
      check(false, `${tag}: ${e.message}`);
    }
  }
console.log(fail ? `robe-skirt: ${fail} failures` : 'ok   robe-skirt');
process.exit(fail ? 1 : 0);
