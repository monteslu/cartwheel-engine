// Cloth determinism across hosts AND job-pool sizes: the unit-cloth
// gate (tests/gates.json) on native, Chromium and wcnative, once
// each at CW_WORKERS=0, 1, 2, 4, 8 and the machine's default. The gate holds every
// host to the native run and every run to the same pinned checksum, so all
// host and pool combinations must agree.
//
// CW_WORKERS sizes the pool natively; Chromium's carts get it as
// navigator.hardwareConcurrency (tools/lib/workers.mjs), and wcnative's carts
// as the online CPU count the GPU sandbox shows wasmcart-run
// (tools/lib/gpu-sandbox.mjs).
//
//   node tests/cloth-determinism.mjs [extra gate.mjs args]
import { spawn } from 'node:child_process';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
await takeSuiteLock('cloth-determinism'); // the gate runs below inherit it

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const extra = process.argv.slice(2);
let fail = 0;
for (const w of ['0', '1', '2', '4', '8', undefined]) {
  const env = { ...process.env };
  if (w === undefined) delete env.CW_WORKERS;
  else env.CW_WORKERS = w;
  console.log(`── CW_WORKERS=${w ?? '(default)'}`);
  const code = await new Promise((res) => spawn('node', [join(ROOT, 'tests', 'gate.mjs'), 'unit-cloth', 'cloth-demo',
    '--hosts', 'native,chromium,wcnative', ...extra], { stdio: 'inherit', env }).on('close', (c) => res(c ?? 1)));
  if (code !== 0) fail++;
}
console.log(fail ? `cloth-determinism: ${fail} pool size(s) failed` : 'ok   cloth-determinism (native, chromium, wcnative at CW_WORKERS 0, 1, 2, 4, 8 and default)');
process.exit(fail ? 1 : 0);
