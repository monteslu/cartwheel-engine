// Reflection probes and the light probe grid against known answers
// (cartwheel/gi.h), natively on GL and on WebGPU:
//
//   1. gi_probe_check: a mirror ball at a probe in a box of six glowing walls
//      must show each wall where a straight line from its surface reaches it
//      (the middle the +z wall behind the camera, left -x, right +x, top
//      ceiling, bottom floor), with box projection; a point left of the
//      ball's middle reaches the +z wall with box projection and the -x wall
//      without it (GI_BOX=0), so box projection is seen to act. Control:
//      GI_PROBE=0 (intensity 0) must fail the same checks. The same with
//      captures spread over frames (GI_EVERY=3), which must also match the
//      whole capture's frame (the control: against the probe off it cannot),
//      and with a 64 cube (GI_RES=64). And with FORWARD lighting and a cascaded sun
//      (GI_CSM=1): the ball still shows the walls and gi.materials > 0; control:
//      CW_TEST_GI_NO_PART=1 (no material gets the gi part) must fail both. And
//      the last probe leaving the tree (GI_LEAVE=1) takes the gi part off every
//      material (gi.materials 0) so look-alike meshes batch again (fewer draws);
//      control: CW_TEST_GI_KEEP_PART=1 (the part stays, as before) must fail.
//   2. gi_room: the light probe grid's bounce light tints the ceiling red by
//      the red wall and green by the green one; with the grid's intensity at
//      0 (the control) the tint must be gone.
//   3. Screen-space reflections: the plinth's warm light shows in the floor
//      in front of it with SSR on, not with it off (the probe, captured
//      from elsewhere, puts it somewhere else). Screen-space bounce light: with
//      the grid off and the Ultra preset (SSGI runs only there), SSGI alone
//      tints the ceiling by the coloured walls;
//      off (the control) it does not.
//   4. GL and WebGPU draw the same gi_room (pixels over 16 levels apart < 0.5 %).
//   5. A probe captured once in the first frame under clustered lighting and a
//      cascaded sun (GI_ONCE=1) is no further apart between GL and WebGPU than
//      one captured every other frame; control: CW_TEST_GI_NO_FLUSH=1 (the
//      capture submitted ahead of the frame's cascades and atlas) is.
//
//   node tests/gi-probe.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { readImage, diffImages } from '../tools/lib/png.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gi');
mkdirSync(OUT, { recursive: true });
let failed = false;
const report = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failed = true; };

function run(app, gpu, frames, tag, env = {}, startCmds = null, extra = []) {
  const shot = join(OUT, `${tag}.png`), state = join(OUT, `${tag}.json`);
  const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', app, '--frames', String(frames), '--shot', shot, '--state', state];
  if (startCmds) args.push('--start-cmds', startCmds);
  args.push(...extra);
  const r = spawnSync(process.execPath, args, { encoding: 'utf8', env: { ...process.env, ...env } });
  if (r.status !== 0) return { err: (r.stderr || r.stdout).trim().split('\n').slice(-3).join(' | ') };
  return { img: readImage(shot), state: JSON.parse(readFileSync(state, 'utf8')) };
}
const px = (img, fx, fy) => {
  const x = Math.round(fx * (img.w - 1)), y = Math.round(fy * (img.h - 1)), i = (y * img.w + x) * 4;
  return [img.rgba[i], img.rgba[i + 1], img.rgba[i + 2]];
};
// the colour a pixel is: each channel high (> 160) or low (< 90), or null
const NAMES = { '110': 'yellow', '100': 'red', '010': 'green', '111': 'white', '011': 'cyan', '001': 'blue', '000': 'black' };
const colour = (p) => (p.every((c) => c > 160 || c < 90) ? NAMES[p.map((c) => (c > 160 ? 1 : 0)).join('')] : null);

