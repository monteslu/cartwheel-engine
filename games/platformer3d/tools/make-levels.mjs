// Writes platformer3d's scenes (assets/scenes/{title,level1,level2,end}.cwscene)
// from the layouts below: the level editor until cartwheel has one. The
// output is ordinary, readable .cwscene text (the engine loads only that);
// rerun after editing a layout:
//
//   node games/platformer3d/tools/make-levels.mjs
//
// Conventions: Y up, levels run toward -Z, the camera looks from +Z. A
// block is given by its TOP surface: x, top, z (centre) and w x h x d.
import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const D = join(dirname(fileURLToPath(import.meta.url)), '..', 'assets', 'scenes');
const f = (x) => {
  const s = (Math.round(x * 10000) / 10000).toString();
  return s === '-0' ? '0' : s;
};
const vec = (a) => `vec3(${a.map(f).join(', ')})`;
const eased = (a) => `${vec(a)} in=vec3(0, 0, 0) out=vec3(0, 0, 0)`;
// palettes below are picked as screen (sRGB) colours; every colour the scenes
// hold is linear (three.js r187's workflow): materials and lights through the
// game's own 2.2 curve, the background and fog through sRGB's (they were
// screen colours under r128 and keep their look that way)
const lin = (v) => Math.pow(v, 2.2);
const col = (c) => `color(${[...c.slice(0, 3).map(lin), c.length === 4 ? c[3] : 1].map(f).join(', ')})`;
const srgbToLinear = (v) => (v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4));
const scol = (c) => `color(${[...c.slice(0, 3).map(srgbToLinear), 1].map(f).join(', ')})`;
// light energies below are the r128-era values the levels were tuned with; the
// scenes hold physical units (r187's): directional and ambient x PI, an omni
// light matched at 2 m (PI x d^2 x r128's (1 - d / range)^2 window there)
const dirE = (e) => f(e * Math.PI);
const omniE = (e, range, d = 2) => f(e * Math.PI * d * d * Math.pow(Math.max(0, 1 - d / range), 2));

const STYLES = {
  grass: { body: [0.36, 0.21, 0.1], top: [0.17, 0.42, 0.1], lip: 0.22 },
  stone: { body: [0.4, 0.4, 0.45], top: [0.62, 0.63, 0.66], lip: 0.18 },
  metal: { body: [0.2, 0.22, 0.3], top: [0.42, 0.47, 0.58], lip: 0.16, trim: [1, 0.55, 0.1] },
  wood: { body: [0.5, 0.31, 0.14], top: [0.68, 0.46, 0.22], lip: 0.1, trim: [1, 0.8, 0.2] },
  sand: { body: [0.55, 0.42, 0.28], top: [0.93, 0.82, 0.55], lip: 0.2 },
};

class Scene {
  constructor(root) {
    this.out = ['[scene format=1]'];
    this.ext = [];
    this.nodes = [];
    this.anims = [];
    this.root = root;
  }
  extScene(path) {
    let i = this.ext.indexOf(path);
    if (i < 0) { this.ext.push(path); i = this.ext.length - 1; }
    return i + 1;
  }
  node(name, parent, type, props = {}, extra = '') {
    const head = parent === null ? `[node name="${name}" type="${type}"${extra}]`
      : type ? `[node name="${name}" parent="${parent}" type="${type}"${extra}]` : `[node name="${name}" parent="${parent}"${extra}]`;
    const lines = [head];
    for (const [k, v] of Object.entries(props)) if (v !== undefined) lines.push(`${k} = ${v}`);
    this.nodes.push(lines.join('\n'));
    return name;
  }
  instance(name, parent, path, props = {}, groups) {
    const id = this.extScene(path);
    const lines = [`[node name="${name}" parent="${parent}" instance=${id}${groups ? ` groups="${groups}"` : ''}]`];
    for (const [k, v] of Object.entries(props)) if (v !== undefined) lines.push(`${k} = ${v}`);
    this.nodes.push(lines.join('\n'));
  }
  anim(name, player, length, loop, tracks) {
    const lines = [`[animation name="${name}" player="${player}" length=${f(length)}${loop ? ` loop=${loop}` : ''}]`];
    for (const t of tracks) {
      lines.push(`[track type=value path="${t.path}"${t.interp ? ` interp=${t.interp}` : ''}]`);
      for (const [time, v] of t.keys) lines.push(`${f(time)} = ${v}`);
    }
    this.anims.push(lines.join('\n'));
  }
  text() {
    return [this.out[0], ...this.ext.map((p, i) => `[ext_scene id=${i + 1} path="${p}"]`), '', this.nodes.join('\n\n'), '',
      this.anims.join('\n\n'), ''].join('\n').replace(/\n{3,}/g, '\n\n');
  }
}

