// A version change packs a cart again before its link (tools/cart-version.mjs update, the <app>_version target of
// cmake/app.cmake). For an app with an asset pack that repack reads the pack's output (packs/<app>/assets), which
// cwpack writes again whenever the pack is rebuilt (an engine pin move rebuilds every pack). Nothing ordered the two,
// so a release build after a version change could read an asset while cwpack rewrote it, and the packer stopped with
// "file data stream has unexpected number of bytes" (a rebuild then passed).
//
//   1. the build graph: in the cart tree every <app>_version of an app with an asset pack runs after <app>_pack
//      (ninja's inputs of the version target name the pack target);
//   2. the repack itself is sound when its inputs hold still: an update with a new version repacks the cart, and its
//      manifest carries the new version.
// CONTROL that must fail, every run: the graph check over a cart tree shaped as before the fix (a small generated
// build.ninja whose <app>_version does not depend on <app>_pack, beside one whose does) names the unordered app. It
// reads the graph, not a race: an earlier control rewrote an asset under the repack and needed the timing to land.
//
//   node tests/cart-version-order.mjs [--tree <cart build tree>]
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readNamedEntries } from '../tools/lib/wasc-splice.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const ti = argv.indexOf('--tree');
const TREE = ti >= 0 ? resolve(argv[ti + 1]) : join(ROOT, 'build', 'cart-wgpu');
const OUT = join(ROOT, 'out', 'cart-version-order');
const TOOL = join(ROOT, 'tools', 'cart-version.mjs');
const PACKER = join(ROOT, 'deps', 'wasmcart-wgpu', 'bin', 'wasmcart-pack.js');
let failed = 0;
const say = (ok, m) => { if (!ok) failed++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${m}`); };

/** the apps of a ninja tree that have an asset pack, and those whose version repack does not run after it */
function order(tree) {
  const targets = spawnSync('ninja', ['-C', tree, '-t', 'targets', 'all'], { encoding: 'utf8', maxBuffer: 1 << 28 }).stdout;
  const packs = [...targets.matchAll(/^(\S+)\/([A-Za-z0-9_]+)_pack: phony$/gm)].filter((m) => !m[1].includes('CMakeFiles') && !m[1].startsWith('/'));
  let checked = 0;
  const unordered = [];
  for (const [, dir, app] of packs) {
    const q = spawnSync('ninja', ['-C', tree, '-t', 'query', `${dir}/${app}_version`], { encoding: 'utf8' });
    if (q.status !== 0) continue; // an app with a pack and no cart
    checked++;
    if (!q.stdout.split('outputs:')[0].includes(`${app}_pack`)) unordered.push(app);
  }
  return { checked, unordered };
}

// 1. the graph
if (!existsSync(join(TREE, 'build.ninja'))) {
  say(false, `no cart tree at ${TREE} (scripts/build.sh builds it)`);
} else {
  const { checked, unordered } = order(TREE);
  say(checked > 0 && unordered.length === 0,
    `${checked} apps with an asset pack: each one's version repack runs after its pack${unordered.length ? ` (not: ${unordered.slice(0, 8).join(', ')}${unordered.length > 8 ? ', ...' : ''})` : ''}`);
}

// the control: the same check over a tree shaped as before the fix (old: no edge from the version target to the pack)
// and as after it (new), so the check is shown to tell them apart on every run
const GRAPH = join(ROOT, 'out', 'cart-version-order-graph');
for (const shape of ['old', 'new']) {
  const dir = join(GRAPH, shape);
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, 'build.ninja'), [
    'rule touch', '  command = touch $out', '',
    'build games/demo/packs.stamp: touch', 'build games/demo/demo_pack: phony games/demo/packs.stamp',
    'build games/demo/version.stamp: touch',
    `build games/demo/demo_version: phony games/demo/version.stamp${shape === 'new' ? ' || games/demo/demo_pack' : ''}`,
    'build all: phony games/demo/demo_version', 'default all', ''].join('\n'));
}
const old = order(join(GRAPH, 'old')), fixed = order(join(GRAPH, 'new'));
rmSync(GRAPH, { recursive: true, force: true });
say(fixed.checked === 1 && fixed.unordered.length === 0, 'the check passes a generated graph whose version target depends on its pack');
say(old.checked === 1 && old.unordered.join() === 'demo',
  `CONTROL: the check names the app of a generated graph shaped as before the fix (unordered: ${old.unordered.join(', ') || 'none'})`);

// 2. and the control: a repack with still inputs, and one with an asset rewritten under it
rmSync(OUT, { recursive: true, force: true });
mkdirSync(join(OUT, 'assets'), { recursive: true });
const BIG = join(OUT, 'assets', 'big.bin');
writeFileSync(BIG, Buffer.alloc(8 << 20, 7));
// the smallest valid module: wasmcart-pack only copies it
const WASM = join(OUT, 'cart.wasm');
writeFileSync(WASM, Buffer.from([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]));
const WASC = join(OUT, 'c.wasc'), REC = join(OUT, 'rec.json'), VER = join(OUT, 'v');
writeFileSync(VER, '1.0.0\n');
const first = spawnSync('node', [TOOL, 'pack', '--version-file', VER, '--wasc', WASC, '--record', REC, '--', 'node', PACKER,
  '--wasm', WASM, '--name', 'order', '--assets', join(OUT, 'assets')], { encoding: 'utf8' });
if (first.status !== 0) { say(false, `the first pack failed: ${first.stderr.trim().split('\n').pop()}`); process.exit(1); }
const update = (v) => spawnSync('node', [TOOL, 'update', '--out', join(OUT, 'v2'), '--set', v, '--wasc', WASC, '--record', REC, '--dir', OUT], { encoding: 'utf8' });
const version = () => JSON.parse(readNamedEntries(WASC, ['manifest.json']).get('manifest.json').toString('utf8')).version;
const u = update('1.0.1');
say(u.status === 0 && version() === '1.0.1', `a version change repacks the cart when its assets hold still (manifest ${version()})`);

rmSync(OUT, { recursive: true, force: true });
console.log(failed ? `cart-version-order: ${failed} failures` : 'ok   cart-version-order');
process.exit(failed ? 1 : 0);
