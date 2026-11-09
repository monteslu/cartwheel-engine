// Every gate's environment reaches its cart on romdev. A gate's env and controlEnv (and webgpu.controls' env) go to a
// cart as the debug channel's env request, which romdev answers at the start of frame 2: a game or engine switch read
// in init or the first frame would run unset there, and the request fails the run naming it (cartwheel/debug.h,
// cw_test_env). This runs each such gate's cart once on romdev (GL) with each environment its gate gives, for the
// gate's frames, and fails on every run the cart refused or could not finish.
//   node tests/env-romdev.mjs [gate ...]
// Needs build/cart-wgpu/carts (scripts/build.sh) and the romdev server. CONTROL that must fail: unit_core given
// CW_TEST_EARLY_READ, which every unit suite reads in its init on purpose (tests/unit/test.h), must be refused.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'env-romdev');
mkdirSync(OUT, { recursive: true });
const only = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const gates = JSON.parse(readFileSync(join(ROOT, 'tests', 'gates.json'), 'utf8'));
const runs = [];
for (const g of gates) {
  if (!g.app || g.script || (only.length && !only.includes(g.name))) continue;
  if (g.hosts && !g.hosts.includes('romdev')) continue;
  if ((g.skip || []).includes('romdev')) continue;
  const envs = [['env', g.env], ['controlEnv', g.controlEnv], ...((g.webgpu && g.webgpu.controls) || []).map((c, i) => [`webgpu.controls[${i}]`, c.env])]
    .filter(([, e]) => e && Object.keys(e).length && !Object.values(e).some((v) => typeof v === 'string' && v.startsWith('repo:')));
  for (const [what, env] of envs) runs.push({ gate: g.name, app: g.app, frames: g.frames, what, env, input: g.input, startCmds: g.startCmds });
}
let failed = 0;
runs.push({ gate: 'control', app: 'unit_core', frames: 2, what: 'read in init', env: { CW_TEST_EARLY_READ: '1' }, control: true });
for (const r of runs) {
  // an app with no cart target (a native test binary such as unit_pipeline) has no romdev leg
  if (!existsSync(join(ROOT, 'build', 'cart-wgpu', 'carts', `${r.app}.wasc`))) { console.log(`skip ${r.gate} ${r.what}: no cart ${r.app}.wasc (native only, or build the cart tree)`); continue; }
  const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'romdev', '--gpu', 'gl', '--dual-gl', '--app', r.app, '--frames', String(r.frames),
    '--env', JSON.stringify(r.env), '--allow-skipped', '--state', join(OUT, `${r.gate}.${r.what}.json`)];
  if (r.input) args.push('--input', join(ROOT, 'tests', r.input));
  if (r.startCmds) args.push('--start-cmds', r.startCmds.join(';'));
  const p = spawnSync(process.execPath, args, { encoding: 'utf8', timeout: 600000 });
  const why = p.status === 0 ? '' : (p.stderr || p.stdout || '').trim().split('\n').filter((l) => /error|read|refus|env/i.test(l)).slice(-2).join(' | ') || `exit ${p.status}`;
  if (r.control) {
    if (why) console.log(`ok   control: a variable read in init is refused (${why.slice(0, 200)})`);
    else { failed++; console.log('FAIL CONTROL PASSED: a variable read in init was taken'); }
  } else if (why) { failed++; console.log(`FAIL ${r.gate} ${r.what} ${JSON.stringify(r.env)}: ${why.slice(0, 400)}`); }
  else console.log(`ok   ${r.gate} ${r.what} ${JSON.stringify(r.env)}`);
}
console.log(`${failed} failed of ${runs.length} runs (the control included)`);
process.exit(failed ? 1 : 0);
