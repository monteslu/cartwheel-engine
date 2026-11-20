// What a surface of the game's own costs (samples/custom_surface, cartwheel/surface.h): GPU ms of a frame at
// 1920 x 1080 with the three surfaces drawing their effects against the same room with plain materials, per
// graphics preset, on GL and WebGPU, natively on the test GPU. A report, not a gate: the rounds are interleaved
// (plain, surfaces, plain, surfaces...) and each line gives the median of the rounds, with the machine's load
// and the GPU's clock beside it (an integrated GPU's times follow its clock: docs/guides/profiling.md).
//
//   node tools/with-gpu-suite.mjs surface-perf node tests/custom-surface-perf.mjs [--rounds 3] [--frames 240]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { loadavg } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { gpuClock, clockLine } from '../tools/lib/gpu-clock.mjs';
await enterGpuSandbox();

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'custom-surface');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : d);
const rounds = +opt('--rounds', 3), frames = opt('--frames', '240');
const PRESETS = ['verylow', 'low', 'medium', 'high', 'ultra'];
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.floor(s.length / 2)] : NaN; };

function run(gpu, preset, mode) {
  const state = join(OUT, `perf.${gpu}.${preset}.${mode}.json`);
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'custom_surface', '--frames', frames, '--state', state],
    { encoding: 'utf8', env: { ...process.env, CW_GPU_TIMERS: '1', CS_QUALITY: preset, ...(mode === 'plain' ? { CS_MODE: 'stock' } : {}) } });
  if (r.status !== 0) return { err: (r.stderr || r.stdout).trim().split('\n').slice(-2).join(' | ') };
  const st = JSON.parse(readFileSync(state, 'utf8'));
  return { ms: st['cs.gpu_ms'], draws: st['cs.draws'] };
}

const clk = gpuClock();
console.log(`custom_surface, 1920 x 1080, GPU ms a frame (median of ${rounds} interleaved rounds); load ${loadavg().map((v) => v.toFixed(1)).join(' ')}`);
console.log('gpu     preset    plain   surfaces   difference   draws');
let failed = false;
for (const gpu of ['gl', 'webgpu']) for (const preset of PRESETS) {
  const a = [], b = [];
  let draws = '';
  for (let i = 0; i < rounds; i++) {
    const p = run(gpu, preset, 'plain'), s = run(gpu, preset, 'surfaces');
    if (p.err || s.err) { console.log(`${gpu} ${preset}: ${p.err || s.err}`); failed = true; break; }
    a.push(p.ms); b.push(s.ms);
    draws = `${p.draws} / ${s.draws}`;
  }
  if (a.length) console.log(`${gpu.padEnd(7)} ${preset.padEnd(8)} ${median(a).toFixed(2).padStart(6)}   ${median(b).toFixed(2).padStart(6)}   ${(median(b) - median(a)).toFixed(2).padStart(8)}      ${draws}   (rounds: ${a.map((v, i) => `${v.toFixed(2)}/${b[i].toFixed(2)}`).join(' ')})`);
}
console.log(clockLine(await clk.stop()));
console.log(`load at the end ${loadavg().map((v) => v.toFixed(1)).join(' ')}`);
process.exit(failed ? 1 : 0);
