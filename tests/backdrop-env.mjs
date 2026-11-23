// Backdrop environments and the screen-space reflections' backdrop fallback (cartwheel/backdrop.h environment,
// cw_post_settings.ssr_backdrop; src/render/backdrop.c, src/gi/ssr.c), GL and WebGPU, native: samples/backdrop's lane
// made a mirror with nothing else of the highway drawn (BD_MODE=mirror*), so what it shows is what it reflects. One
// frame per run, each the same frame of the same deterministic run:
//   reflect    the stage at 1 lighting the scene (environment on, ssr_backdrop off): the mirror shows the stage's
//              lamps and laser, bright pixels on the lane. CONTROL: environment off (BD_ENV=0) must not.
//   crossfade  the sea at 0.5 over the stage, both lighting the scene, the post chain plain (BD_LINEAR=1): every lane
//              pixel is the 50/50 blend in linear light of the lane over the stage alone and over the sea alone, as
//              the pictures are. CONTROL: opacity ignored (CW_TEST_BACKDROP_OPACITY_IGNORED=1: the sea at 1) must
//              break it.
//   ssr        environment off, ssr_backdrop on (BD_ENV=2): reflection rays that leave the scene take the backdrop's
//              picture, so the mirror shows the stage's laser sharp. CONTROL: the trace ignores the composite
//              (CW_TEST_BACKDROP_SSR_OFF=1) must not show it.
//   off        environment and ssr_backdrop off (BD_ENV=0): neither pass ran. With --baseline <exe> and
//              --baseline-assets <dir> (a build of the sample before environments and its assets): this build on those
//              assets draws byte for byte that build's frames.
//   water      a Water3D sea in front of the stage instead of the highway (BD_MODE=water): where its planar
//              reflection finds nothing of the scene it mirrors the stage, not the scene's own (magenta) background.
//              CONTROLS: the reflection's backdrop fallback off (CW_TEST_WATER_BACKDROP_OFF=1), and environment off,
//              must mirror the magenta.
//   split      two views side by side, the mirror in each: both views' lanes reflect the stage (one environment for
//              the tree); the one-pass split screen (BD_ARRAY=1) draws the frame the per-view one does. CONTROL:
//              environment off must not reflect.
//
//   node tests/backdrop-env.mjs [--gpus gl,webgpu] [--frames N] [--baseline <exe>] [--survey]
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv, wgpuPin } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox();
await takeSuiteLock('backdrop-env.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'backdrop-env');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const GPUS = opt('--gpus', 'gl,webgpu').split(',');
const FRAMES = +opt('--frames', '90');
const BASELINE = opt('--baseline', '');
const BASE_ASSETS = opt('--baseline-assets', ''); // the samples/backdrop assets of the build before environments
const SURVEY = argv.includes('--survey'); // (with CW_PROGRAMS_LOG_DIR: every run logs the states it lacks; none fails for them)
const exe = join(ROOT, 'build/native/samples/backdrop');
if (!existsSync(exe)) throw new Error('no native build of backdrop (scripts/build.sh backdrop)');

function run(gpu, tag, env, bin = exe) {
  const shot = join(OUT, `${gpu}-${tag}.png`), dump = join(OUT, `${gpu}-${tag}.json`);
  const e = { ...process.env, CW_GPU: gpu, CW_FRAMES: String(FRAMES), CW_SCREENSHOT: shot, CW_DEBUG_DUMP: dump, SDL_AUDIODRIVER: 'dummy',
    CW_RENDER_SCALE: '1', ...(gpu === 'webgpu' ? wgpuPin().env : {}), ...env };
  return new Promise((res, rej) => {
    const p = spawn('timeout', ['-s', 'KILL', '300', bin], { env: e, stdio: ['ignore', 'ignore', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => {
      if (code !== 0) return rej(new Error(`backdrop ${gpu} ${tag} exited ${code}: ${err.slice(-800)}`));
      const st = JSON.parse(readFileSync(dump, 'utf8'));
      if (st['render.skipped'] && !SURVEY) return rej(new Error(`backdrop ${gpu} ${tag}: three.c skipped ${st['render.skipped']} draws (program table)`));
      res({ img: readImage(shot), st, err });
    });
  });
}

const lin = new Float64Array(256);
for (let i = 0; i < 256; i++) { const c = i / 255; lin[i] = c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; }
const srgb = (x) => { x = Math.min(1, Math.max(0, x)); return 255 * (x <= 0.0031308 ? 12.92 * x : 1.055 * x ** (1 / 2.4) - 0.055); };

function differs(a, b, lv) {
  const n = a.w * a.h, m = new Uint8Array(n);
  for (let i = 0; i < n; i++)
    for (let k = 0; k < 3; k++) if (Math.abs(a.rgba[4 * i + k] - b.rgba[4 * i + k]) > lv) { m[i] = 1; break; }
  return m;
}
function erode(m, w, h, r) {
  const o = new Uint8Array(m.length);
  for (let y = r; y < h - r; y++)
    for (let x = r; x < w - r; x++) {
      let v = 1;
      for (let dy = -r; dy <= r && v; dy++) for (let dx = -r; dx <= r; dx++) if (!m[(y + dy) * w + x + dx]) { v = 0; break; }
      o[y * w + x] = v;
    }
  return o;
}
const count = (m) => m.reduce((s, v) => s + v, 0);
/** lane pixels lit like a lamp or a laser: some channel at least `lv`, in [x0, x1) of the width */
function bright(img, mask, lv, x0 = 0, x1 = 1) {
  let n = 0;
  const X0 = Math.floor(x0 * img.w), X1 = Math.floor(x1 * img.w);
  for (let y = 0; y < img.h; y++)
    for (let x = X0; x < X1; x++) {
      const i = y * img.w + x;
      if (!mask[i]) continue;
      if (Math.max(img.rgba[4 * i], img.rgba[4 * i + 1], img.rgba[4 * i + 2]) >= lv) n++;
    }
  return n;
}
/** the laser (green-cyan, bright) on the lane */
function laser(img, mask) {
  let n = 0;
  for (let i = 0; i < mask.length; i++) {
    if (!mask[i]) continue;
    const r = img.rgba[4 * i], g = img.rgba[4 * i + 1];
    if (g >= 150 && g >= r + 30) n++;
  }
  return n;
}
function crossfade(mix, a, b, mask) {
  let n = 0, bad = 0, sum = 0;
  for (let i = 0; i < mask.length; i++) {
    if (!mask[i]) continue;
    let clipped = false;
    for (let k = 0; k < 3; k++) if (a.rgba[4 * i + k] >= 250 || b.rgba[4 * i + k] >= 250) clipped = true;
    if (clipped) continue;
    n++;
    let worst = 0;
    for (let k = 0; k < 3; k++) worst = Math.max(worst, Math.abs(mix.rgba[4 * i + k] - srgb(0.5 * lin[a.rgba[4 * i + k]] + 0.5 * lin[b.rgba[4 * i + k]])));
    sum += worst;
    if (worst > 3) bad++;
  }
  return { n, share: n ? bad / n : 1, mean: n ? sum / n : 99 };
}

let fail = 0;
const report = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fail++; };
const NOSSR = { BD_SSR: '0' };
for (const gpu of GPUS) {
  const R = {};
  R.hidemain = await run(gpu, 'hidelane', { BD_MODE: 'mirror', BD_HIDELANE: '1', BD_ENV: '0' });
R.hidesplit = await run(gpu, 'hidesplit', { BD_MODE: 'mirrorsplit', BD_HIDELANE: '1', BD_ENV: '0' });
  R.mirror = await run(gpu, 'mirror', { BD_MODE: 'mirror', ...NOSSR });
  R.mirror_off = await run(gpu, 'mirror-off', { BD_MODE: 'mirror', BD_ENV: '0' });
  const { w, h } = R.mirror.img;
  const lane = erode(differs(R.mirror_off.img, R.hidemain.img, 8), w, h, 3);
  const nl = count(lane);
  const b = bright(R.mirror.img, lane, 200), bc = bright(R.mirror_off.img, lane, 200);
  report(nl > 20000 && b >= 300, `${gpu} reflect: ${b} of ${nl} lane pixels lit by the stage's lamps and laser (environment on); env updates ${R.mirror.st['bd.stage_caps']} captures`);
  report(bc < 40, `${gpu} reflect CONTROL (environment off): ${bc} lane pixels lit`);
  report(R.mirror.st['bd.env_ran'] === 1 && !R.mirror_off.st['bd.env_ran'] && !R.mirror_off.st['bd.ssrc_ran'],
    `${gpu} the environment pass ran with it on (${R.mirror.st['bd.env_ran']}) and neither pass with both off (${R.mirror_off.st['bd.env_ran'] | 0}, ${R.mirror_off.st['bd.ssrc_ran'] | 0})`);

  const L = { BD_LINEAR: '1', ...NOSSR };
  R.lstage = await run(gpu, 'lin-stage', { BD_MODE: 'mirror', ...L });
  R.lsea = await run(gpu, 'lin-sea', { BD_MODE: 'mirrorsea', ...L });
  R.lmix = await run(gpu, 'lin-mix', { BD_MODE: 'mirrormix', ...L });
  R.lign = await run(gpu, 'lin-ignored', { BD_MODE: 'mirrormix', ...L, CW_TEST_BACKDROP_OPACITY_IGNORED: '1' });
  const c = crossfade(R.lmix.img, R.lstage.img, R.lsea.img, lane), cc = crossfade(R.lign.img, R.lstage.img, R.lsea.img, lane);
  report(c.n > 10000 && c.share <= 0.02, `${gpu} crossfade 0.5: ${c.n} lane pixels, ${(c.share * 100).toFixed(3)}% over 3/255 from the 50/50 linear blend of each alone's reflection, mean ${c.mean.toFixed(3)}`);
  report(cc.share > 0.02, `${gpu} crossfade CONTROL (opacity ignored): ${(cc.share * 100).toFixed(1)}% over 3/255, mean ${cc.mean.toFixed(2)}`);

  R.ssr = await run(gpu, 'ssr', { BD_MODE: 'mirror', BD_ENV: '2' });
  R.ssr_off = await run(gpu, 'ssr-off', { BD_MODE: 'mirror', BD_ENV: '2', CW_TEST_BACKDROP_SSR_OFF: '1' });
  const s = laser(R.ssr.img, lane), sc = laser(R.ssr_off.img, lane);
  report(s >= 60 && R.ssr.st['bd.ssrc_ran'] === 1, `${gpu} ssr: ${s} lane pixels show the stage's laser through the reflections' backdrop fallback (composite pass ran ${R.ssr.st['bd.ssrc_ran']})`);
  report(sc < 20, `${gpu} ssr CONTROL (the trace ignores the composite): ${sc} lane pixels show the laser`);

  if (BASELINE) {
    for (const m of ['fire', 'mix', 'sea', 'zero']) {
      const A = BASE_ASSETS ? { CW_ASSETS: resolve(BASE_ASSETS) } : {};
      const a = await run(gpu, `ident-${m}`, { BD_MODE: m, BD_ENV: '0', ...A }), z = await run(gpu, `base-${m}`, { BD_MODE: m, BD_ENV: '0', ...A }, BASELINE);
      const d = count(differs(a.img, z.img, 0));
      report(d === 0, `${gpu} off ${m}: ${d} pixels differ from the build without environments`);
    }
  }

  R.water = await run(gpu, 'water', { BD_MODE: 'water', ...NOSSR });
  R.water_off = await run(gpu, 'water-off', { BD_MODE: 'water', BD_ENV: '0' });
  R.water_ctl = await run(gpu, 'water-ctl', { BD_MODE: 'water', ...NOSSR, CW_TEST_WATER_BACKDROP_OFF: '1' });
  // where the sea's planar reflection finds nothing of the scene it mirrors what is behind everything: without the
  // fallback the scene's own background colour (magenta), with it the stage. Magenta pixels of the sea (lower 70%):
  const magenta = (img) => {
    let n = 0;
    for (let y = Math.floor(h * 0.3); y < h; y++)
      for (let x = 0; x < w; x++) {
        const i = 4 * (y * w + x), r = img.rgba[i], g = img.rgba[i + 1], b = img.rgba[i + 2];
        if (r > g + 25 && b > g + 25) n++;
      }
    return n;
  };
  const wm = magenta(R.water.img), wc = magenta(R.water_ctl.img), wo = magenta(R.water_off.img);
  report(wc > 5000 && wm < wc * 0.02, `${gpu} water: ${wm} sea pixels mirror the scene's own background with the backdrop fallback, ${wc} without it (the stage mirrored instead)`);
  report(wo > 5000, `${gpu} water CONTROL (environment off): ${wo} sea pixels mirror the scene's own background`);

  R.split = await run(gpu, 'split', { BD_MODE: 'mirrorsplit', ...NOSSR });
  R.split_arr = await run(gpu, 'split-array', { BD_MODE: 'mirrorsplit', BD_ARRAY: '1', ...NOSSR });
  R.split_off = await run(gpu, 'split-off', { BD_MODE: 'mirrorsplit', BD_ENV: '0' });
  const sl = erode(differs(R.split_off.img, R.hidesplit.img, 8), w, h, 3);
  const bl = bright(R.split.img, sl, 200, 0, 0.5), br = bright(R.split.img, sl, 200, 0.5, 1);
  const cl = bright(R.split_off.img, sl, 200, 0, 0.5), cr = bright(R.split_off.img, sl, 200, 0.5, 1);
  report(bl >= 100 && br >= 100, `${gpu} split: ${bl} and ${br} lit lane pixels in the two views`);
  report(cl + cr < 40, `${gpu} split CONTROL (environment off): ${cl + cr} lit`);
  const ad = count(differs(R.split.img, R.split_arr.img, 16));
  report(ad <= w * h * 0.005, `${gpu} split in one pass: ${ad} pixels over 16/255 from the per-view frame`);
}
process.exit(fail ? 1 : 0);
