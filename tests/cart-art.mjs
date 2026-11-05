// Cart art (wasmcart's manifest `art`, `description`, `author`): what
// cw_add_app packs for launchers.
//
//   1. project.cwproj's `art`, `description` and `author` keys parse as
//      cw_add_app reads them (cmake/app.cmake cw__project_value), quotes,
//      commas and an apostrophe kept; `artist` is not `art`;
//   2. the pack tool packs tests/fixtures/cart-art: every picture under art/,
//      the manifest naming each, a screenshot's label, the credits, and
//      wasmcart's describeArtProblems finds nothing wrong;
//   3. built carts: hello.wasc (cw_add_app ART/DESCRIPTION/AUTHOR) carries
//      the same, and a sample without ART carries no art.
// CONTROL that must fail: a cover of the wrong shape is reported by
// describeArtProblems.
//
//   node tests/cart-art.mjs [--pack <wasmcart-pack.js>] [--carts <dir>]
//
// --pack checks another pack tool (an SDK's, scripts/sdk-link-check.sh
// cart-wgpu); --carts another cart tree (default build/cart-wgpu/carts,
// skipped when it is not built).
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? resolve(argv[i + 1]) : d; };
const PACK = opt('--pack', join(ROOT, 'deps', 'wasmcart-wgpu', 'bin', 'wasmcart-pack.js'));
const CARTS = opt('--carts', join(ROOT, 'build', 'cart-wgpu', 'carts'));
const FIX = join(ROOT, 'tests', 'fixtures', 'cart-art');
const OUT = join(ROOT, 'out', 'cart-art');
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
// the checks the pack tool itself imports, from beside it (an SDK ships them there)
const { describeArtProblems } = await import(pathToFileURL(join(dirname(PACK), '..', 'src', 'cartArt.js')).href);
let failed = 0;
const fail = (m) => { failed++; console.log(`FAIL ${m}`); };
const ok = (m) => console.log(`ok   ${m}`);

/** a .wasc's manifest and a reader for its files (unzip: an SDK ships no zip reader) */
function openCart(file) {
  const read = (p) => {
    const r = spawnSync('unzip', ['-p', file, p], { maxBuffer: 1 << 28 });
    return r.status === 0 && r.stdout.length ? new Uint8Array(r.stdout) : null;
  };
  const m = read('manifest.json');
  return { manifest: m ? JSON.parse(Buffer.from(m).toString('utf8')) : null, read };
}

const fixture = JSON.parse(readFileSync(join(FIX, 'art.json'), 'utf8'));
const DESC = 'A test cart, with commas, an apostrophe\'s mark and (brackets).';
/** the cart's manifest names every fixture picture under art/, each in the archive and sound */
function checkArt(what, cart, desc, author) {
  const before = failed;
  const { manifest, read } = openCart(cart);
  if (!manifest) return fail(`${what}: no manifest.json in ${cart}`);
  const art = manifest.art;
  if (!art) return fail(`${what}: the manifest has no art`);
  for (const k of ['icon', 'cover', 'background', 'logo', 'title']) {
    if (art[k] !== 'art/' + fixture[k]) fail(`${what}: art.${k} is ${art[k]}, not art/${fixture[k]}`);
  }
  const shot = art.screenshots && art.screenshots[0];
  if (!shot || shot.path !== 'art/' + fixture.screenshots[0].path || shot.label !== fixture.screenshots[0].label) {
    fail(`${what}: the screenshot is ${JSON.stringify(shot)}`);
  }
  const problems = describeArtProblems(art, read);
  if (problems.length) fail(`${what}: describeArtProblems: ${problems.join('; ')}`);
  if (manifest.description !== desc) fail(`${what}: description is ${JSON.stringify(manifest.description)}`);
  if (manifest.author !== author) fail(`${what}: author is ${JSON.stringify(manifest.author)}`);
  if (failed === before) ok(`${what}: art, description and author packed, no art problems`);
}

// 1. project.cwproj keys as cw_add_app reads them
{
  const script = join(OUT, 'read-keys.cmake');
  writeFileSync(script, `include(${join(ROOT, 'cmake', 'app.cmake')})\n` +
    'foreach(k app art description author)\n  cw__project_value(v ${k})\n  message(STATUS "${k}=[${v}]")\nendforeach()\n');
  const r = spawnSync('cmake', ['-P', script], { cwd: join(ROOT, 'tests', 'fixtures', 'cart-art-project'), encoding: 'utf8' });
  const got = Object.fromEntries([...(r.stdout + r.stderr).matchAll(/-- (\w+)=\[(.*)\]/g)].map((m) => [m[1], m[2]]));
  const want = { app: 'arttest', art: '../cart-art', description: DESC, author: 'The cartwheel authors' };
  const bad = Object.keys(want).filter((k) => got[k] !== want[k]);
  if (r.status !== 0 || bad.length) fail(`project keys: ${bad.map((k) => `${k} read as ${JSON.stringify(got[k])}`).join(', ') || r.stderr}`);
  else ok('project.cwproj art, description and author read as cw_add_app reads them');
}

// 2. the pack tool packs the fixture
{
  const wasm = join(OUT, 'cart.wasm');
  writeFileSync(wasm, Buffer.from([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]));
  const cart = join(OUT, 'arttest.wasc');
  const r = spawnSync('node', [PACK, '--wasm', wasm, '--name', 'arttest', '--art', FIX, '--description', DESC,
    '--author', 'The cartwheel authors', '--output', cart], { encoding: 'utf8' });
  if (r.status !== 0) fail(`pack: ${(r.stderr || r.stdout).trim()}`);
  else if (/art /.test(r.stderr)) fail(`pack warned about the fixture's art: ${r.stderr.trim()}`);
  else checkArt('pack --art', cart, DESC, 'The cartwheel authors');
}

// control: a cover of the wrong shape must be reported
{
  const square = readFileSync(join(FIX, 'icon.png'));
  const problems = describeArtProblems({ cover: 'art/cover.png' }, (p) => (p === 'art/cover.png' ? new Uint8Array(square) : null));
  if (!problems.some((p) => /cover/.test(p))) fail('control: a square cover was not reported');
  else ok('control: a square cover is reported');
}

// 3. built carts
if (existsSync(join(CARTS, 'hello.wasc'))) {
  const before = failed;
  checkArt('hello.wasc', join(CARTS, 'hello.wasc'),
    'A spinning cube that moves with the stick and plays a tone while the south button is held.', 'The cartwheel authors');
  if (failed === before) {
    const other = openCart(join(CARTS, 'vertex_colors.wasc')).manifest;
    if (!other) fail('vertex_colors.wasc: no manifest');
    else if (other.art || other.description || other.author) fail('vertex_colors.wasc (no ART) carries art or credits');
    else ok('a sample without ART carries no art');
  }
} else {
  console.log(`skip built carts: no ${join(CARTS, 'hello.wasc')}`);
}
process.exit(failed ? 1 : 0);
