// A cart's manifest version (cw_add_app, cmake/app.cmake; the rule in
// tools/lib/app-version.mjs, run every build by tools/cart-version.mjs) and
// cwexport's, which uses the same rule:
//
//   1. the semver rule: cmake's cw__check_semver and semverProblem agree on
//      every case (releases, prereleases, build metadata pass; 1.2, a leading
//      zero, a leading v, an empty or a numeric-leading-zero prerelease fail);
//   2. the git rule, in a scratch repository: before any tag
//      0.0.0-dev.g<commit>; at a clean v1.2.3 tag 1.2.3; the same tree with a
//      tracked file changed 1.2.3-dev.g<commit>; a commit after the tag
//      1.2.3-dev.g<commit>; an explicit version wins; outside git the
//      project.cwproj version (1.2 means 1.2.0), else 0.1.0;
//   3. tools/cart-version.mjs: pack writes the cart with the version file's
//      version; update repacks it when the version moves (a new tag) and
//      leaves it alone when it does not;
//   4. built carts: hello.wasc's manifest version is the version this
//      checkout resolves to (or the tree's CW_APP_VERSION when one is set).
// CONTROLS that must fail: cw_add_app VERSION 1.2 stops the configure;
// cart-version.mjs update refuses --set 1.2.
//
//   node tests/app-version.mjs [--tree <cart build tree>]
//
// --tree: the cart build tree (default build/cart-wgpu, skipped when not built).
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { semverProblem, resolveAppVersion } from '../tools/lib/app-version.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? resolve(argv[i + 1]) : d; };
const TREE = opt('--tree', join(ROOT, 'build', 'cart-wgpu'));
const OUT = join(ROOT, 'out', 'app-version');
const TOOL = join(ROOT, 'tools', 'cart-version.mjs');
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
let failed = 0;
const fail = (m) => { failed++; console.log(`FAIL ${m}`); };
const ok = (m) => console.log(`ok   ${m}`);
const sh = (cmd, args, cwd) => spawnSync(cmd, args, { cwd, encoding: 'utf8' });
const APP = join(ROOT, 'cmake', 'app.cmake').replace(/\\/g, '/');
const expect = (what, got, want) => (got === want ? ok(`${what}: ${got}`) : fail(`${what}: ${got}, not ${want}`));

// 1. the semver rule, in cmake and in JS
const GOOD = ['0.0.0', '1.4.2', '10.20.30', '1.4.2-dev.g1a2b3c4', '1.5.0-rc.1', '1.0.0-alpha.0', '1.0.0+build.5', '1.0.0-x-y.7+meta-1', '0.0.0-dev.g0123abc'];
const BAD = ['1.2', '1', '01.2.3', '1.02.3', 'v1.2.3', '1.2.3.4', '1.0.0-', '1.0.0-01', '1.0.0-a..b', '1.0.0+', '1.2.3 x', ''];
const semDir = join(OUT, 'semver');
mkdirSync(semDir, { recursive: true });
writeFileSync(join(semDir, 'CMakeLists.txt'), `cmake_minimum_required(VERSION 3.20)
project(t NONE)
include(${APP})
foreach(v ${[...GOOD, ...BAD].map((v) => `"${v}"`).join(' ')})
  cw__check_semver(b "\${v}")
  message(STATUS "@semver [\${v}] [\${b}]")
endforeach()
`);
const sem = sh('cmake', ['-S', semDir, '-B', join(semDir, 'b')]);
const seen = new Map([...sem.stdout.matchAll(/@semver \[(.*?)\] \[(.*?)\]/g)].map((m) => [m[1], m[2]]));
const before = failed;
for (const v of GOOD) {
  if (seen.get(v) !== '') fail(`cmake rejects "${v}" (${seen.get(v) ?? 'not run: ' + sem.stderr.slice(0, 300)})`);
  if (semverProblem(v)) fail(`app-version.mjs rejects "${v}" (${semverProblem(v)})`);
}
for (const v of BAD) {
  if (!seen.get(v)) fail(`cmake takes "${v}" as semver`);
  if (!semverProblem(v)) fail(`app-version.mjs takes "${v}" as semver`);
}
if (failed === before) ok(`semver: cmake and app-version.mjs take ${GOOD.length} versions and refuse ${BAD.length}, the same ones`);

