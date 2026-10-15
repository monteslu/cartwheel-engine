// Four game controllers reach a native game through SDL, each in its own pad
// slot and to its own player: virtual controllers (tools/vpad.c, uinput) and
// input_demo, run natively with CW_TEST_PADS=1 (a CW_FRAMES run otherwise
// ignores the machine's controllers) and no input script, read through its
// debug state (cw.pad<N>.*, the platform's pads; players.*, the actions each
// player saw).
//   1. two controllers present at start: pads 0 and 1, pads 2 and 3 empty.
//      SDL reports a controller present at start twice (the device list and
//      a queued SDL_CONTROLLERDEVICEADDED); both were opened, so pad 0 came
//      back as pad 2 and pad 1 as pad 3, and a third controller found the
//      slots full.
//   2. four: each its own buttons and stick, each its own player.
// CONTROL: the four controllers without CW_TEST_PADS must not be seen (the
// check would pass on a run that never read a controller otherwise).
//
// Linux only; needs write access to /dev/uinput (tools/lib/vpad.mjs).
//   node tests/native-pads.mjs
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { openVpad, VPAD_ENV } from '../tools/lib/vpad.mjs';
await enterGpuSandbox();
await takeSuiteLock('native-pads.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'native-pads');
mkdirSync(OUT, { recursive: true });
const FRAMES = 30;
let failed = 0;

function runNative(name, env) {
  const state = join(OUT, `${name}.json`);
  return new Promise((res) => {
    const p = spawn('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--app', 'input_demo', '--frames', String(FRAMES),
      '--state', state, '--shot', join(OUT, `${name}.png`)], { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...VPAD_ENV, ...env } });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => res(code === 0 ? JSON.parse(readFileSync(state, 'utf8')) : { error: err.trim().split('\n').slice(-3).join(' | ') }));
  });
}

const pads = (s, k) => [0, 1, 2, 3].map((i) => s[`cw.pad${i}.${k}`]);
function check(name, s, want) {
  if (s.error) { failed++; console.log(`FAIL ${name}: the run failed: ${s.error}`); return; }
  const got = { connected: pads(s, 'connected'), buttons: pads(s, 'buttons'), lx: pads(s, 'axes').map((a) => a && a[0]),
    jump_jp: s['players.jump_jp'], axis: s['players.axis'] };
  const bad = Object.keys(want).filter((k) => JSON.stringify(got[k]) !== JSON.stringify(want[k]));
  if (bad.length) { failed++; console.log(`FAIL ${name}: ${bad.map((k) => `${k} ${JSON.stringify(got[k])}, want ${JSON.stringify(want[k])}`).join('; ')}`); }
  else console.log(`ok   ${name}: ${JSON.stringify(got)}`);
}

const v = await openVpad(OUT);
try {
  // plugged in one at a time (two at once reach SDL in either order), then let udev settle
  await v.send('add 0', 'wait 300', 'add 1', 'wait 300', 'btn 0 south 1', 'btn 1 east 1', 'wait 700');
  check('two controllers at start: pads 0 and 1 only', await runNative('two', { CW_TEST_PADS: '1' }),
    { connected: [1, 1, 0, 0], buttons: [1, 2, 0, 0], jump_jp: [1, 0, 0, 0] });
  await v.send('add 2', 'wait 300', 'add 3', 'wait 300', 'btn 2 west 1', 'btn 3 north 1', 'btn 3 south 1', 'abs 3 lx -32768', 'wait 700');
  check('four controllers: each its own pad and player', await runNative('four', { CW_TEST_PADS: '1' }),
    { connected: [1, 1, 1, 1], buttons: [1, 2, 4, 9], lx: [0, 0, 0, -1], jump_jp: [1, 0, 0, 1], axis: [0, 0, 0, -FRAMES] });
  // CONTROL: the same controllers, CW_TEST_PADS unset: the test run's neutral pads
  const c = await runNative('control', { CW_TEST_PADS: '0' });
  if (c.error || JSON.stringify(pads(c, 'connected')) !== '[1,0,0,0]') {
    failed++;
    console.log(`FAIL control: a run without CW_TEST_PADS must see only the neutral pad 0 (${c.error || JSON.stringify(pads(c, 'connected'))})`);
  } else console.log('ok   control: without CW_TEST_PADS the controllers are not read');
} finally {
  v.close();
  await v.exited;
}
process.exit(failed ? 1 : 0);
