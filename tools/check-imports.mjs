// Fail if a cart imports anything outside the wasmcart contract: the "gl"
// module, wasmcart's env functions (wc_*), WASI, and wasi.thread-spawn.
// An unexpected import means a symbol was never linked (a dropped object
// file), or a library reached for something a cart cannot have.
//
//   node tools/check-imports.mjs cart.wasm --wgpu <wasmcart>/src/wgpu/host.js
//
// --wgpu: a dual (GL + WebGPU) cart. Its link uses --allow-undefined (the
// WebGPU functions are host imports), which would also turn a dropped symbol
// into an import, so the WebGPU names are checked against the tier's own
// definition (host.js isWgpuImportName) and every other env import still
// fails. A dual cart must import both APIs.
//
// Every cart is a dual one: without --wgpu the check fails (a GL-only cart is
// never packed; tests/no-gl-only-carts.mjs checks the carts a build made).
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
const path = process.argv[2];
const wi = process.argv.indexOf('--wgpu');
if (wi < 0) {
  console.error(`${path}: no --wgpu: a GL-only cart. There are no GL-only carts: build the dual cart (CW_WGPU=ON, build/cart-wgpu)`);
  process.exit(1);
}
const host = await import(pathToFileURL(process.argv[wi + 1]).href);
const mod = new WebAssembly.Module(readFileSync(path));
const wgpuName = (n) => host && (host.isWgpuImportName(n) || n === 'emscripten_has_asyncify');
const ok = (i) =>
  i.module === 'gl' ||
  i.module === 'wasi_snapshot_preview1' ||
  (i.module === 'wasi' && i.name === 'thread-spawn') ||
  (i.module === 'env' && (/^wc_/.test(i.name) || i.name === 'memory' || (i.kind === 'function' && wgpuName(i.name))));
const imports = WebAssembly.Module.imports(mod);
const bad = imports.filter((i) => !ok(i));
if (bad.length) {
  console.error(`${path}: unexpected imports:\n` + bad.map((i) => `  ${i.module}.${i.name} (${i.kind})`).join('\n'));
  process.exit(1);
}
if (host && !(imports.some((i) => i.module === 'gl') && imports.some((i) => i.module === 'env' && host.isWgpuImportName(i.name)))) {
  console.error(`${path}: a dual cart must import both gl and WebGPU`);
  process.exit(1);
}
