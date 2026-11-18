// In a cart the wasmcart host draws the on-screen pad (cartwheel/input.h, "touch controls"): the engine's own touch
// controls claim no touch and draw nothing. samples/input_demo has a thumbstick (touch:0, bottom left) and a round
// button (touch:1, bound to jump) at 560,300 in its 640x360 window. Through romdev, on GL and WebGPU, a finger held on
// the button's place for 6 frames, then one in the stick's zone for 6 frames:
//   - the cart: the button and the stick never active, jump never pressed, every touch frame the game's own
//     (ui.free_touch_frames 12), nothing drawn (cw.touch_overlay 0, cw_touch_overlay_visible never true);
//   - the control, CW_TEST_TOUCH_CONTROLS=1 (the controls kept on in a cart: the old behaviour): the tap must fire
//     the button and jump, so the check above fails on it.
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..'), OUT = join(ROOT, 'out/touch-host-pad');
mkdirSync(OUT, { recursive: true });
const gpus = process.argv.includes('--gpu') ? [process.argv[process.argv.indexOf('--gpu') + 1]] : ['gl', 'webgpu'];
const FRAMES = 40, BUTTON = { x: 560, y: 300 }, STICK = { x: 100, y: 280 };
const script = join(OUT, 'input.json');
writeFileSync(script, JSON.stringify([
  { frame: 10, touches: [{ id: 1, ...BUTTON }] }, { frame: 16 },
  { frame: 20, touches: [{ id: 2, ...STICK }] }, { frame: 26 },
]));
function run(gpu, tag, env = {}) {
  const state = join(OUT, `${gpu}-${tag}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'romdev', '--gpu', gpu, '--app', 'input_demo', '--frames', String(FRAMES),
    '--input', script, ...(Object.keys(env).length ? ['--env', JSON.stringify(env)] : []), '--state', state, '--shot', state.replace(/json$/, 'png')],
  { encoding: 'utf8', cwd: ROOT, maxBuffer: 1 << 26 });
  if (r.status !== 0) { console.error(r.stdout.slice(-400), r.stderr.slice(-400)); process.exit(3); }
  return JSON.parse(readFileSync(state, 'utf8'));
}
const hostPad = (s) => s['ui.touch_button'] === 0 && s['ui.stick_frames'] === 0 && s['ui.jump_jp'] === 0 && s['ui.free_touch_frames'] === 12
  && s['cw.touch_overlay'] === 0 && s['ui.overlay_frames'] === 0;
const desc = (s) => `button ${s['ui.touch_button']} frame(s), stick ${s['ui.stick_frames']}, jump pressed ${s['ui.jump_jp']}x, ` +
  `game's own touch ${s['ui.free_touch_frames']} frame(s), overlay drawn ${s['cw.touch_overlay']} control(s) (visible ${s['ui.overlay_frames']} frame(s))`;
let fail = 0;
for (const gpu of gpus) {
  const a = run(gpu, 'cart'), c = run(gpu, 'control', { CW_TEST_TOUCH_CONTROLS: '1' });
  const okA = hostPad(a), okC = !hostPad(c) && c['ui.touch_button'] > 0 && c['ui.jump_jp'] > 0;
  console.log(`${okA ? 'ok  ' : 'FAIL'} touch-host-pad ${gpu} cart: ${desc(a)}`);
  console.log(`${okC ? 'ok  ' : 'FAIL'} touch-host-pad ${gpu} control (CW_TEST_TOUCH_CONTROLS=1, the controls on): ${desc(c)}, the tap must fire the button`);
  if (!okA || !okC) fail++;
}
process.exit(fail ? 1 : 0);
