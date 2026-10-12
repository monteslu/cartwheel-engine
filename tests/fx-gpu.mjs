// WebGPU effects checks that are not frame/state comparisons:
//
//   1. a broken compute shader (fx_gpu with CW_FXGPU_FAULT=1) must fail the
//      run: the WGSL error reaches cw.gpu.errors and run-cart refuses the run;
//      the same app without the fault must pass (the check can tell them apart)
//   2. perf: fx_gpu's perf scene (fx_perf's 100 000 particles: depth
//      collision, curl noise, a 2^15-key sort every frame, an attractor; no
//      post) natively on GL (transform feedback, fragment-pass sort) and on
//      WebGPU (compute), BOTH on the test GPU (GL gets DRI_PRIME = the PCI
//      address WebGPU runs are pinned to; by default native GL lands on the
//      desktop's discrete GPU, which would compare two different GPUs).
//      Interleaved A/B rounds, frame work (render start to GPU done) medians,
//      load average before and after. Checks: 100 000 alive on both; WebGPU's
//      median frame work under two frames (33.3 ms: the box is shared, the
//      number is reported, the bound only catches a broken path); control: the
//      same check against an impossible 0.05 ms must fail.
//
//   node tests/fx-gpu.mjs [--rounds n]
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { loadavg } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { wgpuPin } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'perf');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const ROUNDS = argv.includes('--rounds') ? +argv[argv.indexOf('--rounds') + 1] : 3;
const FRAMES = 300, COUNT = 100000;

function run(gpu, frames, env, tag) {
  return new Promise((res) => {
    const state = join(OUT, `fxgpu.${tag}.json`);
    const p = spawn('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'fx_gpu', '--frames', String(frames),
      '--state', state, '--shot', join(OUT, `fxgpu.${tag}.png`)], { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...env } });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => {
      if (code !== 0) return res({ code, err: err.trim().split('\n').slice(-2).join(' | ') });
      res({ code, state: JSON.parse(readFileSync(state, 'utf8')) });
    });
  });
}

const problems = [];
// 1. the broken shader must fail, the healthy run must pass
const broken = await run('webgpu', 20, { CW_FXGPU_FAULT: '1' }, 'broken');
if (broken.code === 0) problems.push('a broken compute shader did not fail the run (cw.gpu.errors stayed 0)');
else if (!/WebGPU error/.test(broken.err)) problems.push(`the broken shader run failed for another reason: ${broken.err}`);
const healthy = await run('webgpu', 20, {}, 'healthy');
if (healthy.code !== 0) problems.push(`the healthy run failed: ${healthy.err}`);

// 2. perf
const pci = wgpuPin().pci || readFileSync(join(process.env.HOME, '.config', 'cartwheel', 'chromium-gpu'), 'utf8').trim();
const dri = 'pci-' + pci.replace(/[:.]/g, '_');
const times = { gl: [], webgpu: [] };
const load0 = loadavg();
for (let r = 0; r < ROUNDS; r++)
  for (const gpu of r % 2 ? ['webgpu', 'gl'] : ['gl', 'webgpu']) {
    const x = await run(gpu, FRAMES, { CW_FXGPU_PERF: '1', ...(gpu === 'gl' ? { DRI_PRIME: dri } : {}) }, `perf-${gpu}`);
    if (x.code !== 0) { problems.push(`perf ${gpu}: ${x.err}`); continue; }
    if (x.state['fxg.perf_alive'] !== COUNT) problems.push(`perf ${gpu}: ${x.state['fxg.perf_alive']} alive, expected ${COUNT}`);
    times[gpu].push(x.state['host.fxg.frame_ms']);
  }
const load1 = loadavg();
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[s.length >> 1] : NaN; };
const mg = median(times.gl), mw = median(times.webgpu);
const within = (ms, budget) => ms > 0 && ms < budget;
if (!within(mw, 1000 / 30)) problems.push(`WebGPU 100k frame work ${mw.toFixed(2)} ms is over two frames`);
if (within(mw, 0.05)) problems.push('CONTROL PASSED: an impossible 0.05 ms budget was met: the timing measures nothing');
const report = [
  `fx-gpu perf: ${COUNT} particles (fx_perf's systems), ${FRAMES} frames, frame work after 60 warm-up frames, ${ROUNDS} interleaved rounds`,
  `test GPU ${pci} for both (GL via DRI_PRIME=${dri})`,
  `load average before ${load0.map((v) => v.toFixed(1)).join(' ')}, after ${load1.map((v) => v.toFixed(1)).join(' ')}`,
  `GL (transform feedback + fragment-pass sort): median ${mg.toFixed(2)} ms  [${times.gl.map((v) => v.toFixed(2)).join(', ')}]`,
  `WebGPU (compute sim + compute sort):          median ${mw.toFixed(2)} ms  [${times.webgpu.map((v) => v.toFixed(2)).join(', ')}]`,
].join('\n');
writeFileSync(join(OUT, 'fx-gpu-report.txt'), report + '\n');
console.log(report);
if (problems.length) {
  console.log('FAIL\n  ' + problems.join('\n  '));
  process.exit(1);
}
console.log('ok');
