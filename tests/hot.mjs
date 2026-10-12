// Native hot reload: run hot_demo under cwhot for 120 frames and swap to code
// built with twice the spin speed at frame 60. The node keeps its state
// (ticks run on to 120, the angle continues) and only the rate changes.
// Control: the same run without the swap must end elsewhere.
import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const B = join(ROOT, 'build/native');
const b = spawnSync('ninja', ['-C', B, 'cwhot', 'hot_demo_hot.so', 'hot_demo_hot2.so'], { encoding: 'utf8' });
if (b.status !== 0) { console.log(b.stdout + b.stderr); process.exit(1); }
const run = (swap) => {
  const state = join(ROOT, 'out', `hot-${swap ? 'swap' : 'plain'}.json`);
  const env = { ...process.env, CW_FRAMES: '120', CW_DEBUG_DUMP: state };
  if (swap) Object.assign(env, { CW_HOT_SWAP_AT: '60', CW_HOT_SWAP_PATH: join(B, 'samples/hot_demo_hot2.so') });
  const r = spawnSync(join(B, 'cwhot'), [join(B, 'samples/hot_demo_hot.so')], { encoding: 'utf8', env });
  if (r.status !== 0) throw new Error(r.stderr);
  return JSON.parse(readFileSync(state, 'utf8'));
};
const swapped = run(true), plain = run(false);
let fail = 0;
const f32 = Math.fround;
// frames 0..59 run ticks with the old code; the swap happens before frame 60's ticks
let expect = 0;
for (let t = 0; t < 120; t++) expect = f32(expect + (t < 60 ? f32(0.01) : f32(0.02)));
if (swapped['spin.ticks'] !== 120) { fail++; console.log('FAIL ticks', swapped['spin.ticks']); }
if (swapped['spin.angle'] !== expect) { fail++; console.log('FAIL angle', swapped['spin.angle'], 'expected', expect); }
if (swapped['spin.speed'] !== f32(0.02)) { fail++; console.log('FAIL the new code did not run', swapped['spin.speed']); }
// both generations draw with hot_demo's program tables, which cwhot registers from the module
const missing = (st) => Buffer.from(String(st['cw.three.missing_states'] || ''), 'hex').toString('latin1').replace(/\0.*$/s, '').split('\n').filter(Boolean);
for (const [name, st] of [['swapped', swapped], ['plain', plain]]) {
  const m = missing(st);
  if (st['cw.three.missing'] !== 0 || m.length) { fail++; console.log(`FAIL ${name}: program states in none of the module's tables (draws skipped): ${m.join(', ') || st['cw.three.missing']}`); }
}
if (plain['spin.angle'] === swapped['spin.angle']) { fail++; console.log('CONTROL PASSED: no swap gave the same angle'); }
console.log(fail ? `hot: ${fail} failures` : `ok   hot (swap at frame 60 kept 120 ticks of state, angle ${swapped['spin.angle']} vs ${plain['spin.angle']} without the swap)`);
process.exit(fail ? 1 : 0);
