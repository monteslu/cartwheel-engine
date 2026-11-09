// A dev cart's fast pack (tools/cart-version.mjs pack --reuse-assets, cmake/app.cmake with CW_CART_DEV):
// after a code change only cart.wasm is new, so it is spliced into the last archive
// (tools/lib/wasc-splice.mjs) instead of every asset being deflated again. The archive must be the one
// a fresh pack gives: the same entries in the same order with the same bytes.
//
//   1. a first pack with --reuse-assets packs in full (no archive to reuse);
//   2. a new cart.wasm (a stored one, 64 KiB, and a deflated one, 5 MiB: wasmcart-pack's 4 MiB rule)
//      is spliced in: the archive equals a fresh wasmcart-pack of the same inputs entry for entry,
//      `unzip -t` accepts it, and the pack took the splice path;
//   3. an asset changed (same size), an asset added, the version moved: each packs in full and the
//      archive equals a fresh pack.
// CONTROLS that must fail: the comparison against a fresh pack, given an archive spliced with a stale
// asset (the splice forced past a changed asset), finds the difference; and a splice of the old wasm
// differs from the fresh pack of the new one.
//
//   node tests/cart-repack.mjs
import { spawnSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, utimesSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { randomBytes } from 'node:crypto';
import { readEntries, spliceWasm } from '../tools/lib/wasc-splice.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'cart-repack');
const TOOL = join(ROOT, 'tools', 'cart-version.mjs');
const PACK = join(ROOT, 'deps', 'wasmcart-wgpu', 'bin', 'wasmcart-pack.js');
if (!existsSync(PACK)) { console.log(`FAIL no ${PACK}: run scripts/fetch-deps.sh`); process.exit(1); }
rmSync(OUT, { recursive: true, force: true });
mkdirSync(join(OUT, 'assets', 'models'), { recursive: true });
let failed = 0;
const fail = (m) => { failed++; console.log(`FAIL ${m}`); };
const ok = (m) => console.log(`ok   ${m}`);

// a fake cart: compressible models and sounds, a small config, a cart.wasm of either size
const seeded = (n, seed) => { const b = Buffer.alloc(n); let x = seed; for (let i = 0; i < n; i++) { x = (x * 1103515245 + 12345) >>> 0; b[i] = (x >>> 24) & 0x3f; } return b; };
writeFileSync(join(OUT, 'assets', 'models', 'hero.glb'), seeded(3 << 20, 1));
writeFileSync(join(OUT, 'assets', 'models', 'stage.glb'), seeded(1 << 20, 2));
writeFileSync(join(OUT, 'assets', 'input.cfg'), 'jump = pad:a key:space\n');
const WASM = join(OUT, 'game.wasm'), VER = join(OUT, 'game.version'), WASC = join(OUT, 'game.wasc'), REC = join(OUT, 'game.pack.json');
const packCmd = ['node', PACK, '--wasm', WASM, '--name', 'game', '--assets', join(OUT, 'assets')];
const pack = (verbose = true) => spawnSync(process.execPath, [TOOL, 'pack', '--version-file', VER, '--wasc', WASC, '--record', REC, '--reuse-assets', '--', ...packCmd],
  { encoding: 'utf8', env: { ...process.env, ...(verbose ? { CW_CART_PACK_VERBOSE: '1' } : {}) } });
const fresh = (version, out) => spawnSync('node', [...packCmd.slice(1), '--version', version, '--output', out], { encoding: 'utf8' });
const same = (a, b) => {
  const x = readEntries(a), y = readEntries(b);
  if (x.length !== y.length) return `${x.length} entries against ${y.length}`;
  for (let i = 0; i < x.length; i++) {
    if (x[i].name !== y[i].name) return `entry ${i} is ${x[i].name}, not ${y[i].name}`;
    // the manifest's bytes too: the version is in it
    if (!x[i].data.equals(y[i].data)) return `${x[i].name} differs`;
  }
  return '';
};
const unzipOk = (f) => { const r = spawnSync('unzip', ['-tq', f], { encoding: 'utf8' }); return r.error ? 'skipped (no unzip)' : r.status === 0 ? '' : r.stdout + r.stderr; };
const step = (what, wantSplice, version) => {
  const r = pack();
  if (r.status !== 0) return fail(`${what}: pack failed: ${r.stderr}`);
  const spliced = /spliced in/.test(r.stdout);
  if (spliced !== wantSplice) return fail(`${what}: ${spliced ? 'spliced' : 'packed in full'}, expected ${wantSplice ? 'a splice' : 'a full pack'} (${r.stdout.trim()})`);
  const ref = join(OUT, 'fresh.wasc');
  const f = fresh(version, ref);
  if (f.status !== 0) return fail(`${what}: fresh pack failed: ${f.stderr}`);
  const d = same(WASC, ref), z = unzipOk(WASC);
  if (d) return fail(`${what}: not the fresh pack's archive: ${d}`);
  if (z && !z.startsWith('skipped')) return fail(`${what}: unzip -t: ${z}`);
  ok(`${what}: ${wantSplice ? 'spliced' : 'packed in full'}, the same ${readEntries(WASC).length} entries as a fresh pack${z ? ' (unzip ' + z + ')' : ', unzip -t ok'}`);
};

writeFileSync(VER, '1.0.0\n');
writeFileSync(WASM, randomBytes(64 << 10));
step('first pack', false, '1.0.0');
writeFileSync(WASM, randomBytes(64 << 10));
step('new stored cart.wasm', true, '1.0.0');
const small = join(OUT, 'small.wasm');
copyFileSync(WASM, small);
writeFileSync(WASM, Buffer.concat([randomBytes(1 << 20), seeded(4 << 20, 9)]));
step('new deflated cart.wasm (over 4 MiB)', true, '1.0.0');

// controls: the comparison must catch a wrong archive
const stale = join(OUT, 'stale.wasc');
const hero = join(OUT, 'assets', 'models', 'hero.glb');
writeFileSync(hero, seeded(3 << 20, 77)); // the same size, other bytes
utimesSync(hero, new Date(2001, 0, 1), new Date(2001, 0, 1));
if (spliceWasm(WASC, WASM, stale) !== true) fail('control: forced splice failed');
fresh('1.0.0', join(OUT, 'fresh.wasc'));
const dc = same(stale, join(OUT, 'fresh.wasc'));
if (dc) ok(`control: an archive spliced past a changed asset is caught (${dc})`);
else fail('CONTROL PASSED: the comparison cannot see a stale asset');
const oldWasm = join(OUT, 'old.wasc');
if (spliceWasm(WASC, small, oldWasm) !== true) fail('control: splice of the old wasm failed');
const dw = same(oldWasm, join(OUT, 'fresh.wasc'));
if (dw) ok(`control: a splice of the old cart.wasm is caught (${dw})`);
else fail('CONTROL PASSED: the comparison cannot see a different cart.wasm');

// what must pack in full
step('an asset changed (same size, older time)', false, '1.0.0');
writeFileSync(join(OUT, 'assets', 'models', 'extra.glb'), seeded(2 << 20, 5));
step('an asset added', false, '1.0.0');
writeFileSync(VER, '1.0.1\n');
step('the version moved', false, '1.0.1');
writeFileSync(WASM, randomBytes(64 << 10));
step('then a code change again', true, '1.0.1');

rmSync(OUT, { recursive: true, force: true });
console.log(failed ? `${failed} failed` : 'all ok');
process.exit(failed ? 1 : 0);
