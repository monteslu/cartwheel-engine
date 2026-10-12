// Writes action2d's room scenes (assets/rooms/room*.cwscene) from the ASCII
// maps below, so a level is edited here as a picture and the .cwscene stays
// the thing the engine loads (and the thing the editor would save).
//   node games/action2d/make-rooms.mjs
//
// Legend
//   #  wall top (solid)          =  wall face (solid)       T  wall face + torch (a Light2D)
//   N  wall face + banner        K  cracked wall face       D  door (two in a face row)
//   o  pillar (solid, occludes light in lit rooms)
//   .  floor (variants)  ,  mossy floor  r  rug  R rug edge  +  grate  _  dark floor  u  rune (glows)
//   x  skull  y  bones  z  rubble  w  cobweb (decor, not solid)
//   P  player start   s slime   b bat   t spitter   B boss (centre of its 2x2)
//   h  heart pickup   g gem
import { writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = join(dirname(fileURLToPath(import.meta.url)), 'assets', 'rooms');
mkdirSync(OUT, { recursive: true });
// A stateless position hash (no cycle); it loses low bits in doubles, but changing it
// int-math-ok: changing it would change the committed art. New code: Math.imul or tools/lib/rng.mjs.
const hash = (x, y, s = 0) => { let h = (x * 374761393 + y * 668265263 + s * 2147483647) >>> 0; h = ((h ^ (h >>> 13)) * 1274126177) >>> 0; return ((h ^ (h >>> 16)) >>> 0) / 4294967296; };

const ROOMS = [
  {
    file: 'room1.cwscene', title: 'THE HALL', music: 'crypt', next: 2,
    ambient: 'color(0.78, 0.74, 0.86, 1)', clear: 'color(0.05, 0.04, 0.08, 1)', lamp: 0.5,
    map: [
      '####################',
      '#==T==N==DD==N==T==#',
      '#w................w#',
      '#..s............s..#',
      '#...o...rRRr...o...#',
      '#.......rrrr.......#',
      '#..x....rrrr....g..#',
      '#...o...rrrr...o...#',
      '#.h.....rrrr....s..#',
      '#..y.....P......z..#',
      '#..................#',
      '####################',
    ],
  },
  {
    file: 'room2.cwscene', title: 'THE DARK CRYPT', music: 'crypt', next: 3,
    ambient: 'color(0.08, 0.07, 0.14, 1)', clear: 'color(0.02, 0.02, 0.04, 1)', lamp: 1.2, lampRange: 76, torch: 84,
    map: [
      '################################',
      '#==T======K====T====DD===N==T==#',
      '#w............................w#',
      '#...b....................t.....#',
      '#.....o.......o....o.......o...#',
      '#.........y....................#',
      '#..t........,,,,...........b...#',
      '#...........,uu,.........x.....#',
      '#.....o.....,,,,...o.......o...#',
      '#........g.................h...#',
      '#=====T=====.....=====T====....#',
      '#############.....#########....#',
      '#############.....#########....#',
      '#..z..........s............g...#',
      '#.....o.................o......#',
      '#....P..........x..............#',
      '#..............................#',
      '################################',
    ],
  },
  {
    file: 'room3.cwscene', title: 'THE WARDEN', music: 'boss', next: 0,
    ambient: 'color(0.42, 0.3, 0.5, 1)', clear: 'color(0.03, 0.01, 0.05, 1)', lamp: 0.7,
    map: [
      '######################',
      '#==T====N====N====T==#',
      '#w__________________w#',
      '#h_u______________u_h#',
      '#____o__________o____#',
      '#__________B_________#',
      '#____________________#',
      '#__x______uu______y__#',
      '#_________uu_________#',
      '#____o__________o____#',
      '#__u_______P______u__#',
      '#____________________#',
      '######################',
    ],
  },
];

const SOLID = new Set('#=TNKDo');
const fmtCells = (rows) => rows.map((r) => r.map((c) => (c < 0 ? '.' : String(c))).join(' ')).join(' / ');
const ellipse = (rx, ry, n = 10) => Array.from({ length: n }, (_, i) => `${(Math.cos(i / n * Math.PI * 2) * rx).toFixed(1)} ${(Math.sin(i / n * Math.PI * 2) * ry).toFixed(1)}`).join(', ');

for (const [ri, R] of ROOMS.entries()) {
  const H = R.map.length, W = R.map[0].length;
  R.map.forEach((r, y) => { if (r.length !== W) throw new Error(`${R.file}: row ${y} is ${r.length} wide, want ${W}`); });
  const at = (x, y) => (y >= 0 && y < H && x >= 0 && x < R.map[y].length ? R.map[y][x] : '#');
  const floor = [], walls = [];
  const ents = [], torches = [], pillars = [];
  let door = null;
  for (let y = 0; y < H; y++) {
    const f = [], w = [];
    for (let x = 0; x < W; x++) {
      const c = at(x, y);
      const dark = R.music === 'boss';
      let fl = dark ? 7 : hash(x, y, ri) < 0.7 ? 0 : hash(x, y, ri + 9) < 0.7 ? 1 : 2;
      if (c === ',') fl = 3;
      if (c === 'r') fl = 4;
      if (c === 'R') fl = 5;
      if (c === '+') fl = 6;
      if (c === '_') fl = 7;
      if (c === 'u') fl = 13;
      if (!SOLID.has(c) && '=TNKD'.includes(at(x, y - 1))) fl = fl === 7 ? 26 : 15; // shadow under a wall face
      let wl = -1;
      if (c === '#') wl = 8;
      else if (c === '=') wl = 9;
      else if (c === 'T') { wl = 10; torches.push([x, y]); }
      else if (c === 'N') wl = 25;
      else if (c === 'K') wl = 24;
      else if (c === 'o') { wl = 12; pillars.push([x, y]); }
      else if (c === 'x') wl = 16;
      else if (c === 'y') wl = 17;
      else if (c === 'z') wl = 18;
      else if (c === 'w') wl = 19;
      if (c === 'D' && !door) door = [x, y];
      if ('Psbthg'.includes(c)) ents.push([c, x, y]);
      if (c === 'B') ents.push([c, x, y]);
      f.push(SOLID.has(c) && c !== 'o' && c !== 'D' ? -1 : fl);
      w.push(wl);
    }
    floor.push(f); walls.push(w);
  }
  const px = (x) => x * 16 + 8;
  const L = [];
  L.push('[scene format=1]');
  L.push('[ext_scene id=1 path="actors/hero.cwscene"]');
  L.push('[ext_scene id=2 path="actors/slime.cwscene"]');
  L.push('[ext_scene id=3 path="actors/bat.cwscene"]');
  L.push('[ext_scene id=4 path="actors/spitter.cwscene"]');
  L.push('[ext_scene id=5 path="actors/boss.cwscene"]');
  L.push('[ext_scene id=6 path="actors/heart.cwscene"]');
  L.push('[ext_scene id=7 path="actors/gem.cwscene"]');
  L.push('[ext_scene id=8 path="actors/door.cwscene"]');
  L.push('');
  L.push(`; generated by games/action2d/make-rooms.mjs from an ASCII map: edit it there`);
  L.push(`[node name="Room" type="Room"]`);
  L.push(`title = "${R.title}"`);
  L.push(`music = "${R.music}"`);
  L.push(`index = ${ri + 1}`);
  L.push('');
  L.push(`[node name="Settings" parent="." type="CanvasSettings"]`);
  L.push('virtual_width = 320', 'virtual_height = 180', 'scale_mode = INTEGER', `clear_color = ${R.clear}`, `ambient = ${R.ambient}`, 'pixel_snap = true', 'y_sort = true');
  L.push('');
  L.push(`[node name="Map" parent="." type="TileMap2D"]`);
  L.push('tileset = "images/tiles.png"', 'layers = 2', 'animations = "10:2@6; 13:2@2"', `cells0 = "${fmtCells(floor)}"`, `cells1 = "${fmtCells(walls)}"`);
  L.push('z_index = -10');
  L.push('');
  L.push(`[node name="Walls" parent="." type="TileWalls"]`);
  L.push('tilemap = "../Map"', 'layer = 1', 'solid = "8-12, 24-25"', 'collision_layer_name = "world"');
  L.push('');
  // pillars cast shadows
  pillars.forEach(([x, y], i) => {
    L.push(`[node name="Pillar${i + 1}" parent="." type="LightOccluder2D"]`);
    L.push(`polygon = "${x * 16 + 2} ${y * 16 + 5}, ${x * 16 + 14} ${y * 16 + 5}, ${x * 16 + 14} ${y * 16 + 16}, ${x * 16 + 2} ${y * 16 + 16}"`);
    L.push('');
  });
  torches.forEach(([x, y], i) => {
    L.push(`[node name="Torch${i + 1}" parent="." type="Light2D"]`);
    L.push(`position = vec2(${px(x)}, ${y * 16 + 22})`, 'color = color(1, 0.62, 0.3, 1)', `energy = ${R.music === 'boss' ? 1.1 : 1.3}`, 'range = 120', 'shadow = true');
    L.push('');
  });
  if (door) {
    L.push(`[node name="Door" parent="." instance=8]`);
    L.push(`position = vec2(${door[0] * 16 + 16}, ${door[1] * 16 + 8})`, `next_room = ${R.next}`);
    L.push('');
  }
  const counts = {};
  const camera = [0, 0, W * 16, H * 16];
  for (const [c, x, y] of ents) {
    const kind = { P: ['Hero', 1], s: ['Slime', 2], b: ['Bat', 3], t: ['Spitter', 4], B: ['Boss', 5], h: ['Heart', 6], g: ['Gem', 7] }[c];
    counts[kind[0]] = (counts[kind[0]] || 0) + 1;
    const name = kind[0] === 'Hero' ? 'Hero' : kind[0] + counts[kind[0]];
    L.push(`[node name="${name}" parent="." instance=${kind[1]}]`);
    L.push(`position = vec2(${c === 'B' ? x * 16 + 16 : px(x)}, ${px(y)})`);
    if (c === 's') L.push(`patrol = vec2(${counts.Slime % 2 ? 1 : -1}, 0)`);
    if (c === 't') L.push(`phase = ${counts.Spitter * 37}`);
    L.push('');
    if (c === 'P') {
      L.push(`[node name="Camera" parent="Hero"]`);
      L.push(`limit_right = ${camera[2]}`, `limit_bottom = ${camera[3]}`);
      L.push('');
      L.push(`[node name="Lamp" parent="Hero"]`);
      L.push(`energy = ${R.lamp}`);
      if (R.lampRange) L.push(`range = ${R.lampRange}`);
      L.push('');
    }
  }
  // torches flicker: one looping animation over every torch's energy
  if (torches.length) {
    L.push(`[node name="Flicker" parent="." type="AnimationPlayer"]`);
    L.push('autoplay = "flicker"');
    L.push('');
    const e = R.music === 'boss' ? 1.1 : 1.3;
    L.push('[animation name="flicker" player="Flicker" length=1.6 loop=LOOP]');
    const pattern = [1, 0.8, 1.1, 0.86];
    torches.forEach((_, i) => {
      L.push(`[track type=value path="Torch${i + 1}:energy" interp=CUBIC]`);
      const v = pattern.map((_, j) => pattern[(j + i) % pattern.length]);
      [0, 0.4, 0.7, 1.1, 1.6].forEach((t, j) => L.push(`${t} = ${(e * v[j % v.length]).toFixed(3)}`));
    });
  }
  writeFileSync(join(OUT, R.file), L.join('\n') + '\n');
}
console.log('wrote', OUT);
