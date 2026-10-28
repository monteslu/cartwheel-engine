// cwproject: build a cartwheel project (a directory made from an editor
// template: project.cwproj + CMakeLists.txt + src/ + scenes) with this
// engine checkout.
//
//   node tools/cwproject.mjs build <project dir> [native|cart|all|plugins|editor]
//   node tools/cwproject.mjs info <project dir>
//
// A project is built BY the engine tree: cartwheel's CMakeLists.txt is
// configured with -DCW_PROJECT_DIR=<project> -DCW_ENGINE_ONLY=ON into
// <project>/.cwbuild/<target>, and adds the project's CMakeLists.txt (which
// only calls cw_add_app). Outputs:
//   native  .cwbuild/native/project/<app> (+ <app>.cwpak beside it)
//   cart    .cwbuild/cart-wgpu/carts/<app>.wasc, the dual GL + WebGPU cart (the
//           one cart a game ships: GL hosts run it on GL, WebGPU hosts on WebGPU)
// After a successful build the paths go into project.cwproj as game_exe /
// game_cart (relative to the project): what the editor's Play launches.
// plugins: the project's plugins (plugins/<name>/plugin.cwplugin) as
// the shared libraries the native editor loads, in the native tree:
//   .cwbuild/native/plugins/<name>/<name>_runtime.so, <name>_editor.so
// (native and cart builds compile enabled runtime plugins into the game).
// editor: the project editor as a dual cart, .cwbuild/cart-wgpu/carts/<app>_editor.wasc:
// the editor with the project's enabled plugins compiled in (a cart cannot
// load plugin code at runtime) and the project as its bundled files, built
// only on request in the game's cart tree (every cart is the dual one).
//
// stdout: the build's own lines, then one line per target:
//   @result {"target":"native","ok":true,"path":".cwbuild/native/project/app"}
//   @result {"target":"cart","ok":false,"error":"..."}
// Exit status 0 only when every target built. The editor's project.build
// runs exactly this.
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { withBuildLock } from './lib/build-lock.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const [cmd, dirArg, targetArg] = process.argv.slice(2);

function usage() {
  console.error('usage: node tools/cwproject.mjs build <project dir> [native|cart|all|plugins|editor]\n       node tools/cwproject.mjs info <project dir>');
  process.exit(2);
}

/** @param {string} dir */
export function readProject(dir) {
  const file = join(dir, 'project.cwproj');
  if (!existsSync(file)) throw new Error(`${dir} is not a cartwheel project: it has no project.cwproj`);
  /** @type {Record<string,string>} */
  const keys = {};
  for (const raw of readFileSync(file, 'utf8').split('\n')) {
    const line = raw.trim();
    if (!line || line[0] === '#' || line[0] === ';' || line[0] === '[') continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    let v = line.slice(eq + 1).trim();
    if (v.length >= 2 && v[0] === '"' && v.endsWith('"')) v = v.slice(1, -1);
    keys[line.slice(0, eq).trim()] = v;
  }
  return keys;
}

/** Set keys in project.cwproj, keeping every other line as it is.
 * @param {string} dir @param {Record<string,string>} set */
export function writeProjectKeys(dir, set) {
  const file = join(dir, 'project.cwproj');
  const lines = readFileSync(file, 'utf8').replace(/\n$/, '').split('\n');
  const left = new Set(Object.keys(set));
  for (let i = 0; i < lines.length; i++) {
    const eq = lines[i].indexOf('=');
    const t = lines[i].trim();
    if (eq < 0 || t[0] === '#' || t[0] === ';') continue;
    const k = lines[i].slice(0, eq).trim();
    if (left.has(k)) { lines[i] = `${k} = ${set[k]}`; left.delete(k); }
  }
  for (const k of left) lines.push(`${k} = ${set[k]}`);
  writeFileSync(file, lines.join('\n') + '\n');
}

/** @param {string} c @param {string[]} a @param {string} [cwd] */
function run(c, a, cwd) {
  return new Promise((res) => {
    const p = spawn(c, a, { cwd, stdio: ['ignore', 'pipe', 'pipe'] });
    const pipe = (d) => process.stdout.write(d);
    p.stdout.on('data', pipe);
    p.stderr.on('data', pipe);
    p.on('error', (e) => { process.stdout.write(`${c}: ${e.message}\n`); res(127); });
    p.on('close', (code) => res(code ?? 1));
  });
}

/** the build tree under .cwbuild/ of a target: every cart is the dual one (cart-wgpu)
 * @param {'native'|'cart'} target */
const treeDir = (target) => (target === 'cart' ? 'cart-wgpu' : target);

const launcher = spawnSync('ccache', ['--version'], { stdio: 'ignore' }).status === 0
  ? ['-DCMAKE_C_COMPILER_LAUNCHER=ccache', '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache'] : [];