// a static block: body + a slightly wider top slab (grass, paving, plating)
function block(S, name, [x, top, z], [w, h, d], style = 'grass', parent = '.') {
  const st = STYLES[style];
  S.node(name, parent, 'StaticBody3D', { collision_layer_name: '"world"', position: vec([x, top - h / 2, z]) });
  const p = parent === '.' ? name : `${parent}/${name}`;
  S.node('Shape', p, 'CollisionShape3D', { shape: 'BOX', size: vec([w, h, d]) });
  S.node('Body', p, 'MeshInstance3D', { size: vec([w, h, d]), color: col(st.body), roughness: 0.9, cast_shadow: true, receive_shadow: true });
  S.node('Top', p, 'MeshInstance3D', { position: vec([0, h / 2 - st.lip / 2 + 0.02, 0]), size: vec([w + 0.12, st.lip, d + 0.12]),
    color: col(st.top), roughness: 0.85, receive_shadow: true, cast_shadow: true });
  if (st.trim) S.node('Trim', p, 'MeshInstance3D', { position: vec([0, h / 2 - st.lip - 0.05, 0]), size: vec([w + 0.06, 0.08, d + 0.06]),
    color: col(st.trim), emissive: col(st.trim.map((c) => c * 0.35)), roughness: 0.4 });
}

// a kinematic platform moved by an AnimationPlayer of its own
function mover(S, name, [x, top, z], [w, h, d], style, anim, extraGroups = '') {
  const st = STYLES[style];
  S.node(name, '.', 'KinematicBody3D', { collision_layer_name: '"world"', position: vec([x, top - h / 2, z]) },
    ` groups="machines${extraGroups}"`);
  S.node('Shape', name, 'CollisionShape3D', anim.shape || { shape: 'BOX', size: vec([w, h, d]) });
  if (anim.disc) {
    S.node('Mesh', name, 'MeshInstance3D', { primitive: 'CYLINDER', size: vec([w, h, 0]), color: col(st.body), roughness: 0.6, cast_shadow: true, receive_shadow: true });
    S.node('Top', name, 'MeshInstance3D', { primitive: 'CYLINDER', position: vec([0, h / 2 + 0.01, 0]), size: vec([w - 0.4, 0.04, 0]), color: col(st.top), roughness: 0.6, receive_shadow: true });
    S.node('Stripe', name, 'MeshInstance3D', { position: vec([0, h / 2 + 0.035, 0]), size: vec([w - 0.5, 0.03, 0.4]), color: col(st.trim), emissive: col(st.trim.map((c) => c * 0.4)) });
  } else {
    S.node('Body', name, 'MeshInstance3D', { size: vec([w, h, d]), color: col(st.body), roughness: 0.7, cast_shadow: true, receive_shadow: true });
    S.node('Top', name, 'MeshInstance3D', { position: vec([0, h / 2 - 0.04, 0]), size: vec([w + 0.08, 0.1, d + 0.08]), color: col(st.top), roughness: 0.7, receive_shadow: true });
    S.node('Edge', name, 'MeshInstance3D', { position: vec([0, -h / 2 + 0.05, 0]), size: vec([w + 0.1, 0.1, d + 0.1]), color: col(st.trim), emissive: col(st.trim.map((c) => c * 0.35)) });
  }
  S.node('Anim', name, 'AnimationPlayer', { root_node: '".."', autoplay: '"move"' });
  S.anim('move', `${name}/Anim`, anim.length, anim.loop, anim.tracks);
}

