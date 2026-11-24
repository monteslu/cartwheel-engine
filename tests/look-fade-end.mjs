// The start and the end of a blend between looks cost no frame (src/render/looks.c, samples/style_switch): the first
// two frames of a blend (the pair set, a look drawn into a target of the frame's size and mixed) and the frame a blend
// ends in one look (the weight reaches 1, the look drawn alone straight into the pass's target, and for a blend across
// the stages the SCENE pass off) with the frame after it are held to the frames round them. The dual cart through
// romdev on GL and WebGPU, every frame timed (frame({op:'timing'}), series: wall, CPU and GPU per frame), RUNS runs and
// a frame's cost its least over them (the runs are deterministic, so frame i is the same work each time; another
// process only adds). Every look blend of the sample is judged, both rounds (the first showing and the warm one): the
// plain scene to ASCII, ASCII to half blocks on a palette, half blocks to a one-bit dither at the DISPLAY stage (a blend
// across the stages), the dither to the plain scene (a blend to no look).
//
// Bound: on the two frames of a start or an end, CPU at most LIMIT_CPU ms (backdrop-look-prewarm's budget for a change)
// and GPU at most LIMIT_GPU ms over the larger of the medians of the 5 frames before them and the 5 after.
// Reported per blend: the blend's GPU ms (median of its frames), the look alone before or after it and the plain scene,
// so what a blend costs over its two looks can be read off (a blend draws both looks, each into a target of the
// frame's size, and the mix: the two looks' costs plus one full screen pass a stage).
// CONTROL: CW_TEST_LOOK_END_REBUILD=1 with CW_TEST_NO_PREWARM=1 (the stage's programs made again on the frame a blend
// ends, and each look's first use in play) must put a start or an end over the bound on each backend.
//
//   node tests/look-fade-end.mjs [--gpu gl|webgpu]   (127.0.0.1:7331; ROMDEV_URL overrides)
import { request as httpRequest } from 'node:http';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { wgpuPin } from '../tools/lib/wgpu-pin.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const CART = process.env.SW_CART || join(ROOT, 'build/cart-wgpu/carts/style_switch.wasc');
const BASE = new URL(process.env.ROMDEV_URL || 'http://127.0.0.1:7331');
const FRAMES = 960, RUNS = 3, LIMIT_CPU = 3, LIMIT_GPU = 1.5, SPAN = 5;
const ROUND = 420, EVENTS = 7; // samples/style_switch: ticks between two rounds' changes, changes a round
const FIELDS = ['sw.tick', 'sw.event', 'look.ran'];
const LOOK_EVENTS = new Map([[1, 'plain to ASCII'], [2, 'ASCII to half blocks'], [3, 'half blocks to one bit (across the stages)'], [5, 'one bit to plain']]);
const arg = (n) => (process.argv.includes(n) ? process.argv[process.argv.indexOf(n) + 1] : undefined);
const gpus = arg('--gpu') ? [arg('--gpu')] : ['gl', 'webgpu'];

function post(tool, args, session) {
  const body = JSON.stringify(args);
  return new Promise((ok, fail) => {
    const req = httpRequest({ host: BASE.hostname, port: BASE.port || 80, path: `/tool/${tool}`, method: 'POST', agent: false,
      headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body), 'x-romdev-session': session,
        'x-romdev-agent': process.env.CW_ROMDEV_AGENT || 'cartwheel-look-fade-end' } }, (res) => {
      let text = '';
      res.setEncoding('utf8');
      res.on('data', (c) => { text += c; });
      res.on('end', () => {
        clearTimeout(timer);
        let b;
        try { b = JSON.parse(text); } catch { b = text; }
        if (b && Array.isArray(b.content)) {
          const t = b.content.find((c) => c.type === 'text');
          if (b.isError) return fail(new Error(`romdev ${tool}: ${t ? t.text : JSON.stringify(b)}`));
          try { b = JSON.parse(t.text); } catch { b = t ? t.text : b; }
        }
        if (res.statusCode < 200 || res.statusCode >= 300 || (b && b.isError)) fail(new Error(`romdev ${tool}: ${typeof b === 'string' ? b : JSON.stringify(b).slice(0, 1500)}`));
        else ok(b);
      });
    });
    const timer = setTimeout(() => req.destroy(new Error(`romdev ${tool}: no reply in 900 s`)), 900000);
    req.on('error', (e) => { clearTimeout(timer); fail(new Error(`romdev at ${BASE.origin} ${tool}: ${e.message} (is the server running?)`)); });
    req.end(body);
  });
}

