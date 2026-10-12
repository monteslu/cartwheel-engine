// M4.4 exit criterion, end to end: a human asks for a change in the agent
// panel; the agent edits C, builds, hot reloads, and the change shows in the
// running game (play-in-editor) in under a few seconds.
//
// Cast: the EDITOR (native, offscreen) on a copy of editor/tests/agent_project
// (one C file, game_module built by its own CMake); the GAME, started by the
// editor's Play (cwhot running the game module, a child of the editor); the
// HUMAN = this script, through the editor's debug channel (`ed` = what the
// panel's Send / Approve / Reject buttons run); the AGENT =
// tests/agent/test-agent.mjs, a separate process that only uses the panel's
// file inbox (.cwagent/requests, replies, the ed/*.cmd bridge).
//
// Rounds:
//   1 "make the box blue": latency from the request to the game reporting
//     the new colour (and its frame showing blue pixels) must be < 5 s; the
//     game keeps its state (ticks run on); the panel shows the agent's
//     commands, the diff, the build and its probes; the human approves.
//   2 "break the build" (must-fail control): the build fails, the error
//     (game.c:line) is in code.status and the panel, the game keeps running
//     the previous code; the human rejects: game.c is restored and rebuilt.
//   3 "spin faster", then the human rejects it: the speed doubles live, then
//     goes back live after the revert + rebuild + reload.
//   control: an agent that edits the C but does not build: "make the box
//     red" must NOT reach the game (the latency check must fail).
// Writes out/editor-agent/report.json and screenshots (game before/after,
// the editor with the agent panel).
//   node tests/editor-agent.mjs
import { spawn, spawnSync } from 'node:child_process';
import { cpSync, rmSync, writeFileSync, readFileSync, mkdirSync, existsSync, renameSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { inflateSync } from 'node:zlib';
import { launch } from './agent/channel.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const B = join(ROOT, 'build/native');
const OUT = join(ROOT, 'out/editor-agent');
const P = join(OUT, 'project');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fail = 0;
const bad = (m) => { fail++; console.log('FAIL ' + m); };
const report = { rounds: {} };

const b = spawnSync('ninja', ['-C', B, 'editor/editor', 'cwhot'], { encoding: 'utf8' });
if (b.status !== 0) { console.log(b.stdout + b.stderr); process.exit(1); }
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
cpSync(join(ROOT, 'editor/tests/agent_project'), P, { recursive: true });
for (const cmd of [['-S', P, '-B', join(P, 'build'), '-G', 'Ninja', `-DCARTWHEEL_ROOT=${ROOT}`], ['--build', join(P, 'build')]]) {
  const r = spawnSync('cmake', cmd, { encoding: 'utf8' });
  if (r.status !== 0) { console.log(r.stdout + r.stderr); process.exit(1); }
}
const settings = join(OUT, 'editor.cfg');
writeFileSync(settings, '');

// ── PNG (8-bit RGB/RGBA, non-interlaced: what the channel writes) ──────
function decodePng(buf) {
  let p = 8, w = 0, h = 0, ct = 0;
  const idat = [];
  while (p < buf.length) {
    const len = buf.readUInt32BE(p), type = buf.toString('ascii', p + 4, p + 8);
    if (type === 'IHDR') { w = buf.readUInt32BE(p + 8); h = buf.readUInt32BE(p + 12); ct = buf[p + 17]; }
    if (type === 'IDAT') idat.push(buf.subarray(p + 8, p + 8 + len));
    p += 12 + len;
  }
  const bpp = ct === 6 ? 4 : 3, raw = inflateSync(Buffer.concat(idat)), px = Buffer.alloc(w * h * bpp);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (w * bpp + 1)];
    for (let x = 0; x < w * bpp; x++) {
      const a = x >= bpp ? px[y * w * bpp + x - bpp] : 0, up = y ? px[(y - 1) * w * bpp + x] : 0, ul = y && x >= bpp ? px[(y - 1) * w * bpp + x - bpp] : 0;
      let v = raw[y * (w * bpp + 1) + 1 + x];
      if (f === 1) v += a; else if (f === 2) v += up; else if (f === 3) v += (a + up) >> 1;
      else if (f === 4) { const q = a + up - ul, pa = Math.abs(q - a), pb = Math.abs(q - up), pc = Math.abs(q - ul); v += pa <= pb && pa <= pc ? a : pb <= pc ? up : ul; }
      px[y * w * bpp + x] = v & 255;
    }
  }
  return { w, h, bpp, px };
}
function countPixels(png, pred) {
  const { w, h, bpp, px } = decodePng(png);
  let n = 0;
  for (let i = 0; i < w * h; i++) if (pred(px[i * bpp], px[i * bpp + 1], px[i * bpp + 2])) n++;
  return n;
}
const BLUE = (r, g, b) => b > 120 && b > r + 60 && b > g + 30;
const ORANGE = (r, g, b) => r > 120 && r > b + 60 && g > b + 20;

