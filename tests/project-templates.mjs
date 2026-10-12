// templates gate: every editor template (editor/templates/<id>) becomes
// a project through the editor's project.new, builds native AND cart against
// the engine (tools/cwproject.mjs: the engine tree with CW_PROJECT_DIR), and
// runs the same pad script on every host:
//   native (the project's dev executable), romdev and chromium (its .wasc):
//     the named debug state must agree exactly and the last frames within the
//     gate's tolerance; wcnative (no input there) must match the frame of an
//     input-free native run.
// Each template's check is something its input causes (a press counted, a
// pickup collected, the player moved); the CONTROL run without the script
// must fail that check.
//
//   node tests/project-templates.mjs [template ...] [--hosts native,romdev,chromium,wcnative]
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, rmSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'project-templates');
const EDITOR = join(ROOT, 'build/native/editor/editor');
const argv = process.argv.slice(2);
const hostsArg = argv.includes('--hosts') ? argv[argv.indexOf('--hosts') + 1].split(',') : ['native', 'romdev', 'chromium', 'wcnative'];
const only = argv.filter((a, i) => !a.startsWith('--') && argv[i - 1] !== '--hosts');
const FRAMES = 200;

/* per template: the pad script, the state fields every host must agree on, what the script must cause */
const SPEC = {
  empty_2d: { input: [{ frame: 30, buttons: ['south'] }, { frame: 34 }], fields: ['game.ticks', 'game.presses', 'marker.rotation', 'tree.nodes'],
    caused: (s) => s['game.presses'] === 1 },
  empty_3d: { input: [{ frame: 30, buttons: ['south'] }, { frame: 34 }], fields: ['game.ticks', 'game.presses', 'cube.rotation', 'tree.nodes'],
    caused: (s) => s['game.presses'] === 1 },
  platformer_2d: { input: [{ frame: 10, axes: { lx: 1 } }, { frame: 60, axes: { lx: 1 }, buttons: ['south'] }, { frame: 64, axes: { lx: 1 } }],
    fields: ['game.ticks', 'player.pos', 'player.on_floor', 'player.jumps', 'coins', 'player.respawns', 'tree.nodes'],
    caused: (s) => s.coins >= 1 && s['player.jumps'] === 1 && s['player.pos'][0] > 200 },
  platformer_3d: { input: [{ frame: 10, axes: { lx: 1 } }, { frame: 40, axes: { lx: 1 }, buttons: ['south'] }, { frame: 44, axes: { lx: 1 } }],
    fields: ['game.ticks', 'player.pos', 'player.on_floor', 'player.jumps', 'gems', 'player.respawns', 'tree.nodes'],
    caused: (s) => s.gems >= 1 && s['player.jumps'] === 1 && s['player.respawns'] >= 1, frames: 300 }, // runs off the edge: respawns
};

let failed = 0;
const check = (name, ok, detail = '') => {
  console.log(ok ? `ok   ${name}` : `FAIL ${name}${detail ? ': ' + String(detail).slice(0, 1200) : ''}`);
  if (!ok) failed++;
  return ok;
};

rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
const ids = readdirSync(join(ROOT, 'editor/templates')).filter((t) => existsSync(join(ROOT, 'editor/templates', t, 'template.cfg')))
  .filter((t) => !only.length || only.includes(t));
check('templates: the four required ones exist', ['empty_2d', 'empty_3d', 'platformer_2d', 'platformer_3d'].every((t) => ids.includes(t) || only.length), ids.join(' '));

// 1. the editor makes the projects (project.new; a CW_FRAMES run answers CW_DEBUG_CMDS at its end)
const cmds = ids.map((t) => `ed project.new name=${t} template=${t} dir=${join(OUT, t)}`);
const er = spawnSync(EDITOR, [], { encoding: 'utf8', timeout: 120000,
  env: { ...process.env, CW_FRAMES: '3', SDL_AUDIODRIVER: 'dummy', CW_EDITOR_SETTINGS: join(OUT, 'editor.cfg'), CW_DEBUG_CMDS: cmds.join(';') } });
const replies = (er.stdout || '').split('\n').filter((l) => l.startsWith('{'));
ids.forEach((t, i) => check(`${t}: project.new`, replies[i] && JSON.parse(replies[i]).ok, replies[i] || er.stderr));

const run = (args) => spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), ...args], { encoding: 'utf8', timeout: 300000 });

