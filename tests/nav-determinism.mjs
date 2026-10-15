// Navigation determinism across hosts AND job-pool sizes: the unit-nav and
// nav-demo gates (tests/gates.json) on native, Chromium and wcnative, once
// each at CW_WORKERS=0, 1, 3 and the machine's default. Each gate holds every
// host to the native run and every run to the same expected hashes, so all
// twelve host and pool combinations must agree.
//
// CW_WORKERS sizes the pool natively; Chromium's carts get it as
// navigator.hardwareConcurrency (tools/lib/workers.mjs); wcnative's carts get
// it from the online CPU list the GPU sandbox shows (tools/lib/gpu-sandbox.mjs).
// So each size runs as its own suite (tools/with-gpu-suite.mjs), which enters
// the sandbox with that CW_WORKERS: run this script outside any suite.
//
//   node tests/nav-determinism.mjs [extra gate.mjs args]
import { spawn } from 'node:child_process';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

if (process.env.CW_GPU_SANDBOXED) {
  console.error('nav-determinism: run it outside a GPU suite; it starts one per pool size (the sandbox fixes the CPU count wcnative sees)');
  process.exit(2);
}
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const extra = process.argv.slice(2);
let fail = 0;
for (const w of ['0', '1', '3', undefined]) {
  const env = { ...process.env };
  if (w === undefined) delete env.CW_WORKERS;
  else env.CW_WORKERS = w;
  const tag = w === undefined ? 'default' : `w${w}`;
  console.log(`── CW_WORKERS=${w ?? '(default)'}`);
  const code = await new Promise((res) => spawn('node', [join(ROOT, 'tools', 'with-gpu-suite.mjs'), `nav-determinism-${tag}`, 'node',
    join(ROOT, 'tests', 'gate.mjs'), 'unit-nav', 'nav-demo', '--hosts', 'native,chromium,wcnative', ...extra],
  { stdio: 'inherit', env }).on('close', (c) => res(c ?? 1)));
  if (code !== 0) fail++;
}
console.log(fail ? `nav-determinism: ${fail} pool size(s) failed` : 'ok   nav-determinism (native, chromium, wcnative at CW_WORKERS 0, 1, 3 and default)');
process.exit(fail ? 1 : 0);