function tree(S, name, [x, y, z], scale = 1) {
  S.node(name, '.', 'Node3D', { position: vec([x, y, z]), scale: scale === 1 ? undefined : vec([scale, scale, scale]) });
  S.node('Trunk', name, 'MeshInstance3D', { primitive: 'CYLINDER', position: vec([0, 0.7, 0]), size: vec([0.35, 1.4, 0]), color: col([0.4, 0.24, 0.1]), cast_shadow: true });
  S.node('Leaves', name, 'MeshInstance3D', { primitive: 'SPHERE', position: vec([0, 2.0, 0]), size: vec([1.9, 1.9, 1.9]), color: col([0.16, 0.5, 0.16]), roughness: 0.9, cast_shadow: true, receive_shadow: true });
  S.node('Leaves2', name, 'MeshInstance3D', { primitive: 'SPHERE', position: vec([0.35, 2.7, 0.2]), size: vec([1.2, 1.2, 1.2]), color: col([0.24, 0.62, 0.2]), roughness: 0.9, cast_shadow: true });
}

function cloud(S, name, [x, y, z], s = 1) {
  S.node(name, '.', 'Node3D', { position: vec([x, y, z]), scale: vec([s, s, s]) });
  const puffs = [[0, 0, 0, 3.2], [2.2, -0.3, 0.4, 2.4], [-2.1, -0.4, -0.2, 2.2], [0.9, 0.9, -0.3, 2.2]];
  puffs.forEach(([px, py, pz, d], i) => S.node(`Puff${i}`, name, 'MeshInstance3D', { primitive: 'SPHERE', position: vec([px, py, pz]),
    scale: vec([1, 0.62, 1]), size: vec([d, d, d]), color: col([1, 1, 1]), emissive: col([0.35, 0.37, 0.42]), roughness: 1 }));
}

// a lamp post with a real light (level 2's dusk)
function lamp(S, name, [x, y, z]) {
  S.node(name, '.', 'Node3D', { position: vec([x, y, z]) });
  S.node('Pole', name, 'MeshInstance3D', { primitive: 'CYLINDER', position: vec([0, 1.1, 0]), size: vec([0.12, 2.2, 0]), color: col([0.15, 0.16, 0.2]), metalness: 0.6, roughness: 0.4, cast_shadow: true });
  S.node('Bulb', name, 'MeshInstance3D', { primitive: 'SPHERE', position: vec([0, 2.3, 0]), size: vec([0.36, 0.36, 0.36]), color: col([1, 0.85, 0.5]), emissive: col([1, 0.75, 0.35]) });
  S.node('Light', name, 'OmniLight3D', { position: vec([0, 2.3, 0]), color: col([1, 0.78, 0.45]), energy: omniE(1.4, 7), range: 7 });
}

// a distant floating island silhouette (decor, no collision)
function farIsland(S, name, [x, y, z], s, top = [0.3, 0.58, 0.25], body = [0.38, 0.27, 0.2]) {
  S.node(name, '.', 'Node3D', { position: vec([x, y, z]), scale: vec([s, s, s]) });
  S.node('Rock', name, 'MeshInstance3D', { primitive: 'SPHERE', position: vec([0, -1.2, 0]), scale: vec([1, 0.8, 1]), size: vec([4, 4, 4]), color: col(body), roughness: 1 });
  S.node('Top', name, 'MeshInstance3D', { primitive: 'CYLINDER', position: vec([0, 0, 0]), size: vec([4.2, 0.5, 0]), color: col(top), roughness: 1 });
}

const coin = (S, name, p) => S.instance(name, '.', 'scenes/coin.cwscene', { position: vec(p) });
const gem = (S, name, p) => S.instance(name, '.', 'scenes/gem.cwscene', { position: vec(p) });

function info(S, props) {
  S.node('Level', null, 'LevelInfo', props);
}

