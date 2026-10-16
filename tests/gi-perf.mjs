// GI timing report: gi_room_perf (gi_room at 1280x720: two reflection probes,
// one box projected and captured every other frame, a 5 x 3 x 5 light probe
// grid) natively on GL at each quality preset, 240 frames, with the pass
// timers on. Reports the GPU ms of the "gi" pass (captures, grid updates and
// the SSR / SSGI G-buffer, averaged over frames with and without a capture),
// of the ssr and ssgi passes and of the whole frame, plus the frame with every
// probe, the grid, SSR and SSGI removed. Timings
// are reported, never compared; the checks: every preset's run has a capture
// time and a frame time, and the control (the probes and grid removed) has no
// capture time.
//
//   node tests/gi-perf.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { loadavg } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'perf');
mkdirSync(OUT, { recursive: true });
const FRAMES = 240;

function run(tag, env) {
  const state = join(OUT, `gi.${tag}.json`);
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--app', 'gi_room_perf', '--frames', String(FRAMES),
    '--state', state, '--shot', join(OUT, `gi.${tag}.png`)], { encoding: 'utf8', env: { ...process.env, CW_GPU_TIMERS: '1', ...env } });
  if (r.status !== 0) return { error: (r.stderr || r.stdout).trim().split('\n').slice(-2).join(' | ') };
  return JSON.parse(readFileSync(state, 'utf8'));
}

const load0 = loadavg()[0];
const rows = [];
let fail = 0;
for (const q of ['low', 'medium', 'high', 'ultra']) {
  const s = run(q, { CW_QUALITY: q });
  if (s.error) { fail++; rows.push(`${q.padEnd(8)} failed: ${s.error}`); continue; }
  const cap = s['gi.ms_capture'], frame = s['gi.ms_frame'], ssr = s['gi.ms_ssr'], ssgi = s['gi.ms_ssgi'];
  if (!(cap > 0) || !(frame > 0)) { fail++; console.log(`FAIL ${q}: no timing (capture ${cap}, frame ${frame})`); }
  rows.push(`${q.padEnd(8)} gi pass ${cap.toFixed(3).padStart(7)}   ssr ${ssr.toFixed(3).padStart(6)}   ssgi ${ssgi.toFixed(3).padStart(6)}   frame ${frame.toFixed(3).padStart(7)} ms`);
}
const none = run('none', { CW_QUALITY: 'high', GI_NONE: '1' });
if (none.error) { fail++; rows.push(`none     failed: ${none.error}`); }
else {
  if (none['gi.ms_capture'] > 0) { fail++; console.log('FAIL control: a capture time with no probes or grid'); }
  rows.push(`no GI    (high) frame ${none['gi.ms_frame'].toFixed(3).padStart(7)} ms`);
}
const text = [`gi_room_perf 1280x720, native GL, ${FRAMES} frames, GPU ms (16-frame averages), load ${load0.toFixed(1)} -> ${loadavg()[0].toFixed(1)}`, ...rows].join('\n');
console.log(text);
writeFileSync(join(OUT, 'gi-report.txt'), text + '\n');
console.log(fail ? `gi-perf: ${fail} failures` : 'ok   gi-perf (every preset timed; the control without probes has no capture time)');
process.exit(fail ? 1 : 0);
