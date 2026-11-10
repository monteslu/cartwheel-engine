// GPU timing of a cart through romdev (frame({op:'timing'}), romdev's AGENTS.md "Frame and GPU timing"): one load,
// the setup requests, a warmup and the measured frames, all in two calls. How a perf check measures a cart without a
// native run: on the server's pinned test GPU, GL (a GL_TIME_ELAPSED query around each frame) or WebGPU (timestamps
// around every pass, and the pipeline's own pass timers, since engine v0.45.0).
//
//   import { romdevTiming } from './lib/romdev-timing.mjs';
//   const t = await romdevTiming({ cart, gpu: 'webgpu', commands: ['quality low', 'pipeline'], warmup: 380, frames: 16,
//     window: 16, passTimers: true, debugFields: ['render.skipped'] });
//   t.gpuMs.mean, t.passTimers.at(-1).totalGpuMs, t.contention.noisy
//
// The server is 127.0.0.1:7331 (ROMDEV_URL overrides). Each call is a fresh session that ends with the run.
//
// A timing scale for a slower or faster test GPU: every budget was measured on one reference GPU, and a run on another
// GPU can be read against it through a fit over a known workload timed on both: measured ms = a + b x reference ms
// (a fast GPU has a fixed cost per frame that no preset cuts, so a plain ratio does not hold at every frame time).
// CW_GPU_TIMING_SCALE_GL and CW_GPU_TIMING_SCALE_WEBGPU hold the fit as JSON {a, b, validMs: [lo, hi], residual,
// unreliable} (validMs: the reference-GPU frame times the fit was checked over; residual: its largest relative error
// there), or a plain number / {factor}, a ratio alone (reference ms = measured ms x factor, no band). CW_GPU_TIMING_LANE
// names that GPU. When set, the result carries `timingScale` and `scaledMs` ((gpuMs - a) / b, the reference-GPU
// equivalent) when the scaled mean lies in the band, else `scaledMs: null` and a `scaleNote` saying so; fmtMs() prints
// both. Raw ms stay the measurement; the scaled number is a convenience, as good as the residual, so a gate keeps a
// margin beside it. Unset, nothing changes.
import { request as httpRequest } from 'node:http';
import { wgpuPin } from './wgpu-pin.mjs';

const BASE = new URL(process.env.ROMDEV_URL || 'http://127.0.0.1:7331');

function post(tool, args, session, limitMs) {
  const body = JSON.stringify(args);
  return new Promise((ok, fail) => {
    const req = httpRequest({ host: BASE.hostname, port: BASE.port || 80, path: `/tool/${tool}`, method: 'POST', agent: false,
      headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body), 'x-romdev-session': session,
        'x-romdev-agent': process.env.CW_ROMDEV_AGENT || 'cartwheel-romdev-timing' } }, (res) => {
      let text = '';
      res.setEncoding('utf8');
      res.on('data', (c) => { text += c; });
      res.on('end', () => {
        clearTimeout(timer);
        let b;
        try { b = JSON.parse(text); } catch { b = text; }
        if (res.statusCode < 200 || res.statusCode >= 300 || (b && b.isError)) fail(new Error(`romdev ${tool}: ${typeof b === 'string' ? b : JSON.stringify(b).slice(0, 2000)}`));
        else ok(b);
      });
    });
    // a timing run holds the server and other callers wait for it, so the wait is generous; it fails naming the call
    const timer = setTimeout(() => req.destroy(new Error(`romdev ${tool}: no reply in ${limitMs} ms`)), limitMs);
    req.on('error', (e) => { clearTimeout(timer); fail(new Error(`romdev at ${BASE.origin} ${tool}: ${e.message} (is the server running?)`)); });
    req.end(body);
  });
}

/**
 * The timing scale set for a backend, or null when none is set.
 * @param {'gl'|'webgpu'} gpu
 * @returns {{gpuName: string, backend: string, a: number, b: number, validMs: number[]|null, residual: number|null, unreliable: boolean}|null}
 */
