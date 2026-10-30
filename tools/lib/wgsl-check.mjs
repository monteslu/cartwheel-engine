// Compile every captured WebGPU program (three.c's program-table captures: <state>.wgpu.vert / .frag / .json)
// with Dawn's own WGSL compiler (Tint) into a render pipeline, the way a host does when it first draws the state.
//
// Why: the captures run on three.c's mock GPU, which accepts any WGSL. Tint can still reject a program at
// pipeline creation (an internal compiler error on valid WGSL is one way: "swizzle view instruction still has
// usages after lowering" for a swizzle of a swizzle on a variable, v.xy.x), and a table holding such a
// program draws nothing on WebGPU while every tool says ok. tools/programs.mjs runs this after each
// regeneration and fails on any rejection.
//
// The vertex buffers are three.c's own layout (its tools/program-model.mjs vertexBufferLayout: the instance
// matrix columns share one buffer), and a program that needs more vertex buffers or attributes than every
// WebGPU device has (maxVertexBuffers 8, maxVertexAttributes 16, core and compatibility mode alike) is
// reported as over the device limits, naming the buffers, before Dawn sees it.
//
// It runs on lavapipe (Mesa's software Vulkan), never on a hardware GPU: Tint's output does not depend on
// the driver, so no GPU lock is needed and CI can run it. A program that passed is stamped
// (<state>.wgpu.tint: the hash of its sources and of the Dawn library) and not compiled again.
//
//   node tools/lib/wgsl-check.mjs <capture dir> [state ...]      (exit 1 on any rejection)
//   CW_THREE_DIR=<three.c> picks the three.c whose layout is checked (default deps/three.c)
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const LVP = ['/usr/share/vulkan/icd.d/lvp_icd.json', '/usr/share/vulkan/icd.d/lvp_icd.x86_64.json'];

/** an engine checkout, where the check's tools are always fetched; a prebuilt SDK has none of them */
const ENGINE_TREE = existsSync(join(ROOT, 'tests', 'gates.json'));
/** webgpu-node (native-dawn's Dawn) as the engine's deps carry it */
function webgpuNode() {
  const c = [process.env.WEBGPU_NODE, join(ROOT, 'deps', 'wasmcart', 'node_modules', 'webgpu-node'),
    join(ROOT, 'deps', 'wasmcart-wgpu', 'node_modules', 'webgpu-node')].filter(Boolean);
  const d = c.find((x) => existsSync(join(x, 'index.mjs')));
  if (!d) throw new Error(`wgsl-check: webgpu-node not found (looked in ${c.join(', ')}; scripts/fetch-deps.sh, or WEBGPU_NODE=<dir>)`);
  return d;
}

/** the three.c tree whose vertex buffer layout the check uses */
export function threeDir() { return resolve(process.env.CW_THREE_DIR || join(ROOT, 'deps', 'three.c')); }

/** the captured states in a directory (each with .wgpu.vert, .wgpu.frag, .wgpu.json) */
export function capturedStates(dir) {
  return readdirSync(dir).filter((f) => f.endsWith('.wgpu.json')).map((f) => f.slice(0, -'.wgpu.json'.length))
    .filter((s) => existsSync(join(dir, s + '.wgpu.vert'))).sort();
}

/**
 * Compile the states' WGSL with Tint (a child process on lavapipe). Throws, listing each rejected state and
 * Tint's message, when any is rejected.
 * @param {string} dir capture directory
 * @param {string[]} [states] default: every captured state in it
 * @param {{ three?: string }} [opts] the three.c tree (default: threeDir())
 * @returns {{ checked: number, cached: number }}
 */
