// Froxel binning determinism across hosts AND job-pool sizes: the unit-froxel
// and night-street gates (tests/gates.json) on native, Chromium and wcnative,
// once each at CW_WORKERS=0, 1, 3 and the machine's default. Each gate holds
// every host to the native run and every run to its pinned hashes
// (unit.checksum, ns.grid_hash), so every host and pool size must agree.
//
//   node tests/froxel-determinism.mjs [extra gate.mjs args]
import { spawn } from 'node:child_process';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
await takeSuiteLock('froxel-determinism'); // the gate runs below inherit it

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const extra = process.argv.slice(2);
let fail = 0;
for (const w of ['0', '1', '3', undefined]) {
  const env = { ...process.env };
  if (w === undefined) delete env.CW_WORKERS;
  else env.CW_WORKERS = w;
  console.log(`── CW_WORKERS=${w ?? '(default)'}`);
  const code = await new Promise((res) => spawn('node', [join(ROOT, 'tests', 'gate.mjs'), 'unit-froxel', 'night-street',
    '--hosts', 'native,chromium,wcnative', ...extra], { stdio: 'inherit', env }).on('close', (c) => res(c ?? 1)));
  if (code !== 0) fail++;
}
console.log(fail ? `froxel-determinism: ${fail} pool size(s) failed` : 'ok   froxel-determinism (native, chromium, wcnative at CW_WORKERS 0, 1, 3 and default)');
process.exit(fail ? 1 : 0);
