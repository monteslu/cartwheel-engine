// M4.6 exit gate: "New project -> export native + cart with one action".
//
// Two sessions with the native editor, each over the debug channel's socket
// (tools/lib/edsession.mjs), the editor running in real time (headless):
//   scripted  `ed` commands only: project.new from the 2D platformer template,
//             then export.run presets=native,cart (ONE action: the editor runs
//             the engine's exporter, tools/cwexport.mjs, which builds and
//             packages both);
//   pointer   the mouse only (CW_INPUT clicks at widget rects the editor
//             reports with ui.rect): the Projects tab, the 3D Platformer
//             template, Create Project, the Export tab, Export All; the socket
//             only watches.
// Then every export RUNS and its game state is probed through the debug
// channel: the native executable (CW_INPUT pad script, CW_DEBUG_DUMP, and
// CW_DEBUG_CMDS requests), the .wasc on romdev (pad script, debugState,
// wasm({op:'command'}) requests) and on wasmcart-native (wasmcart-run
// --debug-cmd teleports the player onto a pickup, --debug-dump reads the
// state).
// Controls that MUST fail (else the checks could not detect anything):
//   - an invalid preset (unknown platform) is refused with a message naming
//     the preset and the problem, locally and by the exporter (a bad version);
//   - each probe without its input (no pad script, no teleport) leaves the
//     pickups uncollected: the same check must fail.
//
//   node tests/project-export.mjs     (romdev on 127.0.0.1:7331; wcnative beside the checkout)
// It needs the M5.0 exporter (tools/cwexport.mjs); tests/gates.json lists it
// once that is in main.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, rmSync, writeFileSync, readFileSync, readdirSync, statSync, mkdtempSync } from 'node:fs';
import { join, resolve, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { wcnativePlayer } from '../tools/lib/wcnative.mjs';
import { launchApp, until } from '../tools/lib/edsession.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'project-export');
const EDITOR = join(ROOT, 'build/native/editor/editor');
const EXPORT_TIMEOUT = 45 * 60 * 1000;
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
if (!existsSync(EDITOR)) { console.log(`FAIL missing ${EDITOR}: run scripts/build.sh`); process.exit(1); }

let failed = 0;
function check(name, ok, detail = '') {
  if (ok) console.log(`ok   ${name}`);
  else { failed++; console.log(`FAIL ${name}${detail ? ': ' + String(detail).slice(0, 1500) : ''}`); }
  return ok;
}
/** a control: `ok` is the result the real check would give; it must be false */
function control(name, realCheckPasses, detail = '') {
  return check(`control must fail: ${name}`, !realCheckPasses, realCheckPasses ? `the check passed without its cause: ${detail}` : '');
}

function settings(file, extra) {
  writeFileSync(file, `# project-export gate\n${Object.entries(extra).map(([k, v]) => `${k} = ${v}`).join('\n')}\n`);
  return file;
}

/** every file under dir (relative paths) */
function walk(dir, base = dir) {
  if (!existsSync(dir)) return [];
  return readdirSync(dir).flatMap((f) => {
    const p = join(dir, f);
    return statSync(p).isDirectory() ? walk(p, base) : [p];
  });
}

/** the native export's executable: a file named after the app (not a library), executable */
function findExe(dir, app) {
  return walk(dir).find((p) => basename(p) === app && (statSync(p).mode & 0o111) && !p.includes('.AppDir')) ||
         walk(dir).find((p) => basename(p) === app && (statSync(p).mode & 0o111));
}

async function waitExport(s, what) {
  const t0 = Date.now();
  return until(async () => {
    const st = await s.ed('export.status');
    if (st.state === 'idle' && Date.now() - t0 > 15000) throw new Error(`${what}: no export started (${JSON.stringify(st)})`);
    return st.state !== 'running' && st.state !== 'idle' ? st : null;
  }, EXPORT_TIMEOUT, what);
}

/* ── probes: run an export and read its game state ───────────────────── */


/** native executable: CW_FRAMES with a pad script; state = CW_DEBUG_DUMP; requests = CW_DEBUG_CMDS replies */
function runNative(exe, frames, input, cmds) {
  const dir = mkdtempSync(join(OUT, 'run-'));
  const env = { ...process.env, CW_FRAMES: String(frames), SDL_AUDIODRIVER: 'dummy', CW_DEBUG_DUMP: join(dir, 'state.json'),
    CW_SCREENSHOT: join(dir, 'shot.png'), CW_DEBUG_CMDS: cmds.join(';') };
  if (input) {
    writeFileSync(join(dir, 'input.txt'), input.map((s) => `${s.frame - 1} ${s.buttons || 0} ${s.lx || 0} ${s.ly || 0} 0 0 0 0`).join('\n') + '\n');
    env.CW_INPUT = join(dir, 'input.txt');
  }
  const r = spawnSync(exe, [], { env, encoding: 'utf8', timeout: 120000, cwd: dirname(exe) });
  const replies = (r.stdout || '').split('\n').filter((l) => l.startsWith('{'));
  const state = existsSync(join(dir, 'state.json')) ? JSON.parse(readFileSync(join(dir, 'state.json'), 'utf8')) : null;
  return { status: r.status, state, replies, out: (r.stdout || '') + (r.stderr || ''), shot: join(dir, 'shot.png') };
}

async function romdev(tool, a, session) {
  const res = await fetch(`http://127.0.0.1:7331/tool/${tool}`, { method: 'POST',
    headers: { 'Content-Type': 'application/json', 'x-romdev-session': session }, body: JSON.stringify(a) });
  const text = await res.text();
  let body;
  try { body = JSON.parse(text); } catch { body = text; }
  if (body && Array.isArray(body.content)) {
    const t = body.content.find((c) => c.type === 'text');
    try { body = JSON.parse(t.text); } catch { body = t ? t.text : body; }
  }
  if (!res.ok || (body && (body.isError || body.error))) throw new Error(`romdev ${tool}: ${JSON.stringify(body).slice(0, 800)}`);
  return body;
}

/** the .wasc on romdev: pad script, then debugState + channel requests */
async function runRomdev(wasc, frames, input, cmds) {
  const session = `cw-project-export-${process.pid}-${Math.random().toString(36).slice(2, 8)}`;
  try {
    await romdev('loadMedia', { platform: 'wasmcart', path: wasc, deterministicSeed: 1, deterministicStepMs: 1000 / 60 }, session);
    let at = 1;
    for (const s of input || []) {
      if (s.frame - 1 > at) { await romdev('frame', { op: 'step', frames: s.frame - 1 - at }, session); at = s.frame - 1; }
      const pad = {};
      if (s.buttons & 1) pad.south = true;
      pad.axes = { lx: s.lx || 0, ly: s.ly || 0 };
      await romdev('input', { op: 'set', ports: [pad] }, session);
    }
    if (frames > at) await romdev('frame', { op: 'step', frames: frames - at }, session);
    const st = await romdev('wasm', { op: 'debugState' }, session);
    const state = {};
    for (const f of st.fields || []) state[f.name] = f.values ?? f.value;
    const replies = [];
    for (const c of cmds) {
      const r = await romdev('wasm', { op: 'command', text: c, maxFrames: 60 }, session);
      replies.push(JSON.stringify(r.reply ?? r));
    }
    return { state, replies };
  } finally {
    await romdev('host', { op: 'shutdown' }, session).catch(() => {});
  }
}

/** the .wasc on wasmcart-native: --debug-cmd requests at frames, --debug-dump the state */
async function runWcnative(wasc, frames, cmds) {
  const exe = wcnativePlayer(); // the headless-capable player (tools/lib/wcnative.mjs)
  const dir = mkdtempSync(join(OUT, 'wcn-'));
  const dump = join(dir, 'state.json');
  const args = [wasc, '--fixed-step', String(1000 / 60), '--uncapped', '--debug-dump', String(frames - 1), dump];
  args.push('--debug-cmd', '0', 'warmup_spread 0'); // the warm-up in frame 1, as on romdev (a deterministic load)
  for (const [f, text] of cmds) args.push('--debug-cmd', String(f), text);
  const env = { ...process.env, SDL_AUDIODRIVER: 'dummy' };
  testEnv(env);
  const { spawn } = await import('node:child_process');
  const child = spawn(exe, args, { env, stdio: ['ignore', 'ignore', 'pipe'] });
  let err = '';
  child.stderr.on('data', (d) => (err += d));
  const deadline = Date.now() + 120000;
  while (!existsSync(dump) && child.exitCode === null && Date.now() < deadline) await new Promise((r) => setTimeout(r, 100));
  await new Promise((r) => setTimeout(r, 300));
  child.kill('SIGTERM');
  if (!existsSync(dump)) throw new Error(`wcnative: no debug dump: ${err.slice(-1500)}`);
  // replies come in request order: the first answers this harness's own warmup_spread 0
  // (it must have taken: false), the rest the caller's commands
  const [warm, ...replies] = err.split('\n').filter((l) => l.includes('debug reply')).map((l) => l.replace(/^.*debug reply \d+: /, ''));
  if (warm !== '{"warmup_spread":false}') throw new Error(`wcnative: warmup_spread 0 answered ${warm}: ${err.slice(-1500)}`);
  return { state: JSON.parse(readFileSync(dump, 'utf8')), replies };
}

/* ── the scripted session ─────────────────────────────────────────────── */

const WALK_RIGHT = [{ frame: 10, lx: 1 }]; // pad 0 stick right: the player runs through the pickups on its row

async function scripted() {
  const dir = join(OUT, 'scripted');
  const s = await launchApp(EDITOR, { env: { CW_EDITOR_SETTINGS: settings(join(OUT, 'scripted.cfg'), { 'projects.dir': OUT }) } });
  try {
    const made = await s.ed(`project.new name="Scripted Platformer" template=platformer_2d dir=${dir}`);
    check('scripted: project.new from platformer_2d', made.ok && made.app === 'scripted_platformer' && made.files >= 7, JSON.stringify(made));
    const info = await s.ed('project.info');
    check('scripted: the new project is open', info.ok && info.name === 'Scripted Platformer' && info.storage === 'disk', JSON.stringify(info));
    for (const sc of ['main.cwscene', 'coin.cwscene']) {
      const c = await s.ed(`scene.check path=${sc}`);
      check(`scripted: template scene ${sc} is lossless and canonical in the editor`, c.lossless && c.canonical, JSON.stringify(c));
    }
    const presets = await s.ed('export.presets');
    check('scripted: the template has native (linux-x64) + cart (wasc) presets', presets.ok &&
      presets.presets.some((p) => p.name === 'native' && p.platform === 'linux-x64' && p.valid) &&
      presets.presets.some((p) => p.name === 'cart' && p.platform === 'wasc' && p.valid) && presets.exporter.endsWith('cwexport.mjs'), JSON.stringify(presets));

    // controls: invalid presets are refused clearly (locally, then by the exporter)
    await s.ed('project.set key=export.bad.platform value=amiga');
    const bad = await s.ed('export.run presets=bad');
    control('export.run of a preset with an unknown platform', bad.ok, JSON.stringify(bad));
    check('the refusal names the preset and the problem', !bad.ok && /export\.bad\.platform = amiga: unknown platform/.test(bad.error), bad.error);
    const log = await s.ed('log.tail n=5');
    check('the refusal is in the Output log', JSON.stringify(log).includes('invalid preset'), JSON.stringify(log).slice(0, 400));
    await s.ed('export.remove name=bad');
    await s.ed('export.add name=badver platform=wasc');
    await s.ed('project.set key=export.badver.version value=banana');
    const v = await s.ed('export.validate presets=badver');
    const vst = v.ok && v.started ? await waitExport(s, 'validation') : { state: v.ok ? 'ok' : 'failed', ...v };
    control('the exporter validating a preset with version "banana"', vst.state === 'ok', JSON.stringify(vst));
    check('the exporter names the problem', JSON.stringify(vst).includes('banana'), JSON.stringify(vst));
    await s.ed('export.remove name=badver');

    // ONE action: export native + cart
    const t0 = Date.now();
    const run = await s.ed('export.run presets=native,cart');
    check('scripted: export.run presets=native,cart starts', run.ok, JSON.stringify(run));
    const st = await waitExport(s, 'the export');
    check(`scripted: both presets exported (${((Date.now() - t0) / 1000).toFixed(0)} s)`, st.state === 'ok' && st.results.length === 2 && st.results.every((r) => r.ok),
      JSON.stringify(st));
    const tail = await s.ed('log.tail n=400');
    check('scripted: the exporter output is in the Output log', JSON.stringify(tail).includes('export: '), '');
    return { dir, app: 'scripted_platformer', results: st.results };
  } finally {
    await s.close();
  }
}

/* ── the pointer session ─────────────────────────────────────────────── */

async function pointer() {
  const base = join(OUT, 'pointer');
  const cfg = { 'projects.dir': base };
  // 1. where the widgets are (a throwaway editor with the same settings and window)
  const probe = await launchApp(EDITOR, { env: { CW_EDITOR_SETTINGS: settings(join(OUT, 'pointer-probe.cfg'), cfg) } });
  const at = {};
  try {
    const rect = async (n, tab) => {
      const r = await probe.ed(`ui.rect name=${n}`);
      if (!r.ok || (tab ? !r.tab : !r.visible)) throw new Error(`ui.rect ${n}: ${JSON.stringify(r)}`);
      return r;
    };
    const settle = async () => { const f0 = (await probe.request('status')).frame; await until(async () => (await probe.request('status')).frame > f0 + 3, 20000, 'frames'); };
    await settle(); // laid out
    at.projectsTab = (await rect('Projects', true)).tab;
    await probe.ed('panel.show projects');
    await settle();
    at.template = (await rect('PmTemplate_platformer_3d')).rect;
    at.create = (await rect('PmCreate')).rect;
    at.exportTab = (await rect('Export', true)).tab;
    await probe.ed('panel.show export');
    await settle();
    at.exportAll = (await rect('ExpRunAll')).rect;
  } finally {
    await probe.close();
  }
  // 2. the session: clicks only (press on one frame, release two later)
  const steps = [];
  let f = 90;
  const click = (r) => {
    steps.push({ frame: f, x: r.cx, y: r.cy, b: 0 }, { frame: f + 2, x: r.cx, y: r.cy, b: 1 }, { frame: f + 4, x: r.cx, y: r.cy, b: 0 });
    f += 30;
  };
  click(at.projectsTab);
  click(at.template);
  click(at.create);
  f += 60; // the project opens
  click(at.exportTab);
  click(at.exportAll);
  const input = join(OUT, 'pointer-input.txt');
  writeFileSync(input, steps.map((s) => `${s.frame} 0 0 0 0 0 0 0 m=${s.x},${s.y},${s.b}`).join('\n') + '\n');
  const s = await launchApp(EDITOR, { env: { CW_EDITOR_SETTINGS: settings(join(OUT, 'pointer.cfg'), cfg), CW_INPUT: input } });
  try {
    const made = await until(async () => {
      const i = await s.ed('project.info');
      return i.name === 'New Game' ? i : null;
    }, 60000, 'the clicked project to open').catch(async (e) => {
      await s.screenshot('ui', join(OUT, 'pointer-stuck.png')).catch(() => {});
      const log = await s.ed('log.tail n=30').catch(() => ({}));
      throw new Error(`${e.message}; widget rects ${JSON.stringify(at)}; log ${JSON.stringify(log).slice(0, 2000)}`);
    });
    check('pointer: Projects tab, 3D Platformer, Create Project made and opened a project', made.dir === join(base, 'new_game'), JSON.stringify(made));
    const tpl = await s.ed('project.get key=app');
    check('pointer: from the 3D platformer template', existsSync(join(base, 'new_game', 'gem.cwscene')) && tpl.value === 'new_game', JSON.stringify(tpl));
    const started = await until(async () => {
      const st = await s.ed('export.status');
      return st.state !== 'idle' ? st : null;
    }, 60000, 'Export All to start the export');
    check('pointer: Export tab + Export All started the export', started.presets.length === 2, JSON.stringify(started));
    const st = await waitExport(s, 'the pointer export');
    check('pointer: both presets exported', st.state === 'ok' && st.results.every((r) => r.ok), JSON.stringify(st));
    await s.screenshot('ui', join(OUT, 'pointer-editor.png'));
    return { dir: join(base, 'new_game'), app: 'new_game', results: st.results };
  } finally {
    await s.close();
  }
}

/* ── run the exports ─────────────────────────────────────────────────── */

async function probeExports(tag, ex, kind) {
  const nat = ex.results.find((r) => r.preset === 'native'), cart = ex.results.find((r) => r.preset === 'cart');
  const exe = nat && findExe(join(ex.dir, 'exports', 'native'), ex.app);
  const wasc = cart && (cart.files || []).map((f) => resolve(ex.dir, f)).find((f) => f.endsWith('.wasc'));
  check(`${tag}: the native export has the executable`, !!exe, JSON.stringify(nat));
  check(`${tag}: the cart export has the .wasc`, !!wasc && existsSync(wasc), JSON.stringify(cart));
  const picked = kind === '2d' ? 'coins' : 'gems';
  const P = kind === '2d' ? 'Level/Player' : 'Level/Player';
  if (exe) {
    const r = runNative(exe, 200, WALK_RIGHT, ['status', `get ${P} position`]);
    check(`${tag}: native export runs`, r.status === 0 && r.state, r.out.slice(-800));
    const moved = kind === '2d' ? r.state?.['player.pos']?.[0] > 300 : r.state?.['player.pos']?.[0] > 1.5;
    check(`${tag}: native export plays (pad: player ran, ${picked} = ${r.state?.[picked]})`, moved && r.state?.[picked] >= 1, JSON.stringify(r.state));
    check(`${tag}: native export answers the debug channel`, r.replies.length === 2 && r.replies[0].includes('"tick":200') && r.replies[1].includes('"type":"vec'),
      r.replies.join(' | '));
    const c = runNative(exe, 200, null, []);
    control(`${tag}: native export without the pad script`, c.state?.[picked] >= 1, JSON.stringify(c.state));
  }
  if (wasc) {
    const r = await runRomdev(wasc, 200, WALK_RIGHT, ['status', `get ${P} position`]).catch((e) => ({ error: e.message }));
    check(`${tag}: cart on romdev plays (pad: ${picked} = ${r.state?.[picked]})`, !r.error && r.state?.[picked] >= 1, r.error || JSON.stringify(r.state));
    check(`${tag}: cart on romdev answers the debug channel`, !r.error && r.replies.length === 2 && r.replies[1].includes('"type":"vec'), r.error || r.replies.join(' | '));
    const c = await runRomdev(wasc, 200, null, []).catch((e) => ({ error: e.message }));
    control(`${tag}: cart on romdev without the pad script`, !c.error && c.state?.[picked] >= 1, JSON.stringify(c.state));
    // wasmcart-native has no input injection: the debug channel puts the player on a pickup
    const onto = kind === '2d' ? `set ${P} position vec2(120, 110)` : `set ${P} position vec3(2, 1.5, 0)`;
    const w = await runWcnative(wasc, 120, [[30, onto], [60, 'status']]).catch((e) => ({ error: e.message }));
    check(`${tag}: cart on wcnative: teleported over the debug channel onto a pickup (${picked} = ${w.state?.[picked]})`,
      !w.error && w.state?.[picked] >= 1 && w.replies?.length === 2 && w.replies[1].includes('"tick":'), w.error || JSON.stringify({ s: w.state, r: w.replies }));
    const wc = await runWcnative(wasc, 120, [[60, 'status']]).catch((e) => ({ error: e.message }));
    control(`${tag}: cart on wcnative without the teleport`, !wc.error && wc.state?.[picked] >= 1, wc.error || JSON.stringify(wc.state));
  }
}

const a = process.env.PE_SKIP_SCRIPTED ? null : await scripted().catch((e) => (check('scripted session', false, e.stack), null));
if (a) await probeExports('scripted (2D platformer)', a, '2d');
const b = await pointer().catch((e) => (check('pointer session', false, e.stack), null));
if (b) await probeExports('pointer (3D platformer)', b, '3d');
console.log(failed ? `${failed} check(s) failed` : 'project-export: all checks passed');
process.exit(failed ? 1 : 0);
