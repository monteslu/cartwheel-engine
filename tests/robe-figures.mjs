// SpringBone3D skirt mode on real game figures (samples/anim_robe with ROBE_FIGURES=1): the tall robed caster
// (tidecaller.glb, built at 1.5 times a person's size through a scaled Hips bone, a robe of eight chains of four bones)
// and the coated spearman (lancer.glb, 1.08, a coat open at the front on five chains of three), copied from
// even-more-cartwheel-examples' emberthrone by samples/anim_robe/take-figures.mjs, their stance, walk and run with a
// weapon retargeted onto them when the sample is packed. Both run, turn, stop and turn on the spot (960 ticks); the
// setup is the one the guide gives for them (the seam moved out and up in the rig, skirt_stretch 1). The sample's oks:
//   legs_ok    no leg (each leg's own mesh size) more than 5 cm through either garment, in the runs, stands and turn
//   settle_ok  after each stop every tip is within 2 cm of its own 31-tick running mean (the swing gone) within 90 ticks
//   rest_ok    at rest each tip's offset strays from its 5-tick running mean by at most 1.5 mm
//   flip_ok    in the runs no tip rises above its chain's root
// and the pose hash per 60-tick window, the same bits on every host and backend. CONTROLS, each of which must fail one:
//   ROBE_SKIRT=OFF            the chains on their own (legs 8 cm through, tips thrown over their roots, never settling)
//   CW_TEST_SKIRT_074=1       skirt mode as 0.74.0 solved it (pushes along the shape's normal, the axis through the
//                             roots' mean, no plumb hang, no seam or free-edge rule): legs through, 3 mm of jitter
//   ROBE_STRETCH=0.3          the setup the game first used (links that stretch 30%): 36 mm of jitter, tips over roots
//   node tests/robe-figures.mjs [--hosts native,romdev] [--gpu gl,webgpu]   (romdev runs the dual cart, build/cart-wgpu)
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'robe-figures');
mkdirSync(OUT, { recursive: true });
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const SOFTWARE = process.env.CW_TEST_GPU === 'software';
// (CI, CW_TEST_GPU=software, has no romdev: native alone there)
const HOSTS = arg('--hosts', SOFTWARE ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
const COST_MAX = 60;
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

function run(host, gpu, name, env) {
  const st = join(OUT, `${name}.${host}.${gpu}.json`);
  const a = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'anim_robe', '--frames', '960', '--gpu', gpu,
    '--shot', join(OUT, `${name}.${host}.${gpu}.png`), '--state', st, '--env', JSON.stringify({ ROBE_FIGURES: '1', ...env })];
  const r = spawnSync('node', a, { encoding: 'utf8', env: process.env });
  if (r.status !== 0) throw new Error(`${host} ${gpu} ${name}: ${((r.stderr || '') + (r.stdout || '')).trim().split('\n').slice(-3).join(' | ')}`);
  const j = JSON.parse(readFileSync(st, 'utf8'));
  return j.state || j;
}

let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const f = (v) => JSON.stringify(v, (k, x) => (typeof x === 'number' ? Number(x.toFixed(4)) : x));
const OKS = ['rb.legs_ok', 'rb.settle_ok', 'rb.rest_ok', 'rb.flip_ok'];
const summary = (s) => `legs (m): run ${f(s['rb.pen_run'])}, stands ${f(s['rb.pen_stand'])}, turn ${f(s['rb.pen_turn'])}; swing gone after ${f(s['rb.settle_osc'])} ticks; rest jitter max ${f(s['rb.jit_max'])} mm; highest tip against its root ${f(s['rb.lift_run'])}`;
let refHash = null;
for (const host of HOSTS)
  for (const gpu of GPUS) {
    const tag = `${host} ${gpu}`;
    try {
      const s = run(host, gpu, 'main');
      check(s['rb.tick'] === 960, `${tag}: ran 960 ticks`);
      check(OKS.every((k) => s[k] === 1), `${tag}: robe, coat ${OKS.map((k) => `${k.slice(3)} ${s[k]}`).join(', ')}: ${summary(s)}`);
      const h = JSON.stringify(s['rb.hash_win']);
      if (!refHash) refHash = { tag, h };
      check(h === refHash.h && !s['rb.hash_win'].includes(0), `${tag}: pose hashes per 60-tick window ${h === refHash.h ? 'equal to' : 'DIFFER from'} ${refHash.tag}`);
      console.log(`     ${tag}: modifier phase ${f(s['rb.us'])} us a figure on average, ${f(s['rb.us_min'])} on the quietest tick (two figures)`);
      for (const [name, env, why] of [['off', { ROBE_SKIRT: 'OFF' }, 'the chains on their own'], ['v074', { CW_TEST_SKIRT_074: '1' }, 'skirt mode as 0.74.0'],
        ['stretch03', { ROBE_STRETCH: '0.3' }, 'links that stretch 30%']]) {
        const c = run(host, gpu, name, env);
        const failed = OKS.filter((k) => c[k] !== 1);
        check(failed.length > 0, `${tag}: CONTROL ${why} fails ${failed.map((k) => k.slice(3)).join(', ') || 'nothing'}: ${summary(c)}`);
      }
      if (host === 'native' && gpu === GPUS[0]) {
        const crowd = run(host, gpu, 'crowd', { ROBE_CROWD: '8' });
        const ok = SOFTWARE || crowd['rb.us_min'] <= COST_MAX;
        check(ok, `${tag}: eight figures: ${f(crowd['rb.us'])} us a figure on average, ${f(crowd['rb.us_min'])} on the quietest tick${SOFTWARE ? ' (reported: CI runners are shared)' : ` (<= ${COST_MAX})`}`);
      }
    } catch (e) {
      check(false, `${tag}: ${e.message}`);
    }
  }
console.log(fail ? `robe-figures: ${fail} failures` : 'ok   robe-figures');
process.exit(fail ? 1 : 0);
