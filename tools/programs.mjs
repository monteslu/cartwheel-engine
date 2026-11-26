// three.c program tables (cmake/programs.cmake): find the r187 program states
// apps draw that their tables lack, add them to the tables' manifests and
// regenerate the C with three.c's tools/gen-project-table.mjs.
//
//   node tools/programs.mjs [app ...] [--gates] [--frames N] [--gl-only] [--logs <dir> ...] [--no-run]
//        [--build <dir>] [--check] [--extend <module.mjs> ...]
//       runs every native app (or the named ones) for N frames (default 120)
//       on GL and on WebGPU with CW_PROGRAMS_LOG_DIR set, so each writes the
//       states it could not draw to <logs>/<app>.log (a run that fails stops the survey with exit 1, no table changed;
//       --gates-file a.json,b.json reads those gate files in the project's tests/); --gates also runs every
//       gate's native runs (inputs, assets, controls); --logs also reads logs
//       other runs left (a gate run with CW_PROGRAMS_LOG_DIR=<dir>), --no-run
//       only those. Each app's states go to its first table (cw_add_app
//       PROGRAMS, or <its dir>/programs/<app>.txt, created when the app has
//       none: re-run cmake to pick a new one up). The changed tables are
//       regenerated.
//   node tools/programs.mjs --carts [app ...] [--gates] [--frames N] [--gl-only | --webgpu-only]
//       the same survey on the app's DUAL cart (build/cart-wgpu/carts/<app>.wasc), the one that ships: a cart is
//       asked for states a native run is not (the host's quality and capabilities differ: no cascaded sun
//       shadows, say), so a game that surveys only natively can ship a red "no program" banner. Runs the cart on a GL
//       host (romdev's dual cart told to render with GL) and on a WebGPU host, headless, reads the states the cart
//       reports (debug field cw.three.missing_states) and merges them into the app's table like a native
//       survey; --gates also runs each gate's input scripts. Carts are named by their file, which is
//       the app's name (the cart must be built: scripts/build.sh). --webgpu-only skips the GL host leg (a cart
//       that cannot be loaded on romdev's GL path is still surveyed where it ships on WebGPU). Each cart (and each gate's run with
//       --gates) is also run as a player's host runs it, with no seed (run-cart --warmup-spread): the warm-up under the
//       loading screen, which a deterministic run never shows.
//       Every run must survey something: one that fails, never renders a frame or has no state list to read (a cart
//       that draws without the engine's pipeline) stops the survey with exit 1, naming the app and the host, and no
//       table changes; no carts to survey is an error too (it names the directory it looked in).
//   --presets a,b   the graphics presets each native app run goes through (default every one: verylow, low, medium,
//       high, ultra; cartwheel/quality.h), since each asks for states of its own
//   Every survey (native and --carts) also switches the preset half way through a run, from High to each other
//   preset and from each back to High: what a game built on one preset draws on another (a player's settings
//   change, or Auto stepping down in play) asks for states neither preset asks for from the start (High's
//   lights on Very Low's shadows, say), and those draws vanished mid-game. --no-switch leaves these runs out.
//   --scenes NAME=a,b,c   runs each app once per value with environment NAME set to it (on GL and on WebGPU; with
//       --carts each cart run too), for a game that picks its scene from the environment or runs a different scene
//       per level: the table gets the union of every scene's states (a runtime-created light or particle needs its
//       state in the table, and a scene the run never visits is a skipped draw later). A list with a semicolon splits
//       there instead, so values keep their commas ('SEATS=-1,0,1;-2,-2'); --scene NAME=value (repeatable) is one run
//       with the whole value.
//   --carts finds the project from the working directory (the nearest directory with a build: build/cart-wgpu/carts,
//       build/cart-wgpu/apps or build/native/apps, no higher than the checkout's root) when neither CW_PROJECT_ROOT
//       (an override) nor --build names it, and reads its app list from its build/native, else its build/cart-wgpu:
//       a game's checkout runs node deps/cartwheel-sdk/<target>/tools/programs.mjs --carts with no --build and no
//       CW_PROJECT_ROOT. Every run-cart it starts is told the project (CW_PROJECT_ROOT in its environment), so it
//       loads the project's carts, not the SDK's.
//   node tools/programs.mjs ... --dry
//       only reports each app's missing states (the project's out/programs-survey.json)
//   node tools/programs.mjs --projects [project dir ...]
//       the same for projects (editor/templates/*, docs/tutorials/*/project by
//       default): built with tools/cwproject.mjs, run natively, their states
//       into <project>/programs/<app>.txt
//   node tools/programs.mjs --table <manifest.txt> [--missing <log> ...]
//       regenerates one table (what `ninja <table>_programs` runs); several --table regenerate each in its own
//       process, a few at once (the command the configure prints for out-of-date tables)
//   node tools/programs.mjs --all-tables
//       regenerates every table the build knows (`ninja programs`)
//   Every regeneration then compiles the table's WebGPU programs with Dawn's WGSL compiler (Tint) on
//   lavapipe (tools/lib/wgsl-check.mjs) and fails, naming the state and Tint's message, on any rejection
//   (--no-wgsl-check skips it). An engine checkout requires the check. A prebuilt SDK carries neither Dawn
//   nor webgpu-node, so there it warns and goes on unless WEBGPU_NODE=<dir> names one.
//   A manifest line "# extend: <path>" (relative to it) adds one of the project's own extension modules.
//
// --extend adds project-owned extensions, including composed x<project>_csm
// states, without copying project code into the engine. The module filename
// must match its exported extension name. Duplicate names are rejected.
//
// Regenerating adds the manifest's variant closure (tools/lib/program-closure.mjs): every surveyed material in every
// environment surveyed for its class (fog, shadows, the scene's environment, each preset's lighting parts, the most
// lights seen), so a preset switched in play, a level or a weather that only some materials were surveyed in draws
// with programs the table has. The states go to the manifest under a "# closure" line. A manifest line
// "# closure: off" leaves it out (a table several apps share, a test's table that must stay incomplete);
// "# closure-max: N" bounds it (default 3000: more fails, naming the counts). With the closure, the manifest is
// not minimized: three.c draws each light count with the smallest program that covers it.
// Without it, regenerating minimizes the manifest (states another listed state covers
// are dropped: more lights of each kind, the same casters), captures what the
// capture cache (<build>/programs-cache/<table>) lacks twice to check it
// (--check off with --fast), and writes <table>_gl.c and <table>_wgpu.c beside
// the manifest. Capturing runs three.js r187's node renderer on three.c's mock
// GPU (byte-identical to a real device): Node only, no GPU. Running the apps
// does use the test GPU, inside the GPU sandbox.
import { spawnSync } from 'node:child_process';
// the graphics presets every native survey run goes through (--presets narrows it)
const PRESETS = ['verylow', 'low', 'medium', 'high', 'ultra'];
import { createHash } from 'node:crypto';
import { copyFileSync, cpSync, existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync, rmSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { enterGpuSandbox } from './lib/gpu-sandbox.mjs';
import { testEnv, wgpuPin } from './lib/wgpu-pin.mjs';
import { checkWgsl } from './lib/wgsl-check.mjs';
import { tableStamp } from './lib/program-sources.mjs';
import { closure, featureOrder } from './lib/program-closure.mjs';
import { sceneEnvs, sceneLabel, surveyTree } from './lib/programs-args.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const one = (k) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : null; };
const all = (k) => argv.flatMap((a, i) => (a === k ? [argv[i + 1]] : []));
const VALUED = new Set(['--scenes', '--scene', '--presets', '--frames', '--logs', '--build', '--gates-file', '--table', '--missing', '--extend']);
const names = argv.filter((a, i) => !a.startsWith('--') && !VALUED.has(argv[i - 1]));
// CW_PROJECT_ROOT (as tools/run-cart.mjs reads it): the tree whose carts, gates, inputs and assets these are. Without it,
// --build <project>/build/native names the project: from an SDK, ROOT holds the engine and none of the game's files. A
// --carts survey with neither finds the project from the working directory (tools/lib/programs-args.mjs surveyTree)
const { proj: PROJ, build: BUILD } = surveyTree({ root: ROOT, cwd: process.cwd(), env: process.env, build: one('--build'), carts: argv.includes('--carts') });
// --gates-file a.json,b.json (or CW_GATES_FILE): gate files in the project's tests/, as tests/gate.mjs reads them
const gateFile = () => (one('--gates-file') || process.env.CW_GATES_FILE || 'gates.json').split(',')
  .flatMap((f) => JSON.parse(readFileSync(join(PROJ, 'tests', f.trim()), 'utf8')));
/** the line of a failed node run that says why (an uncaught error ends with the node version) */
const whyFailed = (r) => {
  const lines = (r.stderr || r.stdout || '').trim().split('\n');
  return lines.find((l) => /Error|Cannot find/.test(l))?.trim() || lines.at(-1) || r.signal || `exit ${r.status}`;
};
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
function composedModules(stateFiles, extensions = EXTENSIONS) {
  const names = new Set();
  for (const f of stateFiles) for (const s of statesOf(f)) for (const t of s.split('+')) if (/^x[a-z0-9]+(_[a-z0-9]+)+$/.test(t)) names.add(t.slice(1));
  if (!names.size) return [];
  const dir = join(BUILD, 'programs-ext');
  mkdirSync(dir, { recursive: true });
  return [...names].sort().map((n) => {
    const parts = n.split('_').map((p) => {
      const m = extensions.find((e) => basename(e) === p + '.mjs');
      if (!m) throw new Error(`composed extension ${n}: no part ${p} (src/<module>/extensions/${p}.mjs, --extend ${p}.mjs, or "# extend: <path>" in the manifest)`);
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
// A surface module (cartwheel/surface.h: it exports `surface`, its inputs, instead of apply) becomes a part
// through tools/lib/surface-part.mjs: a wrapper of the same name in the build tree, so the module imports nothing
// from the engine.
const SURFACE_PART = join(ROOT, 'tools', 'lib', 'surface-part.mjs');
const isSurface = (path) => /^export\s+(?:async\s+)?(?:function\s+surface\b|const\s+surface\b)/m.test(readFileSync(path, 'utf8'));
function asPart(path) {
  if (!isSurface(path)) return path;
  const dir = join(BUILD, 'programs-ext', 'surfaces');
  mkdirSync(dir, { recursive: true });
  const file = join(dir, basename(path));
  const body = "// GENERATED by tools/programs.mjs: the surface " + basename(path, '.mjs') + " as a shading part (cartwheel/surface.h)\n"
    + `import * as m from ${JSON.stringify(pathToFileURL(path).href)};\n`
    + `import { surfaceApply } from ${JSON.stringify(pathToFileURL(SURFACE_PART).href)};\n`
    + "export const name = m.name;\nexport const apply = surfaceApply(m);\n";
  if (!existsSync(file) || readFileSync(file, 'utf8') !== body) writeFileSync(file, body);
  return file;
}
const statesOf = (f) => (existsSync(f) ? readFileSync(f, 'utf8').split('\n').map((l) => l.replace(/#.*/, '').trim()).filter(Boolean) : []);

// A manifest names the project's own parts itself, so `ninja programs` and a bare --table need no flags:
//   # extend: ../shaders/foliage.mjs        (relative to the manifest)
function manifestExtensions(manifest) {
  if (!existsSync(manifest)) return [];
  return [...readFileSync(manifest, 'utf8').matchAll(/^#\s*extend:\s*(\S+)\s*$/gm)].map((m) => {
    const path = resolve(dirname(manifest), m[1]);
    if (!existsSync(path)) throw new Error(`${manifest}: extension module not found: ${path}`);
    if (!/^[a-z0-9]+\.mjs$/.test(basename(path))) throw new Error(`${manifest}: extension filename must be <name>.mjs: ${path}`);
    return path;
  });
}
// Lightmaps read through the second UV set since v0.25.0 (a lightmap's texture channel is part of the
// program): a table made before lists lmap states without it, and baked meshes would skip. Each gets its
// c1lmap state beside it, placed where three.c's state names put it (after the other features). Kept beside,
// not replaced: a game's own material may still read a lightmap through uv0.
function addLightmapChannelStates(manifest) {
  if (!existsSync(manifest)) return;
  const text = readFileSync(manifest, 'utf8');
  const have = new Set(statesOf(manifest));
  const add = [];
  for (const s of have) {
    const t = s.split('+');
    if (!t.includes('lmap') || t.includes('c1lmap')) continue;
    let at = t.findIndex((x, i) => i > 0 && (x === 'rdepth' || /^l\d{4}$/.test(x) || /^s\d{3}$/.test(x) || /^x[a-z0-9_]+$/.test(x)));
    if (at < 0) at = t.length;
    t.splice(at, 0, 'c1lmap');
    const n = t.join('+');
    if (!have.has(n) && !add.includes(n)) add.push(n);
  }
  if (!add.length) return;
  writeFileSync(manifest, text.replace(/\n?$/, '\n') + add.join('\n') + '\n');
  console.log(`${basename(manifest)}: added ${add.length} lightmap state(s) on the second UV set (c1lmap)`);
}
// Water3D draws with its cheaper "waterlow" program on effects Very Low (cartwheel/quality.h), and without the sun's
// shadow: a scene's water state on any other preset has that twin, whatever scene or mode the survey reached on Very
// Low. Each xwater state (from the manifest or this run's missing logs) gets it beside it: no shadow, no shadow
// casters, the water part as waterlow.
function addVeryLowWaterStates(manifest, logs) {
  if (!existsSync(manifest)) return;
  const text = readFileSync(manifest, 'utf8');
  const have = new Set(statesOf(manifest));
  const add = [];
  for (const st of [...have, ...logs.flatMap((l) => statesOf(l))]) {
    const t = st.split('+');
    const xi = t.findIndex((x) => /^x[a-z0-9_]+$/.test(x) && x.slice(1).split('_').includes('water'));
    if (xi < 0 || t[0] !== 'standard') continue;
    const n = t.filter((x) => x !== 'shadow' && !/^s\d{3}$/.test(x))
      .map((x) => (x === t[xi] ? 'x' + x.slice(1).split('_').map((p) => (p === 'water' ? 'waterlow' : p)).join('_') : x)).join('+');
    if (!have.has(n) && !add.includes(n)) add.push(n);
  }
  if (!add.length) return;
  writeFileSync(manifest, text.replace(/\n?$/, '\n') + add.join('\n') + '\n');
  console.log(`${basename(manifest)}: added ${add.length} Very Low water state(s) (xwaterlow, no shadow)`);
}
const CLOSURE_MAX = 3000;
/** the manifest's closure directives: { on, max } */
function closureOf(manifest) {
  const text = existsSync(manifest) ? readFileSync(manifest, 'utf8') : '';
  const max = /^#\s*closure-max:\s*(\d+)\s*$/m.exec(text);
  return { on: !/^#\s*closure:\s*off\s*$/m.test(text), max: max ? +max[1] : CLOSURE_MAX };
}
// The variant closure (tools/lib/program-closure.mjs): the logs' states merged into the manifest first (as
// gen-project-table would), then every state the closure adds, under one "# closure" line per regeneration that adds
// any. Over the bound it fails, naming the counts: an app with that many materials and environments sets its own.
function addClosureStates(manifest, logs) {
  const { on, max } = closureOf(manifest);
  if (!on) return false;
  const text = existsSync(manifest) ? readFileSync(manifest, 'utf8') : '';
  const have = statesOf(manifest);
  const fromLogs = [...new Set(logs.flatMap((l) => statesOf(l)))].filter((s) => !have.includes(s));
  const states = [...have, ...fromLogs];
  if (!states.length) return true;
  const order = featureOrder(readFileSync(join(THREE, 'src', 'gen', 'programs.h'), 'utf8'));
  const c = closure(states, order);
  if (c.add.length > max)
    throw new Error(`${manifest}: the variant closure adds ${c.add.length} states (${c.materials} materials, ${c.environments} environments), ` +
      `over its bound of ${max}: set "# closure-max: <n>" in the manifest to allow it, or "# closure: off"`);
  if (fromLogs.length || c.add.length) {
    let out = text.replace(/\n?$/, '\n');
    if (fromLogs.length) out += `# surveyed (tools/programs.mjs): ${fromLogs.length} state(s) the runs drew\n` + fromLogs.join('\n') + '\n';
    if (c.add.length) out += `# closure (tools/programs.mjs): ${c.add.length} state(s), the surveyed materials in the environments surveyed for their class\n` + c.add.join('\n') + '\n';
    writeFileSync(manifest, out);
  }
  console.log(`${basename(manifest)}: variant closure of ${c.materials} materials in ${c.environments} environments: +${c.add.length} state(s)`);
  return true;
}
// A closure block of a manifest: the states under a "# closure (tools/programs.mjs)" line, up to the next comment line
// (a regeneration writes the states it merges from runs under a "# surveyed" line).
const CLOSURE_HEAD = /^#\s*closure \(tools\/programs\.mjs\)/;
const SKIPPED_HEAD = /^#\s*closure skipped \(tools\/programs\.mjs\)/;
/** the manifest's closure states */
function closureStatesOf(manifest) {
  const out = new Set();
  let inBlock = false;
  for (const line of existsSync(manifest) ? readFileSync(manifest, 'utf8').split('\n') : []) {
    const t = line.trim();
    if (t.startsWith('#')) inBlock = CLOSURE_HEAD.test(t);
    else if (t && inBlock) out.add(t.replace(/#.*/, '').trim());
  }
  return out;
}
/** a closure state out of the manifest, its block's line gone with its last state, a "# closure skipped" line for it */
function skipClosureState(manifest, state, why) {
  const lines = readFileSync(manifest, 'utf8').replace(/\n$/, '').split('\n').filter((l) => l.replace(/#.*/, '').trim() !== state);
  const keep = lines.filter((l, i) => {
    if (!CLOSURE_HEAD.test(l.trim())) return true;
    for (let j = i + 1; j < lines.length; j++) { const t = lines[j].trim(); if (t.startsWith('#')) return false; if (t) return true; }
    return false;
  });
  keep.push(`# closure skipped (tools/programs.mjs): ${state}: ${why.replace(/\s+/g, ' ')}`);
  writeFileSync(manifest, keep.join('\n') + '\n');
}
/** the last regeneration's skipped lines out of the manifest: each regeneration tries its closure states again */
function clearSkipped(manifest) {
  if (!existsSync(manifest)) return;
  const text = readFileSync(manifest, 'utf8');
  const out = text.split('\n').filter((l) => !SKIPPED_HEAD.test(l.trim())).join('\n');
  if (out !== text) writeFileSync(manifest, out);
}
function reportSkipped(name, skipped) {
  console.error(`${name}: ${skipped.length} closure state(s) SKIPPED, their captures failed (the table lacks them; a draw in one is ` +
    `drawn with the program that covers it, or reported missing):\n` + skipped.map((s) => `  ${s.state}: ${s.why}`).join('\n'));
}
/**
 * The capture gen-project-table reports failed (three.c's tools/gen-programs.mjs: "capture <state> <backend> failed:
 * <its child's stderr>"), and why: a capture that finds its state's values missing says so on its stdout and exits 1, so
 * the child's stderr is empty; the capture runs again here, alone, and its own output is the reason.
 * @returns {{ state: string, backend: string, why: string } | null}
 */
function failedCapture(stderr, genArgs) {
  const m = /capture (\S+) (wgpu|gl) failed:([^\n]*)/.exec(stderr || '');
  if (!m) return null;
  const [, state, backend, said] = m;
  const extend = genArgs.flatMap((a, i) => (genArgs[i - 1] === '--extend' ? ['--extend', resolve(a)] : []));
  const out = join(BUILD, 'programs-cache', `.why-${process.pid}`);
  mkdirSync(out, { recursive: true });
  const r = spawnSync(process.execPath, [join(THREE, 'tools', 'gen-programs.mjs'), '--states', state, '--backend', backend, '--out', out,
    '--single', '--mock-gpu', join(THREE, 'tools', 'gpu-answers.json'), ...extend],
  { encoding: 'utf8', env: { ...process.env, GEN_ALLOW_UNCOVERED: '1' }, maxBuffer: 64 << 20 });
  rmSync(out, { recursive: true, force: true });
  const text = `${r.stdout || ''}\n${r.stderr || ''}`.split('\n').map((l) => l.trim()).filter(Boolean);
  const why = text.find((l) => /missing:/.test(l))?.replace(/^.*?\(missing:/, 'its values were not found in the program (missing:')
    || text.find((l) => /^(\w*Error|Error)\b|Error:/.test(l)) || said.trim() || text.at(-1)
    || `exit ${r.status}${r.signal ? ' ' + r.signal : ''}`;
  return { state, backend, why: why.replace(/\s+/g, ' ').slice(0, 400) };
}
/** regenerate one table from its manifest (plus missing logs) */
function regenerate(manifest, logs = []) {
  const name = basename(manifest, '.txt');
  addLightmapChannelStates(manifest);
  addVeryLowWaterStates(manifest, logs);
  clearSkipped(manifest);
  const closed = addClosureStates(manifest, logs);
  const own = manifestExtensions(manifest).filter((e) => !EXTENSIONS.includes(e));
  for (const e of own) {
    const previous = EXTENSIONS.find((x) => basename(x) === basename(e));
    if (previous) throw new Error(`${manifest}: duplicate extension ${basename(e)}: ${previous} and ${e}`);
  }
  const extensions = [...EXTENSIONS, ...own];
  const helpers = [...new Set(extensions.map((e) => dirname(e)))].flatMap((d) => readdirSync(d).filter((f) => f.endsWith('.mjs')).map((f) => join(d, f))).filter((f) => !extensions.includes(f));
  if (!existsSync(GEN)) throw new Error(`${GEN} not found: CW_THREE_DIR must be a three.c with tools/gen-project-table.mjs`);
  if (!statesOf(manifest).length && !logs.some((l) => statesOf(l).length)) {
    console.log(`${name}: no states, nothing to generate`);
    return;
  }
  // the cache is keyed by state name only: captures made with other extension sources go to another one
  const extHash = createHash('sha256');
  for (const e of [...extensions, ...helpers].sort()) extHash.update(readFileSync(e));
  if (extensions.some(isSurface)) extHash.update(readFileSync(SURFACE_PART));
  const parts = extensions.map(asPart); // what the generator loads: a surface module through its wrapper
  const cache = join(BUILD, 'programs-cache', extensions.length ? `${name}-${extHash.digest('hex').slice(0, 12)}` : name);
  const args = [GEN, '--name', name, '--out', dirname(manifest), '--states', manifest, '--cache', cache, ...(closed ? [] : ['--minimize']),
    ...logs.flatMap((l) => ['--missing', l]), ...parts.flatMap((e) => ['--extend', e]),
    ...composedModules([manifest, ...logs], parts).flatMap((e) => ['--extend', e]),
    ...(argv.includes('--fast') ? [] : ['--check'])];
  console.log(`${name}: regenerating (${dirname(manifest)})`);
  // the capture runs on three.c's mock GPU: no device. A closure state whose capture fails is left out of the table and
  // reported (the closure only guesses at a state no run drew); a surveyed state's failure fails the regeneration
  const skipped = [];
  for (;;) {
    const r = spawnSync(process.execPath, args, { stdio: ['inherit', 'inherit', 'pipe'], encoding: 'utf8', maxBuffer: 256 << 20 });
    if (r.stderr) process.stderr.write(r.stderr);
    if (r.status === 0) break;
    const f = failedCapture(r.stderr, args);
    const why = f ? `capture ${f.state} ${f.backend} failed: ${f.why}` : `gen-project-table failed (${r.signal || 'exit ' + r.status})`;
    if (!f || !closureStatesOf(manifest).has(f.state)) {
      if (skipped.length) reportSkipped(name, skipped);
      throw new Error(`${name}: ${why}`);
    }
    console.error(`${name}: closure state ${f.state} SKIPPED: ${why}`);
    skipped.push({ state: f.state, why });
    skipClosureState(manifest, f.state, why);
  }
  if (skipped.length) reportSkipped(name, skipped);
  // the captures ran on a mock GPU, which takes any WGSL: compile each WebGPU program with Dawn's own compiler
  // (Tint) as a host will, and fail here, not on the first WebGPU draw (tools/lib/wgsl-check.mjs)
  if (!argv.includes('--no-wgsl-check')) {
    const c = checkWgsl(cache, undefined, { three: THREE });
    if (c.skipped) console.warn(`${name}: WebGPU programs NOT checked with Tint: ${c.skipped}`);
    else console.log(`${name}: Tint accepts all ${c.checked + c.cached} WebGPU programs (${c.checked} compiled)`);
  }
  // the capture cache sits in the build tree; the generated C beside the manifest, checked in
  rmSync(join(dirname(manifest), '.t3-captures'), { recursive: true, force: true });
  stamp(manifest, extensions);
}

/** the generated C stamped with the extension sources its states were made from (tools/lib/program-sources.mjs; the
 * build compares it, cmake/programs.cmake) */
function stamp(manifest, extensions) {
  const name = basename(manifest, '.txt'), dir = dirname(manifest);
  const parts = new Set();
  for (const st of statesOf(manifest)) for (const t of st.split('+')) if (/^x[a-z0-9_]+$/.test(t)) t.slice(1).split('_').forEach((p) => parts.add(p));
  const modules = [...parts].map((p) => {
    const m = extensions.find((e) => basename(e) === p + '.mjs');
    if (!m) throw new Error(`${manifest}: a state uses the extension part ${p}, and no module ${p}.mjs was given`);
    return m;
  });
  const st = tableStamp(modules, ROOT, dir);
  for (const be of ['gl', 'wgpu']) {
    const c = join(dir, `${name}_${be}.c`);
    if (!existsSync(c)) continue;
    const text = readFileSync(c, 'utf8').replace(/^\/\* cw-source[^\n]*\n/gm, '');
    writeFileSync(c, st + text);
  }
}

const apps = () => {
  const dir = join(BUILD, 'apps');
  if (!existsSync(dir)) throw new Error(`${dir}: configure and build ${BUILD} first`);
  return readdirSync(dir).filter((f) => f.endsWith('.json')).map((f) => JSON.parse(readFileSync(join(dir, f), 'utf8')));
};

if (all('--table').length > 1) {
  // several tables (the list the configure prints for out-of-date tables): each in its own process, a few at once
  const { spawn } = await import('node:child_process');
  const { availableParallelism } = await import('node:os');
  const pass = argv.filter((a, i) => a !== '--table' && argv[i - 1] !== '--table');
  const queue = all('--table').map((m) => resolve(m));
  const jobs = Math.max(1, Math.min(queue.length, Math.floor(availableParallelism() / 4) || 1));
  const failed = [];
  console.log(`programs: regenerating ${queue.length} tables, ${jobs} at a time`);
  await Promise.all(Array.from({ length: jobs }, async () => {
    for (let m; (m = queue.shift()); ) {
      const code = await new Promise((res) => spawn(process.execPath, [fileURLToPath(import.meta.url), ...pass, '--table', m], { stdio: 'inherit' }).on('close', res));
      if (code !== 0) failed.push(m);
    }
  }));
  if (failed.length) { console.error(`programs: ${failed.length} table(s) failed:\n  ${failed.join('\n  ')}`); process.exit(1); }
  process.exit(0);
}
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
    if (!existsSync(manifest)) writeFileSync(manifest, `# r187 program states ${app} draws (cmake/programs.cmake, tools/programs.mjs)\n`);
    console.log(`${app}: ${new Set(statesOf(log)).size} missing state(s) -> ${manifest.replace(ROOT + '/', '')}`);
    if (!argv.includes('--dry')) regenerate(manifest, [log]);
  }
  process.exit(0);
}

// ── run the apps ──────────────────────────────────────────────────────
if (!argv.includes('--no-run')) await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
const frames = one('--frames') || '120';
// the preset switch half way through a run (see --presets): [from, to] pairs, High to each other preset and back
const switchAt = Math.max(3, Math.floor(+frames / 2));
const switches = (presets) => argv.includes('--no-switch') ? [] :
  presets.filter((p) => p !== 'high').flatMap((p) => [['high', p], [p, 'high']]);
const runDir = join(BUILD, 'programs-logs');
const droppedSeen = new Set(); // "<app>: <state> uniform|texture <name>"
const CARTS = argv.includes('--carts');
const logDirs = all('--logs').map((d) => resolve(d));
// a tree says in its cache; a game built on an SDK has the engine's choice in the SDK's manifest
const wgpu = /^CW_WGPU:BOOL=ON$/m.test(existsSync(join(BUILD, 'CMakeCache.txt')) ? readFileSync(join(BUILD, 'CMakeCache.txt'), 'utf8') : '')
  || (existsSync(join(ROOT, 'manifest.json')) && JSON.parse(readFileSync(join(ROOT, 'manifest.json'), 'utf8')).wgpu === true);
// unit_programs_missing leaves a state no table has on purpose (tests/programs-carts.mjs must find it): only by name;
// unit_pipeline reads GL framebuffers itself, so a WebGPU run of it aborts: its runs are GL only (below)
const list = apps().filter((a) => !names.length || names.includes(a.name)).filter((a) => existsSync(a.binary))
  .filter((a) => names.includes(a.name) || a.name !== 'unit_programs_missing');
if (names.length && list.length !== names.length)
  throw new Error(`not built: ${names.filter((n) => !list.some((a) => a.name === n)).join(' ')}`);

/**
 * What a cart run surveyed, from its "pipeline" reply (run-cart --cmd-end pipeline --cmd-out): the states the cart
 * reported, one per line, or why the run proves nothing. A run with no reply (it failed before its last frame), an
 * error reply (no pipeline: no 3D frame was ever made) or no render pass that ran (no frame was rendered) surveyed
 * nothing, and "no missing states" from it would be a false all-clear.
 * @param {string} replyFile
 * @returns {{ states: string[], why: string | null }}
 */
const cartSurvey = (replyFile) => {
  if (!existsSync(replyFile)) return { states: [], why: 'no pipeline reply (the run ended before its last frame)' };
  let rep;
  try {
    const j = JSON.parse(readFileSync(replyFile, 'utf8'));
    rep = j.at(-1)?.reply?.reply ?? j.at(-1)?.reply;
  } catch (e) { return { states: [], why: `unreadable pipeline reply (${e.message})` }; }
  if (!rep || typeof rep !== 'object') return { states: [], why: `no pipeline reply object (${JSON.stringify(rep)?.slice(0, 200)})` };
  if (rep.error) return { states: [], why: `the cart never reached a rendered 3D frame (${rep.error})` };
  if (!Array.isArray(rep.passes) || !rep.passes.some((p) => p.ran))
    return { states: [], why: 'no render pass ran: the cart never reached a rendered frame' };
  if (typeof rep.missing_states !== 'string') return { states: [], why: 'the pipeline reply has no missing_states field' };
  const dropped = typeof rep.dropped_values === 'string' ? rep.dropped_values.split('\n').map((l) => l.trim()).filter(Boolean) : [];
  return { states: rep.missing_states.split('\n').map((l) => l.trim()).filter(Boolean), dropped, why: null };
};
const cartScenes = sceneEnvs(argv);
if (CARTS && !argv.includes('--no-run')) {
  rmSync(runDir, { recursive: true, force: true });
  mkdirSync(runDir, { recursive: true });
  const cartDir = join(PROJ, 'build', 'cart-wgpu', 'carts');
  const have = existsSync(cartDir) ? readdirSync(cartDir).filter((f) => f.endsWith('.wasc')).map((f) => f.slice(0, -5)) : [];
  const todo = names.length ? names : have;
  // a survey of nothing is a failure, not "0 states": name the directory it looked in (run from inside the game's
  // checkout, or name it with CW_PROJECT_ROOT or --build <project>/build/native)
  const where = '(build them: scripts/build.sh; run it inside the game\'s checkout, or name the project with CW_PROJECT_ROOT)';
  if (!todo.length) throw new Error(`no dual carts to survey in ${cartDir} ${where}`);
  for (const n of todo) if (!have.includes(n)) throw new Error(`no dual cart ${n} in ${cartDir} ${where}`);
  console.log(`surveying ${todo.length} dual cart(s) from ${cartDir}`);
  const failed = []; // every run that surveyed nothing: "<run> on the <leg> host: why"
  if (argv.includes('--gl-only') && argv.includes('--webgpu-only')) throw new Error('--gl-only and --webgpu-only leave no host');
  const legs = argv.includes('--webgpu-only') ? [] : [['gl', ['--gpu', 'gl', '--dual-gl']]];
  if (!argv.includes('--gl-only')) legs.push(['webgpu', ['--gpu', 'webgpu']]);
  const gates = argv.includes('--gates') ? gateFile().filter((g) => g.app && !g.script) : [];
  for (const app of todo) {
    // every preset but the default High, switched to at frame 2 (cartwheel/quality.h: each asks for states of its own)
    const runs0 = [{ tag: app, args: ['--frames', frames] }, ...PRESETS.filter((p) => p !== 'high').map((p) => ({ tag: `${app}.${p}`, args: ['--frames', frames, '--start-cmds', `quality ${p}`] }))];
    // the preset switched in play, both ways (the cart starts on High)
    for (const [from, to] of switches(PRESETS))
      runs0.push({ tag: `${app}.${from}-to-${to}`, args: ['--frames', frames, '--start-cmds', [...(from !== 'high' ? [`quality ${from}`] : []), `@${switchAt} quality ${to}`].join(';')] });
    for (const g of gates.filter((x) => x.app === app))
      runs0.push({ tag: g.name, args: ['--frames', String(g.frames), ...(g.input ? ['--input', join(PROJ, 'tests', g.input)] : [])] });
    // as a player's host runs it: no seed, so the warm-up spreads over frames under the loading screen (its slices draw
    // the scene in parts) and the clock is real; a deterministic run does the warm-up in one frame and never shows them
    runs0.push({ tag: `${app}.player`, args: ['--frames', frames, '--warmup-spread'] });
    for (const g of gates.filter((x) => x.app === app))
      runs0.push({ tag: `${g.name}.player`, args: ['--frames', String(g.frames), '--warmup-spread', ...(g.input ? ['--input', join(PROJ, 'tests', g.input)] : [])] });
    // every run once per scene (--scenes / --scene): the cart reads the variable through its env request (run-cart --env)
    const runs = cartScenes.flatMap((sc, k) => runs0.map((r) => (Object.keys(sc).length
      ? { tag: `${r.tag}.scene${k}`, args: [...r.args, '--env', JSON.stringify(sc)], label: sceneLabel(sc) } : r)));
    const seen = new Set();
    let surveyed = 0;
    for (const run of runs) for (const [leg, legArgs] of legs) {
      const stateFile = join(runDir, `${run.tag}.${leg}.cmd.json`);
      const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'romdev', '--app', app, ...run.args, ...legArgs, '--cmd-end', 'pipeline', '--cmd-out', stateFile],
        // the project found here, not run-cart's own default (the tree it sits in: an SDK's has no carts of the game)
        { encoding: 'utf8', timeout: 600000, env: testEnv({ ...process.env, CW_PROJECT_ROOT: PROJ }) });
      const got = cartSurvey(stateFile);
      const why = r.status !== 0 ? `run failed (${r.signal || 'exit ' + r.status}): ${(r.stderr || r.stdout || '').trim().split('\n').slice(-1)[0]}` : got.why;
      if (why) { failed.push(`${run.tag}${run.label ? ' (' + run.label + ')' : ''} on the ${leg} host: ${why}`); console.log(`cart ${run.tag} (${leg}): FAILED: ${why}`); continue; }
      surveyed++;
      for (const st of got.states) seen.add(st);
      for (const d of got.dropped) droppedSeen.add(`${app}: ${d}`);
    }
    if (seen.size) writeFileSync(join(runDir, `${app}.log`), [...seen].join('\n') + '\n');
    console.log(`cart ${app}: ${seen.size} state(s) no table has (${surveyed} of ${runs.length * legs.length} run(s) surveyed)`);
  }
  // fail loudly: a run that surveyed nothing would otherwise read as "no missing states" (the states it would have
  // reported then show up later as skipped draws on that host); the states the other runs found are not merged
  if (failed.length) {
    console.error(`programs --carts: ${failed.length} run(s) surveyed nothing, no table changed:\n  ${failed.join('\n  ')}`);
    process.exit(1);
  }
  logDirs.push(runDir);
}
if (!CARTS && !argv.includes('--no-run')) {
  rmSync(runDir, { recursive: true, force: true });
  mkdirSync(runDir, { recursive: true });
  const failed = []; // every run that surveyed nothing: as --carts, a false all-clear otherwise
  const legs = [['gl', {}]];
  if (wgpu && !argv.includes('--gl-only')) legs.push(['webgpu', { CW_GPU: 'webgpu', ...wgpuPin().env }]);
  const scenes = sceneEnvs(argv); // --scenes / --scene (tools/lib/programs-args.mjs): one run per value, commas kept with ';' or --scene
  // every graphics preset (cartwheel/quality.h): each asks for its own states (hard filtering, one cascade, no bloom)
  const presets = (one('--presets') || PRESETS.join(',')).split(',');
  for (const a of list) {
    for (const [leg, extra] of legs.filter(([l]) => l === 'gl' || a.name !== 'unit_pipeline')) {
      // each preset from the start, then each switch in play (CW_START_CMDS "@N quality <to>", src/platform/native.c)
      const runs = [...presets.map((p) => [p, null]), ...switches(presets)];
      for (const scene of scenes) for (const [preset, to] of runs) {
        const r = spawnSync(a.binary, [], {
          cwd: dirname(a.binary), encoding: 'utf8', timeout: 180000,
          env: testEnv({ ...process.env, ...extra, ...scene, CW_FRAMES: frames, SDL_AUDIODRIVER: 'dummy', CW_PROGRAMS_LOG_DIR: runDir, CW_QUALITY: preset,
            ...(to ? { CW_START_CMDS: `@${switchAt} quality ${to}` } : {}) }),
        });
        if (r.status !== 0) {
          const run = `${a.name} (${leg} ${preset}${to ? ' to ' + to : ''}${Object.keys(scene).length ? ' ' + sceneLabel(scene) : ''}): ${r.signal || 'exit ' + r.status}`;
          failed.push(run);
          console.log(run);
        }
      }
    }
  }
  // --gates: also every gate's native runs (its input, assets and control variant), where most states show up
  if (argv.includes('--gates')) {
    const gates = gateFile().filter((g) => g.app && !g.script && (!names.length || names.includes(g.app)));
    for (const g of gates) {
      const base = ['--host', 'native', '--app', g.app, '--frames', String(g.frames), ...(g.frameHz ? ['--frame-hz', String(g.frameHz)] : []),
        ...(g.assets ? ['--assets', join(PROJ, g.assets)] : [])];
      const inp = (f) => (f ? ['--input', join(PROJ, 'tests', f)] : []);
      // "repo:<path>" names a file the run may write back (the editor saves its settings): a copy, as gate.mjs does
      const repo = (e) => Object.fromEntries(Object.entries(e || {}).map(([k, val]) => {
        if (typeof val !== 'string' || !val.startsWith('repo:')) return [k, val];
        const copy = join(runDir, g.name + '.' + basename(val.slice(5)));
        copyFileSync(join(PROJ, val.slice(5)), copy);
        return [k, copy];
      }));
      const variants = [{ args: inp(g.input), env: {} }];
      if (g.controlInput || g.controlEnv) variants.push({ args: inp(g.controlInput || g.input), env: repo(g.controlEnv), control: true });
      // the gate's run on the cheapest preset too: the states Very Low asks for where the gate's scene goes
      variants.push({ args: inp(g.input), env: { ...repo(g.env), CW_QUALITY: 'verylow' }, control: true });
      const gpus = [...(g.gl === false ? [] : ['gl']), ...(g.webgpu && wgpu && !argv.includes('--gl-only') ? ['webgpu'] : [])];
      for (const v of variants)
        for (const gpu of gpus) {
          const env = { ...process.env, ...v.env, CW_PROJECT_ROOT: PROJ, CW_PROGRAMS_LOG_DIR: runDir };
          const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'run-cart.mjs'), ...base, ...v.args, '--gpu', gpu, '--allow-skipped'], { env, encoding: 'utf8', timeout: 300000 });
          if (r.status === 0) continue;
          const run = `gate ${g.name}${v.control ? ' control' : ''} (${gpu}): run failed: ${whyFailed(r)}`;
          if (!v.control) failed.push(run); // a control may be meant to break its run
          console.log(run);
        }
    }
  }
  if (failed.length) {
    console.error(`programs: ${failed.length} run(s) surveyed nothing, no table changed:\n  ${failed.join('\n  ')}`);
    process.exit(1);
  }
  logDirs.push(runDir);
}

// values extension hooks set that their programs lacked (src/render/pipeline.c ext_miss_hook: <app>.dropped natively, the
// pipeline reply's dropped_values on carts): a table generated before an extension changed. The survey fails on any.
for (const d of logDirs) {
  if (!existsSync(d)) continue;
  for (const f of readdirSync(d).filter((x) => x.endsWith('.dropped')))
    for (const l of statesOf(join(d, f))) droppedSeen.add(`${basename(f, '.dropped')}: ${l}`);
}
if (droppedSeen.size) {
  console.error(`programs: ${droppedSeen.size} value(s) set for programs that lack them (the tables predate an extension change; ` +
    `regenerate each with node tools/programs.mjs --table <its manifest>):\n  ${[...droppedSeen].sort().join('\n  ')}`);
  process.exit(1);
}

// --dry: report what each app misses (out/programs-survey.json), change nothing
if (argv.includes('--dry')) {
  const out = {};
  for (const a of list.length ? list : apps()) {
    const st = [...new Set(logDirs.map((d) => join(d, `${a.name}.log`)).flatMap(statesOf))].sort();
    if (st.length) out[a.name] = st;
  }
  mkdirSync(join(PROJ, 'out'), { recursive: true });
  writeFileSync(join(PROJ, 'out', 'programs-survey.json'), JSON.stringify(out, null, 1) + '\n');
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
      writeFileSync(manifest, `# r187 program states ${a.name} draws (cmake/programs.cmake, tools/programs.mjs)\n`);
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
