// Editor gate over the debug channel: a scripted editing session sent as
// `ed <command line>` requests to the native editor (CW_DEBUG_CMDS): every
// reply must say ok (except the ones that must fail), the edits must land in
// the saved scene, and settings must persist in a file (CW_EDITOR_SETTINGS).
// (The editor cart leg is gone: the editor draws with GL itself, GL_ONLY, so
// it has no dual cart, and there is no GL-only cart.)
//   node tests/editor-channel.mjs
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'editor-channel');
mkdirSync(OUT, { recursive: true });
const exe = join(ROOT, 'build/native/editor/editor');
if (!existsSync(exe)) { console.error('missing', exe); process.exit(1); }
const b64 = (s) => Buffer.from(s, 'utf8').toString('base64');

// Requests as a tool sends them. Lines never contain ';' (CW_DEBUG_CMDS
// separates on it). A key="value with spaces" form goes through ed64.
const session = [
  'ed scene.list',
  'ed select paths=Ball',
  'ed node.set Ball position vec3(0, 3, 0) merge=drag',
  'ed node.set Ball position vec3(0, 4, 0) merge=drag',
  'ed history.list',
  'ed node.add MeshInstance3D Props Pillar',
  'ed node.set Props/Pillar primitive CYLINDER',
  'ed node.instance props/crate.cwscene Props CrateC',
  'ed node.delete Props/CrateA',
  'ed edit.undo',
  'ed edit.redo',
  'ed edit.undo',
  `ed64 ${b64('node.rename path=Props/Pillar name="Tall Pillar"')}`,
  'ed node.get "Props/Tall Pillar" primitive',
  'ed node.props Ball',
  'ed scene.tree',
  'ed settings.set interface.accent blue',
  'ed panel.move id=output slot=right_bottom',
  'ed panel.list',
  'ed scene.save',
  'ed scene.text',
  'ed editor.state',
  'ed help node.set',
  'ed project.info',
  'vfs ls',
];
const mustFail = ['ed node.delete NoSuchNode', 'ed nosuch.command', 'ed node.set Ball position notavalue'];
const all = [...session, ...mustFail];
for (const q of all) if (q.includes(';')) throw new Error('a request contains ; ' + q);

// native: every request after 30 frames, one reply per line
const env = { ...process.env, CW_FRAMES: '30', SDL_AUDIODRIVER: 'dummy', CW_DEBUG_CMDS: all.join(';') };
delete env.CW_PROJECT;
delete env.CW_EDITOR_SETTINGS;
const r = spawnSync(exe, [], { encoding: 'utf8', env, timeout: 120000 });
if (r.status !== 0) { console.error('native failed', r.stderr); process.exit(1); }
const native = r.stdout.trim().split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));

let fail = 0;
const bad = (m) => { fail++; console.log('FAIL ' + m); };
if (native.length !== all.length) bad(`native gave ${native.length} replies for ${all.length} requests`);
all.forEach((q, i) => {
  const r = JSON.stringify(native[i]);
  if (q.startsWith('ed') && i < session.length && !native[i]?.ok) bad(`${q} did not succeed: ${r?.slice(0, 300)}`);
  if (i >= session.length && native[i]?.ok !== false) bad(`CONTROL PASSED: ${q} should have failed`);
});
const reply = (q) => native[all.indexOf(q)];
// the merged drag is ONE history entry with the last value
const hist = reply('ed history.list');
if (!(hist?.entries?.length === 1 && hist.entries[0].includes('vec3(0, 4, 0)'))) bad('drag did not merge into one entry: ' + JSON.stringify(hist));
const text = reply('ed scene.text')?.text || '';
if (!text.includes('[node name="Tall Pillar" parent="Props" type="MeshInstance3D"]')) bad('rename through ed64 missing from the scene text');
if (!text.includes('[node name="CrateA" parent="Props" instance=1]')) bad('undo of the delete did not bring CrateA back');
if (!text.includes('[node name="CrateC" parent="Props" instance=1]')) bad('the instanced CrateC is missing');

// native settings persistence in a file
const cfg = join(OUT, 'settings.cfg');
rmSync(cfg, { force: true });
const runN = (cmds, withFile) => {
  const e = { ...process.env, CW_FRAMES: '3', SDL_AUDIODRIVER: 'dummy', CW_DEBUG_CMDS: cmds };
  if (withFile) e.CW_EDITOR_SETTINGS = cfg;
  else delete e.CW_EDITOR_SETTINGS;
  const x = spawnSync(exe, [], { encoding: 'utf8', env: e, timeout: 60000 });
  return x.stdout.trim().split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));
};
runN('ed settings.set interface.theme light', true); // saved on exit
const got = runN('ed settings.get interface.theme', true)[0];
const ctl = runN('ed settings.get interface.theme', false)[0];
if (got?.value !== 'light') bad('native settings did not persist in ' + cfg + ': ' + JSON.stringify(got));
if (ctl?.value !== 'dark') bad('CONTROL: without the settings file the theme should be the default: ' + JSON.stringify(ctl));

console.log(fail ? `editor-channel: ${fail} failures` : `ok   editor-channel (${session.length} requests + ${mustFail.length} must-fail, natively, settings persistence)`);
process.exit(fail ? 1 : 0);