// ── the human's side: the editor's debug channel + the file bridge ──────
const app = await launch(join(B, 'editor/editor'), [P], { SDL_VIDEODRIVER: 'offscreen', SDL_AUDIODRIVER: 'dummy', CW_EDITOR_SETTINGS: settings });
const ed = async (line) => {
  const r = await app.ed(line);
  if (!r.ok) throw new Error(`${line}: ${r.error}`);
  return r;
};
let hseq = 0;
async function bridge(...lines) { // the same file bridge the agent uses (game requests go through it)
  const name = `human-${String(++hseq).padStart(5, '0')}`, dir = join(P, '.cwagent/ed');
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, name + '.tmp'), lines.join('\n') + '\n');
  renameSync(join(dir, name + '.tmp'), join(dir, name + '.cmd'));
  for (let i = 0; i < 3000; i++) {
    if (existsSync(join(dir, name + '.reply'))) {
      const t = readFileSync(join(dir, name + '.reply'), 'utf8');
      rmSync(join(dir, name + '.reply'));
      return t.trim().split('\n').map((l) => JSON.parse(l));
    }
    await sleep(5);
  }
  throw new Error('no bridge reply to ' + lines.join(' | '));
}
const box = async () => {
  const [r] = await bridge('game get Level/Box color speed ticks angle');
  if (!r?.values) throw new Error('game probe failed: ' + JSON.stringify(r));
  return { color: r.values.color.value, speed: +r.values.speed.value, ticks: +r.values.ticks.value, angle: +r.values.angle.value, tick: r.tick };
};
const gameShot = async (name) => {
  const path = join(OUT, name);
  await ed(`play.screenshot layers=world path=${path}`);
  return readFileSync(path);
};
const startAgent = (...extra) => {
  const a = spawn('node', [join(ROOT, 'tests/agent/test-agent.mjs'), P, '--once', '--log', join(OUT, 'agent.log'), ...extra], { stdio: 'inherit' });
  return new Promise((r) => a.once('exit', r));
};
async function untilShow(id, pred, ms = 20000) {
  for (const t0 = Date.now(); Date.now() - t0 < ms;) {
    const s = await ed(`agent.show id=${id}`);
    if (pred(s)) return s;
    await sleep(50);
  }
  return ed(`agent.show id=${id}`);
}
async function untilBox(pred, ms) {
  const t0 = Date.now();
  let last;
  while (Date.now() - t0 < ms) {
    last = await box();
    if (pred(last)) return { ok: true, at: Date.now(), box: last };
  }
  return { ok: false, at: Date.now(), box: last };
}

