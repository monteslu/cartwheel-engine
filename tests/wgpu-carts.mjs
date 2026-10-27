// Every dual cart of build/cart-wgpu runs on BOTH of its backends: the same
// .wasc is loaded with wasmcart's Node host (deps/wasmcart-wgpu) once given
// WebGPU (webgpu-node on the test GPU) and once told to use GL (wgpu: false,
// webgl-node), and runs as many frames as the longest gate of its app in
// tests/gates.json (at least --frames, 30 by default), so a sample that turns
// a feature on late (post_stack's MSAA segment starts at frame 220) is
// reached.
//
// A WebGPU run fails on any GL call: the engine stops with the GL function,
// the call site and the feature that was running (cartwheel/gl.h), and a GL
// call outside the engine's headers traps in the host ("the cart called glX,
// but this host selected WebGPU"). It also fails when the host did not run the
// cart on WebGPU, on WebGPU errors the engine counted (cw.gpu.errors) or a
// lost device. A GL run fails when the cart did not draw with GL. Both fail
// on a trap, a debug field cw.gpu.api other than the backend asked for, and,
// for an app with a frame gate, a last frame with fewer colours than the
// gate's minColors (default 2; an app with no frame gate, a measurement app
// such as fx_spikes, gets no picture check).
//
// The control must fail: post_stack is run on WebGPU with the pipeline's old
// defect put back (debug command `pipeline_test gl_msaa_on_webgpu`: a scene
// that cannot be multisampled on WebGPU asks GL for an MSAA framebuffer), and
// must stop with the engine's message naming the feature and the GL call.
//
// The carts checked are the ones the build produces (build/cart-wgpu/carts.txt,
// written by CMake; apps built with cw_add_app(... GL_ONLY) make no dual cart),
// never whatever else lies in carts/. A native GL_ONLY app (input_demo) asked
// for WebGPU must refuse at startup, naming GL_ONLY.
//
// CW_PROJECT_ROOT=<games repository>: its build/cart-wgpu carts and its
// tests/gates.json instead (no control or native check: those are the engine's).
//
//   node tests/wgpu-carts.mjs [cart ...] [--frames N] [--gpu webgpu|gl] [-j N]
import { spawn } from 'node:child_process';
import { readFileSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { armGuard, killTree } from '../tools/lib/gpu-guard.mjs';
import { testEnv, wgpuPin } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PROJ = process.env.CW_PROJECT_ROOT ? resolve(process.env.CW_PROJECT_ROOT) : ROOT;
const BUILD = join(PROJ, 'build', 'cart-wgpu');
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : d);
const minFrames = +opt('--frames', 30);
const VALUED = ['--frames', '-j', '--gpu', '--cmd', '--min-colors'];

