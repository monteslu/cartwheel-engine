// The GL particle simulation without transform feedback (src/fx/particles3d.c): each step is a fragment-shader pass
// from one pair of RGBA32F state textures into the other, and the draws, sort, trails and GPU stats read the state
// from those textures. Natively on GL, against the transform feedback simulation (CW_TEST_PSYS_TF=1, as before):
//   - samples/fx_particles (six systems: depth collision, trails, a sorted smoke, a mesh emitter, an attractor) at
//     frames 120 and 260 and samples/fx_sub_emitters (a PARENT child) at 200: every system's alive count read back
//     from the GPU equal to the transform feedback run's and to the CPU's emission function, which gives the WebGPU
//     run the same counts (exact; WebGPU's own GPU counts arrive a frame or more late, so they are not compared);
//   - against the WebGPU run (the compute simulation): each position sum (or mean) as near as the transform feedback
//     run's is (within twice its distance, or 1e-3 of the sum's size), and the share of pixels more than 16 levels
//     apart at most twice the transform feedback run's plus 0.2 % (bounces off the depth buffer are chaotic: float
//     rounding of a different shader stage moves a bounced particle, as it does between the two backends);
//   - no transform feedback session (fx.feedback 0; the control run has one a system and step);
//   - CONTROL: the picture and the fields of a run a frame early must fail the same comparison.
// WebGPU is untouched (a compute pass). Native only.
//   node tests/particle-fs-sim.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'particle-fs-sim');
mkdirSync(OUT, { recursive: true });
{
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

function run(app, name, frames, env = {}, gpu = 'gl') {
  const png = join(OUT, `${name}.png`), st = join(OUT, `${name}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', app, '--frames', String(frames),
    '--shot', png, '--state', st], { encoding: 'utf8', env: { ...process.env, ...env } });
  if (r.status !== 0) throw new Error(`${name}: ${((r.stderr || '') + (r.stdout || '')).trim().split('\n').slice(-3).join(' | ')}`);
  return { img: readImage(png), state: JSON.parse(readFileSync(st, 'utf8')) };
}
const keys = (s, re) => Object.keys(s).filter((k) => re.test(k));
// what differs between two runs' particle fields: alive counts exactly; unless counts only, sums within 1e-4 of their
// size and the picture within 0.1 % of pixels
function compare(a, b, counts) {
  const out = [];
  for (const k of keys(a.state, /\.(gpu_)?alive$/)) if (a.state[k] !== b.state[k]) out.push(`${k} ${a.state[k]} against ${b.state[k]}`);
  if (counts) return out;
  for (const k of keys(a.state, /\.(gpu_sum|mean)$/)) {
    const x = a.state[k], y = b.state[k];
    for (let i = 0; i < 3; i++) {
      const tol = 1e-4 * Math.max(1, Math.abs(x[i]), Math.abs(y[i]));
      if (!(Math.abs(x[i] - y[i]) <= tol)) { out.push(`${k}[${i}] ${x[i]} against ${y[i]}`); break; }
    }
  }
  const d = diffImages(a.img, b.img);
  if (!d.comparable || d.gt16 > 0.001) out.push(`picture: ${d.comparable ? (d.gt16 * 100).toFixed(3) + ' % of pixels more than 16 levels apart' : d.reason}`);
  return out;
}

let fail = 0;
const bad = (m) => { console.log('  FAIL ' + m); fail++; };
const cases = [
  { app: 'fx_particles', frames: 120, fb: 'fx.feedback' },
  { app: 'fx_particles', frames: 260, fb: 'fx.feedback' },
  { app: 'fx_sub_emitters', frames: 200 },
];
for (const c of cases) {
  const tag = `${c.app}.${c.frames}`;
  const a = run(c.app, `${tag}.fs`, c.frames), b = run(c.app, `${tag}.tf`, c.frames, { CW_TEST_PSYS_TF: '1' });
  const w = run(c.app, `${tag}.wgpu`, c.frames, {}, 'webgpu');
  /* (WebGPU's GPU counts arrive a frame or more late, by design: its CPU counts are the ones of this frame) */
  for (const k of keys(a.state, /\.alive$/).filter((k) => !/gpu_alive$/.test(k))) if (a.state[k] !== w.state[k]) bad(`${tag}: ${k} ${a.state[k]}, WebGPU ${w.state[k]}`);
  for (const k of keys(a.state, /\.(gpu_sum|mean)$/)) {
    const dist = (x, y) => Math.max(...[0, 1, 2].map((i) => Math.abs(x[i] - y[i])));
    const fsw = dist(a.state[k], w.state[k]), tfw = dist(b.state[k], w.state[k]);
    const size = Math.max(1, ...w.state[k].map(Math.abs));
    console.log(`  ${k}: from WebGPU's ${fsw.toPrecision(3)} (transform feedback ${tfw.toPrecision(3)})`);
    if (fsw > Math.max(2 * tfw, 1e-3 * size)) bad(`${tag}: ${k} ${fsw.toPrecision(3)} from WebGPU's, transform feedback ${tfw.toPrecision(3)}`);
  }
  {
    const dw = diffImages(a.img, w.img), tw = diffImages(b.img, w.img);
    console.log(`  picture: ${(dw.gt16 * 100).toFixed(3)} % of pixels more than 16 levels from WebGPU's (transform feedback ${(tw.gt16 * 100).toFixed(3)} %)`);
    if (!dw.comparable || dw.gt16 > 2 * tw.gt16 + 0.002) bad(`${tag}: the picture ${(dw.gt16 * 100).toFixed(3)} % from WebGPU's`);
  }
  const gpu = keys(a.state, /\.gpu_alive$/);
  console.log(`${tag}: ` + gpu.map((k) => `${k.replace(/\.gpu_alive$/, '')} ${a.state[k]} (tf ${b.state[k]}, cpu ${a.state[k.replace(/gpu_alive$/, 'alive')]})`).join(', '));
  if (gpu.length < 1 || gpu.every((k) => !a.state[k])) bad(`${tag}: no particles alive on the GPU`);
  for (const k of gpu) if (a.state[k] !== a.state[k.replace(/gpu_alive$/, 'alive')]) bad(`${tag}: ${k} ${a.state[k]}, the CPU's emission says ${a.state[k.replace(/gpu_alive$/, 'alive')]}`);
  for (const m of compare(a, b, true)) bad(`${tag}: ${m}`);
  if (c.fb) {
    console.log(`${tag}: transform feedback sessions a frame ${a.state[c.fb]} (control ${b.state[c.fb]})`);
    if (a.state[c.fb] !== 0) bad(`${tag}: ${a.state[c.fb]} transform feedback sessions`);
    if (!(b.state[c.fb] > 0)) bad(`CONTROL PASSED: ${tag} with CW_TEST_PSYS_TF=1 shows ${b.state[c.fb]} transform feedback sessions`);
  }
}
// the comparison sees motion and the counts: a frame early must fail it
{
  const a = run('fx_particles', 'early.a', 120), b = run('fx_particles', 'early.b', 119);
  const m = compare(a, b);
  console.log(`control (frame 119 against 120): ${m.length} difference(s)${m.length ? ': ' + m[0] : ''}`);
  if (!m.length) bad('CONTROL PASSED: frames 119 and 120 compare equal');
}
console.log(fail ? `FAIL particle-fs-sim (${fail})` : 'ok   particle-fs-sim');
process.exit(fail ? 1 : 0);
