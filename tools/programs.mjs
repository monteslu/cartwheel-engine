// three.c program tables (cmake/programs.cmake): find the r186 program states
// apps draw that their tables lack, add them to the tables' manifests and
// regenerate the C with three.c's tools/gen-project-table.mjs.
//
//   node tools/programs.mjs [app ...] [--gates] [--frames N] [--gl-only] [--logs <dir> ...] [--no-run]
//        [--build <dir>] [--check] [--extend <module.mjs> ...]
//       runs every native app (or the named ones) for N frames (default 120)
//       on GL and on WebGPU with CW_PROGRAMS_LOG_DIR set, so each writes the
//       states it could not draw to <logs>/<app>.log; --gates also runs every
//       gate's native runs (inputs, assets, controls); --logs also reads logs
//       other runs left (a gate run with CW_PROGRAMS_LOG_DIR=<dir>), --no-run
//       only those. Each app's states go to its first table (cw_add_app
//       PROGRAMS, or <its dir>/programs/<app>.txt, created when the app has
//       none: re-run cmake to pick a new one up). The changed tables are
//       regenerated.
//   node tools/programs.mjs --carts [app ...] [--gates] [--frames N] [--gl-only]
//       the same survey on the app's DUAL cart (build/cart-wgpu/carts/<app>.wasc), the one that ships: a cart is
//       asked for states a native run is not (the host's quality and capabilities differ: no cascaded sun
//       shadows, say), so a game that surveys only natively can ship a red "no program" banner. Runs the cart on a GL
//       host (romdev's dual cart told to render with GL) and on a WebGPU host, headless, reads the states the cart
//       reports (debug field cw.three.missing_states) and merges them into the app's table like a native
//       survey; --gates also runs each gate's input scripts. Carts are named by their file, which is
//       the app's name (the cart must be built: scripts/build.sh).
//   node tools/programs.mjs ... --dry
//       only reports each app's missing states (out/programs-survey.json)
//   node tools/programs.mjs --projects [project dir ...]
//       the same for projects (editor/templates/*, docs/tutorials/*/project by
//       default): built with tools/cwproject.mjs, run natively, their states
//       into <project>/programs/<app>.txt
//   node tools/programs.mjs --table <manifest.txt> [--missing <log> ...]
//       regenerates one table (what `ninja <table>_programs` runs)
//   node tools/programs.mjs --all-tables
//       regenerates every table the build knows (`ninja programs`)
//
// --extend adds project-owned extensions, including composed x<project>_csm
// states, without copying project code into the engine. The module filename
// must match its exported extension name. Duplicate names are rejected.
//
// Regenerating minimizes the manifest (states another listed state covers
// are dropped: more lights of each kind, the same casters), captures what the
// capture cache (<build>/programs-cache/<table>) lacks twice to check it
// (--check off with --fast), and writes <table>_gl.c and <table>_wgpu.c beside
// the manifest. Capturing runs three.js r186's node renderer on three.c's mock
// GPU (byte-identical to a real device): Node only, no GPU. Running the apps
// does use the test GPU, inside the GPU sandbox.
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { copyFileSync, cpSync, existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync, rmSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { enterGpuSandbox } from './lib/gpu-sandbox.mjs';
import { testEnv, wgpuPin } from './lib/wgpu-pin.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const one = (k) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : null; };
const all = (k) => argv.flatMap((a, i) => (a === k ? [argv[i + 1]] : []));
const VALUED = new Set(['--frames', '--logs', '--build', '--table', '--missing', '--extend']);
const names = argv.filter((a, i) => !a.startsWith('--') && !VALUED.has(argv[i - 1]));
const BUILD = resolve(one('--build') || join(ROOT, 'build', 'native'));
const THREE = resolve(process.env.CW_THREE_DIR || cmakeCache('CW_THREE_DIR') || join(ROOT, 'deps', 'three.c'));
const GEN = join(THREE, 'tools', 'gen-project-table.mjs');

function cmakeCache(key) {
  try {
    const m = new RegExp(`^${key}:[A-Z]+=(.*)$`, 'm').exec(readFileSync(join(BUILD, 'CMakeCache.txt'), 'utf8'));
    return m ? m[1] : null;
  } catch { return null; }
}
// cartwheel's own three.c generator extensions (states x<name>): src/*/extensions/<name>.mjs, each module
// named as its extension (a file with an underscore is a helper they import, not an extension)
const EXTENSIONS = readdirSync(join(ROOT, 'src'), { withFileTypes: true }).filter((d) => d.isDirectory())
  .flatMap((d) => { const dir = join(ROOT, 'src', d.name, 'extensions'); return existsSync(dir) ? readdirSync(dir).filter((f) => /^[a-z0-9]+\.mjs$/.test(f)).map((f) => join(dir, f)) : []; });
// Project parts participate in composition and source-keyed capture caches.
for (const file of all('--extend')) {
  if (!file) throw new Error('--extend requires a module path');
  const path = resolve(file);
  if (!existsSync(path)) throw new Error(`extension module not found: ${path}`);
  if (!/^[a-z0-9]+\.mjs$/.test(basename(path))) throw new Error(`extension filename must be <name>.mjs: ${path}`);
  const previous = EXTENSIONS.find((e) => basename(e) === basename(path));
  if (previous && previous !== path) throw new Error(`duplicate extension ${basename(path)}: ${previous} and ${path}`);
  if (!previous) EXTENSIONS.push(path);
}
// and the helpers they import (their source is part of what a capture makes)
const EXTENSION_HELPERS = EXTENSIONS.length ? [...new Set(EXTENSIONS.map((e) => dirname(e)))]
  .flatMap((d) => readdirSync(d).filter((f) => f.endsWith('.mjs')).map((f) => join(d, f))).filter((f) => !EXTENSIONS.includes(f)) : [];
// composed extensions (cartwheel/shading.h): a state x<a>_<b> applies the parts a and b (each an
// extension above) in that order; its module is written into the build tree for the generator
function composedModules(stateFiles) {
  const names = new Set();
  for (const f of stateFiles) for (const s of statesOf(f)) for (const t of s.split('+')) if (/^x[a-z0-9]+(_[a-z0-9]+)+$/.test(t)) names.add(t.slice(1));
  if (!names.size) return [];
  const dir = join(BUILD, 'programs-ext');
  mkdirSync(dir, { recursive: true });
  return [...names].sort().map((n) => {
    const parts = n.split('_').map((p) => {
      const m = EXTENSIONS.find((e) => basename(e) === p + '.mjs');
      if (!m) throw new Error(`composed extension ${n}: no part ${p} (src/<module>/extensions/${p}.mjs or --extend ${p}.mjs)`);
      return m;
    });
    const file = join(dir, n + '.mjs');
    const body = "// GENERATED by tools/programs.mjs: the composed extension " + n + " (cartwheel/shading.h)\n"
      + parts.map((m, i) => `import * as p${i} from ${JSON.stringify(pathToFileURL(m).href)};\n`).join("")
      + `export const name = ${JSON.stringify(n)};\n`
      + "export async function apply(ctx) {\n" + parts.map((_, i) => `  await p${i}.apply(ctx);\n`).join("") + "}\n";
    if (!existsSync(file) || readFileSync(file, 'utf8') !== body) writeFileSync(file, body);
    return file;
  });
}
const statesOf = (f) => (existsSync(f) ? readFileSync(f, 'utf8').split('\n').map((l) => l.replace(/#.*/, '').trim()).filter(Boolean) : []);

/** regenerate one table from its manifest (plus missing logs) */
function regenerate(manifest, logs = []) {
  const name = basename(manifest, '.txt');
  if (!existsSync(GEN)) throw new Error(`${GEN} not found: CW_THREE_DIR must be a three.c with tools/gen-project-table.mjs`);
  if (!statesOf(manifest).length && !logs.some((l) => statesOf(l).length)) {
    console.log(`${name}: no states, nothing to generate`);
    return;
  }
  // the cache is keyed by state name only: captures made with other extension sources go to another one
  const extHash = createHash('sha256');
  for (const e of [...EXTENSIONS, ...EXTENSION_HELPERS].sort()) extHash.update(readFileSync(e));
  const cache = join(BUILD, 'programs-cache', EXTENSIONS.length ? `${name}-${extHash.digest('hex').slice(0, 12)}` : name);
  const args = [GEN, '--name', name, '--out', dirname(manifest), '--states', manifest, '--cache', cache, '--minimize',
    ...logs.flatMap((l) => ['--missing', l]), ...EXTENSIONS.flatMap((e) => ['--extend', e]),
    ...composedModules([manifest, ...logs]).flatMap((e) => ['--extend', e]),
    ...(argv.includes('--fast') ? [] : ['--check'])];
  console.log(`${name}: regenerating (${dirname(manifest)})`);
  const r = spawnSync(process.execPath, args, { stdio: 'inherit' }); // the capture runs on three.c's mock GPU: no device
  if (r.status !== 0) throw new Error(`${name}: gen-project-table failed (${r.signal || 'exit ' + r.status})`);
  // the capture cache sits in the build tree; the generated C beside the manifest, checked in
  rmSync(join(dirname(manifest), '.t3-captures'), { recursive: true, force: true });
}

const apps = () => {
  const dir = join(BUILD, 'apps');
  if (!existsSync(dir)) throw new Error(`${dir}: configure and build ${BUILD} first`);
  return readdirSync(dir).filter((f) => f.endsWith('.json')).map((f) => JSON.parse(readFileSync(join(dir, f), 'utf8')));
};

if (one('--table')) {
  regenerate(resolve(one('--table')), all('--missing').map((l) => resolve(l)));
  process.exit(0);
}
if (argv.includes('--all-tables')) {
  const tables = new Set([join(ROOT, 'src', 'render', 'programs', 'cw_engine.txt'), join(ROOT, 'editor', 'programs', 'cw_editor.txt')]);
  for (const a of apps()) for (const m of a.programs) tables.add(m);
  for (const m of tables) regenerate(m);
  process.exit(0);
}

// --projects [dir ...]: the projects outside the tree's build (editor/templates/*, docs/tutorials/*/project,
// or the named project directories), built with tools/cwproject.mjs and run natively; their states
// go to <project>/programs/<app>.txt, which cw_add_app picks up in the project's build
if (argv.includes('--projects')) {
  await enterGpuSandbox();
  const dirs = names.length ? names.map((d) => resolve(d))
    : [...readdirSync(join(ROOT, 'editor', 'templates')).map((t) => join(ROOT, 'editor', 'templates', t)),
       ...readdirSync(join(ROOT, 'docs', 'tutorials')).map((t) => join(ROOT, 'docs', 'tutorials', t, 'project'))]
      .filter((d) => existsSync(join(d, 'project.cwproj')));
  const logs = join(BUILD, 'programs-logs-projects');
  rmSync(logs, { recursive: true, force: true });
  mkdirSync(logs, { recursive: true });
  for (const d of dirs) {
    const app = (/^\s*app\s*=\s*"?([A-Za-z_][A-Za-z0-9_]*)/m.exec(readFileSync(join(d, 'project.cwproj'), 'utf8')) || [])[1];
    if (!app) { console.log(`${d}: no app in project.cwproj`); continue; }
    // built from a copy: a build writes into the project (project.cwproj's game_exe, .cwbuild/)
    const work = join(BUILD, 'programs-projects', basename(d) === 'project' ? basename(dirname(d)) : basename(d));
    rmSync(join(work, 'programs'), { recursive: true, force: true });
    cpSync(d, work, { recursive: true, filter: (p) => !/[\\/](\.cwbuild|\.cwcache|exports)([\\/]|$)/.test(p.slice(d.length)) });
    const b = spawnSync(process.execPath, [join(ROOT, 'tools', 'cwproject.mjs'), 'build', work, 'native'], { encoding: 'utf8', timeout: 3600000 });
    const exe = join(work, '.cwbuild', 'native', 'project', app);
    if (b.status !== 0 || !existsSync(exe)) { console.log(`${app}: the project build failed\n${(b.stdout + b.stderr).trim().split('\n').slice(-5).join('\n')}`); continue; }
    const legs = [{}, ...(argv.includes('--gl-only') ? [] : [{ CW_GPU: 'webgpu', ...wgpuPin().env }])];
    for (const extra of legs)
      spawnSync(exe, [], { cwd: dirname(exe), encoding: 'utf8', timeout: 180000,
        env: testEnv({ ...process.env, ...extra, CW_FRAMES: one('--frames') || '300', SDL_AUDIODRIVER: 'dummy', CW_PROGRAMS_LOG_DIR: logs }) });
    const log = join(logs, `${app}.log`);
    if (!statesOf(log).length) { console.log(`${app}: no missing states`); continue; }
    const manifest = join(d, 'programs', `${app}.txt`);
    mkdirSync(dirname(manifest), { recursive: true });
    if (!existsSync(manifest)) writeFileSync(manifest, `# r186 program states ${app} draws (cmake/programs.cmake, tools/programs.mjs)\n`);
    console.log(`${app}: ${new Set(statesOf(log)).size} missing state(s) -> ${manifest.replace(ROOT + '/', '')}`);
    if (!argv.includes('--dry')) regenerate(manifest, [log]);
  }
  process.exit(0);
}

// ── run the apps ──────────────────────────────────────────────────────
if (!argv.includes('--no-run')) await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
const frames = one('--frames') || '120';
const runDir = join(BUILD, 'programs-logs');
const CARTS = argv.includes('--carts');
const logDirs = all('--logs').map((d) => resolve(d));
const wgpu = /^CW_WGPU:BOOL=ON$/m.test(existsSync(join(BUILD, 'CMakeCache.txt')) ? readFileSync(join(BUILD, 'CMakeCache.txt'), 'utf8') : '');
const list = apps().filter((a) => !names.length || names.includes(a.name)).filter((a) => existsSync(a.binary));
if (names.length && list.length !== names.length)
  throw new Error(`not built: ${names.filter((n) => !list.some((a) => a.name === n)).join(' ')}`);

/** the states a cart run reported (its debug state file), one per line */
const cartStates = (stateFile) => {
  try {
    const st = JSON.parse(readFileSync(stateFile, 'utf8'));
    return Buffer.from(String(st['cw.three.missing_states'] || ''), 'hex').toString('latin1').replace(/\0.*$/s, '').split('\n').filter(Boolean);
  } catch { return []; }
};
if (CARTS && !argv.includes('--no-run')) {
  rmSync(runDir, { recursive: true, force: true });
  mkdirSync(runDir, { recursive: true });
  const cartDir = join(ROOT, 'build', 'cart-wgpu', 'carts');
  const have = existsSync(cartDir) ? readdirSync(cartDir).filter((f) => f.endsWith('.wasc')).map((f) => f.slice(0, -5)) : [];
  const todo = names.length ? names : have;
  for (const n of todo) if (!have.includes(n)) throw new Error(`no dual cart ${n} in ${cartDir} (build it: scripts/build.sh)`);
  const legs = [['gl', ['--gpu', 'gl', '--dual-gl']]];
  if (!argv.includes('--gl-only')) legs.push(['webgpu', ['--gpu', 'webgpu']]);
  const gates = argv.includes('--gates') ? JSON.parse(readFileSync(join(ROOT, 'tests', 'gates.json'), 'utf8')).filter((g) => g.app && !g.script) : [];
  for (const app of todo) {
    const runs = [{ tag: app, args: ['--frames', frames] }];
    for (const g of gates.filter((x) => x.app === app))
      runs.push({ tag: g.name, args: ['--frames', String(g.frames), ...(g.input ? ['--input', join(ROOT, 'tests', g.input)] : [])] });
    const seen = new Set();
    for (const run of runs) for (const [leg, legArgs] of legs) {
      const stateFile = join(runDir, `${run.tag}.${leg}.json`);
      const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'romdev', '--app', app, ...run.args, ...legArgs, '--state', stateFile],
        { encoding: 'utf8', timeout: 600000, env: testEnv({ ...process.env }) });
      if (r.status !== 0) console.log(`cart ${run.tag} (${leg}): run failed: ${(r.stderr || r.stdout || '').trim().split('\n').slice(-1)[0]}`);
      for (const st of cartStates(stateFile)) seen.add(st);
    }
    if (seen.size) writeFileSync(join(runDir, `${app}.log`), [...seen].join('\n') + '\n');
    console.log(`cart ${app}: ${seen.size} state(s) no table has`);
  }
  logDirs.push(runDir);
}
if (!CARTS && !argv.includes('--no-run')) {
  rmSync(runDir, { recursive: true, force: true });
  mkdirSync(runDir, { recursive: true });
  const legs = [['gl', {}]];
  if (wgpu && !argv.includes('--gl-only')) legs.push(['webgpu', { CW_GPU: 'webgpu', ...wgpuPin().env }]);
  for (const a of list) {
    for (const [leg, extra] of legs) {
      const r = spawnSync(a.binary, [], {
        cwd: dirname(a.binary), encoding: 'utf8', timeout: 180000,
        env: testEnv({ ...process.env, ...extra, CW_FRAMES: frames, SDL_AUDIODRIVER: 'dummy', CW_PROGRAMS_LOG_DIR: runDir }),
      });
      if (r.status !== 0) console.log(`${a.name} (${leg}): ${r.signal || 'exit ' + r.status}`);
    }
  }
  // --gates: also every gate's native runs (its input, assets and control variant), where most states show up
  if (argv.includes('--gates')) {
    const gates = JSON.parse(readFileSync(join(ROOT, 'tests', 'gates.json'), 'utf8'))
      .filter((g) => g.app && !g.script && (!names.length || names.includes(g.app)));
    for (const g of gates) {
      const base = ['--host', 'native', '--app', g.app, '--frames', String(g.frames), ...(g.frameHz ? ['--frame-hz', String(g.frameHz)] : []),
        ...(g.assets ? ['--assets', join(ROOT, g.assets)] : [])];
      const inp = (f) => (f ? ['--input', join(ROOT, 'tests', f)] : []);
      // "repo:<path>" names a file the run may write back (the editor saves its settings): a copy, as gate.mjs does
      const repo = (e) => Object.fromEntries(Object.entries(e || {}).map(([k, val]) => {
        if (typeof val !== 'string' || !val.startsWith('repo:')) return [k, val];
        const copy = join(runDir, g.name + '.' + basename(val.slice(5)));
        copyFileSync(join(ROOT, val.slice(5)), copy);
        return [k, copy];
      }));
      const variants = [{ args: inp(g.input), env: {} }];
      if (g.controlInput || g.controlEnv) variants.push({ args: inp(g.controlInput || g.input), env: repo(g.controlEnv) });
      const gpus = [...(g.gl === false ? [] : ['gl']), ...(g.webgpu && wgpu && !argv.includes('--gl-only') ? ['webgpu'] : [])];
      for (const v of variants)
        for (const gpu of gpus) {
          const env = { ...process.env, ...v.env, CW_PROGRAMS_LOG_DIR: runDir };
          const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), ...base, ...v.args, '--gpu', gpu], { env, encoding: 'utf8', timeout: 300000 });
          if (r.status !== 0) console.log(`gate ${g.name} (${gpu}): run failed: ${(r.stderr || '').trim().split('\n').slice(-1)[0]}`);
        }
    }
  }
  logDirs.push(runDir);
}

