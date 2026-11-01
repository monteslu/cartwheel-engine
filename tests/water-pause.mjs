// A paused game's water holds still, and pausing or resuming does not make it jump.
//
// A scene that is paused keeps its tree tick, but a host's frame accumulator keeps cycling, so the interpolation alpha
// the pipeline passes on changes every frame. Water drawn at tick - 1 + alpha then moved by up to one tick, back and
// forth, for as long as the pause lasted. Paused water is drawn at the frozen tick itself.
//
// The run renders at a rate that is not the tick rate (47 Hz against 60), so alpha differs on every frame, in samples/water_ripples
// (Gerstner waves, a wake, foam). tree_pause is the same request a game's pause makes. Each frame is its own run.
//   still   paused frames several frames apart are the same picture
//   pause   the step into the pause moves no more of the picture than a step of normal play does (bound: JUMP_MAX times it)
//   resume  the same for the first step out of the pause
//
// CONTROL: the same frames with no pause must differ, or the "still" check could see nothing. A second one moves the
// waves on by half a second as the pause starts, and it must break the "pause" bound, or that check could see nothing.
// On the engine before this fix the "still" check fails (see the CHANGELOG entry for the release).
//
//   node tests/water-pause.mjs [--gpus gl,webgpu] [--exe <binary>]
import { spawnSync } from 'node:child_process';
import { mkdirSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
await takeSuiteLock('water-pause.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'water-pause');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const GPUS = opt('--gpus', 'gl,webgpu').split(',');
const EXE = opt('--exe', null);
const P = 200, U = 240, HZ = 47, LEVELS = 4, JUMP_MAX = 1.5, STILL_MAX = 0.00002;

function shot(gpu, tag, frames, cmds) {
  const file = join(OUT, `${gpu}-${tag}-${frames}.png`);
  const args = ['tools/run-cart.mjs', '--host', 'native', '--gpu', gpu, '--app', 'water_ripples', '--frames', String(frames), '--frame-hz', String(HZ),
    '--shot', file, ...(EXE ? ['--exe', resolve(EXE)] : []), ...(cmds.length ? ['--start-cmds', cmds.join(';')] : [])];
  const r = spawnSync(process.execPath, args, { cwd: ROOT, env: process.env, encoding: 'utf8' });
  if (r.status !== 0 || !existsSync(file)) { console.log(`FAIL water-pause ${gpu} ${tag} frame ${frames}: exit ${r.status}: ${(r.stderr || '').slice(-600)}`); process.exit(1); }
  return readImage(file);
}
// the share of pixels that moved by more than LEVELS on any channel, and the largest move
function moved(a, b) {
  let n = 0, max = 0;
  for (let i = 0; i < a.rgba.length; i += 4) {
    const d = Math.max(Math.abs(a.rgba[i] - b.rgba[i]), Math.abs(a.rgba[i + 1] - b.rgba[i + 1]), Math.abs(a.rgba[i + 2] - b.rgba[i + 2]));
    if (d > LEVELS) n++;
    if (d > max) max = d;
  }
  return { share: n / (a.w * a.h), max };
}
const pct = (v) => `${(v * 100).toFixed(4)}%`;
let fail = 0;
const check = (ok, text) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} water-pause ${text}`); };
for (const gpu of GPUS) {
  const paused = [`@${P + 1} tree_pause 1`, `@${U + 1} tree_pause 0`];
  const f = (n, cmds = paused, tag = 'paused') => shot(gpu, tag, n, cmds);
  const step = moved(f(P - 1, []), f(P, []));
  const last = f(P), first = f(P + 1);
  const a = f(P + 10), b = f(P + 11), c = f(P + 12), d = f(P + 27);
  for (const [n, x, y] of [['1 frame', a, b], ['1 more', b, c], ['17 frames', a, d]]) {
    const m = moved(x, y);
    check(m.share <= STILL_MAX, `${gpu} still, ${n} apart: ${pct(m.share)} moved over ${LEVELS} levels, max ${m.max}`);
  }
  const jump = moved(last, first);
  check(jump.share <= step.share * JUMP_MAX, `${gpu} pause: ${pct(jump.share)} against ${pct(step.share)} for a step of play (bound ${JUMP_MAX} times)`);
  const out = moved(f(U), f(U + 1));
  check(out.share <= step.share * JUMP_MAX, `${gpu} resume: ${pct(out.share)} against ${pct(step.share)} for a step of play`);
  const ctl = moved(f(P + 10, [], 'play'), f(P + 11, [], 'play'));
  check(ctl.share > STILL_MAX * 100, `${gpu} control, no pause: ${pct(ctl.share)} moved, as it must`);
  const hop = [`@${P + 1} tree_pause 1`, `@${P + 1} set Root/Water time_offset 0.5`];
  const ctl2 = moved(last, f(P + 1, hop, 'hop'));
  check(ctl2.share > step.share * JUMP_MAX, `${gpu} control, waves moved on half a second at the pause: ${pct(ctl2.share)} breaks the bound, as it must`);
}
process.exit(fail ? 1 : 0);
