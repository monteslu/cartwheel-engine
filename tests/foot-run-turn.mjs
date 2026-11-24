// FootPlacement3D at a run (cartwheel/ik.h) on samples/anim_feet: a person runs at 5.4 m/s over stones 5 to 50 cm
// high with its feet placed at full influence, turns right at speed, turns right again sharply, stops, turns half
// round on the spot, runs over a platform with a left turn and stops (780 ticks); a twin runs the same clips with its
// rays hitting nothing (the animation's legs). Per host and backend:
//   (a) no jump: no thigh, shin or foot turns more than JUMP_MAX degrees in one tick past the twin's same bone
//       (fp.jump); CONTROL: CW_TEST_FOOT_NO_RATE=1 (no limit on how fast the solve's turn may change) turns a leg
//       tens of degrees in a tick;
//   (b) no fold: every drawn ankle stays at least FOLD_MIN of the height the reach must leave it under its hip (half
//       the animated height, or a quarter of the leg) (fp.fold); CONTROL: CW_TEST_FOOT_NO_REACH=1 pulls a swinging
//       foot up toward its hip over a stone;
//   (c) placed: a planted foot on raised ground that has stayed put for 8 ticks sits on it (ankle within 2 cm), on
//       PLACED_MIN of such ticks, at least RAISED_MIN of them (fp.placed, fp.raised); CONTROL: FP_OFF=1 (the
//       runner's FootPlacement3D at influence 0) places none;
//   (d) determinism: the runner's pose hash after each 60-tick window (fp.hash_win) is the same bits on every host
//       and backend.
//   node tests/foot-run-turn.mjs [--hosts native,romdev] [--gpu gl,webgpu]   (romdev runs the dual cart, build/cart-wgpu)
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'foot-run-turn');
mkdirSync(OUT, { recursive: true });
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const SOFTWARE = process.env.CW_TEST_GPU === 'software';
// (CI, CW_TEST_GPU=software, has no romdev: native alone there)
const HOSTS = arg('--hosts', SOFTWARE ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
const FRAMES = 780, JUMP_MAX = 12, FOLD_MIN = 0.9, PLACED_MIN = 0.9, RAISED_MIN = 30; // the sample's fp.ok uses the same
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

function run(host, gpu, name, env) {
  const st = join(OUT, `${name}.${host}.${gpu}.json`);
  const a = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'anim_feet', '--frames', String(FRAMES), '--gpu', gpu,
    '--shot', join(OUT, `${name}.${host}.${gpu}.png`), '--state', st];
  if (env) a.push('--env', JSON.stringify(env));
  const r = spawnSync('node', a, { encoding: 'utf8', env: process.env });
  if (r.status !== 0) throw new Error(`${host} ${gpu} ${name}: ${((r.stderr || '') + (r.stdout || '')).trim().split('\n').slice(-3).join(' | ')}`);
  const j = JSON.parse(readFileSync(st, 'utf8'));
  return j.state || j;
}

let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const f = (v) => JSON.stringify(v, (k, x) => (typeof x === 'number' ? Number(x.toFixed(3)) : x));
const BONES = ['UpperLegL', 'LowerLegL', 'FootL', 'UpperLegR', 'LowerLegR', 'FootR'];
const jumpOk = (s) => s['fp.jump'] <= JUMP_MAX;
const foldOk = (s) => s['fp.fold'] >= FOLD_MIN;
const placedOk = (s) => s['fp.raised'] >= RAISED_MIN && s['fp.placed'] >= PLACED_MIN;
const where = (s) => `${BONES[s['fp.jump_bone']]} at tick ${s['fp.jump_at']}`;
let refHash = null;
for (const host of HOSTS)
  for (const gpu of GPUS) {
    const tag = `${host} ${gpu}`;
    try {
      const s = run(host, gpu, 'main');
      check(s['fp.tick'] === FRAMES, `${tag}: ran ${FRAMES} ticks`);
      check(jumpOk(s), `${tag}: (a) a leg bone turns at most ${f(s['fp.jump'])} deg in a tick past the animation (${where(s)}; runs ${f(s['fp.jump_run'])}, stands ${f(s['fp.jump_stand'])}; <= ${JUMP_MAX})`);
      check(foldOk(s), `${tag}: (b) ankles keep ${f(s['fp.fold'])} of the height under the hip the reach must leave (>= ${FOLD_MIN})`);
      check(placedOk(s), `${tag}: (c) planted on raised steady ground: ${f(s['fp.placed'])} of ${s['fp.raised']} foot-ticks within 2 cm (>= ${PLACED_MIN} of >= ${RAISED_MIN})`);
      const h = JSON.stringify(s['fp.hash_win']);
      if (!refHash) refHash = { tag, h };
      check(h === refHash.h && !s['fp.hash_win'].includes(0), `${tag}: (d) pose hashes per 60-tick window ${h === refHash.h ? 'equal to' : 'DIFFER from'} ${refHash.tag}`);
      const rate = run(host, gpu, 'norate', { CW_TEST_FOOT_NO_RATE: '1' });
      check(!jumpOk(rate), `${tag}: CONTROL (a): with no turn limit a leg turns ${f(rate['fp.jump'])} deg in a tick past the animation (${where(rate)})`);
      check(JSON.stringify(rate['fp.hash_win']) !== h, `${tag}: CONTROL (d): the hashes see the turn limit off`);
      const reach = run(host, gpu, 'noreach', { CW_TEST_FOOT_NO_REACH: '1' });
      check(!foldOk(reach), `${tag}: CONTROL (b): with no reach guard an ankle keeps ${f(reach['fp.fold'])} of it`);
      const off = run(host, gpu, 'off', { FP_OFF: '1' });
      check(!placedOk(off), `${tag}: CONTROL (c): foot placement at influence 0 places ${f(off['fp.placed'])} of ${off['fp.raised']}`);
    } catch (e) {
      check(false, `${tag}: ${e.message}`);
    }
  }
console.log(fail ? `foot-run-turn: ${fail} failures` : 'ok   foot-run-turn');
process.exit(fail ? 1 : 0);
