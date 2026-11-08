// A big 2D stream on WebGPU (natively, the test GPU): r2d_stress sprites200k, one pass of 800,000 vertices (22 MB) and
// 1,200,000 indices (4.8 MB), uploaded in pieces of at most 4 MB.
//   - it draws what GL draws (under 0.5 % of pixels past 16 levels) and logs no stream error;
//   - a stream over the device's largest buffer is not drawn and says why, without losing the device: with
//     CW_TEST_R2D_STREAM_MAX=1000000 (a 1 MB limit) the run ends normally, logs the error once, and draws no sprite
//     (the frame differs from the drawn one on over 20 % of pixels). CONTROL: the undrawn frame must fail the GL bound
//     the drawn one meets, so the parity check can tell a drawn frame from an empty one.
// Holds the GPU suite lock. Run: node tests/r2d-big-stream.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
if (!process.env.CW_SUITE_LOCK_HELD) {
  const r = spawnSync('node', [join(ROOT, 'tools/with-gpu-suite.mjs'), 'r2d-big-stream', 'node', fileURLToPath(import.meta.url), ...process.argv.slice(2)], { stdio: 'inherit' });
  process.exit(r.status ?? 1);
}
const OUT = join(ROOT, 'out', 'gate', 'r2d-big-stream');
mkdirSync(OUT, { recursive: true });
let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const ERR = /larger than this device's largest buffer/g;

function run(name, gpu, env = {}) {
  const png = join(OUT, `${name}.${gpu}.png`), log = join(OUT, `${name}.${gpu}.log`);
  rmSync(log, { force: true });
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', 'r2d_stress', '--frames', '4', '--gpu', gpu,
    '--shot', png, '--log', log], { env: { ...process.env, CW_STRESS: 'sprites200k', CW_STRESS_SIZE: '1280x720', ...env }, encoding: 'utf8' });
  let text = '';
  try { text = readFileSync(log, 'utf8'); } catch {}
  return { status: r.status, img: r.status === 0 ? readImage(png) : null, errors: (text.match(ERR) || []).length, tail: (r.stderr || '').trim().split('\n').slice(-2).join(' | ') };
}
const share = (a, b, level) => {
  let n = 0;
  for (let i = 0; i < a.rgba.length; i += 4)
    if (Math.max(Math.abs(a.rgba[i] - b.rgba[i]), Math.abs(a.rgba[i + 1] - b.rgba[i + 1]), Math.abs(a.rgba[i + 2] - b.rgba[i + 2])) > level) n++;
  return n / (a.w * a.h);
};

const gl = run('drawn', 'gl'), wg = run('drawn', 'webgpu'), cut = run('over', 'webgpu', { CW_TEST_R2D_STREAM_MAX: '1000000' });
check(gl.status === 0 && wg.status === 0, `GL and WebGPU runs end normally (${gl.status}, ${wg.status}) ${wg.tail}`);
check(cut.status === 0, `the over-limit run ends normally, the device kept (${cut.status}) ${cut.tail}`);
if (gl.img && wg.img) {
  const d = share(gl.img, wg.img, 16);
  check(d < 0.005, `WebGPU draws the 22 MB stream as GL does (${(d * 100).toFixed(3)} % of pixels past 16 levels)`);
}
check(wg.errors === 0, `the drawn run logs no stream error (${wg.errors})`);
check(cut.errors === 1, `the over-limit run logs the stream error once (${cut.errors})`);
if (wg.img && cut.img) {
  const d = share(wg.img, cut.img, 16);
  check(d > 0.2, `the over-limit pass draws no sprites (${(d * 100).toFixed(1)} % of pixels differ from the drawn frame, over 20 %)`);
  const c = gl.img ? share(gl.img, cut.img, 16) : 0;
  check(c >= 0.005, `CONTROL: the undrawn frame fails the GL bound the drawn one meets (${(c * 100).toFixed(1)} % past 16 levels)`);
}
console.log(fail ? `r2d-big-stream: ${fail} failures` : 'ok   r2d-big-stream');
process.exit(fail ? 1 : 0);