// 2. the git rule
const repo = join(OUT, 'repo');
mkdirSync(repo, { recursive: true });
const git = (...a) => {
  const r = sh('git', ['-c', 'user.name=t', '-c', 'user.email=t@example.com', '-c', 'commit.gpgsign=false', '-c', 'tag.gpgsign=false', ...a], repo);
  if (r.status !== 0) throw new Error(`git ${a.join(' ')}: ${r.stderr}`);
  return r.stdout.trim();
};
const v = (o = {}) => resolveAppVersion({ dir: repo, ...o }).version;
git('init', '-q');
writeFileSync(join(repo, 'one.txt'), '1\n');
git('add', 'one.txt');
git('commit', '-q', '-m', 'one');
const sha1 = git('rev-parse', '--short=7', 'HEAD');
expect('git, before any tag', v(), `0.0.0-dev.g${sha1}`);
git('tag', 'v1.2.3');
expect('git, at a clean v1.2.3 tag', v(), '1.2.3');
writeFileSync(join(repo, 'untracked.txt'), 'x\n');
expect('git, at the tag with an untracked file', v(), '1.2.3');
writeFileSync(join(repo, 'one.txt'), 'changed\n');
expect('git, at the tag with a tracked file changed', v(), `1.2.3-dev.g${sha1}`);
git('add', 'one.txt');
git('commit', '-q', '-m', 'two');
const sha2 = git('rev-parse', '--short=7', 'HEAD');
expect('git, a commit after v1.2.3', v(), `1.2.3-dev.g${sha2}`);
expect('an explicit version wins', v({ set: '2.0.0' }), '2.0.0');
const nogit = join(OUT, 'nogit');
mkdirSync(nogit, { recursive: true });
process.env.GIT_CEILING_DIRECTORIES = OUT; // nogit sits inside this checkout: git must not find it
expect('outside git, project.cwproj 1.2', resolveAppVersion({ dir: nogit, fallback: '1.2' }).version, '1.2.0');
expect('outside git, nothing', resolveAppVersion({ dir: nogit }).version, '0.1.0');
delete process.env.GIT_CEILING_DIRECTORIES;

// 3. pack and repack (a stand-in pack command that writes its --version)
const fake = join(OUT, 'fake-pack.mjs');
writeFileSync(fake, `import { writeFileSync } from 'node:fs';
const a = process.argv.slice(2);
writeFileSync(a[a.indexOf('--output') + 1], 'version=' + a[a.indexOf('--version') + 1] + '\\n');
`);
const cart = join(OUT, 'game.wasc');
const vfile = join(OUT, 'game.version');
const update = (set = '') => sh(process.execPath, [TOOL, 'update', '--out', vfile, '--dir', repo, '--set', set, '--fallback', '', '--wasc', cart]);
const cartVersion = () => (readFileSync(cart, 'utf8').match(/version=(.*)/) || [])[1];
let r = update();
if (r.status !== 0) fail(`update: ${r.stderr}`);
r = sh(process.execPath, [TOOL, 'pack', '--version-file', vfile, '--wasc', cart, '--', process.execPath, fake]);
if (r.status !== 0) fail(`pack: ${r.stderr}`);
expect('pack: the cart carries the version file', cartVersion(), `1.2.3-dev.g${sha2}`);
git('tag', 'v1.2.4');
r = update();
expect('update after tagging v1.2.4: the cart is repacked', cartVersion(), '1.2.4');
if (!/repacked as 1\.2\.4/.test(r.stdout)) fail(`update did not say it repacked: ${r.stdout}${r.stderr}`);
r = update();
if (/repacked/.test(r.stdout)) fail('update repacked a cart whose version had not moved');
else ok('update with the version unchanged leaves the cart alone');

// 4. a built cart
const cache = join(TREE, 'CMakeCache.txt');
const hello = join(TREE, 'carts', 'hello.wasc');
if (!existsSync(cache) || !existsSync(hello)) {
  console.log(`skip built carts: no ${hello}`);
} else {
  const set = (/^CW_APP_VERSION:STRING=(.*)$/m.exec(readFileSync(cache, 'utf8')) || [])[1] || '';
  const want = resolveAppVersion({ set, dir: join(ROOT, 'samples') }).version;
  const m = sh('unzip', ['-p', hello, 'manifest.json']);
  expect(`hello.wasc's manifest version (${set ? 'the tree\'s CW_APP_VERSION' : 'this checkout\'s'})`, m.status === 0 ? JSON.parse(m.stdout).version : `(no manifest: ${m.stderr})`, want);
}

// CONTROLS
const ctl = join(OUT, 'control');
mkdirSync(ctl, { recursive: true });
writeFileSync(join(ctl, 'CMakeLists.txt'), `cmake_minimum_required(VERSION 3.20)
project(t C)
include(${APP})
file(WRITE \${CMAKE_BINARY_DIR}/main.c "int main(void) { return 0; }\\n")
function(cw__app_programs)
endfunction()
cw_add_app(ctl SOURCES \${CMAKE_BINARY_DIR}/main.c VERSION 1.2 NO_PLUGINS)
`);
const c = sh('cmake', ['-S', ctl, '-B', join(ctl, 'b')]);
if (c.status === 0) fail('CONTROL: cw_add_app took VERSION 1.2 (not semver)');
else if (!/version "1\.2" is not semver/.test(c.stderr)) fail(`CONTROL: the configure failed for another reason: ${c.stderr.slice(0, 400)}`);
else ok('CONTROL: cw_add_app refuses VERSION 1.2');
r = update('1.2');
if (r.status === 0) fail('CONTROL: cart-version.mjs update took --set 1.2');
else ok('CONTROL: cart-version.mjs update refuses --set 1.2');

console.log(failed ? `${failed} failed` : 'app version: all ok');
process.exit(failed ? 1 : 0);
