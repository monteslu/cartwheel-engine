// The warm-up builds the programs of what the camera cannot see yet (samples/warm_culled), spread over frames as a player gets
// it: the camera turns round to a mesh with its own program and no new program compiles. Control: a mesh with a new program
// appears then, and one does. GL and WebGPU, natively.
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..'), OUT = join(ROOT, 'out/warm-culled');
mkdirSync(OUT, { recursive: true });
function run(gpu, env) {
  const state = join(OUT, `${gpu}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'warm_culled', '--frames', '90', '--warmup-spread', '--state', state, '--shot', join(OUT, `${gpu}.png`)], { encoding: 'utf8', env: { ...process.env, ...env }, cwd: ROOT, maxBuffer: 1 << 26 });
  if (r.status !== 0) { console.error(r.stdout.slice(-400), r.stderr.slice(-400)); process.exit(3); }
  return JSON.parse(readFileSync(state, 'utf8'));
}
let fail = 0;
for (const gpu of ['gl', 'webgpu']) {
  const a = run(gpu, {}), b = run(gpu, { WARM_LATE: '1' });
  const ok = a['warm.tick'] > 40 && a['warm.new'] === 0 && a['cw.three.missing'] === 0, ctl = b['warm.new'] > 0;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${gpu}: ${a['warm.base']} programs after the warm-up, ${a['warm.new']} more after the camera turned`);
  console.log(`${ctl ? 'ok  ' : 'FAIL'} ${gpu} control: a new mesh's program arrives in play: ${b['warm.new']} more`);
  if (!ok || !ctl) fail++;
}
process.exit(fail ? 1 : 0);
