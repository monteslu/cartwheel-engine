// M5.2 plugins gate: a project's plugin (samples/plugin_demo/plugins/turntable,
// copied to a fresh project directory, i.e. a third party's tree) used
// WITHOUT editing the cartwheel tree.
//
//   1. builds: the plugin's shared libraries (tools/cwproject.mjs build <dir>
//      plugins) and the game natively (build <dir> native, the runtime plugin
//      compiled in); the game runs with the Turntable type and the plugin's
//      `turntables` debug command; the cart (repository tree, same sources)
//      on wasmcart-native with --debug-dump / --debug-cmd (gates plugin-demo
//      and unit-plugin cover native, romdev and chromium)
//   2. native editor, scripted (`ed` over the socket): the plugin loads (its
//      panel, inspector plugin, gizmo, commands, menu items), the inspector
//      plugin renders for a Turntable and not for another node
//   3. native editor, pointer only (CW_INPUT at rects a probe editor reported):
//      the Turntables tab, its list row (selects), a drag of the gizmo's radius
//      handle (one undo step), the inspector plugin's slider and Reverse
//   4. hot reload: the plugin's sources edited and rebuilt OUTSIDE the editor;
//      the running editor picks the new editor + runtime code up; a failing
//      build keeps the old code
//   5. disable: refused while a scene using its type has unsaved changes;
//      then its panel, commands, inspector plugin and gizmo go, the type is
//      withdrawn, scene.open refuses the scene with a message naming the
//      plugin (the file is byte-identical), the manifest says enabled = false;
//      the game rebuilt without it reports the missing type by plugin name;
//      enable brings everything back
//   6. version mismatch: engine_api = 2.0 is refused by the editor and by the
//      build with a clear message; code compiled for another API (a stale
//      library) is refused at load
//   7. the project built as a cart game (romdev: the Turntable turns) and as
//      its cart editor (cwproject build <dir> editor: plugin_demo_editor.wasc,
//      the plugin compiled in) on romdev: the plugin is there, the scene opens,
//      the inspector plugin renders, disable/enable work; the stock
//      editor.wasc refuses the same scene (the type is not there)
// Every check that could pass by accident has a control that MUST fail.
//
//   node tests/plugins.mjs      (romdev on 127.0.0.1:7331; wasmcart-native beside the checkout)
import { spawnSync, spawn } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, rmSync, writeFileSync, readFileSync, readdirSync, mkdtempSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { wcnativePlayer } from '../tools/lib/wcnative.mjs';
import { launchApp, until } from '../tools/lib/edsession.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'plugins');
const EDITOR = join(ROOT, 'build/native/editor/editor');
const PROJ = join(OUT, 'proj');
const CWPROJECT = join(ROOT, 'tools/cwproject.mjs');
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
for (const f of [EDITOR, join(ROOT, 'build/cart/carts/plugin_demo.wasc'), join(ROOT, 'build/cart/carts/plugin_demo_editor.wasc')])
  if (!existsSync(f)) { console.log(`FAIL missing ${f}: run scripts/build.sh`); process.exit(1); }

let failed = 0;
function check(name, ok, detail = '') {
  if (ok) console.log(`ok   ${name}`);
  else { failed++; console.log(`FAIL ${name}${detail ? ': ' + String(detail).slice(0, 1500) : ''}`); }
  return ok;
}
function control(name, realCheckPasses, detail = '') {
  return check(`control must fail: ${name}`, !realCheckPasses, realCheckPasses ? `the check passed without its cause: ${detail}` : '');
}
const sha = (f) => createHash('sha256').update(readFileSync(f)).digest('hex');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const MANIFEST = join(PROJ, 'plugins/turntable/plugin.cwplugin');
const EDSRC = join(PROJ, 'plugins/turntable/editor/turntable_editor.c');
const RTSRC = join(PROJ, 'plugins/turntable/runtime/turntable.c');
const editFile = (f, from, to) => {
  const t = readFileSync(f, 'utf8');
  if (!t.includes(from)) throw new Error(`${f}: no "${from}"`);
  writeFileSync(f, t.replace(from, to));
};
function settings(name, extra = {}) {
  const file = join(OUT, `${name}.cfg`);
  // the 3D view's select tool: the gizmo handle is not under a move handle;
  // the right_bottom panels closed: the Inspector gets the whole right column
  // (the Turntable's rows are below the fold otherwise)
  const all = { 'viewport3d.tool': '0', 'plugins.poll_frames': '10', 'layout.panels': 'closed:agent,triggers', ...extra };
  writeFileSync(file, `# plugins gate\n${Object.entries(all).map(([k, v]) => `${k} = ${v}`).join('\n')}\n`);
  return file;
}
function cwproject(target) {
  const r = spawnSync('node', [CWPROJECT, 'build', PROJ, target], { encoding: 'utf8', timeout: 30 * 60 * 1000 });
  const res = (r.stdout || '').split('\n').filter((l) => l.startsWith('@result ')).map((l) => JSON.parse(l.slice(8)));
  return { ok: r.status === 0, results: res, out: (r.stdout || '') + (r.stderr || '') };
}

