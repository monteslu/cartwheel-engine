// Every dual cart of build/cart-wgpu runs on WebGPU without trapping: each is
// loaded with wasmcart's Node host (deps/wasmcart-wgpu, webgpu-node on the
// test GPU) and runs a few frames. A cart that reaches GL while the host gave
// it WebGPU traps (the host's import trap names the GL function), and so does
// one that calls WebGPU wrongly enough to throw; either fails the check, as
// does a cart the host did not run on WebGPU or WebGPU errors the engine
// counted (debug field cw.gpu.errors).
//
// The carts checked are the ones the build produces (build/cart-wgpu/carts.txt,
// written by CMake; apps built with cw_add_app(... GL_ONLY) make no dual cart),
// never whatever else lies in carts/. A native GL_ONLY app (input_demo) asked
// for WebGPU must refuse at startup, naming GL_ONLY.
//
//   node tests/wgpu-carts.mjs [cart ...] [--frames N] [-j N]
import { spawn } from 'node:child_process';
import { readFileSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { armGuard, killTree } from '../tools/lib/gpu-guard.mjs';
import { testEnv, wgpuPin } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const BUILD = join(ROOT, 'build', 'cart-wgpu');
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : d);
const frames = +opt('--frames', 30);

// ── one cart (a child process: a trap or a crash stays its own) ───────────
if (argv[0] === '--one') {
  const file = argv[1];
  const { CartHost } = await import(pathToFileURL(join(ROOT, 'deps', 'wasmcart-wgpu', 'index.js')).href);
  const pin = JSON.parse(process.env.CW_WGPU_CARTS_PIN);
  const host = new CartHost();
  let code = 0;
  try {
    await host.load(file, { adapterOptions: { powerPreference: pin.power }, deterministic: { seed: 1 } });
    if (!host.usesWgpu) throw new Error('the host did not run it on WebGPU (usesWgpu false)');
    const a = host.getGpuAdapterInfo ? host.getGpuAdapterInfo() : null;
    const name = a ? String(a.device || a.description || '') : '';
    if (!name.toLowerCase().includes(pin.match)) throw new Error(`WebGPU adapter "${name}" is not the test GPU (${pin.match})`);
    for (let i = 0; i < frames; i++) {
      host.runFrame();
      await new Promise((r) => setImmediate(r)); // WebGPU's asynchronous results arrive between frames
      if (host.gpuLost) throw new Error(`WebGPU device lost at frame ${i + 1}: ${JSON.stringify(host.gpuLost)}`);
    }
    let errors = 0;
    try { errors = host.readDebugValue('cw.gpu.errors').value; } catch { /* no debug channel */ }
    if (errors) throw new Error(`${errors} WebGPU error(s) (cw.gpu.errors)`);
  } catch (e) {
    console.log(String(e && e.message ? e.message : e).split('\n')[0]);
    code = 1;
  }
  try { host.destroy(); } catch { /* the cart failed */ }
  process.exit(code);
}

// ── the suite ─────────────────────────────────────────────────────────────
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
await takeSuiteLock('wgpu-carts.mjs');
testEnv(process.env);
const list = join(BUILD, 'carts.txt');
if (!existsSync(list)) {
  console.error(`wgpu-carts: ${list} does not exist (TARGETS=cart-wgpu scripts/build.sh writes it)`);
  process.exit(2);
}
const only = argv.filter((a, i) => !a.startsWith('-') && !['--frames', '-j'].includes(argv[i - 1]));
const carts = readFileSync(list, 'utf8').split('\n').map((s) => s.trim()).filter(Boolean).filter((n) => !only.length || only.includes(n));
const missing = carts.filter((n) => !existsSync(join(BUILD, 'carts', `${n}.wasc`)));
if (missing.length) {
  console.error(`wgpu-carts: not built: ${missing.join(' ')}`);
  process.exit(2);
}
const pin = wgpuPin();
const guard = armGuard('wgpu-carts.mjs');
await guard.gate('wgpu-carts');
const jobs = Math.min(8, Math.max(1, +opt('-j', 4)));
const env = { ...process.env, ...pin.env, CW_WGPU_CARTS_PIN: JSON.stringify({ power: pin.power, match: pin.match }) };

function check(name) {
  return new Promise((res) => {
    const p = spawn(process.execPath, [fileURLToPath(import.meta.url), '--one', join(BUILD, 'carts', `${name}.wasc`), '--frames', String(frames)],
      { env, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    p.stdout.on('data', (d) => (out += d));
    p.stderr.on('data', () => {}); // the carts' own logging
    const t = setTimeout(() => { out += 'timed out after 120 s'; killTree(p.pid); }, 120000);
    p.on('close', (code, sig) => { clearTimeout(t); res({ name, ok: code === 0, why: out.trim() || `exit ${code ?? sig}` }); });
  });
}

const failed = [];
let next = 0;
await Promise.all(Array.from({ length: jobs }, async () => {
  while (next < carts.length) {
    const r = await check(carts[next++]);
    console.log(`${r.ok ? 'ok  ' : 'FAIL'} ${r.name}${r.ok ? '' : ': ' + r.why}`);
    if (!r.ok) failed.push(r.name);
  }
}));
// A GL_ONLY app run natively with CW_GPU=webgpu refuses at startup with a
// message naming GL_ONLY (src/platform/gpu.c), not a crash in its first GL call.
const glOnly = await new Promise((res) => {
  const exe = join(ROOT, 'build', 'native', 'samples', 'input_demo');
  if (!existsSync(exe)) return res({ ok: false, why: `${exe} not built (TARGETS=native scripts/build.sh)` });
  const p = spawn(exe, [], { cwd: dirname(exe), env: { ...env, CW_GPU: 'webgpu', CW_FRAMES: '5', SDL_AUDIODRIVER: 'dummy' }, stdio: ['ignore', 'pipe', 'pipe'] });
  let out = '';
  p.stdout.on('data', (d) => (out += d));
  p.stderr.on('data', (d) => (out += d));
  const t = setTimeout(() => killTree(p.pid), 60000);
  p.on('close', (code, sig) => {
    clearTimeout(t);
    const ok = code === 1 && !sig && /has no WebGPU build/.test(out) && /GL_ONLY/.test(out);
    res({ ok, why: `exit ${code ?? sig}: ${out.trim().split('\n').slice(-2).join(' | ')}` });
  });
});
console.log(`${glOnly.ok ? 'ok  ' : 'FAIL'} native GL_ONLY input_demo refuses CW_GPU=webgpu${glOnly.ok ? '' : ': ' + glOnly.why}`);
if (!glOnly.ok) failed.push('native-gl-only');
guard.finish();
console.log(`wgpu-carts: ${carts.length - failed.length}/${carts.length} dual carts ran ${frames} frames on WebGPU` +
  (failed.length ? `; trapped or failed: ${failed.join(' ')}` : ''));
process.exit(failed.length ? 1 : 0);
