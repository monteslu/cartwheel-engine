// Auto's save, on the real clock: test_auto_retry in unit_weakgpu runs only with
// CW_PLAYER_TIMING, since Auto is locked off in a deterministic run.
// Control: an injected failing check must fail the same run.
import { spawnSync } from 'node:child_process';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const B = join(ROOT, 'build/native');
const b = spawnSync('ninja', ['-C', B, 'unit_weakgpu'], { encoding: 'utf8' });
if (b.status !== 0) { console.log(b.stdout + b.stderr); process.exit(1); }
const run = (env) => {
  const r = spawnSync(join(B, 'tests/unit/unit_weakgpu'), [], { encoding: 'utf8', env: { ...process.env, CW_FRAMES: '2', CW_PLAYER_TIMING: '1', ...env } });
  return r.stdout + r.stderr;
};
const failed = (log) => { const m = /weakgpu: \d+ passed, (\d+) failed/.exec(log); return m ? Number(m[1]) : -1; };
const log = run({}), control = run({ CW_TEST_INJECT_FAIL: '1' });
let fail = 0;
if (failed(log) !== 0) { fail++; console.log(log); console.log('FAIL the checks failed'); }
if (!/weakgpu\.auto_retry: ok/.test(log) || !/quality: auto Medium 100% -> Low 100%/.test(log)) { fail++; console.log('FAIL Auto did not step, the test ran nothing'); }
if (failed(control) < 1) { fail++; console.log('CONTROL PASSED: an injected failure went unseen'); }
console.log(fail ? `unit-weakgpu-auto: ${fail} failures` : 'ok   unit-weakgpu-auto (Auto saved its step and retried a saved level)');
process.exit(fail ? 1 : 0);
