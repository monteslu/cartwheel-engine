// Editor gate over the debug channel: a scripted editing session sent as
// `ed <command line>` requests to the editor cart in romdev
// (wasm({op:'command'})) and to the native editor (CW_DEBUG_CMDS) must give
// the same replies, every reply must say ok (except the one that must fail),
// the scene the cart saved into its project layer must come out of
// `vfs export` byte for byte, and settings must persist: natively in a file
// (CW_EDITOR_SETTINGS), in the cart through the project layer (export, then
// import into a fresh cart).
//   node tests/editor-channel.mjs        ROMDEV_PORT (default 7331)
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { wgpuPin } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'editor-channel');
mkdirSync(OUT, { recursive: true });
const port = process.env.ROMDEV_PORT || 7331;
const exe = join(ROOT, 'build/native/editor/editor');
const cart = join(ROOT, 'build/cart/carts/editor.wasc');
for (const f of [exe, cart]) if (!existsSync(f)) { console.error('missing', f); process.exit(1); }
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

const S = `cw-editor-channel-${process.pid}`;
const call = async (tool, a) => {
  const res = await fetch(`http://127.0.0.1:${port}/tool/${tool}`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'x-romdev-session': S }, body: JSON.stringify(a) });
  const t = await res.text();
  try { return JSON.parse(t); } catch { return { raw: t }; }
};
const cmd = async (text) => (await call('wasm', { op: 'command', text })).reply;

let fail = 0;
const bad = (m) => { fail++; console.log('FAIL ' + m); };
await call('loadMedia', { platform: 'wasmcart', gpuPower: wgpuPin().power, path: cart, deterministicSeed: 1 });
await call('frame', { op: 'step', frames: 29 });
const cartReplies = [];
for (const q of all) cartReplies.push(await cmd(q));
await call('frame', { op: 'step', frames: 2 });
await call('frame', { op: 'screenshot', path: join(OUT, 'romdev.png') });

if (native.length !== all.length) bad(`native gave ${native.length} replies for ${all.length} requests`);
all.forEach((q, i) => {
  // node uids are process-wide counters (the cart made more UI nodes over its frames) and the
  // storage line names the host by design: everything else must match exactly
  const norm = (o) => JSON.stringify(o)?.replace(/"uid":\d+/g, '"uid":0').replace(/"storage":"[^"]*"/, '"storage":""')
    // a cart keeps its settings in the project layer (.cweditor/settings.cfg); natively they live in a user file
    .replace('".cweditor/settings.cfg",', '').replace(/"changed_files":\d+/, '"changed_files":0');
  const a = norm(native[i]), b = norm(cartReplies[i]);
  const isEd = q.startsWith('ed');
  if (a !== b) bad(`${q}\n  native ${a?.slice(0, 400)}\n  cart   ${b?.slice(0, 400)}`);
  if (isEd && i < session.length && !cartReplies[i]?.ok) bad(`${q} did not succeed: ${b?.slice(0, 300)}`);
  if (i >= session.length && cartReplies[i]?.ok !== false) bad(`CONTROL PASSED: ${q} should have failed`);
});
const reply = (q) => cartReplies[all.indexOf(q)];
// the merged drag is ONE history entry with the last value
const hist = reply('ed history.list');
if (!(hist?.entries?.length === 1 && hist.entries[0].includes('vec3(0, 4, 0)'))) bad('drag did not merge into one entry: ' + JSON.stringify(hist));
const text = reply('ed scene.text')?.text || '';
if (!text.includes('[node name="Tall Pillar" parent="Props" type="MeshInstance3D"]')) bad('rename through ed64 missing from the scene text');
if (!text.includes('[node name="CrateA" parent="Props" instance=1]')) bad('undo of the delete did not bring CrateA back');
if (!text.includes('[node name="CrateC" parent="Props" instance=1]')) bad('the instanced CrateC is missing');

// the saved scene and the settings come out of the cart's project layer
const exp = await cmd('vfs export');
const archive = Buffer.from(exp?.base64 || '', 'base64');
const files = {};
for (let p = 7; p < archive.length;) {
  const nl = archive.indexOf(10, p), name = archive.subarray(p, nl).toString();
  const nl2 = archive.indexOf(10, nl + 1), len = +archive.subarray(nl + 1, nl2).toString();
  files[name] = archive.subarray(nl2 + 1, nl2 + 1 + len).toString();
  p = nl2 + 1 + len;
}
if (files['main.cwscene'] !== text) bad('vfs export main.cwscene differs from the scene text the editor saved');
if (!(files['.cweditor/settings.cfg'] || '').includes('interface.accent = blue')) bad('settings missing from the project layer: ' + Object.keys(files));
writeFileSync(join(OUT, 'project.cwvfs'), archive);
await call('host', { op: 'shutdown' });

// a fresh cart: import the archive, reload settings, they persist; without the import they do not
await call('loadMedia', { platform: 'wasmcart', gpuPower: wgpuPin().power, path: cart, deterministicSeed: 1 });
await call('frame', { op: 'step', frames: 5 });
const before = await cmd('ed settings.get interface.accent');
const imp = await cmd(`vfs import ${exp.base64}`);
await cmd('ed settings.reload');
const after = await cmd('ed settings.get interface.accent');
const reopened = await cmd('ed scene.close force=true');
const reopen = await cmd('ed scene.open main.cwscene');
const text2 = (await cmd('ed scene.text'))?.text;
await call('host', { op: 'shutdown' });
if (before?.value !== 'teal') bad('control: a fresh cart should start with the default accent, got ' + JSON.stringify(before));
if (!imp?.files || after?.value !== 'blue') bad('cart settings did not persist through vfs export/import: ' + JSON.stringify(after));
if (!reopened?.ok || !reopen?.ok || text2 !== text) bad('the imported project scene did not reopen as saved');

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

console.log(fail ? `editor-channel: ${fail} failures` : `ok   editor-channel (${session.length} requests + ${mustFail.length} must-fail, native vs romdev cart, vfs export/import, settings persistence)`);
process.exit(fail ? 1 : 0);