function environment(S, { bg, fog, sky, ground, ambient = 0.25, hemi = 0.6, fogNear = 35, fogFar = 110, exposure = 1.1 }) {
  S.node('Env', '.', 'WorldEnvironment', { background: scol(bg), ambient_energy: dirE(ambient), hemi_energy: dirE(hemi), sky_color: col(sky),
    ground_color: col(ground), fog_mode: 'LINEAR', fog_color: scol(fog), fog_near: f(fogNear), fog_far: f(fogFar) });
  // r128 look with the ACES filmic curve (the render pipeline's settings)
  S.node('Post', '.', 'PostProcess', { tonemap: 'ACES_FILMIC', exposure: f(exposure) });
}

// the sun rides with the player (a child of the Player instance) so its
// shadow map covers the play area at full resolution wherever the hero is
function playerWithSun(S, pos, sunColor = [1, 0.96, 0.88], energy = 1.15) {
  S.instance('Player', '.', 'scenes/player.cwscene', { position: vec(pos) });
  S.node('Sun', 'Player', 'DirectionalLight3D', { position: vec([6, 16, 8]), rotation: vec([-1.17, 0.33, 0]), color: col(sunColor),
    energy: dirE(energy), shadow: true, shadow_size: 2048, shadow_extent: 16, shadow_far: 60, shadow_bias: f(-0.0006) });
  S.node('Camera', '.', 'FollowCamera', { target: '"../Player"', fov: 55, near: f(0.2), far: 300 });
}

