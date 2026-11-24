// The end of a style change into a world that lights the scene costs no frame (src/render/backdrop.c, src/render/looks.c,
// samples/look_env_end): the frame a cross-fade's last world goes out and the incoming Backdrop's environment turns on
// (the environment-return frame: the stage at its weight just under 1, the half blocks on a palette at SCENE and the
// one-bit dither at DISPLAY still blended) and the frame after it (the weight 1, the dither alone) are held to the
// frames round them. The dual cart through romdev on GL and WebGPU, every frame timed (frame({op:'timing'}), series:
// CPU and GPU per frame), RUNS runs and a frame's cost its least over them (the runs are deterministic, so frame i is
// the same work each time; another process only adds). Both rounds of the sample are judged: the first (the stage's
// kept filter from the warm-up's prewarm) and the second (its kept filter from its last capture of the first round).
//
// Bound: on each of the two frames, CPU at most LIMIT_CPU ms and GPU at most LIMIT_GPU ms over the larger of the medians
// of the SPAN frames before them (the fade, two worlds and two looks) and the SPAN after (the stage held, the dither).
// CONTROL: CW_TEST_ENV_OWN_TINY=1 CW_TEST_ENV_NO_KEEP=1 CW_TEST_LOOK_WARM_ONE_TONE=1 LE_RES=50 (the engine before: the
// scene's own environment brought in at a share of 0.0002 and out again the frame after, a filter each, nothing kept;
// and the dither's other tone path left to its first use) must put a return over the bound on each backend. The normal
// runs set LE_RES=50 too, so both draw the same pictures.
//
//   node tests/look-env-end-timing.mjs [--gpu gl|webgpu] [--dump]   (127.0.0.1:7331; ROMDEV_URL overrides)
import { request as httpRequest } from 'node:http';
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { wgpuPin } from '../tools/lib/wgpu-pin.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..'), OUT = join(ROOT, 'out/gate/look-env-end-timing');
const CART = process.env.LE_CART || join(ROOT, 'build/cart-wgpu/carts/look_env_end.wasc');
const BASE = new URL(process.env.ROMDEV_URL || 'http://127.0.0.1:7331');
const FRAMES = 440, RUNS = 3, LIMIT_CPU = 1, LIMIT_GPU = 1, SPAN = 5;
const FIELDS = ['le.tick', 'le.round', 'le.env', 'le.phase'];
const BASE_ENV = { LE_RES: '50' };
const CONTROL_ENV = { ...BASE_ENV, CW_TEST_ENV_OWN_TINY: '1', CW_TEST_ENV_NO_KEEP: '1', CW_TEST_LOOK_WARM_ONE_TONE: '1' };
const arg = (n) => (process.argv.includes(n) ? process.argv[process.argv.indexOf(n) + 1] : undefined);
const gpus = arg('--gpu') ? [arg('--gpu')] : ['gl', 'webgpu'];
const dump = process.argv.includes('--dump');
mkdirSync(OUT, { recursive: true });

function post(tool, args, session) {
  const body = JSON.stringify(args);
  return new Promise((ok, fail) => {
    const req = httpRequest({ host: BASE.hostname, port: BASE.port || 80, path: `/tool/${tool}`, method: 'POST', agent: false,
      headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body), 'x-romdev-session': session,
        'x-romdev-agent': process.env.CW_ROMDEV_AGENT || 'cartwheel-look-env-end-timing' } }, (res) => {
      let text = '';
      res.setEncoding('utf8');
      res.on('data', (c) => { text += c; });
      res.on('end', () => {
        clearTimeout(timer);
        let b;
        try { b = JSON.parse(text); } catch { b = text; }
        if (res.statusCode < 200 || res.statusCode >= 300 || (b && b.isError)) fail(new Error(`romdev ${tool}: ${typeof b === 'string' ? b : JSON.stringify(b).slice(0, 1500)}`));
        else ok(b);
      });
    });
    const timer = setTimeout(() => req.destroy(new Error(`romdev ${tool}: no reply in 900 s`)), 900000);
    req.on('error', (e) => { clearTimeout(timer); fail(new Error(`romdev at ${BASE.origin} ${tool}: ${e.message} (is the server running?)`)); });
    req.end(body);
  });
}

/** one deterministic load, every frame timed: { cpu, gpu, tick, round, env, phase } arrays */
async function run(gpu, label, env) {
  const session = `cw-look-env-end-${process.pid}-${label}-${gpu}`;
  const pin = wgpuPin();
  const first = `env ${Object.entries(env).map(([k, v]) => `${k}=${v}`).join(' ')}`;
  const loaded = await post('loadMedia', { platform: 'wasmcart', path: CART, deterministicSeed: 1, deterministicStepMs: 1000 / 60, firstCommand: first,
    gpuPower: pin.power, wgpu: gpu === 'webgpu', ...(gpu === 'webgpu' ? { gpuTiming: true } : {}) }, session);
  try {
    if (loaded.firstCommandReply === undefined) throw new Error('romdev: firstCommand was not delivered (romdev too old for it)');
    if (loaded.gpu !== gpu) throw new Error(`romdev: the cart runs on ${loaded.gpu}, not ${gpu}`);
    pin.check('romdev', gpu === 'webgpu' ? loaded.gpuAdapter && (loaded.gpuAdapter.device || loaded.gpuAdapter.description) : loaded.glRenderer);
    const t = await post('frame', { op: 'timing', frames: FRAMES, warmup: 0, series: true, debugFields: FIELDS, maxSeconds: 900 }, session);
    if (t.gpu && t.gpu.onPinnedGpu === false) throw new Error(`romdev: GPU work went off the test GPU: ${JSON.stringify(t.gpu.workByDeviceMs)}`);
    const s = t.series;
    if (!s || !s.cpuMs || !s.gpuMs || !s.fields) throw new Error(`romdev: no timing series: ${JSON.stringify(t).slice(0, 400)}`);
    if (s.gpuMs.every((x) => x == null)) throw new Error('romdev: no GPU times (a host without GPU timers)');
    return { cpu: s.cpuMs, gpu: s.gpuMs, tick: s.fields['le.tick'], round: s.fields['le.round'], env: s.fields['le.env'], phase: s.fields['le.phase'],
      noisy: !!(t.contention && t.contention.noisy) };
  } finally {
    await post('host', { op: 'shutdown' }, session).catch(() => {});
  }
}