/** one deterministic load, every frame timed: { cpu, gpu, tick, event, ran } arrays */
async function run(gpu, label, env) {
  const session = `cw-look-fade-end-${process.pid}-${label}-${gpu}`;
  const pin = wgpuPin();
  const first = Object.keys(env).length ? `env ${Object.entries(env).map(([k, v]) => `${k}=${v}`).join(' ')}` : undefined;
  const loaded = await post('loadMedia', { platform: 'wasmcart', path: CART, deterministicSeed: 1, deterministicStepMs: 1000 / 60,
    ...(first ? { firstCommand: first } : {}), gpuPower: pin.power, wgpu: gpu === 'webgpu', ...(gpu === 'webgpu' ? { gpuTiming: true } : {}) }, session);
  try {
    if (first && loaded.firstCommandReply === undefined) throw new Error('romdev: firstCommand was not delivered (romdev too old for it)');
    if (loaded.gpu !== gpu) throw new Error(`romdev: the cart runs on ${loaded.gpu}, not ${gpu}`);
    pin.check('romdev', gpu === 'webgpu' ? loaded.gpuAdapter && (loaded.gpuAdapter.device || loaded.gpuAdapter.description) : loaded.glRenderer);
    const t = await post('frame', { op: 'timing', frames: FRAMES, warmup: 0, series: true, debugFields: FIELDS, maxSeconds: 900 }, session);
    if (t.gpu && t.gpu.onPinnedGpu === false) throw new Error(`romdev: GPU work went off the test GPU: ${JSON.stringify(t.gpu.workByDeviceMs)}`);
    const s = t.series;
    if (!s || !s.cpuMs || !s.gpuMs || !s.fields) throw new Error(`romdev: no timing series: ${JSON.stringify(t).slice(0, 400)}`);
    if (s.gpuMs.every((x) => x == null)) throw new Error('romdev: no GPU times (a host without GPU timers)');
    return { cpu: s.cpuMs, gpu: s.gpuMs, tick: s.fields['sw.tick'], event: s.fields['sw.event'], ran: s.fields['look.ran'], noisy: !!(t.contention && t.contention.noisy) };
  } finally {
    await post('host', { op: 'shutdown' }, session).catch(() => {});
  }
}

const median = (a) => { const s = a.filter(Number.isFinite).sort((x, y) => x - y); return s.length ? s[s.length >> 1] : NaN; };
const f2 = (x) => (Number.isFinite(x) ? x.toFixed(2) : String(x));

/** every look blend's end in the runs (a frame's cost its least over them) */
function ends(runs) {
  const n = Math.min(...runs.map((r) => r.cpu.length));
  const minOf = (k) => Array.from({ length: n }, (_, i) => Math.min(...runs.map((r) => r[k][i] ?? Infinity)));
  const cpu = minOf('cpu'), gpu = minOf('gpu'), { tick, event, ran } = runs[0];
  /* (samples/style_switch: a blend runs over ticks 0 to 18 of its event, k 19 sets the look alone) */
  const kOf = (i) => { const t = tick[i] - (tick[i] >= ROUND + 60 ? ROUND : 0); return t >= 60 ? (t - 60) % 60 : -1; };
  // the plain scene: the frames before the first look blend (event 1) once the fire is in
  const plainIdx = [];
  for (let i = 0; i < n; i++) if (event[i] === 0 && kOf(i) >= 30) plainIdx.push(i);
  const plain = median(plainIdx.slice(-20).map((i) => gpu[i]));
  const out = [];
  for (let i = 1; i < n - SPAN - 2; i++) {
    const e = event[i] % EVENTS;
    const edge = kOf(i) === 0 && kOf(i - 1) !== 0 ? 'start' : kOf(i) === 19 && kOf(i - 1) !== 19 ? 'end' : null;
    if (!LOOK_EVENTS.has(e) || !edge) continue;
    if (edge === 'start') {
      /* the blend's first two frames against the 5 before it (the look or the plain scene alone) and the 5 after */
      const before = Array.from({ length: SPAN }, (_, k) => i - 1 - k), after = Array.from({ length: SPAN }, (_, k) => i + 2 + k);
      if (!after.every((k) => ran[k] === 7)) throw new Error(`event ${event[i]}: the blend's frames did not blend`);
      const steadyCpu = Math.max(median(before.map((k) => cpu[k])), median(after.map((k) => cpu[k])));
      const steadyGpu = Math.max(median(before.map((k) => gpu[k])), median(after.map((k) => gpu[k])));
      const endCpu = Math.max(cpu[i], cpu[i + 1]), endGpu = Math.max(gpu[i], gpu[i + 1]);
      out.push({ edge, event: event[i], name: LOOK_EVENTS.get(e), round: event[i] >= EVENTS ? 2 : 1, endCpu, endGpu, steadyCpu, steadyGpu,
        overCpu: endCpu - steadyCpu, overGpu: endGpu - steadyGpu, blendGpu: median(after.map((k) => gpu[k])), aloneGpu: median(before.map((k) => gpu[k])), plain });
      continue;
    }
    const blendIdx = [];
    for (let k = i - 1; k > 0 && kOf(k) >= 0 && kOf(k) < 19 && event[k] === event[i]; k--) blendIdx.push(k);
    if (!blendIdx.every((k) => ran[k] === 7)) throw new Error(`event ${event[i]}: a frame of the blend did not blend (look.ran ${blendIdx.map((k) => ran[k]).join(',')})`);
    const before = Array.from({ length: SPAN }, (_, k) => i - 1 - k), after = Array.from({ length: SPAN }, (_, k) => i + 2 + k);
    const steadyCpu = Math.max(median(before.map((k) => cpu[k])), median(after.map((k) => cpu[k])));
    const steadyGpu = Math.max(median(before.map((k) => gpu[k])), median(after.map((k) => gpu[k])));
    const endCpu = Math.max(cpu[i], cpu[i + 1]), endGpu = Math.max(gpu[i], gpu[i + 1]);
    out.push({ edge, event: event[i], name: LOOK_EVENTS.get(e), round: event[i] >= EVENTS ? 2 : 1, endCpu, endGpu, steadyCpu, steadyGpu,
      overCpu: endCpu - steadyCpu, overGpu: endGpu - steadyGpu, blendGpu: median(blendIdx.map((k) => gpu[k])),
      aloneGpu: median(after.map((k) => gpu[k])), plain });
  }
  return out;
}

