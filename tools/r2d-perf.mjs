// The 2D renderer's perf report: samples/r2d_stress case by case, natively on GL and WebGPU, at each size,
// with what the renderer did (draw calls, WebGPU calls) and how long it took (CPU: submission plus cw_r2d_end;
// frame: to GPU completion through cw_gpu_finish, so GPU work is in it). Runs inside the GPU suite:
//   node tools/with-gpu-suite.mjs r2d-perf node tools/r2d-perf.mjs [--cases a,b] [--gpus gl,webgpu]
//        [--sizes 1920x1080,3840x2160] [--frames 40] [--json out/r2d-perf/report.json]
//        [--build build/native] [--quality high] [--timers] [--shots] [--check]
// --timers: GL time of each part of cw_r2d_end (draw, field, lights, gi, composite, post; CW_R2D_TIMERS=1)
// --check fails (exit 1) when a case's WebGPU calls go over its budget in BUDGETS below, the
// numbers that hold the weak-host budget (about 8000 WebGPU calls a frame for a whole game on a
// Vega 8 libretro box). Its control (--control) runs the same check against budgets of one call,
// which must fail.
import { spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync, existsSync, mkdirSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes('--' + k) ? argv[argv.indexOf('--' + k) + 1] : d);
const flag = (k) => argv.includes('--' + k);
const ALL = ['sprites10k', 'sprites50k', 'sprites100k', 'sprites200k', 'lit1', 'lit16', 'lit64', 'lit500', 'shadow1', 'shadow16',
  'shadow64', 'shadow500', 'tiles', 'particles50k', 'text', 'rocks20k', 'post', 'soft64', 'volumetric16', 'postnobloom', 'gi16', 'gpupart100k', 'gpupart1m', 'inst200k', 'instkept200k', 'sidescroll'];
const CASES = opt('cases', ALL.join(',')).split(',');
const GPUS = opt('gpus', 'gl,webgpu').split(',');
const SIZES = opt('sizes', '1920x1080').split(',');
const FRAMES = +opt('frames', '40');
const BUILD = opt('build', 'build/native'); // the tree whose samples/r2d_stress runs
const QUALITY = opt('quality', ''); // a preset for every run (CW_QUALITY: verylow, low, medium, high, ultra)
const OUT = join(ROOT, 'out', 'r2d-perf');
mkdirSync(OUT, { recursive: true });
const JSON_OUT = resolve(opt('json', join(OUT, 'report.json')));
// WebGPU calls a case may make (the 2D renderer's share of a frame)
const BUDGETS = { default: 300, gi16: 800, sidescroll: 950 }; /* measured: lit cases 65, shadowed 226-232, post 219, gi16 542, instances 21-22, sidescroll 765 (GI, shadows, post and depth culling's pre-pass) */

const rows = [];
for (const size of SIZES)
  for (const gpu of GPUS)
    for (const c of CASES) {
      const base = join(OUT, `${c}.${gpu}.${size}`);
      const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', 'r2d_stress', '--exe', resolve(ROOT, BUILD, 'samples/r2d_stress'), '--frames', String(FRAMES),
        '--gpu', gpu, '--state', base + '.json', ...(flag('shots') ? ['--shot', base + '.png'] : [])],
        { env: { ...process.env, CW_STRESS: c, CW_STRESS_SIZE: size, ...(QUALITY ? { CW_QUALITY: QUALITY } : {}), ...(flag('timers') ? { CW_R2D_TIMERS: '1' } : {}) }, encoding: 'utf8', timeout: 300000 });
      if (r.status !== 0 || !existsSync(base + '.json')) {
        console.log(`FAIL ${c} ${gpu} ${size}: run failed\n${(r.stderr || '').split('\n').slice(-6).join('\n')}`);
        rows.push({ case: c, gpu, size, failed: true });
        continue;
      }
      const s = JSON.parse(readFileSync(base + '.json', 'utf8'));
      rmSync(base + '.json');
      const i = ALL.indexOf(c);
      const pick = (k) => (Array.isArray(s[k]) ? s[k][i] : s[k]);
      rows.push({ case: c, gpu, size, cpu_ms: +pick('stress.cpu_ms').toFixed(3), frame_ms: +pick('stress.frame_ms').toFixed(3),
        draws: pick('stress.draws'), gpu_calls: pick('stress.gpu_calls'), lights: pick('stress.lights'),
        ...(flag('timers') ? { parts: Object.fromEntries(Object.entries(s).filter(([k]) => k.startsWith('r2d_gpu.')).map(([k, v]) => [k.slice(8), +(+v).toFixed(3)])) } : {}) });
    }
console.log('case            gpu     size        cpu ms  frame ms  draws  gpu calls');
for (const r of rows)
  if (!r.failed)
    console.log(`${r.case.padEnd(15)} ${r.gpu.padEnd(7)} ${r.size.padEnd(11)} ${String(r.cpu_ms).padStart(7)} ${String(r.frame_ms).padStart(9)} ${String(r.draws).padStart(6)} ${String(r.gpu_calls).padStart(10)}${r.parts ? '  ' + JSON.stringify(r.parts) : ''}`);
writeFileSync(JSON_OUT, JSON.stringify(rows, null, 1));
let fail = rows.some((r) => r.failed);
if (flag('check') || flag('control')) {
  for (const r of rows) {
    if (r.failed || r.gpu !== 'webgpu') continue;
    const b = flag('control') ? 1 : BUDGETS[r.case] ?? BUDGETS.default;
    if (r.gpu_calls > b) { console.log(`FAIL ${r.case} ${r.size}: ${r.gpu_calls} WebGPU calls (budget ${b})`); fail = true; }
  }
  if (flag('control')) { console.log(fail ? 'ok   control failed as it must' : 'CONTROL PASSED'); process.exit(fail ? 0 : 1); }
}
process.exit(fail ? 1 : 0);