// ── one cart on one backend (a child process: a trap or a crash stays its own) ──
if (argv[0] === '--one') {
  const file = argv[1];
  const gpu = opt('--gpu', 'webgpu');
  const frames = +opt('--frames', 30);
  const cmd = opt('--cmd', '');
  const minColors = +opt('--min-colors', 0); // the picture check (0: the app has no frame gate, e.g. a measurement app) // "@frame text": a debug-channel request posted before that frame (0-based)
  const { CartHost } = await import(pathToFileURL(join(ROOT, 'deps', 'wasmcart-wgpu', 'index.js')).href);
  const pin = JSON.parse(process.env.CW_WGPU_CARTS_PIN);
  const host = new CartHost();
  let code = 0;
  const field = (name) => { try { return host.readDebugValue(name).value; } catch { return undefined; } };
  const postCmd = (text) => {
    const ex = host.instance.exports;
    if (!ex.wc_debug_state) throw new Error('--cmd: the cart has no debug channel');
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
  try {
    await host.load(file, gpu === 'webgpu'
      ? { adapterOptions: { powerPreference: pin.power }, deterministic: { seed: 1 } }
      : { wgpu: false, deterministic: { seed: 1 } });
    if (gpu === 'webgpu') {
      if (!host.usesWgpu) throw new Error('the host did not run it on WebGPU (usesWgpu false)');
      const a = host.getGpuAdapterInfo ? host.getGpuAdapterInfo() : null;
      const name = a ? String(a.device || a.description || '') : '';
      if (!name.toLowerCase().includes(pin.match)) throw new Error(`WebGPU adapter "${name}" is not the test GPU (${pin.match})`);
    } else if (host.usesWgpu || !host.usesGL) throw new Error('the host did not run it on GL (wgpu: false)');
    const at = cmd ? +cmd.slice(1, cmd.indexOf(' ')) : -1;
    let shot = null;
    for (let i = 0; i < frames; i++) {
      if (i === at) postCmd(cmd.slice(cmd.indexOf(' ') + 1));
      await host.runFrame();
      if (gpu === 'webgpu' && i === frames - 1) shot = await host.readGpuFrame();
      await new Promise((r) => setImmediate(r)); // WebGPU's asynchronous results arrive between frames
      if (host.gpuLost) throw new Error(`WebGPU device lost at frame ${i + 1}: ${JSON.stringify(host.gpuLost)}`);
    }
    const api = field('cw.gpu.api');
    if (api !== undefined && api !== (gpu === 'webgpu' ? 2 : 1)) throw new Error(`the cart reports cw.gpu.api ${api} on a ${gpu} run`);
    const errors = field('cw.gpu.errors');
    if (errors) throw new Error(`${errors} ${gpu === 'webgpu' ? 'WebGPU' : 'GL'} error(s) (cw.gpu.errors)`);
    let px;
    if (shot) px = new Uint8Array(shot.data.buffer || shot.data);
    else {
      const gl = host.getGlContext();
      // the frame the cart drew is in the host's redirect target (the default framebuffer
      // holds nothing readable after a present)
      const read = (w, h) => {
        px = new Uint8Array(w * h * 4);
        gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
      };
      if (!host.withRenderedFrame(read)) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        read(gl.drawingBufferWidth, gl.drawingBufferHeight);
      }
    }
    const colours = new Set();
    for (let i = 0; i < px.length && colours.size < minColors; i += 4) colours.add(px[i] | (px[i + 1] << 8) | (px[i + 2] << 16));
    if (colours.size < minColors) throw new Error(`the last frame has ${colours.size} colour(s) on ${gpu}, its gate wants ${minColors}`);
  } catch (e) {
    console.log(`RESULT ${String(e && e.message ? e.message : e).split('\n')[0]}`);
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
const only = argv.filter((a, i) => !a.startsWith('-') && !VALUED.includes(argv[i - 1]));
const carts = readFileSync(list, 'utf8').split('\n').map((s) => s.trim()).filter(Boolean).filter((n) => !only.length || only.includes(n));
const missing = carts.filter((n) => !existsSync(join(BUILD, 'carts', `${n}.wasc`)));
if (missing.length) {
  console.error(`wgpu-carts: not built: ${missing.join(' ')}`);
  process.exit(2);
}
// frames per cart: its app's longest gate
const gateFrames = {}, gateColors = {};
try {
  for (const g of JSON.parse(readFileSync(join(PROJ, 'tests', 'gates.json'), 'utf8')))
    if (g.app && g.frames) {
      gateFrames[g.app] = Math.max(gateFrames[g.app] || 0, g.frames);
      gateColors[g.app] = Math.max(gateColors[g.app] || 0, g.minColors ?? 2);
    }
} catch { /* no gates file: --frames for every cart */ }
const framesOf = (name) => Math.max(minFrames, gateFrames[name] || 0);
const gpus = argv.includes('--gpu') ? [opt('--gpu')] : ['webgpu', 'gl'];
const pin = wgpuPin();
const guard = armGuard('wgpu-carts.mjs');
await guard.gate('wgpu-carts');
const jobs = Math.min(8, Math.max(1, +opt('-j', 4)));
const env = { ...process.env, ...pin.env, CW_WGPU_CARTS_PIN: JSON.stringify({ power: pin.power, match: pin.match }) };

function check(name, gpu, extra = []) {
  return new Promise((res) => {
    const frames = framesOf(name);
    const p = spawn(process.execPath, [fileURLToPath(import.meta.url), '--one', join(BUILD, 'carts', `${name}.wasc`), '--frames', String(frames),
      '--gpu', gpu, '--min-colors', String(gateColors[name] || 0), ...extra], { env, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '', log = '';
    p.stdout.on('data', (d) => (out += d));
    p.stderr.on('data', (d) => (log += d)); // the cart's own logging
    const t = setTimeout(() => { out += 'RESULT timed out'; killTree(p.pid); }, 60000 + frames * 400);
    p.on('close', (code, sig) => {
      clearTimeout(t);
      const why = (out.match(/RESULT (.*)/) || [])[1] || `exit ${code ?? sig}`;
      res({ name, gpu, ok: code === 0, why, log: out + log });
    });
  });
}

const failed = [];
const todo = carts.flatMap((n) => gpus.map((g) => [n, g]));
let next = 0;
await Promise.all(Array.from({ length: jobs }, async () => {
  while (next < todo.length) {
    const [n, g] = todo[next++];
    const r = await check(n, g);
    console.log(`${r.ok ? 'ok  ' : 'FAIL'} ${r.name} (${g}, ${framesOf(n)} frames)${r.ok ? '' : ': ' + r.why}`);
    if (!r.ok) {
      const site = r.log.split('\n').find((l) => / called GL \(/.test(l));
      if (site) console.log(`     ${site.trim()}`);
      failed.push(`${r.name}/${g}`);
    }
  }
}));

if (PROJ === ROOT && (!only.length || only.includes('post_stack'))) {
  // the control: the old GL MSAA framebuffer on WebGPU must be caught, and named
  if (!existsSync(join(BUILD, 'carts', 'post_stack.wasc'))) failed.push('control (post_stack not built)');
  else {
    const r = await check('post_stack', 'webgpu', ['--cmd', '@10 pipeline_test gl_msaa_on_webgpu']);
    const named = /PostProcess msaa/.test(r.log) && /called GL \(gl(GenFramebuffers|GenRenderbuffers|BindRenderbuffer)/.test(r.log);
    if (r.ok) console.log('FAIL control: post_stack with the GL MSAA defect put back ran on WebGPU: CONTROL PASSED');
    else if (!named) console.log(`FAIL control: post_stack failed, but not with the engine naming the feature and the GL call: ${r.why}`);
    else console.log('ok   control: the GL MSAA defect on WebGPU is caught and named');
    if (r.ok || !named) failed.push('control');
  }
}

if (PROJ === ROOT && !only.length) {
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
}
guard.finish();
console.log(`wgpu-carts: ${todo.length - failed.filter((f) => f.includes('/')).length}/${todo.length} dual cart runs (${gpus.join(' + ')}) passed` +
  (failed.length ? `; failed: ${failed.join(' ')}` : ''));
process.exit(failed.length ? 1 : 0);
