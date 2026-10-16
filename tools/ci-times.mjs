// Measure how long each gate takes in GitHub CI, from the job logs of finished
// runs, and write tests/ci-times.json, which tests/ci.mjs uses to balance its
// shares. A gate's time is the gap between its result line and the line before
// it (gate.mjs runs gates one after another and prints one line per gate).
//
//   node tools/ci-times.mjs [run-id ...]     default: the last 3 successful CI runs on main
//   node tools/ci-times.mjs --print [run-id] print the table, write nothing
//
// Needs the GitHub CLI (gh), logged in. Several runs are averaged per gate.
import { execFileSync } from 'node:child_process';
import { writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'tests/ci-times.json');
const args = process.argv.slice(2);
const print = args.includes('--print');
const gh = (...a) => execFileSync('gh', a, { encoding: 'utf8', maxBuffer: 256 << 20 });
const repo = JSON.parse(gh('repo', 'view', '--json', 'nameWithOwner')).nameWithOwner;

let runs = args.filter((a) => /^\d+$/.test(a));
if (!runs.length) {
  runs = JSON.parse(gh('run', 'list', '-R', repo, '-w', 'CI', '-b', 'main', '-s', 'success', '-L', '3', '--json', 'databaseId'))
    .map((r) => String(r.databaseId));
}
if (!runs.length) { console.error('ci-times: no successful CI runs found'); process.exit(1); }

/** @type {{gl: Record<string, number[]>, webgpu: Record<string, number[]>}} */
const samples = { gl: {}, webgpu: {} };
const T = (line) => Date.parse(line.slice(0, 28));
for (const run of runs) {
  const jobs = JSON.parse(gh('api', `repos/${repo}/actions/runs/${run}/jobs?per_page=100`)).jobs;
  for (const job of jobs) {
    if (job.conclusion !== 'success') continue;
    const log = gh('api', `repos/${repo}/actions/jobs/${job.id}/logs`).split('\n');
    let prev = 0;
    for (const line of log) {
      const body = line.slice(29);
      if (/^== (GL|WebGPU): \d+ gates/.test(body)) { prev = T(line); continue; }
      const m = /^(ok|FAIL) {1,3}(\S+)( \[webgpu\])?/.exec(body);
      if (!m || !prev) continue;
      const t = T(line), kind = m[3] ? 'webgpu' : 'gl';
      (samples[kind][m[2]] ||= []).push((t - prev) / 1000);
      prev = t;
    }
  }
}
const out = { runs, gl: {}, webgpu: {} };
for (const kind of ['gl', 'webgpu']) {
  const rows = Object.entries(samples[kind]).map(([n, s]) => [n, Math.round(s.reduce((a, b) => a + b, 0) / s.length)]);
  rows.sort((a, b) => b[1] - a[1] || (a[0] < b[0] ? -1 : 1));
  for (const [n, s] of rows) out[kind][n] = s;
}
const total = (k) => Object.values(out[k]).reduce((a, b) => a + b, 0);
if (!Object.keys(out.gl).length) { console.error('ci-times: no gate result lines in the logs'); process.exit(1); }
if (print) {
  for (const kind of ['gl', 'webgpu']) for (const [n, s] of Object.entries(out[kind])) console.log(`${kind.padEnd(7)}${String(s).padStart(6)} s  ${n}`);
}
console.log(`ci-times: runs ${runs.join(', ')}: GL ${Object.keys(out.gl).length} gates ${total('gl')} s, WebGPU ${Object.keys(out.webgpu).length} gates ${total('webgpu')} s`);
if (!print) { writeFileSync(OUT, JSON.stringify(out, null, 1) + '\n'); console.log(`wrote ${OUT}`); }
