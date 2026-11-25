// tools/programs.mjs's scene runs and project lookup (tools/lib/programs-args.mjs), no GPU:
//   --scenes 'NAME=a,b;c' keeps the commas inside each value, --scene NAME=value is one run with the whole value,
//   --scenes NAME=a,b,c still splits at commas; --carts with no --build and no CW_PROJECT_ROOT finds the game's
//   checkout from the working directory and its app list (build/native, else build/cart-wgpu).
//   CONTROLS (must fail the same checks): the parser and the lookup as they were before v0.84.0, inline here.
import { mkdirSync, rmSync } from 'node:fs';
import { join, resolve, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { sceneEnvs, surveyTree } from '../tools/lib/programs-args.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
let bad = 0;
const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : ': ' + extra}`); if (!ok) bad++; };
const J = JSON.stringify;

// the old --scenes parse (v0.83): one flag, a regex that refuses nothing useful, values split at every comma
const oldScenes = (argv) => {
  const i = argv.indexOf('--scenes');
  const sc = i >= 0 ? argv[i + 1] : null;
  if (sc && !/^[A-Za-z_][A-Za-z0-9_]*=[^,]+(,[^,]+)*$/.test(sc)) throw new Error('--scenes wants NAME=a,b,c');
  return sc ? sc.slice(sc.indexOf('=') + 1).split(',').map((v) => ({ [sc.slice(0, sc.indexOf('='))]: v })) : [{}];
};
// the old lookup: the engine tree unless CW_PROJECT_ROOT or --build <project>/build/native
const oldTree = ({ root, env, build }) => {
  const B = resolve(build || join(root, 'build', 'native'));
  const proj = env.CW_PROJECT_ROOT ? resolve(env.CW_PROJECT_ROOT)
    : build && basename(B) === 'native' && basename(dirname(B)) === 'build' ? dirname(dirname(B)) : root;
  return { proj, build: B };
};

const seats = ['--scenes', 'HEX_BOARD_SEATS=-1,0,1,2;-2,-2'];
const one = ['--scene', 'SEATS=-1,0,1,2', '--scene', 'MODE=a;b'];
const plain = ['--scenes', 'LEVEL=1,2,3'];
const wantSeats = [{ HEX_BOARD_SEATS: '-1,0,1,2' }, { HEX_BOARD_SEATS: '-2,-2' }];
const wantOne = [{ SEATS: '-1,0,1,2' }, { MODE: 'a;b' }];
const wantPlain = [{ LEVEL: '1' }, { LEVEL: '2' }, { LEVEL: '3' }];
const scenesOk = (fn) => {
  const r = [];
  try { r.push(J(fn(seats)) === J(wantSeats), J(fn(one)) === J(wantOne), J(fn(plain)) === J(wantPlain), J(fn([])) === J([{}])); }
  catch (e) { r.push(false); }
  return r.every(Boolean);
};
check('--scenes with ";" keeps commas, --scene is one whole value, plain commas still split',
  scenesOk(sceneEnvs), `${J(sceneEnvs(seats))} ${J(sceneEnvs(one))} ${J(sceneEnvs(plain))}`);
let threw = false;
try { sceneEnvs(['--scenes', 'NOEQUALS']); } catch { threw = true; }
check('a scene without NAME= is refused', threw);
check('CONTROL: the v0.83 parser fails the same checks (it splits -1,0,1,2 into four runs)', !scenesOk(oldScenes));

// a game checkout built against an SDK: carts in build/cart-wgpu/carts, its app list in build/cart-wgpu/apps only
const fx = join(ROOT, 'out', 'programs-args-fixture');
rmSync(fx, { recursive: true, force: true });
mkdirSync(join(fx, 'build', 'cart-wgpu', 'carts'), { recursive: true });
mkdirSync(join(fx, 'build', 'cart-wgpu', 'apps'), { recursive: true });
mkdirSync(join(fx, 'src', 'deep'), { recursive: true });
const cwd = join(fx, 'src', 'deep');
const treeOk = (fn) => {
  const a = fn({ root: ROOT, cwd, env: {}, build: null, carts: true });
  const n = fn({ root: ROOT, cwd, env: {}, build: null, carts: false });
  const e = fn({ root: ROOT, cwd: ROOT, env: { CW_PROJECT_ROOT: fx }, build: null, carts: true });
  return a.proj === fx && a.build === join(fx, 'build', 'cart-wgpu') && n.proj === ROOT && n.build === join(ROOT, 'build', 'native')
    && e.proj === fx && e.build === join(fx, 'build', 'cart-wgpu');
};
check('--carts from inside a game checkout finds it (and its cart app list) with no --build; a native survey is unchanged',
  treeOk(surveyTree), J(surveyTree({ root: ROOT, cwd, env: {}, build: null, carts: true })));
mkdirSync(join(fx, 'build', 'native', 'apps'), { recursive: true });
const withNative = surveyTree({ root: ROOT, cwd, env: {}, build: null, carts: true });
check('a checkout with a native build reads that app list', withNative.build === join(fx, 'build', 'native'), J(withNative));
const named = surveyTree({ root: ROOT, cwd, env: {}, build: join(ROOT, 'build', 'native'), carts: true });
check('--build still wins', named.proj === ROOT && named.build === join(ROOT, 'build', 'native'), J(named));
rmSync(join(fx, 'build', 'native'), { recursive: true, force: true });
check('CONTROL: the v0.83 lookup fails the same check (the engine tree, not the game)', !treeOk(oldTree));
rmSync(fx, { recursive: true, force: true });

console.log(bad ? `programs-args: ${bad} FAILED` : 'programs-args: all passed');
process.exit(bad ? 1 : 0);
