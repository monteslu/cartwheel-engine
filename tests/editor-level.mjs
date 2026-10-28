// "build level 1 of platformer3d in the editor".
//
// Level 1 of platformer3d (games/platformer3d, scenes/level1.cwscene) is
// rebuilt from an EMPTY scene by driving only editor commands, sent as
// `ed64 <base64>` debug-channel requests (the lines come from
// tools/scene-to-ed.mjs: scene.new, node.add / node.instance, node.set,
// node.group, node.connect, anim.new / track_add / key_add, scene.save):
//   1. natively (platformer3d_editor, CW_PROJECT = a copy of the game's
//      assets WITHOUT level1.cwscene): every reply ok, and the saved file
//      identical to the editor's canonical save of the hand-written level (so
//      the content is the same node for node, value for value); the editor
//      draws with GL itself (GL_ONLY), so it has no dual cart and there is no
//      cart editor leg;
//   2. the saved level PLAYS identically to the hand-written one: the
//      game's full autopilot run (platformer3d_auto, 2150 frames,
//      title -> level 1 -> level 2 -> end) with the editor-built level1 in
//      place of the original gives exactly the same named state (score,
//      coins, deaths, player and camera, props.hash, phys.hash, state.hash,
//      ...) as the original, natively (CW_PROJECT) and on romdev, Chromium
//      and wasmcart-native (the dual cart repacked with the built level and
//      run on GL; on
//      wcnative, which reports no state, the frames at level 1 and at the
//      end must match pixel for pixel);
//   3. controls that must fail: the command script without one coin, and
//      with one crate moved 0.1, each build a level whose run ends in a
//      different state.
//
//   node tests/editor-level.mjs [--hosts native,romdev,chromium,wcnative]
import { spawn, spawnSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync, copyFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { sceneToEd } from '../tools/scene-to-ed.mjs';
import { readImage, diffImages } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'editor-level');
const argv = process.argv.slice(2);
const HOSTS = argv.includes('--hosts') ? argv[argv.indexOf('--hosts') + 1].split(',') : ['native', 'romdev', 'chromium', 'wcnative'];
const P3D = join(ROOT, 'games/platformer3d/assets');
const LEVEL = 'scenes/level1.cwscene';
const SOURCE = process.env.CW_EDLEVEL_SOURCE || join(P3D, LEVEL); // the hand-written level
const editorExe = join(ROOT, 'build/native/games/platformer3d_editor');
const gameApp = 'platformer3d_auto';
const FRAMES = +(process.env.CW_EDLEVEL_FRAMES || 2150);
const SHOT_FRAMES = [600]; // mid level 1 (frame-exact compare on wcnative)
const FIELDS = ['tree.nodes', 'game.state', 'game.level', 'score', 'coins', 'gems', 'lives', 'deaths', 'checkpoint', 'timer', 'level.ticks',
  'player.jumps', 'player.double_jumps', 'player.dashes', 'player.floor', 'anim.state', 'player.pos', 'player.vel', 'cam.pos', 'props.hash',
  'phys.hash', 'state.hash', 'ticks'];
for (const f of [editorExe, join(ROOT, 'build/native/games', gameApp), join(ROOT, 'build/cart-wgpu/carts', gameApp + '.wasc')])
  if (!existsSync(f)) { console.error('missing', f); process.exit(1); }
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });

let fail = 0;
const bad = (m) => { fail++; console.log('FAIL ' + m); };
const ok = (m) => console.log('ok   ' + m);
const b64 = (s) => Buffer.from(s, 'utf8').toString('base64');