// ── level 1: Meadow Run ─────────────────────────────────────────────
function level1() {
  const S = new Scene();
  info(S, { title: '"Meadow Run"', next_scene: '"scenes/level2.cwscene"', music: '"audio/music_level.opus"', kill_y: -12, index: 1 });
  environment(S, { bg: [0.42, 0.66, 0.95], fog: [0.62, 0.78, 0.96], sky: [0.65, 0.8, 1], ground: [0.35, 0.3, 0.2] });
  playerWithSun(S, [0, 0, 2]);

  // start island
  block(S, 'Start', [0, 0, -3], [8, 3, 14], 'grass');
  coin(S, 'Coin1', [0, 1, -1]);
  coin(S, 'Coin2', [0, 1, -3]);
  coin(S, 'Coin3', [0, 1, -5]);
  S.instance('Crate1', '.', 'scenes/crate.cwscene', { position: vec([2.4, 0.45, -4]) });
  S.instance('Crate2', '.', 'scenes/crate.cwscene', { position: vec([2.4, 0.45, -5]) });
  S.instance('Crate3', '.', 'scenes/crate.cwscene', { position: vec([2.4, 1.35, -4.5]), rotation: vec([0, 0.3, 0]) });
  S.instance('Crate4', '.', 'scenes/crate.cwscene', { position: vec([0.85, 0.45, -7.6]), rotation: vec([0, 0.2, 0]) });
  tree(S, 'Tree1', [-3, 0, 1]);
  tree(S, 'Tree2', [3, 0, -8.6], 0.85);
  tree(S, 'Tree3', [-3.1, 0, -7], 1.1);

  // the first gap
  coin(S, 'Coin4', [0, 1.9, -11.5]);
  block(S, 'Meadow', [0, 0, -17.5], [7, 3, 9], 'grass');
  block(S, 'Step', [0, 1.0, -17.75], [3, 1.0, 2.5], 'stone');
  gem(S, 'Gem1', [0, 2.1, -17.75]);
  S.instance('Lava1', '.', 'scenes/lava.cwscene', { position: vec([2.6, 0.02, -20.4]), scale: vec([0.45, 1, 0.45]) });
  tree(S, 'Tree4', [-2.7, 0, -14.5], 0.9);
  coin(S, 'Coin5', [0, 1, -20.8]);

  // the moving platform across the long gap
  mover(S, 'Ferry', [0, 0, -24], [3, 0.4, 3], 'wood', {
    length: 2.6, loop: 'PING_PONG',
    tracks: [{ path: ':position', interp: 'HERMITE', keys: [[0, eased([0, -0.2, -24])], [0.3, eased([0, -0.2, -24])], [2.3, eased([0, -0.2, -30])], [2.6, eased([0, -0.2, -30])]] }],
  });
  coin(S, 'Coin6', [0, 1.2, -27]);

  // the far meadow: checkpoint, props, the saw, stairs to the goal
  block(S, 'Far', [0, 0, -39], [8, 3, 14], 'grass');
  S.instance('Check1', '.', 'scenes/checkpoint.cwscene', { position: vec([-1.4, 0, -34]) });
  coin(S, 'Coin7', [0, 1, -35.5]);
  coin(S, 'Coin8', [0, 1, -37.5]);
  for (let i = 0; i < 3; i++) S.instance(`Ball${i + 1}`, '.', 'scenes/ball.cwscene', { position: vec([1.5 + i * 0.95, 0.45, -37]) });
  S.instance('Saw1', '.', 'scenes/saw.cwscene', { position: vec([-3, 0.75, -40.5]) });
  S.node('SawAnim', '.', 'AnimationPlayer', { autoplay: '"sweep"' });
  S.anim('sweep', 'SawAnim', 3, 'PING_PONG', [{ path: 'Saw1:position', interp: 'HERMITE', keys: [[0, eased([-3, 0.75, -40.5])], [3, eased([3, 0.75, -40.5])]] }]);
  block(S, 'Stair1', [0, 1.0, -43.75], [4, 1.0, 1.5], 'stone');
  block(S, 'Stair2', [0, 2.0, -45.25], [4, 2.0, 1.5], 'stone');
  block(S, 'Summit', [0, 3.0, -50], [6, 4, 8], 'grass');
  coin(S, 'Coin9', [0, 2.0, -43.75]);
  coin(S, 'Coin10', [0, 3.0, -45.25]);
  S.instance('Goal', '.', 'scenes/goal.cwscene', { position: vec([0, 3, -51]) });
  tree(S, 'Tree5', [3.2, 0, -33.5]);
  tree(S, 'Tree6', [-2.4, 3, -53]);

  // sky decor
  cloud(S, 'Cloud1', [-14, 4, -20], 1.4);
  cloud(S, 'Cloud2', [16, 7, -38], 1.8);
  cloud(S, 'Cloud3', [-10, -7, -45], 2.2);
  cloud(S, 'Cloud4', [10, -5, -8], 1.6);
  cloud(S, 'Cloud5', [-6, 12, -70], 2.5);
  farIsland(S, 'Isle1', [-24, -4, -30], 2.2);
  farIsland(S, 'Isle2', [26, 0, -55], 2.8);
  farIsland(S, 'Isle3', [-18, 3, -80], 3.2);
  return S.text();
}