for (const t of ids) {
  const dir = join(OUT, t), spec = SPEC[t];
  if (!spec) { check(`${t}: has a gate spec`, false); continue; }
  // a project's net.domains reaches its cart's manifest (cw_add_app reads it);
  // the other templates, without the key, are the control
  if (t === 'empty_2d') writeFileSync(join(dir, 'project.cwproj'), readFileSync(join(dir, 'project.cwproj'), 'utf8') + 'net.domains = play.example.com, lobby.example.com\n');
  // 2. build native + cart
  const b = spawnSync('node', [join(ROOT, 'tools/cwproject.mjs'), 'build', dir, 'all'], { encoding: 'utf8', timeout: 1800000 });
  const results = (b.stdout || '').split('\n').filter((l) => l.startsWith('@result ')).map((l) => JSON.parse(l.slice(8)));
  if (!check(`${t}: builds native + cart against the engine`, b.status === 0 && results.length === 2 && results.every((r) => r.ok),
    results.length ? JSON.stringify(results) : (b.stdout || '').slice(-1500))) continue;
  const exe = join(dir, results[0].path), cart = join(dir, results[1].path), app = t;
  const man = JSON.parse(spawnSync('unzip', ['-p', cart, 'manifest.json'], { encoding: 'utf8' }).stdout || '{}');
  const domains = JSON.stringify((man.net || {}).domains || []);
  check(`${t}: net.domains from project.cwproj`, domains === (t === 'empty_2d' ? '["play.example.com","lobby.example.com"]' : '[]'), domains);
  const input = join(OUT, `${t}-input.json`);
  writeFileSync(input, JSON.stringify(spec.input));
  // 3. the same script on every host
  const states = {}, shots = {};
  for (const h of hostsArg) {
    const shot = join(OUT, `${t}.${h}.png`), state = join(OUT, `${t}.${h}.json`);
    const a = ['--host', h, '--app', app, '--exe', exe, '--cart', cart, '--frames', String(spec.frames || FRAMES), '--shot', shot];
    if (h !== 'wcnative') a.push('--input', input, '--state', state);
    const r = run(a);
    if (!check(`${t}: runs on ${h}`, r.status === 0 && existsSync(shot), (r.stderr || r.stdout).slice(-1000))) continue;
    shots[h] = shot;
    if (h !== 'wcnative') states[h] = JSON.parse(readFileSync(state, 'utf8'));
  }
  const ref = states.native;
  if (ref) {
    check(`${t}: native: the script did what it should (${spec.fields.map((f) => `${f}=${JSON.stringify(ref[f])}`).join(' ')})`, spec.caused(ref));
    for (const h of Object.keys(states).filter((h) => h !== 'native')) {
      const diff = spec.fields.filter((f) => JSON.stringify(states[h][f]) !== JSON.stringify(ref[f]));
      check(`${t}: ${h} state equals native`, !diff.length, diff.map((f) => `${f}: ${JSON.stringify(states[h][f])} vs ${JSON.stringify(ref[f])}`).join('; '));
    }
    for (const h of Object.keys(shots).filter((h) => h !== 'native' && h !== 'wcnative')) {
      const d = diffImages(readImage(shots.native), readImage(shots[h]));
      check(`${t}: ${h} frame matches native (${(d.gt16 * 100).toFixed(3)}% > 16)`, d.gt16 <= 0.002);
    }
    // control: no input -> the check must fail
    const cs = join(OUT, `${t}.control.json`);
    const c = run(['--host', 'native', '--app', app, '--exe', exe, '--frames', String(spec.frames || FRAMES), '--state', cs, '--shot', join(OUT, `${t}.control.png`)]);
    const cst = c.status === 0 ? JSON.parse(readFileSync(cs, 'utf8')) : null;
    check(`${t}: control (no input) must fail the check`, cst && !spec.caused(cst), JSON.stringify(cst));
    if (shots.wcnative && cst) {
      // wcnative cannot take input: its frame must match the input-free native run's
      const d = diffImages(readImage(join(OUT, `${t}.control.png`)), readImage(shots.wcnative));
      check(`${t}: wcnative frame matches the input-free native run (${(d.gt16 * 100).toFixed(3)}% > 16)`, d.gt16 <= 0.002);
    }
  }
}
console.log(failed ? `${failed} check(s) failed` : 'project-templates: all checks passed');
process.exit(failed ? 1 : 0);
