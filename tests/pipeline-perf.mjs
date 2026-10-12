// Render pipeline GPU timing report: post_stack_perf (the full-quality
// segment: MSAA 4x, TAA, GTAO, bloom, AgX, LUT grade, vignette, chromatic
// aberration, grain) at 1280x720 for 240 frames, natively, and the per-pass
// GPU ms each pass's GL_TIME_ELAPSED queries measured (16-frame average).
// Timings are reported, never compared. The checks: natively every pass the
// segment runs has a timing (timer queries work and every pass is wrapped),
// and a pass the segment does NOT run (FXAA/SMAA, the depth prepass) has
// none, which is the control: a report that timed everything, or nothing,
// fails. Other hosts are timed too when they can be (report only).
//
//   node tests/pipeline-perf.mjs [--hosts native,chromium,romdev]
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
testEnv(process.env); // what it starts renders offscreen and cannot reach the desktop

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'perf');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const hosts = argv.includes('--hosts') ? argv[argv.indexOf('--hosts') + 1].split(',') : ['native', 'chromium', 'romdev'];
const FRAMES = 240;
const RUN = ['scene', 'velocity', 'ssao', 'taa', 'bloom', 'tonemap', 'grade', 'lens'];
const NOT_RUN = ['depth_prepass', 'aa'];

function run(host) {
  return new Promise((res) => {
    const state = join(OUT, `pipeline.${host}.json`);
    const p = spawn('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'post_stack_perf', '--frames',
      String(FRAMES), '--state', state, '--shot', join(OUT, `pipeline.${host}.png`)], { stdio: ['ignore', 'pipe', 'pipe'] });
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
let fail = 0;
const rows = [...RUN, ...NOT_RUN];
const pad = (s, n) => String(s).padEnd(n);
const lines = [`pipeline GPU ms per pass, post_stack_perf 1280x720 full quality, ${FRAMES} frames (16-frame average)`,
  pad('pass', 15) + results.map((r) => pad(r.host, 12)).join('')];
for (const name of rows) {
  lines.push(pad(name, 15) + results.map((r) => {
    if (r.error) return pad('(failed)', 12);
    const v = r.state['gpu.' + name];
    return pad(v >= 0 ? v.toFixed(3) : '-', 12);
  }).join(''));
}
lines.push(pad('total', 15) + results.map((r) => {
  if (r.error) return pad('', 12);
  const t = RUN.reduce((a, n) => a + Math.max(0, r.state['gpu.' + n] ?? 0), 0);
  return pad(t.toFixed(3), 12);
}).join(''));
console.log(lines.join('\n'));
writeFileSync(join(OUT, 'pipeline-report.txt'), lines.join('\n') + '\n');

const native = results.find((r) => r.host === 'native');
if (!native || native.error) { fail++; console.log('FAIL native run: ' + (native ? native.error : 'not run')); }
else {
  if (native.state['pipe.float'] !== 1) { fail++; console.log('FAIL native: RGBA16F is not renderable'); }
  for (const n of RUN) if (!(native.state['gpu.' + n] > 0)) { fail++; console.log(`FAIL native: pass ${n} has no GPU time`); }
  for (const n of NOT_RUN) if (native.state['gpu.' + n] >= 0) { fail++; console.log(`FAIL native: pass ${n} did not run but has a GPU time (control)`); }
}
for (const r of results) if (r.error) console.log(`note ${r.host}: ${r.error}`);
console.log(fail ? `pipeline-perf: ${fail} failures` : 'ok   pipeline-perf (every running pass timed natively; idle passes untimed)');
process.exit(fail ? 1 : 0);
