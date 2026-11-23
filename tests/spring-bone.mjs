// SpringBone3D (cartwheel/ik.h) on samples/anim_spring: a person with a ponytail and two coat tails walks, stops,
// spins three turns, stands, teleports and walks back (600 ticks). Per host and backend:
//   (a) motion: every tip trails its animated place by more than 3 cm while walking and is back within 1 cm of where
//       it hung before the walk, for good, within 75 ticks of the stop (sb.motion_ok); CONTROL: the modifier off
//       (SPRING_OFF) leaves no lag;
//   (b) collision: during the spin every simulated joint of every chain stays outside every collision shape (the
//       body capsule, the shoulders, the thighs, the head; sb.pen_spin <= 0) while the tips fly out more than 5 cm
//       (sb.collide_ok); CONTROL: collisions off (SPRING_NO_COLLIDE) puts joints inside a shape;
//   (c) teleport: 6 m in one tick, every tip's offset matches an identical twin's that did not teleport within 1 mm
//       (sb.teleport_ok), and the reset call puts every tip on its animated place (sb.reset_ok); CONTROL: the jump
//       check off (SPRING_NO_TELEPORT) whips the tips more than 10 cm;
//   (d) determinism: the pose hash after each 60-tick window (sb.hash_win) is the same bits on every host and backend.
//   node tests/spring-bone.mjs [--hosts native,romdev] [--gpu gl,webgpu]   (romdev runs the dual cart, build/cart-wgpu)
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'spring-bone');
mkdirSync(OUT, { recursive: true });
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
// (CI, CW_TEST_GPU=software, has no romdev: native alone there)
const HOSTS = arg('--hosts', process.env.CW_TEST_GPU === 'software' ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

function run(host, gpu, name, env) {
  const st = join(OUT, `${name}.${host}.${gpu}.json`);
  const a = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'anim_spring', '--frames', '600', '--gpu', gpu,
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
let refHash = null;
for (const host of HOSTS)
  for (const gpu of GPUS) {
    const tag = `${host} ${gpu}`;
    try {
      const s = run(host, gpu, 'main');
      check(s['sb.tick'] === 600, `${tag}: ran 600 ticks`);
      check(s['sb.motion_ok'] === 1, `${tag}: (a) tips lag while walking ${f(s['sb.lag'])} m and settle ${f(s['sb.settle'])} ticks after the stop`);
      check(s['sb.collide_ok'] === 1, `${tag}: (b) during the spin the deepest joint is ${f(s['sb.pen_spin'])} m from inside a shape (<= 0: outside), tips out ${f(s['sb.spin_lag'])} m`);
      check(s['sb.teleport_ok'] === 1 && s['sb.reset_ok'] === 1, `${tag}: (c) teleport: tips off the twin's by ${f(s['sb.whip'])} m; reset puts them on the animated pose (${s['sb.reset_ok']}); resets ${s['sb.resets']}`);
      const h = JSON.stringify(s['sb.hash_win']);
      if (!refHash) refHash = { tag, h };
      check(h === refHash.h && !s['sb.hash_win'].includes(0), `${tag}: (d) pose hashes per 60-tick window ${h === refHash.h ? 'equal to' : 'DIFFER from'} ${refHash.tag}`);
      const off = run(host, gpu, 'off', { SPRING_OFF: '1' });
      check(off['sb.motion_ok'] === 0 && Math.max(...off['sb.lag']) === 0, `${tag}: CONTROL (a): the modifier off leaves no lag ${f(off['sb.lag'])}, motion_ok ${off['sb.motion_ok']}`);
      check(JSON.stringify(off['sb.hash_win']) !== h, `${tag}: CONTROL (d): the hashes see the modifier off`);
      const nc = run(host, gpu, 'nocollide', { SPRING_NO_COLLIDE: '1' });
      check(nc['sb.collide_ok'] === 0 && nc['sb.pen_spin'] > 0.01, `${tag}: CONTROL (b): collisions off put a joint ${f(nc['sb.pen_spin'])} m inside a shape`);
      const nt = run(host, gpu, 'noteleport', { SPRING_NO_TELEPORT: '1' });
      check(nt['sb.teleport_ok'] === 0 && Math.min(...nt['sb.whip']) > 0.1, `${tag}: CONTROL (c): the jump check off whips the tips ${f(nt['sb.whip'])} m`);
    } catch (e) {
      check(false, `${tag}: ${e.message}`);
    }
  }
console.log(fail ? `spring-bone: ${fail} failures` : 'ok   spring-bone');
process.exit(fail ? 1 : 0);
