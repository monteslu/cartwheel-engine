// Turn a build tree configured with -DCW_SDK=ON (cmake/sdk.cmake) into a
// prebuilt engine SDK: the static libraries, every header they expose, the
// CMake package (lib/cmake/cartwheel), the engine's CMake functions and the
// tools a game build runs, and manifest.json (versions and toolchains).
// scripts/build-sdk.sh runs it; docs/sdk.md describes the result.
//
//   node tools/sdk-pack.mjs --build <dir> --target <name> --out <dir> [--tar <file.tar.gz>]
//
// Paths: whatever the build names under this checkout lands at the same
// relative path under the SDK (deps/box2d/include, cmake/cart/...), and
// whatever it names under the build tree lands under build/, so the targets
// file is the build's own interface with one prefix swapped for ${CW_ROOT}.
import { spawnSync } from 'node:child_process';
import { copyFileSync, cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync, chmodSync } from 'node:fs';
import { basename, dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const arg = (k) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : null; };
const BUILD = resolve(arg('--build') || '');
const TARGET = arg('--target');
const OUT = resolve(arg('--out') || '');
const TAR = arg('--tar');
// a cart SDK's second build of the same libraries without ThinLTO (CW_CART_LTO=OFF): lib/nolto/, for dev carts
const NOLTO = arg('--nolto-build');
if (!arg('--build') || !TARGET || !arg('--out')) {
  console.error('usage: node tools/sdk-pack.mjs --build <dir> --target <name> --out <dir> [--tar <file.tar.gz>]');
  process.exit(2);
}
const T = JSON.parse(readFileSync(join(BUILD, 'sdk/targets.json'), 'utf8'));
const CART = T.cart === 'ON';
const WGPU = T.wgpu === 'ON';
const SRC = resolve(T.source_dir), BIN = resolve(T.binary_dir);
const DAWN = T.native_dawn ? resolve(T.native_dawn) : null; // its whole prefix ships as deps/native-dawn
const fail = (m) => { console.error(`sdk-pack: ${m}`); process.exit(1); };

rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });

const HEADER = /\.(h|hh|hpp|inc|inl)$/;
const copied = new Set();
function copyFile(src, dst) {
  if (copied.has(dst)) return;
  copied.add(dst);
  mkdirSync(dirname(dst), { recursive: true });
  copyFileSync(src, dst);
  if (statSync(src).mode & 0o111) chmodSync(dst, 0o755);
}
/** copy a tree; filter(relPath, isDir) */
function copyTree(src, dst, filter = () => true, rel = '') {
  if (!existsSync(src)) fail(`missing ${src}`);
  for (const e of readdirSync(src, { withFileTypes: true })) {
    const r = rel ? `${rel}/${e.name}` : e.name;
    const s = join(src, e.name);
    const dir = e.isDirectory() || (e.isSymbolicLink() && statSync(s).isDirectory());
    if (e.name === '.git' || !filter(r, dir)) continue;
    if (dir) copyTree(s, join(dst, e.name), filter, r);
    else copyFile(s, join(dst, e.name));
  }
}
const headersOnly = (r, dir) => dir || HEADER.test(r);

/** a path the build names -> its place in the SDK ('${CW_ROOT}/...'), copying what it points at */
function mapPath(p, kind) {
  const abs = resolve(p);
  let rel = null;
  if (DAWN && (abs === DAWN || abs.startsWith(DAWN + sep))) rel = join('deps/native-dawn', relative(DAWN, abs));
  else if (abs === BIN || abs.startsWith(BIN + sep)) rel = join('build', relative(BIN, abs));
  else if (abs === SRC || abs.startsWith(SRC + sep)) rel = relative(SRC, abs);
  if (rel === null) return p; // a system path (the sysroot, the NDK): the consumer's toolchain has it
  const dst = join(OUT, rel);
  if (existsSync(abs)) {
    if (statSync(abs).isDirectory()) copyTree(abs, dst, kind === 'include' ? headersOnly : () => true);
    else copyFile(abs, dst);
  }
  return '${CW_ROOT}/' + rel.split(sep).join('/');
}

const split = (s) => (s ? s.split(';').filter(Boolean) : []);
const IMPORTED_NAME = { 'SDL2-static': 'SDL2::SDL2-static', SDL2main: 'SDL2::SDL2main' };
const cmakeList = (items) => items.join(';');

