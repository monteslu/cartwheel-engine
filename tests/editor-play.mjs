// Play-in-editor gate (the editor side): "Edit a
// property in a running game; pause, step, inspect", on both game transports
// from the native editor, scripted and by pointer, plus the cart editor's
// clear "not available".
//
//  1. scripted (ed protocol): build/native/tests/editor_play_test drives the
//     native editor over its debug channel while the editor plays
//     samples/play_demo natively (child process) and in romdev (the cart).
//     Each run has a --control twin (no live edit) that MUST fail.
//  2. pointer: the native editor on a frame clock with a CW_INPUT script only
//     (clicks, a typed value, Enter): Play, the Remote tab, the ball's row,
//     the Live tab, filter, edit gravity_scale, Step x10, Keep, Crash,
//     Relaunch. Control rects come from the editor itself (`ed play.widgets`)
//     on the same session cut short, so the script follows the real layout.
//     Control: the same moves without presses MUST fail every effect check.
//  3. cart editor (romdev): Play has no transport there; play.start must fail
//     saying so and the Game panel must show the reason. Control: the native
//     editor shows no such banner.
//   node tests/editor-play.mjs [native|romdev ...]     ROMDEV_PORT (default 7331)
import { spawnSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, rmSync, writeFileSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'editor-play');
mkdirSync(OUT, { recursive: true });
const which = process.argv.slice(2).length ? process.argv.slice(2) : ['native', 'romdev'];
const driver = join(ROOT, 'build/native/tests/editor_play_test');
const editor = join(ROOT, 'build/native/editor/editor');
const editorCart = join(ROOT, 'build/cart/carts/editor.wasc');
const project = join(ROOT, 'samples/play_demo/assets');
for (const p of [driver, editor, join(ROOT, 'build/native/samples/play_demo'), ...(which.includes('romdev') ? [editorCart, join(ROOT, 'build/cart/carts/play_demo.wasc')] : [])])
  if (!existsSync(p)) { console.error(`missing ${p}: run scripts/build.sh`); process.exit(1); }

let fail = 0;
const bad = (m) => { fail++; console.log('FAIL ' + m); };
const good = (m) => console.log('ok   ' + m);
const fresh = (name) => {
  const dir = join(OUT, name);
  rmSync(dir, { recursive: true, force: true });
  cpSync(project, dir, { recursive: true });
  return dir;
};
const headless = { SDL_VIDEODRIVER: 'offscreen', SDL_AUDIODRIVER: 'dummy' };

// ── 1. scripted sessions ─────────────────────────────────────────────────
const scripted = {}, controls = {};
for (const t of which) {
  for (const control of [false, true]) {
    const dir = fresh(`project-${t}${control ? '-control' : ''}`);
    const r = spawnSync(driver, [t, editor, dir, OUT, ...(control ? ['--control'] : [])],
      { encoding: 'utf8', timeout: 300000, env: { ...process.env, ...headless } });
    const out = (r.stdout || '') + (r.stderr || '');
    const last = out.trim().split('\n').reverse().find((l) => l.startsWith('{"transport"'));
    const fails = (out.match(/^FAIL /mg) || []).length;
    if (!control) {
      if (r.status !== 0 || !last) bad(`editor-play scripted ${t} (exit ${r.status})\n${out.split('\n').filter((l) => /FAIL/.test(l)).join('\n') || out.slice(-2000)}`);
      else { scripted[t] = JSON.parse(last); good(`editor-play scripted ${t}: ${scripted[t].checks} checks (play, remote tree, live edit, step, pause/resume, changes, layers, keep + undo, crash, relaunch)`); }
    } else {
      if (last) controls[t] = JSON.parse(last);
      if (r.status === 0 || fails === 0) bad(`CONTROL PASSED: editor-play scripted ${t} --control (no live edit) still saw its effect`);
      else good(`editor-play scripted ${t} control: the unedited run fails ${fails} effect checks, as it must`);
    }
  }
}
if (scripted.native && scripted.romdev && controls.native && controls.romdev) {
  // the unedited ball falls: a value that differs per run of the simulation, so equal means the same game state
  const a = [scripted.native.y_after_edit, controls.native.y_after_edit], b = [scripted.romdev.y_after_edit, controls.romdev.y_after_edit];
  if (a[0] !== b[0] || a[1] !== b[1]) bad(`native and romdev games disagree: y ${a} vs ${b}`);
  else if (a[0] === a[1]) bad(`CONTROL PASSED: the edited and unedited balls end at the same height ${a[0]}`);
  else good(`editor-play: native and romdev games measure the same states (edited y ${a[0]}, unedited y ${a[1]})`);
}

