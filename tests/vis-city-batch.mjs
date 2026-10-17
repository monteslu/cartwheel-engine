// Batched clustered lighting: vis_city_lights (the city of about 4500 meshes
// under 64 clustered street lamps and the sun's four cascades, 1080p) with
// LightingSettings.batching off (the default: one draw a mesh) and on (meshes
// that share geometry and material are one instanced draw, the cluster and
// csm parts filled once a batch: cw_shading_part_batching), on each host.
// Checks, per host: the two pictures are the same (at most 0.05% of pixels off
// by more than 16 levels: instance order flips a few pixels of coplanar
// faces), and batching draws at most a tenth as many calls. Reported: draw
// calls, frame work (update start to a GPU sync after the frame) and frame ms,
// both ways. Controls, which must fail: the draw check between two unbatched
// runs, and the picture check against a frame later on the walk.
//
//   node tests/vis-city-batch.mjs [--hosts native,chromium]
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
import { readImage, diffImages } from '../tools/lib/png.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
testEnv(process.env); // what it starts renders offscreen and cannot reach the desktop
process.env.CW_GPU_TIMERS = '1'; // the native runs' GPU ms (the pipeline's timers are off unless asked for)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'vis-city-batch');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const hosts = argv.includes('--hosts') ? argv[argv.indexOf('--hosts') + 1].split(',') : ['native', 'chromium'];
const FRAMES = 240, MAX_GT16 = 0.0005;
const BATCH = 'set /root/World/Lighting batching true';

function run(host, tag, frames, cmds) {
  return new Promise((res) => {
    const state = join(OUT, `${tag}.${host}.json`), shot = join(OUT, `${tag}.${host}.png`);
    const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'vis_city_lights', '--frames', String(frames),
      '--state', state, '--shot', shot];
    if (cmds) args.push('--start-cmds', cmds);
    const p = spawn('node', args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => {
      if (code !== 0) return res({ error: err.trim().split('\n').slice(-2).join(' | ') });
      res({ state: JSON.parse(readFileSync(state, 'utf8')), img: readImage(shot) });
    });
  });
}

const num = (v, d = 2) => (typeof v === 'number' ? v.toFixed(d) : '-');
const fewer = (a, b) => a.state['city.draws_avg'] * 10 <= b.state['city.draws_avg'];
let fail = 0;
const lines = [`vis_city_lights 1080p, ${FRAMES} frames (averages after 60 warm-up frames): unbatched / batched`];
for (const h of hosts) {
  const off = await run(h, 'off', FRAMES), on = await run(h, 'on', FRAMES, BATCH);
  const off2 = await run(h, 'off2', FRAMES), later = await run(h, 'later', FRAMES + 8, BATCH);
  const bad = [off, on, off2, later].find((r) => r.error);
  if (bad) { fail++; console.log(`FAIL ${h}: ${bad.error}`); continue; }
  const d = diffImages(off.img, on.img), dl = diffImages(later.img, on.img);
  const s = (r, k, n) => num(r.state[k], n);
  lines.push(`${h.padEnd(9)} draws ${s(off, 'city.draws_avg', 0)} / ${s(on, 'city.draws_avg', 0)}, frame work ms ${s(off, 'city.cpu_ms')} / ${s(on, 'city.cpu_ms')}, ` +
    `frame ms ${s(off, 'city.frame_ms')} / ${s(on, 'city.frame_ms')}` + (h === 'native' ? `, gpu ms ${s(off, 'city.gpu_ms')} / ${s(on, 'city.gpu_ms')}` : '') +
    `, pixels off >16: ${(d.gt16 * 100).toFixed(4)}%`);
  if (!d.comparable || d.gt16 > MAX_GT16) { fail++; console.log(`FAIL ${h}: batched and unbatched pictures differ (${JSON.stringify(d)})`); }
  if (!fewer(on, off)) { fail++; console.log(`FAIL ${h}: batching drew ${s(on, 'city.draws_avg', 0)} calls, unbatched ${s(off, 'city.draws_avg', 0)}`); }
  if (fewer(off2, off)) { fail++; console.log(`FAIL CONTROL ${h}: two unbatched runs passed the draw check: it measures nothing`); }
  if (dl.comparable && dl.gt16 <= MAX_GT16) { fail++; console.log(`FAIL CONTROL ${h}: a later frame passed the picture check (${(dl.gt16 * 100).toFixed(3)}%)`); }
}
console.log(lines.join('\n'));
writeFileSync(join(OUT, 'report.txt'), lines.join('\n') + '\n');
console.log(fail ? `vis-city-batch: ${fail} failures` : 'ok   vis-city-batch (same picture batched and not, a tenth of the draws or fewer, on every host; controls fail)');
process.exit(fail ? 1 : 0);
