// The cross-host gate: every app in tests/gates.json runs on every host with
// the same frame count and input script, and must agree with the native run:
//   - frames: share of pixels differing by > 16 levels under the gate's bound
//   - named debug state: equal (exactly, or within `tol` for floats)
// Before trusting a pass, each gate also runs a CONTROL that must fail: the
// native run one frame early (or with the control input) compared against
// the reference must differ, or the gate cannot detect anything.
//
//   node tests/gate.mjs [gate-name ...] [--hosts native,romdev,chromium] [--gpu gl|webgpu|both] [--cpu]
//
// --hosts romdev: the romdev mode, every leg through romdev and no native run (romdevGates below): the dual cart
// on GL is the reference, --gpu webgpu (or both) adds its WebGPU leg against it.
//
// --cpu: the CPU-only mode for state checks. No GPU is used and no GPU suite lock is taken (so it never queues
// behind other suites): the native host runs on Mesa's software renderer (CW_TEST_GPU=software), only gates with
// an `expect` block run (their named state is the check; frames are not compared across hosts), and each still
// runs its control, which must fail. Gates without `expect` are listed as skipped, never counted as passing.
//
// --gpu webgpu runs the gates that have a "webgpu" entry on the WebGPU backend instead (see wgpuGates).
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, existsSync, copyFileSync } from 'node:fs';
import { join, resolve, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages, diffAreas, uniqueColors } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { armGuard } from '../tools/lib/gpu-guard.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { textFloorProblem, FLOOR_MIN } from '../tools/lib/text-floor.mjs';
if (process.argv.includes('--cpu')) process.env.CW_TEST_GPU = 'software'; // see the header
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

// the romdev mode runs nothing on this machine's GPU itself (romdev's server does, as for every other client of it):
// it does not queue behind the native suites
const romdevMode = process.argv.includes('--hosts') && process.argv[process.argv.indexOf('--hosts') + 1] === 'romdev';
if (!romdevMode) await takeSuiteLock('gate.mjs');
// nothing a gate starts can reach the desktop (SDL offscreen, no DISPLAY / WAYLAND_DISPLAY / XAUTHORITY);
// the guards watch the kernel log, the card's memory and the budgets (tools/lib/gpu-guard.mjs)
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
// CW_PROJECT_ROOT: the gates of another tree built with this engine (a games
// repository with its own tests/gates.json, input scripts, build/ and out/);
// default this checkout. run-cart.mjs reads the same variable.
const PROJ = process.env.CW_PROJECT_ROOT ? resolve(process.env.CW_PROJECT_ROOT) : ROOT;
const OUT = join(PROJ, 'out', 'gate');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const cpuMode = argv.includes('--cpu');
const hostsArg = cpuMode ? ['native'] : argv.includes('--hosts') ? argv[argv.indexOf('--hosts') + 1].split(',') : null;
const gpuArg = argv.includes('--gpu') ? argv[argv.indexOf('--gpu') + 1] : 'gl';
// --no-chromium drops the Chromium leg from every gate; --chromium-only runs only the gates' Chromium leg (with the native reference)
const noChromium = argv.includes('--no-chromium'), chromiumOnly = argv.includes('--chromium-only');
const only = argv.filter((a, i) => !a.startsWith('--') && argv[i - 1] !== '--hosts' && argv[i - 1] !== '--gpu' && argv[i - 1] !== '--gates-file');
const guard = armGuard('gate.mjs', { outFile: join(OUT, 'gpu-mem.json') });
// a host --hosts asked for that a gate's hosts / skip list leaves out: said loudly, never
// dropped in silence (a gate pinned to native looks like a passing romdev leg otherwise)
const warnDropped = (name, hosts) => {
  const miss = hostsArg ? hostsArg.filter((h) => !hosts.includes(h)) : [];
  if (miss.length) console.log(`WARN ${name}: --hosts asked for ${miss.join(', ')}, but this gate's hosts / skip list leaves them out: those legs did NOT run`);
};
const usesHosts = (hosts) => hosts.filter((h) => !(noChromium && h === 'chromium') && !(chromiumOnly && !['native', 'chromium'].includes(h)));
// There is one cart per game: the dual GL + WebGPU one (build/cart-wgpu). The GL leg of a
// cart is the dual cart told to render with GL on romdev (run-cart --dual-gl, implied there); Chromium and wasmcart-native
// run the WebGPU axis (--gpu webgpu).
// A native-only test binary (unit_pipeline: it reads GL framebuffers itself) has no cart target at all: its romdev leg
// is dropped, said in the output.
const dualCart = (app) => existsSync(join(PROJ, 'build', 'cart-wgpu', 'carts', `${app}.wasc`));
const glHosts = (hosts, name, app) => usesHosts(hosts).filter((h) => !['chromium', 'wcnative'].includes(h)).filter((h) => {
  if (h !== 'romdev' || dualCart(app)) return true;
  console.log(`note ${name}: no romdev leg (${app} is a native-only test binary: it has no cart)`);
  return false;
});
// --gates-file a.json,b.json (or CW_GATES_FILE): gate files in the project's tests/ in place of gates.json, so a
// repository of several games keeps one file per game and they never collide
const gatesArg = argv.includes('--gates-file') ? argv[argv.indexOf('--gates-file') + 1] : process.env.CW_GATES_FILE || 'gates.json';
const gates = gatesArg.split(',').flatMap((f) => JSON.parse(readFileSync(resolve(PROJ, 'tests', f.trim()), 'utf8'))).filter((g) => !only.length || only.includes(g.name));