// ── 2. pointer sessions ──────────────────────────────────────────────────
// A step list grows one action at a time; before each action the session so
// far is replayed (deterministic: the game starts paused, every request the
// editor sends finishes inside its frame) and `ed play.widgets` says where
// the next control is.
const KEY = { backspace: 42, return: 40 };
function inputLines(steps, presses) {
  const lines = [];
  for (const s of steps) {
    const parts = [s.frame - 1, 0, 0, 0, 0, 0, 0, 0];
    if (s.keys && presses) parts.push('k=' + s.keys.join(','));
    if (s.mouse) parts.push(`m=${s.mouse.x},${s.mouse.y},${presses ? s.mouse.b || 0 : 0}`);
    if (s.text && presses) parts.push('x=' + Buffer.from(s.text, 'utf8').toString('hex'));
    lines.push(parts.join(' '));
  }
  return lines.join('\n') + '\n';
}

function runEditor(name, transport, steps, frames, cmds, { presses = true, shot = null } = {}) {
  const dir = fresh(`pointer-${transport}-project`);
  const cfg = join(OUT, `pointer-${transport}.cfg`);
  writeFileSync(cfg, `play.start_paused = true\nplay.transport = ${transport}\n`);
  const inp = join(OUT, `pointer-${transport}-${name}.input`);
  writeFileSync(inp, inputLines(steps, presses));
  const env = { ...process.env, ...headless, CW_FRAMES: String(frames), CW_INPUT: inp, CW_EDITOR_SETTINGS: cfg,
    CW_DEBUG_CMDS: cmds.join(';') };
  if (shot) env.CW_SCREENSHOT = shot;
  delete env.CW_PROJECT;
  const r = spawnSync(editor, [dir], { encoding: 'utf8', env, timeout: 180000 });
  if (r.status !== 0) throw new Error(`editor exited ${r.status}: ${(r.stderr || '').slice(-1500)}`);
  return r.stdout.trim().split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));
}

