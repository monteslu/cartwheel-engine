// tools/programs.mjs --carts surveys an app's DUAL cart on a GL host and on a WebGPU host, so the states a cart is
// asked for (which a native run is not) reach the app's table.
//   run:      unit_programs_missing leaves a light vector its table lacks: the survey must report it
//   control:  hello (fully covered) through the same survey must report nothing, so a survey that reports
//             everything (or the run's own noise) cannot pass the first check
// Needs the native tree (the app list and tables) and build/cart-wgpu/carts (scripts/build.sh), and the romdev server.
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
for (const c of ['unit_programs_missing', 'hello'])
  if (!existsSync(join(ROOT, 'build', 'cart-wgpu', 'carts', `${c}.wasc`))) { console.error(`FAIL build the dual cart ${c} first (scripts/build.sh)`); process.exit(1); }
let bad = 0;
const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : ': ' + extra}`); if (!ok) bad++; };
const survey = (app) => {
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'programs.mjs'), '--carts', app, '--frames', '30', '--dry'], { encoding: 'utf8', timeout: 900000 });
  let out = {};
  try { out = JSON.parse(readFileSync(join(ROOT, 'out', 'programs-survey.json'), 'utf8')); } catch {}
  return { r, states: out[app] || [] };
};
const a = survey('unit_programs_missing');
check('the survey of the leaving cart exits 0', a.r.status === 0, (a.r.stderr + a.r.stdout).slice(-400));
check('it finds the light vector the table lacks', a.states.some((s) => /^standard\b/.test(s) && /l1[0-9]1[0-9]|l[0-9]+/.test(s)), JSON.stringify(a.states));
const c = survey('hello');
check('CONTROL: a fully covered cart reports no state', c.r.status === 0 && c.states.length === 0, JSON.stringify(c.states) + (c.r.stdout + c.r.stderr).slice(-300));
process.exit(bad ? 1 : 0);
