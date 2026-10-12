// a full recorded run of platformer3d replays to the
// exact same end state. Native only (carts cannot write the recording):
//   1. play the gate's input script with PLAT3D_RECORD: the action stream of
//      every tick is written at exit
//   2. run again with NO input and PLAT3D_REPLAY: the recording drives every
//      tick; every state field must equal run 1's
//   3. control: replaying another run's recording (the fall script) must NOT
//      reach the same state (the replay really drives the game)
//
//   node tests/platformer3d-replay.mjs      (after scripts/build.sh)
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
testEnv(process.env); // what it starts renders offscreen and cannot reach the desktop

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const W = join(ROOT, 'out', 'platformer3d-replay');
mkdirSync(W, { recursive: true });
const FRAMES = 2150;
const FIELDS = ['game.state', 'score', 'coins', 'gems', 'lives', 'deaths', 'checkpoint', 'timer', 'player.pos', 'player.vel',
  'cam.pos', 'props.hash', 'phys.hash', 'state.hash', 'input.hash'];
let failed = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok || !detail ? '' : ': ' + detail}`);
  if (!ok) failed++;
};
function run(name, extra, args = ['--frames', String(FRAMES)]) {
  const state = join(W, `${name}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', 'platformer3d', ...args, '--state', state],
    { env: { ...process.env, PLAT3D_NO_AUDIO: '1', ...extra }, encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`${name}: ${r.stderr}`);
  return JSON.parse(readFileSync(state, 'utf8'));
}
const same = (a, b) => FIELDS.filter((k) => JSON.stringify(a[k]) !== JSON.stringify(b[k]));

const rec = join(W, 'run.rec');
const live = run('live', { PLAT3D_RECORD: rec }, ['--frames', String(FRAMES), '--input', join(ROOT, 'tests/platformer3d-input.json')]);
check('the recorded run reaches the end screen', live['game.state'] === 5 && live.coins === 20, JSON.stringify({ state: live['game.state'], coins: live.coins }));
const replay = run('replay', { PLAT3D_REPLAY: rec });
const d = same(live, replay);
check(`replaying the recording reproduces every field (${FIELDS.length})`, d.length === 0, d.map((k) => `${k}: ${JSON.stringify(live[k])} vs ${JSON.stringify(replay[k])}`).join('; '));
// control: another run's recording replayed must end somewhere else
const other = join(W, 'fall.rec');
run('fall-live', { PLAT3D_RECORD: other }, ['--frames', String(FRAMES), '--input', join(ROOT, 'tests/platformer3d-fall.json')]);
const ctl = run('control', { PLAT3D_REPLAY: other });
check('control: replaying a different recording does not reach the same state', ctl['game.state'] === 1 && same(live, ctl).length > 0,
  JSON.stringify({ state: ctl['game.state'], lives: ctl.lives }));
console.log(failed ? `${failed} FAILED` : 'platformer3d replay: all checks passed');
process.exit(failed ? 1 : 0);
