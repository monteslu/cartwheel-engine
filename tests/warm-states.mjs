// The warm-up draws only what play draws (samples/warm_states): the dual cart loaded through romdev as a player's host
// runs it (run-cart --warmup-spread: no seed, the warm-up's shader phase spread over frames, several slices a frame)
// reports no program state its table lacks, on GL and WebGPU, after the warm-up and into play. Control:
// CW_TEST_WARM_NO_PRERENDER=1 drops the pre-render of a frame's later slices (as before the fix): they draw the cascaded
// sun as a plain shadowed light, and states no table has must show up.
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..'), OUT = join(ROOT, 'out/warm-states');
mkdirSync(OUT, { recursive: true });
const gpus = process.argv.includes('--gpu') ? [process.argv[process.argv.indexOf('--gpu') + 1]] : ['gl', 'webgpu'];
function run(gpu, control) {
  const state = join(OUT, `${gpu}${control ? '-control' : ''}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'romdev', '--gpu', gpu, '--app', 'warm_states', '--frames', '180',
    '--warmup-spread', ...(control ? ['--allow-skipped', '--env', JSON.stringify({ CW_TEST_WARM_NO_PRERENDER: '1' })] : []), '--state', state, '--shot', state.replace(/json$/, 'png')],
  { encoding: 'utf8', cwd: ROOT, maxBuffer: 1 << 26 });
  if (r.status !== 0) { console.error(r.stdout.slice(-400), r.stderr.slice(-400)); process.exit(3); }
  return JSON.parse(readFileSync(state, 'utf8'));
}
let fail = 0;
for (const gpu of gpus) {
  const a = run(gpu, false), b = run(gpu, true);
  const ok = a['warm.tick'] > 30 && a['cw.three.missing'] === 0, ctl = b['cw.three.missing'] > 0;
  console.log(`${ok ? 'ok  ' : 'FAIL'} warm-states ${gpu}: ${a['cw.three.missing']} missing state(s) after the spread warm-up and ${a['warm.tick']} ticks of play`);
  console.log(`${ctl ? 'ok  ' : 'FAIL'} warm-states ${gpu} control (later slices without their pre-render): ${b['cw.three.missing']} missing state(s), at least 1 wanted`);
  if (!ok || !ctl) fail++;
}
process.exit(fail ? 1 : 0);