// ── 1. the mirror ball ──
const BALL = [
  ['middle', 0.5, 0.5, 'yellow'], ['left', 0.361, 0.5, 'red'], ['right', 0.639, 0.5, 'green'],
  ['top', 0.5, 0.306, 'white'], ['bottom', 0.5, 0.704, 'cyan'], ['left of middle', 0.4167, 0.5, 'yellow'],
];
function ballProblems(img, expect) {
  const out = [];
  for (const [what, fx, fy, want] of expect) {
    const got = colour(px(img, fx, fy));
    if (got !== want) out.push(`${what}: ${got ?? px(img, fx, fy).join(',')} (want ${want})`);
  }
  return out;
}
for (const gpu of ['gl', 'webgpu']) {
  const box = run('gi_probe_check', gpu, 10, `check.${gpu}`);
  if (box.err) { report(false, `gi_probe_check ${gpu}: ${box.err}`); continue; }
  const p = ballProblems(box.img, BALL);
  report(!p.length && box.state['gi.captures'] >= 1 && box.state['gi.bound'] === 1,
    `${gpu}: the mirror ball shows the six walls where they are (box projected)${p.length ? ': ' + p.join('; ') : ''}`);
  const nobox = run('gi_probe_check', gpu, 10, `check.nobox.${gpu}`, { GI_BOX: '0' });
  const q = nobox.err ? [nobox.err] : ballProblems(nobox.img, [['left of middle (no box projection)', 0.4167, 0.5, 'red']]);
  report(!q.length, `${gpu}: without box projection the same point shows the -x wall${q.length ? ': ' + q.join('; ') : ''}`);
  const off = run('gi_probe_check', gpu, 10, `check.off.${gpu}`, { GI_PROBE: '0' });
  const c = off.err ? [off.err] : ballProblems(off.img, BALL);
  report(c.length > 0, `${gpu}: control (probe intensity 0) fails the ball checks (${c.length} of ${BALL.length} wrong)`);
  // captures spread over frames (EVERY_N_FRAMES 3: a few faces a frame, the filter in a frame of its own) make the
  // capture a whole one makes: the same walls, the same picture of this still box; and a 64 cube (below r187's 256)
  // still shows every wall where it is
  const sliced = run('gi_probe_check', gpu, 40, `check.every3.${gpu}`, { GI_EVERY: '3' });
  if (sliced.err || box.err) report(false, `${gpu}: sliced captures: ${sliced.err}`);
  else {
    const s = ballProblems(sliced.img, BALL), d = diffImages(box.img, sliced.img), dOff = off.err ? null : diffImages(off.img, sliced.img);
    report(!s.length && sliced.state['gi.captures'] >= 8 && d.gt16 < 0.002,
      `${gpu}: captures spread over 3 frames (${sliced.state['gi.captures']} in 40 frames) show the walls where they are and match the whole capture (${(d.gt16 * 100).toFixed(3)}% over 16 levels)${s.length ? ': ' + s.join('; ') : ''}`);
    report(dOff && dOff.gt16 >= 0.002, `${gpu}: control: the same comparison against the probe off sees a difference (${dOff ? (dOff.gt16 * 100).toFixed(3) : '?'}%)`);
  }
  // FORWARD lighting with a cascaded sun: the materials carry the forward sun's csm part, and the gi part composes
  // after it (the probe once silently did nothing there: gi.materials 0). Control: CW_TEST_GI_NO_PART=1, no material
  // gets the part (the old control, the csm part keeping gi off, stopped biting when the tagging order changed)
  const fwd = run('gi_probe_check', gpu, 10, `check.csm.${gpu}`, { GI_CSM: '1' });
  const fp = fwd.err ? [fwd.err] : ballProblems(fwd.img, BALL);
  report(!fp.length && fwd.state['gi.materials'] > 0,
    `${gpu}: forward lighting with a cascaded sun: the ball shows the walls, ${fwd.state ? fwd.state['gi.materials'] : '?'} material(s) carry gi${fp.length ? ': ' + fp.join('; ') : ''}`);
  // (its walls draw with xcsm alone, a state the table has no need of: skipped draws allowed in the control)
  const fwdOld = run('gi_probe_check', gpu, 10, `check.csm.old.${gpu}`, { GI_CSM: '1', CW_TEST_GI_NO_PART: '1' }, null, ['--allow-skipped']);
  const fo = fwdOld.err ? [fwdOld.err] : ballProblems(fwdOld.img, BALL);
  report(!fwdOld.err && fo.length > 0 && fwdOld.state['gi.materials'] === 0,
    `${gpu}: control (no material gets the gi part) fails: ${fo.length} of ${BALL.length} wrong, gi.materials ${fwdOld.state ? fwdOld.state['gi.materials'] : '?'}`);
  const small = run('gi_probe_check', gpu, 10, `check.res64.${gpu}`, { GI_RES: '64' });
  const sp = small.err ? [small.err] : ballProblems(small.img, BALL);
  report(!sp.length, `${gpu}: a 64-pixel capture shows the six walls where they are${sp.length ? ': ' + sp.join('; ') : ''}`);
  // the probe leaves the tree (GI_LEAVE=1, freed at frame 6, the last probe and no grid): the gi part comes off every
  // material and the 24 look-alike boxes batch again (fewer draws). Control: CW_TEST_GI_KEEP_PART=1 leaves the part on
  const left = run('gi_probe_check', gpu, 12, `check.leave.${gpu}`, { GI_LEAVE: '1' });
  const kept = run('gi_probe_check', gpu, 12, `check.leave.keep.${gpu}`, { GI_LEAVE: '1', CW_TEST_GI_KEEP_PART: '1' });
  if (left.err || kept.err) report(false, `${gpu}: probe leaving: ${left.err || kept.err}`);
  else {
    const L = left.state, K = kept.state;
    report(L['gi.materials'] === 0 && L['gi.draws'] > 0 && L['gi.draws'] + 5 <= K['gi.draws'],
      `${gpu}: the last probe left: gi.materials ${L['gi.materials']}, ${L['gi.draws']} draws (the part kept: ${K['gi.draws']})`);
    report(!(K['gi.materials'] === 0 && K['gi.draws'] + 5 <= L['gi.draws']),
      `${gpu}: control (the part kept) fails it: gi.materials ${K['gi.materials']}, ${K['gi.draws']} draws`);
  }
}

