// A cart renders at the size its host asks for, through wasmcart-libretro in a
// headless libretro frontend (test/lr_frontend of wasmcart-libretro,
// RetroArch's call order) with the core's Internal Resolution option set.
//
// pixel_scale under a host preference (tests/unit/pixel_scale.c, N = 3): the
// cart hands the host the low frame, the preference's frame / 3
// (pixel_auto3_low), or with crisp UI offered at 540 lines the low frame
// times the smallest whole k that reaches 540 (pixel_auto3: 1080p -> a
// 640x360 scene in a 1280x720 frame); every k x k block of that frame is one
// colour (crisp UI starts off), and the settled size is announced.
//
//   node tests/host-resolution.mjs [app ...] [--cart-dir <dir>]
//
// The dual carts of build/cart-wgpu (or --cart-dir), which the libretro core runs on GL; the core and frontend from
// tools/lib/libretro.mjs.
import { spawn } from 'node:child_process';
import { mkdirSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, writePng } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { armGuard } from '../tools/lib/gpu-guard.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { tempDir } from '../tools/lib/tmp.mjs';
import { libretroBuild } from '../tools/lib/libretro.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
await takeSuiteLock('host-resolution.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'host-resolution');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : d);
const cartDir = resolve(opt('--cart-dir', join(ROOT, 'build', 'cart-wgpu', 'carts')));
const only = argv.filter((a, i) => !a.startsWith('-') && argv[i - 1] !== '--cart-dir');
const { core, frontend } = libretroBuild();

function run(args) {
  return new Promise((res) => {
    const p = spawn(frontend, [core, ...args], { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, SDL_AUDIODRIVER: 'dummy' } });
    let out = '';
    p.stdout.on('data', (d) => (out += d));
    p.stderr.on('data', (d) => (out += d));
    const t = setTimeout(() => p.kill('SIGKILL'), 240000);
    p.on('close', (code) => { clearTimeout(t); res({ code, out }); });
  });
}

// [app, option, scene w, h, frame w, h]
const PIXEL = [
  ['pixel_auto3_low', '1920x1080', 640, 360, 640, 360], ['pixel_auto3_low', '960x540', 320, 180, 320, 180],
  ['pixel_auto3_low', '480x270', 160, 90, 160, 90],
  ['pixel_auto3', '1920x1080', 640, 360, 1280, 720], ['pixel_auto3', '960x540', 320, 180, 960, 540],
  ['pixel_auto3', '480x270', 160, 90, 960, 540],
].filter((p) => !only.length || only.includes(p[0]));

async function checkPixel([app, optv, sw, sh, W, H]) {
  const name = `${app} ${optv}`, problems = [];
  const cart = join(cartDir, `${app}.wasc`);
  if (!existsSync(cart)) return { name, problems: [`no cart ${cart}`] };
  const tmp = tempDir(join(ROOT, 'out', 'gate'), `hostres-${app}-`), shot = join(tmp, 'f.ppm');
  const r = await run([cart, '--frames', '30', '--option', `wasmcart_resolution=${optv}`, '--shot', `29:${shot}`]);
  if (r.code !== 0) return { name, problems: [`run failed: ${r.out.trim().split('\n').slice(-3).join(' | ')}`] };
  const init = /deferred init complete \((\d+)x(\d+)/.exec(r.out), geom = /SET_GEOMETRY (\d+)x(\d+)/.exec(r.out);
  if (!init || +init[1] !== W || +init[2] !== H) problems.push(`the cart settled at ${init ? `${init[1]}x${init[2]}` : '?'}, not ${W}x${H}`);
  if (!geom || +geom[1] !== W || +geom[2] !== H) problems.push(`the core announced ${geom ? `${geom[1]}x${geom[2]}` : 'no geometry'}, not ${W}x${H}`);
  const img = readImage(shot);
  writePng(join(OUT, `${app}-${optv}.png`), img.w, img.h, img.rgba);
  if (img.w !== W || img.h !== H) return { name, problems: [...problems, `the frame is ${img.w}x${img.h}, not ${W}x${H}`] };
  const k = W / sw;
  let odd = 0, colours = new Set();
  for (let by = 0; by < sh; by++) for (let bx = 0; bx < sw; bx++) {
    const i0 = 4 * (by * k * W + bx * k);
    colours.add((img.rgba[i0] << 16) | (img.rgba[i0 + 1] << 8) | img.rgba[i0 + 2]);
    let u = true;
    for (let y = by * k; y < (by + 1) * k && u; y++) for (let x = bx * k; x < (bx + 1) * k; x++) {
      const i = 4 * (y * W + x);
      if (img.rgba[i] !== img.rgba[i0] || img.rgba[i + 1] !== img.rgba[i0 + 1] || img.rgba[i + 2] !== img.rgba[i0 + 2]) { u = false; break; }
    }
    if (!u) odd++;
  }
  if (k > 1 && odd) problems.push(`${odd} of the ${k}x${k} blocks are not one colour`);
  if (colours.size < 8) problems.push(`only ${colours.size} colours: the scene did not draw`);
  return { name, problems, detail: `scene ${sw}x${sh} in a ${W}x${H} frame (k ${k}), ${colours.size} colours` };
}

const guard = armGuard('host-resolution.mjs');
await guard.gate('host-resolution');
const failed = [];
let total = 0;
for (const p of PIXEL) {
  const r = await checkPixel(p);
  total++;
  console.log(`${r.problems.length ? 'FAIL' : 'ok  '} host-resolution ${r.name}: ${r.problems.length ? r.problems.join('; ') : r.detail}`);
  if (r.problems.length) failed.push(r.name);
}
guard.finish();
console.log(`host-resolution: ${total - failed.length}/${total} pass` + (failed.length ? `; failed: ${failed.join(', ')}` : '') + ` (frames in ${OUT})`);
process.exit(failed.length ? 1 : 0);