// ── the command scripts ───────────────────────────────────────────────
const source = readFileSync(SOURCE, 'utf8');
const lines = sceneToEd(source, { save: LEVEL });
const ctlDrop = sceneToEd(source, { save: LEVEL, drop: ['Coin5'] });
const moved = lines.map((l) => (l === 'node.set path=Crate1 prop=position value="vec3(2.4, 0.45, -4)"' ? l.replace('2.4,', '2.5,') : l));
if (moved.join('\n') === lines.join('\n')) bad('control script: the Crate1 line to move was not found');
for (const l of lines) if (/^(node\.load|scene\.open|scene\.import)/.test(l)) bad('the script must build, not load: ' + l);
console.log(`level 1: ${lines.length} editor commands (${lines.filter((l) => l.startsWith('node.add')).length} node.add, ` +
  `${lines.filter((l) => l.startsWith('node.instance')).length} node.instance, ${lines.filter((l) => l.startsWith('node.set')).length} node.set, ` +
  `${lines.filter((l) => l.startsWith('anim.')).length} anim.*, ${lines.filter((l) => l.startsWith('node.connect')).length} node.connect)`);

// a project directory: the game's assets without the level (the editor must make it)
function project(name) {
  const dir = join(OUT, name);
  cpSync(P3D, dir, { recursive: true });
  rmSync(join(dir, LEVEL));
  return dir;
}

function runEditorNative(dir, requests) {
  const env = { ...process.env, CW_FRAMES: '3', SDL_AUDIODRIVER: 'dummy', CW_PROJECT: dir, CW_DEBUG_CMDS: requests.join(';') };
  delete env.CW_EDITOR_SETTINGS;
  const r = spawnSync(editorExe, [], { encoding: 'utf8', env, timeout: 300000, maxBuffer: 64 << 20 });
  if (r.status !== 0) throw new Error('native editor failed: ' + r.stderr.slice(-2000));
  return r.stdout.trim().split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));
}

function buildNative(name, script) {
  const dir = project(name);
  const replies = runEditorNative(dir, script.map((l) => 'ed64 ' + b64(l)));
  const failed = replies.map((r, i) => [r, script[i]]).filter(([r]) => !r.ok);
  return { dir, replies, failed, text: existsSync(join(dir, LEVEL)) ? readFileSync(join(dir, LEVEL), 'utf8') : null };
}

// 1. native build, the canonical form of the hand-written level, the controls
const built = buildNative('native', lines);
if (built.replies.length !== lines.length) bad(`native: ${built.replies.length} replies for ${lines.length} commands`);
for (const [r, l] of built.failed.slice(0, 5)) bad(`native: \`${l}\` failed: ${r.error}`);
if (!built.text) bad('native: the editor did not save ' + LEVEL);
const refDir = join(OUT, 'canonical');
cpSync(P3D, refDir, { recursive: true });
if (process.env.CW_EDLEVEL_SOURCE) copyFileSync(SOURCE, join(refDir, LEVEL));
const canonReplies = runEditorNative(refDir, [`ed scene.open ${LEVEL}`, 'ed scene.save path=canonical.cwscene']);
const canonical = existsSync(join(refDir, 'canonical.cwscene')) ? readFileSync(join(refDir, 'canonical.cwscene'), 'utf8') : '';
if (!canonReplies[0]?.ok || !canonReplies[0]?.lossless) bad('the hand-written level does not open losslessly: ' + JSON.stringify(canonReplies[0]).slice(0, 300));
if (built.text !== canonical) {
  const a = built.text?.split('\n') || [], b = canonical.split('\n');
  const i = a.findIndex((l, k) => l !== b[k]);
  bad(`the editor-built level differs from the canonical hand-written one at line ${i + 1}:\n  built ${a[i]}\n  canon ${b[i]}`);
} else ok(`native: ${lines.length} commands built ${LEVEL} (${built.text.length} bytes), identical to the hand-written level's canonical form`);
const ctl1 = buildNative('control-drop', ctlDrop), ctl2 = buildNative('control-moved', moved);
if (!ctl1.text || !ctl2.text || ctl1.failed.length || ctl2.failed.length) bad('control builds did not complete');

