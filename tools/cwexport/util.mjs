// Shared helpers for the exporters: processes, the engine builds, packing,
// deterministic archives. Every exporter goes through these, so the editor's
// export button and `tools/cwexport.mjs` produce the same bytes.
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync, copyFileSync, rmSync, readdirSync, statSync, chmodSync, utimesSync } from 'node:fs';
import { join, dirname, resolve, relative } from 'node:path';
import { fileURLToPath } from 'node:url';

/** The cartwheel checkout this tool lives in, or the prebuilt SDK (docs/sdk.md) it ships in. */
export const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
/** True when ROOT is a prebuilt SDK: apps build against the target's SDK, not the engine's sources. */
export const IN_SDK = existsSync(join(ROOT, 'lib/cmake/cartwheel/cartwheelConfig.cmake'));
/** Toolchains and runtimes fetched by `scripts/fetch-deps.sh --export` (in an SDK: --only export/NAME). */
export const EXPORT_DEPS = join(ROOT, 'deps', 'export');
/** export platform -> the SDK target that builds it */
export const SDK_TARGET = { 'linux-x64': 'linux-x64', 'linux-arm64': 'linux-arm64', 'windows-x64': 'windows-x64', 'android-arm64': 'android-arm64',
  macos: 'macos-arm64', cart: 'cart', 'cart-wgpu': 'cart-wgpu' };
/**
 * The SDK that builds a platform: SDKs unpack side by side, one directory per
 * target (deps/cartwheel-sdk/<target>/ in a game repository), so the target's
 * is this one's sibling. CW_SDK_<TARGET> (e.g. CW_SDK_WINDOWS_X64) names another.
 */
export function sdkFor(platform) {
  const t = SDK_TARGET[platform];
  if (!t) throw new ExportError(`${platform}: no SDK target builds it`);
  const env = process.env[`CW_SDK_${t.toUpperCase().replace(/-/g, '_')}`];
  const dir = env ? resolve(env) : join(dirname(ROOT), t);
  if (!existsSync(join(dir, 'lib/cmake/cartwheel/cartwheelConfig.cmake')))
    throw new ExportError(`no cartwheel SDK for ${t} at ${dir} (unpack cartwheel-sdk-<version>-${t}.tar.gz beside ${ROOT}, or set CW_SDK_${t.toUpperCase().replace(/-/g, '_')})`);
  return dir;
}
/** 1980-01-02: the timestamp every archive member gets (zip cannot store earlier). */
export const FIXED_TIME = new Date(Date.UTC(1980, 0, 2));

export class ExportError extends Error {}

/**
 * Run a command, streaming its output lines to log. Rejects (ExportError
 * naming the command and its last output) on a nonzero exit.
 * @param {string} cmd
 * @param {string[]} args
 * @param {{cwd?: string, env?: object, log?: (line: string) => void, quiet?: boolean}} [o]
 * @returns {Promise<string>} the combined output
 */
export function run(cmd, args, o = {}) {
  return new Promise((res, rej) => {
    const child = spawn(cmd, args, { cwd: o.cwd, env: { ...process.env, ...(o.env || {}) }, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '', partial = '';
    const onData = (d) => {
      const s = d.toString();
      out += s;
      if (o.quiet || !o.log) return;
      partial += s;
      const lines = partial.split('\n');
      partial = lines.pop();
      for (const l of lines) if (l.trim()) o.log(l);
    };
    child.stdout.on('data', onData);
    child.stderr.on('data', onData);
    child.on('error', (e) => rej(new ExportError(`${cmd}: ${e.message}`)));
    child.on('close', (code) => {
      if (partial.trim() && o.log && !o.quiet) o.log(partial);
      if (code === 0) res(out);
      else rej(new ExportError(`${cmd} ${args.join(' ')} exited ${code}:\n${out.split('\n').slice(-25).join('\n')}`));
    });
  });
}

/** mkdir -p */
export function mkdirp(d) { mkdirSync(d, { recursive: true }); return d; }

/** Copy a file, creating the destination directory. */
export function copy(src, dst) {
  if (!existsSync(src)) throw new ExportError(`missing file: ${src}`);
  mkdirp(dirname(dst));
  copyFileSync(src, dst);
  return dst;
}

/** Copy a directory tree. */
export function copyTree(src, dst, filter = () => true) {
  for (const e of readdirSync(src, { withFileTypes: true })) {
    const s = join(src, e.name), d = join(dst, e.name);
    if (!filter(s, e)) continue;
    if (e.isDirectory()) copyTree(s, d, filter);
    else copy(s, d);
  }
}

/** Every file under dir (relative paths, '/' separated, sorted). */
export function listFiles(dir, base = dir) {
  const out = [];
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) out.push(...listFiles(p, base));
    else out.push(relative(base, p).split('\\').join('/'));
  }
  return out.sort();
}