/** the project's native game: CW_FRAMES, CW_DEBUG_DUMP, CW_DEBUG_CMDS replies */
function runGame(frames, cmds) {
  const exe = join(PROJ, '.cwbuild/native/project/plugin_demo');
  const dir = mkdtempSync(join(OUT, 'game-'));
  const r = spawnSync(exe, [], { cwd: dirname(exe), encoding: 'utf8', timeout: 120000,
    env: { ...process.env, SDL_VIDEODRIVER: 'offscreen', SDL_AUDIODRIVER: 'dummy', CW_FRAMES: String(frames), CW_DEBUG_DUMP: join(dir, 'state.json'),
      CW_DEBUG_CMDS: cmds.join(';'), CW_SCREENSHOT: join(dir, 'shot.png') } });
  const state = existsSync(join(dir, 'state.json')) ? JSON.parse(readFileSync(join(dir, 'state.json'), 'utf8')) : null;
  return { status: r.status, state, replies: (r.stdout || '').split('\n').filter((l) => l.startsWith('{')), out: (r.stdout || '') + (r.stderr || '') };
}

async function runWcnative(wasc, frames, cmds) {
  const exe = wcnativePlayer(); // the headless-capable player (tools/lib/wcnative.mjs)
  const dir = mkdtempSync(join(OUT, 'wcn-'));
  const dump = join(dir, 'state.json');
  const args = [wasc, '--fixed-step', String(1000 / 60), '--uncapped', '--debug-dump', String(frames - 1), dump];
  args.push('--debug-cmd', '0', 'warmup_spread 0'); // the warm-up in frame 1, as on romdev (a deterministic load)
  for (const [f, text] of cmds) args.push('--debug-cmd', String(f), text);
  const env = { ...process.env, SDL_AUDIODRIVER: 'dummy' };
  testEnv(env);
  const child = spawn(exe, args, { env, stdio: ['ignore', 'ignore', 'pipe'] });
  let err = '';
  child.stderr.on('data', (d) => (err += d));
  const deadline = Date.now() + 120000;
  while (!existsSync(dump) && child.exitCode === null && Date.now() < deadline) await sleep(100);
  await sleep(300);
  child.kill('SIGTERM');
  if (!existsSync(dump)) throw new Error(`wcnative: no debug dump: ${err.slice(-1500)}`);
  // replies come in request order: the first answers this harness's own warmup_spread 0
  // (it must have taken: false), the rest the caller's commands
  const [warm, ...replies] = err.split('\n').filter((l) => l.includes('debug reply')).map((l) => l.replace(/^.*debug reply \d+: /, ''));
  if (warm !== '{"warmup_spread":false}') throw new Error(`wcnative: warmup_spread 0 answered ${warm}: ${err.slice(-1500)}`);
  return { state: JSON.parse(readFileSync(dump, 'utf8')), replies };
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
/** a cart editor in romdev: run `ed` lines through the debug channel */
async function romdevEditor(wasc, fn) {
  const session = `cw-plugins-${process.pid}-${Math.random().toString(36).slice(2, 8)}`;
  try {
    await romdev('loadMedia', { platform: 'wasmcart', path: wasc, deterministicSeed: 1, deterministicStepMs: 1000 / 60 }, session);
    await romdev('frame', { op: 'step', frames: 30 }, session);
    const req = async (text) => {
      const r = await romdev('wasm', { op: 'command', text, maxFrames: 120 }, session);
      const rep = r.reply ?? r;
      return typeof rep === 'string' ? JSON.parse(rep) : rep;
    };
    return await fn({ ed: (line) => req('ed64 ' + Buffer.from(line, 'utf8').toString('base64')), request: req });
  } finally {
    await romdev('host', { op: 'shutdown' }, session).catch(() => {});
  }
}

/* ── 1. builds and the game ─────────────────────────────────────────── */

cpSync(join(ROOT, 'samples/plugin_demo'), PROJ, { recursive: true, filter: (s) => !s.includes('.cwbuild') });
let b = cwproject('plugins');
check('project build: the plugin libraries (cwproject build <dir> plugins)', b.ok && existsSync(join(PROJ, '.cwbuild/native/plugins/turntable/turntable_editor.so')) &&
  existsSync(join(PROJ, '.cwbuild/native/plugins/turntable/turntable_runtime.so')), b.out.slice(-1500));
const t0 = Date.now();
b = cwproject('native');
check(`project build: the game natively with the runtime plugin compiled in (${((Date.now() - t0) / 1000).toFixed(0)} s)`, b.ok, b.out.slice(-1500));
{
  const g = runGame(90, ['plugins', 'turntables', 'call Main/Table reverse']);
  check('game (project build, native): the Turntable from the plugin turns', g.status === 0 && g.state?.['table.found'] === 1 &&
    Math.abs(g.state['table.angle'] - 1.5 * 90 / 60) < 1e-3 && g.state['plugins.loaded'] === 1, JSON.stringify(g.state) + g.out.slice(-600));
  check('game: the plugin is listed by the `plugins` debug command', g.replies[0]?.includes('"name":"turntable"') && g.replies[0].includes('"state":"loaded"'), g.replies[0]);
  check('game: the plugin\'s own debug command answers', /"turntables":\[\{"path":"\/root\/Main\/Table".*"speed":1.5/.test(g.replies[1] || ''), g.replies[1]);
  check('game: the plugin\'s method answers over the channel', /"result"/.test(g.replies[2] || '') && g.replies[2].includes('-1.5'), g.replies[2]);
}
try {
  const w = await runWcnative(join(ROOT, 'build/cart/carts/plugin_demo.wasc'), 90, [[30, 'turntables'], [60, 'plugins']]);
  check('cart on wasmcart-native: the Turntable turns (debug dump)', w.state['table.found'] === 1 && Math.abs(w.state['table.angle'] - 1.5 * 89 / 60) < 0.05 &&
    w.state['plugins.loaded'] === 1, JSON.stringify(w.state));
  check('cart on wasmcart-native: the plugin\'s debug command answers', w.replies.length === 2 && w.replies[0].includes('"path":"/root/Main/Table"') &&
    w.replies[1].includes('"state":"loaded"'), JSON.stringify(w.replies));
} catch (e) { check('cart on wasmcart-native', false, e.message); }

/* ── 2. the native editor, scripted ─────────────────────────────────── */

async function scripted() {
  const s = await launchApp(EDITOR, { args: [PROJ], env: { CW_EDITOR_SETTINGS: settings('scripted') } });
  try {
    const list = await s.ed('plugin.list');
    const p = list.plugins?.find((x) => x.name === 'turntable');
    check('editor: the plugin loads from the project (runtime + editor parts)', p && p.runtime.state === 'loaded' && p.editor.state === 'loaded' && !p.compiled_in,
      JSON.stringify(list));
    const c = p?.contributes || {};
    check('editor: it registered 1 panel, 1 inspector plugin, 1 gizmo, 2 commands, 2 menu items',
      c.panels === 1 && c.inspectors === 1 && c.gizmos === 1 && c.commands === 2 && c.menu_items === 2, JSON.stringify(c));
    const panels = await s.ed('panel.list');
    check('editor: the Turntables panel is docked', panels.panels.some((x) => x.id === 'turntables' && x.slot === 'left_bottom'), JSON.stringify(panels.panels.map((x) => x.id)));
    const open = await s.ed('scene.open main.cwscene');
    check('editor: the scene using the plugin\'s type opens', open.ok, JSON.stringify(open));
    const chk = await s.ed('scene.check path=main.cwscene');
    check('editor: it round-trips (lossless and canonical)', chk.lossless && chk.canonical, JSON.stringify(chk));
    await s.ed('panel.show turntables');
    await s.ed('select paths=Table');
    await sleep(500);
    const insp = await s.ed('ui.rect name=TurntableSpeed');
    check('editor: the inspector plugin renders for the selected Turntable', insp.ok && insp.visible && insp.rect.w > 100, JSON.stringify(insp));
    const tl = await s.ed('ui.rect name=TurntableList');
    check('editor: the panel shows its list', tl.ok && tl.visible, JSON.stringify(tl));
    const gz = await s.ed('turntable.gizmo');
    check('editor: the gizmo draws for the selected Turntable', gz.visible && gz.draws > 0 && gz.handle_px?.length === 2, JSON.stringify(gz));
    await s.screenshot('all', join(OUT, 'editor-plugin.png'));
    await s.ed('select paths=Floor');
    await sleep(500);
    const no = await s.ed('ui.rect name=TurntableSpeed');
    control('inspector plugin for a node that is not a Turntable (Floor)', no.ok && no.visible, JSON.stringify(no));
    const gz2 = await s.ed('turntable.gizmo');
    control('gizmo for a node that is not a Turntable', gz2.visible, JSON.stringify(gz2));
  } finally {
    await s.close();
  }
}
await scripted().catch((e) => check('editor scripted session', false, e.stack));

/* ── 3. pointer only ────────────────────────────────────────────────── */

async function pointer() {
  // where things are (a probe editor with the same settings, window and scene)
  const probe = await launchApp(EDITOR, { args: [PROJ], env: { CW_EDITOR_SETTINGS: settings('pointer-probe') } });
  const at = {};
  try {
    const settle = async () => { const f0 = (await probe.request('status')).frame; await until(async () => (await probe.request('status')).frame > f0 + 3, 20000, 'frames'); };
    const rect = async (n, tab) => {
      const r = await probe.ed(`ui.rect name=${n}`);
      if (!r.ok || (tab ? !r.tab : !r.visible)) throw new Error(`ui.rect ${n}: ${JSON.stringify(r)}`);
      return tab ? r.tab : r.rect;
    };
    await settle();
    at.tab = await rect('Turntables', true);
    await probe.ed('panel.show turntables');
    await settle();
    at.list = await rect('TurntableList');
    await probe.ed('select paths=Table');
    await settle();
    await settle();
    at.slider = await rect('TurntableSlider');
    at.reverse = await rect('TurntableReverse');
    const insp = await rect('Inspector');
    const inside = (p) => p.x >= insp.x && p.x <= insp.x + insp.w && p.y >= insp.y && p.y <= insp.y + insp.h;
    if (!inside({ x: at.slider.x + at.slider.w * (21.5 / 40), y: at.slider.cy }) || !inside({ x: at.reverse.cx, y: at.reverse.cy }))
      throw new Error(`the inspector plugin's widgets are outside the visible Inspector ${JSON.stringify({ insp, s: at.slider, r: at.reverse })}`);
    const g = await probe.ed('turntable.gizmo');
    const v = await probe.ed('view3d.state');
    if (!g.visible) throw new Error('probe: no gizmo ' + JSON.stringify(g));
    const [vx, vy] = v.view_rect;
    at.handle = { x: vx + g.handle_px[0] / g.px, y: vy + g.handle_px[1] / g.px };
    at.center = { x: vx + g.center_px[0] / g.px, y: vy + g.center_px[1] / g.px };
  } finally {
    await probe.close();
  }
  const r = (v) => Math.round(v * 10) / 10;
  const script = (presses) => {
    const steps = [];
    const at2 = (frame, x, y, b) => steps.push(`${frame} 0 0 0 0 0 0 0 m=${r(x)},${r(y)},${presses ? b : 0}`);
    let f = 90;
    const click = (x, y) => { at2(f, x, y, 0); at2(f + 2, x, y, 1); at2(f + 4, x, y, 0); f += 30; };
    click(at.tab.cx, at.tab.cy);
    click(at.list.x + 40, at.list.y + 12); // the first row: selects Table
    f += 20;
    // drag the gizmo handle outward along centre -> handle (radius grows)
    const dx = at.handle.x - at.center.x, dy = at.handle.y - at.center.y;
    at2(f, at.handle.x, at.handle.y, 0);
    at2(f + 2, at.handle.x, at.handle.y, 1);
    for (let i = 1; i <= 8; i++) at2(f + 2 + i * 2, at.handle.x + dx * 0.08 * i, at.handle.y + dy * 0.08 * i, 1);
    at2(f + 22, at.handle.x + dx * 0.64, at.handle.y + dy * 0.64, 0);
    f += 50;
    // drag the slider's grabber (speed 1.5 of -20..20) to three quarters (about 10)
    const gx = at.slider.x + at.slider.w * (21.5 / 40), tx = at.slider.x + at.slider.w * 0.75;
    at2(f, gx, at.slider.cy, 0);
    at2(f + 2, gx, at.slider.cy, 1);
    for (let i = 1; i <= 6; i++) at2(f + 2 + i * 2, gx + (tx - gx) * i / 6, at.slider.cy, 1);
    at2(f + 16, tx, at.slider.cy, 0);
    f += 40;
    click(at.reverse.cx, at.reverse.cy); // speed -10
    return { text: steps.join('\n') + '\n', last: f };
  };
  const run = async (tag, presses) => {
    const sc = script(presses);
    const input = join(OUT, `pointer-${tag}.txt`);
    writeFileSync(input, sc.text);
    const s = await launchApp(EDITOR, { args: [PROJ], env: { CW_EDITOR_SETTINGS: settings(`pointer-${tag}`), CW_INPUT: input } });
    try {
      await until(async () => (await s.request('status')).frame > sc.last + 20, 60000, 'the pointer script');
      const tl = await s.ed('turntable.list');
      const hist = await s.ed('history.list');
      const sel = await s.ed('editor.state');
      await s.screenshot('all', join(OUT, `pointer-${tag}.png`));
      let undo = null;
      if (presses) {
        await s.ed('edit.undo');
        await s.ed('edit.undo');
        undo = await s.ed('edit.undo');
        undo.after = await s.ed('turntable.list');
      }
      return { tl, hist, sel, undo };
    } finally {
      await s.close();
    }
  };
  const a = await run('session', true);
  const t = a.tl.turntables?.[0] || {};
  check('pointer: the Turntables tab + list row selected the Table', t.selected === true, JSON.stringify(a.tl));
  check(`pointer: dragging the gizmo handle grew the radius (1.4 -> ${t.radius})`, t.radius > 1.4 * 1.25, JSON.stringify(a.tl));
  check(`pointer: the inspector plugin's slider (dragged to about 10) then Reverse set the speed (${t.speed})`, t.speed < -5 && t.speed > -13 &&
    Math.abs(t.speed * 10 - Math.round(t.speed * 10)) < 1e-6, JSON.stringify(a.tl));
  const lines = (a.hist.entries || []).map((e) => e.line || e);
  check('pointer: three undo steps (the drag merged into one, slider, Reverse)', lines.length === 3 && lines.every((l) => String(l).startsWith('node.set')) &&
    String(lines[0]).includes('radius'), JSON.stringify(a.hist));
  const back = a.undo?.after?.turntables?.[0] || {};
  check('pointer: undo x3 restores radius 1.4 and speed 1.5', back.radius === 1.4 && back.speed === 1.5, JSON.stringify(a.undo));
  const c = await run('control', false);
  const ct = c.tl.turntables?.[0] || {};
  control('pointer path without presses selects, drags or sets', ct.selected === true || ct.radius !== 1.4 || ct.speed !== 1.5, JSON.stringify(c.tl));
}
await pointer().catch((e) => check('pointer session', false, e.stack));

/* ── 4. hot reload, 5. disable, 6. versions (one editor session) ─────── */

async function lifecycle() {
  const s = await launchApp(EDITOR, { args: [PROJ], env: { CW_EDITOR_SETTINGS: settings('lifecycle') } });
  try {
    await s.ed('scene.open main.cwscene');
    await s.ed('select paths=Table');
    const before = await s.ed('turntable.list');
    const tip0 = await s.ed('node.get Table tip_speed');
    check('hot reload: the loaded editor code is rev 1, tip_speed = speed * radius', before.rev === '1' && tip0.value === '2.1', JSON.stringify([before, tip0]));
    // the control first: an edit that is NOT built never reaches the editor
    editFile(EDSRC, '#define TT_EDITOR_REV "1"', '#define TT_EDITOR_REV "2"');
    editFile(RTSRC, '*out = cw_val_float(t->speed * t->radius);', '*out = cw_val_float(t->speed * t->radius * 2);');
    await sleep(2500);
    const unbuilt = await s.ed('turntable.list');
    control('an edited but unbuilt plugin is reloaded', unbuilt.rev === '2', JSON.stringify(unbuilt));
    const tb = Date.now();
    const bres = cwproject('plugins'); // built OUTSIDE the editor (an agent, a terminal)
    check('hot reload: the edited plugin builds', bres.ok, bres.out.slice(-800));
    const built = Date.now();
    const seen = await until(async () => { const r = await s.ed('turntable.list'); return r.rev === '2' ? r : null; }, 30000, 'the reload').catch(() => null);
    check(`hot reload: the running editor picked the new editor code up (${built - tb} ms build, seen ${Date.now() - built} ms later)`, !!seen);
    const tip1 = await s.ed('node.get Table tip_speed');
    check('hot reload: and the new runtime code (tip_speed now speed * radius * 2)', tip1.value === '4.2', JSON.stringify(tip1));
    const info = await s.ed('plugin.info name=turntable');
    check('hot reload: still one panel, inspector plugin, gizmo; reloads counted', info.plugin.reloads >= 1 && info.plugin.contributes.panels === 1 &&
      info.plugin.contributes.inspectors === 1 && info.plugin.contributes.gizmos === 1, JSON.stringify(info));
    const doc = await s.ed('editor.state');
    check('hot reload: the open scene and the selection survived', JSON.stringify(doc).includes('main.cwscene') && (await s.ed('turntable.list')).turntables[0].selected,
      JSON.stringify(doc).slice(0, 400));
    const insp = await s.ed('ui.rect name=TurntableSpeed');
    check('hot reload: the inspector plugin renders from the new code', insp.ok && insp.visible, JSON.stringify(insp));
    // a failing build keeps the old code
    editFile(EDSRC, '#define TT_EDITOR_REV "2"', '#define TT_EDITOR_REV "3" this does not compile');
    const bad = cwproject('plugins');
    check('hot reload: a broken edit fails to build', !bad.ok, bad.out.slice(-400));
    await sleep(1500);
    const kept = await s.ed('turntable.list');
    check('hot reload: the failed build left the loaded code running (rev 2)', kept.rev === '2', JSON.stringify(kept));
    editFile(EDSRC, '#define TT_EDITOR_REV "3" this does not compile', '#define TT_EDITOR_REV "2"');
    cwproject('plugins');
    await sleep(1500);

    // 5. disable
    const scenePath = join(PROJ, 'main.cwscene');
    await s.ed('node.set path=Table prop=speed value=3');
    const dirty = await s.ed('plugin.disable name=turntable');
    check('disable: refused while a scene using its type has unsaved changes', !dirty.ok && /unsaved changes/.test(dirty.error), JSON.stringify(dirty));
    await s.ed('edit.undo');
    await s.ed('scene.save');
    const hash0 = sha(scenePath);
    const off = await s.ed('plugin.disable name=turntable');
    check('disable: the plugin unloads', off.ok && off.plugin.runtime.state === 'disabled' && off.plugin.editor.state === 'disabled', JSON.stringify(off));
    const c = off.plugin?.contributes || {};
    check('disable: nothing of it stays registered', c.panels === 0 && c.inspectors === 0 && c.gizmos === 0 && c.commands === 0 && c.menu_items === 0, JSON.stringify(c));
    const panels = await s.ed('panel.list');
    check('disable: its panel is gone', !panels.panels.some((x) => x.id === 'turntables'), '');
    const cmd = await s.ed('turntable.list');
    check('disable: its commands are gone', !cmd.ok, JSON.stringify(cmd));
    const reopen = await s.ed('scene.open main.cwscene');
    check('disable: scene.open refuses the scene, naming the plugin', !reopen.ok && /node type Turntable/.test(reopen.error) && /plugin 'turntable'/.test(reopen.error) &&
      /disabled/.test(reopen.error), JSON.stringify(reopen));
    check('disable: the scene file is byte-identical', sha(scenePath) === hash0);
    check('disable: the manifest says enabled = false', /^enabled = false$/m.test(readFileSync(MANIFEST, 'utf8')));
    const nb = cwproject('native');
    const g = runGame(30, ['plugins']);
    check('disable: the game rebuilt without the plugin runs, the type is missing', nb.ok && g.status === 0 && g.state?.['table.found'] === 0 &&
      g.state?.['plugins.disabled'] === 1 && g.state?.['plugins.loaded'] === 0, JSON.stringify(g.state));
    check('disable: the game\'s error names the plugin and how to enable it',
      /unknown node type 'Turntable': plugin 'turntable' provides it, but it is disabled/.test(g.out) && /main.cwscene: node 'Table' \(type Turntable\)/.test(g.out), g.out.slice(-1200));
    const on = await s.ed('plugin.enable name=turntable');
    check('enable: the plugin loads again', on.ok && on.plugin.runtime.state === 'loaded' && on.plugin.editor.state === 'loaded' && on.plugin.contributes.panels === 1, JSON.stringify(on));
    const reopen2 = await s.ed('scene.open main.cwscene');
    check('enable: the scene opens again', reopen2.ok, JSON.stringify(reopen2));

    // 6. versions: a manifest asking for engine API 2.0
    await s.ed('scene.close force=true');
    await s.ed('plugin.disable name=turntable');
    editFile(MANIFEST, 'engine_api = 1.0', 'engine_api = 2.0');
    const v2 = await s.ed('plugin.enable name=turntable');
    control('plugin.enable of a plugin asking for engine API 2.0', v2.ok, JSON.stringify(v2));
    check('version: refused with a message naming the versions and the manifest', /refused/.test(v2.error) && /needs engine API 2\.0/.test(v2.error) &&
      /provides 1\.0/.test(v2.error) && /engine_api = 2\.0/.test(v2.error), v2.error);
    const vb = cwproject('native');
    const vg = runGame(30, ['plugins']);
    check('version: the game build refuses it (not compiled) and says why', vb.ok && /plugin 'turntable' refused: it needs engine API 2\.0/.test(vb.out) &&
      vg.state?.['plugins.refused'] === 1 && vg.state?.['table.found'] === 0 && /plugin 'turntable' refused/.test(vg.out), vb.out.slice(-600) + JSON.stringify(vg.state));
    editFile(MANIFEST, 'engine_api = 2.0', 'engine_api = 1.0');
    // a stale library: the manifest is fine but the code claims API 1.9
    editFile(RTSRC, 'CW_PLUGIN(turntable, .version = "1.0.0",', 'CW_PLUGIN(turntable, .api = CW_API_VERSION(1, 9), .version = "1.0.0",');
    cwproject('plugins');
    const stale = await s.ed('plugin.enable name=turntable');
    control('plugin.enable of code compiled for engine API 1.9', stale.ok, JSON.stringify(stale));
    check('version: the stale library is refused at load with its compiled API named', /compiled for engine API 1\.9/.test(stale.error || ''), stale.error);
    editFile(RTSRC, 'CW_PLUGIN(turntable, .api = CW_API_VERSION(1, 9), .version = "1.0.0",', 'CW_PLUGIN(turntable, .version = "1.0.0",');
    cwproject('plugins');
    await s.ed('plugin.disable name=turntable');
    const ok = await s.ed('plugin.enable name=turntable');
    check('version: rebuilt for API 1.0 it loads again', ok.ok, JSON.stringify(ok));
  } finally {
    await s.close();
  }
}
await lifecycle().catch((e) => check('editor lifecycle session', false, e.stack));

/* ── 7. the project's cart game and cart editor on romdev ──────────── */

try {
  const cg = cwproject('cart');
  const gameCart = join(PROJ, '.cwbuild/cart/carts/plugin_demo.wasc');
  check('project build: the game as a cart (runtime plugin compiled in)', cg.ok && existsSync(gameCart), cg.out.slice(-800));
  const session = `cw-plugins-game-${process.pid}`;
  try {
    await romdev('loadMedia', { platform: 'wasmcart', path: gameCart, deterministicSeed: 1, deterministicStepMs: 1000 / 60 }, session);
    await romdev('frame', { op: 'step', frames: 60 }, session);
    const st = await romdev('wasm', { op: 'debugState' }, session);
    const state = {};
    for (const f of st.fields || []) state[f.name] = f.values ?? f.value;
    const r = await romdev('wasm', { op: 'command', text: 'turntables', maxFrames: 60 }, session);
    check('project cart game on romdev: the Turntable turns, its debug command answers', state['table.found'] === 1 && state['table.angle'] > 1 &&
      JSON.stringify(r).includes('/root/Main/Table'), JSON.stringify(state) + JSON.stringify(r).slice(0, 300));
  } finally { await romdev('host', { op: 'shutdown' }, session).catch(() => {}); }
  const ce = cwproject('editor');
  const edCart = join(PROJ, '.cwbuild/cart/carts/plugin_demo_editor.wasc');
  check('project build: the project editor cart with the plugin compiled in (cwproject build <dir> editor)', ce.ok && existsSync(edCart), ce.out.slice(-800));
  await romdevEditor(edCart, async (s) => {
    const list = await s.ed('plugin.list');
    const p = list.plugins?.find((x) => x.name === 'turntable');
    check('cart editor (romdev): the plugin is compiled in and loaded', p && p.compiled_in && p.runtime.state === 'loaded' && p.editor.state === 'loaded' &&
      p.contributes.panels === 1 && p.contributes.inspectors === 1 && p.contributes.gizmos === 1, JSON.stringify(list));
    const o = await s.ed('scene.check path=main.cwscene');
    check('cart editor: the scene with its type is lossless', o.lossless && o.canonical, JSON.stringify(o));
    await s.ed('select paths=Table');
    await s.ed('editor.state'); // a frame for the inspector to build
    const insp = await s.ed('ui.rect name=TurntableSpeed');
    check('cart editor: the inspector plugin renders', insp.ok && insp.visible, JSON.stringify(insp));
    const off = await s.ed('plugin.disable name=turntable');
    const re = await s.ed('scene.open main.cwscene');
    check('cart editor: disable withdraws it; the scene is refused naming the plugin', off.ok && !re.ok && /plugin 'turntable'/.test(re.error), JSON.stringify([off, re]));
    const on = await s.ed('plugin.enable name=turntable');
    const re2 = await s.ed('scene.open main.cwscene');
    check('cart editor: enable brings it back', on.ok && re2.ok, JSON.stringify([on, re2]));
  });
  // the stock editor cart has no Turntable: the same scene is refused
  const text = readFileSync(join(ROOT, 'samples/plugin_demo/main.cwscene'), 'utf8');
  await romdevEditor(join(ROOT, 'build/cart/carts/editor.wasc'), async (s) => {
    await s.request(`vfs write plugin_scene.cwscene ${Buffer.from(text).toString('base64')}`).catch(() => null);
    const w = await s.ed(`scene.check path=plugin_scene.cwscene`);
    const opened = await s.ed('scene.open plugin_scene.cwscene');
    control('the stock editor cart (no plugin) opening the scene', opened.ok, JSON.stringify([w, opened]));
    check('the stock editor cart names the missing type', /Turntable/.test(opened.error || ''), JSON.stringify(opened));
  });
} catch (e) { check('cart editor on romdev', false, e.stack); }

console.log(failed ? `${failed} check(s) failed` : 'plugins: all checks passed');
process.exit(failed ? 1 : 0);
