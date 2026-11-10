// Auto's save, on the real clock: test_auto_retry in unit_weakgpu runs only with
// CW_PLAYER_TIMING, since Auto is locked off in a deterministic run.
// Control: an injected failing check must fail the same run. The count is the run's unit.failed at its end
// (CW_DEBUG_DUMP), not the suite's summary line: that line is printed in init, and the injected check runs at
// frame 2 (tests/unit/test.h), after it, so the summary never counts it.
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const B = join(ROOT, 'build/native');
const b = spawnSync('ninja', ['-C', B, 'unit_weakgpu'], { encoding: 'utf8' });
if (b.status !== 0) { console.log(b.stdout + b.stderr); process.exit(1); }
const OUT = join(ROOT, 'out', 'unit-weakgpu-auto');
mkdirSync(OUT, { recursive: true });
/** the run's log and its unit.failed once the late checks ran (-1: no dump, or unit.done not set) */
const run = (tag, env) => {
  const dump = join(OUT, tag + '.json');
  rmSync(dump, { force: true });
  const r = spawnSync(join(B, 'tests/unit/unit_weakgpu'), [], { encoding: 'utf8', env: { ...process.env, CW_FRAMES: '2', CW_PLAYER_TIMING: '1', CW_DEBUG_DUMP: dump, ...env } });
  let st = null;
  try { st = JSON.parse(readFileSync(dump, 'utf8')); } catch { st = null; }
  return { log: r.stdout + r.stderr, failed: st && st['unit.done'] === 1 && Number.isInteger(st['unit.failed']) ? st['unit.failed'] : -1 };
};
const { log, failed: failedRun } = run('run', {}), control = run('control', { CW_TEST_INJECT_FAIL: '1' });
let fail = 0;
if (failedRun !== 0) { fail++; console.log(log); console.log(`FAIL the checks failed (unit.failed ${failedRun}; -1: no state dump with unit.done)`); }
if (!/weakgpu\.auto_retry: ok/.test(log) || !/quality: auto Medium rung 3 -> Low rung 4 \(step:/.test(log)) { fail++; console.log('FAIL Auto did not step, the test ran nothing'); }
if (control.failed < 1) { fail++; console.log('CONTROL PASSED: an injected failure went unseen'); }
console.log(fail ? `unit-weakgpu-auto: ${fail} failures` : 'ok   unit-weakgpu-auto (Auto saved its step and retried a saved level)');
process.exit(fail ? 1 : 0);
