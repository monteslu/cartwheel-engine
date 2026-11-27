// The GPU-idle wait after loading (native WebGPU, src/platform/gpu.c): the first frame after the start's warm-up, and
// the first after a loading phase in play (cw_loading_begin), waits for the GPU once with a limit of 300 s, so a
// driver that compiles shaders when a program first draws (lavapipe, on a cold shader cache) never trips a frame's
// 10 s wait in the frames after it. samples/loading_phase natively on WebGPU, as a test runs it (the warm-up in one
// frame) and spread over frames as a player gets it, with a phase loaded at tick 60: host.gpu.settle_waits is 2 (the
// start and the phase), the phase finished. Control: CW_TEST_WGPU_NO_SETTLE=1 leaves the wait out (0), which must fail.
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..'), OUT = join(ROOT, 'out/gpu-settle');
mkdirSync(OUT, { recursive: true });
function run(name, spread, env) {
  const state = join(OUT, `${name}.json`);
  const args = [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--gpu', 'webgpu', '--app', 'loading_phase', '--frames', '150', '--state', state, '--shot', join(OUT, `${name}.png`)];
  if (spread) args.push('--warmup-spread');
  const r = spawnSync('node', args, { encoding: 'utf8', env: { ...process.env, LP_LOAD_AT: '60', LP_LOAD_N: '8', ...env }, cwd: ROOT, maxBuffer: 1 << 26 });
  if (r.status !== 0) { console.error(r.stdout.slice(-400), r.stderr.slice(-400)); process.exit(3); }
  return JSON.parse(readFileSync(state, 'utf8'));
}
const good = (s) => s['cw.gpu.api'] === 2 && s['host.gpu.settle_waits'] === 2 && s['lp.done'] === 1;
let fail = 0;
for (const spread of [false, true]) {
  const how = spread ? 'spread' : 'test';
  const a = run(how, spread, {}), b = run(`${how}-control`, spread, { CW_TEST_WGPU_NO_SETTLE: '1' });
  const ok = good(a), ctl = !good(b);
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${how}: ${a['host.gpu.settle_waits']} settle waits (start and the phase: 2), phase done ${a['lp.done']}, api ${a['cw.gpu.api']}`);
  console.log(`${ctl ? 'ok  ' : 'FAIL'} ${how} control (no settle wait): ${b['host.gpu.settle_waits']} settle waits, which must fail`);
  if (!ok || !ctl) fail++;
}
process.exit(fail ? 1 : 0);
