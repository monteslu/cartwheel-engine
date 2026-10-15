// three.c program tables (cmake/programs.cmake): find the r186 program states
// apps draw that their tables lack, add them to the tables' manifests and
// regenerate the C with three.c's tools/gen-project-table.mjs.
//
//   node tools/programs.mjs [app ...] [--gates] [--frames N] [--gl-only] [--logs <dir> ...] [--no-run]
//        [--build <dir>] [--check]
//       runs every native app (or the named ones) for N frames (default 120)
//       on GL and on WebGPU with CW_PROGRAMS_LOG_DIR set, so each writes the
//       states it could not draw to <logs>/<app>.log; --gates also runs every
//       gate's native runs (inputs, assets, controls); --logs also reads logs
//       other runs left (a gate run with CW_PROGRAMS_LOG_DIR=<dir>), --no-run
//       only those. Each app's states go to its first table (cw_add_app
//       PROGRAMS, or <its dir>/programs/<app>.txt, created when the app has
//       none: re-run cmake to pick a new one up). The changed tables are
//       regenerated.
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
// Regenerating minimizes the manifest (states another listed state covers
// are dropped: more lights of each kind, the same casters), captures what the
// capture cache (<build>/programs-cache/<table>) lacks twice to check it
// (--check off with --fast), and writes <table>_gl.c and <table>_wgpu.c beside
// the manifest. Capturing runs three.js r186's node renderer on three.c's mock
// GPU (byte-identical to a real device): Node only, no GPU. Running the apps
// does use the test GPU, inside the GPU sandbox.
import { spawnSync } from 'node:child_process';
import { copyFileSync, cpSync, existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync, rmSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from './lib/gpu-sandbox.mjs';
import { testEnv, wgpuPin } from './lib/wgpu-pin.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const one = (k) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : null; };
const all = (k) => argv.flatMap((a, i) => (a === k ? [argv[i + 1]] : []));
const VALUED = new Set(['--frames', '--logs', '--build', '--table', '--missing']);
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
const statesOf = (f) => (existsSync(f) ? readFileSync(f, 'utf8').split('\n').map((l) => l.replace(/#.*/, '').trim()).filter(Boolean) : []);

/** regenerate one table from its manifest (plus missing logs) */
function regenerate(manifest, logs = []) {
  const name = basename(manifest, '.txt');
  if (!existsSync(GEN)) throw new Error(`${GEN} not found: CW_THREE_DIR must be a three.c with tools/gen-project-table.mjs`);
  if (!statesOf(manifest).length && !logs.some((l) => statesOf(l).length)) {
    console.log(`${name}: no states, nothing to generate`);
    return;
  }
  const cache = join(BUILD, 'programs-cache', name);
  const args = [GEN, '--name', name, '--out', dirname(manifest), '--states', manifest, '--cache', cache, '--minimize',
    ...logs.flatMap((l) => ['--missing', l]), ...EXTENSIONS.flatMap((e) => ['--extend', e]),
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
const logDirs = all('--logs').map((d) => resolve(d));
const wgpu = /^CW_WGPU:BOOL=ON$/m.test(existsSync(join(BUILD, 'CMakeCache.txt')) ? readFileSync(join(BUILD, 'CMakeCache.txt'), 'utf8') : '');
const list = apps().filter((a) => !names.length || names.includes(a.name)).filter((a) => existsSync(a.binary));
if (names.length && list.length !== names.length)
  throw new Error(`not built: ${names.filter((n) => !list.some((a) => a.name === n)).join(' ')}`);

if (!argv.includes('--no-run')) {
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