/** Set every file and directory under dir to FIXED_TIME (reproducible archives). */
export function pinTimes(dir) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) pinTimes(p);
    utimesSync(p, FIXED_TIME, FIXED_TIME);
  }
  utimesSync(dir, FIXED_TIME, FIXED_TIME);
}

/** A fresh, empty directory. */
export function freshDir(d) {
  rmSync(d, { recursive: true, force: true });
  return mkdirp(d);
}

/**
 * Deterministic .tar.gz of dir's contents under a top directory named top.
 * @returns {Promise<string>} the archive path
 */
export async function tarGz(dir, top, outFile) {
  pinTimes(dir);
  const parent = dirname(dir);
  rmSync(outFile, { force: true });
  await run('sh', ['-c', `tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@315619200 --transform 's,^${relative(parent, dir)},${top},' -cf - '${relative(parent, dir)}' | gzip -9n > '${outFile}'`], { cwd: parent });
  return outFile;
}

/** Deterministic .zip of dir's contents under a top directory named top. */
export async function zipDir(dir, top, outFile) {
  pinTimes(dir);
  rmSync(outFile, { force: true });
  const stage = join(dirname(dir), `.zip-${process.pid}`);
  freshDir(stage);
  const { renameSync } = await import('node:fs');
  renameSync(dir, join(stage, top));
  try {
    const files = listFiles(join(stage, top)).map((f) => `${top}/${f}`);
    await run('zip', ['-X', '-q', '-9', resolve(outFile), ...files], { cwd: stage });
  } finally {
    renameSync(join(stage, top), dir);
    rmSync(stage, { recursive: true, force: true });
  }
  return outFile;
}

/**
 * Parse a project.cwproj: flat `key = value` lines, `;` or `#` comments,
 * values optionally double-quoted.
 * @returns {Record<string, string>}
 */
