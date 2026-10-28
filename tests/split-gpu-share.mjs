// GPU work the renderer does once where it did it twice, and the same picture for it (native host, GL and WebGPU).
//   1. One resolve of the multisampled scene (WebGPU): viewmodel_demo at 4x MSAA. The scene pass leaves its samples
//      unresolved and the viewmodel pass after it resolves: cw.msaa_resolves is 1 a frame (was 2), and the frame is
//      the one both resolving draw (CW_TEST_WGPU_RESOLVE_TWICE=1, the old path), to the bit.
//      Control: CW_TEST_WGPU_RESOLVE_TWICE=1 must fail the one-resolve check.
//   2. The depth prepass on GL and WebGPU: viewmodel_demo at 4x MSAA with depth_prepass on draws the frame it draws
//      with it off, to the bit, and the prepass ran (its GPU time is in the trace). Control: CW_TEST_PREPASS_SHIFT=1
//      (the prepass's depth squeezed into 0..0.5) must change the frame.
//   3. The profiler's GPU lane in split screen: msaa_hdr in four views (MH_VIEWS=4) with CW_PROFILE_GPU=1 times each
//      view's scene pass apart (scene@v0..scene@v3) and the pipeline's own steps (views, resolve), on GL and WebGPU.
//      Control: the same run without CW_PROFILE_GPU has no GPU lane, and must fail the check.
//   node tests/split-gpu-share.mjs [--gpu gl,webgpu]
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
const OUT = join(ROOT, 'out/gate/split-gpu-share');
mkdirSync(OUT, { recursive: true });
const BIN = join(ROOT, 'build/native/samples');
const argv = process.argv.slice(2);
const GPUS = (argv.includes('--gpu') ? argv[argv.indexOf('--gpu') + 1] : 'gl,webgpu').split(',');
let fail = 0;
const check = (what, ok, detail = '') => {
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${detail ? ': ' + detail : ''}`);
};

/** one native run: its last frame (PNG), the debug fields, and the trace's GPU lane names when traced */
function run(app, gpu, tag, env = {}, { frames = 30, trace = false } = {}) {
  const exe = join(BIN, app);
  const shot = join(OUT, `${tag}-${gpu}.png`), dump = join(OUT, `${tag}-${gpu}.json`), tr = join(OUT, `${tag}-${gpu}-trace.json`);
  for (const f of [shot, dump, tr]) rmSync(f, { force: true });
  const e = { ...process.env, CW_GPU: gpu, CW_FRAMES: String(frames), CW_SCREENSHOT: shot, CW_DEBUG_DUMP: dump, SDL_AUDIODRIVER: 'dummy', ...env };
  if (gpu === 'webgpu') Object.assign(e, wgpuPin().env, { CW_GPU: 'webgpu' });
  if (trace) Object.assign(e, { CW_PROFILE_TRACE: tr, CW_PROFILE_TRACE_START: String(frames - 40), CW_PROFILE_TRACE_FRAMES: '38' });
  const r = spawnSync(exe, [], { encoding: 'utf8', env: e });
  if (r.status !== 0 || !existsSync(shot)) throw new Error(`${app} ${tag} (${gpu}) failed (${r.status}): ${(r.stderr || '').trim().split('\n').slice(-4).join(' | ')}`);
  const fields = existsSync(dump) ? JSON.parse(readFileSync(dump, 'utf8')) : {};
  let lane = new Map();
  if (trace && existsSync(tr)) {
    for (const ev of JSON.parse(readFileSync(tr, 'utf8')).traceEvents)
      if (ev.ph === 'X' && ev.tid === 1000) lane.set(ev.name, (lane.get(ev.name) || 0) + ev.dur / 1000);
  }
  return { img: readImage(shot), fields, lane };
}
const field = (f, k) => f[k];
const same = (a, b) => { const d = diffImages(a.img, b.img); return { d, exact: d.comparable && d.any === 0 }; };

// the three.c this build compiled (build/native/CMakeCache.txt): the WebGPU checks of 1 and 2 need a three.c with
// t3_external_target keep_multisampled and the WebGPU depth prepass (T3_EXTERNAL_TARGET_KEEP_MULTISAMPLED,
// T3_WGPU_DEPTH_PREPASS); with an older one the engine resolves twice and has no WebGPU prepass, and they are skipped
const cache = readFileSync(join(ROOT, 'build/native/CMakeCache.txt'), 'utf8');
const t3dir = (/^CW_THREE_DIR:PATH=(.*)$/m.exec(cache) || [])[1] || join(ROOT, 'deps/three.c');
const t3h = readFileSync(join(t3dir, 'include/three.h'), 'utf8');
const KEEP = t3h.includes('T3_EXTERNAL_TARGET_KEEP_MULTISAMPLED'), WPRE = t3h.includes('T3_WGPU_DEPTH_PREPASS');
if (!KEEP || !WPRE) console.log(`note three.c at ${t3dir} lacks ${KEEP ? '' : 'keep_multisampled '}${WPRE ? '' : 'the WebGPU depth prepass'}: those WebGPU checks are skipped`);
const VM = { VIEWMODEL_MSAA: '4' };
for (const gpu of GPUS) {
  console.log(`-- ${gpu}`);
  // 1. one resolve (WebGPU only: GL resolves with one blit after the opaque phase already)
  if (gpu === 'webgpu' && KEEP) {
    const one = run('viewmodel_demo', gpu, 'vm-msaa', VM);
    const two = run('viewmodel_demo', gpu, 'vm-msaa-twice', { ...VM, CW_TEST_WGPU_RESOLVE_TWICE: '1' });
    const r1 = field(one.fields, 'cw.msaa_resolves'), r2 = field(two.fields, 'cw.msaa_resolves');
    const s = same(one, two);
    check('one resolve of the multisampled scene a frame (the viewmodel pass resolves)', r1 === 1, `cw.msaa_resolves ${r1}`);
    check('the frame is the one two resolves draw, to the bit', s.exact, `any ${(100 * (s.d.any || 0)).toFixed(4)}%, max ${s.d.max}`);
    check('control: CW_TEST_WGPU_RESOLVE_TWICE=1 resolves twice (must fail the one-resolve check)', r2 === 2, `cw.msaa_resolves ${r2}`);
    check('the gun is drawn in both (the viewmodel pass ran)', field(one.fields, 'vm.on') === 1 && field(two.fields, 'vm.on') === 1);
  }
  // 2. the depth prepass
  if (gpu === 'gl' || WPRE) {
    const off = run('viewmodel_demo', gpu, 'vm-noprepass', VM);
    const on = run('viewmodel_demo', gpu, 'vm-prepass', { ...VM, VIEWMODEL_PREPASS: '1', CW_PROFILE_GPU: '1' }, { frames: 80, trace: true });
    const bad = run('viewmodel_demo', gpu, 'vm-prepass-shift', { ...VM, VIEWMODEL_PREPASS: '1', CW_TEST_PREPASS_SHIFT: '1' });
    const s = same(off, on), c = same(off, bad);
    const pre = on.lane.get('depth_prepass');
    check('the depth prepass draws the frame drawn without it, to the bit', s.exact, `any ${(100 * (s.d.any || 0)).toFixed(4)}%, max ${s.d.max}`);
    if (on.lane.size) check('the depth prepass ran (its GPU time is in the trace)', pre !== undefined && pre > 0, `${pre}`);
    else console.log('note no GPU lane (no timer queries on this device): the prepass run is not proved by time');
    check('control: a prepass depth squeezed into 0..0.5 changes the frame (must)', !c.exact && c.d.any > 0.01, `any ${(100 * (c.d.any || 0)).toFixed(2)}%`);
  }
  // 3. the GPU lane per view
  {
    const timed = run('msaa_hdr', gpu, 'mh4-timed', { MH_VIEWS: '4', MH_MSAA: '4', CW_PROFILE_GPU: '1' }, { frames: 80, trace: true });
    const plain = run('msaa_hdr', gpu, 'mh4-untimed', { MH_VIEWS: '4', MH_MSAA: '4' }, { frames: 80, trace: true });
    const want = ['scene@v0', 'scene@v1', 'scene@v2', 'scene@v3', 'views', 'resolve@v0'];
    const has = (l) => want.filter((n) => (l.get(n) || 0) > 0);
    check('split screen: each view\'s scene pass and the pipeline\'s steps in the GPU lane', has(timed.lane).length === want.length,
      want.map((n) => `${n} ${(timed.lane.get(n) || 0).toFixed(3)}`).join(', '));
    check('control: without CW_PROFILE_GPU there is no GPU lane (must fail the check above)', has(plain.lane).length === 0, `${has(plain.lane).length} found`);
  }
}
console.log(fail ? `split-gpu-share: ${fail} check(s) failed` : 'split-gpu-share: ok');
process.exit(fail ? 1 : 0);