export function checkWgsl(dir, states = capturedStates(dir), { three = threeDir() } = {}) {
  const icd = LVP.find(existsSync);
  if (!ENGINE_TREE && !process.env.WEBGPU_NODE) return { checked: 0, cached: 0, skipped: 'a prebuilt SDK carries no webgpu-node (WEBGPU_NODE=<dir> names one); a program Tint rejects shows on its first WebGPU draw' };
  if (!ENGINE_TREE && !icd) return { checked: 0, cached: 0, skipped: 'lavapipe (mesa-vulkan-drivers) is not installed' };
  if (!icd) throw new Error('wgsl-check: lavapipe (Mesa software Vulkan, mesa-vulkan-drivers) is not installed: cannot check the WGSL Tint gets');
  const r = spawnSync(process.execPath, [fileURLToPath(import.meta.url), '--child', dir, ...states], {
    encoding: 'utf8', timeout: 1800000, maxBuffer: 64 << 20,
    // software Vulkan only (no hardware GPU, no display), two threads: a background check on a shared machine
    env: { ...process.env, CW_THREE_DIR: three, VK_ICD_FILENAMES: icd, VK_DRIVER_FILES: icd, LP_NUM_THREADS: '2', DISPLAY: '', WAYLAND_DISPLAY: '' },
  });
  const out = (r.stdout || '').trim();
  const last = out.split('\n').filter((l) => l.startsWith('{')).pop();
  const res = last ? JSON.parse(last) : null;
  if (!res) throw new Error(`wgsl-check: the checker died (${r.signal || 'exit ' + r.status})\n${(r.stderr || '').slice(-2000)}`);
  if (res.failed.length) {
    const lines = res.failed.map((f) => `  ${f.state}: ${f.message.trim().split('\n').join('\n    ')}${hint(f.message)}`);
    throw new Error(`wgsl-check: Tint (Dawn's WGSL compiler) rejected ${res.failed.length} program(s) in ${dir}:\n${lines.join('\n')}`);
  }
  return { checked: res.checked, cached: res.cached };
}

function hint(msg) {
  if (/over the device limits/.test(msg))
    return '\n    (three.c binds one vertex buffer per geometry attribute plus the instance matrix and the instance colour:'
      + ' drop an attribute the material does not need, e.g. vertex colours or tangents; see docs/guides/rendering-3d.md, Program tables)';
  if (/swizzle view instruction still has usages/.test(msg))
    return '\n    (a known Tint internal error: a swizzle of a swizzle on a variable, e.g. v.xy.x. In TSL, swizzle the whole'
      + ' value once (k.x, k.y) or combine with dot(); see docs/guides/rendering-3d.md, Program tables)';
  return '';
}

