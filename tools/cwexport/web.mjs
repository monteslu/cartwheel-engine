// Web: a static site (index.html + player.js, wasmcart's web host, the cart)
// with the COOP/COEP headers documented and shipped (_headers, serve.mjs); the
// page checks crossOriginIsolated and explains a failure. The cart is the dual
// one (WebGPU where the browser has it, else WebGL 2), split in two: game.wasm
// (its code, compiled while it downloads and cached compiled by the browser:
// WebAssembly.compileStreaming) and game.wasc (the rest).
// Also the .wasc cart export (platform "wasc"), the same dual cart on its own.
import { readFileSync, existsSync } from 'node:fs';
import { join, posix } from 'node:path';
import { buildCart, write, freshDir, copy, zipDir, slug, run, ROOT, ExportError } from './util.mjs';
import { writeIcon } from './icon.mjs';

const TEMPLATE = join(ROOT, 'tools/cwexport/templates/web');
const WC = join(ROOT, 'deps/wasmcart');
// what CartHostWeb loads: src/CartHostWeb.js, its worker (cartWorkerWeb.js) and every file they
// reach through relative static or dynamic imports (wasmcart 0.32 adds jit.js, actions.js,
// debugState.js, cartMemory.js and the WebGPU host in wgpu/), walked from the sources so a
// wasmcart that adds a module cannot ship a page that 404s on it
const WC_ROOTS = ['CartHostWeb.js', 'cartWorkerWeb.js'];
export function wcFiles(wc = WC) {
  const seen = new Set(), todo = [...WC_ROOTS];
  while (todo.length) {
    const f = todo.pop();
    if (seen.has(f)) continue;
    const p = join(wc, 'src', f);
    if (!existsSync(p)) throw new ExportError(`${wc}/src/${f} is missing: run scripts/fetch-deps.sh`);
    seen.add(f);
    for (const m of readFileSync(p, 'utf8').matchAll(/(?:\bfrom\s+|\bimport\s*\(\s*)(['"])(\.{1,2}\/[^'"]+)\1/g))
      todo.push(posix.normalize(posix.join(posix.dirname(f), m[2])));
  }
  return [...seen].sort();
}

export function validate(ctx) {
  wcFiles();
  if (!existsSync(join(WC, 'node_modules/fflate/esm/browser.js'))) throw new ExportError('deps/wasmcart/node_modules is missing: run scripts/fetch-deps.sh');
  if (!existsSync(join(ROOT, 'deps/wasi-sdk'))) throw new ExportError('deps/wasi-sdk is missing: run scripts/fetch-deps.sh');
}

const coiShim = (ctx) => ctx.opt('coi_shim', 'false') === 'true';
const fill = (text, ctx) => text.replaceAll('@TITLE@', ctx.title.replace(/</g, '&lt;')).replaceAll('@VERSION@', ctx.version)
  .replaceAll('@COI_SHIM@', coiShim(ctx) ? 'on' : 'off');

export async function runWeb(ctx) {
  const cart = await buildCart(ctx);
  const name = slug(ctx.app), top = `${name}-${ctx.version}-web`;
  const site = freshDir(join(ctx.outDir, top));
  ctx.progress(60, 'site');
  for (const f of ['index.html', 'player.js', 'pads.js', 'serve.mjs', '_headers', 'README.txt', ...(coiShim(ctx) ? ['coi-sw.js'] : [])])
    write(join(site, f), fill(readFileSync(join(TEMPLATE, f), 'utf8'), ctx));
  // wasmcart's web host, with its one bare import ("fflate") made relative so
  // the page and its module workers need no import map
  for (const f of wcFiles()) {
    const fflate = posix.relative(posix.dirname(f), 'fflate.js').replace(/^(?!\.)/, './');
    write(join(site, 'wasmcart', f), readFileSync(join(WC, 'src', f), 'utf8').replace(/(from\s+|import\()(['"])fflate\2/g, `$1$2${fflate}$2`));
  }
  copy(join(WC, 'node_modules/fflate/esm/browser.js'), join(site, 'wasmcart/fflate.js'));
  copy(join(WC, 'LICENSE'), join(site, 'wasmcart/LICENSE'));
  copy(join(WC, 'node_modules/fflate/LICENSE'), join(site, 'wasmcart/fflate.LICENSE'));
  await splitCart(cart.binary, join(site, 'game.wasm'), join(site, 'game.wasc'));
  writeIcon(ctx, join(site, 'icon.png'));
  ctx.progress(85, 'zip');
  return [await zipDir(site, top, join(ctx.outDir, `${top}.zip`)), site];
}

// The cart's wasm (its manifest's entry) to wasmFile, the rest of the archive
// to wascFile: the page fetches the wasm on its own, so the browser compiles
// it as it streams in and keeps the compiled code (CartHostWeb load's `wasm`).
export async function splitCart(cart, wasmFile, wascFile) {
  let entry = 'cart.wasm';
  try {
    const m = JSON.parse(await run('unzip', ['-p', cart, 'manifest.json'], { quiet: true }));
    if (m.entry) entry = m.entry;
  } catch { /* no manifest: the default entry */ }
  const { spawnSync } = await import('node:child_process');
  const wasm = spawnSync('unzip', ['-p', cart, entry], { maxBuffer: 1 << 30 });
  if (wasm.status !== 0 || !wasm.stdout.length) throw new ExportError(`${cart} has no ${entry}`);
  write(wasmFile, wasm.stdout);
  copy(cart, wascFile);
  await run('zip', ['-q', '-d', wascFile, entry], { quiet: true });
}

export async function runCart(ctx) {
  const cart = await buildCart(ctx);
  const out = join(ctx.outDir, `${slug(ctx.app)}-${ctx.version}.wasc`);
  copy(cart.binary, out);
  return [out];
}
