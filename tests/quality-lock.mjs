// A test host with a real clock must not let Auto change the picture: tools/run-cart.mjs locks quality on Chromium and
// wasmcart-native. Asking for Auto after the lock leaves Chromium on a preset; the same run with --keep-auto reaches Auto
// (the control, and what a game on Auto got there before). wasmcart-native reports no state, so its log must show the lock.
import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out');
const MODE_AUTO = 2; // cw.quality_mode
const run = (host, tag, extra) => {
  const state = join(OUT, `quality-lock-${tag}.json`), log = join(OUT, `quality-lock-${tag}.log`);
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', host, '--app', 'hello', '--frames', '6', '--shot', join(OUT, `quality-lock-${tag}.png`),
    ...(host === 'chromium' ? ['--state', state] : ['--log', log]), ...extra], { encoding: 'utf8', env: process.env });
  if (r.status !== 0) { console.log(r.stdout + r.stderr); throw new Error(`${tag}: the run failed`); }
  return host === 'chromium' ? JSON.parse(readFileSync(state, 'utf8')) : readFileSync(log, 'utf8');
};
let fail = 0;
const locked = run('chromium', 'locked', ['--start-cmds', 'quality auto']);
const control = run('chromium', 'control', ['--start-cmds', 'quality auto', '--keep-auto']);
if (locked['cw.quality_mode'] === MODE_AUTO) { fail++; console.log('FAIL Chromium reached Auto through the lock'); }
if (control['cw.quality_mode'] !== MODE_AUTO) { fail++; console.log(`CONTROL PASSED: --keep-auto did not reach Auto (mode ${control['cw.quality_mode']})`); }
const wc = run('wcnative', 'wcnative', []);
if (!/quality: locked for a test run/.test(wc)) { fail++; console.log('FAIL wasmcart-native: no lock in the log'); }
console.log(fail ? `quality-lock: ${fail} failures` : 'ok   quality-lock (Chromium stayed off Auto, control reached it, wasmcart-native locked)');
process.exit(fail ? 1 : 0);
