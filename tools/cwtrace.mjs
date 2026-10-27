// Capture a profiler trace (cartwheel/profile.h) and write it as a Chrome
// trace JSON file: open it in ui.perfetto.dev, chrome://tracing or speedscope.
//
//   node tools/cwtrace.mjs <app.wasc | native executable> [-o trace.json] [--frames 60] [--start 60] [--gpu]
//   node tools/cwtrace.mjs --session <romdev session> [-o trace.json] [--frames 60]
//
// A cart is loaded in romdev (ROMDEV_PORT, default 7331), run to --start, and
// asked `profile trace <frames>` over the debug channel; the trace is then read
// back with `profile trace_get` in pieces. --session uses a cart that is
// already running in that romdev session instead (it is left running).
// A native executable is run with CW_PROFILE_TRACE (offscreen; --gpu adds the
// render passes' GPU times, native only).
import { spawnSync } from 'node:child_process';
import { writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { wgpuPin } from './lib/wgpu-pin.mjs';

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf(name);
  if (i < 0) return def;
  const v = argv[i + 1];
  argv.splice(i, 2);
  return v;
};
const flag = (name) => {
  const i = argv.indexOf(name);
  if (i >= 0) argv.splice(i, 1);
  return i >= 0;
};
const out = resolve(opt('-o', 'trace.json'));
const frames = +opt('--frames', '60');
const start = +opt('--start', '60');
const session = opt('--session', '');
const gpu = flag('--gpu');
const port = process.env.ROMDEV_PORT || 7331;
const target = argv[0];
if ((!target && !session) || !(frames >= 1)) {
  console.error('usage: cwtrace.mjs <app.wasc | native executable> [-o trace.json] [--frames 60] [--start 60] [--gpu]\n'
    + '       cwtrace.mjs --session <romdev session> [-o trace.json] [--frames 60]');
  process.exit(2);
}

/** one romdev tool call (the REST route) */
async function romdev(tool, args, s) {
  const res = await fetch(`http://127.0.0.1:${port}/tool/${tool}`, {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'x-romdev-session': s }, body: JSON.stringify(args),
  });
  const text = await res.text();
  let body;
  try { body = JSON.parse(text); } catch { body = text; }
  if (!res.ok || body?.isError) throw new Error(`romdev ${tool}: ${typeof body === 'string' ? body : JSON.stringify(body).slice(0, 500)}`);
  if (Array.isArray(body?.content)) {
    const t = body.content.find((c) => c.type === 'text');
    try { return JSON.parse(t.text); } catch { return t?.text; }
  }
  return body;
}

/** a debug channel request through romdev; the reply as an object */
async function command(text, s, maxFrames = 4) {
  const r = await romdev('wasm', { op: 'command', text, maxFrames }, s);
  const rep = r?.reply ?? r;
  const j = typeof rep === 'string' ? JSON.parse(rep) : rep;
  if (j?.error) throw new Error(`${text}: ${j.error}`);
  return j;
}

/**
 * Records `n` frames in a romdev session's cart and reads the trace back.
 * @returns {Promise<string>} the trace JSON text
 */
async function traceRomdev(s, n) {
  const head = await command(`profile trace ${n}`, s, n + 30);
  if (!head || !(head.bytes > 0)) throw new Error(`profile trace: no trace (${JSON.stringify(head)})`);
  let text = '';
  while (text.length < head.bytes) {
    const piece = await command(`profile trace_get ${text.length}`, s);
    if (!(piece.bytes > 0)) throw new Error(`profile trace_get ${text.length}: an empty piece`);
    text += piece.text;
  }
  if (text.length !== head.bytes) throw new Error(`trace: ${text.length} bytes read, ${head.bytes} announced`);
  return text;
}

let text;
if (session) {
  text = await traceRomdev(session, frames);
} else if (target.endsWith('.wasc')) {
  const s = `cwtrace-${process.pid}`;
  await romdev('loadMedia', { platform: 'wasmcart', gpuPower: wgpuPin().power, path: resolve(target), deterministicSeed: 1, deterministicStepMs: 1000 / 60 }, s);
  try {
    if (start > 1) await romdev('frame', { op: 'step', frames: start - 1 }, s);
    text = await traceRomdev(s, frames);
  } finally {
    await romdev('host', { op: 'shutdown' }, s).catch(() => {});
  }
} else {
  const r = spawnSync(resolve(target), [], {
    encoding: 'utf8',
    env: { SDL_VIDEODRIVER: 'offscreen', ...process.env, CW_FRAMES: String(start + frames + 2), CW_PROFILE_TRACE: out,
      CW_PROFILE_TRACE_START: String(start), CW_PROFILE_TRACE_FRAMES: String(frames), ...(gpu ? { CW_PROFILE_GPU: '1' } : {}) },
  });
  if (r.status !== 0) { console.error(r.stderr); process.exit(1); }
}
if (text !== undefined) writeFileSync(out, text);
const t = JSON.parse(text ?? (await import('node:fs')).readFileSync(out, 'utf8'));
const lanes = t.traceEvents.filter((e) => e.ph === 'M' && e.name === 'thread_name').map((e) => e.args.name);
console.log(`wrote ${out}: ${t.otherData.frames} frames, ${t.traceEvents.length} events, ${lanes.length} threads, ${t.otherData.cpu_ms_avg.toFixed(3)} ms CPU per frame`);