const median = (a) => { const s = a.filter(Number.isFinite).sort((x, y) => x - y); return s.length ? s[s.length >> 1] : NaN; };
const f2 = (x) => (Number.isFinite(x) ? x.toFixed(2) : String(x));

/** each round's environment-return frame and the frame after it, against the frames round them */
function returns(runs, label, gpu) {
  const n = Math.min(...runs.map((r) => r.cpu.length));
  const minOf = (k) => Array.from({ length: n }, (_, i) => Math.min(...runs.map((r) => r[k][i] ?? Infinity)));
  const cpu = minOf('cpu'), gpuMs = minOf('gpu'), { env, phase, round } = runs[0];
  if (dump) writeFileSync(join(OUT, `${label}.${gpu}.json`), JSON.stringify({ cpu, gpu: gpuMs, env, phase, round, tick: runs[0].tick }));
  const out = [];
  for (let i = 1; i < n - SPAN - 2; i++) {
    if (!(phase[i] === 1 && env[i] === 1 && env[i - 1] === 0)) continue; // the fade's frame that turned the environment on
    const before = Array.from({ length: SPAN }, (_, k) => i - 1 - k), after = Array.from({ length: SPAN }, (_, k) => i + 2 + k);
    const steadyCpu = Math.max(median(before.map((k) => cpu[k])), median(after.map((k) => cpu[k])));
    const steadyGpu = Math.max(median(before.map((k) => gpuMs[k])), median(after.map((k) => gpuMs[k])));
    const endCpu = Math.max(cpu[i], cpu[i + 1]), endGpu = Math.max(gpuMs[i], gpuMs[i + 1]);
    out.push({ round: round[i], frame: i, cpu: [cpu[i], cpu[i + 1]], gpu: [gpuMs[i], gpuMs[i + 1]], steadyCpu, steadyGpu,
      overCpu: endCpu - steadyCpu, overGpu: endGpu - steadyGpu });
  }
  return out;
}

let failed = 0;
const ok = (s) => console.log(`ok   ${s}`);
const fail = (s) => { console.log(`FAIL ${s}`); failed++; };
const line = (gpu, tag, x) => console.log(`     ${gpu} ${tag} round ${x.round} (frame ${x.frame}): the return CPU ${f2(x.cpu[0])} ms and the next ${f2(x.cpu[1])} ` +
  `(steady ${f2(x.steadyCpu)}), GPU ${f2(x.gpu[0])} and ${f2(x.gpu[1])} (steady ${f2(x.steadyGpu)})`);

for (const gpu of gpus) {
  const now = [], ctl = [];
  for (let r = 0; r < RUNS; r++) now.push(await run(gpu, `now${r}`, BASE_ENV));
  for (let r = 0; r < RUNS; r++) ctl.push(await run(gpu, `control${r}`, CONTROL_ENV));
  const noisy = [...now, ...ctl].some((x) => x.noisy);
  const e = returns(now, 'now', gpu), c = returns(ctl, 'control', gpu);
  if (e.length !== 2 || c.length !== 2) { fail(`look env end (${gpu}): ${e.length} and ${c.length} environment returns found (want 2 each): the check proves nothing`); continue; }
  for (const x of e) line(gpu, 'now', x);
  for (const x of c) line(gpu, 'control', x);
  const bad = e.filter((x) => x.overCpu > LIMIT_CPU || x.overGpu > LIMIT_GPU);
  const worstCpu = Math.max(...e.map((x) => x.overCpu)), worstGpu = Math.max(...e.map((x) => x.overGpu));
  if (bad.length) fail(`look env end (${gpu}): ${bad.map((x) => `round ${x.round} (CPU +${f2(x.overCpu)}, GPU +${f2(x.overGpu)})`).join(', ')} over the bound${noisy ? ' (another process used the GPU during the runs)' : ''}`);
  else ok(`look env end (${gpu}): both returns, the worst of their frames CPU +${f2(worstCpu)} ms and GPU +${f2(worstGpu)} ms over the frames round them (bounds ${LIMIT_CPU} and ${LIMIT_GPU})`);
  const cbad = c.filter((x) => x.overCpu > LIMIT_CPU || x.overGpu > LIMIT_GPU);
  const cw = c.reduce((m, x) => (Math.max(x.overCpu, x.overGpu) > Math.max(m.overCpu, m.overGpu) ? x : m), c[0]);
  if (cbad.length) ok(`look env end control (${gpu}): the engine before put ${cbad.length} of 2 returns over the bound (worst CPU +${f2(cw.overCpu)} ms, GPU +${f2(cw.overGpu)} ms), as it must`);
  else fail(`look env end control (${gpu}): the engine before stayed inside the bound (worst CPU +${f2(cw.overCpu)} ms, GPU +${f2(cw.overGpu)} ms): the check cannot see the end's cost`);
}
process.exit(failed ? 1 : 0);