let targets = '';
const libs = [];
for (const t of T.targets) {
  const name = IMPORTED_NAME[t.name] || t.name;
  const includes = [...new Set(split(t.includes).map((p) => mapPath(p, 'include')))];
  const options = split(t.options).map((o) => (o.startsWith('/') ? mapPath(o, 'file') : o));
  const linkOptions = split(t.link_options);
  // $<BUILD_INTERFACE:...>: build-tree-only usage requirements (SDL's build options)
  const links = split(t.links).filter((l) => !l.includes('$<BUILD_INTERFACE:')).map((l) => {
    const m = /^\$<LINK_ONLY:(.*)>$/.exec(l);
    const item = m ? m[1] : l;
    const mapped = item.startsWith('/') ? mapPath(item, 'file') : IMPORTED_NAME[item] || item;
    return m ? `$<LINK_ONLY:${mapped}>` : mapped;
  });
  const props = [];
  if (t.name === 'cw_wgpu_cart_archive') {
    // the WebGPU port, linked whole (in-tree an OBJECT library), with the
    // exports and --allow-undefined its kit adds to every cart
    const file = join('lib', basename(t.file));
    copyFile(t.file, join(OUT, file));
    copyNolto(t.file);
    libs.push(file);
    targets += `add_library(cw_wgpu_cart INTERFACE IMPORTED)\n`;
    props.push(['INTERFACE_LINK_LIBRARIES', cmakeList(['-Wl,--whole-archive', '${CW_ROOT}/${CW_SDK_LIBDIR}/' + basename(t.file), '-Wl,--no-whole-archive'])]);
    props.push(['INTERFACE_LINK_OPTIONS', cmakeList(split(T.wgpu_cart_link_options))]);
    targets += setProps('cw_wgpu_cart', props);
    continue;
  }
  if (t.type === 'INTERFACE_LIBRARY') {
    targets += `add_library(${name} INTERFACE IMPORTED)\n`;
  } else {
    const file = join('lib', basename(t.file));
    copyFile(t.file, join(OUT, file));
    copyNolto(t.file);
    libs.push(file);
    targets += `add_library(${name} STATIC IMPORTED)\n`;
    props.push(['IMPORTED_LOCATION', '${CW_ROOT}/${CW_SDK_LIBDIR}/' + basename(t.file)]);
    props.push(['IMPORTED_LINK_INTERFACE_LANGUAGES', 'C;CXX']);
  }
  if (includes.length) props.push(['INTERFACE_INCLUDE_DIRECTORIES', cmakeList(includes)]);
  if (split(t.defines).length) props.push(['INTERFACE_COMPILE_DEFINITIONS', t.defines]);
  if (options.length) props.push(['INTERFACE_COMPILE_OPTIONS', cmakeList(options)]);
  if (linkOptions.length) props.push(['INTERFACE_LINK_OPTIONS', cmakeList(linkOptions)]);
  if (links.length) props.push(['INTERFACE_LINK_LIBRARIES', cmakeList(links)]);
  targets += setProps(name, props);
}
/** the same library from the --nolto-build tree, into lib/nolto/ */
function copyNolto(file) {
  if (!NOLTO) return;
  const twin = join(resolve(NOLTO), relative(BIN, resolve(file)));
  if (!existsSync(twin)) fail(`${twin}: the --nolto-build tree lacks ${relative(BIN, file)}`);
  copyFile(twin, join(OUT, 'lib/nolto', basename(file)));
}
function setProps(name, props) {
  if (!props.length) return '';
  return `set_target_properties(${name} PROPERTIES\n${props.map(([k, v]) => `  ${k} "${v.replace(/"/g, '\\"')}"`).join('\n')})\n`;
}

// ── the engine's own files a game build uses ──────────────────────────
const engineFile = (rel) => copyFile(join(SRC, rel), join(OUT, rel));
const engineTree = (rel, filter) => copyTree(join(SRC, rel), join(OUT, rel), filter);
for (const f of ['cmake/flags.cmake', 'cmake/app.cmake', 'cmake/programs.cmake', 'cmake/plugins.cmake', 'LICENSE', 'NOTICE'])
  engineFile(f);
engineTree('include');
engineTree('docs', (r, dir) => dir ? !r.startsWith('tutorials') : /\.md$/.test(r) && !r.includes('/'));
// three.c: its headers (program tables compile against src/gen/programs.h) and
// the program-table generator (tools/, Node only)
const three = resolve(T.three_dir);
copyTree(join(three, 'include'), join(OUT, 'deps/three.c/include'), headersOnly);
copyTree(join(three, 'src'), join(OUT, 'deps/three.c/src'), headersOnly);
for (const f of ['LICENSE', 'LICENSE.three.js', 'package.json']) if (existsSync(join(three, f))) copyFile(join(three, f), join(OUT, 'deps/three.c', f));
// engine-provided assets packs may name (@engine/deps/fonts/...)
if (existsSync(join(SRC, 'deps/fonts'))) engineTree('deps/fonts', (r) => !r.endsWith('.version'));
// the toolchain fetcher, reading the lock (scripts/fetch-deps.sh --only NAME)
engineFile('scripts/fetch-deps.sh');
engineFile('scripts/deps.lock');

