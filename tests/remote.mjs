// Play-in-editor gate: build/native/tests/remote_test drives
// samples/remote_target through cartwheel/remote.h on each transport
// (native child process, romdev cart): launch paused, edit while paused, step,
// check the game's own state, call methods, follow the change log, layer
// screenshots, record, reload, change scenes, crash on purpose, kill,
// relaunch. Then, across runs:
//   - each transport's --control run (skips the edit, still expects its
//     effect) MUST fail: the effect check can fail;
//   - the position after "start paused at tick 1, step 30" equals a plain
//     CW_FRAMES=31 run's (stepping is the same simulation as running);
//   - both transports measure the same values.
//   node tests/remote.mjs [native|romdev ...]     ROMDEV_PORT (default 7331)
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const which = process.argv.slice(2).length ? process.argv.slice(2) : ['native', 'romdev'];
const driver = join(ROOT, 'build/native/tests/remote_test');
const game = { native: join(ROOT, 'build/native/samples/remote_target'), romdev: join(ROOT, 'build/cart/carts/remote_target.wasc') };
for (const p of [driver, ...which.map((t) => game[t])])
  if (!existsSync(p)) { console.error(`missing ${p}: run scripts/build.sh`); process.exit(1); }

let fail = 0;
const results = {};
const run = (t, control) => spawnSync(driver, [t, game[t], ...(control ? ['--control'] : [])],
  { encoding: 'utf8', timeout: 300000, env: { ...process.env, SDL_VIDEODRIVER: 'offscreen', SDL_AUDIODRIVER: 'dummy' } });
for (const t of which) {
  const r = run(t, false);
  const out = (r.stdout || '') + (r.stderr || '');
  const last = out.trim().split('\n').reverse().find((l) => l.startsWith('{"transport"'));
  if (r.status !== 0 || !last) {
    fail++;
    console.log(`FAIL remote ${t} (exit ${r.status}${r.signal ? ', ' + r.signal : ''})\n${out.split('\n').filter((l) => /FAIL|launch/.test(l)).join('\n') || out.slice(-2000)}`);
  } else {
    results[t] = JSON.parse(last);
    console.log(`ok   remote ${t}: ${results[t].checks} checks (launch paused, edit, step, call, changes, layers, record, reload, scenes, crash, kill, relaunch)`);
  }
  const c = run(t, true);
  const cfails = ((c.stdout || '').match(/^FAIL .*remote_test\.c/mg) || []).length;
  if (c.status === 0 || cfails === 0) {
    fail++;
    console.log(`CONTROL PASSED: remote ${t} --control (no edit) still saw the edit's effect`);
  } else {
    console.log(`ok   remote ${t} control: the unedited run fails ${cfails} effect checks, as it must`);
  }
}

// stepping a paused game is the same simulation as running it
const ref = spawnSync(game.native, [], { encoding: 'utf8', timeout: 60000,
  env: { ...process.env, SDL_VIDEODRIVER: 'offscreen', SDL_AUDIODRIVER: 'dummy', CW_FRAMES: '31', CW_DEBUG_CMDS: 'get Level/Mover position' } });
const m = /vec2\(([-\d.e]+), ([-\d.e]+)\)/.exec(ref.stdout || '');
const refX = m ? +m[1] : NaN;
// values travel as text (cw_value_format: enough digits to round-trip a
// float), so compare them as the floats they are
const f32 = Math.fround;
let stepFails = 0;
for (const t of Object.keys(results)) {
  if (f32(results[t].x_fresh_step) !== f32(refX)) {
    stepFails++;
    fail++;
    console.log(`FAIL remote ${t}: paused + step 30 gives x ${results[t].x_fresh_step}, a 31-frame run gives ${refX}`);
  }
}
if (Object.keys(results).length && !Number.isNaN(refX) && !stepFails) console.log(`ok   remote: paused + step 30 == a 31-frame run (x ${refX})`);
// control: one frame fewer must NOT match (the comparison can fail)
const short = spawnSync(game.native, [], { encoding: 'utf8', timeout: 60000,
  env: { ...process.env, SDL_VIDEODRIVER: 'offscreen', SDL_AUDIODRIVER: 'dummy', CW_FRAMES: '30', CW_DEBUG_CMDS: 'get Level/Mover position' } });
const ms = /vec2\(([-\d.e]+), /.exec(short.stdout || '');
if (!ms || f32(+ms[1]) === f32(refX)) { fail++; console.log(`CONTROL PASSED: a 30-frame run matches the 31-frame one (${ms && ms[1]})`); }
else console.log(`ok   remote control: a 30-frame run differs (x ${ms[1]})`);
if (Number.isNaN(refX)) { fail++; console.log(`FAIL remote: the CW_FRAMES reference run gave no position: ${ref.stdout} ${ref.stderr}`); }
if (results.native && results.romdev) {
  for (const k of ['x_after_edit', 'x_fresh_step']) {
    if (f32(results.native[k]) !== f32(results.romdev[k])) { fail++; console.log(`FAIL remote: ${k} native ${results.native[k]} vs romdev ${results.romdev[k]}`); }
  }
  if (f32(results.native.x_after_edit) === f32(results.romdev.x_after_edit)) console.log('ok   remote: native and romdev measure the same state');
}
console.log(fail ? `remote: ${fail} failures` : 'ok   remote');
process.exit(fail ? 1 : 0);