// the current gate's "env" (every native run of it, controls too; a control's controlEnv wins): how a game picks one
// mode for a gate (a game, a skill level) without a build of its own. A cart has no environment: the gate's "env"
// reaches it as the debug channel's env request (run-cart --env), which fails a run whose game read a variable
// before the request arrived (romdev: frame 2).
let gateEnv = {};
// the same environment as run-cart --env: a cart host gets it as the debug channel's env request (src/core/env.c)
const envArg = (e) => (e && Object.keys(e).length ? ['--env', JSON.stringify(e)] : []);
function run(args, env = {}) {
  return new Promise((res) => {
    const p = spawn('node', [join(ROOT, 'tools', 'run-cart.mjs'), ...args], { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...gateEnv, ...env } });
    let out = '', err = '';
    p.stdout.on('data', (d) => (out += d));
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => res({ code, out, err }));
  });
}

// The program states a run drew without a program (cw.three.missing_states: three.c skipped those draws).
// A gate fails on any, with the lines to add to the app's program table (cmake/programs.cmake).
function missingStates(st) {
  const v = st['cw.three.missing_states'];
  if (!v) return [];
  const bytes = Array.isArray(v) ? v : typeof v === 'string' && /^[0-9a-f]*$/i.test(v) ? v.match(/../g) || [] : null;
  const text = bytes ? String.fromCharCode(...bytes.map((b) => (typeof b === 'number' ? b : parseInt(b, 16))).filter((b) => b)) : String(v);
  return text.split('\n').map((l) => l.trim()).filter(Boolean);
}
function checkMissing(problems, app, runs) {
  const seen = new Set();
  for (const [h, file] of runs) {
    let st;
    try { st = JSON.parse(readFileSync(file, 'utf8')); } catch { continue; }
    const m = missingStates(st).filter((x) => !seen.has(x));
    if (!m.length) continue;
    m.forEach((x) => seen.add(x));
    problems.push(`${h}: ${m.length} program state(s) in none of ${app}'s program tables (draws skipped); add to its manifest ` +
      `(node tools/programs.mjs ${app}, or by hand):\n    ${m.join('\n    ')}`);
  }
}

// A run whose prompts named an action that does not exist ({jmup}), or were drawn from fixed_update, counted it in
// prompt.errors (cartwheel/prompt.h): every gate fails on it, unless the gate's expect block names the count.
function checkPromptErrors(problems, g, runs) {
  if (g.expect && 'prompt.errors' in g.expect) return;
  for (const [h, file] of runs) {
    let st;
    try { st = JSON.parse(readFileSync(file, 'utf8')); } catch { continue; }
    if (st['prompt.errors'] > 0) problems.push(`${h}: prompt.errors = ${st['prompt.errors']}: a prompt names an action that does not exist or ran in fixed_update (the run's log names it)`);
  }
}

// The text size floor (cartwheel/render2d.h "text size"): a game's run fails when any frame drew text under
// CW_TEXT_FLOOR_MIN of the canvas height (text.run_min_share), naming the string. On by default for games (a games
// repository's apps, CW_PROJECT_ROOT, and this repository's games/); "textFloor": false turns it off for one gate,
// true turns it on for a sample, a number sets another floor (a share of the canvas height).
// (a game's editor build, <game>_editor_*, is held to the floor too: the editor's text is at least 13 px on its 720-line window)
const isGame = (app) => PROJ !== ROOT || existsSync(join(ROOT, 'build', 'native', 'games', app));
function checkTextFloor(problems, g, runs) {
  const on = g.textFloor ?? isGame(g.app);
  if (!on) return;
  const floor = typeof on === 'number' ? on : FLOOR_MIN;
  for (const [h, file] of runs) {
    let st;
    try { st = JSON.parse(readFileSync(file, 'utf8')); } catch { continue; }
    const p = textFloorProblem(st, floor);
    if (p) { problems.push(`${h}: ${p}`); return; } // one host says it: the runs agree on text
  }
}

function stateDiff(a, b, tol, fields) {
  const diffs = [];
  // the debug channel's mailboxes are transport, not state
  // host.* fields describe the machine (adapter, job threads), never the run
  // text.*_where name the smallest string for a person (a byte field romdev's state does not carry); the shares are compared
  const keys = fields || Object.keys(a).filter((k) => !k.startsWith('dbg.') && !k.startsWith('host.') && !/^text\..*_where$/.test(k));
  for (const k of keys) {
    const va = a[k], vb = b[k];
    if (vb === undefined) { diffs.push(`${k}: missing`); continue; }
    const xa = [].concat(va), xb = [].concat(vb);
    if (xa.length !== xb.length) { diffs.push(`${k}: length ${xa.length} vs ${xb.length}`); continue; }
    for (let i = 0; i < xa.length; i++) {
      const d = typeof xa[i] === 'number' ? Math.abs(xa[i] - xb[i]) : xa[i] === xb[i] ? 0 : Infinity;
      const scale = Math.max(1, Math.abs(xa[i]));
      if (d > tol * scale) { diffs.push(`${k}[${i}]: ${xa[i]} vs ${xb[i]}`); break; }
    }
  }
  return diffs;
}


