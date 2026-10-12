// Editor gate: every .cwscene in the repository (samples, the editor's
// demo project) loads in the native editor and saves LOSSLESSLY
// (`ed scene.check`: no load warnings, the saved text loads back to the same
// nodes, types, groups and values, and saving is a fixed point). Canonical
// (byte-identical) scenes are counted; hand-written ones often are not
// (property order, enums by number, defaults written out), and the editor
// demo project must be. Scenes that use node types only their game registers
// are refused by the editor (it would lose those nodes) and are listed, not
// failed. Controls: reordered property lines must report canonical:false
// (still lossless); an unknown property must report lossless:false.
//   node tests/editor-scenes.mjs
import { spawnSync } from 'node:child_process';
import { readdirSync, statSync, readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const exe = join(ROOT, 'build/native/editor/editor');
if (!existsSync(exe)) { console.error('missing', exe); process.exit(1); }

function walk(dir, out = []) {
  for (const e of readdirSync(dir)) {
    const p = join(dir, e);
    if (statSync(p).isDirectory()) walk(p, out);
    else if (e.endsWith('.cwscene')) out.push(p);
  }
  return out;
}
// an asset root is the directory scenes' ext_scene paths are relative to:
// samples/<x>/assets, samples/ui_assets, editor/demo_project
const roots = new Map();
for (const f of [...walk(join(ROOT, 'samples')), ...walk(join(ROOT, 'editor/demo_project'))]) {
  const r = relative(ROOT, f);
  const m = r.match(/^(samples\/[^/]+\/assets|samples\/ui_assets|editor\/demo_project)\/(.*)$/);
  if (!m) { console.log('skip (no asset root):', r); continue; }
  if (!roots.has(m[1])) roots.set(m[1], []);
  roots.get(m[1]).push(m[2]);
}

function check(root, scenes) {
  const env = { ...process.env, CW_FRAMES: '1', SDL_AUDIODRIVER: 'dummy', CW_PROJECT: root,
                CW_DEBUG_CMDS: scenes.map((s) => `ed scene.check path=${s}`).join(';') };
  delete env.CW_EDITOR_SETTINGS;
  const r = spawnSync(exe, [], { encoding: 'utf8', env, timeout: 120000 });
  if (r.status !== 0) throw new Error(`editor failed in ${root}: ${r.stderr.slice(-500)}`);
  return r.stdout.trim().split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));
}

let fail = 0, ok = 0, canonical = 0;
const refused = [];
for (const [root, scenes] of roots) {
  const replies = check(join(ROOT, root), scenes);
  scenes.forEach((s, i) => {
    const r = replies[i];
    if (r?.ok && r.lossless) {
      ok++;
      if (r.canonical) canonical++;
      else if (root === 'editor/demo_project') { fail++; console.log(`FAIL ${root}/${s} is not canonical: ${JSON.stringify(r)}`); }
    }
    else if (r && !r.ok && /node types this editor build does not have/.test(r.error)) refused.push(`${root}/${s}`);
    else { fail++; console.log(`FAIL ${root}/${s}: ${JSON.stringify(r)}`); }
  });
}
// controls: reordered property lines are lossless but NOT canonical; an
// unknown property is NOT lossless
const ctlDir = join(ROOT, 'out', 'editor-scenes-control');
mkdirSync(ctlDir, { recursive: true });
const src = readFileSync(join(ROOT, 'editor/demo_project/props/crate.cwscene'), 'utf8').split('\n');
const i = src.findIndex((l) => l.startsWith('color ='));
[src[i], src[i + 1]] = [src[i + 1], src[i]];
writeFileSync(join(ctlDir, 'crate.cwscene'), src.join('\n'));
writeFileSync(join(ctlDir, 'lossy.cwscene'), src.join('\n').replace('roughness = 0.8', 'roughness = 0.8\nno_such_property = 3'));
const [ctl, ctl2] = check(ctlDir, ['crate.cwscene', 'lossy.cwscene']);
if (!(ctl?.ok && ctl.lossless === true && ctl.canonical === false)) { fail++; console.log('CONTROL: a reordered scene reported ' + JSON.stringify(ctl)); }
if (!(ctl2?.ok && ctl2.lossless === false)) { fail++; console.log('CONTROL PASSED: a scene with an unknown property reported ' + JSON.stringify(ctl2)); }
if (ok < 30) { fail++; console.log(`only ${ok} scenes checked: the corpus walk is broken`); }
if (refused.length) console.log(`(refused, game-registered node types: ${refused.length}: ${refused.join(' ')})`);
console.log(fail ? `editor-scenes: ${fail} failures` : `ok   editor-scenes (${ok} scenes lossless, ${canonical} of them byte for byte; controls fail)`);
process.exit(fail ? 1 : 0);
