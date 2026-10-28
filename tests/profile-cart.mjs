// Profiler gate, cart host (romdev, ROMDEV_PORT default 7331): the
// profile_demo cart records a trace over the debug channel (`profile trace`,
// read back in pieces with `profile trace_get` by tools/cwtrace.mjs) that
// parses and holds the engine's phase zones nested per thread, job zones on
// workers and the audio mixer, and no GPU lane (carts issue no timer queries);
// profiling changes nothing the cart simulates, and the state matches the
// native run's; the zones cost the main thread under 0.1 ms a frame.
// Controls that must fail: trace_get before any trace, a state one tick
// short, an impossible budget.
//   node tests/profile-cart.mjs
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testGpu, wgpuPin } from '../tools/lib/wgpu-pin.mjs';
import { checkTrace } from './profile-check.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out/gate');
mkdirSync(OUT, { recursive: true });
const port = process.env.ROMDEV_PORT || 7331;
const cart = join(ROOT, 'build/cart-wgpu/carts/profile_demo.wasc') // the dual cart, run on GL (wgpu: false);
const exe = join(ROOT, 'build/native/samples/profile_demo');
if (!existsSync(cart)) { console.error('no cart', cart); process.exit(1); }
const FRAMES = 60;
let fail = 0;
const check = (what, ok, detail = '') => {
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${detail ? ': ' + detail : ''}`);
};

async function romdev(tool, args, s) {
  const res = await fetch(`http://127.0.0.1:${port}/tool/${tool}`, {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'x-romdev-session': s }, body: JSON.stringify(args),
  });
  const text = await res.text();
  let body;
  try { body = JSON.parse(text); } catch { body = text; }
  if (!res.ok || body?.isError) throw new Error(`romdev ${tool}: ${typeof body === 'string' ? body : JSON.stringify(body).slice(0, 400)}`);
  return body;
}
const command = async (text, s, maxFrames = 4) => {
  const r = await romdev('wasm', { op: 'command', text, maxFrames }, s);
  const rep = r?.reply ?? r;
  return typeof rep === 'string' ? JSON.parse(rep) : rep;
};
const fields = ['demo.tick', 'demo.crates', 'demo.flock', 'phys.hash'];
const load = (s) => romdev('loadMedia', { platform: 'wasmcart', gpuPower: wgpuPin().power, wgpu: false, path: cart, deterministicSeed: 1, deterministicStepMs: 1000 / 60, gpuPower: testGpu().power }, s);
/** the cart's state after `frames` frames, with profiling on from frame 1 or not */
async function cartState(frames, profile) {
  const s = `cw-profile-state-${process.pid}-${frames}-${profile ? 1 : 0}`;
  await load(s);
  try {
    let ran = 1; // the load ran frame 1
    if (profile) {
      const on = await command('profile on', s);
      if (on?.enabled !== true) throw new Error(`profile on: ${JSON.stringify(on)}`);
      ran++; // the command ran a frame
    }
    await romdev('frame', { op: 'step', frames: frames - ran }, s);
    const st = await romdev('wasm', { op: 'debugState' }, s);
    const m = Object.fromEntries((st.fields || []).map((f) => [f.name, f.values ?? f.value]));
    return JSON.stringify(fields.map((f) => m[f]));
  } finally {
    await romdev('host', { op: 'shutdown' }, s).catch(() => {});
  }
}

// the trace, through the tool a user runs
const out = join(OUT, 'profile-cart.json');
const r = spawnSync('node', [join(ROOT, 'tools/cwtrace.mjs'), cart, '-o', out, '--frames', String(FRAMES), '--start', '10'], { encoding: 'utf8' });
const text = r.status === 0 && existsSync(out) ? readFileSync(out, 'utf8') : '';
if (r.status !== 0) console.log(r.stderr.trim());
const problems = checkTrace(text, { frames: FRAMES, gpu: false, jobs: ['physics3d.step/job', 'game.flock/job'], audio: true });
check(`cart trace of ${FRAMES} frames over the debug channel (${text.length} bytes)`, !problems.length, problems.slice(0, 6).join('; '));

// control, and the cost: trace_get before any trace must fail; zone_bench in the cart
const s = `cw-profile-cart-${process.pid}`;
await load(s);
let bench = [];
try {
  const early = await command('profile trace_get 0', s);
  check('control: trace_get before any trace is refused', !!early?.error, JSON.stringify(early).slice(0, 120));
  for (let i = 0; i < 3; i++) bench.push(await command('zone_bench 100000', s));
} finally {
  await romdev('host', { op: 'shutdown' }, s).catch(() => {});
}
const onNs = Math.min(...bench.map((b) => b.on_ns)), offNs = Math.min(...bench.map((b) => b.off_ns));
let perFrame = 0;
try {
  perFrame = JSON.parse(text).traceEvents.filter((e) => e.ph === 'X' && e.tid === 0 && e.name !== 'frame').length / FRAMES;
} catch { /* reported above */ }
const msOn = (perFrame * onNs) / 1e6;
const budget = (ms, limit) => ms > 0 && ms < limit;
check(`cart cost: ${perFrame.toFixed(1)} main-thread zones a frame x ${onNs.toFixed(0)} ns = ${msOn.toFixed(4)} ms a frame on, `
  + `${((perFrame * offNs) / 1e6).toFixed(5)} ms off (budget 0.1 ms)`, budget(msOn, 0.1));
check('control: the same cost against an impossible budget fails', !budget(msOn, 1e-6));

// determinism: the cart with profiling off and on, and the native run, at frame 120
const off = await cartState(120, false), on = await cartState(120, true), short = await cartState(119, true);
const nat = (() => {
  const f = join(OUT, 'profile-cart-native.json');
  const n = spawnSync(exe, [], { encoding: 'utf8', env: { ...process.env, CW_FRAMES: '120', CW_DEBUG_DUMP: f } });
  if (n.status !== 0) return `native failed: ${n.stderr.slice(-200)}`;
  const st = JSON.parse(readFileSync(f, 'utf8'));
  return JSON.stringify(fields.map((k) => st[k]));
})();
check(`cart state with profiling off and on equals native (${fields.join(', ')})`, off === on && on === nat, `${off} / ${on} / native ${nat}`);
check('control: a cart run one tick short differs', short !== on, short);

console.log(fail ? `profile-cart: ${fail} failures` : 'ok   profile-cart');
process.exit(fail ? 1 : 0);
