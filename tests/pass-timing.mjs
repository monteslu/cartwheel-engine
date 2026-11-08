// WebGPU pass timestamps time every pass GL's timer queries do (natively, the test GPU, CW_GPU_TIMERS=1).
//   fx_pipeline with FXP_DRAWS=600: its scene pass draws its boxes 600 times (600 draws, one picture), then the
//   effects, the post passes and the overlays run. On WebGPU every pass that ran and got a GPU time on GL gets one
//   too (gpu_ms > 0), so the pipeline's total (gpu_ms, the sum of its passes) leaves none out.
//   post_stack's full segment (MSAA, TAA, bloom, SSAO, grade and the rest): velocity, taa and tonemap are timed on
//   WebGPU as on GL.
//   CONTROL: CW_TEST_TIMING_SPLIT=1 (every draw its own render pass and timestamp pair, as the timers once did) runs
//   the frame out of timestamps in fx_pipeline's scene pass and must leave later passes untimed.
// Holds the GPU suite lock. Run: node tests/pass-timing.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
if (!process.env.CW_SUITE_LOCK_HELD) {
  const r = spawnSync('node', [join(ROOT, 'tools/with-gpu-suite.mjs'), 'pass-timing', 'node', fileURLToPath(import.meta.url), ...process.argv.slice(2)], { stdio: 'inherit' });
  process.exit(r.status ?? 1);
}
const OUT = join(ROOT, 'out', 'gate', 'pass-timing');
mkdirSync(OUT, { recursive: true });
let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };

function stats(app, gpu, frames, env = {}, label = '') {
  const out = join(OUT, `${app}${label}.${gpu}.json`);
  rmSync(out, { force: true });
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', app, '--frames', String(frames), '--gpu', gpu,
    '--end-cmds', 'pipeline', '--end-out', out, '--allow-skipped'], { env: { ...process.env, CW_GPU_TIMERS: '1', ...env }, encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`${app} ${gpu}${label}: ${(r.stderr || r.stdout || '').trim().split('\n').slice(-3).join(' | ')}`);
  const j = JSON.parse(readFileSync(out, 'utf8').trim().split('\n')[0]);
  const passes = j.passes.filter((p) => p.ran && !p.fused);
  return { total: j.gpu_ms, passes, by: Object.fromEntries(passes.map((p) => [p.name, p.gpu_ms])) };
}
const show = (s) => s.passes.map((p) => `${p.name} ${p.gpu_ms.toFixed(3)}`).join(', ');
const untimed = (gl, wg) => gl.passes.filter((p) => p.gpu_ms > 0.0005 && !(wg.by[p.name] > 0)).map((p) => p.name);

// fx_pipeline: a pass of 600 draws, then the rest
const fg = stats('fx_pipeline', 'gl', 90, { FXP_DRAWS: '600' }), fw = stats('fx_pipeline', 'webgpu', 90, { FXP_DRAWS: '600' });
console.log(`     GL:     total ${fg.total.toFixed(3)} ms: ${show(fg)}\n     WebGPU: total ${fw.total.toFixed(3)} ms: ${show(fw)}`);
const miss = untimed(fg, fw);
check(fg.passes.length >= 3 && miss.length === 0, `fx_pipeline (600 draws in its scene pass): every pass GL times is timed on WebGPU${miss.length ? ` (untimed: ${miss.join(', ')})` : ''}`);
const ctl = stats('fx_pipeline', 'webgpu', 90, { FXP_DRAWS: '600', CW_TEST_TIMING_SPLIT: '1' }, '-split');
const cmiss = untimed(fg, ctl);
check(cmiss.length > 0, `CONTROL: a render pass and timestamp pair per draw leaves passes untimed (${cmiss.join(', ') || 'none'}; total ${ctl.total.toFixed(3)} ms)`);

// post_stack's full segment: the post passes the finding named
const pg = stats('post_stack', 'gl', 315), pw = stats('post_stack', 'webgpu', 315);
console.log(`     GL:     total ${pg.total.toFixed(3)} ms: ${show(pg)}\n     WebGPU: total ${pw.total.toFixed(3)} ms: ${show(pw)}`);
for (const n of ['velocity', 'taa', 'tonemap'])
  check(pg.by[n] > 0 && pw.by[n] > 0, `post_stack: ${n} is timed on GL (${(pg.by[n] ?? 0).toFixed(3)} ms) and on WebGPU (${(pw.by[n] ?? 0).toFixed(3)} ms)`);
const pmiss = untimed(pg, pw);
check(pmiss.length === 0, `post_stack: every pass GL times is timed on WebGPU${pmiss.length ? ` (untimed: ${pmiss.join(', ')})` : ''}`);
console.log(fail ? `pass-timing: ${fail} failures` : 'ok   pass-timing');
process.exit(fail ? 1 : 0);
