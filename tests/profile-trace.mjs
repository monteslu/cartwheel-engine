// Profiler gate, native host: samples/profile_demo writes a Chrome trace
// (CW_PROFILE_TRACE) that parses, holds the engine's phase zones nested per
// thread, its job zones on workers, the audio mixer and (where the GL driver
// has timer queries) the GPU lane; profiling changes nothing the game
// simulates; and the zones cost the main thread under 0.1 ms a frame.
// The runs use SDL's dummy audio driver, as the other native gates do: it
// needs no device, so the mixer runs on a machine without one (a CI runner)
// too, and the demo's audio stats confirm it mixed.
// Controls that must fail: a run that never records, a trace whose zones
// overlap, a trace without the mixer's zones, a run one tick short (the
// state check), an impossible budget.
//   node tests/profile-trace.mjs
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { checkTrace, breakNesting, dropZones } from './profile-check.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out/gate');
mkdirSync(OUT, { recursive: true });
const exe = join(ROOT, 'build/native/samples/profile_demo');
const START = 10, FRAMES = 60;
let fail = 0;
const check = (what, ok, detail = '') => {
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${detail ? ': ' + detail : ''}`);
};
/** runs the demo; returns stdout lines as JSON (the CW_DEBUG_CMDS replies) */
function run(env) {
  const r = spawnSync(exe, [], { encoding: 'utf8', env: { ...process.env, SDL_AUDIODRIVER: 'dummy', ...env } });
  if (r.status !== 0) throw new Error(`profile_demo failed: ${r.stderr.trim().split('\n').slice(-3).join(' | ')}`);
  return r.stdout.trim().split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));
}
const fields = ['demo.tick', 'demo.crates', 'demo.flock', 'phys.hash'];
const state = (file) => {
  const s = JSON.parse(readFileSync(file, 'utf8'));
  return JSON.stringify(fields.map((f) => s[f]));
};

// the trace, the GPU lane asked for
const trace = join(OUT, 'profile-trace.json');
rmSync(trace, { force: true });
const [pipe, au] = run({ CW_FRAMES: String(START + FRAMES + 5), CW_PROFILE_TRACE: trace, CW_PROFILE_TRACE_START: String(START),
  CW_PROFILE_TRACE_FRAMES: String(FRAMES), CW_PROFILE_GPU: '1', CW_DEBUG_CMDS: 'pipeline;fields audio.mixed_frames' });
check(`the mixer ran in the traced run (audio.mixed_frames ${au?.['audio.mixed_frames']})`, au?.['audio.mixed_frames'] > 0);
const text = existsSync(trace) ? readFileSync(trace, 'utf8') : '';
const timers = pipe?.timer_query === true;
const problems = checkTrace(text, { frames: FRAMES, gpu: timers, jobs: ['physics3d.step/job', 'game.flock/job'], audio: true });
check(`trace of ${FRAMES} frames (${text.length} bytes; GPU lane ${timers ? 'checked' : 'not checked: no GL timer queries here'})`, !problems.length,
  problems.slice(0, 6).join('; '));

// control: profiling never on, so no trace (the "has zones" check must fail)
const none = join(OUT, 'profile-trace-off.json');
rmSync(none, { force: true });
run({ CW_FRAMES: '20', CW_PROFILE_TRACE: none, CW_PROFILE_TRACE_START: '1000' });
const offProblems = checkTrace(existsSync(none) ? readFileSync(none, 'utf8') : '', { frames: FRAMES });
check('control: a run that never recorded fails the checks', offProblems.length > 0, offProblems[0] || 'CONTROL PASSED');
// control: the mixer ran, but its zones are not in the trace
const noMix = text ? checkTrace(dropZones(text, 'audio.mix'), { frames: FRAMES, gpu: timers, jobs: ['physics3d.step/job', 'game.flock/job'], audio: true }) : [];
check('control: a trace without the mixer\'s zones fails the audio check', noMix.some((p) => p.includes('audio.mix')), noMix[0] || 'CONTROL PASSED');
// control: zones that overlap their parent's end
const broken = text ? checkTrace(breakNesting(text), { frames: FRAMES, gpu: timers, jobs: ['physics3d.step/job', 'game.flock/job'], audio: true }) : [];
check('control: a trace with a zone overlapping its parent fails the nesting check', broken.some((p) => p.includes('overlaps')), broken[0] || 'CONTROL PASSED');

// profiling changes nothing the game simulates: the state at frame 120 with
// it off, on, and on with GPU timing; a run one tick short must differ
const st = (name, env) => {
  const f = join(OUT, `profile-state-${name}.json`);
  run({ CW_FRAMES: '120', CW_DEBUG_DUMP: f, ...env });
  return state(f);
};
const off = st('off', {}), on = st('on', { CW_PROFILE: '1' }), onGpu = st('on-gpu', { CW_PROFILE: '1', CW_PROFILE_GPU: '1' });
const short = (() => {
  const f = join(OUT, 'profile-state-short.json');
  run({ CW_FRAMES: '119', CW_DEBUG_DUMP: f, CW_PROFILE: '1' });
  return state(f);
})();
check(`state with profiling off, on, on with GPU timing (${fields.join(', ')})`, off === on && on === onGpu, `${off} / ${on} / ${onGpu}`);
check('control: a run one tick short differs', short !== off, short);

// the cost: ns per zone (the demo's zone_bench) times the main thread's zones a frame
const bench = run({ CW_FRAMES: '5', CW_DEBUG_CMDS: 'zone_bench 200000;zone_bench 200000;zone_bench 200000' });
const onNs = Math.min(...bench.map((b) => b.on_ns)), offNs = Math.min(...bench.map((b) => b.off_ns));
let perFrame = 0;
try {
  const t = JSON.parse(text);
  perFrame = t.traceEvents.filter((e) => e.ph === 'X' && e.tid === 0 && e.name !== 'frame').length / FRAMES;
} catch { /* reported above */ }
const msOn = (perFrame * onNs) / 1e6, msOff = (perFrame * offNs) / 1e6;
const budget = (ms, limit) => ms > 0 && ms < limit;
check(`cost: ${perFrame.toFixed(1)} main-thread zones a frame x ${onNs.toFixed(1)} ns = ${msOn.toFixed(4)} ms a frame on, `
  + `${msOff.toFixed(5)} ms off (budget 0.1 ms)`, budget(msOn, 0.1));
check('control: the same cost against an impossible budget fails', !budget(msOn, 1e-6));

console.log(fail ? `profile-trace: ${fail} failures` : 'ok   profile-trace');
process.exit(fail ? 1 : 0);
