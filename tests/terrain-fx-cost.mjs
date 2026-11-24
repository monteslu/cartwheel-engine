// A terrain that uses none of Terrain3D's near-ground options draws with the program it always had, and costs what
// it did (cartwheel/terrain.h; samples/terrain_detail). The case: a control map with a random uv rotation on every
// texel (a game breaking a layer's repeat that way) and side_detile 1, uv_blend off.
//   1. program, per host and backend: the near terrain has no terrainfx part (td.fx 0). CONTROL:
//      CW_TEST_TERRAIN_FX=1 (the fx program forced) must have one; uv_blend on over a map with a rotation per 4 m block
//      has one (the option used); uv_blend on over a map with one rotation has none (nothing to blend).
//   2. GPU time, natively on a real GPU (skipped on software rendering): the pipeline's GPU ms at 1080p, interleaved
//      rounds, the least of the rounds. The jumbled map within the noise of the same field with one rotation (the
//      same program: what any terrain without the options cost before them). CONTROL: the jumbled map with the fx
//      program forced (what it cost from v0.71.0 to v0.77.1) must be over the noise.
//   node tests/terrain-fx-cost.mjs [--hosts native,romdev] [--gpu gl,webgpu] [--rounds 3] [--frames 240]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'terrain-fx-cost');
mkdirSync(OUT, { recursive: true });
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const SOFT = process.env.CW_TEST_GPU === 'software';
const HOSTS = arg('--hosts', SOFT ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
const ROUNDS = +arg('--rounds', 3), FRAMES = arg('--frames', '240');
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

function run(host, gpu, tag, env, frames, timers) {
  const state = join(OUT, `${tag}.${host}.${gpu}.json`);
  const a = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'terrain_detail', '--frames', String(frames), '--gpu', gpu,
    '--env', JSON.stringify(env), '--state', state];
  const r = spawnSync(process.execPath, a, { encoding: 'utf8', env: { ...process.env, ...(timers ? { CW_GPU_TIMERS: '1' } : {}) }, maxBuffer: 1 << 26 });
  if (r.status !== 0) throw new Error(`${host} ${gpu} ${tag}: ${((r.stderr || '') + (r.stdout || '')).trim().split('\n').slice(-3).join(' | ')}`);
  const st = JSON.parse(readFileSync(state, 'utf8'));
  return st.state || st;
}

const JUMBLE = { TD_PAINT: 'jumble', TD_UV_BLEND: '0', TD_DETILE: '1' };
const PLAIN = { TD_PAINT: 'plain', TD_UV_BLEND: '0', TD_DETILE: '1' };
const FORCED = { ...JUMBLE, CW_TEST_TERRAIN_FX: '1' };
let fails = 0;
const check = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails++; };

// ── 1. the program ──
const PROGRAM = [
  ['jumbled rotation, no options', JUMBLE, 0],
  ['CONTROL: the fx program forced', FORCED, 1],
  // (a rotation per 4 m block; blended, it takes minutes a frame on software WebGPU, so there GL only: which program
  // draws is decided on the CPU, the same for both backends)
  ['rotation per block, uv_blend on', { ...PLAIN, TD_PAINT: 'rot', TD_UV_BLEND: '1' }, 1, SOFT ? ['gl'] : null],
  ['one rotation, uv_blend on', { ...PLAIN, TD_UV_BLEND: '1' }, 0],
];
for (const host of HOSTS)
  for (const gpu of GPUS)
    PROGRAM.forEach(([label, env, want, only], i) => {
      if (only && !only.includes(gpu)) return;
      const s = run(host, gpu, `p${i}`, env, 12, false);
      check(s['td.applied'] === 1 && s['td.fx'] === want, `${host} ${gpu} ${label}: td.fx ${s['td.fx']} (want ${want})`);
    });

// ── 2. the GPU time ──
if (SOFT || !HOSTS.includes('native')) console.log('GPU time: skipped (software rendering, or no native host)');
else
  for (const gpu of GPUS) {
    const V = { plain: [], jumble: [], forced: [] };
    for (let r = 0; r < ROUNDS; r++)
      for (const [k, env] of [['plain', PLAIN], ['jumble', JUMBLE], ['forced', FORCED]]) {
        const s = run('native', gpu, `t${r}.${k}`, env, FRAMES, true);
        if (!(s['td.gpu_ms'] > 0)) { check(false, `${gpu} ${k}: no GPU time`); continue; }
        V[k].push(s['td.gpu_ms']);
      }
    if (V.plain.length < ROUNDS || V.jumble.length < ROUNDS || V.forced.length < ROUNDS) continue;
    const mn = (a) => Math.min(...a), spread = (a) => Math.max(...a) - Math.min(...a);
    // the noise: the rounds' spread of the two plain-program cases, at least 3% of the frame or 0.05 ms
    const noise = Math.max(spread(V.plain), spread(V.jumble), 0.03 * mn(V.plain), 0.05);
    const d = mn(V.jumble) - mn(V.plain), dc = mn(V.forced) - mn(V.plain);
    const f = (a) => a.map((x) => x.toFixed(3)).join(' ');
    console.log(`     ${gpu} GPU ms (rounds) one rotation ${f(V.plain)} | jumbled ${f(V.jumble)} | fx forced ${f(V.forced)}; noise ${noise.toFixed(3)}`);
    check(Math.abs(d) <= noise, `${gpu} jumbled rotation without options costs ${d.toFixed(3)} ms over one rotation (within ${noise.toFixed(3)})`);
    check(dc > noise, `${gpu} CONTROL: the fx program forced costs ${dc.toFixed(3)} ms more (over ${noise.toFixed(3)})`);
  }
console.log(fails ? `FAIL terrain-fx-cost (${fails})` : 'ok   terrain-fx-cost');
process.exit(fails ? 1 : 0);
