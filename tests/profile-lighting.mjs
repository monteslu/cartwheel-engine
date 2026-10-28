// Profiler gate for the clustered lighting pass in split screen (native host, GL and WebGPU):
// lighting_changes in its stress mode (LC_STRESS=40: 1600 boxes with a material each under 64 lights
// a game added and a cascaded sun) traced in one view and in four. The pass's scene work (the
// index walk and the material marking) runs once a frame whatever the view count, and four views
// cost the pass well under four times one view (each view only projects, sorts and bins).
// Checks, read from the profile trace (CW_PROFILE_TRACE), per backend:
//   - lighting.index and lighting.materials: one call a frame with four views;
//   - lighting.pass with four views under 2.5 times lighting.pass with one (the better of two
//     interleaved runs each, so another process on the machine does not decide it).
// Controls (must fail): CW_TEST_LIGHTS_EACH_VIEW=1, the scene work again for each view (as the pass
// did before), with four views; the cost check against an impossible ratio (1.0).
//   node tests/profile-lighting.mjs [--gpu gl,webgpu]
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out/gate');
mkdirSync(OUT, { recursive: true });
const exe = join(ROOT, 'build/native/tests/unit/lighting_changes');
const argv = process.argv.slice(2);
const GPUS = (argv.includes('--gpu') ? argv[argv.indexOf('--gpu') + 1] : 'gl,webgpu').split(',');
const START = 30, FRAMES = 60, RATIO = 2.5;
let fail = 0;
const check = (what, ok, detail = '') => {
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${detail ? ': ' + detail : ''}`);
};

/** one traced run: per-frame mean ms and calls of the lighting zones (main thread) */
function run(gpu, views, env = {}) {
  const trace = join(OUT, `profile-lighting-${gpu}-${views}.json`);
  rmSync(trace, { force: true });
  const r = spawnSync(exe, [], { encoding: 'utf8', env: { ...process.env, CW_GPU: gpu, CW_ASSETS: join(ROOT, 'tests/unit/lighting_changes_assets'),
    LC_STRESS: '40', LC_VIEWS: String(views), CW_FRAMES: String(START + FRAMES + 2), CW_PROFILE_TRACE: trace,
    CW_PROFILE_TRACE_START: String(START), CW_PROFILE_TRACE_FRAMES: String(FRAMES), ...env } });
  if (r.status !== 0 || !existsSync(trace)) throw new Error(`lighting_changes (${gpu}, ${views} views) failed: ${r.stderr.trim().split('\n').slice(-3).join(' | ')}`);
  const ev = JSON.parse(readFileSync(trace, 'utf8')).traceEvents;
  const frames = ev.filter((e) => e.ph === 'X' && e.name === 'frame' && e.tid === 0).length;
  const z = {};
  for (const e of ev) {
    if (e.ph !== 'X' || e.tid !== 0 || !e.name.startsWith('lighting.')) continue;
    const s = (z[e.name] ||= { ms: 0, calls: 0 });
    s.ms += e.dur / 1000;
    s.calls++;
  }
  for (const s of Object.values(z)) s.ms /= frames, s.calls /= frames;
  return z;
}
const ms = (z, n) => (z[n] ? z[n].ms : 0);
const calls = (z, n) => (z[n] ? z[n].calls : 0);

for (const gpu of GPUS) {
  const one = [], four = [];
  for (let k = 0; k < 2; k++) one.push(run(gpu, 1)), four.push(run(gpu, 4));
  const ctl = run(gpu, 4, { CW_TEST_LIGHTS_EACH_VIEW: '1' });
  const best = (a) => a.reduce((x, y) => (ms(y, 'lighting.pass') < ms(x, 'lighting.pass') ? y : x));
  const b1 = best(one), b4 = best(four);
  const ratio = ms(b4, 'lighting.pass') / ms(b1, 'lighting.pass');
  const ctlRatio = ms(ctl, 'lighting.pass') / ms(b1, 'lighting.pass');
  console.log(`  ${gpu}: lighting.pass ${ms(b1, 'lighting.pass').toFixed(3)} ms a frame in one view, ${ms(b4, 'lighting.pass').toFixed(3)} in four` +
    ` (x${ratio.toFixed(2)}); index ${ms(b4, 'lighting.index').toFixed(3)}, materials ${ms(b4, 'lighting.materials').toFixed(3)}, cascades ${ms(b4, 'lighting.csm').toFixed(3)} ms a frame in four` +
    `; control x${ctlRatio.toFixed(2)}`);
  const once = (z) => calls(z, 'lighting.index') === 1 && calls(z, 'lighting.materials') === 1 && calls(z, 'lighting.pass') === 4;
  check(`${gpu}: four views walk the scene once a frame (index ${calls(b4, 'lighting.index')}, materials ${calls(b4, 'lighting.materials')}, pass ${calls(b4, 'lighting.pass')} calls a frame)`, once(b4));
  check(`${gpu}: four views cost the pass under ${RATIO} times one view (x${ratio.toFixed(2)})`, ratio < RATIO);
  check(`${gpu}: control: the scene work each view fails the once-a-frame check (index ${calls(ctl, 'lighting.index')} a frame)`, !once(ctl),
    once(ctl) ? 'CONTROL PASSED' : '');
  check(`${gpu}: control: an impossible ratio (1.0) fails the cost check (x${ratio.toFixed(2)})`, !(ratio < 1.0), ratio < 1.0 ? 'CONTROL PASSED' : '');
}
process.exit(fail ? 1 : 0);