/** @param {string} dir @param {'native'|'cart'} target @param {Record<string,string>} proj @param {string} [ninjaTarget] */
async function buildTarget(dir, target, proj, ninjaTarget) {
  if (target !== 'native') {
    // a cart tree packs with the native tree's cwpack (a host tool): built
    // first, under the native tree's own lock
    const cwpack = join(dir, '.cwbuild', 'native', 'tools', 'cwpack', 'cwpack');
    if (!existsSync(cwpack)) {
      const r = await buildTarget(dir, 'native', proj);
      if (!r.ok) return { target, ok: false, error: `the native tree (for its cwpack) failed: ${r.error}` };
    }
  }
  return withBuildLock(join(dir, '.cwbuild', treeDir(target)), () => buildTargetLocked(dir, target, proj, ninjaTarget));
}

/** @param {string} dir @param {'native'|'cart'} target @param {Record<string,string>} proj @param {string} [ninjaTarget] */
async function buildTargetLocked(dir, target, proj, ninjaTarget) {
  const bdir = join(dir, '.cwbuild', treeDir(target));
  const app = ninjaTarget || proj.app;
  const args = ['-S', ROOT, '-B', bdir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', `-DCW_PROJECT_DIR=${dir}`, '-DCW_ENGINE_ONLY=ON', ...launcher];
  if (target !== 'native') {
    const cwpack = join(dir, '.cwbuild', 'native', 'tools', 'cwpack', 'cwpack');
    args.push(`-DCMAKE_TOOLCHAIN_FILE=${join(ROOT, 'cmake', 'cart.cmake')}`, `-DCW_CWPACK=${cwpack}`);
    if (target === 'cart') args.push('-DCW_WGPU=ON'); // the dual cart
  }
  console.log(`== ${target}: configure ${bdir}`);
  if (await run('cmake', args)) return { target, ok: false, error: `cmake configure failed for ${target} (see the lines above)` };
  console.log(`== ${target}: build`);
  if (await run('ninja', ['-C', bdir, app])) return { target, ok: false, error: `the ${target} build failed (see the compiler output above)` };
  if (ninjaTarget === 'cwplugins_all') return { target, ok: true, path: join('.cwbuild', 'native', 'plugins') };
  if (ninjaTarget) {
    const p = join('.cwbuild', treeDir(target), 'carts', `${ninjaTarget}.wasc`);
    return existsSync(join(dir, p)) ? { target, ok: true, path: p } : { target, ok: false, error: `the build finished but ${p} is missing` };
  }
  const path = target === 'native' ? join('.cwbuild', 'native', 'project', app) : join('.cwbuild', 'cart-wgpu', 'carts', `${app}.wasc`);
  if (!existsSync(join(dir, path))) return { target, ok: false, error: `the build finished but ${path} is missing` };
  return { target, ok: true, path };
}

async function build() {
  if (!dirArg) usage();
  const dir = resolve(dirArg);
  const proj = readProject(dir);
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(proj.app || ''))
    throw new Error('project.cwproj: set `app = <name>` (a C identifier: the executable and cart name)');
  const which = targetArg || 'all';
  if (!['native', 'cart', 'all', 'plugins', 'editor'].includes(which)) usage();
  if (which === 'editor') {
    if (!existsSync(join(dir, 'plugins'))) throw new Error('the project has no plugins/ directory: the stock editor cart (editor.wasc) is its editor');
    const r = await buildTarget(dir, 'cart', proj, `${proj.app}_editor`);
    console.log('@result ' + JSON.stringify({ ...r, target: 'editor' }));
    process.exit(r.ok ? 0 : 1);
  }
  if (which === 'plugins') {
    const r = await buildTarget(dir, 'native', proj, 'cwplugins_all');
    console.log('@result ' + JSON.stringify({ ...r, target: 'plugins' }));
    process.exit(r.ok ? 0 : 1);
  }
  const targets = which === 'all' ? ['native', 'cart'] : [which];
  let ok = true;
  for (const t of targets) {
    const r = await buildTarget(dir, /** @type {'native'|'cart'} */ (t), proj);
    if (r.ok) writeProjectKeys(dir, t === 'native' ? { game_exe: r.path } : { game_cart: r.path });
    ok &&= r.ok;
    console.log('@result ' + JSON.stringify(r));
  }
  process.exit(ok ? 0 : 1);
}

if (cmd === 'build') build().catch((e) => { console.log('@result ' + JSON.stringify({ ok: false, error: e.message })); process.exit(1); });
else if (cmd === 'info') {
  if (!dirArg) usage();
  try { console.log(JSON.stringify(readProject(resolve(dirArg)), null, 1)); } catch (e) { console.error(e.message); process.exit(1); }
} else usage();
