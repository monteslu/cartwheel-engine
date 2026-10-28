// A cart's heap grows while every job thread allocates: no thread may trap on
// memory another thread grew. V8 (Node, Chromium) brings a thread's copy of a
// shared wasm memory's size up to date only at that thread's next interrupt
// check, and memset / memcpy (memory.fill / memory.copy) check the copy, so a
// thread that waited a moment for malloc's lock while another thread grew the
// heap, then got its block from the new pages, trapped with "memory access out
// of bounds" (a navmesh tile build job did, about 1 run in 4 of a game). The
// engine refreshes the thread's view on every allocation and at job
// boundaries (src/core/mem.c). The cart is build/cart-wgpu/carts/mem_grow.wasc
// (tests/unit/mem_grow.c), loaded with wasmcart's Node host on GL:
//   1. RUNS runs (default 3) of `mem_grow_test 1 <storms>`: every storm
//      finishes, no allocation hands a thread a block it cannot see
//      (mem.bad 0), and the engine refreshed at least one stale view in the
//      runs (the race really happened and was handled);
//   2. control: `mem_grow_test 0 <storms>` (refresh off) must trap in at least
//      one of CONTROL_RUNS runs (default 6; it trapped in 2 of 3 runs measured).
//   node tests/mem-grow.mjs [--runs N] [--storms N] [--control-runs N]
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const CART = join(ROOT, 'build', 'cart-wgpu', 'carts', 'mem_grow.wasc');
const arg = (name, def) => {
  const i = process.argv.indexOf(name);
  return i > 0 ? +process.argv[i + 1] : def;
};

if (process.argv[2] === '--one') {
  const [sync, storms] = [process.argv[3], +process.argv[4]];
  const { CartHost } = await import(pathToFileURL(join(process.env.WASMCART_DIR || join(ROOT, 'deps', 'wasmcart-wgpu'), 'index.js')).href);
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
  const v = (n) => host.readDebugValue(n).value;
  for (let i = 0; i < 5; i++) {
    await host.runFrame();
    await new Promise((r) => setImmediate(r)); // the workers start between frames
  }
  post(`mem_grow_test ${sync} ${storms}`);
  for (let i = 0; i < storms + 10 && v('mem.storms') < storms; i++) {
    await host.runFrame();
    await new Promise((r) => setImmediate(r));
  }
  console.log(`RESULT storms=${v('mem.storms')} blocks=${v('mem.blocks')} pages=${v('mem.pages')} refreshes=${v('mem.refreshes')} bad=${v('mem.bad')}`);
  process.exit(0);
}

await enterGpuSandbox();
await takeSuiteLock('mem-grow.mjs');
testEnv(process.env);
if (!existsSync(CART)) {
  console.error(`mem-grow: ${CART} not built (TARGETS=cart-wgpu scripts/build.sh)`);
  process.exit(2);
}
const RUNS = arg('--runs', 3), STORMS = arg('--storms', 6);
function run(sync) {
  return new Promise((res) => {
    const p = spawn(process.execPath, [fileURLToPath(import.meta.url), '--one', String(sync), String(STORMS)], { stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    p.stdout.on('data', (d) => (out += d));
    p.stderr.on('data', (d) => (out += d));
    let timedOut = false;
    const t = setTimeout(() => { timedOut = true; p.kill('SIGKILL'); }, 120000);
    p.on('close', (code) => {
      clearTimeout(t);
      const m = /RESULT storms=(\d+) blocks=(\d+) pages=(\d+) refreshes=(\d+) bad=(\d+)/.exec(out);
      const r = m ? { storms: +m[1], blocks: +m[2], pages: +m[3], refreshes: +m[4], bad: +m[5] } : null;
      const trapped = /memory access out of bounds|a worker thread stopped in the middle of a chunk/.test(out);
      res({ code, timedOut, out, r, trapped });
    });
  });
}
const tail = (o) => o.trim().split('\n').filter((l) => !/\[debug\]/.test(l)).slice(-3).join(' | ');
let fail = 0, refreshed = 0;
for (let i = 0; i < RUNS; i++) {
  const x = await run(1);
  const ok = x.code === 0 && !x.trapped && x.r && x.r.storms === STORMS && x.r.bad === 0;
  if (x.r) refreshed += x.r.refreshes;
  console.log(`${ok ? 'ok  ' : 'FAIL'} run ${i + 1}: ${STORMS} storms on 24 threads, ${x.r ? `${x.r.blocks} blocks, heap ${x.r.pages} pages, ${x.r.refreshes} stale views refreshed, ${x.r.bad} unseen blocks` : 'no result'}` +
    `${ok ? '' : `: exit ${x.code}${x.timedOut ? ' (timed out)' : ''}${x.trapped ? ' (trapped)' : ''}, ${tail(x.out)}`}`);
  if (!ok) fail++;
}
const raced = refreshed > 0;
console.log(`${raced ? 'ok  ' : 'FAIL'} the race happened and was handled: ${refreshed} stale views refreshed in ${RUNS} runs${raced ? '' : ' (none: the test no longer races the heap)'}`);
if (!raced) fail++;
const CONTROL_RUNS = arg('--control-runs', 6);
let traps = 0, tries = 0;
for (; tries < CONTROL_RUNS && !traps; tries++) {
  const x = await run(0);
  if (x.trapped || (x.r && x.r.bad > 0) || x.code !== 0) traps++;
}
const controlOk = traps > 0;
console.log(`${controlOk ? 'ok  ' : 'FAIL'} control: with the refresh off a thread traps on memory another thread grew${controlOk ? ` (run ${tries})` : ': CONTROL PASSED (no trap in ' + CONTROL_RUNS + ' runs)'}`);
if (!controlOk) fail++;
process.exit(fail ? 1 : 0);
