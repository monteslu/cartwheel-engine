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
 * Load a cart on romdev and time it.
 * @param {{cart: string, gpu: 'gl'|'webgpu', frames: number, warmup?: number, window?: number, commands?: string[],
 *   passTimers?: boolean|string, debugFields?: string[], input?: string|object[], seed?: number|null, maxSeconds?: number,
 *   preferredSize?: {width: number, height: number}, firstCommand?: string}} o - firstCommand: a debug request answered
 *   in the load's first frame (`env NAME=value` for a switch the game reads from frame 1; needs romdev's firstCommand) - seed null: a load with no seed (the cart then sizes itself
 *   from preferredSize; a deterministic run keeps its default 1080 lines)
 * @returns {Promise<object>} the timing result, plus `loaded` (the load result) and `gpuSeen` (the adapter or GL renderer)
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
    return { ...t, loaded, gpuSeen: seen };
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