// ── the WebGPU axis (--gpu webgpu) ───────────────────────────────────────
// Gates with a "webgpu" entry in gates.json run again on the WebGPU backend
// (cartwheel/gpu.h): the native build with CW_GPU=webgpu and the dual carts of
// build/cart-wgpu, every host held to the test GPU (tools/lib/wgpu-pin.mjs).
// WebGPU output is not bit-identical across GPUs and backends (wasmcart SPEC
// "WebGPU", Determinism), so:
//   - across hosts: named debug state EXACT (the gate's fields), frames under
//     webgpu.maxGt16 (default: the gate's own bound);
//   - GL vs WebGPU: the native GL run against the native WebGPU run, state
//     exact, frames under webgpu.vsGl (default 0.01);
//   - webgpu.maxArea (when set): on every WebGPU host, the share of the
//     frame in solid blocks (7 x 7 px, > 35 levels apart) that differs from
//     native GL stays under it (tools/lib/png.mjs diffAreas): a shadow or a
//     surface one backend draws and the other does not, which the per-pixel
//     share hides among the thin edges and grass blades that always differ;
//   - every run reports zero WebGPU errors (run-cart fails it otherwise);
//   - the dual cart on romdev told to render with GL (loadMedia wgpu:false)
//     against native GL: frames under the gate's bound, state exact;
//   - CONTROLS that must fail: the WebGPU run one frame short (or with the
//     control input) against the WebGPU reference, and a GL-vs-WebGPU
//     comparison against that short run.
//   - webgpu.controls: more native WebGPU controls, each with its own env
//     (CW_TEST_BREAK_PROGRAM=<program> compiles that pass program with a
//     magenta fragment, CW_TEST_SKIP_PASS=<pass> never runs that pass, or an
//     app's own switch). Each must fail on the PICTURE alone (named state is
//     not consulted): over the cross-host bound against the WebGPU
//     reference and over vsGl against GL, so the frame tolerances are proven
//     to see a broken or missing pass. A control with "fields" may instead be
//     told apart by those named state fields, each differing from both the
//     WebGPU reference and GL (a feature whose effect leaves the last frame,
//     e.g. impostors at the end of a sweep that ends with every mesh near).
// "webgpu": true | { "hosts": [...], "skip": [...], "maxGt16": n, "vsGl": n, "maxArea": n, "controls": [{ "env": {...}, "fields": [...] }] }
// "gl": false keeps a gate off the GL axis (controls that inject WebGPU faults)
async function wgpuGates(list) {
  let failedW = 0;
  for (const g of list) {
    if (!g.webgpu || g.script) continue;
    const w = g.webgpu === true ? {} : g.webgpu;
    const defaults = g.input ? ['native', 'romdev', 'chromium'] : ['native', 'romdev', 'chromium', 'wcnative'];
    const eligible = w.hosts || g.hosts || defaults;
    const hosts = usesHosts((hostsArg ? hostsArg.filter((h) => eligible.includes(h)) : eligible).filter((h) => !(w.skip || g.skip || []).includes(h)));
    if (!hosts.includes('native')) hosts.unshift('native');
    if (chromiumOnly && !hosts.includes('chromium')) continue;
    warnDropped(g.name + ' [webgpu]', hosts);
    await guard.gate(g.name + ' [webgpu]');
    gateEnv = g.env || {};
    const common = ['--app', g.app, '--frames', String(g.frames), ...(g.frameHz ? ['--frame-hz', String(g.frameHz)] : []), ...envArg(g.env)];
    if (g.input) common.push('--input', join(PROJ, 'tests', g.input));
    if (g.startCmds) common.push('--start-cmds', g.startCmds.join(';'));
    if (g.assets) common.push('--assets', join(PROJ, g.assets));
    const p = (tag) => ({ shot: join(OUT, `${g.name}.wgpu.${tag}.png`), state: join(OUT, `${g.name}.wgpu.${tag}.json`) });
    const results = await Promise.all(hosts.map(async (h) => {
      const q = p(h);
      const extra = h === 'wcnative' ? ['--shot', q.shot] : ['--shot', q.shot, '--state', q.state];
      return { h, ...q, ...(await run(['--host', h, '--gpu', 'webgpu', ...common, ...extra])) };
    }));
    const glq = p('native-gl');
    const gl = await run(['--host', 'native', '--gpu', 'gl', ...common, '--shot', glq.shot, '--state', glq.state]);
    // the same dual cart on romdev told to use GL (loadMedia wgpu:false) against native GL
    const dq = p('romdev-dual-gl');
    const dual = hosts.includes('romdev') ? await run(['--host', 'romdev', '--gpu', 'gl', '--dual-gl', ...common, '--shot', dq.shot, '--state', dq.state]) : null;
    const problems = [], ctlNotes = [];
    for (const r of results) if (r.code !== 0) problems.push(`${r.h}: run failed: ${r.err.trim().split('\n').slice(-3).join(' | ')}`);
    if (gl.code !== 0) problems.push(`native GL run failed: ${gl.err.trim().split('\n').slice(-2).join(' | ')}`);
    if (dual && dual.code !== 0) problems.push(`romdev dual cart on GL failed: ${dual.err.trim().split('\n').slice(-2).join(' | ')}`);
    const adapters = results.filter((r) => r.code === 0).map((r) => `${r.h}: ${(() => { try { return JSON.parse(r.out.trim().split('\n').pop()).adapter; } catch { return '?'; } })()}`);
    if (!problems.length) {
      const ref = results.find((r) => r.h === 'native');
      const refImg = readImage(ref.shot);
      const refState = JSON.parse(readFileSync(ref.state, 'utf8'));
      if (uniqueColors(refImg) < (g.minColors || 2)) problems.push(`native WebGPU frame has ${uniqueColors(refImg)} colours: not a real picture`);
      if (refState['cw.gpu.api'] !== 2) problems.push(`native run used gpu api ${refState['cw.gpu.api']}, not WebGPU`);
      checkMissing(problems, g.app, [...results.filter((r) => r.h !== 'wcnative').map((r) => [r.h + ' webgpu', r.state]), ['native gl', glq.state], ...(dual ? [['romdev dual-gl', dq.state]] : [])]);
      checkPromptErrors(problems, g, [...results.filter((r) => r.h !== 'wcnative').map((r) => [r.h + ' webgpu', r.state]), ['native gl', glq.state]]);
      checkTextFloor(problems, g, [...results.filter((r) => r.h !== 'wcnative').map((r) => [r.h + ' webgpu', r.state]), ['native gl', glq.state]]);
      for (const [k, v] of Object.entries(g.expect || {}))
        if (JSON.stringify(refState[k]) !== JSON.stringify(v)) problems.push(`native WebGPU ${k} = ${JSON.stringify(refState[k])}, expected ${JSON.stringify(v)}`);
      const bound = w.maxGt16 ?? g.maxGt16 ?? 0.001;
      for (const r of results) {
        if (r === ref) continue;
        const d = diffImages(refImg, readImage(r.shot));
        if (!d.comparable) problems.push(`${r.h}: ${d.reason}`);
        else if (d.gt16 > bound) problems.push(`${r.h}: ${(d.gt16 * 100).toFixed(3)}% pixels differ by >16 (max ${d.max})`);
        if (r.h !== 'wcnative') {
          const sd = stateDiff(refState, JSON.parse(readFileSync(r.state, 'utf8')), g.tol ?? 0, g.fields);
          if (sd.length) problems.push(`${r.h}: state ${sd.slice(0, 4).join('; ')}`);
        }
      }
      // GL vs WebGPU, natively
      const vsGl = w.vsGl ?? 0.01;
      const glImg = readImage(glq.shot), glState = JSON.parse(readFileSync(glq.state, 'utf8'));
      const dg = diffImages(glImg, refImg);
      if (!dg.comparable) problems.push(`GL vs WebGPU: ${dg.reason}`);
      else if (dg.gt16 > vsGl) problems.push(`GL vs WebGPU: ${(dg.gt16 * 100).toFixed(3)}% pixels differ by >16 (bound ${vsGl * 100}%)`);
      if (w.maxArea !== undefined)
        for (const r of results) {
          const da = diffAreas(glImg, r === ref ? refImg : readImage(r.shot));
          if (da.comparable && da.share > w.maxArea) problems.push(`${r.h} WebGPU vs native GL: ${(da.share * 100).toFixed(3)}% of the frame in solid areas that differ (bound ${w.maxArea * 100}%)`);
        }
      const sg = stateDiff(glState, refState, g.tol ?? 0, g.fields || Object.keys(glState).filter((k) => !/^(dbg|host|cw\.gpu)\./.test(k)));
      if (sg.length) problems.push(`GL vs WebGPU: state ${sg.slice(0, 4).join('; ')}`);
      if (dual) {
        const dd = diffImages(glImg, readImage(dq.shot));
        if (!dd.comparable) problems.push(`romdev dual-GL: ${dd.reason}`);
        else if (dd.gt16 > (g.maxGt16 ?? 0.001)) problems.push(`romdev dual cart on GL: ${(dd.gt16 * 100).toFixed(3)}% pixels differ from native GL by >16`);
        const sd = stateDiff(glState, JSON.parse(readFileSync(dq.state, 'utf8')), g.tol ?? 0, g.fields);
        if (sd.length) problems.push(`romdev dual cart on GL: state ${sd.slice(0, 4).join('; ')}`);
      }
      // controls
      const ctl = p('control');
      const ctlFrames = g.controlFrames ?? (g.controlInput ? g.frames : g.frames - 1);
      const ctlArgs = ['--host', 'native', '--gpu', 'webgpu', '--app', g.app, '--frames', String(ctlFrames), '--shot', ctl.shot, '--state', ctl.state,
        ...(g.frameHz ? ['--frame-hz', String(g.frameHz)] : [])];
      if (g.assets) ctlArgs.push('--assets', join(PROJ, g.assets));
      if (g.controlStartCmds) ctlArgs.push('--start-cmds', g.controlStartCmds.join(';'));
      if (g.controlInput) ctlArgs.push('--input', join(PROJ, 'tests', g.controlInput));
      else if (g.input) ctlArgs.push('--input', join(PROJ, 'tests', g.input));
      const ctlEnv = Object.fromEntries(Object.entries(g.controlEnv || {}).map(([k, v]) => {
        if (typeof v !== 'string' || !v.startsWith('repo:')) return [k, v];
        const copy = join(OUT, g.name + '.wgpu.control.' + basename(v.slice(5)));
        copyFileSync(join(PROJ, v.slice(5)), copy);
        return [k, copy];
      }));
      const c = await run(ctlArgs, ctlEnv);
      if (c.code !== 0) problems.push(`control run failed: ${c.err}`);
      else {
        const ci = readImage(ctl.shot), cs = JSON.parse(readFileSync(ctl.state, 'utf8'));
        const d = diffImages(refImg, ci);
        const sd = stateDiff(refState, cs, g.tol ?? 0, g.fields);
        if (d.gt16 <= bound && !sd.length) problems.push('CONTROL PASSED: the WebGPU cross-host check cannot tell a different run apart');
        const dv = diffImages(glImg, ci);
        const sv = stateDiff(glState, cs, g.tol ?? 0, g.fields || Object.keys(glState).filter((k) => !/^(dbg|host|cw\.gpu)\./.test(k)));
        if (dv.gt16 <= vsGl && !sv.length) problems.push('CONTROL PASSED: the GL-vs-WebGPU check cannot tell a different run apart');
      }
      for (const [i, k] of (w.controls || []).entries()) {
        const q = p('control' + i);
        const args = ['--host', 'native', '--gpu', 'webgpu', ...common, '--shot', q.shot, '--state', q.state];
        const r = await run(args, k.env || {});
        const tag = Object.entries(k.env || {}).map(([a, b]) => a + '=' + b).join(' ');
        if (r.code !== 0) { problems.push(`control [${tag}] run failed: ${r.err.trim().split('\n').slice(-2).join(' | ')}`); continue; }
        const ci = readImage(q.shot);
        const d = diffImages(refImg, ci), dv = diffImages(glImg, ci);
        let told = [];
        if (k.fields) {
          const cs = JSON.parse(readFileSync(q.state, 'utf8')), j = (v) => JSON.stringify(v);
          told = k.fields.filter((f) => j(cs[f]) !== j(refState[f]) && j(cs[f]) !== j(glState[f]));
        }
        const by = told.length ? `; state ${told.join(', ')}` : '';
        if (d.gt16 <= bound && !told.length) problems.push(`CONTROL PASSED [${tag}]: ${(d.gt16 * 100).toFixed(3)}% vs the WebGPU reference is within the bound`);
        if (dv.gt16 <= vsGl && !told.length) problems.push(`CONTROL PASSED [${tag}]: ${(dv.gt16 * 100).toFixed(3)}% vs GL is within vsGl`);
        else ctlNotes.push(`${tag}: ${(d.gt16 * 100).toFixed(2)}% / ${(dv.gt16 * 100).toFixed(2)}%${by}`);
      }
    }
    if (problems.length) { failedW++; console.log(`FAIL ${g.name} [webgpu]\n  ${problems.join('\n  ')}`); }
    else console.log(`ok   ${g.name} [webgpu] (${hosts.join(', ')}; vs GL)${ctlNotes.length ? '; controls fail: ' + ctlNotes.join(', ') : ''} ${adapters.length ? '\n       ' + adapters.join('\n       ') : ''}`);
  }
  return failedW;
}
// ── the romdev mode (--hosts romdev) ─────────────────────────────────────
// Every leg on romdev, no native run: the dual cart on GL is the reference (its state against the gate's expect
// block, its frame a real picture, the text floor, missing program states, prompt errors), and the control (one
// frame short, or the control input, start requests and environment) on romdev GL must differ from it. With
// --gpu webgpu (or both) the same cart runs again on WebGPU: the same checks on its own state, named state EXACT
// against the GL run (the gate's fields), frames within webgpu.vsGl (default 0.01) and webgpu.maxArea, and its own
// control must differ from it. The gate's "env" reaches the cart through the debug channel's env request
// (run-cart --env), so a gate whose game reads a variable before frame 2 fails, naming it.
// A gate whose hosts / skip list leaves romdev out is skipped (said, never counted as passing); so are tool
// gates (a script picks its own hosts) and webgpu.controls (native fault injections).
async function romdevGates(list) {
  const gpus = gpuArg === 'gl' ? ['gl'] : ['gl', 'webgpu'];
  let failedR = 0, ran = 0;
  const skipped = [];
  for (const g of list) {
    if (g.script) { skipped.push(`${g.name} (a tool gate: run ${g.script})`); continue; }
    const eligible = g.hosts || ['native', 'romdev', 'chromium'];
    if (!eligible.includes('romdev') || (g.skip || []).includes('romdev')) { skipped.push(`${g.name} (its hosts / skip list leaves romdev out)`); continue; }
    if (!dualCart(g.app)) { skipped.push(`${g.name} (no cart: build/cart-wgpu/carts/${g.app}.wasc)`); continue; }
    const legs = g.gl === false ? ['webgpu'] : gpus.filter((x) => x === 'gl' || g.webgpu !== false);
    await guard.gate(g.name + ' [romdev]');
    ran++;
    gateEnv = {}; // nothing native runs: the environment goes to the cart as an env request
    const w = g.webgpu && g.webgpu !== true ? g.webgpu : {};
    const common = ['--host', 'romdev', '--app', g.app, ...(g.frameHz ? ['--frame-hz', String(g.frameHz)] : [])];
    const runArgs = ['--frames', String(g.frames), ...(g.input ? ['--input', join(PROJ, 'tests', g.input)] : []),
      ...(g.startCmds ? ['--start-cmds', g.startCmds.join(';')] : []), ...envArg(g.env)];
    const ctlFrames = g.controlFrames ?? (g.controlInput ? g.frames : g.frames - 1);
    const ctlInput = g.controlInput || g.input;
    const ctlCmds = g.controlStartCmds || g.startCmds;
    const ctlEnvObj = { ...(g.env || {}), ...(g.controlEnv || {}) };
    const problems = [], notes = [];
    if (Object.values(ctlEnvObj).some((v) => typeof v === 'string' && v.startsWith('repo:'))) problems.push('controlEnv names a repository file (repo:): a cart cannot read the host\'s files');
    const ctlArgs = ['--frames', String(ctlFrames), ...(ctlInput ? ['--input', join(PROJ, 'tests', ctlInput)] : []),
      ...(ctlCmds ? ['--start-cmds', ctlCmds.join(';')] : []), ...envArg(ctlEnvObj)];
    if (w.controls && w.controls.length) notes.push(`${w.controls.length} webgpu.controls not run (native fault injections)`);
    const leg = {};
    for (const gpu of legs) {
      const q = { shot: join(OUT, `${g.name}.romdev-${gpu}.png`), state: join(OUT, `${g.name}.romdev-${gpu}.json`) };
      const r = await run([...common, '--gpu', gpu, ...runArgs, '--shot', q.shot, '--state', q.state]);
      if (r.code !== 0) { problems.push(`romdev ${gpu}: run failed: ${r.err.trim().split('\n').slice(-3).join(' | ')}`); continue; }
      leg[gpu] = { ...q, img: readImage(q.shot), st: JSON.parse(readFileSync(q.state, 'utf8')) };
      const L = leg[gpu];
      if (uniqueColors(L.img) < (g.minColors || 2)) problems.push(`romdev ${gpu} frame has ${uniqueColors(L.img)} colours: not a real picture`);
      if (!Object.keys(L.st).length) problems.push(`romdev ${gpu} state is empty`);
      if (gpu === 'webgpu' && L.st['cw.gpu.api'] !== 2) problems.push(`romdev webgpu run used gpu api ${L.st['cw.gpu.api']}, not WebGPU`);
      checkMissing(problems, g.app, [[`romdev ${gpu}`, q.state]]);
      checkPromptErrors(problems, g, [[`romdev ${gpu}`, q.state]]);
      checkTextFloor(problems, g, [[`romdev ${gpu}`, q.state]]);
      // glOnlyFields: state the game measures on GL alone (a glReadPixels probe): not expected of, or compared with, WebGPU
      const glOnly = new Set(gpu === 'webgpu' ? g.glOnlyFields || [] : []);
      for (const [k, v] of Object.entries(g.expect || {}))
        if (!glOnly.has(k) && JSON.stringify(L.st[k]) !== JSON.stringify(v)) problems.push(`romdev ${gpu} ${k} = ${JSON.stringify(L.st[k])}, expected ${JSON.stringify(v)}`);
      // the control on the same backend: it must differ from this run
      const c = { shot: join(OUT, `${g.name}.romdev-${gpu}.control.png`), state: join(OUT, `${g.name}.romdev-${gpu}.control.json`) };
      const cr = await run([...common, '--gpu', gpu, ...ctlArgs, '--shot', c.shot, '--state', c.state]);
      if (cr.code !== 0) { problems.push(`romdev ${gpu} control run failed: ${cr.err.trim().split('\n').slice(-3).join(' | ')}`); continue; }
      const ci = readImage(c.shot), cs = JSON.parse(readFileSync(c.state, 'utf8'));
      const d = diffImages(L.img, ci), sd = stateDiff(L.st, cs, g.tol ?? 0, g.fields);
      if (d.comparable && d.any === 0 && !sd.length) problems.push(`CONTROL PASSED (romdev ${gpu}): the gate cannot tell a different run apart`);
      else notes.push(`${gpu} control differs: ${d.comparable ? (d.gt16 * 100).toFixed(2) + '% of pixels > 16' : d.reason}${sd.length ? `, state ${sd.slice(0, 2).join('; ')}` : ''}`);
      leg[gpu].ctl = { img: ci, st: cs };
    }
    // WebGPU against GL, the same cart on the same host
    if (leg.gl && leg.webgpu) {
      const vsGl = w.vsGl ?? g.vsGl ?? 0.01; // a gate with no webgpu block can bound it too (top-level vsGl)
      const fields = (g.fields || Object.keys(leg.gl.st).filter((k) => !/^(dbg|host|cw\.gpu)\./.test(k) && !/^text\..*_where$/.test(k)))
        .filter((k) => !(g.glOnlyFields || []).includes(k));
      // vsGlTol: how far a number may differ between the backends (GPU readbacks round differently), else the gate's tol
      const tolW = g.vsGlTol ?? g.tol ?? 0;
      const dg = diffImages(leg.gl.img, leg.webgpu.img);
      if (!dg.comparable) problems.push(`WebGPU vs GL: ${dg.reason}`);
      else if (dg.gt16 > vsGl) problems.push(`WebGPU vs GL: ${(dg.gt16 * 100).toFixed(3)}% pixels differ by >16 (bound ${vsGl * 100}%)`);
      else notes.push(`WebGPU vs GL ${(dg.gt16 * 100).toFixed(2)}% (bound ${(vsGl * 100).toFixed(1)}%)`);
      if (w.maxArea !== undefined) {
        const da = diffAreas(leg.gl.img, leg.webgpu.img);
        if (da.comparable && da.share > w.maxArea) problems.push(`WebGPU vs GL: ${(da.share * 100).toFixed(3)}% of the frame in solid areas that differ (bound ${w.maxArea * 100}%)`);
      }
      const sg = stateDiff(leg.gl.st, leg.webgpu.st, tolW, fields);
      if (sg.length) problems.push(`WebGPU vs GL: state ${sg.slice(0, 4).join('; ')}`);
      // the WebGPU control against GL: the GL-vs-WebGPU comparison must see it too
      if (leg.webgpu.ctl) {
        const dv = diffImages(leg.gl.img, leg.webgpu.ctl.img), sv = stateDiff(leg.gl.st, leg.webgpu.ctl.st, tolW, fields);
        if (dv.comparable && dv.gt16 <= vsGl && !sv.length) problems.push('CONTROL PASSED: the WebGPU-vs-GL check cannot tell the WebGPU control apart');
      }
    }
    if (problems.length) { failedR++; console.log(`FAIL ${g.name} [romdev ${legs.join('+')}]\n  ${problems.join('\n  ')}`); }
    else console.log(`ok   ${g.name} [romdev ${legs.join('+')}]: ${notes.join('; ')}`);
  }
  for (const s of skipped) console.log(`SKIP ${s}`);
  if (!ran) { console.log('FAIL --hosts romdev ran no gate'); return 1; }
  return failedR;
}
if (hostsArg && !hostsArg.includes('native')) {
  if (hostsArg.join() !== 'romdev') { console.log('FAIL without native, --hosts takes romdev alone (the romdev mode: its GL run is the reference)'); guard.finish(); process.exit(2); }
  const f = await romdevGates(gates);
  guard.finish();
  process.exit(f ? 1 : 0);
}