// ── 2. bounce light ──
// the ceiling near the red wall is redder than near the green one, by the grid
const tint = (img) => {
  const l = px(img, 0.15, 0.04), r = px(img, 0.85, 0.04);
  return (l[0] - l[1]) - (r[0] - r[1]);
};
const rooms = {};
for (const gpu of ['gl', 'webgpu']) {
  const on = run('gi_room', gpu, 60, `room.${gpu}`, {}, 'set Room/Post ssgi false');
  const off = run('gi_room', gpu, 60, `room.nogrid.${gpu}`, {}, 'set Room/Bounce intensity 0;set Room/Post ssgi false');
  if (on.err || off.err) { report(false, `gi_room ${gpu}: ${on.err || off.err}`); continue; }
  rooms[gpu] = on.img;
  const t = tint(on.img), t0 = tint(off.img);
  report(t > 15, `${gpu}: the red and green walls tint the ceiling by their side (red-green difference ${t})`);
  report(!(t0 > 15), `${gpu}: control (grid intensity 0) has no such tint (${t0})`);
}

// ── 3. SSR and SSGI ──
for (const gpu of ['gl', 'webgpu']) {
  const on = run('gi_room', gpu, 60, `room.ssr.${gpu}`, {}, 'set Room/Post ssgi false');
  const off = run('gi_room', gpu, 60, `room.nossr.${gpu}`, {}, 'set Room/Post ssr false;set Room/Post ssgi false');
  if (on.err || off.err) { report(false, `gi_room ssr ${gpu}: ${on.err || off.err}`); continue; }
  const warm = (img) => { const p = px(img, 0.3672, 0.6389); return p[0] - p[2]; };
  report(warm(on.img) > 45, `${gpu}: SSR shows the plinth in the floor in front of it (red - blue ${warm(on.img)})`);
  report(!(warm(off.img) > 45), `${gpu}: control (SSR off) does not (${warm(off.img)})`);
  const gi = run('gi_room', gpu, 60, `room.ssgi.${gpu}`, { GI_GRID: '0', CW_QUALITY: 'ultra' }, 'set Room/Post ssr false');
  const gi0 = run('gi_room', gpu, 60, `room.nossgi.${gpu}`, { GI_GRID: '0', CW_QUALITY: 'ultra' }, 'set Room/Post ssr false;set Room/Post ssgi false');
  if (gi.err || gi0.err) { report(false, `gi_room ssgi ${gpu}: ${gi.err || gi0.err}`); continue; }
  report(tint(gi.img) > 25, `${gpu}: SSGI alone (grid off) tints the ceiling by the walls (${tint(gi.img)})`);
  report(!(tint(gi0.img) > 25), `${gpu}: control (SSGI off too) has no tint (${tint(gi0.img)})`);
}

// ── 4. GL against WebGPU ──
if (rooms.gl && rooms.webgpu) {
  const d = diffImages(rooms.gl, rooms.webgpu);
  report(d.comparable && d.gt16 < 0.005, `gi_room GL vs WebGPU: ${(d.gt16 * 100).toFixed(3)}% of pixels over 16 levels apart`);
}

// ── 5. a probe captured once, in the first frame, under clustered lighting and cascades ──
// GI_ONCE=1: RoomProbe captures once, in the frame the sun's cascades and the clustered shadow atlas are first
// drawn. On WebGPU those are recorded into the frame's encoder while the capture draws with command buffers of its
// own, so the capture must be submitted after them, or it samples maps not drawn yet (a darker room in the
// reflections). GL against WebGPU must then be no further apart than the same scene with the probe captured every
// other frame (GI_ONCE=every). Control: CW_TEST_GI_NO_FLUSH=1, the old ordering, must be further apart.
{
  const cmds = 'set Room/Post ssgi false;set Room/Post ssr false';
  const shot = (gpu, tag, env) => run('gi_room', gpu, 30, `room.once.${tag}.${gpu}`, env, cmds);
  const legs = {};
  for (const [tag, env] of [['every', { GI_ONCE: 'every' }], ['once', { GI_ONCE: '1' }], ['old', { GI_ONCE: '1', CW_TEST_GI_NO_FLUSH: '1' }]]) {
    const g = shot('gl', tag, env), w = shot('webgpu', tag, env);
    if (g.err || w.err) { report(false, `gi_room ${tag}: ${g.err || w.err}`); continue; }
    legs[tag] = diffImages(g.img, w.img).gt16;
  }
  if (legs.every !== undefined && legs.once !== undefined && legs.old !== undefined) {
    const pc = (v) => (v * 100).toFixed(3) + '%';
    report(legs.once <= legs.every, `a ONCE probe under clustered lighting and cascades: GL vs WebGPU ${pc(legs.once)}, within the ${pc(legs.every)} of one captured every other frame`);
    report(legs.old > legs.every, `control (the capture submitted before the frame's cascades and atlas) is further apart: ${pc(legs.old)}`);
  }
}
process.exit(failed ? 1 : 0);
