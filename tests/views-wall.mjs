// Split screen in one pass is never slower in wall time than the views drawn on their own, on a host that waits for
// each frame (romdev's wasmcart host: one frame, then the next): the split_views RELEASE cart at four views on the
// server's test GPU, frame({op:'timing'}) with the two interleaved over rounds, GL and WebGPU. The bound is the median
// of the rounds' mean wall time: one pass <= per view x 1.05. On WebGPU each view of the one pass goes to the GPU as it
// is recorded (pipeline views_submit 1, the default); the other order (views_submit 0) is measured and printed.
// Control: `pipeline_test views_stall 0.15` (the one pass spends 0.15 ms of CPU a view before its scene, 0.6 ms a frame) must fail the bound.
//   node tests/views-wall.mjs [--gpu gl,webgpu] [--cart build/cart-wgpu/carts/split_views.wasc]
import { request as httpRequest } from 'node:http';
import { existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { wgpuPin } from '../tools/lib/wgpu-pin.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : d);
const GPUS = opt('--gpu', 'gl,webgpu').split(',');
const CART = resolve(opt('--cart', join(ROOT, 'build/cart-wgpu/carts/split_views.wasc')));
const BOUND = 1.05;
const BASE = new URL(process.env.ROMDEV_URL || 'http://127.0.0.1:7331');
if (!existsSync(CART)) throw new Error(`views-wall: no cart at ${CART} (TARGETS=cart-wgpu scripts/build.sh split_views)`);

const post = (tool, args, session) => new Promise((ok, fail) => {
  const body = JSON.stringify(args);
  const req = httpRequest({ host: BASE.hostname, port: BASE.port || 80, path: `/tool/${tool}`, method: 'POST', agent: false,
    headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body), 'x-romdev-session': session,
      'x-romdev-agent': 'cartwheel-views-wall' } }, (res) => {
    let t = '';
    res.setEncoding('utf8');
    res.on('data', (c) => { t += c; });
    res.on('end', () => {
      let b;
      try { b = JSON.parse(t); } catch { b = t; }
      if (res.statusCode >= 300 || (b && b.isError)) fail(new Error(`romdev ${tool}: ${JSON.stringify(b).slice(0, 1500)}`));
      else ok(b);
    });
  });
  req.on('error', (e) => fail(new Error(`romdev at ${BASE.origin} ${tool}: ${e.message} (is the server running?)`)));
  req.end(body);
});
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s.length % 2 ? s[(s.length - 1) / 2] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2; };
const roundWall = (v) => median(v.rounds.map((r) => (typeof r.wallMs === 'number' ? r.wallMs : r.wallMs.mean)));

let fail = 0;
const check = (what, ok, detail) => {
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}: ${detail}`);
};
const pin = wgpuPin();
for (const gpu of GPUS) {
  const session = `views-wall-${process.pid}-${gpu}`;
  const loaded = await post('loadMedia', { platform: 'wasmcart', path: CART, firstCommand: 'env SV_MODE=v4', gpuPower: pin.power,
    wgpu: gpu === 'webgpu', ...(gpu === 'webgpu' ? { gpuTiming: true } : {}) }, session);
  try {
    if (loaded.gpu !== gpu) throw new Error(`the cart runs on ${loaded.gpu}, not ${gpu}`);
    const variants = [
      { name: 'per view', commands: ['pipeline views_array 0', 'pipeline_test views_stall 0'] },
      { name: 'one pass', commands: ['pipeline views_array 1', 'pipeline views_submit 1', 'pipeline_test views_stall 0'] },
      ...(gpu === 'webgpu' ? [{ name: 'one pass, submitted together', commands: ['pipeline views_array 1', 'pipeline views_submit 0', 'pipeline_test views_stall 0'] }] : []),
      { name: 'control: one pass + 0.15 ms a view', commands: ['pipeline views_array 1', 'pipeline views_submit 1', 'pipeline_test views_stall 0.15'] },
    ];
    const t = await post('frame', { op: 'timing', frames: 180, warmup: 120, window: 60, rounds: 5, variants, maxSeconds: 600 }, session);
    const by = Object.fromEntries(t.variants.map((v) => [v.name, v]));
    const clock = t.gpu && t.gpu.clockMhz ? `${Math.round(t.gpu.clockMhz.mean)} MHz` : '?';
    console.log(`-- ${gpu} (${t.gpu?.adapter?.device || t.gpu?.glRenderer || '?'}, GPU clock ${clock}, contention ${t.contention?.noisy ? 'NOISY' : 'quiet'})`);
    for (const v of t.variants) console.log(`     ${v.name.padEnd(34)} wall ${roundWall(v).toFixed(2)} ms (rounds ${v.rounds.map((r) => (typeof r.wallMs === 'number' ? r.wallMs : r.wallMs.mean).toFixed(2)).join('/')}), cpu ${v.cpuMs.mean.toFixed(2)}, gpu ${v.gpuMs ? v.gpuMs.mean.toFixed(2) : '-'}`);
    const pv = roundWall(by['per view']), op = roundWall(by['one pass']), ctl = roundWall(by['control: one pass + 0.15 ms a view']);
    check(`${gpu}: one pass no slower than per view (<= x${BOUND})`, op <= pv * BOUND, `${op.toFixed(2)} vs ${pv.toFixed(2)} ms`);
    check(`${gpu}: control (0.15 ms a view more) must fail the bound`, ctl > pv * BOUND, `${ctl.toFixed(2)} vs ${pv.toFixed(2)} ms`);
  } finally {
    await post('host', { op: 'shutdown' }, session).catch(() => {});
  }
}
console.log(fail ? `views-wall: ${fail} check(s) failed` : 'views-wall: ok');
process.exit(fail ? 1 : 0);
