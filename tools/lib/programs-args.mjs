// tools/programs.mjs's arguments that need more than a lookup: the scene runs (--scenes, --scene) and which tree a
// --carts survey reads (its carts, gates and tables). Kept apart so tests/programs-args.mjs checks them without a GPU.
import { existsSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';

/**
 * The environments each app runs once with: one object per run ({} when no scene was asked for).
 *   --scenes NAME=a,b,c     a run per value, split at commas
 *   --scenes 'NAME=a;b'     split at semicolons instead when the list has one, so a value keeps its commas
 *                           ("HEX_BOARD_SEATS=-1,0,1,2;-2,-2" is two runs, each with four or two seats)
 *   --scene NAME=value      one run with the whole value, commas and semicolons kept; give it as often as needed
 *                           (several names may differ: each --scene is its own run)
 * @param {string[]} argv
 * @returns {Record<string, string>[]}
 */
export function sceneEnvs(argv) {
  const runs = [];
  const nameOf = (s, flag) => {
    const eq = s.indexOf('=');
    if (eq < 1 || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(s.slice(0, eq))) throw new Error(`${flag} wants NAME=value (got '${s}')`);
    return [s.slice(0, eq), s.slice(eq + 1)];
  };
  argv.forEach((a, i) => {
    if (a !== '--scenes' && a !== '--scene') return;
    const v = argv[i + 1];
    if (v === undefined || v.startsWith('--')) throw new Error(`${a} wants NAME=value`);
    const [name, rest] = nameOf(v, a);
    const values = a === '--scene' ? [rest] : rest.split(rest.includes(';') ? ';' : ',');
    if (values.some((x) => x === '')) throw new Error(`${a} '${v}' has an empty value`);
    for (const x of values) runs.push({ [name]: x });
  });
  return runs.length ? runs : [{}];
}

/** a short label for a scene run, for logs and failures */
export const sceneLabel = (env) => Object.entries(env).map(([k, v]) => `${k}=${v}`).join(' ');

/**
 * The project a survey belongs to and the build whose app list (build/<dir>/apps/*.json: each app's tables) it reads.
 *   CW_PROJECT_ROOT names the project (an override); else --build <project>/build/native names it; else, for --carts,
 *   the nearest directory at or above the working directory with a build (build/cart-wgpu/carts, build/cart-wgpu/apps
 *   or build/native/apps: a game's own checkout, built against a prebuilt SDK whose tools sit in its deps/), looking
 *   no higher than the checkout's root (the first directory with a .git), else the engine tree itself.
 *   The build: --build when given; for a native survey this tree's build/native (as before); for --carts the
 *   project's build/native when it has an app list, else its build/cart-wgpu (a project that builds only carts).
 * @param {{ root: string, cwd: string, env: Record<string, string | undefined>, build: string | null, carts: boolean }} o
 * @returns {{ proj: string, build: string }}
 */
const BUILT = [['build', 'cart-wgpu', 'carts'], ['build', 'cart-wgpu', 'apps'], ['build', 'native', 'apps']];
export function surveyTree({ root, cwd, env, build, carts }) {
  const named = build ? resolve(build) : null;
  let proj = root;
  if (env.CW_PROJECT_ROOT) proj = resolve(env.CW_PROJECT_ROOT);
  else if (named && basename(named) === 'native' && basename(dirname(named)) === 'build') proj = dirname(dirname(named));
  else if (carts && !named) {
    for (let d = resolve(cwd); ; d = dirname(d)) {
      if (BUILT.some((p) => existsSync(join(d, ...p)))) { proj = d; break; }
      if (existsSync(join(d, '.git')) || dirname(d) === d) break; // a checkout's root: never a parent tree's build
    }
  }
  let out = named;
  if (!out && !carts) out = join(root, 'build', 'native'); // a native survey: this tree's build, as before
  if (!out) {
    const nat = join(proj, 'build', 'native');
    out = carts && !existsSync(join(nat, 'apps')) && existsSync(join(proj, 'build', 'cart-wgpu', 'apps')) ? join(proj, 'build', 'cart-wgpu') : nat;
  }
  return { proj, build: out };
}
