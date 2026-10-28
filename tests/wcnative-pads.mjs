// Four game controllers reach a cart on wasmcart-native (wasmcart-run), each in
// its own pad slot and to its own player, plugged in and out while it runs:
// virtual controllers (tools/vpad.c, uinput), the dual input_demo cart run on
// GL (WASMCART_NO_WGPU=1), and the player's --debug-dump at four frames of
// one run (it paces to 60 Hz, so the controllers change between dumps). The
// whole path: SDL on the machine -> wasmcart-run's pad slots -> wc_pad_t ->
// the cart's SDL (wasmcart-sdl2) -> the engine's pads (cw.pad<N>.*) -> each
// player's actions (players.*).
//   1. two controllers at start: pads 0 and 1, pads 2 and 3 empty (the player
//      opened a controller present at start twice: pad 0 came back as pad 2)
//   2. two more plugged in: pads 2 and 3, each its own buttons and player
//   3. pads 1 and 2 unplugged: empty; pad 3 stays pad 3 (and player 4)
//   4. one plugged in again: the free slot, pad 1
// CONTROL: SDL told to take no controller at all: the cart sees no pad
// buttons and nothing in pads 1-3, so the checks above can tell a controller
// from none.
//
// Linux only; needs write access to /dev/uinput (tools/lib/vpad.mjs) and a
// wasmcart-native build (tools/lib/wcnative.mjs) that takes --debug-dump more
// than once (0.12.0 and later).
//   node tests/wcnative-pads.mjs
import { spawn } from 'node:child_process';
import { readFileSync, existsSync, mkdirSync, rmSync, copyFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { setTimeout as sleep } from 'node:timers/promises';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv, scrubDisplay } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { wcnativePlayer } from '../tools/lib/wcnative.mjs';
import { openVpad, VPAD_ENV } from '../tools/lib/vpad.mjs';
await enterGpuSandbox();
await takeSuiteLock('wcnative-pads.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'wcnative-pads');
mkdirSync(OUT, { recursive: true });
const CART = join(ROOT, 'build/cart-wgpu/carts/input_demo.wasc');
if (!existsSync(CART)) { console.error(`missing ${CART}: run scripts/build.sh`); process.exit(1); }
const DUMPS = [60, 240, 420, 600];
let failed = 0;

/** start wasmcart-run with dumps at DUMPS; returns { dump(i), stop() } */
function start(name, env) {
  const dir = join(OUT, name);
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  copyFileSync(CART, join(dir, 'input_demo.wasc')); // its save and log land beside it
  const files = DUMPS.map((f) => join(dir, `dump${f}.json`));
  const e = { ...process.env, SDL_VIDEODRIVER: 'offscreen', SDL_AUDIODRIVER: 'dummy', WASMCART_NO_WGPU: '1', ...env };
  scrubDisplay(e);
  const p = spawn(wcnativePlayer(), [join(dir, 'input_demo.wasc'), ...DUMPS.flatMap((f, i) => ['--debug-dump', String(f), files[i]])],
    { stdio: ['ignore', 'ignore', 'pipe'], env: e });
  let log = '';
  p.stderr.on('data', (d) => (log += d));
  let ended = false;
  p.on('close', () => (ended = true));
  return {
    async dump(i) { /* the dump for DUMPS[i], once written (up to 30 s) */
      for (let t = 0; t < 300 && !existsSync(files[i]); t++) { if (ended) break; await sleep(100); }
      await sleep(100); // written in one go; let it close
      if (!existsSync(files[i])) return { error: `no dump at frame ${DUMPS[i]}: ${log.trim().split('\n').slice(-3).join(' | ')}` };
      return JSON.parse(readFileSync(files[i], 'utf8'));
    },
    stop() { if (!ended) p.kill('SIGKILL'); },
  };
}

const pads = (s, k) => [0, 1, 2, 3].map((i) => s[`cw.pad${i}.${k}`]);
function check(name, s, want) {
  if (s.error) { failed++; console.log(`FAIL ${name}: ${s.error}`); return; }
  const got = { connected: pads(s, 'connected'), buttons: pads(s, 'buttons'), jump_jp: s['players.jump_jp'] };
  const bad = Object.keys(want).filter((k) => JSON.stringify(got[k]) !== JSON.stringify(want[k]));
  if (bad.length) { failed++; console.log(`FAIL ${name}: ${bad.map((k) => `${k} ${JSON.stringify(got[k])}, want ${JSON.stringify(want[k])}`).join('; ')}`); }
  else console.log(`ok   ${name}: ${JSON.stringify(got)}`);
}

const v = await openVpad(OUT);
let run = null;
try {
  // one at a time (two plugged in together reach SDL in either order), then let udev settle
  await v.send('add 0', 'wait 300', 'add 1', 'wait 300', 'btn 0 south 1', 'btn 1 east 1', 'wait 700');
  run = start('run', VPAD_ENV);
  check('two controllers at start: pads 0 and 1 only', await run.dump(0), { connected: [1, 1, 0, 0], buttons: [1, 2, 0, 0], jump_jp: [1, 0, 0, 0] });
  await v.send('add 2', 'wait 300', 'add 3', 'wait 300', 'btn 2 west 1', 'btn 3 north 1', 'btn 3 south 1');
  check('two more plugged in: each its own pad and player', await run.dump(1), { connected: [1, 1, 1, 1], buttons: [1, 2, 4, 9], jump_jp: [1, 0, 0, 1] });
  await v.send('del 1', 'del 2');
  check('pads 1 and 2 unplugged: pad 3 stays pad 3', await run.dump(2), { connected: [1, 0, 0, 1], buttons: [1, 0, 0, 9], jump_jp: [1, 0, 0, 1] });
  await v.send('add 4', 'wait 300', 'btn 4 south 1');
  check('one plugged in again: the free slot, pad 1', await run.dump(3), { connected: [1, 1, 0, 1], buttons: [1, 1, 0, 9], jump_jp: [1, 1, 0, 1] });
  run.stop();
  // CONTROL: SDL told to take no controller: pads 1-3 empty and pad 0 (which wasmcart-run
  // connects while the keyboard plays it) without the held buttons
  run = start('control', { SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT: '0x1209/0x7765' }); // a product no controller has
  const c = await run.dump(0);
  const seen = c.error ? null : { connected: pads(c, 'connected').slice(1), buttons: pads(c, 'buttons') };
  if (!seen || JSON.stringify(seen) !== JSON.stringify({ connected: [0, 0, 0], buttons: [0, 0, 0, 0] })) {
    failed++;
    console.log(`FAIL control: with no controller taken the cart must see none (${c.error || JSON.stringify(seen)})`);
  } else console.log(`ok   control: no controller taken, the cart sees none ${JSON.stringify(seen)}`);
} finally {
  if (run) run.stop();
  v.close();
  await v.exited;
}
process.exit(failed ? 1 : 0);
