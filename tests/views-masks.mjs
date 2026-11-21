// Per-view visibility (cw_object_set_views), native host, GL and WebGPU: samples/split_views shows and hides its
// objects per view with masks, in one pass (sv.array 1), and draws the picture its view hook draws (SV_HOOK=1, which
// shows and hides the same objects before each view and so draws view by view), within 0.003 of its pixels over 16
// levels; the masks drawn view by view (CW_TEST_VIEWS_ARRAY=0) draw it too. At two and four views, at High and Very Low.
// Further cases (each against the per-view picture of the same frame, CW_TEST_VIEWS_ARRAY=0):
//   - vm: a viewmodel per camera (each view's gun on its own layer, drawn by the viewmodel pass in its rectangle)
//   - water: a pond with planar reflection and refraction, drawn per view in one pass
//   - vis: a VisibilityManager culling to the frustum, per view in one pass
//   - vignette: the shared post chain's vignette in each view's rectangle (vignette_per_view) draws the vignette a
//     post chain of each view's own draws (cw_pipeline_set_views_shared_post off)
//   - spots, spots4: clustered spot lights with shadows (the shadow atlas) over the statues whose level of detail is
//     chosen per view, a depth prepass, a decal and particles beside them, the cameras moving: each view's spots cast
//     what that view draws (and the small casters the Shadows setting keeps from its own camera), at frames 90 and 91;
//     recorded together (views_submit 0) such a frame goes view by view
// Controls that must fail: SV_WRONG=1 (every mask the next view's) against the hook's picture; CW_TEST_VIEWS_ARRAY_SHIFT=1
// (each view's passes a view over) against the per-view picture; vignette_per_view off against the per-view chain;
// CW_TEST_VIEWS_SHARED_CASTERS=1 (every view's shadow casters view 0's, as before v0.57.4) against spots' per-view picture.
//   node tests/views-masks.mjs [--gpu gl,webgpu] [--only v2,vm,...]
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
const OUT = join(ROOT, 'out/gate/views-masks');
mkdirSync(OUT, { recursive: true });
const EXE = join(ROOT, 'build/native/samples/split_views');
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : d);
const GPUS = opt('--gpu', 'gl,webgpu').split(',');
const ONLY = opt('--only', '') ? opt('--only', '').split(',') : null;
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
  if (r.status !== 0 || !existsSync(shot)) throw new Error(`split_views ${tag} (${gpu}) failed (${r.status}): ${(r.stderr || '').trim().split('\n').slice(-4).join(' | ')}`);
  return { img: readImage(shot), f: existsSync(dump) ? JSON.parse(readFileSync(dump, 'utf8')) : {} };
}
const share = (a, b) => diffImages(a.img, b.img).gt16;
// the share of pixels over 16 levels away from the views' seams: a shared chain lays the views out (and darkens their
// seams) before the tone curve, a chain of each view's own after it, so the seam pixels differ between the two whatever the
// lens does (four quadrants: 3 pixels either side of the middle row and column and of the target's edges)
function shareOffSeams(a, b) {
  const { w, h } = a.img, near = (v, m) => Math.abs(v - m) < 3;
  let n = 0, all = 0;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    if (near(y, h / 2) || near(x, w / 2) || y < 3 || x < 3 || y >= h - 3 || x >= w - 3) continue;
    all++;
    const i = (y * w + x) * 4;
    let d = 0;
    for (let c = 0; c < 3; c++) d = Math.max(d, Math.abs(a.img.rgba[i + c] - b.img.rgba[i + c]));
    if (d > 16) n++;
  }
  return n / all;
}
const text = (f) => (typeof f === 'string' && /^[0-9a-f]+$/.test(f) ? Buffer.from(f, 'hex').toString().replace(/\0.*/s, '') : f || '');
const pct = (x) => `${(100 * x).toFixed(4)}%`;

