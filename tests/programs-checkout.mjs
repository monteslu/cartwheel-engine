// tools/programs.mjs --carts run the way a game runs it, no GPU: from inside a game's checkout, with the SDK's copy of
// the tools (deps/cartwheel-sdk/<target>/tools/), no --build and no CW_PROJECT_ROOT. The survey must find the game's
// carts (build/cart-wgpu/carts) and app list (build/native/apps), start every run without crashing (v0.84.0 threw
// "Cannot access 'runs' before initialization" building its run list) and hand each run-cart the game's tree: a
// run-cart left to its own default looks in the SDK, which holds none of the game's carts.
//   The fixture: a scratch checkout under out/ holding a copy of this tree's tools as its SDK, with tools/run-cart.mjs
//   replaced by a stand-in that finds the cart exactly as the real one does (CW_PROJECT_ROOT, else the tree it sits
//   in: checked against the real run-cart.mjs below) and answers with a pipeline reply, so no GPU or host is needed.
//   CONTROLS (must fail): the stand-in refuses a run not told the game's tree; and, when this checkout has the
//   v0.84.0 commit, that release's programs.mjs fails the same survey (CW_PROGRAMS_CONTROL=<tools dir> names
//   another tools tree to run as the control).
import { spawnSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
let bad = 0;
const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : ': ' + extra}`); if (!ok) bad++; };
const V0840 = '273c3fb4d76f5cae814cbebdf997047ba5f154af';

// the stand-in must find the cart as the real run-cart does, or a pass here proves nothing about the real one
const real = readFileSync(join(ROOT, 'tools', 'run-cart.mjs'), 'utf8');
check('tools/run-cart.mjs finds its project as the stand-in does (CW_PROJECT_ROOT, else its own tree; build/cart-wgpu/carts/<app>.wasc)',
  real.includes("const PROJ = process.env.CW_PROJECT_ROOT ? resolve(process.env.CW_PROJECT_ROOT) : ROOT;")
  && real.includes("join(PROJ, 'build/cart-wgpu/carts', `${app}.wasc`)"));
const STAND_IN = `// a stand-in for run-cart.mjs (tests/programs-checkout.mjs): the real one's cart lookup, then a pipeline reply
import { existsSync, readFileSync, writeFileSync, appendFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PROJ = process.env.CW_PROJECT_ROOT ? resolve(process.env.CW_PROJECT_ROOT) : ROOT;
const a = process.argv.slice(2), one = (k) => a[a.indexOf(k) + 1];
const app = one('--app');
const cart = join(PROJ, 'build/cart-wgpu/carts', app + '.wasc');
if (!existsSync(cart)) { console.error('run-cart: no cart ' + cart); process.exit(1); }
if (readFileSync(cart, 'utf8') !== 'game cart ' + app + '\\n') { console.error('run-cart: not the game\\'s cart: ' + cart); process.exit(1); }
appendFileSync(join(PROJ, 'out', 'runs.txt'), a.join(' ') + '\\n');
const env = a.includes('--env') ? JSON.parse(one('--env')) : {};
writeFileSync(one('--cmd-out'), JSON.stringify([{ reply: { passes: [{ ran: true }],
  missing_states: 'standard_' + app + (env.SEATS ? '_seats' + env.SEATS.split(',').length : ''), dropped_values: '' } }]));
`;

/** a game checkout built against an SDK whose tools are `tools` (a directory like this tree's tools/) */
function checkout(dir, tools) {
  rmSync(dir, { recursive: true, force: true });
  const sdk = join(dir, 'deps', 'cartwheel-sdk', 'linux-x64');
  cpSync(tools, join(sdk, 'tools'), { recursive: true });
  writeFileSync(join(sdk, 'tools', 'run-cart.mjs'), STAND_IN);
  // the engine's generator extensions (programs.mjs reads src/*/extensions), as the SDK ships them
  for (const d of readdirSync(join(ROOT, 'src'), { withFileTypes: true }).filter((x) => x.isDirectory())) {
    const ext = join(ROOT, 'src', d.name, 'extensions');
    if (existsSync(ext)) cpSync(ext, join(sdk, 'src', d.name, 'extensions'), { recursive: true });
  }
  mkdirSync(join(sdk, 'src'), { recursive: true });
  writeFileSync(join(sdk, 'manifest.json'), JSON.stringify({ name: 'cartwheel-sdk', target: 'linux-x64', wgpu: true }) + '\n');
  // the game: a .git (its checkout's root), its native app list and its dual carts; the SDK has none of its carts
  mkdirSync(join(dir, '.git'), { recursive: true });
  mkdirSync(join(dir, 'games', 'boardgame', 'src'), { recursive: true });
  mkdirSync(join(dir, 'build', 'native', 'apps'), { recursive: true });
  mkdirSync(join(dir, 'build', 'native', 'games', 'boardgame'), { recursive: true });
  mkdirSync(join(dir, 'build', 'cart-wgpu', 'carts'), { recursive: true });
  mkdirSync(join(dir, 'out'), { recursive: true });
  for (const app of ['boardgame', 'boardgame_auto']) {
    const bin = join(dir, 'build', 'native', 'games', 'boardgame', app);
    writeFileSync(bin, '');
    writeFileSync(join(dir, 'build', 'native', 'apps', `${app}.json`),
      JSON.stringify({ name: app, binary: bin, dir: join(dir, 'games', 'boardgame'), programs: [] }) + '\n');
    writeFileSync(join(dir, 'build', 'cart-wgpu', 'carts', `${app}.wasc`), `game cart ${app}\n`);
  }
  return sdk;
}

/** programs.mjs --carts from `cwd` with no CW_PROJECT_ROOT, as a game's checkout runs it */
function survey(dir, sdk, cwd, extra = []) {
  rmSync(join(dir, 'out', 'programs-survey.json'), { force: true });
  rmSync(join(dir, 'out', 'runs.txt'), { force: true });
  writeFileSync(join(dir, 'out', 'runs.txt'), '');
  const env = { ...process.env, CW_TEST_GPU: 'desktop' };
  delete env.CW_PROJECT_ROOT;
  delete env.CW_GPU_SANDBOXED;
  const r = spawnSync(process.execPath, [join(sdk, 'tools', 'programs.mjs'), '--carts', '--frames', '4', '--no-switch', '--dry', ...extra],
    { cwd, encoding: 'utf8', timeout: 120000, env });
  let out = null;
  try { out = JSON.parse(readFileSync(join(dir, 'out', 'programs-survey.json'), 'utf8')); } catch {}
  const runs = readFileSync(join(dir, 'out', 'runs.txt'), 'utf8').split('\n').filter(Boolean);
  return { r, out, runs, text: (r.stdout + r.stderr).trim() };
}
const passes = (s, dir, scenes) => s.r.status === 0 && s.out !== null
  && s.text.includes(`surveying 2 dual cart(s) from ${join(dir, 'build', 'cart-wgpu', 'carts')}`)
  && new RegExp(`cart boardgame: ${scenes ? 2 : 1} state\\(s\\) no table has \\(([1-9][0-9]*) of \\1 run\\(s\\) surveyed\\)`).test(s.text)
  && JSON.stringify(s.out.boardgame) === JSON.stringify(scenes ? ['standard_boardgame_seats2', 'standard_boardgame_seats4'] : ['standard_boardgame'])
  && JSON.stringify(s.out.boardgame_auto) === JSON.stringify(scenes ? ['standard_boardgame_auto_seats2', 'standard_boardgame_auto_seats4'] : ['standard_boardgame_auto'])
  && s.runs.some((l) => l.includes('--warmup-spread')) && s.runs.some((l) => l.includes('quality verylow'));

const dir = join(ROOT, 'out', 'programs-checkout-fixture');
const sdk = checkout(dir, join(ROOT, 'tools'));
const top = survey(dir, sdk, dir);
check('--carts from the root of a game checkout finds its carts and surveys every run (no --build, no CW_PROJECT_ROOT)',
  passes(top, dir, false), `exit ${top.r.status}: ${top.text.slice(-600)}`);
const deep = survey(dir, sdk, join(dir, 'games', 'boardgame', 'src'), ['--scene', 'SEATS=-1,0', '--scene', 'SEATS=-1,0,1,2']);
check('--carts from a directory inside the checkout finds it too, and each --scene run reaches the cart',
  passes(deep, dir, true) && deep.runs.every((l) => l.includes('--env')), `exit ${deep.r.status}: ${deep.text.slice(-600)}`);
// the stand-in refuses a run that was not told the game's tree (what a run-cart left to its default does)
const env = { ...process.env };
delete env.CW_PROJECT_ROOT;
const lone = spawnSync(process.execPath, [join(sdk, 'tools', 'run-cart.mjs'), '--host', 'romdev', '--app', 'boardgame', '--cmd-out', join(dir, 'out', 'x.json')], { cwd: dir, encoding: 'utf8', env });
check('CONTROL: run-cart with no CW_PROJECT_ROOT looks in the SDK and finds no cart', lone.status !== 0 && /no cart .*cartwheel-sdk/.test(lone.stderr), lone.stderr);
rmSync(dir, { recursive: true, force: true });

// the release this gate was written for: its programs.mjs crashes on its run list and sends run-cart to the SDK
let control = process.env.CW_PROGRAMS_CONTROL ? resolve(process.env.CW_PROGRAMS_CONTROL) : null;
const ctlDir = join(ROOT, 'out', 'programs-checkout-control');
if (!control && spawnSync('git', ['-C', ROOT, 'cat-file', '-e', `${V0840}^{commit}`]).status === 0) {
  rmSync(ctlDir, { recursive: true, force: true });
  cpSync(join(ROOT, 'tools'), join(ctlDir, 'tools'), { recursive: true });
  for (const f of ['tools/programs.mjs', 'tools/lib/programs-args.mjs'])
    writeFileSync(join(ctlDir, f), spawnSync('git', ['-C', ROOT, 'show', `${V0840}:${f}`], { encoding: 'utf8' }).stdout);
  control = join(ctlDir, 'tools');
}
const runControl = (name, tools) => {
  const cdir = join(ROOT, 'out', 'programs-checkout-fixture-control');
  const csdk = checkout(cdir, tools);
  const c = survey(cdir, csdk, cdir);
  check(`CONTROL: ${name} fails the same survey`, !passes(c, cdir, false), c.text.slice(-400));
  console.log(`  control: exit ${c.r.status}: ${c.text.split('\n').filter((l) => /Error|FAILED|surveyed nothing|on the (gl|webgpu) host/.test(l)).slice(0, 2).join(' | ')}`);
  rmSync(cdir, { recursive: true, force: true });
};
if (control) {
  runControl(process.env.CW_PROGRAMS_CONTROL ? `${control}'s programs.mjs` : "v0.84.0's programs.mjs (its run list throws)", control);
  if (!process.env.CW_PROGRAMS_CONTROL) {
    // the second bug alone: v0.84.0 with its run list mended still starts each run-cart without the game's tree
    const p = join(control, 'programs.mjs');
    writeFileSync(p, readFileSync(p, 'utf8').replace(/\bruns\.push\(/g, 'runs0.push('));
    runControl("v0.84.0's programs.mjs with its run list mended (run-cart looks in the SDK)", control);
  }
} else console.log('skip the v0.84.0 control: this checkout does not have that commit (a shallow clone)');
rmSync(ctlDir, { recursive: true, force: true });

console.log(bad ? `programs-checkout: ${bad} FAILED` : 'programs-checkout: all passed');
process.exit(bad ? 1 : 0);