const lock = readFileSync(join(SRC, 'scripts/deps.lock'), 'utf8').split('\n').filter((l) => l && !l.startsWith('#')).map((l) => l.trim().split(/\s+/));
const lockRev = (name) => { const l = lock.find((x) => x[0] === name); return l ? l[2] : null; };

if (CART) {
  engineFile('cmake/cart.cmake');
  engineTree('cmake/cart');
  engineFile('tools/check-imports.mjs');
  engineFile('tools/stage-assets.mjs');
  // wasmcart-pack and, for dual carts, the WebGPU tier's import list (check-imports --wgpu)
  for (const wc of WGPU ? ['wasmcart-wgpu'] : ['wasmcart']) {
    const d = join(SRC, 'deps', wc);
    const files = ['package.json', 'LICENSE', 'bin/wasmcart-pack.js', 'src/abi.js'];
    if (WGPU) files.push('src/wgpu/host.js', 'src/wgpu/glue-loader.js', 'src/wgpu/emdawnwebgpu-glue.mjs', 'src/wgpu/emdawnwebgpu-glue-manifest.js', 'src/cartMemory.js', 'src/wgpu/release.js');
    for (const f of files) copyFile(join(d, f), join(OUT, 'deps', wc, f));
    for (const m of ['yazl', 'buffer-crc32']) copyTree(join(d, 'node_modules', m), join(OUT, 'deps', wc, 'node_modules', m));
  }
  engineFile('include/cartwheel/plugin.h');
  copyTree(join(SRC, 'deps/wasmcart/include'), join(OUT, 'deps/wasmcart/include'), headersOnly);
} else {
  engineFile('src/platform/native_entry.c'); // Android: the game library carries main itself
  engineTree('cmake/export');
  // the host tools: cwpack, program-table regeneration, exports
  if (T.cwpack && existsSync(T.cwpack)) copyFile(T.cwpack, join(OUT, 'bin', basename(T.cwpack)));
  for (const f of ['tools/programs.mjs', 'tools/cwexport.mjs']) engineFile(f);
  engineTree('tools/lib');
  engineTree('tools/cwexport');
  // the engine's own three.c generator extensions (terrain, clustered lights, gi, mesh decals): a game's table captures their states
  engineTree('src/terrain/extensions');
  engineTree('src/render/extensions'); // the clustered lights' shading part
  engineTree('src/gi/extensions');
  engineTree('src/fx/extensions');
  copyTree(join(three, 'tools'), join(OUT, 'deps/three.c/tools'));
  // SDL2's Android activity (Java) and Gradle wrapper: the Android export's project
  if (TARGET.startsWith('android')) engineTree('deps/sdl2/android-project');
  copyFile(join(SRC, 'deps/sdl2/LICENSE.txt'), join(OUT, 'deps/sdl2/LICENSE.txt'));
}
// native WebGPU: native-dawn's whole prefix (its CMake package, the shared library)
let dawn = null;
if (WGPU && !CART) {
  const nd = resolve(T.native_dawn || join(SRC, 'deps/native-dawn'));
  copyTree(nd, join(OUT, 'deps/native-dawn'), (r) => !r.endsWith('.node') && r !== '.version');
  const bj = join(nd, 'build.json');
  dawn = existsSync(bj) ? JSON.parse(readFileSync(bj, 'utf8')) : null;
}

// ANGLE: the whole lib/ beside the libGLESv2 the build links (libEGL, d3dcompiler)
let angleDir = null;
if (T.angle) {
  const dir = dirname(resolve(T.angle));
  angleDir = mapPath(dir, 'file').replace('${CW_ROOT}/', '');
  copyTree(dir, join(OUT, angleDir));
}
// what a game needs beside its executable at run time (tools/cwexport copies these)
const runtime = [];
const sdkFiles = (dir, re) => existsSync(join(OUT, dir)) ? readdirSync(join(OUT, dir)).filter((f) => re.test(f)).map((f) => `${dir}/${f}`) : [];
if (WGPU && !CART) runtime.push(...sdkFiles('deps/native-dawn/lib', /\.(so|dylib)$/), ...sdkFiles('deps/native-dawn/bin', /\.dll$/));
if (angleDir) runtime.push(...sdkFiles(angleDir, /\.(dll|dylib)$/).filter((f) => !/GLESv1_CM/.test(f)));

