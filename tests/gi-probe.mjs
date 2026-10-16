// Reflection probes and the light probe grid against known answers
// (cartwheel/gi.h), natively on GL and on WebGPU:
//
//   1. gi_probe_check: a mirror ball at a probe in a box of six glowing walls
//      must show each wall where a straight line from its surface reaches it
//      (the middle the +z wall behind the camera, left -x, right +x, top
//      ceiling, bottom floor), with box projection; a point left of the
//      ball's middle reaches the +z wall with box projection and the -x wall
//      without it (GI_BOX=0), so box projection is seen to act. Control:
//      GI_PROBE=0 (intensity 0) must fail the same checks.
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

function run(app, gpu, frames, tag, env = {}, startCmds = null) {
  const shot = join(OUT, `${tag}.png`), state = join(OUT, `${tag}.json`);
  const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', app, '--frames', String(frames), '--shot', shot, '--state', state];
  if (startCmds) args.push('--start-cmds', startCmds);
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
process.exit(failed ? 1 : 0);