// ── level 2: Sky Works ──────────────────────────────────────────────
function level2() {
  const S = new Scene();
  info(S, { title: '"Sky Works"', next_scene: '"scenes/end.cwscene"', music: '"audio/music_level.opus"', kill_y: -12, index: 2 });
  environment(S, { bg: [0.99, 0.78, 0.6], fog: [0.99, 0.8, 0.64], sky: [1, 0.86, 0.72], ground: [0.32, 0.24, 0.32], ambient: 0.3, hemi: 0.65, fogNear: 30, fogFar: 100 });
  playerWithSun(S, [0, 0, 2], [1, 0.82, 0.62], 1.25);

  block(S, 'Start', [0, 0, -0.5], [7, 3, 9], 'metal');
  coin(S, 'Coin1', [0, 1, -2]);
  coin(S, 'Coin2', [0, 1, -4]);

  // the spinning disc
  mover(S, 'Disc', [0, 0, -10.5], [7, 0.5, 7], 'metal', {
    disc: true, shape: { shape: 'CONVEX', mesh: '"../Mesh"' }, length: 8, loop: 'LOOP',
    tracks: [{ path: ':rotation', keys: [[0, vec([0, 0, 0])], [8, vec([0, 6.283185, 0])]] }],
  });
  // rides the disc (a child: areas move with their node)
  S.instance('Coin3', 'Disc', 'scenes/coin.cwscene', { position: vec([0, 1.25, 0]) });

  block(S, 'Yard', [0, 0, -20.5], [6, 3, 9], 'metal');
  coin(S, 'Coin4', [0, 1, -18]);
  // a wall only a double jump clears
  block(S, 'Wall', [0, 2.6, -28], [6, 5.6, 6], 'stone');
  gem(S, 'Gem1', [0, 3.6, -28]);
  coin(S, 'Coin5', [0, 2.9, -24.6]);

  // the workshop floor: checkpoint, crates, balls
  block(S, 'Shop', [0, 0, -36], [8, 3, 10], 'metal');
  S.instance('Check1', '.', 'scenes/checkpoint.cwscene', { position: vec([1.2, 0, -33]) });
  for (let i = 0; i < 4; i++) S.instance(`Crate${i + 1}`, '.', 'scenes/crate.cwscene', { position: vec([-2.6, 0.45 + 0.9 * i, -34 - (i % 2) * 0.06]), rotation: vec([0, 0.15 * i, 0]) });
  for (let i = 0; i < 3; i++) S.instance(`Ball${i + 1}`, '.', 'scenes/ball.cwscene', { position: vec([2.8, 0.45, -37.5 - i * 1.0]) });
  coin(S, 'Coin6', [0, 1, -35]);
  coin(S, 'Coin7', [0, 1, -37]);

  // the lift up to the high deck
  mover(S, 'Lift', [0, 0, -42.6], [3, 0.4, 3], 'wood', {
    length: 2.4, loop: 'PING_PONG',
    tracks: [{ path: ':position', interp: 'HERMITE', keys: [[0, eased([0, -0.2, -42.6])], [0.4, eased([0, -0.2, -42.6])], [2.0, eased([0, 4.3, -42.6])], [2.4, eased([0, 4.3, -42.6])]] }],
  });
  S.instance('Lava1', '.', 'scenes/lava.cwscene', { position: vec([0, -2.6, -43]) });
  block(S, 'Deck', [0, 4.5, -49], [6, 3, 9], 'metal');
  gem(S, 'Gem2', [0, 6.9, -54]);
  coin(S, 'Coin8', [0, 5.5, -47]);
  // the dash gap
  coin(S, 'Coin9', [0, 6.2, -55]);
  coin(S, 'Coin10', [0, 6.2, -57]);
  block(S, 'Pier', [0, 4.5, -64], [6, 3, 11], 'metal');
  S.instance('Saw1', '.', 'scenes/saw.cwscene', { position: vec([0, 5.25, -61.5]), rotation: vec([0, 1.5708, 0]) });
  S.node('SawAnim', '.', 'AnimationPlayer', { autoplay: '"sweep"' });
  S.anim('sweep', 'SawAnim', 2.2, 'PING_PONG', [{ path: 'Saw1:position', interp: 'HERMITE', keys: [[0, eased([-2.3, 5.25, -61.5])], [2.2, eased([2.3, 5.25, -61.5])]] }]);
  S.instance('Goal', '.', 'scenes/goal.cwscene', { position: vec([0, 4.5, -66.5]) });

  lamp(S, 'Lamp1', [-2.8, 0, -2]);
  lamp(S, 'Lamp2', [2.6, 0, -18]);
  lamp(S, 'Lamp3', [-3.4, 0, -39]);
  lamp(S, 'Lamp4', [2.6, 4.5, -51]);
  lamp(S, 'Lamp5', [-2.6, 4.5, -67]);
  cloud(S, 'Cloud1', [-15, 2, -15], 1.5);
  cloud(S, 'Cloud2', [14, 9, -40], 2);
  cloud(S, 'Cloud3', [-12, -6, -55], 2.4);
  cloud(S, 'Cloud4', [8, -4, -5], 1.4);
  farIsland(S, 'Isle1', [22, -2, -25], 2.5, [0.55, 0.5, 0.6], [0.3, 0.25, 0.32]);
  farIsland(S, 'Isle2', [-25, 4, -60], 3, [0.55, 0.5, 0.6], [0.3, 0.25, 0.32]);
  return S.text();
}

