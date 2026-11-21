// Split screen drawn in one pass (cw_pipeline_set_views_array), native host, GL and WebGPU: samples/split_array draws
// the same picture with its views in one target (CW_TEST_VIEWS_ARRAY=1) as with each view on its own
// (CW_TEST_VIEWS_ARRAY=0), within 0.003 of its pixels over 16 levels, with every per-view pass in the run: terrain and
// its scatter (each view's own patches), the sky's sun disk, height fog, a pond, clustered lamps (one with a shadow
// atlas tile), a sorted particle fountain colliding with the depth, a decal, SSAO and bloom; at two and four views, at
// Very Low (the fog drawn per view, not fused into the shared tonemap), with volumetric fog (Ultra), with a reflection
// probe, a light probe grid, screen-space reflections and bounce light over a polished slab at Ultra (SA_GI: each view's G-buffer,
// reflections and bounce light in its own rectangle), with a depth prepass (SA_PREPASS), with the scene multisampled 4x (SA_MSAA: on GL each view is drawn and flushed in turn), and with a shockwave ring and a
// heat haze bending the picture (SA_DISTORT: one distortion buffer, each view's part in its rectangle). The one-pass
// runs must have run in one pass (sa.array 1), the per-view runs not.
//   - the cascaded sun is drawn view by view: with it the frame falls back (sa.array 0) and the picture is the same
//   - at Very Low the height fog is drawn in split screen (it was fused into the shared tonemap, which drew none)
//   - with SA_GI the reflections and the bounce light are in the picture (a per-view run without the ssr pass, and one
//     without the ssgi pass, must each differ from it)
//   - with SA_DISTORT the distortion is in the picture (a per-view run without the distort pass must differ from it)
// Control: CW_TEST_GL_VIEW_SCISSOR=1 (GL, four views with MSAA) leaves the last view's scissor on for the passes after the
// views and must fail the bound. Control: CW_TEST_VIEWS_ARRAY_SHIFT=1 draws each view's passes a view over (wrapped round the target) and must fail
// the bound; a Very Low run without the fog pass must differ from one with it.
//   node tests/views-array.mjs [--gpu gl,webgpu]
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv, wgpuPin } from '../tools/lib/wgpu-pin.mjs';
import { readImage, diffImages } from '../tools/lib/png.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out/gate/views-array');
mkdirSync(OUT, { recursive: true });
const EXE = join(ROOT, 'build/native/samples/split_array');
const argv = process.argv.slice(2);
const GPUS = (argv.includes('--gpu') ? argv[argv.indexOf('--gpu') + 1] : 'gl,webgpu').split(',');
const BOUND = 0.003;
let fail = 0;
const check = (what, ok, detail = '') => {
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${detail ? ': ' + detail : ''}`);
};

/** one native run: its last frame and debug fields */
function run(gpu, tag, env) {
  const shot = join(OUT, `${tag}-${gpu}.png`), dump = join(OUT, `${tag}-${gpu}.json`);
  for (const f of [shot, dump]) rmSync(f, { force: true });
  const e = { ...process.env, CW_GPU: gpu, CW_FRAMES: '90', CW_SCREENSHOT: shot, CW_DEBUG_DUMP: dump, SDL_AUDIODRIVER: 'dummy', ...env };
  if (gpu === 'webgpu') Object.assign(e, wgpuPin().env, { CW_GPU: 'webgpu' });
  const r = spawnSync(EXE, [], { encoding: 'utf8', env: e });
  if (r.status !== 0 || !existsSync(shot)) throw new Error(`split_array ${tag} (${gpu}) failed (${r.status}): ${(r.stderr || '').trim().split('\n').slice(-4).join(' | ')}`);
  return { img: readImage(shot), f: existsSync(dump) ? JSON.parse(readFileSync(dump, 'utf8')) : {} };
}
const share = (a, b) => diffImages(a.img, b.img).gt16;
const pct = (x) => `${(100 * x).toFixed(4)}%`;

const CASES = [
  { tag: 'v2', env: { SA_VIEWS: '2', CW_QUALITY: 'high' } },
  { tag: 'v4', env: { SA_VIEWS: '4', CW_QUALITY: 'high' } },
  { tag: 'v4-verylow', env: { SA_VIEWS: '4', CW_QUALITY: 'verylow' } },
  { tag: 'v2-vfog', env: { SA_VIEWS: '2', CW_QUALITY: 'ultra', SA_VFOG: '1' } },
  { tag: 'v2-gi', env: { SA_VIEWS: '2', CW_QUALITY: 'ultra', SA_GI: '1' } },
  { tag: 'v4-gi', env: { SA_VIEWS: '4', CW_QUALITY: 'ultra', SA_GI: '1' } },
  { tag: 'v2-prepass', env: { SA_VIEWS: '2', CW_QUALITY: 'high', SA_PREPASS: '1' } },
  { tag: 'v4-prepass', env: { SA_VIEWS: '4', CW_QUALITY: 'high', SA_PREPASS: '1' } },
  { tag: 'v2-water', env: { SA_VIEWS: '2', CW_QUALITY: 'high', SA_WATER: '1' } },
  { tag: 'v4-water', env: { SA_VIEWS: '4', CW_QUALITY: 'high', SA_WATER: '1' } },
  { tag: 'v2-water-gi', env: { SA_VIEWS: '2', CW_QUALITY: 'high', SA_WATER: '1', SA_GI: '1' } },
  { tag: 'v2-distort', env: { SA_VIEWS: '2', CW_QUALITY: 'high', SA_DISTORT: '1' } },
  { tag: 'v4-distort', env: { SA_VIEWS: '4', CW_QUALITY: 'high', SA_DISTORT: '1' } },
  { tag: 'v2-msaa', env: { SA_VIEWS: '2', CW_QUALITY: 'high', SA_MSAA: '1' } },
  { tag: 'v4-msaa', env: { SA_VIEWS: '4', CW_QUALITY: 'high', SA_MSAA: '1' } },
  { tag: 'v2-csm', env: { SA_VIEWS: '2', CW_QUALITY: 'high', SA_CSM: '1' }, fallback: true },
];
/* CI renders these on a software GPU (llvmpipe and lavapipe), where the set took over an hour of one shard and timed it
 * out (v0.57.0, and v0.57.1 with only the Ultra GI and four-view water, distortion and prepass cases left out): CI runs
 * the two- and four-view frames and the four views over 4x MSAA, with their controls; every case runs on a real GPU */
const CI_CASES = new Set(['v2', 'v4', 'v4-msaa']);
const LOCAL_ONLY = process.env.CW_TEST_GPU === 'software' || !!process.env.CI; // (CI's software GPU: tests/ci.mjs)
for (const gpu of GPUS) {
  console.log(`-- ${gpu}`);
  for (const c of CASES) {
    if (LOCAL_ONLY && !CI_CASES.has(c.tag)) { console.log(`skip ${c.tag}: local only (a real GPU; CI's software GPU takes too long)`); continue; }
    const one = run(gpu, `${c.tag}-onepass`, { ...c.env, CW_TEST_VIEWS_ARRAY: '1' });
    const each = run(gpu, `${c.tag}-perview`, { ...c.env, CW_TEST_VIEWS_ARRAY: '0' });
    const d = share(one, each);
    check(`${c.tag}: every draw has its program (cw.three.missing 0)`, !one.f["cw.three.missing"] && !each.f["cw.three.missing"], `${one.f["cw.three.missing"]} / ${each.f["cw.three.missing"]}`);
    if (c.fallback) check(`${c.tag}: the cascaded sun's frame is drawn view by view`, one.f['sa.array'] === 0, `sa.array ${one.f['sa.array']}`);
    else check(`${c.tag}: the views drawn in one pass`, one.f['sa.array'] === 1 && each.f['sa.array'] === 0, `sa.array ${one.f['sa.array']} / ${each.f['sa.array']}`);
    check(`${c.tag}: one pass draws the per-view picture (pixels over 16 levels <= ${BOUND * 100}%)`, d <= BOUND, pct(d));
    if (c.tag === 'v4' || c.tag === 'v4-gi' || c.tag === 'v4-distort') {
      const shifted = run(gpu, `${c.tag}-shifted`, { ...c.env, CW_TEST_VIEWS_ARRAY: '1', CW_TEST_VIEWS_ARRAY_SHIFT: '1' });
      const ds = share(shifted, each);
      check(`control: each view's passes a view over must fail the bound`, ds > BOUND, pct(ds));
    }
    if (c.tag === 'v2-gi') {
      for (const pass of ['ssr', 'ssgi']) {
        const without = run(gpu, `${c.tag}-no-${pass}`, { ...c.env, CW_TEST_VIEWS_ARRAY: '0', CW_TEST_SKIP_PASS: pass });
        const dw = share(without, each);
        check(`${c.tag}: the ${pass} pass is in the picture (control: the frame without it must differ)`, dw > BOUND, pct(dw));
      }
    }
    if (c.tag === 'v2-distort') {
      const without = run(gpu, `${c.tag}-no-distort`, { ...c.env, CW_TEST_VIEWS_ARRAY: '0', CW_TEST_SKIP_PASS: 'distort' });
      const dw = share(without, each);
      check(`${c.tag}: the distortion is in the picture (control: the frame without the distort pass must differ)`, dw > BOUND, pct(dw));
    }
    if (c.tag === 'v4-msaa' && gpu === 'gl') {
      /* GL draws each view of the one pass on its own and flushes it: the scissor its rectangle set must be off for the
         passes after the views (the multisampled scene's resolve is a blit, which the scissor clips) */
      const kept = run(gpu, `${c.tag}-scissor`, { ...c.env, CW_TEST_VIEWS_ARRAY: '1', CW_TEST_GL_VIEW_SCISSOR: '1' });
      const dk = share(kept, each);
      check(`control: the last view's scissor left on after the views must fail the bound`, dk > BOUND, pct(dk));
    }
    if (c.tag === 'v4-verylow') {
      const nofog = run(gpu, `${c.tag}-nofog`, { ...c.env, CW_TEST_VIEWS_ARRAY: '0', CW_TEST_SKIP_PASS: 'height_fog' });
      const dn = share(nofog, each);
      check(`${c.tag}: the height fog is drawn in split screen (control: the frame without its pass must differ)`, dn > BOUND, pct(dn));
    }
  }
}
console.log(fail ? `views-array: ${fail} check(s) failed` : 'views-array: ok');
process.exit(fail ? 1 : 0);
