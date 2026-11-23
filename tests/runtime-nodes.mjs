// Nodes that own render passes or per-frame GPU work, added from code while the game runs (cw_node_new, properties set
// one by one), draw as the same nodes declared in a scene file: samples/runtime_nodes, its world in the main tree, in a
// Backdrop's world and in a SubViewport's world, natively and as the dual cart on romdev, GL and WebGPU:
//   - the content built from code 30 ticks in (GPU particles with an attractor, a trail, a screen and a mesh decal, an
//     omni, a spot and a cascaded sun, height fog with a fog volume, a shockwave, an explosion, water, a reflection
//     probe, a light probe grid, cloth) draws the declared content's picture (declared hidden, shown on the same tick)
//     within 0.1% of pixels more than 16/255 apart, and every type's counter matches and moved;
//     CONTROL: no content added (RT_ADD=none) must fail the picture;
//   - a world's program states that no table has (draws skipped) stay reported with every other tree's, whichever tree
//     changed its list last (cw.three.missing_states, the field the gates and the cart survey read): a toon box in
//     the world and another in the main tree 30 ticks later, both reported; CONTROL: CW_TEST_MISSING_LAST_TREE=1 (the
//     last tree's list only, as before) must lose the world's.
//   node tests/runtime-nodes.mjs [--hosts native,romdev] [--gpu gl,webgpu]   (romdev runs the dual cart, build/cart-wgpu)
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'runtime-nodes');
mkdirSync(OUT, { recursive: true });
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
// (CI, CW_TEST_GPU=software, has no romdev: native alone there)
const HOSTS = arg('--hosts', process.env.CW_TEST_GPU === 'software' ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
const TREES = ['main', 'backdrop', 'subviewport'];
const FRAMES = 150;
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

function run(host, gpu, name, env, extra = []) {
  const png = join(OUT, `${name}.${host}.${gpu}.png`), st = join(OUT, `${name}.${host}.${gpu}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'runtime_nodes', '--frames', String(FRAMES),
    '--gpu', gpu, '--shot', png, '--state', st, '--env', JSON.stringify(env), ...extra], { encoding: 'utf8', env: process.env });
  if (r.status !== 0) throw new Error(`${host} ${gpu} ${name}: ${((r.stderr || '') + (r.stdout || '')).trim().split('\n').slice(-3).join(' | ')}`);
  const s = JSON.parse(readFileSync(st, 'utf8'));
  return { img: readImage(png), s: s.fields || s };
}
const v = (r, k) => { const x = r.s[k]; return typeof x === 'object' && x !== null && 'value' in x ? x.value : x; };
/** a bytes field as text (hex in a native dump, text on romdev) */
const text = (r, k) => {
  const x = v(r, k);
  if (typeof x !== 'string') return '';
  const t = /^[0-9a-f]+$/i.test(x) && x.length % 2 === 0 ? Buffer.from(x, 'hex').toString('latin1') : x;
  return t.replace(/\0+$/, '');
};
/** share of pixels more than lv apart in any channel */
function apart(a, b, lv) {
  let n = 0;
  for (let i = 0; i < a.rgba.length; i += 4)
    for (let k = 0; k < 3; k++) if (Math.abs(a.rgba[i + k] - b.rgba[i + k]) > lv) { n++; break; }
  return n / (a.w * a.h);
}

const COUNTERS = ['rt.p3_draws', 'rt.dec_drawn', 'rt.fog_volumes', 'rt.water_surfaces', 'rt.gi_captures', 'rt.boom_flashes', 'rt.cloth_hash'];
let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
for (const host of HOSTS)
  for (const gpu of GPUS) {
    const tag = `${host} ${gpu}`;
    for (const tree of TREES) {
      let decl, code, none;
      try {
        decl = run(host, gpu, `${tree}-scene`, { RT_TREE: tree, RT_ADD: 'scene' });
        code = run(host, gpu, `${tree}-code`, { RT_TREE: tree, RT_ADD: 'code' });
        none = run(host, gpu, `${tree}-none`, { RT_TREE: tree, RT_ADD: 'none' });
      } catch (e) {
        check(false, `${tag} ${tree}: ${e.message}`);
        continue;
      }
      const d = apart(decl.img, code.img, 16), c = apart(decl.img, none.img, 16);
      check(d <= 0.001, `${tag} ${tree}: added from code draws the declared picture (${(100 * d).toFixed(3)}% of pixels > 16 apart, at most 0.1%)`);
      check(c > 0.05, `${tag} ${tree}: CONTROL, nothing added, differs (${(100 * c).toFixed(2)}% of pixels > 16 apart)`);
      for (const k of COUNTERS)
        check(v(code, k) === v(decl, k) && v(code, k) !== 0 && v(none, k) !== v(decl, k),
          `${tag} ${tree}: ${k} ${v(code, k)} (declared ${v(decl, k)}, none ${v(none, k)})`);
      if (tree === 'main') continue;
      // a world's skipped states reported with the main tree's
      let miss, ctl;
      try {
        miss = run(host, gpu, `${tree}-missing`, { RT_TREE: tree, RT_MISSING: '1' }, ['--allow-skipped']);
        ctl = run(host, gpu, `${tree}-missing-control`, { RT_TREE: tree, RT_MISSING: '1', CW_TEST_MISSING_LAST_TREE: '1' }, ['--allow-skipped']);
      } catch (e) {
        check(false, `${tag} ${tree}: ${e.message}`);
        continue;
      }
      const lines = (r) => text(r, 'cw.three.missing_states').split('\n').filter(Boolean);
      const world = (l) => l.startsWith('toon') && l.includes('xcsm'), mainT = (l) => l.startsWith('toon') && !l.includes('xcsm');
      const m = lines(miss), k = lines(ctl);
      if (!m.length && v(miss, 'cw.three.missing') > 0) {
        // (a host that leaves the 16 KB bytes field out of its state, romdev: the counts tell the same)
        const n = v(miss, 'cw.three.missing'), nc = v(ctl, 'cw.three.missing');
        check(n >= 3 && nc < n, `${tag} ${tree}: the world's and the main tree's skipped states both counted (${n}; the last tree's list only: ${nc})`);
        continue;
      }
      check(m.some(world) && m.some(mainT) && v(miss, 'cw.three.missing') === m.length,
        `${tag} ${tree}: the world's and the main tree's skipped states both reported (${m.join(', ')}; count ${v(miss, 'cw.three.missing')})`);
      check(!k.some(world), `${tag} ${tree}: CONTROL, the last tree's list only, loses the world's (${k.join(', ')})`);
    }
  }
console.log(fail ? `runtime-nodes: ${fail} failures` : 'ok   runtime-nodes');
process.exit(fail ? 1 : 0);