const center = (r) => ({ x: Math.round(r.x + r.w / 2), y: Math.round(r.y + r.h / 2) });
function pointerSession(transport) {
  const steps = [];
  let frame = 4;
  const GAP = 6; // frames after an action before the next (panels poll every play.poll_frames)
  const probe = (cmds = []) => {
    const rep = runEditor('probe', transport, steps, frame + GAP, ['ed play.widgets', ...cmds]);
    frame += GAP;
    return rep;
  };
  const click = (r, what) => {
    if (!r || !(r.w > 0)) throw new Error(`no rect for ${what}: ${JSON.stringify(r)}`);
    const p = center(r);
    steps.push({ frame: frame + 1, mouse: { ...p, b: 0 } }, { frame: frame + 2, mouse: { ...p, b: 1 } }, { frame: frame + 3, mouse: { ...p, b: 0 } });
    frame += 3;
  };
  const clickAt = (p) => {
    steps.push({ frame: frame + 1, mouse: { ...p, b: 0 } }, { frame: frame + 2, mouse: { ...p, b: 1 } }, { frame: frame + 3, mouse: { ...p, b: 0 } });
    frame += 3;
  };
  const key = (k, at) => {
    steps.push({ frame: frame + 1, mouse: { ...at, b: 0 }, keys: [k] }, { frame: frame + 2, mouse: { ...at, b: 0 } });
    frame += 2;
  };
  const type = (text, at) => {
    steps.push({ frame: frame + 1, mouse: { ...at, b: 0 }, text }, { frame: frame + 2, mouse: { ...at, b: 0 } });
    frame += 2;
  };

  let w = probe()[0];
  click(w.toolbar.Play, 'Play');
  w = probe(['ed play.tree'])[0];
  click(w.tabs.remote_tree, 'Remote tab');
  let rep = probe(['ed play.tree']);
  w = rep[0];
  const ball = rep[1].rows.find((r) => r.name === 'Ball' && r.depth === 1);
  if (!ball) throw new Error('no Ball row in the remote tree: ' + JSON.stringify(rep[1]).slice(0, 400));
  click(w.remote_rows[String(ball.uid)], 'Ball row');
  w = probe()[0];
  click(w.tabs.live, 'Live tab');
  w = probe()[0];
  const filt = center(w.live.filter);
  clickAt(filt);
  type('gravity', filt);
  w = probe()[0];
  const g = w.live.rows.gravity_scale;
  if (!g) throw new Error('no gravity_scale row after filtering: ' + JSON.stringify(w.live).slice(0, 400));
  const at = { x: Math.round(g.x + g.w - 6), y: Math.round(g.y + g.h / 2) }; // right end: the caret lands after the text
  clickAt(at);
  key(KEY.backspace, at);
  type('0', at);
  key(KEY.return, at);
  w = probe()[0];
  for (let i = 0; i < 10; i++) {
    click(w.toolbar.Step, 'Step');
    frame += 1;
  }
  rep = probe(['ed play.status', 'ed play.get node=Level/Ball props=position,gravity_scale', 'ed play.get node=Level/Hud props=text']);
  const afterSteps = { status: rep[1], ball: rep[2], hud: rep[3], live: rep[0].live };
  w = rep[0];
  click(w.live.rows.gravity_scale?.keep, 'Keep');
  rep = probe(['ed node.get Ball gravity_scale', 'ed history.list']);
  const afterKeep = { node: rep[1], history: rep[2] };
  w = rep[0];
  click(w.tabs.game, 'Game tab');
  w = probe()[0];
  click(w.game.crash, 'Crash');
  rep = probe(['ed play.status']);
  const afterCrash = { banner: rep[0].game.banner, status: rep[1] };
  w = rep[0];
  click(w.game.relaunch, 'Relaunch');
  frame += GAP;
  const finalCmds = ['ed play.status', 'ed log.tail 60'];
  const shot = join(OUT, `pointer-${transport}.png`);
  const fin = runEditor('final', transport, steps, frame, finalCmds, { shot });
  const ctl = runEditor('control', transport, steps, frame, ['ed play.status', 'ed play.get node=Level/Ball props=position,gravity_scale', 'ed node.get Ball gravity_scale', 'ed history.list'], { presses: false });
  return { steps, frame, afterSteps, afterKeep, afterCrash, final: { status: fin[0], log: fin[1] }, control: ctl };
}

function checkPointer(t, s) {
  const c = [];
  const ok = (cond, msg) => c.push([!!cond, msg]);
  const st = s.afterSteps.status;
  ok(st.state === 'running' && st.paused === true && st.tick === 11, `after Play, a typed edit and 10 Step clicks: running, paused at tick 11 (${st.state} ${st.paused} ${st.tick})`);
  ok(st.transport === t, `transport ${t} (${st.transport})`);
  ok(/vec3\(-1\.5, 4, 0\)/.test(s.afterSteps.ball.values?.position?.value || ''), `the ball did not fall: gravity_scale 0 typed into the Live panel took effect in the game (${s.afterSteps.ball.values?.position?.value})`);
  ok(s.afterSteps.ball.values?.gravity_scale?.value === '0', `the game's gravity_scale is 0 (${s.afterSteps.ball.values?.gravity_scale?.value})`);
  ok(/ball height 4\.00 m\s+tick 11/.test(s.afterSteps.hud.value || ''), `the game's HUD logic shows it (${s.afterSteps.hud.value})`);
  ok(s.afterSteps.live.tick === 11 && s.afterSteps.live.rows?.gravity_scale?.value === '0', `the Live panel inspects tick 11 with the live value (${s.afterSteps.live.tick} ${s.afterSteps.live.rows?.gravity_scale?.value})`);
  ok(s.afterKeep.node.value === '0', `Keep put the live value into the edited scene (${JSON.stringify(s.afterKeep.node)})`);
  ok((s.afterKeep.history.entries || []).some((e) => e.startsWith('play.apply path=Ball prop=gravity_scale value=0')), `as one undoable history entry (${JSON.stringify(s.afterKeep.history.entries)})`);
  ok(/CRASHED/.test(s.afterCrash.banner) && /SIGSEGV|unreachable/.test(s.afterCrash.banner), `the Crash button's crash is reported in the Game panel (${s.afterCrash.banner.slice(0, 120)})`);
  ok(s.afterCrash.status.state === 'crashed', `state crashed (${s.afterCrash.status.state})`);
  const fs = s.final.status;
  ok(fs.state === 'running' && fs.launches === 2 && fs.tick === 1, `Relaunch: a fresh game, launch 2, tick 1 (${fs.state} ${fs.launches} ${fs.tick})`);
  ok((s.final.log.lines || []).some((l) => l.kind === 'error' && /crashed/.test(l.text)), 'the Output panel has the crash report');
  return c;
}

