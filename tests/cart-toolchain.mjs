// The cart toolchain (cmake/cart.cmake) gives the same compile and link flags on a tree's first
// configure as on every later one. When the flags of the first configure differ, the first rebuild
// after a clean build recompiles every cart object (the whole engine and three.c in an engine
// checkout; every game file and program table in a project), whatever changed.
//
//   1. a small C project configured with cmake/cart.cmake, then configured again: build.ninja's
//      FLAGS, DEFINES and LINK_FLAGS lines are the same both times, and a no-change ninja run after
//      the second configure has nothing to do.
// CONTROL that must fail: the same project with a toolchain that includes wasi-sdk's own
// wasi-sdk-pthread.cmake (the old cart.cmake): its first configure's flags differ from its second's.
//
//   node tests/cart-toolchain.mjs
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'cart-toolchain');
if (!existsSync(join(ROOT, 'deps', 'wasi-sdk', 'bin', 'clang'))) { console.log('FAIL no deps/wasi-sdk: run scripts/fetch-deps.sh'); process.exit(1); }
rmSync(OUT, { recursive: true, force: true });
mkdirSync(join(OUT, 'src'), { recursive: true });
writeFileSync(join(OUT, 'src', 'CMakeLists.txt'), 'cmake_minimum_required(VERSION 3.20)\nproject(t C)\nadd_executable(t main.c)\n');
writeFileSync(join(OUT, 'src', 'main.c'), 'int main(void) { return 0; }\n');
// the old toolchain: wasi-sdk's file, which appends -pthread and the memory flags to the cached flags
const old = join(OUT, 'old-cart.cmake');
writeFileSync(old, `set(WASI_SDK_PREFIX ${JSON.stringify(join(ROOT, 'deps', 'wasi-sdk').replace(/\\/g, '/'))})\n` +
  'include(${WASI_SDK_PREFIX}/share/cmake/wasi-sdk-pthread.cmake)\nset(CMAKE_C_COMPILER_TARGET wasm32-wasip1-threads)\n');
let failed = 0;
const fail = (m) => { failed++; console.log(`FAIL ${m}`); };
const ok = (m) => console.log(`ok   ${m}`);

const flags = (tree) => readFileSync(join(tree, 'build.ninja'), 'utf8').split('\n').filter((l) => /^\s+(FLAGS|DEFINES|LINK_FLAGS) =/.test(l)).sort().join('\n');
function twice(toolchain, tag) {
  const tree = join(OUT, tag);
  const conf = () => spawnSync('cmake', ['-S', join(OUT, 'src'), '-B', tree, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', `-DCMAKE_TOOLCHAIN_FILE=${toolchain}`], { encoding: 'utf8' });
  let r = conf();
  if (r.status !== 0) return { error: r.stderr };
  const first = flags(tree);
  r = spawnSync('ninja', ['-C', tree], { encoding: 'utf8' });
  if (r.status !== 0) return { error: r.stdout + r.stderr };
  r = conf();
  if (r.status !== 0) return { error: r.stderr };
  const second = flags(tree);
  const again = spawnSync('ninja', ['-C', tree, '-n', '-d', 'explain'], { encoding: 'utf8' });
  const rebuilds = (again.stdout + again.stderr).split('\n').filter((l) => /^\[\d+\/\d+\] Building/.test(l)).length;
  return { first, second, rebuilds };
}

const now = twice(join(ROOT, 'cmake', 'cart.cmake'), 'new');
if (now.error) fail(`cmake/cart.cmake: ${now.error}`);
else if (now.first !== now.second) fail(`cmake/cart.cmake: the second configure changed the flags:\n${now.first}\n---\n${now.second}`);
else if (now.rebuilds) fail(`cmake/cart.cmake: ${now.rebuilds} object(s) rebuilt after a configure that changed nothing`);
else ok('cmake/cart.cmake: the same flags on the first and second configure, nothing rebuilt');

const ctl = twice(old, 'old');
if (ctl.error) fail(`control toolchain: ${ctl.error}`);
else if (ctl.first !== ctl.second || ctl.rebuilds) ok(`control: wasi-sdk's own toolchain file changes the flags between configures (${ctl.rebuilds} object(s) rebuilt)`);
else fail('CONTROL PASSED: the check cannot see flags that change between configures');

rmSync(OUT, { recursive: true, force: true });
process.exit(failed ? 1 : 0);
