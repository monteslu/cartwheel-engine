// Very Low's post (src/render/pipeline.c plan_fusion, pipeline_post.c tm_cheap_variant), GL and WebGPU: on effects
// detail Very Low the height fog and FXAA run inside the tonemap pass. Per app and backend (native, a gameplay frame at
// CW_QUALITY=verylow): the passes run apart (CW_TEST_NO_FUSION=1) against the fused frame. The fused run must report
// the fog and the anti-aliasing as fused where the separate run ran them, and the picture must be the separate passes'
// but for FXAA's edges: at most MAX_GT16 of the pixels may differ by more than 16/255 (the console variant against the
// quality preset), the mean difference stays under MAX_MEAN, and where FXAA ran fused it smoothed the frame: against the
// same frame with FXAA left out of the fused pass (CW_TEST_VERYLOW_POST=aa, the reference), at least MIN_AA of the pixels
// changed by more than 16/255.
// CONTROLS, per backend: the fused pass with the fog left out (CW_TEST_VERYLOW_POST=fog) on the first case that fused
// the fog, and with FXAA left out (CW_TEST_VERYLOW_POST=aa: it must show no smoothing) on the first that fused FXAA, must
// each break the bounds.
//
//   node tests/quality-verylow-post.mjs [--gpus gl,webgpu] [app[:frame] ...]
//
// The apps are native builds of this checkout, or of CW_PROJECT_ROOT's tree (see tests/gate.mjs).
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { armGuard } from '../tools/lib/gpu-guard.mjs';
import { testEnv, wgpuPin } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
await takeSuiteLock('quality-verylow-post.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PROJ = process.env.CW_PROJECT_ROOT ? resolve(process.env.CW_PROJECT_ROOT) : ROOT;
const OUT = join(PROJ, 'out', 'gate', 'quality-verylow-post');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const gi = argv.indexOf('--gpus');
const GPUS = (gi >= 0 ? argv[gi + 1] : 'gl,webgpu').split(',');
const only = argv.filter((a, i) => !a.startsWith('-') && !(gi >= 0 && i === gi + 1));
const CASES = (only.length ? only : ['open_world:600', 'showcase:300']).map((a) => {
  const [app, frame] = a.split(':');
  return { app, frames: +frame || 600, tag: app };
});
const MAX_GT16 = 0.005, MAX_MEAN = 1.0, MIN_AA = 0.0005; // share of pixels over 16/255; mean difference in 255ths; share FXAA changes by over 16/255

const guard = armGuard('quality-verylow-post', { outFile: join(OUT, 'gpu-mem.json') });

function run(c, gpu, tag, extra) {
  const { app, frames } = c;
  let exe = ['games', 'samples'].map((d) => join(PROJ, 'build/native', d, app)).find((p) => existsSync(p));
  if (!exe) try { exe = JSON.parse(readFileSync(join(PROJ, 'build/native/apps', `${app}.json`), 'utf8')).binary; } catch {}
  if (!exe || !existsSync(exe)) throw new Error(`no native build of ${app} (scripts/build.sh)`);
  const shot = join(OUT, `${c.tag}-${gpu}-${tag}.png`);
  const env = { ...process.env, CW_GPU: gpu, CW_FRAMES: String(frames), CW_SCREENSHOT: shot, SDL_AUDIODRIVER: 'dummy', CW_QUALITY: 'verylow',
    CW_DEBUG_CMDS: 'pipeline', CW_ASSETS: exe + '.assets', ...(gpu === 'webgpu' ? wgpuPin().env : {}), ...extra };
  return new Promise((res, rej) => {
    const p = spawn('timeout', ['-s', 'KILL', '300', exe], { env, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '', err = '';
    p.stdout.on('data', (d) => (out += d));
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => {
      if (code !== 0) return rej(new Error(`${app} ${gpu} ${tag} exited ${code}: ${err.slice(-800)}`));
      const line = out.split('\n').find((l) => l.includes('"passes"'));
      const passes = line ? JSON.parse(line).passes : [];
      res({ img: readImage(shot), ran: Object.fromEntries(passes.map((q) => [q.name, q.ran])),
        fused: Object.fromEntries(passes.map((q) => [q.name, q.fused])) });
    });
  });
}

/** mean absolute difference over every channel, in 255ths */
function meanDiff(a, b) {
  let s = 0;
  for (let i = 0; i < a.rgba.length; i++) s += Math.abs(a.rgba[i] - b.rgba[i]);
  return s / a.rgba.length;
}
const broken = (d, m) => !d.comparable || d.gt16 > MAX_GT16 || m > MAX_MEAN;
/** the share of pixels FXAA changed by over 16/255: img against the reference without it */
const smoothed = (img, noaa) => { const d = diffImages(noaa, img); return d.comparable ? d.gt16 : 0; };
const STEPS = { height_fog: 'fog', aa: 'aa' };
let fail = 0;
const ctl = {}; // per backend and step: the first case that fused it
for (const gpu of GPUS)
  for (const c of CASES) {
    await guard.gate(`${c.tag} ${gpu}`);
    const sep = await run(c, gpu, 'separate', { CW_TEST_NO_FUSION: '1' });
    const fused = await run(c, gpu, 'fused', {});
    const problems = [];
    for (const k of Object.keys(STEPS)) {
      if (sep.fused[k]) problems.push(`CW_TEST_NO_FUSION=1 still fused ${k}`);
      if (sep.ran[k] && !fused.fused[k]) problems.push(`${k} ran as its own pass on Very Low`);
      if (fused.fused[k]) (ctl[gpu] ||= {})[STEPS[k]] ||= c;
    }
    const d = diffImages(sep.img, fused.img), m = d.comparable ? meanDiff(sep.img, fused.img) : 0;
    const aa = !!fused.fused.aa;
    const ref = aa ? (await run(c, gpu, 'noaa', { CW_TEST_VERYLOW_POST: 'aa' })).img : null;
    const sm = aa ? smoothed(fused.img, ref) : 0;
    if (broken(d, m)) problems.push(d.comparable ? `picture differs: ${(d.gt16 * 100).toFixed(3)}% of pixels over 16/255 (bound ${MAX_GT16 * 100}%), mean ${m.toFixed(3)} (bound ${MAX_MEAN})` : d.reason);
    if (aa && sm < MIN_AA) problems.push(`FXAA changed ${(sm * 100).toFixed(3)}% of pixels by over 16/255 (at least ${MIN_AA * 100}%)`);
    const what = Object.keys(STEPS).filter((k) => fused.fused[k]).join(', ') || 'none';
    console.log(`${problems.length ? 'FAIL' : 'ok  '} ${c.tag} ${gpu} frame ${c.frames} (fused: ${what}): ` +
      (d.comparable ? `${(d.gt16 * 100).toFixed(3)}% px over 16, max ${d.max}, mean ${m.toFixed(3)}${aa ? `, FXAA smoothed ${(sm * 100).toFixed(3)}%` : ''}` : '') + (problems.length ? '\n  ' + problems.join('\n  ') : ''));
    if (problems.length) fail++;
  }
for (const gpu of GPUS)
  for (const step of ['fog', 'aa']) {
    const c = ctl[gpu] && ctl[gpu][step];
    if (!c) {
      console.log(`FAIL control: no case fused ${step} on ${gpu}`);
      fail++;
      continue;
    }
    await guard.gate(`control ${step} ${gpu}`);
    const sep = readImage(join(OUT, `${c.tag}-${gpu}-separate.png`));
    const r = await run(c, gpu, `without-${step}`, { CW_TEST_VERYLOW_POST: step });
    const d = diffImages(sep, r.img), m = d.comparable ? meanDiff(sep, r.img) : 0;
    const sm = step === 'aa' ? smoothed(r.img, readImage(join(OUT, `${c.tag}-${gpu}-noaa.png`))) : 0;
    const seen = broken(d, m) || (step === 'aa' && sm < MIN_AA);
    console.log(`${seen ? 'ok  ' : 'FAIL'} control: ${c.tag} ${gpu} without the fused ${step}: ${(d.gt16 * 100).toFixed(3)}% px over 16, mean ${m.toFixed(3)}${step === 'aa' ? `, smoothed ${(sm * 100).toFixed(3)}%` : ''}` +
      (seen ? '' : ' (the bound cannot see it)'));
    if (!seen) fail++;
  }
guard.finish();
process.exit(fail ? 1 : 0);
