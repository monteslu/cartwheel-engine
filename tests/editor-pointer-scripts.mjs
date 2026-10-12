// Writes the pointer input scripts of the gates editor-gizmo,
// editor-pointer and editor-pointer-keys from the editor's CURRENT layout:
// the native session apps report where things are (view3d.project,
// view2d.project, scene_tree.rows) at the frame the pointer starts, and the
// drags are aimed at exact world targets (the gizmo's own axis math lands on
// them, snapping makes them exact). Run it when panels added to the dock
// moved the viewports or the tree:
//   node tests/editor-pointer-scripts.mjs        (needs build/native)
import { spawnSync } from 'node:child_process';
import { writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const exe = (s) => join(ROOT, 'build/native/games', `platformer3d_editor_${s}`);

function query(app, frames, cmds) {
  const env = { ...process.env, CW_FRAMES: String(frames), SDL_AUDIODRIVER: 'dummy', CW_DEBUG_CMDS: cmds.map((c) => 'ed64 ' + Buffer.from(c, 'utf8').toString('base64')).join(';') };
  delete env.CW_EDITOR_SETTINGS;
  delete env.CW_PROJECT;
  const r = spawnSync(exe(app), [], { encoding: 'utf8', env, timeout: 120000 });
  if (r.status !== 0) throw new Error(`${app}: ${r.stderr.slice(-1000)}`);
  const out = r.stdout.trim().split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));
  for (const [i, o] of out.entries()) if (!o.ok) throw new Error(`${app}: ${cmds[i]}: ${o.error}`);
  return out;
}
const v3 = (p) => `vec3(${p.join(', ')})`;
const rnd = (p) => [Math.round(p[0]), Math.round(p[1])];

const pts = [];
const add = (list, frame, p, down, keys) =>
  list.push({ frame, mouse: { x: rnd(p)[0], y: rnd(p)[1], ...(down ? { buttons: ['left'] } : {}) }, ...(keys ? { keys } : {}) });
const drag = (list, f0, a, b, steps = 6) => {
  add(list, f0, a, false);
  add(list, f0 + 2, a, true);
  for (let i = 1; i <= steps; i++) add(list, f0 + 2 + 2 * i, [a[0] + (b[0] - a[0]) * i / steps, a[1] + (b[1] - a[1]) * i / steps], true);
  add(list, f0 + 4 + 2 * steps, b, false);
};
const fmt = (a) => '[\n' + a.map((p) => '  ' + JSON.stringify(p)).join(',\n') + '\n]\n';
const write = (name, list) => {
  writeFileSync(join(ROOT, 'tests', `${name}.json`), fmt(list));
  writeFileSync(join(ROOT, 'tests', `${name}-control.json`), fmt(list.map((p) => ({ frame: p.frame, mouse: { x: p.mouse.x, y: p.mouse.y } }))));
};

// editor-gizmo: Coin1 at the origin -> Y handle up 1 -> Z handle back 1 (world (0, 1, -1))
{
  const [st0] = query('gizmo', 15, ['view3d.state']);
  const s0 = st0.handle_size, ya = [0, 0.6 * s0, 0], yb = [0, 0.6 * s0 + 1, 0];
  const [, st1] = query('gizmo', 15, ['node.set path=Coin1 prop=position value="vec3(0, 1, 0)"', 'view3d.state']);
  const s1 = st1.handle_size, za = [0, 1, 0.6 * s1], zb = [0, 1, 0.6 * s1 - 1];
  const [p0] = query('gizmo', 15, [`view3d.project points="${v3(ya)}|${v3(yb)}"`]);
  const [, p1] = query('gizmo', 15, ['node.set path=Coin1 prop=position value="vec3(0, 1, 0)"', `view3d.project points="${v3(za)}|${v3(zb)}"`]);
  const list = [];
  drag(list, 20, p0.points[0], p0.points[1]);
  drag(list, 40, p1.points[0], p1.points[1]);
  write('editor-gizmo', list);
  console.log('editor-gizmo: Y', p0.points.map(rnd), 'Z', p1.points.map(rnd));
}
// editor-pointer(-keys): polygon point 2 of Ground/Shape (world (360, 180)) -> (368, 164); tree: Rope onto Ground; clicks
{
  const [proj, rows] = query('pointer', 30, ['view2d.project points="vec2(360, 180)|vec2(368, 164)"', 'scene_tree.rows']);
  const row = (p) => {
    const r = rows.rows.find((x) => x.path === p);
    if (!r) throw new Error('no tree row ' + p);
    return [r.rect[0] + 56, r.rect[1] + r.rect[3] / 2];
  };
  const list = [];
  drag(list, 40, proj.points[0], proj.points[1]);
  drag(list, 70, row('Rope'), row('Ground'), 8);
  add(list, 110, row('Lamp'), false);
  add(list, 112, row('Lamp'), true);
  add(list, 114, row('Lamp'), false);
  write('editor-pointer', list);
  const k = list.slice();
  add(k, 120, row('Settings'), false); add(k, 122, row('Settings'), true); add(k, 124, row('Settings'), false);
  add(k, 126, row('Cam'), false, ['lctrl']); add(k, 128, row('Cam'), true, ['lctrl']); add(k, 130, row('Cam'), false, ['lctrl']);
  add(k, 132, row('Lamp'), false, ['lshift']); add(k, 134, row('Lamp'), true, ['lshift']); add(k, 136, row('Lamp'), false);
  add(k, 138, row('Lamp'), false);
  write('editor-pointer-keys', k);
  console.log('editor-pointer: point', proj.points.map(rnd), 'rows Rope', rnd(row('Rope')), 'Ground', rnd(row('Ground')));
}
