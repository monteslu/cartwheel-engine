// Backdrops (cartwheel/backdrop.h, src/render/backdrop.c), GL and WebGPU, native: samples/backdrop's note highway
// over its two worlds (a stage in volumetric haze, a sea), its post chain plain (BD_LINEAR=1: LINEAR tone curve, no
// bloom) so the pictures add up in linear light. One frame per mode (BD_MODE), each the same frame of the same
// deterministic run:
//   behind     the highway's own pixels (where hiding the highway changes the picture, eroded 3 px off its edges)
//              are the ones it has over its own background: at most 0.5% of them more than 2/255 apart between the
//              stage behind it and no backdrop at all. CONTROL: the stage drawn over the finished scene
//              (CW_TEST_BACKDROP_ON_TOP=1) must break it.
//   crossfade  the sea at 0.5 over the stage: every background pixel (away from the highway, unclipped) is the
//              50/50 blend of the stage alone and the sea alone in linear light, within 2/255 for 99% of them.
//              CONTROL: opacity ignored (CW_TEST_BACKDROP_OPACITY_IGNORED=1: the sea at 1) must break it.
//   free       both backdrops at opacity 0: the frame is byte for byte the frame with both Backdrop nodes freed,
//              the scene's backdrop pass did not run, no pass of either world ran and neither rendered a picture.
//              CONTROL: the stage at 0.001 must break it (its world and the pass run).
//   fog        the stage world's volumetric fog was built, with its three shadowed spot lights.
//
//   node tests/backdrop.mjs [--gpus gl,webgpu] [--frames N]
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv, wgpuPin } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox();
await takeSuiteLock('backdrop.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'backdrop');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const GPUS = opt('--gpus', 'gl,webgpu').split(',');
const FRAMES = +opt('--frames', '90');
const exe = join(ROOT, 'build/native/samples/backdrop');
if (!existsSync(exe)) throw new Error('no native build of backdrop (scripts/build.sh backdrop)');

function run(gpu, tag, env) {
  const shot = join(OUT, `${gpu}-${tag}.png`), dump = join(OUT, `${gpu}-${tag}.json`);
  const e = { ...process.env, CW_GPU: gpu, CW_FRAMES: String(FRAMES), CW_SCREENSHOT: shot, CW_DEBUG_DUMP: dump, SDL_AUDIODRIVER: 'dummy',
    CW_RENDER_SCALE: '1', BD_LINEAR: '1', ...(gpu === 'webgpu' ? wgpuPin().env : {}), ...env };
  return new Promise((res, rej) => {
    const p = spawn('timeout', ['-s', 'KILL', '300', exe], { env: e, stdio: ['ignore', 'ignore', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => {
      if (code !== 0) return rej(new Error(`backdrop ${gpu} ${tag} exited ${code}: ${err.slice(-800)}`));
      const st = JSON.parse(readFileSync(dump, 'utf8'));
      if (st['render.skipped']) return rej(new Error(`backdrop ${gpu} ${tag}: three.c skipped ${st['render.skipped']} draws (program table)`));
      res({ img: readImage(shot), st });
    });
  });
}

const lin = new Float64Array(256);
for (let i = 0; i < 256; i++) { const c = i / 255; lin[i] = c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; }
const srgb = (x) => { x = Math.min(1, Math.max(0, x)); return 255 * (x <= 0.0031308 ? 12.92 * x : 1.055 * x ** (1 / 2.4) - 0.055); };

/** pixels where a and b differ by more than `lv` in any channel */
function differs(a, b, lv) {
  const n = a.w * a.h, m = new Uint8Array(n);
  for (let i = 0; i < n; i++)
    for (let k = 0; k < 3; k++) if (Math.abs(a.rgba[4 * i + k] - b.rgba[4 * i + k]) > lv) { m[i] = 1; break; }
  return m;
}
/** erode (keep = 1) or dilate (keep = 0) a mask by r pixels (square) */
function morph(m, w, h, r, keep) {
  const o = new Uint8Array(m.length);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      let v = keep;
      for (let dy = -r; dy <= r && v === keep; dy++)
        for (let dx = -r; dx <= r; dx++) {
          const X = x + dx, Y = y + dy;
          const s = X < 0 || Y < 0 || X >= w || Y >= h ? 0 : m[Y * w + X];
          if (s !== keep) { v = 1 - keep; break; }
        }
      o[y * w + x] = v;
    }
  return o;
}

/** behind: the highway's pixels over the backdrop against over its own background */
function behind(withBd, none, mask) {
  let n = 0, bad = 0;
  for (let i = 0; i < mask.length; i++) {
    if (!mask[i]) continue;
    n++;
    for (let k = 0; k < 3; k++) if (Math.abs(withBd.rgba[4 * i + k] - none.rgba[4 * i + k]) > 2) { bad++; break; }
  }
  return { n, share: n ? bad / n : 1 };
}
/** crossfade: mix against srgb(0.5 lin(a) + 0.5 lin(b)) over the background pixels */
function crossfade(mix, a, b, bg) {
  let n = 0, bad = 0, sum = 0;
  for (let i = 0; i < bg.length; i++) {
    if (!bg[i]) continue;
    let clipped = false;
    for (let k = 0; k < 3; k++) if (a.rgba[4 * i + k] >= 250 || b.rgba[4 * i + k] >= 250) clipped = true;
    if (clipped) continue;
    n++;
    let worst = 0;
    for (let k = 0; k < 3; k++) {
      const want = srgb(0.5 * lin[a.rgba[4 * i + k]] + 0.5 * lin[b.rgba[4 * i + k]]);
      worst = Math.max(worst, Math.abs(mix.rgba[4 * i + k] - want));
    }
    sum += worst;
    if (worst > 2) bad++;
  }
  return { n, share: n ? bad / n : 1, mean: n ? sum / n : 99 };
}
function freeCheck(r, removed) {
  const p = [];
  const d = differs(r.img, removed.img, 0);
  const diff = d.reduce((s, v) => s + v, 0);
  if (diff) p.push(`${diff} pixels differ from the frame without backdrops`);
  if (r.st['bd.pass_ran']) p.push('the backdrop pass ran');
  if (r.st['bd.world_passes']) p.push(`the worlds ran ${r.st['bd.world_passes']} passes`);
  if (r.st['bd.stage_renders'] || r.st['bd.sea_renders']) p.push(`pictures rendered: stage ${r.st['bd.stage_renders']}, sea ${r.st['bd.sea_renders']}`);
  return p;
}

let fail = 0;
const report = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fail++; };
for (const gpu of GPUS) {
  const R = {};
  for (const m of ['removed', 'zero', 'tiny', 'fire', 'sea', 'mix', 'hidemain']) R[m] = await run(gpu, m, { BD_MODE: m });
  R.ontop = await run(gpu, 'ontop', { BD_MODE: 'fire', CW_TEST_BACKDROP_ON_TOP: '1' });
  R.ignored = await run(gpu, 'ignored', { BD_MODE: 'mix', CW_TEST_BACKDROP_OPACITY_IGNORED: '1' });
  const { w, h } = R.fire.img;
  const geo = differs(R.hidemain.img, R.fire.img, 8); // what the highway covers
  const inside = morph(geo, w, h, 3, 1), bg = morph(geo, w, h, 6, 0).map((v) => 1 - v); // its inside; away from it

  const b = behind(R.fire.img, R.removed.img, inside), bc = behind(R.ontop.img, R.removed.img, inside);
  report(b.n > 20000 && b.share <= 0.005, `${gpu} behind: ${b.n} highway pixels, ${(b.share * 100).toFixed(3)}% over 2/255 from its own background's frame`);
  report(bc.share > 0.005, `${gpu} behind CONTROL (drawn on top): ${(bc.share * 100).toFixed(1)}% over 2/255`);

  const c = crossfade(R.mix.img, R.fire.img, R.sea.img, bg), cc = crossfade(R.ignored.img, R.fire.img, R.sea.img, bg);
  report(c.n > 100000 && c.share <= 0.01, `${gpu} crossfade 0.5: ${c.n} background pixels, ${(c.share * 100).toFixed(3)}% over 2/255 from the 50/50 linear blend, mean ${c.mean.toFixed(3)}`);
  report(cc.share > 0.01, `${gpu} crossfade CONTROL (opacity ignored): ${(cc.share * 100).toFixed(1)}% over 2/255, mean ${cc.mean.toFixed(2)}`);

  const f = freeCheck(R.zero, R.removed), fc = freeCheck(R.tiny, R.removed);
  report(!f.length, `${gpu} opacity 0 free: ${f.length ? f.join('; ') : 'byte-identical to no backdrops, no pass, no world pass, no picture'}`);
  report(fc.length >= 2, `${gpu} free CONTROL (opacity 0.001): ${fc.join('; ') || 'nothing ran'}`);

  const s = R.fire.st;
  report(s['bd.fog_active'] === 1 && s['bd.fog_shadowed'] === 3, `${gpu} the stage's volumetric fog: active ${s['bd.fog_active']}, shadowed lights ${s['bd.fog_shadowed']}`);
}
process.exit(fail ? 1 : 0);