// ── title and end: showcase scenes ──────────────────────────────────
function showcase(kind, opts) {
  const S = new Scene();
  info(S, { title: `"${opts.title}"`, kind, next_scene: `"${opts.next}"`, music: '"audio/music_title.ogg"' });
  environment(S, { bg: opts.bg, fog: opts.fog, sky: [0.65, 0.8, 1], ground: [0.35, 0.3, 0.2], fogNear: 25, fogFar: 90 });
  S.node('Sun', '.', 'DirectionalLight3D', { position: vec([5, 12, 6]), rotation: vec([-1.11, 0.36, 0]), color: col([1, 0.95, 0.85]),
    energy: dirE(1.2), shadow: true, shadow_size: 2048, shadow_extent: 12, shadow_far: 50, shadow_bias: f(-0.0006) });
  S.node('Pivot', '.', 'Node3D', { position: vec([0, 1, 0]) });
  S.node('Camera', 'Pivot', 'Camera3D', { position: vec(opts.cam), rotation: vec(opts.camRot), fov: 50 });
  S.node('Orbit', '.', 'AnimationPlayer', { autoplay: '"orbit"' });
  S.anim('orbit', 'Orbit', 40, 'LOOP', [{ path: 'Pivot:rotation', keys: [[0, vec([0, 0, 0])], [40, vec([0, 6.283185, 0])]] }]);
  block(S, 'Island', [0, 0, 0], [9, 3, 9], opts.style || 'grass');
  S.instance('Robot', '.', 'scenes/hero_model.cwscene', { position: vec([0, 0, 0]), rotation: vec([0, 0.35, 0]) });
  S.node('Anim', 'Robot', null, { autoplay: `"${opts.anim}"` });
  tree(S, 'Tree1', [-3, 0, -2.5]);
  tree(S, 'Tree2', [3.2, 0, -2], 0.8);
  for (let i = 0; i < 6; i++) {
    const a = (i / 6) * Math.PI * 2;
    coin(S, `Coin${i + 1}`, [Math.sin(a) * 2.6, 1.2, Math.cos(a) * 2.6]);
  }
  cloud(S, 'Cloud1', [-12, 3, -14], 1.6);
  cloud(S, 'Cloud2', [13, 6, -10], 1.8);
  cloud(S, 'Cloud3', [5, -6, 14], 2);
  farIsland(S, 'Isle1', [-20, -2, -24], 2.5);
  farIsland(S, 'Isle2', [22, 2, 10], 2.2);
  if (opts.confetti) {
    // a celebration that physics plays: crates and balls rain onto the island
    for (let i = 0; i < 14; i++) {
      const a = i * 2.39996, r = 1.8 + (i % 4) * 0.6;
      const path = i % 2 ? 'scenes/ball.cwscene' : 'scenes/crate.cwscene';
      S.instance(`Drop${i + 1}`, '.', path, { position: vec([Math.sin(a) * r, 6 + i * 1.6, Math.cos(a) * r - 1]), rotation: vec([i * 0.3, i * 0.7, 0]) });
    }
  }
  return S.text();
}

writeFileSync(join(D, 'level1.cwscene'), level1());
writeFileSync(join(D, 'level2.cwscene'), level2());
writeFileSync(join(D, 'title.cwscene'), showcase('TITLE', { title: 'Robo Hop', next: 'scenes/level1.cwscene', bg: [0.42, 0.66, 0.95], fog: [0.62, 0.78, 0.96], cam: [0, 2.2, 9], camRot: [-0.2, 0, 0], anim: 'idle' }));
writeFileSync(join(D, 'end.cwscene'), showcase('END', { title: 'The End', next: 'scenes/title.cwscene', bg: [0.99, 0.78, 0.6], fog: [0.99, 0.8, 0.64], cam: [0, 2.2, 8.5], camRot: [-0.2, 0, 0], anim: 'cheer', confetti: true }));
console.log('scenes written to', D);
