// A cart whose worker thread dies in the middle of a job chunk must stop with
// an error, not spin forever. In a cart the page thread never blocks in
// cw_job_wait; a host learns of a dead worker only when its own thread returns
// to its event loop, which a spinning cart never lets it do (a threaded cart
// wedged romdev's shared server this way). The cart is
// build/cart-wgpu/carts/jobs_stall.wasc (tests/unit/jobs_stall.c), loaded with
// wasmcart's Node host on GL:
//   1. 30 frames of parallel jobs run with no false alarm;
//   2. after `jobs_stall_test 1500` a worker traps in a chunk: the run must end
//      within 20 s with the engine's "a worker thread stopped in the middle of a
//      chunk" error;
//   3. control: `jobs_stall_test 0` (the check off) with the same dead worker
//      must still be running after 12 s (the spin the check exists to end).
//   node tests/jobs-stall.mjs
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const CART = join(ROOT, 'build', 'cart-wgpu', 'carts', 'jobs_stall.wasc');

if (process.argv[2] === '--one') {
  const ms = process.argv[3];
  const { CartHost } = await import(pathToFileURL(join(ROOT, 'deps', 'wasmcart-wgpu', 'index.js')).href);
  const host = new CartHost();
  await host.load(CART, { wgpu: false });
  const post = (text) => {
    const ex = host.instance.exports;
    const dv = new DataView((host.memory || ex.memory).buffer);
    const str = (q) => { let s = ''; for (let b; (b = dv.getUint8(q)); q++) s += String.fromCharCode(b); return s; };
    const f = {};
    for (let p = ex.wc_debug_state(); ; p += 16) {
      const name = dv.getUint32(p, true);
      if (!name) break;
      f[str(name)] = dv.getUint32(p + 4, true);
    }
    const bytes = new TextEncoder().encode(text);
    for (let i = 0; i < bytes.length; i++) dv.setUint8(f['dbg.cmd'] + i, bytes[i]);
    dv.setUint8(f['dbg.cmd'] + bytes.length, 0);
    dv.setUint32(f['dbg.cmd_seq'], dv.getUint32(f['dbg.cmd_seq'], true) + 1, true);
  };
  for (let i = 0; i < 30; i++) {
    await host.runFrame();
    await new Promise((r) => setImmediate(r)); // the workers start and run between frames
  }
  console.log(`RESULT clean frames=${host.readDebugValue('jobs.frames').value}`);
  if (ms === 'none') process.exit(0);
  post(`jobs_stall_test ${ms}`);
  for (let i = 0; i < 600; i++) {
    await host.runFrame();
    await new Promise((r) => setImmediate(r));
  }
  console.log('RESULT the dead worker never stopped a job');
  process.exit(0);
}

await enterGpuSandbox();
await takeSuiteLock('jobs-stall.mjs');
testEnv(process.env);
if (!existsSync(CART)) {
  console.error(`jobs-stall: ${CART} not built (TARGETS=cart-wgpu scripts/build.sh)`);
  process.exit(2);
}
function run(ms, limitMs) {
  return new Promise((res) => {
    const t0 = Date.now();
    const p = spawn(process.execPath, [fileURLToPath(import.meta.url), '--one', ms], { stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    p.stdout.on('data', (d) => (out += d));
    p.stderr.on('data', (d) => (out += d));
    let timedOut = false;
    const t = setTimeout(() => { timedOut = true; p.kill('SIGKILL'); }, limitMs);
    p.on('close', (code) => { clearTimeout(t); res({ code, timedOut, out, secs: ((Date.now() - t0) / 1000).toFixed(1) }); });
  });
}
let fail = 0;
const clean = await run('none', 60000);
const cleanOk = clean.code === 0 && /RESULT clean frames=30/.test(clean.out);
console.log(`${cleanOk ? 'ok  ' : 'FAIL'} 30 frames of parallel jobs, no false alarm${cleanOk ? '' : ': ' + clean.out.trim().split('\n').slice(-3).join(' | ')}`);
if (!cleanOk) fail++;
const stall = await run('1500', 20000 + 20000);
const named = /a worker thread stopped in the middle of a chunk/.test(stall.out);
const stallOk = !stall.timedOut && stall.code !== 0 && named && /RESULT clean/.test(stall.out);
console.log(`${stallOk ? 'ok  ' : 'FAIL'} a dead worker stops the cart with the engine's error (${stall.secs} s)${stallOk ? '' : `: exit ${stall.code}${stall.timedOut ? ' (timed out)' : ''}, ${stall.out.trim().split('\n').slice(-3).join(' | ')}`}`);
if (!stallOk) fail++;
const control = await run('0', 12000 + 20000);
const controlOk = control.timedOut && /RESULT clean/.test(control.out);
console.log(`${controlOk ? 'ok  ' : 'FAIL'} control: with the check off the page thread is still spinning after ${control.secs} s${controlOk ? '' : `: CONTROL PASSED (exit ${control.code}): ${control.out.trim().split('\n').slice(-2).join(' | ')}`}`);
if (!controlOk) fail++;
process.exit(fail ? 1 : 0);