// --dry: report what each app misses (out/programs-survey.json), change nothing
if (argv.includes('--dry')) {
  const out = {};
  for (const a of list.length ? list : apps()) {
    const st = [...new Set(logDirs.map((d) => join(d, `${a.name}.log`)).flatMap(statesOf))].sort();
    if (st.length) out[a.name] = st;
  }
  mkdirSync(join(ROOT, 'out'), { recursive: true });
  writeFileSync(join(ROOT, 'out', 'programs-survey.json'), JSON.stringify(out, null, 1) + '\n');
  for (const [n, st] of Object.entries(out)) console.log(`${n}: ${st.length}`);
  console.log(`${new Set(Object.values(out).flat()).size} distinct states over ${Object.keys(out).length} apps`);
  process.exit(0);
}

// ── each app's states into its first table ──────────────────────────
const byTable = new Map(); // manifest -> [logs]
const created = [];
for (const a of list.length ? list : apps()) {
  const logs = logDirs.map((d) => join(d, `${a.name}.log`)).filter((l) => statesOf(l).length);
  if (!logs.length) continue;
  let manifest = a.programs[0];
  // the editor's apps (editor/) draw with the editor's table
  if (!manifest && a.dir === join(ROOT, 'editor')) manifest = join(ROOT, 'editor', 'programs', 'cw_editor.txt');
  if (!manifest) {
    manifest = join(a.dir, 'programs', `${a.name}.txt`);
    mkdirSync(dirname(manifest), { recursive: true });
    if (!existsSync(manifest)) {
      writeFileSync(manifest, `# r186 program states ${a.name} draws (cmake/programs.cmake, tools/programs.mjs)\n`);
      created.push(manifest);
    }
  }
  const n = new Set(logs.flatMap(statesOf)).size;
  console.log(`${a.name}: ${n} missing state(s) -> ${manifest.replace(ROOT + '/', '')}`);
  byTable.set(manifest, [...(byTable.get(manifest) || []), ...logs]);
}
for (const [manifest, logs] of byTable) regenerate(manifest, logs);
if (!byTable.size) console.log('no missing states');
if (created.length) console.log(`new tables (re-run cmake to link them):\n  ${created.map((m) => m.replace(ROOT + '/', '')).join('\n  ')}`);