for (const t of which) {
  let s;
  try { s = pointerSession(t); } catch (e) { bad(`editor-play pointer ${t}: ${e.message}`); continue; }
  const checks = checkPointer(t, s);
  const failed = checks.filter(([okk]) => !okk);
  if (failed.length) bad(`editor-play pointer ${t}:\n  ${failed.map(([, m]) => m).join('\n  ')}`);
  else good(`editor-play pointer ${t}: ${checks.length} checks over ${s.frame} frames (${s.steps.length} input entries)`);
  // control: the same pointer path without a single press must fail every effect check
  const [cst, cget, cnode, chist] = s.control;
  const ctlEffects = [cst.state === 'running', /vec3\(-1\.5, 4, 0\)/.test(cget?.values?.position?.value || ''), cnode?.value === '0',
    (chist?.entries || []).length > 0];
  if (ctlEffects.some(Boolean)) bad(`CONTROL PASSED: editor-play pointer ${t} without presses still shows an effect: ${JSON.stringify(ctlEffects)} ${JSON.stringify(cst).slice(0, 200)}`);
  else good(`editor-play pointer ${t} control: the same moves without presses do nothing (no game, no edit, no history)`);
}

// ── 3. the cart editor has no transport, and says so ─────────────────────
if (which.includes('romdev')) {
  const port = process.env.ROMDEV_PORT || 7331;
  const S = `cw-editor-play-${process.pid}`;
  const call = async (tool, a) => {
    const res = await fetch(`http://127.0.0.1:${port}/tool/${tool}`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'x-romdev-session': S }, body: JSON.stringify(a) });
    const txt = await res.text();
    try { return JSON.parse(txt); } catch { return { raw: txt }; }
  };
  const cmd = async (text) => (await call('wasm', { op: 'command', text })).reply;
  await call('loadMedia', { platform: 'wasmcart', path: editorCart, deterministicSeed: 1 });
  await call('frame', { op: 'step', frames: 5 });
  const start = await cmd('ed play.start');
  const status = await cmd('ed play.status');
  await cmd('ed panel.show id=game');
  await call('frame', { op: 'step', frames: 3 });
  const widgets = await cmd('ed play.widgets');
  await call('frame', { op: 'screenshot', path: join(OUT, 'cart-editor.png') });
  await call('host', { op: 'shutdown' });
  const why = /not available in the cart editor/;
  if (start?.ok !== false || !why.test(start?.error || '')) bad('cart editor: play.start must fail saying Play is not available there: ' + JSON.stringify(start));
  else good('cart editor: play.start refuses and says why');
  if (status?.available !== false || !why.test(status?.reason || '')) bad('cart editor: play.status must say unavailable: ' + JSON.stringify(status));
  if (!why.test(widgets?.game?.banner || '')) bad('cart editor: the Game panel must show the reason: ' + JSON.stringify(widgets?.game));
  else good('cart editor: the Game panel shows the reason (no transport; a romdev peer bridge is undecided)');
  // control: the native editor shows no such banner and Play is available there
  const n = runEditor('banner-control', 'native', [], 5, ['ed play.status', 'ed play.widgets']);
  if (n[0].available !== true || why.test(n[1].game.banner)) bad('CONTROL PASSED: the native editor also reports Play unavailable');
  else good('cart editor control: the native editor has Play and no banner');
}

console.log(fail ? `editor-play: ${fail} failures` : 'ok   editor-play');
process.exit(fail ? 1 : 0);