export function parseCwproj(text) {
  const keys = {};
  for (const raw of text.split('\n')) {
    const line = raw.trim();
    if (!line || line.startsWith(';') || line.startsWith('#')) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const k = line.slice(0, eq).replace(/\s+/g, '');
    let v = line.slice(eq + 1).trim();
    if (v.length >= 2 && v.startsWith('"') && v.endsWith('"')) v = v.slice(1, -1).replace(/\\"/g, '"').replace(/\\\\/g, '\\');
    keys[k] = v;
  }
  return keys;
}

/** The host's native cwpack (packs for every target), built on demand (an SDK ships it). */
export async function hostCwpack(log) {
  if (IN_SDK) {
    const f = join(ROOT, 'bin', process.platform === 'win32' ? 'cwpack.exe' : 'cwpack');
    if (!existsSync(f)) throw new ExportError(`${ROOT} ships no cwpack: export from the SDK for this machine (linux-x64, windows-x64, macos-arm64)`);
    return f;
  }
  const exe = join(ROOT, 'build/native/tools/cwpack/cwpack');
  if (!existsSync(join(ROOT, 'build/native/CMakeCache.txt')))
    await run('cmake', ['-S', ROOT, '-B', join(ROOT, 'build/native'), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release'], { log, quiet: true });
  await run('ninja', ['-C', join(ROOT, 'build/native'), 'cwpack'], { log, quiet: true });
  if (!existsSync(exe)) throw new ExportError(`cwpack did not build at ${exe}`);
  return exe;
}

/**
 * Build one app for one platform with that platform's toolchain file
 * (cmake/export/<platform>.cmake, or cmake/cart.cmake for carts).
 * Tree apps (samples, games) build in build/export/<platform> (carts: the dev
 * build/cart tree); projects in <project>/.cwbuild/<platform>.
 * @returns {Promise<{name: string, binary: string, pak: string, assets: string, manifest: string, buildDir: string}>}
 */
export async function buildApp(ctx, platform) {
  // 'cart-wgpu': the dual cart (GL + WebGPU imports; the host picks, WebGPU where it has it)
  const dual = platform === 'cart-wgpu';
  const cart = platform === 'cart' || dual;
  const toolchain = cart ? join(ROOT, 'cmake/cart.cmake') : join(ROOT, 'cmake/export', `${platform}.cmake`);
  /* --three-builder: three.c's runtime program builder in the cart (a preview
   * while three.c's table catches up with new states; +4.7 MB, a hitch the
   * first time a state draws), in a tree of its own */
  const tag = ctx.threeBuilder ? '-builder' : '';
  // ctx.buildDir (--build): an already configured tree for the preset's own build
  const own = platform === ctx.platform || (ctx.platform === 'wasc' && platform === 'cart-wgpu') || (ctx.platform === 'macos' && platform === 'macos');
  const buildDir = ctx.buildDir && own ? resolve(ctx.buildDir) : ctx.isProject ? join(ctx.projectDir, '.cwbuild', platform + tag)
    : cart ? join(ROOT, 'build', platform + tag) : join(ROOT, 'build/export', platform + tag);
  const cwpack = await hostCwpack(ctx.log); // carts and cross builds pack with it
  if (IN_SDK && !existsSync(join(buildDir, 'CMakeCache.txt'))) {
    // the project against the target's SDK: a generated top-level CMakeLists
    // that finds the package and adds the project, as the engine's own does
    if (!ctx.isProject) throw new ExportError('an SDK exports projects (a directory with project.cwproj and CMakeLists.txt)');
    if (ctx.threeBuilder) throw new ExportError('--three-builder needs the engine from source: an SDK ships program tables only');
    const sdk = sdkFor(platform);
    const top = join(buildDir, '.top');
    write(join(top, 'CMakeLists.txt'), `cmake_minimum_required(VERSION 3.20)\nproject(${ctx.app} C CXX)\nfind_package(cartwheel REQUIRED CONFIG)\nadd_subdirectory("${ctx.projectDir.replace(/\\/g, '/')}" project)\n`);
    ctx.progress(10, `configure ${platform}`);
    const tc = cart ? join(sdk, 'cmake/cart.cmake') : platform === 'macos' ? join(sdk, 'cmake/export/macos-arm64.cmake') : join(sdk, 'cmake/export', `${platform}.cmake`);
    await run('cmake', ['-S', top, '-B', buildDir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', `-DCMAKE_TOOLCHAIN_FILE=${tc}`,
      `-Dcartwheel_DIR=${join(sdk, 'lib/cmake/cartwheel')}`, `-DCW_CWPACK=${cwpack}`, `-DCW_EXPORT_DEPS=${EXPORT_DEPS}`], { log: ctx.log, quiet: true });
  }
  if (!existsSync(join(buildDir, 'CMakeCache.txt'))) {
    ctx.progress(10, `configure ${platform}`);
    // a shipped build carries its program tables only, never three.c's runtime builder (cmake/deps.cmake)
    const args = ['-S', ROOT, '-B', buildDir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', `-DCMAKE_TOOLCHAIN_FILE=${toolchain}`, '-DCW_THREE_BUILDER=OFF'];
    if (ctx.isProject) args.push(`-DCW_PROJECT_DIR=${ctx.projectDir}`, '-DCW_ENGINE_ONLY=ON');
    if (ctx.threeBuilder) args.push('-DCW_THREE_BUILDER=ON');
    if (dual) args.push('-DCW_WGPU=ON', `-DCW_CWPACK=${cwpack}`);
    await run('cmake', args, { log: ctx.log, quiet: true });
  }
  ctx.progress(20, `build ${ctx.app} (${platform})`);
  await run('ninja', ['-C', buildDir, ctx.app], { log: ctx.log, quiet: !ctx.verbose });
  if (cart) {
    const wasc = join(buildDir, 'carts', `${ctx.app}.wasc`);
    if (!existsSync(wasc)) throw new ExportError(`the cart build made no ${wasc}`);
    return { name: ctx.app, binary: wasc, pak: '', assets: '', manifest: '', buildDir };
  }
  const desc = join(buildDir, 'apps', `${ctx.app}.json`);
  if (!existsSync(desc)) throw new ExportError(`no app named "${ctx.app}" in this build (no ${desc}): check \`app\` in project.cwproj`);
  return { ...JSON.parse(readFileSync(desc, 'utf8')), buildDir };
}

/**
 * The cart a cart export ships: the dual cart (GL + WebGPU imports: the host
 * runs it on WebGPU where it has WebGPU, else on GL), from the dual build
 * (build/cart-wgpu, the deps scripts/fetch-deps.sh fetches for it). An app
 * built GL_ONLY (it calls GL itself) has no dual cart: its GL cart.
 * @returns {Promise<{name: string, binary: string, buildDir: string, dual: boolean}>}
 */
export async function buildCart(ctx) {
  const dual = await buildDualOrNull(ctx);
  return dual ? { ...dual, dual: true } : { ...(await buildApp(ctx, 'cart')), dual: false };
}
async function buildDualOrNull(ctx) {
  if (IN_SDK) {
    // configure (and build) against the cart-wgpu SDK: carts.txt then says whether the app has a dual cart
    const app = await buildApp(ctx, 'cart-wgpu').catch((e) => e);
    if (!(app instanceof Error)) return app;
    const dir = ctx.buildDir ? resolve(ctx.buildDir) : join(ctx.projectDir, '.cwbuild', 'cart-wgpu');
    const listed = existsSync(join(dir, 'carts.txt')) && readFileSync(join(dir, 'carts.txt'), 'utf8').split('\n').includes(ctx.app);
    if (listed) throw app;
    return null;
  }
  const tag = ctx.threeBuilder ? '-builder' : '';
  const dir = ctx.isProject ? join(ctx.projectDir, '.cwbuild', 'cart-wgpu' + tag) : join(ROOT, 'build/cart-wgpu' + tag);
  if (!existsSync(join(dir, 'CMakeCache.txt'))) {
    // configure only: carts.txt then lists the apps that get a dual cart
    const cwpack = await hostCwpack(ctx.log);
    const args = ['-S', ROOT, '-B', dir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', `-DCMAKE_TOOLCHAIN_FILE=${join(ROOT, 'cmake/cart.cmake')}`,
      '-DCW_THREE_BUILDER=' + (ctx.threeBuilder ? 'ON' : 'OFF'), '-DCW_WGPU=ON', `-DCW_CWPACK=${cwpack}`];
    if (ctx.isProject) args.push(`-DCW_PROJECT_DIR=${ctx.projectDir}`, '-DCW_ENGINE_ONLY=ON');
    await run('cmake', args, { log: ctx.log, quiet: true });
  }
  const listed = existsSync(join(dir, 'carts.txt')) && readFileSync(join(dir, 'carts.txt'), 'utf8').split('\n').includes(ctx.app);
  return listed ? buildApp(ctx, 'cart-wgpu') : null;
}

/**
 * Put the app's assets where a native export reads them: the .cwpak the build
 * made (cw_add_app PACK), or a raw pack of its asset directory (ASSETS).
 * @returns {Promise<string|null>} the pack written, or null for an app with no assets
 */
export async function packAssets(ctx, app, dst) {
  mkdirp(dirname(dst));
  if (app.pak) return copy(app.pak, dst);
  if (app.assets) {
    if (!existsSync(app.assets) || !statSync(app.assets).isDirectory())
      throw new ExportError(`the app's asset directory ${app.assets} is missing`);
    await run(await hostCwpack(ctx.log), ['--raw-dir', app.assets, '--pak', dst, '-q'], { log: ctx.log });
    return dst;
  }
  return null;
}

/** Strip a binary with llvm-strip (llvm-mingw's handles ELF, PE and Mach-O; on a Mac, Xcode's strip). */
export async function strip(file, log) {
  if (process.platform === 'darwin' && !existsSync(join(EXPORT_DEPS, 'llvm-mingw/bin/llvm-strip'))) await run('strip', ['-x', file], { log });
  else await run(join(EXPORT_DEPS, 'llvm-mingw/bin/llvm-strip'), ['--strip-unneeded', file], { log });
}

/** The shared libraries an app runs with (Dawn, ANGLE), as its build listed them (apps/<app>.json). */
export function runtimeFiles(app) {
  for (const f of app.runtime || []) if (!existsSync(f)) throw new ExportError(`missing runtime library ${f}`);
  return app.runtime || [];
}

/** Write a file (creating its directory). */
export function write(file, text, mode) {
  mkdirp(dirname(file));
  writeFileSync(file, text);
  if (mode) chmodSync(file, mode);
  return file;
}

/** Fail with the fix when an export dependency is missing. */
export function needDep(...rel) {
  for (const r of rel) {
    if (!existsSync(join(EXPORT_DEPS, r)))
      throw new ExportError(`deps/export/${r} is missing: run scripts/fetch-deps.sh --export (pinned in scripts/deps.lock)`);
  }
}

/** A filesystem-safe base name for the output files. */
export function slug(s) { return String(s).toLowerCase().replace(/[^a-z0-9._-]+/g, '-').replace(/^-+|-+$/g, '') || 'game'; }

/** native-dawn's NOTICE and third-party licenses (its prefix's share/native-dawn) into <dst>/, when the app ships Dawn. */
export function copyDawnLicenses(libs, dst) {
  const lib = libs.find((f) => /webgpu_dawn\.(dll|so|dylib)$/.test(f));
  if (!lib) return false;
  const share = join(dirname(dirname(lib)), 'share/native-dawn');
  if (!existsSync(join(share, 'NOTICE'))) throw new ExportError(`native-dawn's licenses are missing (${share})`);
  copy(join(share, 'NOTICE'), join(dst, 'NOTICE.txt'));
  copyTree(join(share, 'licenses'), join(dst, 'licenses'));
  return true;
}