export function timingScale(gpu) {
  const key = `CW_GPU_TIMING_SCALE_${gpu.toUpperCase()}`;
  const raw = process.env[key];
  if (raw === undefined || raw === '') return null;
  let s;
  try { s = JSON.parse(raw); } catch { throw new Error(`${key}: not a number or JSON: ${raw}`); }
  if (typeof s === 'number') s = { factor: s };
  if (s && s.factor > 0 && s.b === undefined) s = { ...s, a: 0, b: 1 / s.factor };
  if (!s || !(s.b > 0) || typeof s.a !== 'number') throw new Error(`${key}: no {a, b} fit or positive factor in ${raw}`);
  const band = Array.isArray(s.validMs) && s.validMs.length === 2 && s.validMs[0] < s.validMs[1] ? s.validMs.map(Number) : null;
  return { gpuName: process.env.CW_GPU_TIMING_LANE || 'unnamed', backend: gpu, a: s.a, b: s.b, validMs: band, residual: s.residual >= 0 ? s.residual : null, unreliable: !!s.unreliable };
}

/** one measured ms on the reference GPU: (ms - a) / b */
export const scaleMs = (ms, scale) => (ms - scale.a) / scale.b;
/** whether a scaled ms lies in the fit's band (always, without a band) */
export const inBand = (scaled, scale) => !scale.validMs || (scaled >= scale.validMs[0] && scaled <= scale.validMs[1]);

/** the stats of gpuMs on the reference GPU (each number through the fit), or null without a scale or with the mean
 *  outside the band (scaleNote(gpuMs.mean, scale) says why) */
export function scaleStats(gpuMs, scale) {
  if (!scale || !gpuMs || typeof gpuMs.mean !== 'number' || !inBand(scaleMs(gpuMs.mean, scale), scale)) return null;
  const out = {};
  for (const k of ['min', 'mean', 'p50', 'p95', 'p99', 'max']) if (typeof gpuMs[k] === 'number') out[k] = +scaleMs(gpuMs[k], scale).toFixed(3);
  return out;
}

/** why a measured ms has no scaled number ('' when it has one) */
export function scaleNote(ms, scale) {
  if (!scale || typeof ms !== 'number') return '';
  const v = scaleMs(ms, scale);
  if (inBand(v, scale)) return '';
  return `no scaled number: ${ms.toFixed(2)} ms on ${scale.gpuName} would be ${v.toFixed(2)} ms, outside the fit's band of ${scale.validMs[0]} to ${scale.validMs[1]} ms on the reference GPU`;
}

/**
 * One GPU time for a report: "4.21 ms", or with a scale "4.21 ms (10.5 ms scaled on <gpu>, residual 6%)", or the
 * raw number with the note when it lies outside the fit's band.
 * @param {number} ms the measured GPU ms
 * @param {object|null} scale timingScale(gpu), or a result's timingScale
 */
export function fmtMs(ms, scale) {
  const raw = typeof ms === 'number' ? ms.toFixed(2) + ' ms' : '? ms';
  if (!scale || typeof ms !== 'number') return raw;
  const note = scaleNote(ms, scale);
  if (note) return `${raw} (${note})`;
  const res = scale.residual === null ? 'residual unknown' : `residual ${(scale.residual * 100).toFixed(0)}%`;
  return `${raw} (${scaleMs(ms, scale).toFixed(2)} ms scaled on ${scale.gpuName}, ${res}${scale.unreliable ? ', UNRELIABLE' : ''})`;
}

/**
 * Load a cart on romdev and time it.
 * @param {{cart: string, gpu: 'gl'|'webgpu', frames: number, warmup?: number, window?: number, commands?: string[],
 *   passTimers?: boolean|string, debugFields?: string[], input?: string|object[], seed?: number|null, maxSeconds?: number,
 *   preferredSize?: {width: number, height: number}, firstCommand?: string}} o - firstCommand: a debug request answered
 *   in the load's first frame (`env NAME=value` for a switch the game reads from frame 1; needs romdev's firstCommand) - seed null: a load with no seed (the cart then sizes itself
 *   from preferredSize; a deterministic run keeps its default 1080 lines)
 * @returns {Promise<object>} the timing result, plus `loaded` (the load result), `gpuSeen` (the adapter or GL renderer) and,
 *   with a timing scale set for the backend, `timingScale` and `scaledMs` (gpuMs through the fit; null with a `scaleNote` outside its band)
 */
