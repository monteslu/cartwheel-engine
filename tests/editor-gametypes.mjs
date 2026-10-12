// Editor gate: a game's node types load into the native editor from
// the game's hot module (code.load_types: dlopen + the module's exported
// cw_game_register_types), so the scenes the editor refuses without them
// (editor-scenes lists them) open and save LOSSLESSLY. Also checks the
// node -> C jump (code.node_source): a game node's type resolves to the
// file and line of its descriptor and of its fixed_update function.
// Controls: the same scenes without the module must be refused naming the
// types; a module without cw_game_register_types must be refused.
//   node tests/editor-gametypes.mjs
import { spawnSync } from 'node:child_process';
import { readdirSync, statSync, readFileSync, existsSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const B = join(ROOT, 'build/native');
const exe = join(B, 'editor/editor');
const GAMES = [
  { root: 'games/action2d/assets', module: 'games/action2d_hot.so' },
  { root: 'games/platformer3d/assets', module: 'games/platformer3d_hot.so' },
  { root: 'samples/physics3d_nodes/assets', module: 'samples/physics3d_nodes_hot.so' },
  { root: 'samples/remote_target/assets', module: 'samples/remote_target_hot.so' },
  { root: 'samples/triggers_demo/assets', module: 'samples/triggers_demo_hot.so' },
];
const b = spawnSync('ninja', ['-C', B, 'editor/editor', ...GAMES.map((g) => g.module)], { encoding: 'utf8' });
if (b.status !== 0) { console.log(b.stdout + b.stderr); process.exit(1); }

function walk(dir, out = []) {
  for (const e of readdirSync(dir)) {
    const p = join(dir, e);
    if (statSync(p).isDirectory()) walk(p, out);
    else if (e.endsWith('.cwscene')) out.push(p);
  }
  return out;
}

function ed(root, cmds) {
  const env = { ...process.env, CW_FRAMES: '1', SDL_AUDIODRIVER: 'dummy', CW_PROJECT: join(ROOT, root), CW_DEBUG_CMDS: cmds.join(';') };
  delete env.CW_EDITOR_SETTINGS;
  const r = spawnSync(exe, [], { encoding: 'utf8', env, timeout: 120000 });
  if (r.status !== 0) throw new Error(`editor failed in ${root}: ${r.stderr.slice(-800)}`);
  return r.stdout.trim().split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));
}

let fail = 0, opened = 0, refusedWithout = 0, types = 0;
const bad = (m) => { fail++; console.log('FAIL ' + m); };
for (const g of GAMES) {
  const scenes = walk(join(ROOT, g.root)).map((s) => relative(join(ROOT, g.root), s)).sort();
  const checks = scenes.map((s) => `ed scene.check path=${s}`);
  // without the module: the game-type scenes are refused, naming the types
  const before = ed(g.root, checks);
  const needTypes = scenes.filter((s, i) => !before[i]?.ok && /node types this editor build does not have/.test(before[i]?.error));
  refusedWithout += needTypes.length;
  // with it: every scene opens losslessly
  const after = ed(g.root, [`ed code.load_types module=${join(B, g.module)}`, ...checks]);
  if (!after[0]?.ok) { bad(`${g.module}: ${JSON.stringify(after[0])}`); continue; }
  types += after[0].game_types;
  scenes.forEach((s, i) => {
    const r = after[i + 1];
    if (!r?.ok || !r.lossless) bad(`${g.root}/${s} with ${g.module}: ${JSON.stringify(r)}`);
    else if (needTypes.includes(s)) opened++;
  });
}
if (!refusedWithout) bad('CONTROL PASSED: no scene was refused without its game module');

// node -> C: a slime in action2d resolves to actors.c (descriptor + slime_fixed)
const actors = readFileSync(join(ROOT, 'games/action2d/actors.c'), 'utf8').split('\n');
const lineOf = (re) => actors.findIndex((l) => re.test(l)) + 1;
const ns = ed('games/action2d/assets', [`ed code.load_types module=${join(B, 'games/action2d_hot.so')}`, 'ed scene.open path=actors/slime.cwscene',
  'ed code.node_source path=.']).pop();
const slime = ns?.types?.[0];
const fixed = slime?.callbacks?.find((c) => c.callback === 'fixed_update');
if (slime?.type !== 'Slime' || !slime.game) bad(`node_source type: ${JSON.stringify(slime)}`);
else if (!slime.defined?.file.endsWith('games/action2d/actors.c') || slime.defined.line !== lineOf(/\.name = "Slime"/)) bad(`Slime descriptor at ${JSON.stringify(slime.defined)}`);
else if (fixed?.function !== 'slime_fixed' || fixed.line !== lineOf(/^static void slime_fixed\(/)) bad(`slime fixed_update at ${JSON.stringify(fixed)}`);
else if (!ns.opened || ns.opened.line !== fixed.line) bad(`node_source opened ${JSON.stringify(ns.opened)}, expected the fixed_update line`);
const engine = ns?.types?.find((t) => t.type === 'CharacterBody2D');
if (!engine || engine.game || !engine.registered?.file.endsWith('src/scene/phys2d_nodes.c')) bad(`engine type: ${JSON.stringify(engine)}`);

// control: something that is not a game module is refused
const none = ed('editor/demo_project', [`ed code.load_types module=${join(B, 'editor/editor')}`])[0];
if (none?.ok) bad('CONTROL PASSED: an executable without cw_game_register_types loaded as a game module');
if (fail) { console.log(`editor-gametypes: ${fail} failure(s)`); process.exit(1); }
console.log(`ok   editor-gametypes (${opened} scenes refused without their game module open losslessly with it, ${types} game types; node_source finds Slime and slime_fixed; controls fail)`);
