// There are no GL-only carts: every .wasc a build makes is the dual GL + WebGPU
// cart (it renders with WebGPU on a WebGPU host and with GL on a GL host).
//
//   1. every cart in the cart trees (build/*/carts, and a project's
//      .cwbuild/*/carts with CW_PROJECT_ROOT) imports both the "gl" module and
//      the WebGPU tier's functions (deps/wasmcart-wgpu host.js isWgpuImportName);
//      a cart with GL and no WebGPU is a GL-only cart and fails, naming it;
//   2. the build refuses to make one: a cart tree configured with CW_WGPU=OFF
//      stops at configure (cmake/deps.cmake), and tools/check-imports.mjs,
//      which every cart passes before it is packed, fails without --wgpu;
//   3. the project editor cart (tools/cwproject.mjs build <dir> editor) is
//      built in the dual tree or refused, never in a GL tree.
// CONTROLS that must fail: a GL-only cart made here (a module importing only
// gl.glClear, packed with wasmcart-pack) is flagged by check 1, and
// check-imports passes it only when told --wgpu would not be given.
//
// The GL_ONLY apps (cw_add_app ... GL_ONLY: they draw with GL themselves and
// get no cart at all) are listed: only test binaries that inspect GL by
// design belong there.
//
//   node tests/no-gl-only-carts.mjs
import { spawnSync } from 'node:child_process';
import { readFileSync, readdirSync, existsSync, mkdirSync, writeFileSync, statSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PROJ = process.env.CW_PROJECT_ROOT ? resolve(process.env.CW_PROJECT_ROOT) : ROOT;
const OUT = join(PROJ, 'out', 'no-gl-only-carts');
mkdirSync(OUT, { recursive: true });
const WGPU_HOST = join(ROOT, 'deps', 'wasmcart-wgpu', 'src', 'wgpu', 'host.js');
const { unzipSync } = await import(pathToFileURL(join(ROOT, 'deps', 'wasmcart-wgpu', 'node_modules', 'fflate', 'esm', 'index.mjs')).href);
const { isWgpuImportName } = await import(pathToFileURL(WGPU_HOST).href);
let failed = 0;
const fail = (m) => { failed++; console.log(`FAIL ${m}`); };

/** what a cart is: 'dual', 'gl-only', 'no-gpu' (neither API), or an error
 * @param {string} file */
function kindOf(file) {
  let files;
  try { files = unzipSync(new Uint8Array(readFileSync(file)), { filter: (f) => f.name === 'cart.wasm' }); } catch (e) { return `unreadable (${e.message})`; }
  const wasm = files['cart.wasm'];
  if (!wasm) return 'no cart.wasm';
  const imports = WebAssembly.Module.imports(new WebAssembly.Module(wasm));
  const gl = imports.some((i) => i.module === 'gl');
  const wgpu = imports.some((i) => i.module === 'env' && isWgpuImportName(i.name));
  return gl && wgpu ? 'dual' : gl ? 'gl-only' : wgpu ? 'webgpu-only' : 'no-gpu';
}

/** every .wasc under <dir>/<tree>/carts for the build trees of dir
 * @param {string} base */
function cartsUnder(base) {
  const out = [];
  if (!existsSync(base)) return out;
  for (const tree of readdirSync(base)) {
    const d = join(base, tree, 'carts');
    if (!existsSync(d) || !statSync(d).isDirectory()) continue;
    for (const f of readdirSync(d)) if (f.endsWith('.wasc')) out.push(join(d, f));
  }
  return out;
}

// ── 1. the carts the builds made ──
const carts = [...cartsUnder(join(PROJ, 'build')), ...cartsUnder(join(PROJ, '.cwbuild'))];
if (!carts.length) fail(`no carts under ${relative(process.cwd(), join(PROJ, 'build'))}/*/carts: run scripts/build.sh first (nothing checked is not a pass)`);
const bad = [];
for (const c of carts) {
  const k = kindOf(c);
  if (k !== 'dual' && k !== 'no-gpu') bad.push(`${relative(PROJ, c)}: ${k}`);
}
if (bad.length) fail(`${bad.length} cart(s) that are not dual GL + WebGPU carts:\n  ${bad.join('\n  ')}`);
else if (carts.length) console.log(`ok   ${carts.length} carts, every one a dual GL + WebGPU cart`);

// ── control: a GL-only cart must be caught ──
// a module importing gl.glClear (i32) and nothing else, packed as a cart
const glOnlyWasm = Uint8Array.from([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // magic, version
  0x01, 0x05, 0x01, 0x60, 0x01, 0x7f, 0x00, // type: (i32) -> ()
  0x02, 0x0e, 0x01, 0x02, 0x67, 0x6c, 0x07, 0x67, 0x6c, 0x43, 0x6c, 0x65, 0x61, 0x72, 0x00, 0x00, // import gl.glClear
]);
const ctlWasm = join(OUT, 'gl_only_control.wasm'), ctlCart = join(OUT, 'gl_only_control.wasc');
writeFileSync(ctlWasm, glOnlyWasm);
const pack = spawnSync('node', [join(ROOT, 'deps', 'wasmcart-wgpu', 'bin', 'wasmcart-pack.js'), '--wasm', ctlWasm, '--name', 'gl_only_control',
  '--version', '0.1.0', '--output', ctlCart], { encoding: 'utf8' });
if (pack.status !== 0) fail(`control: wasmcart-pack could not pack the GL-only control cart: ${(pack.stderr || pack.stdout).trim().split('\n').pop()}`);
else {
  const k = kindOf(ctlCart);
  if (k !== 'gl-only') fail(`CONTROL PASSED: a cart importing only gl.glClear reads as '${k}', not gl-only: the cart check cannot see a GL-only cart`);
  else console.log('ok   control: a GL-only cart is caught (gl-only)');
}

// ── 2. the build refuses to make one ──
const ci = (args) => spawnSync('node', [join(ROOT, 'tools', 'check-imports.mjs'), ...args], { encoding: 'utf8' });
const noWgpu = ci([ctlWasm]);
if (noWgpu.status === 0 || !/no GL-only carts/.test(noWgpu.stderr)) fail(`check-imports passed a cart checked without --wgpu (exit ${noWgpu.status}): ${noWgpu.stderr.trim()}`);
else console.log('ok   check-imports refuses a cart checked without --wgpu (a GL-only cart is never packed)');
const glOnlyDual = ci([ctlWasm, '--wgpu', WGPU_HOST]);
if (glOnlyDual.status === 0) fail('check-imports --wgpu passed a module with no WebGPU imports');
else console.log('ok   check-imports --wgpu refuses a module with GL and no WebGPU imports');
if (PROJ === ROOT) {
  const cfgDir = join(OUT, 'cfg-gl-cart');
  const cfg = spawnSync('cmake', ['-S', ROOT, '-B', cfgDir, '-G', 'Ninja', `-DCMAKE_TOOLCHAIN_FILE=${join(ROOT, 'cmake', 'cart.cmake')}`, '-DCW_WGPU=OFF'],
    { encoding: 'utf8' });
  const said = /there are no GL-only carts/.test(cfg.stdout + cfg.stderr);
  if (cfg.status === 0 || !said) fail(`a cart tree configured with CW_WGPU=OFF was not refused (exit ${cfg.status})${said ? '' : ': no "there are no GL-only carts" message'}`);
  else console.log('ok   a cart tree with CW_WGPU=OFF stops at configure');
}

// ── 3. the project editor cart: the dual tree or refused ──
const cwp = readFileSync(join(ROOT, 'tools', 'cwproject.mjs'), 'utf8');
if (/editor-cart/.test(cwp) || !/buildTarget\(dir, 'cart', proj, `\$\{proj\.app\}_editor`\)/.test(cwp))
  fail('tools/cwproject.mjs: the project editor cart is not built in the dual tree (cart-wgpu)');
else console.log('ok   the project editor cart builds in the dual tree (cart-wgpu) or is refused');

// ── the GL_ONLY apps (no cart at all) ──
const glOnlyApps = [];
const walk = (d) => {
  for (const f of readdirSync(d, { withFileTypes: true })) {
    if (['deps', 'build', 'out', 'node_modules', '.git', 'worktrees', '.cwbuild'].includes(f.name)) continue;
    const p = join(d, f.name);
    if (f.isDirectory()) walk(p);
    else if (f.name === 'CMakeLists.txt' || f.name.endsWith('.cmake'))
      for (const m of readFileSync(p, 'utf8').matchAll(/cw_add_app\(([^\s)]+)[^)]*\bGL_ONLY\)/g)) glOnlyApps.push(`${m[1]} (${relative(PROJ, p)})`);
  }
};
walk(PROJ);
console.log(`note GL_ONLY apps (native only, no cart): ${glOnlyApps.length ? glOnlyApps.join(', ') : 'none'}`);

console.log(failed ? `no-gl-only-carts: ${failed} failure(s)` : 'no-gl-only-carts: ok');
process.exit(failed ? 1 : 0);