if (gpuArg === 'webgpu') {
  // a gate named on the command line that has no "webgpu" entry would be skipped in silence, and a run of nothing but
  // such gates would pass: say which, and fail when none of the named gates has a WebGPU leg
  if (only.length) {
    const none = gates.filter((g) => !g.webgpu && !g.script).map((g) => g.name);
    for (const n of none) console.log(`SKIP ${n} [webgpu]: no "webgpu" entry in its gate file`);
    if (none.length === gates.length) { console.log('FAIL no named gate has a "webgpu" entry: --gpu webgpu ran nothing'); guard.finish(); process.exit(1); }
  }
  const f = await wgpuGates(gates);
  guard.finish();
  process.exit(f ? 1 : 0);
}

let failed = 0;
for (const g of gates) {
  if (g.gl === false) continue; // a WebGPU-only gate (its controls are WebGPU faults)
  if (cpuMode && (g.script || !g.expect)) { console.log(`skip ${g.name} (--cpu runs only gates with an expect block)`); continue; }
  // tool gates that launch Chromium somewhere (its source names it): left out by --no-chromium, the only tool gates of --chromium-only
  // ("chromium": true in gates.json when the script reaches Chromium only through data, e.g. tutorial specs)
  const chrome = g.script && (g.chromium === true || /chromium/i.test(readFileSync(join(PROJ, g.script), 'utf8')));
  if (g.script && chromiumOnly && !chrome) continue;
  if (g.script && noChromium && chrome) { console.log('skip ' + g.name + ' (has Chromium legs: --chromium-only)'); continue; }
  await guard.gate(g.name);
  gateEnv = g.env || {};
  // a tool gate: a node script that runs its own checks and controls (exit 0 = pass)
  if (g.script) {
    const r = await new Promise((res) => {
      const p = spawn('node', [join(PROJ, g.script)], { stdio: ['ignore', 'pipe', 'pipe'] });
      let out = '';
      p.stdout.on('data', (d) => (out += d));
      p.stderr.on('data', (d) => (out += d));
      p.on('close', (code) => res({ code, out }));
    });
    if (r.code) { failed++; console.log('FAIL ' + g.name + '\n  ' + r.out.trim().split('\n').filter((l) => !l.startsWith('ok')).join('\n  ')); }
    else console.log('ok   ' + g.name + ' (' + g.script + ')');
    continue;
  }
  // wasmcart-run takes no input script, so it joins only input-free gates
  const defaults = g.input ? ['native', 'romdev', 'chromium'] : ['native', 'romdev', 'chromium', 'wcnative'];
  if (process.env.CW_ANGLE_DIR) defaults.push('angle'); // the Windows/macOS/iOS GL stack, on this machine
  const eligible = g.hosts || defaults; // --hosts narrows the set; it never adds a host a gate cannot run on
  const hosts = glHosts((hostsArg ? hostsArg.filter((h) => eligible.includes(h)) : eligible).filter((h) => !(g.skip || []).includes(h)), g.name, g.app);
  if (chromiumOnly && !hosts.includes('chromium')) continue;
  warnDropped(g.name, hosts);
  const common = ['--app', g.app, '--frames', String(g.frames), ...(g.frameHz ? ['--frame-hz', String(g.frameHz)] : []), ...envArg(g.env)];
  if (g.input) common.push('--input', join(PROJ, 'tests', g.input));
  if (g.startCmds) common.push('--start-cmds', g.startCmds.join(';'));
  if (g.assets) common.push('--assets', join(PROJ, g.assets)); // native asset root when it is not samples/<app>/assets
  const paths = (h) => ({ shot: join(OUT, `${g.name}.${h}.png`), state: join(OUT, `${g.name}.${h}.json`) });
  const results = await Promise.all(hosts.map(async (h) => {
    const p = paths(h);
    const extra = h === 'wcnative' ? ['--shot', p.shot] : ['--shot', p.shot, '--state', p.state];
    return { h, ...p, ...(await run(['--host', h, ...common, ...extra])) };
  }));
  const ref = results.find((r) => r.h === 'native');
  const problems = [];
  for (const r of results) if (r.code !== 0) problems.push(`${r.h}: run failed: ${r.err.trim().split('\n').slice(-3).join(' | ')}`);
  if (!problems.length) {
    const refImg = readImage(ref.shot);
    const refState = JSON.parse(readFileSync(ref.state, 'utf8'));
    if (uniqueColors(refImg) < (g.minColors || 2)) problems.push(`native frame has ${uniqueColors(refImg)} colours: not a real picture`);
    if (!Object.keys(refState).length) problems.push('native state is empty');
    checkMissing(problems, g.app, results.filter((r) => r.h !== 'wcnative').map((r) => [r.h, r.state]));
    checkPromptErrors(problems, g, results.filter((r) => r.h !== 'wcnative').map((r) => [r.h, r.state]));
    checkTextFloor(problems, g, results.filter((r) => r.h !== 'wcnative').map((r) => [r.h, r.state]));
    for (const [k, v] of Object.entries(g.expect || {}))
      if (JSON.stringify(refState[k]) !== JSON.stringify(v)) problems.push(`native ${k} = ${JSON.stringify(refState[k])}, expected ${JSON.stringify(v)}`);
    for (const r of results) {
      if (r === ref) continue;
      const d = diffImages(refImg, readImage(r.shot));
      if (!d.comparable) problems.push(`${r.h}: ${d.reason}`);
      else if (d.gt16 > (g.maxGt16 ?? 0.001)) problems.push(`${r.h}: ${(d.gt16 * 100).toFixed(3)}% pixels differ by >16 (max ${d.max})`);
      if (r.h !== 'wcnative') {
        const sd = stateDiff(refState, JSON.parse(readFileSync(r.state, 'utf8')), g.tol ?? 0, g.fields);
        if (sd.length) problems.push(`${r.h}: state ${sd.slice(0, 4).join('; ')}`);
      }
    }
    // control: a run that must NOT match (one frame short, or the control input)
    const ctl = { shot: join(OUT, `${g.name}.control.png`), state: join(OUT, `${g.name}.control.json`) };
    const ctlFrames = g.controlFrames ?? (g.controlInput ? g.frames : g.frames - 1);
    const ctlArgs = ['--host', 'native', '--app', g.app, '--frames', String(ctlFrames), '--shot', ctl.shot, '--state', ctl.state, ...(g.frameHz ? ['--frame-hz', String(g.frameHz)] : [])];
    if (g.assets) ctlArgs.push('--assets', join(PROJ, g.assets));
    if (g.controlStartCmds) ctlArgs.push('--start-cmds', g.controlStartCmds.join(';'));
    if (g.controlInput) ctlArgs.push('--input', join(PROJ, 'tests', g.controlInput));
    else if (g.input) ctlArgs.push('--input', join(PROJ, 'tests', g.input));
    // controlEnv values "repo:<path>" name files in this repository; the run gets a
    // copy in out/ so an app that writes its input back (the editor saves its
    // settings on exit) never dirties the tree
    const ctlEnv = Object.fromEntries(Object.entries(g.controlEnv || {}).map(([k, v]) => {
      if (typeof v !== 'string' || !v.startsWith('repo:')) return [k, v];
      const copy = join(OUT, g.name + '.control.' + basename(v.slice(5)));
      copyFileSync(join(PROJ, v.slice(5)), copy);
      return [k, copy];
    }));
    const c = await run(ctlArgs, ctlEnv);
    if (c.code !== 0) problems.push(`control run failed: ${c.err}`);
    else {
      const d = diffImages(refImg, readImage(ctl.shot));
      const sd = stateDiff(refState, JSON.parse(readFileSync(ctl.state, 'utf8')), g.tol ?? 0, g.fields);
      if (d.any === 0 && !sd.length) problems.push('CONTROL PASSED: the gate cannot tell a different run apart');
    }
  }
  if (problems.length) { failed++; console.log(`FAIL ${g.name}\n  ${problems.join('\n  ')}`); }
  else console.log(`ok   ${g.name} (${hosts.join(', ')})`);
}
guard.finish();
process.exit(failed ? 1 : 0);