let failed = 0;
const ok = (s) => console.log(`ok   ${s}`);
const fail = (s) => { console.log(`FAIL ${s}`); failed++; };

for (const gpu of gpus) {
  const warm = [], ctl = [];
  for (let r = 0; r < RUNS; r++) warm.push(await run(gpu, `warm${r}`, {}));
  for (let r = 0; r < RUNS; r++) ctl.push(await run(gpu, `control${r}`, { CW_TEST_LOOK_END_REBUILD: 1, CW_TEST_NO_PREWARM: 1 }));
  const noisy = [...warm, ...ctl].some((x) => x.noisy);
  const e = ends(warm), c = ends(ctl);
  if (e.length < 16) { fail(`look fade end (${gpu}): only ${e.length} look blend starts and ends in the run (want 16): the check proves nothing`); continue; }
  for (const x of e)
    console.log(`     ${gpu} ${x.name} (round ${x.round}) ${x.edge}: its frames CPU ${f2(x.endCpu)} ms (steady ${f2(x.steadyCpu)}), GPU ${f2(x.endGpu)} (steady ${f2(x.steadyGpu)}); ` +
      `blend GPU ${f2(x.blendGpu)}, the look alone ${x.edge === 'end' ? 'after' : 'before'} it ${f2(x.aloneGpu)}, the plain scene ${f2(x.plain)}`);
  const bad = e.filter((x) => x.overCpu > LIMIT_CPU || x.overGpu > LIMIT_GPU);
  const worstCpu = Math.max(...e.map((x) => x.overCpu)), worstGpu = Math.max(...e.map((x) => x.overGpu));
  if (bad.length) fail(`look fade end (${gpu}): ${bad.length} of ${e.length} blend starts and ends over the bound: ${bad.map((x) => `${x.name} round ${x.round} ${x.edge} (CPU +${f2(x.overCpu)}, GPU +${f2(x.overGpu)})`).join(', ')}${noisy ? ' (another process used the GPU during the runs)' : ''}`);
  else ok(`look fade end (${gpu}): ${e.length} blend starts and ends, the worst of their frames CPU +${f2(worstCpu)} ms and GPU +${f2(worstGpu)} ms over the frames round it (bounds ${LIMIT_CPU} and ${LIMIT_GPU})`);
  const cbad = c.filter((x) => x.overCpu > LIMIT_CPU || x.overGpu > LIMIT_GPU);
  const cworst = c.reduce((m, x) => (x.overCpu > m.overCpu ? x : m), c[0] || { overCpu: NaN });
  if (cbad.length) ok(`look fade end control (${gpu}): programs made again at each blend's end and no prewarm put ${cbad.length} of ${c.length} starts and ends over the bound (worst CPU +${f2(cworst.overCpu)} ms, ${cworst.name} round ${cworst.round} ${cworst.edge}), as it must`);
  else fail(`look fade end control (${gpu}): programs made again at each blend's end and no prewarm stayed inside the bound (worst CPU +${f2(cworst.overCpu)} ms): the check cannot see an end-of-blend hitch`);
}
process.exit(failed ? 1 : 0);
