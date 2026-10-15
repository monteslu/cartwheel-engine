// Post fusion (src/render/pipeline.c plan_fusion), GL and WebGPU: the height
// fog runs inside the SSAO composite (without TAA), bloom's composite and the
// LUT grade inside the tonemap pass, and the lens inside the anti-aliasing
// pass (SMAA's blend, FXAA) or, with no anti-aliasing, inside the tonemap
// pass, with each fused step rounded as the separate pass's target would
// round it. The picture must be the one the separate passes draw.
//
// Per app and backend (native, a gameplay frame): the frame with the passes
// separate (CW_TEST_NO_FUSION=1) against the fused frame. The fused run must
// have fused (the pipeline reports the fog, the grade and the lens, when the
// app's settings use them, as run fused), no pixel may differ by more than
// 2/255 and at most 0.05% by any amount. CONTROL: the first case that fused a
// step, fused without the rounding (CW_TEST_FUSION_RAW=1), must break that
// bound on each backend, or the test could not see a rounding change.
//
//   node tests/post-fusion.mjs [--gpus gl,webgpu] [app[:frame[:preset]] ...]
//
// The apps are native builds of this checkout, or of CW_PROJECT_ROOT's tree
// (see tests/gate.mjs); the default is platformer3d_auto and the showcase
// sample. A preset (low, medium, high, ultra: CW_QUALITY) picks the graphics
// preset, so one app can be checked with FXAA, SMAA and no anti-aliasing.
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
await takeSuiteLock('post-fusion.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PROJ = process.env.CW_PROJECT_ROOT ? resolve(process.env.CW_PROJECT_ROOT) : ROOT;
const OUT = join(PROJ, 'out', 'gate', 'post-fusion');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const gi = argv.indexOf('--gpus');
const GPUS = (gi >= 0 ? argv[gi + 1] : 'gl,webgpu').split(',');
const only = argv.filter((a, i) => !a.startsWith('-') && !(gi >= 0 && i === gi + 1));
// app: a frame in gameplay (the 3D scene with its post settings), and a preset
const CASES = (only.length ? only : ['platformer3d_auto:600', 'showcase:300']).map((a) => {
  const [app, frame, preset] = a.split(':');
  return { app, frames: +frame || 900, preset: preset || '', tag: app + (preset ? '-' + preset : '') };
});
const MAX_ANY = 0.0005, MAX_D = 2;

const guard = armGuard('post-fusion', { outFile: join(OUT, 'gpu-mem.json') });

function run(c, gpu, tag, extra) {
  const { app, frames, preset } = c;
  let exe = ['games', 'samples'].map((d) => join(PROJ, 'build/native', d, app)).find((p) => existsSync(p));
  if (!exe) try { exe = JSON.parse(readFileSync(join(PROJ, 'build/native/apps', `${app}.json`), 'utf8')).binary; } catch {}
  if (!exe || !existsSync(exe)) throw new Error(`no native build of ${app} (scripts/build.sh)`);
  const shot = join(OUT, `${c.tag}-${gpu}-${tag}.png`);
  const env = { ...process.env, CW_GPU: gpu, CW_FRAMES: String(frames), CW_SCREENSHOT: shot, SDL_AUDIODRIVER: 'dummy',
    CW_DEBUG_CMDS: 'pipeline', ...(preset ? { CW_QUALITY: preset } : {}), ...(gpu === 'webgpu' ? wgpuPin().env : {}), ...extra };
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

const STEPS = ['height_fog', 'grade', 'lens'];
let fail = 0;
const fusedSome = {}; // per backend: the first case that fused a step (the control's case)
for (const gpu of GPUS)
  for (const c of CASES) {
    await guard.gate(`${c.tag} ${gpu}`);
    const sep = await run(c, gpu, 'separate', { CW_TEST_NO_FUSION: '1' });
    const fused = await run(c, gpu, 'fused', {});
    const problems = [];
    // which steps the app's settings use, from the separate run; fused, those passes must not run alone
    if (STEPS.some((k) => sep.fused[k])) problems.push('CW_TEST_NO_FUSION=1 still fused');
    if (sep.ran.height_fog && sep.ran.ssao && !sep.ran.taa && !fused.fused.height_fog) problems.push('the fog ran as its own pass');
    if (sep.ran.grade && !fused.fused.grade) problems.push('the grade ran as its own pass');
    if (sep.ran.lens && !fused.fused.lens) problems.push('the lens ran as its own pass');
    const d = diffImages(sep.img, fused.img);
    if (!d.comparable) problems.push(d.reason);
    else if (d.max > MAX_D || d.any > MAX_ANY)
      problems.push(`picture differs: ${(d.any * 100).toFixed(4)}% of pixels, max ${d.max}/255 (bound ${MAX_ANY * 100}%, ${MAX_D})`);
    const what = STEPS.filter((k) => fused.fused[k]).map((k) => k.replace('height_', '')).join(', ') || 'none';
    console.log(`${problems.length ? 'FAIL' : 'ok  '} ${c.tag} ${gpu} frame ${c.frames} (fused: ${what}; bloom ${sep.ran.bloom ? 'on' : 'off'}): ` +
      (d.comparable ? `${(d.any * 100).toFixed(4)}% px differ, max ${d.max}` : '') + (problems.length ? '\n  ' + problems.join('\n  ') : ''));
    if (problems.length) fail++;
    if (!fusedSome[gpu] && STEPS.some((k) => fused.fused[k])) fusedSome[gpu] = c;
  }
// control: the fusion without its rounding must be seen, on each backend
for (const gpu of GPUS) {
  const c = fusedSome[gpu];
  if (!c) {
    console.log(`FAIL control: no case fused a step on ${gpu}, so the rounding cannot be checked`);
    fail++;
    continue;
  }
  await guard.gate(`control ${gpu}`);
  const sep = readImage(join(OUT, `${c.tag}-${gpu}-separate.png`));
  const raw = await run(c, gpu, 'raw', { CW_TEST_FUSION_RAW: '1' });
  const d = diffImages(sep, raw.img);
  const seen = d.comparable && (d.max > MAX_D || d.any > MAX_ANY);
  console.log(`${seen ? 'ok  ' : 'FAIL'} control: ${c.tag} ${gpu} fused without the rounding differs ${(d.any * 100).toFixed(3)}%, max ${d.max}` +
    (seen ? '' : ' (the bound cannot see a rounding change)'));
  if (!seen) fail++;
}
guard.finish();
process.exit(fail ? 1 : 0);
