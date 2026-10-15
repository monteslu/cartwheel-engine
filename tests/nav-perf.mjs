// Navigation perf gate: samples/nav_perf natively and in Chromium, on one
// job-pool thread (CW_WORKERS=0) and on the machine's default pool.
// Reported: navmesh build of a 324-tile world (static and dynamic, best of 5),
// 64 obstacles applied, 1000 batched paths, and the crowd step for 500 and
// 2000 agents (mean and worst of 600 steps).
// Checks:
//   - every run's result hashes (navmesh bytes, paths, both crowds) are equal:
//     the same work on every host and at every pool size;
//   - the budgets below, on each host's default pool;
//   - natively, the default pool builds the world at least 2x faster than one
//     thread when it has 4 or more threads;
// control: the same budget check against impossible budgets must fail.
//
//   node tests/nav-perf.mjs [--hosts native,chromium]
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
const hosts = argv.includes('--hosts') ? argv[argv.indexOf('--hosts') + 1].split(',') : ['native', 'chromium'];
const FRAMES = 40; // the benchmark runs on tick 30

// ms, on each host's default pool
const BUDGET = {
  native: { 'perf.build_ms': 150, 'perf.build_dynamic_ms': 120, 'perf.step500_ms': 1, 'perf.step2000_ms': 3 },
  chromium: { 'perf.build_ms': 400, 'perf.build_dynamic_ms': 300, 'perf.step500_ms': 2, 'perf.step2000_ms': 5 },
};
const HASHES = ['perf.mesh_hash', 'perf.paths_hash', 'perf.crowd500_hash', 'perf.crowd2000_hash', 'perf.tiles', 'perf.polys'];

function run(host, workers) {
  return new Promise((res) => {
    const tag = `${host}.${workers === undefined ? 'default' : 'w' + workers}`;
    const state = join(OUT, `nav_perf.${tag}.json`);
    const env = { ...process.env };
    if (workers === undefined) delete env.CW_WORKERS;
    else env.CW_WORKERS = String(workers);
    const p = spawn('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'nav_perf', '--frames', String(FRAMES),
      '--state', state], { stdio: ['ignore', 'pipe', 'pipe'], env });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => {
      if (code !== 0) return res({ host, workers, tag, error: err.trim().split('\n').slice(-2).join(' | ') });
      res({ host, workers, tag, state: JSON.parse(readFileSync(state, 'utf8')) });
    });
  });
}

const results = [];
for (const h of hosts) for (const w of [0, undefined]) results.push(await run(h, w));
const pad = (s, n) => String(s).padEnd(n);
const num = (v, d = 2) => (typeof v === 'number' ? v.toFixed(d) : '-');
const rows = [['pool threads', 'perf.threads', 0], ['tiles', 'perf.tiles', 0], ['polys', 'perf.polys', 0],
  ['build ms', 'perf.build_ms', 1], ['build dynamic ms', 'perf.build_dynamic_ms', 1], ['64 obstacles ms', 'perf.update_ms', 2],
  ['1000 paths ms', 'perf.paths_ms', 2], ['step 500 ms', 'perf.step500_ms', 3], ['step 500 max', 'perf.step500_max_ms', 3],
  ['step 2000 ms', 'perf.step2000_ms', 3], ['step 2000 max', 'perf.step2000_max_ms', 3]];
const lines = ['nav_perf: 324-tile world (256 x 256 m), crowds of 500 and 2000 (600 steps at 60 Hz)', pad('', 18) + results.map((r) => pad(r.tag, 18)).join('')];
for (const [label, key, d] of rows) lines.push(pad(label, 18) + results.map((r) => pad(r.error ? '(failed)' : num(r.state[key], d), 18)).join(''));
console.log(lines.join('\n'));
writeFileSync(join(OUT, 'nav-report.txt'), lines.join('\n') + '\n');

let fail = 0;
const failed = results.filter((r) => r.error);
for (const r of failed) { fail++; console.log(`FAIL ${r.tag}: ${r.error}`); }
const ok = results.filter((r) => !r.error);
const ref = ok[0];
for (const r of ok.slice(1))
  for (const k of HASHES)
    if (r.state[k] !== ref.state[k]) { fail++; console.log(`FAIL ${r.tag}: ${k} ${r.state[k]} differs from ${ref.tag}'s ${ref.state[k]}`); }
const overBudget = (s, budget) => Object.entries(budget).filter(([k, ms]) => !(s[k] > 0 && s[k] < ms)).map(([k, ms]) => `${k} ${num(s[k])} (budget ${ms})`);
for (const r of ok.filter((r) => r.workers === undefined)) {
  const over = overBudget(r.state, BUDGET[r.host] || BUDGET.chromium);
  if (over.length) { fail++; console.log(`FAIL ${r.tag}: over budget: ${over.join(', ')}`); }
}
const one = ok.find((r) => r.host === 'native' && r.workers === 0), many = ok.find((r) => r.host === 'native' && r.workers === undefined);
if (one && many && many.state['perf.threads'] >= 4 && !(many.state['perf.build_ms'] * 2 < one.state['perf.build_ms'])) {
  fail++;
  console.log(`FAIL native: ${many.state['perf.threads']} threads built in ${num(many.state['perf.build_ms'])} ms, one in ${num(one.state['perf.build_ms'])} ms (expected at least 2x faster)`);
}
// the control: impossible budgets must fail, or the check measures nothing
if (many && !overBudget(many.state, { 'perf.step500_ms': 0.001, 'perf.build_ms': 0.001 }).length) { fail++; console.log('FAIL CONTROL: an impossible budget passed'); }
console.log(fail ? `nav-perf: ${fail} failures` : `ok   nav-perf (same hashes on ${ok.map((r) => r.tag).join(', ')}; within budget)`);
process.exit(fail ? 1 : 0);