export async function romdevTiming(o) {
  const session = `cw-timing-${process.pid}-${Date.now().toString(36)}`;
  const pin = wgpuPin();
  const limit = +(process.env.CW_ROMDEV_TIMEOUT_MS || 900000);
  const loaded = await post('loadMedia', { platform: 'wasmcart', path: o.cart,
    ...(o.seed === null ? {} : { deterministicSeed: o.seed ?? 1, deterministicStepMs: 1000 / 60 }),
    ...(o.preferredSize ? { preferredSize: o.preferredSize } : {}),
    ...(o.firstCommand ? { firstCommand: o.firstCommand } : {}),
    gpuPower: pin.power, wgpu: o.gpu === 'webgpu', ...(o.gpu === 'webgpu' ? { gpuTiming: true } : {}) }, session, limit);
  try {
    if (o.firstCommand) {
      const fr = loaded.firstCommandReply;
      if (fr === undefined) throw new Error('romdev: firstCommand was not delivered (romdev too old for it)');
      if (fr && typeof fr === 'object' && typeof fr.error === 'string') throw new Error(`romdev: '${o.firstCommand}' answered: ${fr.error}`);
    }
    if (loaded.gpu !== o.gpu) throw new Error(`romdev: the cart runs on ${loaded.gpu}, not ${o.gpu}`);
    // the test GPU, checked before a measured frame: the adapter's device name, or the GL renderer string
    const seen = o.gpu === 'webgpu' ? loaded.gpuAdapter && (loaded.gpuAdapter.device || loaded.gpuAdapter.description) : loaded.glRenderer;
    pin.check('romdev', seen);
    if (o.gpu === 'webgpu' && !(loaded.gpuAdapter && loaded.gpuAdapter.timestampQuery))
      throw new Error('romdev: the WebGPU device has no timestamp-query (romdev too old for gpuTiming, or the adapter lacks it)');
    const t = await post('frame', { op: 'timing', frames: o.frames, warmup: o.warmup ?? 0, window: o.window ?? Math.min(120, o.frames),
      ...(o.commands && o.commands.length ? { commands: o.commands } : {}), ...(o.passTimers ? { passTimers: o.passTimers } : {}),
      ...(o.debugFields ? { debugFields: o.debugFields } : {}), ...(o.input ? { input: o.input } : {}),
      ...(o.maxSeconds ? { maxSeconds: o.maxSeconds } : {}) }, session, limit);
    if (t.gpu && t.gpu.onPinnedGpu === false) throw new Error(`romdev: GPU work went off the test GPU: ${JSON.stringify(t.gpu.workByDeviceMs)}`);
    // the timing scale for this backend, when the environment sets one: the raw gpuMs stay, scaledMs is the reference-GPU
    // equivalent (null, with scaleNote, when the frame time lies outside the fit's band)
    const scale = timingScale(o.gpu);
    const note = scale && t.gpuMs ? scaleNote(t.gpuMs.mean, scale) : '';
    return { ...t, loaded, gpuSeen: seen, ...(scale ? { timingScale: scale, scaledMs: scaleStats(t.gpuMs, scale), ...(note ? { scaleNote: note } : {}) } : {}) };
  } finally {
    await post('host', { op: 'shutdown' }, session, 60000).catch(() => {});
  }
}

/** a cart input script ([{frame, buttons}], 1-based engine frames, each held until the next) as romdev's schedule,
 *  in frames from the timing call's first frame (`at` engine frames already ran before it) */
export function scheduleFrom(script, at) {
  const out = [];
  script.forEach((e, i) => {
    if (e.axes && Object.values(e.axes).some((v) => v)) throw new Error('romdev timing input: buttons only (a script with stick axes runs natively)');
    const from = e.frame - 1 - at, to = i + 1 < script.length ? script[i + 1].frame - 1 - at : from + 1;
    const b = (e.buttons || []).map((n) => (n === 'back' ? 'select' : n));
    if (b.length && to > from) out.push({ frame: Math.max(0, from), buttons: b, hold: to - Math.max(0, from) });
  });
  return out;
}