// 2. play: the editor-built level against the hand-written one
function runCart(args, env = {}) {
  return new Promise((res) => {
    const p = spawn('node', [join(ROOT, 'tools/run-cart.mjs'), ...args], { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...env } });
    let out = '', err = '';
    p.stdout.on('data', (d) => (out += d));
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => res({ code, out, err }));
  });
}
const readState = (f) => (existsSync(f) ? JSON.parse(readFileSync(f, 'utf8')) : null);
function stateDiff(a, b) {
  if (!a || !b) return ['no state'];
  const d = [];
  for (const k of FIELDS) {
    if (JSON.stringify(a[k]) !== JSON.stringify(b[k])) d.push(`${k}: ${JSON.stringify(a[k])} vs ${JSON.stringify(b[k])}`);
  }
  return d;
}
function shotDiff(a, b) {
  if (!existsSync(a) || !existsSync(b)) return 1;
  const r = diffImages(readImage(a), readImage(b));
  return r.comparable ? r.any : 1;
}

// the repacked dual cart: platformer3d_auto with the editor-built level in its assets
function repack(name, text) {
  const dir = join(OUT, 'pack-' + name);
  cpSync(join(ROOT, 'build/cart-wgpu/packs', gameApp, 'assets'), dir, { recursive: true });
  writeFileSync(join(dir, LEVEL), text);
  const out = join(OUT, `${gameApp}-${name}.wasc`);
  const r = spawnSync('node', [join(ROOT, 'deps/wasmcart-wgpu/bin/wasmcart-pack.js'), '--wasm', join(ROOT, 'build/cart-wgpu/games', gameApp + '.wasm'),
    '--name', gameApp, '--assets', dir, '--files', '--version', '0.1.0', '--output', out], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error('wasmcart-pack: ' + r.stderr);
  return out;
}

const runs = [];
const play = (host, label, extra, env) => {
  const shot = join(OUT, `${label}.${host}.png`), state = join(OUT, `${label}.${host}.json`);
  const args = ['--host', host, '--app', gameApp, '--frames', String(FRAMES), '--shot', shot, ...(host === 'wcnative' ? [] : ['--state', state]), ...extra];
  runs.push(runCart(args, env).then((r) => ({ host, label, shot, state, ...r })));
};
const cartBuilt = built.text ? repack('built', built.text) : null;
/* CW_EDLEVEL_SOURCE (development: another source scene) is also the original the build is compared with */
const origDir = process.env.CW_EDLEVEL_SOURCE ? refDir : null;
const cartOrig = process.env.CW_EDLEVEL_SOURCE ? repack('original', source) : null;
const origArgs = cartOrig ? ['--cart', cartOrig] : [];
const origEnv = origDir ? { CW_PROJECT: origDir } : {};
const cartCtl = ctl1.text ? repack('control', ctl1.text) : null;
for (const h of HOSTS) {
  if (h === 'native') {
    play(h, 'original', [], origEnv);
    play(h, 'built', [], { CW_PROJECT: built.dir });
    play(h, 'control-drop', [], { CW_PROJECT: ctl1.dir });
    play(h, 'control-moved', [], { CW_PROJECT: ctl2.dir });
  } else {
    play(h, 'original', origArgs, {});
    if (cartBuilt) play(h, 'built', ['--cart', cartBuilt], {});
    if (h === 'wcnative' && cartCtl) play(h, 'control-drop', ['--cart', cartCtl], {});
  }
}
// the frame in level 1 (pixel compare everywhere, the only check wcnative allows)
const mid = (host, label, extra, env) => {
  const shot = join(OUT, `${label}.${host}.f${SHOT_FRAMES[0]}.png`);
  runs.push(runCart(['--host', host, '--app', gameApp, '--frames', String(SHOT_FRAMES[0]), '--shot', shot, ...extra], env)
    .then((r) => ({ host, label: label + '@' + SHOT_FRAMES[0], shot, ...r })));
};
for (const h of HOSTS) {
  if (h === 'native') { mid(h, 'original', [], origEnv); mid(h, 'built', [], { CW_PROJECT: built.dir }); }
  else if (cartBuilt) { mid(h, 'original', origArgs, {}); mid(h, 'built', ['--cart', cartBuilt], {}); }
}
const results = await Promise.all(runs);
const get = (h, l) => results.find((r) => r.host === h && r.label === l);
for (const r of results) if (r.code) bad(`${r.host} ${r.label}: run failed: ${(r.err || r.out).slice(-600)}`);
for (const h of HOSTS) {
  const o = get(h, 'original'), b = get(h, 'built');
  if (!o || !b) continue;
  if (h !== 'wcnative') {
    const so = readState(o.state), sb = readState(b.state);
    const d = stateDiff(so, sb);
    if (d.length) bad(`${h}: the editor-built level plays differently: ${d.slice(0, 4).join('; ')}`);
    else if (so?.['game.state'] !== 5) bad(`${h}: the run did not reach the end screen (game.state ${so?.['game.state']})`);
    else ok(`${h}: identical end state over ${FRAMES} frames (score ${so.score}, coins ${so.coins}, state.hash ${so['state.hash']}, phys.hash ${so['phys.hash']})`);
  }
  const px = shotDiff(o.shot, b.shot), pm = shotDiff(get(h, 'original@' + SHOT_FRAMES[0])?.shot, get(h, 'built@' + SHOT_FRAMES[0])?.shot);
  if (px !== 0 || pm !== 0) bad(`${h}: frames differ (end ${px}, frame ${SHOT_FRAMES[0]} ${pm})`);
  else ok(`${h}: the frames at ${SHOT_FRAMES[0]} (level 1) and ${FRAMES} (the end) are pixel-identical`);
}
// 2b. play from here (native): the editor plays the built level and the player starts where the view looks
if (HOSTS.includes('native')) {
  const env = { ...process.env, CW_FRAMES: '20', SDL_AUDIODRIVER: 'dummy', CW_PROJECT: built.dir,
    CW_EDITOR_RUN: 'scene.open scenes/level1.cwscene;view3d.camera target=vec3(0, 0, -10) yaw=0 pitch=-30 distance=10',
    CW_DEBUG_CMDS: ['ed view3d.play_here start=true', 'ed play.get node=/root/Level/Player props=position', 'ed play.stop'].join(';') };
  delete env.CW_EDITOR_SETTINGS;
  const r = spawnSync(editorExe, [], { encoding: 'utf8', env, timeout: 180000 });
  const rep = r.stdout.trim().split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));
  if (!rep[0]?.ok || !rep[0]?.player_moved || rep[1]?.value !== 'vec3(0, 0, -10)')
    bad('play from here: ' + JSON.stringify(rep).slice(0, 500));
  else ok('native: play from here ran the game on the built level with the player at the view centre (vec3(0, 0, -10))');
}

// 3. controls: they must NOT play the same
if (HOSTS.includes('native')) {
  const o = readState(get('native', 'original')?.state);
  for (const label of ['control-drop', 'control-moved']) {
    const c = readState(get('native', label)?.state);
    const d = stateDiff(o, c);
    if (!c) bad(`CONTROL ${label}: no state`);
    else if (!d.length) bad(`CONTROL PASSED: ${label} played identically (the comparison cannot see a change)`);
    else ok(`control ${label} differs as it must (${d.slice(0, 2).join('; ')})`);
  }
}
if (HOSTS.includes('wcnative') && get('wcnative', 'control-drop')) {
  const px = shotDiff(get('wcnative', 'original').shot, get('wcnative', 'control-drop').shot);
  if (px === 0) bad('CONTROL PASSED: wcnative control-drop end frame is identical');
  else ok(`control wcnative control-drop end frame differs as it must (${px})`);
}
console.log(fail ? `editor-level: ${fail} failures` : `ok   editor-level (level 1 rebuilt by ${lines.length} editor commands natively, plays identically on ${HOSTS.join(', ')}; controls fail)`);
process.exit(fail ? 1 : 0);
