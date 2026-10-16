// The checks a profiler trace (cartwheel/profile.h) must pass, shared by
// tests/profile-trace.mjs (native) and tests/profile-cart.mjs (romdev).

/** the zones every trace of samples/profile_demo must hold (main thread) */
export const MAIN_ZONES = ['frame', 'input', 'fixed_update', 'tree.fixed_update', 'physics3d.step', 'game.flock', 'anim.player', 'update',
  'tree.update', 'render', 'tree.pre_render', 'pipeline.render', 'render3d.prepare', 'pass scene', 'present'];

/**
 * Checks a trace's text. Returns the problems found (empty = a good trace).
 * @param {string} text the trace JSON
 * @param {{ frames: number, gpu?: boolean, jobs?: string[], audio?: boolean }} want
 *   jobs: job zone names that must run on a worker; audio: an audio.mix zone somewhere
 */
export function checkTrace(text, want) {
  const problems = [];
  let t;
  try { t = JSON.parse(text); } catch (e) { return [`the trace does not parse: ${e.message}`]; }
  if (!Array.isArray(t?.traceEvents)) return ['no traceEvents array'];
  const ev = t.traceEvents;
  const names = new Map(ev.filter((e) => e.ph === 'M' && e.name === 'thread_name').map((e) => [e.tid, e.args.name]));
  const zones = ev.filter((e) => e.ph === 'X');
  if (!zones.length) return ['the trace has no zones'];
  for (const e of zones)
    if (!(typeof e.name === 'string' && Number.isFinite(e.ts) && e.ts >= 0 && Number.isFinite(e.dur) && e.dur >= 0 && Number.isInteger(e.tid))) {
      problems.push(`a malformed zone: ${JSON.stringify(e)}`);
      break;
    }
  for (const tid of new Set(zones.map((e) => e.tid))) if (!names.has(tid)) problems.push(`thread ${tid} has zones but no name`);
  const frames = zones.filter((e) => e.name === 'frame' && e.tid === 0);
  if (frames.length !== want.frames) problems.push(`${frames.length} frames, asked for ${want.frames}`);
  if (t.otherData?.frames !== want.frames) problems.push(`otherData.frames ${t.otherData?.frames}, asked for ${want.frames}`);
  // per thread: the zones nest (a stack's zones never overlap partly). ts and dur
  // are microseconds with 3 decimals: 0.002 of rounding is allowed
  const eps = 0.002;
  for (const tid of new Set(zones.map((e) => e.tid))) {
    const z = zones.filter((e) => e.tid === tid).sort((a, b) => a.ts - b.ts || b.dur - a.dur);
    const stack = [];
    for (const e of z) {
      while (stack.length && stack[stack.length - 1].ts + stack[stack.length - 1].dur <= e.ts + eps) stack.pop();
      const top = stack[stack.length - 1];
      if (top && e.ts + e.dur > top.ts + top.dur + eps) {
        problems.push(`${names.get(tid)}: "${e.name}" at ${e.ts} overlaps the end of "${top.name}" (${top.ts} + ${top.dur})`);
        break;
      }
      stack.push(e);
    }
  }
  const main = new Set(zones.filter((e) => e.tid === 0).map((e) => e.name));
  for (const n of MAIN_ZONES) if (!main.has(n)) problems.push(`no "${n}" zone on the main thread`);
  for (const j of want.jobs || []) {
    const on = zones.filter((e) => e.name === j && /^worker /.test(names.get(e.tid) || ''));
    if (!on.length) problems.push(`no "${j}" zone on a worker`);
  }
  if (want.audio && !zones.some((e) => e.name === 'audio.mix')) problems.push('no audio.mix zone');
  const gpu = zones.filter((e) => names.get(e.tid) === 'GPU');
  if (want.gpu && !gpu.some((e) => e.name === 'scene' && e.dur > 0)) problems.push('no GPU time for the scene pass');
  if (want.gpu === false && gpu.length) problems.push(`${gpu.length} GPU zones where none can be`);
  return problems;
}

/** a copy of the trace text with one zone made to overlap its parent's end (the nesting control) */
export function breakNesting(text) {
  const t = JSON.parse(text);
  const parent = t.traceEvents.find((e) => e.ph === 'X' && e.tid === 0 && e.name === 'render');
  const child = t.traceEvents.find((e) => e.ph === 'X' && e.tid === 0 && e.name === 'pipeline.render' && e.ts >= parent.ts && e.ts < parent.ts + parent.dur);
  child.dur = parent.ts + parent.dur - child.ts + 50;
  return JSON.stringify(t);
}
