// City perf report: vis_city (about 4500 meshes, 960x540) along its
// 720-frame walk with the VisibilityManager (BVH frustum culling, occlusion
// queries, portals, LOD, impostors) and vis_city_off (no culling at all: what
// three.c's own per-object frustum test and instancing give), on each host.
// Reported: three.c draw calls and meshes drawn (averaged after 60 warm-up
// frames), triangles in the last frame, the visibility pass's CPU ms, the
// frame work (update start to a GPU sync after the frame), the pipeline's GPU
// ms. Checks: the culled city's frame work fits the 60 fps budget (16.7 ms)
// natively and in Chromium, and it draws fewer meshes than the unculled one on
// every host; control: the same budget check against an impossible budget
// (0.05 ms) must fail.
//
//   node tests/vis-perf.mjs [--hosts native,chromium,romdev]
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
testEnv(process.env); // what it starts renders offscreen and cannot reach the desktop
process.env.CW_GPU_TIMERS = '1'; // the native runs' GPU ms column (the pipeline's timers are off unless asked for)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'perf');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const hosts = argv.includes('--hosts') ? argv[argv.indexOf('--hosts') + 1].split(',') : ['native', 'chromium', 'romdev'];
const FRAMES = 720, BUDGET = 1000 / 60;

function run(host, app) {
  return new Promise((res) => {
    const state = join(OUT, `${app}.${host}.json`);
    const p = spawn('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', app, '--frames', String(FRAMES),
      '--state', state, '--shot', join(OUT, `${app}.${host}.png`)], { stdio: ['ignore', 'pipe', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => {
      if (code !== 0) return res({ host, app, error: err.trim().split('\n').slice(-2).join(' | ') });
      res({ host, app, state: JSON.parse(readFileSync(state, 'utf8')) });
    });
  });
}

const results = [];
for (const h of hosts) for (const app of ['vis_city', 'vis_city_off']) results.push(await run(h, app));
const pad = (s, n) => String(s).padEnd(n);
const num = (v, d = 2) => (typeof v === 'number' ? v.toFixed(d) : '-');
const rows = [['draw calls', 'city.draws_avg', 1], ['meshes drawn', 'city.visible_avg', 0], ['triangles k', 'city.tris_k', 1],
  ['cull cpu ms', 'city.cull_ms', 2], ['frame work ms', 'city.cpu_ms', 2], ['frame ms', 'city.frame_ms', 2], ['gpu ms', 'city.gpu_ms', 2]];
const lines = [`vis_city 960x540, ${FRAMES} frames (averages after 60 warm-up frames): culled (VisibilityManager) / unculled`,
  pad('', 16) + results.map((r) => pad(`${r.host}${r.app === 'vis_city' ? '' : ' off'}`, 15)).join('')];
for (const [label, key, d] of rows)
  lines.push(pad(label, 16) + results.map((r) => pad(r.error ? '(failed)' : r.app !== 'vis_city' && key === 'city.visible_avg' ? '-' : num(r.state[key], d), 15)).join(''));
console.log(lines.join('\n'));
writeFileSync(join(OUT, 'vis-report.txt'), lines.join('\n') + '\n');

let fail = 0;
const within = (s, b) => s['city.cpu_ms'] > 0 && s['city.cpu_ms'] < b;
for (const h of hosts) {
  const on = results.find((r) => r.host === h && r.app === 'vis_city'), off = results.find((r) => r.host === h && r.app === 'vis_city_off');
  if (on.error || off.error) {
    const msg = `${h}: ${on.error || off.error}`;
    if (h === 'native' || h === 'chromium') { fail++; console.log('FAIL ' + msg); } else console.log('note ' + msg);
    continue;
  }
  if ((h === 'native' || h === 'chromium') && !within(on.state, BUDGET)) { fail++; console.log(`FAIL ${h}: culled frame work ${num(on.state['city.cpu_ms'])} ms is over budget`); }
  if (!(on.state['city.tris_k'] < off.state['city.tris_k'])) { fail++; console.log(`FAIL ${h}: culling drew no fewer triangles`); }
  if (h === 'native' && within(on.state, 0.05)) { fail++; console.log('FAIL CONTROL: an impossible budget passed: the timing measures nothing'); }
}
console.log(fail ? `vis-perf: ${fail} failures` : 'ok   vis-perf (culled city within 16.7 ms natively and in Chromium, fewer triangles than unculled on every host)');
process.exit(fail ? 1 : 0);
