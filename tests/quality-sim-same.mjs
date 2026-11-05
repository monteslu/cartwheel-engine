// Nothing in the graphics settings reaches the simulation (cartwheel/quality.h): platformer3d_auto plays the same
// replay at Very Low and at High, natively on GL and WebGPU, and every simulation field (state, physics and props
// hashes, the hero's position and velocity, the level's ticks, score) is the same bits. Control:
// CW_TEST_QUALITY_INTO_SIM=1 feeds the preset into the game's tick (games/platformer3d/main.c), and the gate must see it.
//   node tests/quality-sim-same.mjs [--frames 900]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'quality-sim-same');
mkdirSync(OUT, { recursive: true });
const fi = process.argv.indexOf('--frames');
const FRAMES = fi > 0 ? process.argv[fi + 1] : '900';
const FIELDS = ['state.hash', 'phys.hash', 'props.hash', 'player.pos', 'player.vel', 'level.ticks', 'score', 'coins', 'ticks', 'game.level'];
function run(gpu, preset, extra = {}) {
  const state = join(OUT, `${gpu}-${preset}${extra.CW_TEST_QUALITY_INTO_SIM ? '-control' : ''}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'platformer3d_auto', '--frames', FRAMES, '--state', state],
    { cwd: ROOT, encoding: 'utf8', env: { ...process.env, CW_QUALITY: preset, ...extra }, timeout: 600e3 });
  if (r.status !== 0) throw new Error(`${gpu} ${preset}: ${(r.stderr || r.stdout).slice(-600)}`);
  const s = JSON.parse(readFileSync(state, 'utf8'));
  return Object.fromEntries(FIELDS.map((k) => [k, JSON.stringify(s[k])]));
}
const diff = (a, b) => FIELDS.filter((k) => a[k] !== b[k]);
let fail = 0;
for (const gpu of ['gl', 'webgpu']) {
  const hi = run(gpu, 'high'), vl = run(gpu, 'verylow');
  if (hi['state.hash'] === undefined || hi['ticks'] === undefined) { fail++; console.log(`FAIL ${gpu}: the run reported no simulation state`); continue; }
  const d = diff(hi, vl);
  if (d.length) { fail++; console.log(`FAIL ${gpu}: Very Low and High simulate differently: ${d.map((k) => `${k} ${hi[k]} vs ${vl[k]}`).join('; ')}`); }
  else console.log(`ok   ${gpu}: Very Low and High give the same ${FIELDS.length} simulation fields (state.hash ${hi['state.hash']})`);
  const c = diff(hi, run(gpu, 'verylow', { CW_TEST_QUALITY_INTO_SIM: '1' }));
  if (!c.length) { fail++; console.log(`CONTROL PASSED (${gpu}): the preset fed into the tick went unseen`); }
  else console.log(`ok   control (${gpu}): the preset fed into the tick changes ${c.join(', ')}`);
}
console.log(fail ? `quality-sim-same: ${fail} failures` : 'ok   quality-sim-same');
process.exit(fail ? 1 : 0);
