// Very Low on WebGPU with the camera under the water: the tonemap pass, fused with the vignette and with bloom off,
// declares the bloom texture (binding 2, unit 1) though it never samples it, and the underwater view drew just before it
// with the depth on that unit. A depth texture does not fit a texture_2d<f32> binding, so every such frame logged a
// WebGPU error; the pass now binds an empty 2D texture there. Native water_shore on WebGPU at CW_QUALITY=verylow, the
// camera under the surface (water.under 1) and the vignette on (the fused variant): 0 WebGPU errors.
// CONTROL: CW_TEST_BLOOM_UNIT=stale leaves the unit as the underwater view left it, and must log errors.
//
//   node tests/post-verylow-underwater.mjs
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
await takeSuiteLock('post-verylow-underwater.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'post-verylow-underwater');
mkdirSync(OUT, { recursive: true });
const CMDS = ['quality verylow', 'set Root/Post vignette true', 'set Root/Camera position vec3(0, -1.2, 10)', 'set Root/Camera rotation vec3(0.12, 0, 0)'];

function run(tag, env) {
  const state = join(OUT, `${tag}.json`);
  const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', 'webgpu', '--app', 'water_shore', '--frames', '60',
    '--start-cmds', CMDS.join(';'), '--state', state, '--shot', join(OUT, `${tag}.png`)];
  return new Promise((res) => {
    const p = spawn('node', args, { env: { ...process.env, CW_QUALITY: 'verylow', ...env }, stdio: ['ignore', 'pipe', 'pipe'] });
    let log = '';
    p.stdout.on('data', (d) => (log += d));
    p.stderr.on('data', (d) => (log += d));
    p.on('close', (code) => {
      let st = null;
      try { st = existsSync(state) ? JSON.parse(readFileSync(state, 'utf8')) : null; } catch {}
      res({ code, log, errors: st ? st['cw.gpu.errors'] : null, under: st ? st['water.under'] : null, quality: st ? st['cw.quality'] : null,
        bloomErrors: (log.match(/u_bloom/g) || []).length });
    });
  });
}

const problems = [];
const ok = await run('fixed', {});
if (ok.code !== 0) problems.push(`the run failed (exit ${ok.code}): ${ok.log.trim().split('\n').slice(-3).join(' | ')}`);
if (ok.errors !== 0) problems.push(`cw.gpu.errors = ${ok.errors}, expected 0`);
if (ok.under !== 1) problems.push(`water.under = ${ok.under}: the camera is not under the water, nothing is checked`);
if (ok.quality !== 0) problems.push(`cw.quality = ${ok.quality}: not Very Low`);
const ctl = await run('control', { CW_TEST_BLOOM_UNIT: 'stale' });
if (!(ctl.bloomErrors > 0 || ctl.errors > 0)) problems.push('CONTROL PASSED: the stale bloom unit logged no WebGPU error, so the check cannot see the bug');
if (problems.length) {
  console.log('FAIL post-verylow-underwater\n  ' + problems.join('\n  '));
  process.exit(1);
}
console.log(`ok   post-verylow-underwater (native WebGPU, Very Low, under the water: 0 errors; control: ${ctl.errors ?? ctl.bloomErrors} errors)`);
