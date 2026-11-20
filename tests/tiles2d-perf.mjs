// 2D tile timings: samples/tiles2d_perf natively and as a cart on romdev (GL and WebGPU carts run the same
// code; the timings are CPU work). Reported per host:
//   - TileCollision2D building a 256 x 256 map (64 chunks, best of 5), per chunk, and one chunk rebuilt after
//     a cell edit.
// Checks:
//   - what was built (chunks, chains, segments) is the same on every host;
//   - the budgets below;
// control: the same budget check against impossible budgets must fail.
//
//   node tests/tiles2d-perf.mjs [--hosts native,romdev]
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'perf');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const hosts = argv.includes('--hosts') ? argv[argv.indexOf('--hosts') + 1].split(',') : ['native', 'romdev'];
const FRAMES = 12; // the benchmark runs on tick 5

// ms; a cart runs the same C as wasm
const BUDGET = {
  native: { 'perf.col_chunk_ms': 1, 'perf.col_edit_ms': 1.5 },
  romdev: { 'perf.col_chunk_ms': 3, 'perf.col_edit_ms': 4 },
};
const SAME = ['perf.col_chunks', 'perf.col_chains', 'perf.col_segments'];

function run(host) {
  return new Promise((res) => {
    const state = join(OUT, `tiles2d_perf.${host}.json`);
    const p = spawn('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'tiles2d_perf', '--frames', String(FRAMES),
      '--state', state], { stdio: ['ignore', 'pipe', 'pipe'], env: process.env });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => {
      if (code !== 0) return res({ host, error: err.trim().split('\n').slice(-2).join(' | ') });
      res({ host, state: JSON.parse(readFileSync(state, 'utf8')) });
    });
  });
}

const results = [];
for (const h of hosts) results.push(await run(h));
const pad = (s, n) => String(s).padEnd(n);
const num = (v, d = 2) => (typeof v === 'number' ? v.toFixed(d) : '-');
const rows = [['collision chunks', 'perf.col_chunks', 0], ['chains', 'perf.col_chains', 0], ['segments', 'perf.col_segments', 0],
  ['build all ms', 'perf.col_build_ms', 2], ['per chunk ms', 'perf.col_chunk_ms', 3], ['one edit ms', 'perf.col_edit_ms', 3]];
const lines = ['tiles2d_perf: a 256 x 256 map', pad('', 20) + results.map((r) => pad(r.host, 14)).join('')];
for (const [label, key, d] of rows) lines.push(pad(label, 20) + results.map((r) => pad(r.error ? '(failed)' : num(r.state[key], d), 14)).join(''));
console.log(lines.join('\n'));
writeFileSync(join(OUT, 'tiles2d-report.txt'), lines.join('\n') + '\n');

let fail = 0;
for (const r of results.filter((r) => r.error)) { fail++; console.log(`FAIL ${r.host}: ${r.error}`); }
const ok = results.filter((r) => !r.error);
for (const r of ok) if (r.state['perf.done'] !== 1) { fail++; console.log(`FAIL ${r.host}: the benchmark did not run (perf.done ${r.state['perf.done']})`); }
const ref = ok[0];
for (const r of ok.slice(1))
  for (const k of SAME)
    if (r.state[k] !== ref.state[k]) { fail++; console.log(`FAIL ${r.host}: ${k} ${r.state[k]} differs from ${ref.host}'s ${ref.state[k]}`); }
const overBudget = (s, budget) => Object.entries(budget).filter(([k, ms]) => !(s[k] > 0 && s[k] < ms)).map(([k, ms]) => `${k} ${num(s[k], 3)} (budget ${ms})`);
for (const r of ok) {
  const over = overBudget(r.state, BUDGET[r.host] || BUDGET.romdev);
  if (over.length) { fail++; console.log(`FAIL ${r.host}: over budget: ${over.join(', ')}`); }
}
// the control: impossible budgets must fail, or the check measures nothing
if (ref && !overBudget(ref.state, { 'perf.col_chunk_ms': 0.00001 }).length) { fail++; console.log('FAIL CONTROL: an impossible budget passed'); }
console.log(fail ? `tiles2d-perf: ${fail} failures` : `ok   tiles2d-perf (the same build on ${ok.map((r) => r.host).join(', ')}; within budget)`);
process.exit(fail ? 1 : 0);