try {
  await ed('settings.set play.headless true');
  const loaded = await ed('code.status');
  if (!/game type/.test(loaded.editor) && !(await ed('code.load_types')).game_types) bad('the editor did not load the game types');
  await ed('scene.open path=main.cwscene');
  const play = await app.ed('play.start');
  if (!play.ok) throw new Error('play.start: ' + play.error);
  const first = await untilBox((x) => x.ticks > 5, 10000);
  if (!first.ok) throw new Error('the game does not run: ' + JSON.stringify(first.box));
  const before = first.box;
  const shot0 = await gameShot('game-before.png');
  const orange0 = countPixels(shot0, ORANGE), blue0 = countPixels(shot0, BLUE);
  if (before.color !== 'color(0.787, 0.214, 0.033, 1)' || orange0 < 500) bad(`the game does not start orange: ${before.color}, ${orange0} orange px`);
  // the box draws with the game module's own program table (programs/, which cwhot registers):
  // a state no table has is a skipped draw (or, with CW_THREE_BUILDER, built at runtime, which
  // hid that cwhot registered none)
  const missing = async (when) => {
    const [f] = await bridge('game fields cw.three.missing_states');
    const v = f?.['cw.three.missing_states'];
    if (typeof v !== 'string') throw new Error('game fields failed: ' + JSON.stringify(f));
    const states = Buffer.from(v, 'hex').toString('latin1').replace(/\0.*$/s, '').split('\n').filter(Boolean);
    if (states.length) bad(`${when}: program states in none of the game's tables (draws skipped): ${states.join(', ')}`);
  };
  await missing('the game at start');

  // ── round 1: make the box blue ────────────────────────────────────────
  const agent1 = startAgent();
  const t0 = Date.now();
  const req = await ed('agent.request text="make the box blue"');
  const seen = await untilBox((x) => x.color === 'color(0.15, 0.35, 1, 1)', 15000);
  const latency = seen.at - t0;
  const shot1 = await gameShot('game-after.png');
  const blue1 = countPixels(shot1, BLUE);
  await agent1;
  const s1 = await untilShow(req.id, (s) => s.request.status === 'done');
  const st1 = await ed('code.status');
  report.rounds.blue = { latency_ms: latency, build_ms: +st1.ms, game_ack: st1.game, blue_px_before: blue0, blue_px_after: blue1,
                         ticks_before: before.ticks, ticks_after: seen.box?.ticks };
  if (!seen.ok) bad(`round 1: the game never turned blue (${JSON.stringify(seen.box)})`);
  else if (latency > 5000) bad(`round 1: request to visible took ${latency} ms (budget 5000)`);
  if (blue1 < 500 || blue1 <= blue0) bad(`round 1: the game's frame has ${blue1} blue pixels (before: ${blue0})`);
  await missing('round 1, the reloaded game');
  if (!(seen.box?.ticks > before.ticks) || Math.abs(seen.box.speed - 0.02) > 1e-6) bad(`round 1: state not kept: ${JSON.stringify(seen.box)} after ${JSON.stringify(before)}`);
  if (s1.request.status !== 'done') bad(`round 1: request status ${s1.request.status}`);
  if (!s1.commands.some((c) => /code\.build reload=true/.test(c))) bad(`round 1: the panel does not show the agent's build command: ${JSON.stringify(s1.commands)}`);
  if (!s1.diff.includes('-#define BOX_COLOR 0.787f, 0.214f, 0.033f') || !s1.diff.includes('+#define BOX_COLOR 0.15f, 0.35f, 1.0f')) bad(`round 1: diff: ${s1.diff}`);
  if (!/ok/.test(s1.build) || !s1.notes.some((n) => /probe: .*color\(0\.15, 0\.35, 1, 1\)/.test(n))) bad(`round 1: build/probes: ${s1.build} ${JSON.stringify(s1.notes)}`);
  await ed(`agent.approve id=${req.id}`);
  const blueSrc = readFileSync(join(P, 'game.c'), 'utf8');

  // ── round 2: a build that fails (must-fail control) ──────────────────
  const agent2 = startAgent();
  const req2 = await ed('agent.request text="break the build please"');
  await agent2;
  const s2 = await untilShow(req2.id, (s) => s.request.status === 'failed');
  const st2 = await ed('code.status');
  const errLine = readFileSync(join(P, 'game.c'), 'utf8').split('\n').findIndex((l) => l.includes('forgot the semicolon')) + 1;
  const err = st2.errors.find((e) => e.kind === 'error');
  const g2a = await box();
  await sleep(300);
  const g2b = await box();
  report.rounds.broken = { state: st2.state, first_error: err && `${err.file}:${err.line}: ${err.message}`, game_ticks: [g2a.ticks, g2b.ticks] };
  if (st2.state !== 'failed' || !err || err.file !== 'game.c' || Math.abs(err.line - errLine) > 1) bad(`round 2: build ${st2.state}, error ${JSON.stringify(err)} (expected game.c:${errLine})`);
  if (s2.request.status !== 'failed' || !/FAILED/.test(s2.build) || !/game\.c:\d+/.test(s2.build)) bad(`round 2: panel: ${s2.request.status}; ${s2.build}`);
  if (g2a.color !== 'color(0.15, 0.35, 1, 1)' || !(g2b.ticks > g2a.ticks)) bad(`round 2: the game should keep running the old (blue) code: ${JSON.stringify([g2a, g2b])}`);
  const open = await ed(`code.open path=game.c line=${err?.line ?? 1}`);
  if (open.line !== err?.line) bad(`round 2: the code view did not open the error line: ${JSON.stringify(open)}`);
  await ed(`agent.reject id=${req2.id}`);
  for (let i = 0; i < 400 && (await ed('code.status')).state === 'running'; i++) await sleep(25);
  if (readFileSync(join(P, 'game.c'), 'utf8') !== blueSrc) bad('round 2: reject did not restore game.c');
  if ((await ed('code.status')).state !== 'ok') bad('round 2: the rebuild after the reject failed');

  // ── round 3: faster, then rejected (revert goes live too) ────────────
  const agent3 = startAgent();
  const t3 = Date.now();
  const req3 = await ed('agent.request text="make it spin faster"');
  const fast = await untilBox((x) => Math.abs(x.speed - 0.04) < 1e-6, 15000);
  await agent3;
  await untilShow(req3.id, (s) => s.request.status === 'done');
  const tr = Date.now();
  await ed(`agent.reject id=${req3.id}`);
  const slow = await untilBox((x) => Math.abs(x.speed - 0.02) < 1e-6, 15000);
  const s3 = await ed(`agent.show id=${req3.id}`);
  report.rounds.faster_then_reject = { faster_ms: fast.ok ? fast.at - t3 : null, revert_visible_ms: slow.at - tr };
  if (!fast.ok) bad(`round 3: the game never sped up: ${JSON.stringify(fast.box)}`);
  if (!slow.ok) bad(`round 3: the reject never reached the game: ${JSON.stringify(slow.box)}`);
  if (s3.request.status !== 'rejected' || readFileSync(join(P, 'game.c'), 'utf8') !== blueSrc) bad(`round 3: status ${s3.request.status} / game.c not restored`);

  // the editor with the agent panel (the human's view)
  await ed(`panel.show id=agent`);
  await sleep(300);
  writeFileSync(join(OUT, 'editor.png'), await app.screenshot('all', 1));

  // ── control: edited but not built: the change must NOT show ───────────
  const agentC = startAgent('--no-build');
  await ed('agent.request text="make the box red"');
  const red = await untilBox((x) => x.color === 'color(1, 0.15, 0.1, 1)', 3000);
  await agentC;
  report.control = { red_reached_game: red.ok };
  if (red.ok) bad('CONTROL PASSED: a change that was never built showed up in the game');
  await ed('play.stop');
} catch (e) {
  bad(String(e.stack || e));
  console.log(app.output().slice(-3000));
}
await app.close();
writeFileSync(join(OUT, 'report.json'), JSON.stringify(report, null, 1));
if (fail) { console.log(`editor-agent: ${fail} failure(s); ${JSON.stringify(report)}`); process.exit(1); }
const r1 = report.rounds.blue;
console.log(`ok   editor-agent (request -> visible in the running game: ${r1.latency_ms} ms (build ${r1.build_ms} ms); a failing build shows ${report.rounds.broken.first_error} and the game keeps the old code; reject reverts live in ${report.rounds.faster_then_reject.revert_visible_ms} ms; control fails)`);
