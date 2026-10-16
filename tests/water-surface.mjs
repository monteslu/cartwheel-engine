// The drawn water is where the CPU says it is: cw_water_height_at against
// the GPU's surface, on every host and backend.
//
// samples/water_boat, seen from above, with the Water3D's debug_view HEIGHT (and
// tone mapping NONE) draws each water pixel's world height as 16 bits in R and G over
// debug_range; the app publishes, per probe, where it lands in the frame
// (probe.uv) and the CPU's height there at the drawn time (probe.h). Per
// host and backend this decodes the frame at every probe that shows water
// (B = 255) and requires at least 80% of the probes to, every one within
// 1.5 cm (the grid between vertices and a pixel's footprint), and the mean
// error under 0.5 cm. CONTROL: natively, the water drawn 0.25 s later
// (time_offset) must break the bound, or the test could not see the waves.
//
//   node tests/water-surface.mjs [--hosts native,romdev,chromium] [--gpus gl,webgpu]
import { spawn } from 'node:child_process';
import { mkdirSync, existsSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
await takeSuiteLock('water-surface.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'water-surface');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return (i >= 0 ? argv[i + 1] : d).split(','); };
const HOSTS = opt('--hosts', 'native,romdev,chromium');
const GPUS = opt('--gpus', 'gl,webgpu');
const FRAMES = 90, LO = -2, HI = 2, MAX_ERR = 0.015, MAX_MEAN = 0.005, MIN_SHARE = 0.8;
// a steep view: at grazing angles a crest in front can hide the point a probe projects to
const CMDS = ['set Root/Water debug_view HEIGHT', 'set Root/Post tonemap NONE', 'set Root/Camera position vec3(0, 25, 10)',
  'set Root/Camera rotation vec3(-1.1, 0, 0)'];

function runCart(host, gpu, tag, extraCmds = []) {
  const shot = join(OUT, `${host}-${gpu}-${tag}.png`), state = join(OUT, `${host}-${gpu}-${tag}.json`);
  const args = ['tools/run-cart.mjs', '--host', host, '--gpu', gpu, '--app', 'water_boat', '--frames', String(FRAMES),
    '--shot', shot, '--state', state, '--start-cmds', [...CMDS, ...extraCmds].join(';')];
  return new Promise((res) => {
    const p = spawn(process.execPath, args, { cwd: ROOT, env: process.env, stdio: ['ignore', 'pipe', 'pipe'] });
    let err = '';
    p.stdout.on('data', () => {});
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => {
      if (code !== 0 || !existsSync(shot) || !existsSync(state)) return res({ error: `exit ${code}: ${err.slice(-600)}` });
      res({ img: readImage(shot), st: JSON.parse(readFileSync(state, 'utf8')) });
    });
  });
}

// sRGB byte -> the value the shader wrote (it wrote the byte's exact decode)
function measure({ img, st }) {
  const uv = st['probe.uv'], hs = st['probe.h'];
  if (!uv || !hs) return { error: 'no probe.uv / probe.h in the state' };
  let n = 0, seen = 0, worst = 0, sum = 0;
  for (let k = 0; k < hs.length; k++) {
    const u = uv[2 * k], v = uv[2 * k + 1];
    if (!(u > 0 && u < 1 && v > 0 && v < 1)) continue;
    n++;
    const x = Math.floor(u * img.w), y = Math.floor(v * img.h);
    const i = 4 * (y * img.w + x);
    if (img.rgba[i + 2] !== 255) continue; // not water here (the boat, a crate)
    seen++;
    const h = LO + (HI - LO) * (img.rgba[i] * 256 + img.rgba[i + 1]) / 65535;
    const e = Math.abs(h - hs[k]);
    worst = Math.max(worst, e);
    sum += e;
  }
  return { n, seen, worst, mean: seen ? sum / seen : Infinity };
}
const ok = (m) => !m.error && m.n > 0 && m.seen / m.n >= MIN_SHARE && m.worst <= MAX_ERR && m.mean <= MAX_MEAN;
const fmt = (m) => m.error || `${m.seen}/${m.n} probes on water, worst ${(m.worst * 100).toFixed(2)} cm, mean ${(m.mean * 100).toFixed(2)} cm`;

let fail = 0;
for (const gpu of GPUS) {
  for (const host of HOSTS) {
    const r = await runCart(host, gpu, 'ref');
    if (r.error) { fail++; console.log(`FAIL water-surface ${host} ${gpu}: ${r.error}`); continue; }
    const m = measure(r);
    if (ok(m)) console.log(`ok   water-surface ${host} ${gpu}: ${fmt(m)}`);
    else { fail++; console.log(`FAIL water-surface ${host} ${gpu}: ${fmt(m)}`); }
  }
  const c = await runCart('native', gpu, 'control', ['set Root/Water time_offset 0.25']);
  if (c.error) { fail++; console.log(`FAIL water-surface control ${gpu}: ${c.error}`); continue; }
  const m = measure(c);
  if (ok(m)) { fail++; console.log(`FAIL water-surface ${gpu}: CONTROL PASSED (drawn 0.25 s late): ${fmt(m)}`); }
  else console.log(`ok   water-surface ${gpu} control fails as it must: ${fmt(m)}`);
}
process.exit(fail ? 1 : 0);
