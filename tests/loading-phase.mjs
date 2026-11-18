// A level loaded in play under the loading screen (cw_loading_begin, samples/loading_phase): the sample starts a load of
// 12 pieces at tick 60 by itself, its step building one piece a frame. Through romdev, on GL and WebGPU:
//   - as a player gets it (the warm-up spread): the loading screen shows for at least the step's 12 frames, the phase
//     ends, the level is whole, no tick runs while it loads and the bar never steps back (the step's half, then the
//     scene runner's shader phase);
//   - as a test runs it (a deterministic load, no spread): the whole phase inside one frame (the frame after the start
//     sees it, then it is over), so the ticks line up with a
//     level built in its tick (LP_SYNC=1) exactly.
// Control: LP_SYNC=1, the same level built in its tick with no loading phase (the one long frozen frame the phase
// replaces), must fail the player run's loading-screen bound; CW_TEST_LOADING_NO_SHARE=1, the step's numbers shown as it
// wrote them (12 of 12 is the whole bar) before the shader phase's second half, must step the bar back.
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..'), OUT = join(ROOT, 'out/loading-phase');
mkdirSync(OUT, { recursive: true });
const gpus = process.argv.includes('--gpu') ? [process.argv[process.argv.indexOf('--gpu') + 1]] : ['gl', 'webgpu'];
const N = 12, FRAMES = 150;
function run(gpu, tag, spread, sync, extra = {}) {
  const state = join(OUT, `${gpu}-${tag}.json`);
  const env = { LP_LOAD_AT: '60', LP_LOAD_N: String(N), ...(sync ? { LP_SYNC: '1' } : {}), ...extra };
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'romdev', '--gpu', gpu, '--app', 'loading_phase', '--frames', String(FRAMES),
    ...(spread ? ['--warmup-spread'] : []), '--env', JSON.stringify(env), '--state', state, '--shot', state.replace(/json$/, 'png')],
  { encoding: 'utf8', cwd: ROOT, maxBuffer: 1 << 26 });
  if (r.status !== 0) { console.error(r.stdout.slice(-400), r.stderr.slice(-400)); process.exit(3); }
  return JSON.parse(readFileSync(state, 'utf8'));
}
const player = s => s['lp.frames'] >= N && s['lp.done'] === 1 && s['lp.pieces'] === N && s['lp.ticks_loading'] === 0 && s['lp.back'] === 0;
const desc = s => `${s['lp.frames']} loading frame(s), ${s['lp.done']} phase(s) done, ${s['lp.pieces']}/${N} pieces, ${s['lp.ticks_loading']} tick(s) while loading, bar back ${s['lp.back']}x, ${s['lp.tick']} ticks`;
let fail = 0;
for (const gpu of gpus) {
  const a = run(gpu, 'player', true, false), t = run(gpu, 'test', false, false), ts = run(gpu, 'test-sync', false, true), c = run(gpu, 'control', true, true);
  const cb = run(gpu, 'control-bar', true, false, { CW_TEST_LOADING_NO_SHARE: '1' });
  const okA = player(a), okT = t['lp.pieces'] === N && t['lp.frames'] <= 1 && t['lp.done'] === 1 && t['lp.tick'] === ts['lp.tick'] && ts['lp.pieces'] === N, okC = !player(c), okCB = cb['lp.back'] > 0;
  console.log(`${okA ? 'ok  ' : 'FAIL'} loading-phase ${gpu} player: ${desc(a)}`);
  console.log(`${okT ? 'ok  ' : 'FAIL'} loading-phase ${gpu} test run: ${desc(t)}; built in its tick: ${ts['lp.tick']} ticks (equal wanted)`);
  console.log(`${okC ? 'ok  ' : 'FAIL'} loading-phase ${gpu} control (LP_SYNC=1, built in one frame): ${desc(c)}, must fail the player bound`);
  console.log(`${okCB ? 'ok  ' : 'FAIL'} loading-phase ${gpu} control (CW_TEST_LOADING_NO_SHARE=1): bar back ${cb['lp.back']}x, at least once wanted`);
  if (!okA || !okT || !okC || !okCB) fail++;
}
process.exit(fail ? 1 : 0);