// ── the CMake package ────────────────────────────────────────────────
const version = (() => {
  if (process.env.CW_SDK_VERSION) return process.env.CW_SDK_VERSION.replace(/^v/, '');
  const r = spawnSync('git', ['-C', SRC, 'describe', '--tags', '--always', '--dirty'], { encoding: 'utf8' });
  return r.status === 0 ? r.stdout.trim().replace(/^v/, '') : '0.0.0';
})();
const commit = spawnSync('git', ['-C', SRC, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).stdout.trim();
const pkgDir = join(OUT, 'lib/cmake/cartwheel');
mkdirSync(pkgDir, { recursive: true });
writeFileSync(join(pkgDir, 'cartwheelTargets.cmake'), `# GENERATED by tools/sdk-pack.mjs: the engine's libraries as imported targets.\n${targets}`);
const numericVersion = /^(\d+\.\d+\.\d+)/.exec(version)?.[1] || '0.0.0';
writeFileSync(join(pkgDir, 'cartwheelConfigVersion.cmake'), `# GENERATED by tools/sdk-pack.mjs
set(PACKAGE_VERSION "${numericVersion}")
if(PACKAGE_FIND_VERSION AND NOT PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
  set(PACKAGE_VERSION_EXACT TRUE)
endif()
`);
const info = {
  CW_SDK_TARGET: TARGET, CW_SDK_NOLTO: NOLTO ? 'ON' : 'OFF', CW_SDK_VERSION: version, CW_SDK_COMMIT: commit, CW_SDK_CART: CART ? 'ON' : 'OFF', CW_SDK_WGPU: WGPU ? 'ON' : 'OFF',
  CW_SDK_ANGLE_GLESV2: T.angle ? mapPath(T.angle, 'file').replace('${CW_ROOT}/', '') : '',
  CW_SDK_SYSTEM: T.system, CW_SDK_PROCESSOR: T.processor, CW_SDK_C_COMPILER: T.c_compiler, CW_SDK_THREE_TABLE: T.three_table,
};
writeFileSync(join(pkgDir, 'cartwheel-sdk-info.cmake'), `# GENERATED by tools/sdk-pack.mjs\n${Object.entries(info).map(([k, v]) => `set(${k} "${v}")`).join('\n')}\n`);
copyFile(join(SRC, 'cmake/sdk/cartwheelConfig.cmake'), join(pkgDir, 'cartwheelConfig.cmake'));

// ── manifest.json ────────────────────────────────────────────────────
const toolchains = {};
if (CART) {
  toolchains['wasi-sdk'] = lockRev('wasi-sdk');
  toolchains.binaryen = lockRev('binaryen');
  toolchains.target = 'wasm32-wasip1-threads';
} else if (TARGET === 'android-arm64') {
  toolchains.ndk = process.env.CW_SDK_NDK_VERSION || null;
  toolchains.android_platform = 'android-24';
} else if (TARGET === 'macos-arm64') {
  toolchains.xcode = process.env.CW_SDK_XCODE_VERSION || null;
  toolchains.macos_min = '11.0';
} else {
  toolchains['llvm-mingw'] = lockRev('export/llvm-mingw');
  if (TARGET.startsWith('linux')) toolchains.sysroot = lockRev(`export/sysroot-${TARGET}`);
}
toolchains.compiler = T.c_compiler;
const manifest = {
  name: 'cartwheel-sdk', target: TARGET, version, commit, build_type: T.build_type,
  system: T.system, processor: T.processor, cart: CART, wgpu: WGPU,
  determinism: { compile_options: ['-ffp-contract=off', '-fno-fast-math'], enforced_by: 'lib/cmake/cartwheel/cartwheelConfig.cmake' },
  toolchains,
  dependencies: Object.fromEntries(['three.c', 'box2d', 'box3d', 'sdl2', 'miniaudio', 'meshoptimizer', 'basis_universal', 'wasmcart-libs', 'kb', 'stb', 'recastnavigation']
    .map((n) => [n, lockRev(n)]).filter(([, v]) => v)),
  native_dawn: dawn, angle: angleDir, runtime,
  libraries: libs.map((f) => f.split(sep).join('/')).sort(),
};
writeFileSync(join(OUT, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');

if (TAR) {
  const top = basename(OUT);
  // GNU tar (gtar on a Mac): sorted names, fixed owners and times, so the same inputs give the same archive
  const tar = spawnSync('gtar', ['--version']).status === 0 ? 'gtar' : 'tar';
  const r = spawnSync('sh', ['-c', `${tar} --sort=name --owner=0 --group=0 --numeric-owner --mtime=@315619200 -C '${dirname(OUT)}' -cf - '${top}' | gzip -9n > '${resolve(TAR)}'`], { stdio: 'inherit' });
  if (r.status !== 0) fail('tar failed');
}
console.log(`sdk-pack: ${TARGET} ${version} -> ${OUT} (${libs.length} libraries)`);
