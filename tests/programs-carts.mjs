// tools/programs.mjs --carts surveys an app's DUAL cart on a GL host and on a WebGPU host, so the states a cart is
// asked for (which a native run is not) reach the app's table.
//   run:      unit_programs_missing leaves a light vector its table lacks: the survey must report it
//   control:  noise_demo (fully covered: its gate fails on a missing state) through the same survey must report
//             nothing, so a survey that reports everything (or the run's own noise) cannot pass the first check; both
//             must say every run was surveyed, so "nothing" there means a real empty answer
//   must fail (a survey that surveyed nothing once read as "0 states" and exited 0):
//             hello draws with three.c alone (no engine pipeline, so no state list to read); a cart that cannot load
//             (junk bytes in a project's cart tree); a project tree with no carts at all. Each must exit non-zero
//             naming the app and the host, or the directory it looked in.
// Needs the native tree (the app list and tables) and build/cart-wgpu/carts (scripts/build.sh), and the romdev server.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
for (const c of ['unit_programs_missing', 'noise_demo', 'hello'])
  if (!existsSync(join(ROOT, 'build', 'cart-wgpu', 'carts', `${c}.wasc`))) { console.error(`FAIL build the dual cart ${c} first (scripts/build.sh)`); process.exit(1); }
let bad = 0;
const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : ': ' + extra}`); if (!ok) bad++; };
const SURVEY = join(ROOT, 'out', 'programs-survey.json');
const survey = (apps, env = {}) => {
  rmSync(SURVEY, { force: true }); // a survey that exits early must not be read as the last one's answer
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'programs.mjs'), '--carts', ...apps, '--frames', '30', '--dry'],
    { encoding: 'utf8', timeout: 900000, env: { ...process.env, ...env } });
  let out = null;
  try { out = JSON.parse(readFileSync(SURVEY, 'utf8')); } catch {}
  return { r, out, text: r.stdout + r.stderr, states: (out || {})[apps[0]] || [] };
};
const a = survey(['unit_programs_missing']);
check('the survey of the leaving cart exits 0', a.r.status === 0, a.text.slice(-400));
check('it surveyed both hosts', /unit_programs_missing: .*\(2 of 2 run\(s\) surveyed\)/.test(a.text), a.text.slice(-400));
check('it finds the light vector the table lacks', a.states.some((s) => /^standard\b/.test(s) && /l1[0-9]1[0-9]|l[0-9]+/.test(s)), JSON.stringify(a.states));
const c = survey(['noise_demo']);
check('CONTROL: a fully covered cart is surveyed on both hosts and reports no state',
  c.r.status === 0 && c.out !== null && c.states.length === 0 && /noise_demo: 0 state.*\(2 of 2 run\(s\) surveyed\)/.test(c.text), JSON.stringify(c.states) + c.text.slice(-300));
// must fail: no engine pipeline in the cart, so nothing was surveyed
const h = survey(['hello']);
check('MUST FAIL: a cart with no state list to read (hello) exits non-zero, names the app and both hosts, writes no survey',
  h.r.status !== 0 && h.out === null && /hello on the gl host/.test(h.r.stderr) && /hello on the webgpu host/.test(h.r.stderr), `exit ${h.r.status}: ${h.text.slice(-400)}`);
// must fail: a project tree (CW_PROJECT_ROOT) whose cart does not load
const proj = join(ROOT, 'out', 'programs-carts-fixture');
rmSync(proj, { recursive: true, force: true });
mkdirSync(join(proj, 'build', 'cart-wgpu', 'carts'), { recursive: true });
writeFileSync(join(proj, 'build', 'cart-wgpu', 'carts', 'noise_demo.wasc'), 'not a cart\n');
const j = survey(['noise_demo'], { CW_PROJECT_ROOT: proj });
check('MUST FAIL: a cart that does not load exits non-zero and names the app and host',
  j.r.status !== 0 && /noise_demo on the (gl|webgpu) host: run failed/.test(j.r.stderr), `exit ${j.r.status}: ${j.text.slice(-400)}`);
// must fail: nothing to survey (a project's carts looked for in a tree that has none)
rmSync(join(proj, 'build', 'cart-wgpu', 'carts', 'noise_demo.wasc'));
const e = survey([], { CW_PROJECT_ROOT: proj });
check('MUST FAIL: no carts to survey exits non-zero and names the directory',
  e.r.status !== 0 && e.text.includes(join(proj, 'build', 'cart-wgpu', 'carts')), `exit ${e.r.status}: ${e.text.slice(-400)}`);
rmSync(proj, { recursive: true, force: true });
process.exit(bad ? 1 : 0);