// ── the child: compiles on Dawn ──────────────────────────────────────
async function child(dir, states) {
  const model = join(threeDir(), 'tools', 'program-model.mjs');
  const { vertexBufferLayout } = existsSync(model) ? await import(pathToFileURL(model).href) : {};
  if (typeof vertexBufferLayout !== 'function')
    throw new Error(`wgsl-check: ${model} has no vertexBufferLayout (three.c v0.186.21 or later): the check needs three.c's vertex buffer layout`);
  const { createGPU, globals } = await import(pathToFileURL(join(webgpuNode(), 'index.mjs')).href);
  Object.assign(globalThis, globals);
  const gpu = createGPU(['backend=vulkan']);
  // (a new Dawn compiles every program again: its native-dawn version and Dawn revision are in the stamp)
  const nd = join(webgpuNode(), '..', 'native-dawn');
  const dawnLib = ['package.json', 'upstream.json'].map((f) => (existsSync(join(nd, f)) ? readFileSync(join(nd, f), 'utf8') : '')).join('');
  const devices = {};
  const device = async (compat) => {
    const k = compat ? 'compat' : 'core';
    if (!devices[k]) {
      const adapter = await gpu.requestAdapter({ featureLevel: compat ? 'compatibility' : 'core' });
      if (!adapter) throw new Error(`no ${k} lavapipe adapter`);
      if (!/llvmpipe|lavapipe/i.test(`${adapter.info.device} ${adapter.info.description}`))
        throw new Error(`wgsl-check runs on lavapipe only, got "${adapter.info.description}"`);
      devices[k] = await adapter.requestDevice({ requiredFeatures: [...adapter.features].filter((f) => f !== 'chromium-experimental-timestamp-query-inside-passes') });
    }
    return devices[k];
  };
  const failed = [];
  let checked = 0, cached = 0;
  for (const st of states) {
    const vert = readFileSync(join(dir, st + '.wgpu.vert'), 'utf8');
    const frag = existsSync(join(dir, st + '.wgpu.frag')) ? readFileSync(join(dir, st + '.wgpu.frag'), 'utf8') : null;
    const json = JSON.parse(readFileSync(join(dir, st + '.wgpu.json'), 'utf8'));
    const stamp = join(dir, st + '.wgpu.tint');
    // vertex buffers as three.c binds them (one per geometry attribute, the instance matrix columns in one)
    const buffers = vertexBufferLayout(json.attributes || []);
    const hash = createHash('sha256').update(vert).update('\0').update(frag || '').update('\0' + dawnLib).update('\0' + JSON.stringify(buffers)).digest('hex');
    if (existsSync(stamp) && readFileSync(stamp, 'utf8').trim() === hash) { cached++; continue; }
    const dev = await device((json.features || []).includes('cm'));
    // the device's limits first: Dawn's own message for a pipeline over them names no attribute
    const nattr = buffers.reduce((n, b) => n + b.attributes.length, 0);
    if (buffers.length > dev.limits.maxVertexBuffers || nattr > dev.limits.maxVertexAttributes) {
      const names = (json.attributes || []).map((a) => a.name).join(' ');
      failed.push({ state: st, message: `over the device limits: ${buffers.length} vertex buffers (max ${dev.limits.maxVertexBuffers}), `
        + `${nattr} vertex attributes (max ${dev.limits.maxVertexAttributes}): ${names}` });
      checked++;
      continue;
    }
    dev.pushErrorScope('internal');
    dev.pushErrorScope('validation');
    const messages = [];
    const mod = (code, stage) => {
      const m = dev.createShaderModule({ code, label: `${st} ${stage}` });
      return m.getCompilationInfo().then((ci) => { for (const x of ci.messages) if (x.type === 'error') messages.push(`${stage} ${x.lineNum}:${x.linePos}: ${x.message}`); return m; });
    };
    const vm = await mod(vert, 'vertex');
    const fm = frag ? await mod(frag, 'fragment') : null;
    if (!messages.length) {
      const outs = frag ? [...(/struct OutputStruct\s*\{([\s\S]*?)\}/.exec(frag)?.[1] || '').matchAll(/@location\(\s*(\d+)\s*\)\s*\w+\s*:\s*([\w<>]+)/g)] : [];
      const targets = [];
      for (const [, loc, type] of outs) targets[+loc] = { format: /u32/.test(type) ? 'rgba32uint' : /i32/.test(type) ? 'rgba32sint' : 'rgba16float' };
      for (let i = 0; i < targets.length; i++) targets[i] ??= null;
      try {
        await dev.createRenderPipelineAsync({
          label: st, layout: 'auto',
          vertex: { module: vm, entryPoint: 'main', buffers },
          ...(fm ? { fragment: { module: fm, entryPoint: 'main', targets: targets.length ? targets : [{ format: 'rgba16float' }] } } : {}),
          depthStencil: { format: 'depth24plus', depthWriteEnabled: true, depthCompare: 'less' },
          primitive: { topology: 'triangle-list' },
        });
      } catch (e) { messages.push(e.message); }
    }
    for (const e of [await dev.popErrorScope(), await dev.popErrorScope()]) if (e) messages.push(e.message);
    checked++;
    if (messages.length) failed.push({ state: st, message: [...new Set(messages)].join('\n') });
    else writeFileSync(stamp, hash + '\n');
  }
  console.log(JSON.stringify({ checked, cached, failed }));
  for (const d of Object.values(devices)) d.destroy?.();
  process.exit(0);
}

const argv = process.argv.slice(2);
if (argv[0] === '--child') await child(resolve(argv[1]), argv.length > 2 ? argv.slice(2) : capturedStates(resolve(argv[1])));
else if (import.meta.url === pathToFileURL(process.argv[1] || '').href) {
  if (!argv[0]) { console.error('usage: node tools/lib/wgsl-check.mjs <capture dir> [state ...]'); process.exit(2); }
  try {
    const r = checkWgsl(resolve(argv[0]), argv.length > 1 ? argv.slice(1) : undefined);
    console.log(`wgsl-check: ${r.checked} compiled, ${r.cached} unchanged since they last passed: Tint accepts every program`);
  } catch (e) { console.error(e.message); process.exit(1); }
}