// masks against the hook
const MASKS = [
  { tag: 'v2', env: { SV_VIEWS: '2', CW_QUALITY: 'high' } },
  { tag: 'v4', env: { SV_VIEWS: '4', CW_QUALITY: 'high' } },
  { tag: 'v4-verylow', env: { SV_VIEWS: '4', CW_QUALITY: 'verylow' } },
];
// a feature's one pass against its per-view frame
const FEATURES = [
  { tag: 'vm', env: { SV_VIEWS: '4', CW_QUALITY: 'high', SV_VM: '1' } },
  { tag: 'water', env: { SV_VIEWS: '2', CW_QUALITY: 'high', SV_WATER: '1' } },
  { tag: 'water4', env: { SV_VIEWS: '4', CW_QUALITY: 'high', SV_WATER: '1' } },
  { tag: 'vis', env: { SV_VIEWS: '4', CW_QUALITY: 'high', SV_VIS: '1' } },
  { tag: 'spots', env: { SV_VIEWS: '2', CW_QUALITY: 'high', SV_SPOTS: '1' } },
  { tag: 'spots4', env: { SV_VIEWS: '4', CW_QUALITY: 'high', SV_SPOTS: '1' } },
  { tag: 'spots-f91', env: { SV_VIEWS: '2', CW_QUALITY: 'high', SV_SPOTS: '1', CW_FRAMES: '91' } },
];
const SPOTS = (t) => t.startsWith('spots');
const want = (t) => !ONLY || ONLY.includes(t);
for (const gpu of GPUS) {
  console.log(`-- ${gpu}`);
  for (const c of MASKS) {
    if (!want(c.tag)) continue;
    const one = run(gpu, `${c.tag}-masks`, { ...c.env, CW_TEST_VIEWS_ARRAY: '1' });
    const each = run(gpu, `${c.tag}-masks-perview`, { ...c.env, CW_TEST_VIEWS_ARRAY: '0' });
    const hook = run(gpu, `${c.tag}-hook`, { ...c.env, SV_HOOK: '1' });
    check(`${c.tag}: masks keep the frame in one pass, the hook does not`, one.f['sv.array'] === 1 && hook.f['sv.array'] === 0 && one.f['sv.masks'] > 0,
      `sv.array ${one.f['sv.array']} / ${hook.f['sv.array']}, masks ${one.f['sv.masks']}`);
    const d1 = share(one, hook), d2 = share(each, hook);
    check(`${c.tag}: one pass with masks draws the hook's picture (<= ${BOUND * 100}%)`, d1 <= BOUND, pct(d1));
    check(`${c.tag}: masks drawn view by view draw the hook's picture`, d2 <= BOUND, pct(d2));
    if (c.tag === 'v4') { // the views drawn and sent to the GPU one by one (WebGPU: submitted, GL: flushed) against all recorded first
      const b = run(gpu, `${c.tag}-batch`, { ...c.env, CW_TEST_VIEWS_ARRAY: '1', CW_TEST_VIEWS_SUBMIT: '0' });
      const db = share(one, b);
      check(`${c.tag}: the views sent to the GPU one by one draw what they draw recorded together`, b.f['sv.array'] === 1 && one.f['cw.views_submits'] === 4 && b.f['cw.views_submits'] === 0 && db <= BOUND, `${pct(db)}, submits ${one.f['cw.views_submits']} / ${b.f['cw.views_submits']}`);
    }
    if (c.tag === 'v4') {
      const w = run(gpu, `${c.tag}-wrong`, { ...c.env, CW_TEST_VIEWS_ARRAY: '1', SV_WRONG: '1' });
      const dw = share(w, hook);
      check(`control: every mask the next view's must fail the bound`, dw > BOUND, pct(dw));
    }
  }
  for (const c of FEATURES) {
    if (!want(c.tag)) continue;
    const one = run(gpu, `${c.tag}-onepass`, { ...c.env, CW_TEST_VIEWS_ARRAY: '1' });
    const each = run(gpu, `${c.tag}-perview`, { ...c.env, CW_TEST_VIEWS_ARRAY: '0' });
    check(`${c.tag}: the views drawn in one pass`, one.f['sv.array'] === 1 && each.f['sv.array'] === 0, `sv.array ${one.f['sv.array']} / ${each.f['sv.array']}${text(one.f['sv.why']) ? ' (' + text(one.f['sv.why']) + ')' : ''}`);
    const d = share(one, each);
    check(`${c.tag}: one pass draws the per-view picture (<= ${BOUND * 100}%)`, d <= BOUND, pct(d));
    {
      const b = run(gpu, `${c.tag}-batch`, { ...c.env, CW_TEST_VIEWS_ARRAY: '1', CW_TEST_VIEWS_SUBMIT: '0' });
      const db = share(b, each);
      // (spots: one atlas for every view cannot hold each view's casters, so that frame goes view by view)
      const how = SPOTS(c.tag) ? b.f['sv.array'] === 0 && text(b.f['sv.why']) === 'lights' : b.f['sv.array'] === 1;
      check(`${c.tag}: recorded together, then sent to the GPU, too`, how && db <= BOUND, `${pct(db)}, sv.array ${b.f['sv.array']} ${text(b.f['sv.why'])}`);
    }
    if (SPOTS(c.tag) && c.tag !== 'spots-f91') {
      const s = run(gpu, `${c.tag}-shared`, { ...c.env, CW_TEST_VIEWS_ARRAY: '1', CW_TEST_VIEWS_SHARED_CASTERS: '1' });
      const ds = share(s, each);
      check(`control (${c.tag}): every view's shadow casters view 0's must fail the bound`, ds > BOUND, pct(ds));
    }
    if (c.tag === 'vm' || c.tag === 'water') {
      const s = run(gpu, `${c.tag}-shifted`, { ...c.env, CW_TEST_VIEWS_ARRAY: '1', CW_TEST_VIEWS_ARRAY_SHIFT: '1' });
      const ds = share(s, each);
      check(`control (${c.tag}): each view's passes a view over must fail the bound`, ds > BOUND, pct(ds));
    }
  }
  if (want('vignette')) {
    const env = { SV_VIEWS: '4', CW_QUALITY: 'high', SV_VIGNETTE: '1' };
    const shared = run(gpu, 'vig-shared', { ...env, CW_TEST_VIEWS_ARRAY: '1' });
    const own = run(gpu, 'vig-own', { ...env, SV_OWN_POST: '1' });
    const whole = run(gpu, 'vig-whole', { ...env, CW_TEST_VIEWS_ARRAY: '1', SV_VIGNETTE: '2' });
    check('vignette: a shared chain in one pass', shared.f['sv.array'] === 1, `sv.array ${shared.f['sv.array']}`);
    const d = shareOffSeams(shared, own), dw = shareOffSeams(whole, own);
    check(`vignette: a vignette per view on the shared chain draws each view's own chain's, off the seams (<= ${BOUND * 100}%)`, d <= BOUND, pct(d));
    check('control: the screen\'s one vignette must fail the bound', dw > BOUND, pct(dw));
  }
}
console.log(fail ? `views-masks: ${fail} check(s) failed` : 'views-masks: ok');
process.exit(fail ? 1 : 0);
