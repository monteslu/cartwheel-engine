// A game built against a prebuilt SDK recompiles when another SDK is unpacked
// in its place (an engine bump), even when the archive kept header times older
// than the game's objects. A game repository's scripts/fetch-deps.sh unpacks a
// release SDK into a fresh deps/cartwheel-sdk/<target>/ with tar, which keeps
// each file's time from the archive; an incremental build then saw nothing
// newer and linked stale objects against the new libraries (a struct whose
// layout changed crashed at run time).
//
// The test copies an unpacked linux-x64 SDK (CW_TEST_SDK, or a sibling game
// repository's deps/cartwheel-sdk/linux-x64) with this tree's
// cmake/sdk/cartwheelConfig.cmake in it, dates every file to 2001, packs it
// and unpacks it as fetch-deps does; builds a probe object that includes
// cartwheel/core.h; then unpacks "the next SDK" (core.h and the SDK's commit
// changed, files again dated 2001) into the same place and builds again. The
// probe must be recompiled and see the new header. CONTROL: the same with the
// v0.18.0 cartwheelConfig.cmake must NOT recompile (the bug), or the test
// could not tell.
//   node tests/sdk-rebuild.mjs
import { spawnSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync, statSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'sdk-rebuild');
const candidates = [process.env.CW_TEST_SDK, ...[join(ROOT, '..'), join(ROOT, '..', '..')].flatMap((d) =>
  ['more-cartwheel-examples', 'cartwheel-examples', 'cartwheel-starter'].map((r) => join(d, r, 'deps/cartwheel-sdk/linux-x64')))].filter(Boolean);
const SRC = candidates.find((d) => existsSync(join(d, 'lib/cmake/cartwheel/cartwheelConfig.cmake')));
if (!SRC) { console.log(`FAIL no unpacked linux-x64 SDK (set CW_TEST_SDK; looked in ${candidates.join(', ')})`); process.exit(1); }

const sh = (cmd, args, opts = {}) => {
  const r = spawnSync(cmd, args, { encoding: 'utf8', ...opts });
  if (r.status !== 0) throw new Error(`${cmd} ${args.join(' ')}: ${(r.stdout || '') + (r.stderr || '')}`.slice(-2000));
  return (r.stdout || '') + (r.stderr || '');
};
const OLD = '200101010000'; // touch -t: older than any object this test builds

/** an SDK tree from SRC with `config` as its cartwheelConfig.cmake, app.h + commit marked `tag` */
function stage(dir, config, tag) {
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dirname(dir), { recursive: true });
  cpSync(SRC, dir, { recursive: true, verbatimSymlinks: true });
  writeFileSync(join(dir, 'lib/cmake/cartwheel/cartwheelConfig.cmake'), config);
  const info = join(dir, 'lib/cmake/cartwheel/cartwheel-sdk-info.cmake');
  writeFileSync(info, readFileSync(info, 'utf8').replace(/set\(CW_SDK_COMMIT "[^"]*"\)/, `set(CW_SDK_COMMIT "${tag}")`));
  const app = join(dir, 'include/cartwheel/core.h');
  writeFileSync(app, readFileSync(app, 'utf8') + `\n#define CW_SDK_REBUILD_PROBE "${tag}"\n`);
  sh('find', [dir, '-exec', 'touch', '-h', '-t', OLD, '{}', '+']);
}
/** unpack `tree` at `dst` as a game repository's fetch-deps does: a fresh directory, tar keeping the archive's times */
function unpack(tree, dst) {
  const tgz = tree + '.tar.gz';
  sh('tar', ['-czf', tgz, '-C', dirname(tree), tree.split('/').pop()]);
  rmSync(dst, { recursive: true, force: true });
  mkdirSync(dst, { recursive: true });
  sh('tar', ['-xzf', tgz, '-C', dst, '--strip-components=1']);
}

function run(name, config) {
  const dir = join(OUT, name), sdk = join(dir, 'deps/cartwheel-sdk/linux-x64'), proj = join(dir, 'game'), build = join(proj, 'build');
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(proj, { recursive: true });
  writeFileSync(join(proj, 'CMakeLists.txt'), 'cmake_minimum_required(VERSION 3.20)\nproject(probe C CXX)\nfind_package(cartwheel REQUIRED CONFIG)\n' +
    'add_library(probe_obj OBJECT main.c)\ntarget_link_libraries(probe_obj PRIVATE cartwheel)\n');
  writeFileSync(join(proj, 'main.c'), '#include "cartwheel/core.h"\nconst char *probe_tag = CW_SDK_REBUILD_PROBE;\nint probe_main(void) { return 0; }\n');
  stage(join(dir, 'next-a'), config, 'sdk-a');
  unpack(join(dir, 'next-a'), sdk);
  sh('cmake', ['-S', proj, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', `-Dcartwheel_DIR=${join(sdk, 'lib/cmake/cartwheel')}`]);
  sh('ninja', ['-C', build, '-j4', 'probe_obj']);
  const obj = join(build, 'CMakeFiles/probe_obj.dir/main.c.o');
  const before = statSync(obj).mtimeMs;
  const tagA = readFileSync(obj).includes('sdk-a');
  // the next SDK, unpacked in the same place; this object is newer than every file in it
  stage(join(dir, 'next-b'), config, 'sdk-b');
  unpack(join(dir, 'next-b'), sdk);
  const out = sh('ninja', ['-C', build, '-j4', 'probe_obj']);
  const rebuilt = statSync(obj).mtimeMs !== before && readFileSync(obj).includes('sdk-b');
  return { tagA, rebuilt, out: out.trim().split('\n').slice(-3).join(' | ') };
}

let failed = 0;
const fixed = readFileSync(join(ROOT, 'cmake/sdk/cartwheelConfig.cmake'), 'utf8');
const old = sh('git', ['-C', ROOT, 'show', 'v0.18.0:cmake/sdk/cartwheelConfig.cmake']);
const r = run('fixed', fixed);
if (r.tagA && r.rebuilt) console.log(`ok   another SDK unpacked in place (older file times): the probe recompiled against the new header (${r.out})`);
else { failed++; console.log(`FAIL the probe was not recompiled after the SDK swap (first build saw sdk-a: ${r.tagA}): ${r.out}`); }
const c = run('control', old);
if (c.tagA && !c.rebuilt) console.log('ok   control: with the v0.18.0 cartwheelConfig.cmake the swap goes unseen (the bug), as it must');
else { failed++; console.log(`FAIL control: the v0.18.0 package also rebuilt (or did not build): the test cannot see the bug (${JSON.stringify(c)})`); }
rmSync(OUT, { recursive: true, force: true });
console.log(failed ? `sdk-rebuild: ${failed} failures` : 'ok   sdk-rebuild');
process.exit(failed ? 1 : 0);
