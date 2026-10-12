// M4.2 exit gate, "drag a glTF in, see it imported, place it", on every host.
//
// 1. The in-app session (editor/tests/import_app.c, app editor_import) on
//    native, romdev, Chromium and wasmcart-native: a CC0 glTF is copied into
//    the project (natively onto disk in a fresh project directory, in a cart
//    through the in-memory layer), imports through cwpack's importer, gets a
//    thumbnail, is dragged by the pointer from the FileSystem grid onto the
//    scene preview (placed on the ground there) and placed again with
//    fs.place; a used texture is renamed (the glTF's URI follows), deleting a
//    used file is refused, a corrupt glTF fails to import with a clear
//    message; the scene is saved, reopened, and both lanterns render (the
//    pixels each covers in the scene camera's view).
//    Checks: the session's results (debug fields) on native/romdev/Chromium,
//    the same fields on every host, frames within 0.2% (>16 levels) of
//    native on all four, and natively the files on DISK.
// 2. The same over the debug channel, as an agent works (D6 (d)): native
//    (CW_DEBUG_CMDS) and the editor cart in romdev get the files (`vfs write` / `vfs append` in pieces)
//    and `ed fs.* / import.* / preview.*` requests and must reply alike; the
//    cart's saved scene comes out of `vfs export`.
// Controls that must fail: the session stopped before the drag (its checks
// must fail and its frame differ), a hidden node covers no pixels, the
// corrupt glTF reports an error, fs.place of a missing file is refused.
//
//   node tests/editor-import.mjs [--hosts native,romdev,chromium,wcnative]
import { spawn, spawnSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages, uniqueColors } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'editor-import');
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const HOSTS = argv.includes('--hosts') ? argv[argv.indexOf('--hosts') + 1].split(',') : ['native', 'romdev', 'chromium', 'wcnative'];
const FRAMES = 50;
const port = process.env.ROMDEV_PORT || 7331;
let fail = 0;
const bad = (m) => { fail++; console.log('FAIL ' + m); };

const runCart = (args, env = {}) => new Promise((res) => {
  const p = spawn('node', [join(ROOT, 'tools', 'run-cart.mjs'), ...args], { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...env } });
  let out = '', err = '';
  p.stdout.on('data', (d) => (out += d));
  p.stderr.on('data', (d) => (err += d));
  p.on('close', (code) => res({ code, out, err }));
});
const freshProject = (name) => {
  const dir = join(OUT, name);
  cpSync(join(ROOT, 'editor', 'demo_project'), dir, { recursive: true });
  return dir;
};

// ── 1. the session on every host ─────────────────────────────────────────
const EXPECT = {
  'session.done': 1, 'session.failed': 1, // the one refusal: deleting a texture the glTF uses
  'import_test.copied': 2, 'import_test.listed': 1, 'import_test.import_ok': 1, 'import_test.thumb_ok': 1,
  'import_test.drag_placed': 1, 'import_test.cmd_placed': 1, 'import_test.refused_delete': 1, 'import_test.rename_ok': 1,
  'import_test.broken_error': 1, 'import_test.reload_ok': 1, 'import_test.pixels_hidden': 0,
  'import.ok': 3, 'import.errors': 1, 'import.pending': 0,
};
const MIN = { 'import_test.pixels_drag': 200, 'import_test.pixels_cmd': 100 };
// compared across hosts (reply text names the storage layer, so the reply hash is per host by design)
const SAME = [...Object.keys(EXPECT), ...Object.keys(MIN), 'import_test.broken_hash', 'import.hash', 'import.runs', 'preview.renders',
  'editor.docs', 'editor.nodes', 'editor.doc_hash', 'editor.undo', 'editor.redo', 'editor.dirty', 'editor.selection',
  'editor.layout_hash', 'editor.commands_run', 'editor.command_errors', 'ui.controls', 'ui.drawn', 'ui.popups'];
const checkState = (who, st) => {
  const problems = [];
  for (const [k, v] of Object.entries(EXPECT)) if (st[k] !== v) problems.push(`${who} ${k} = ${st[k]}, expected ${v}`);
  for (const [k, v] of Object.entries(MIN)) if (!(st[k] >= v)) problems.push(`${who} ${k} = ${st[k]}, expected >= ${v}`);
  return problems;
};

const nativeProject = freshProject('native-project');
const results = {};
await Promise.all(HOSTS.map(async (h) => {
  const shot = join(OUT, `${h}.png`), state = join(OUT, `${h}.json`);
  const extra = h === 'wcnative' ? ['--shot', shot] : ['--shot', shot, '--state', state];
  const env = h === 'native' ? { CW_PROJECT: nativeProject } : {};
  const r = await runCart(['--host', h, '--app', 'editor_import', '--frames', String(FRAMES), ...extra], env);
  results[h] = { ...r, shot, state };
}));
for (const h of HOSTS) if (results[h].code !== 0) bad(`${h}: run failed: ${results[h].err.trim().split('\n').slice(-3).join(' | ')}`);
const ref = HOSTS.includes('native') ? 'native' : HOSTS[0];
const refState = existsSync(results[ref]?.state) ? JSON.parse(readFileSync(results[ref].state, 'utf8')) : {};
let refImg = null;
if (results[ref]?.code === 0) {
  refImg = readImage(results[ref].shot);
  if (uniqueColors(refImg) < 200) bad(`${ref} frame has ${uniqueColors(refImg)} colours: not the editor with a rendered scene`);
}
for (const h of HOSTS) {
  if (results[h].code !== 0) continue;
  if (h !== 'wcnative') {
    const st = JSON.parse(readFileSync(results[h].state, 'utf8'));
    for (const p of checkState(h, st)) bad(p);
    for (const k of SAME) if (JSON.stringify(st[k]) !== JSON.stringify(refState[k])) bad(`${h} ${k} = ${st[k]}, ${ref} has ${refState[k]}`);
  }
  if (h !== ref && refImg) {
    const d = diffImages(refImg, readImage(results[h].shot));
    if (!d.comparable) bad(`${h}: ${d.reason}`);
    else if (d.gt16 > 0.002) bad(`${h}: ${(d.gt16 * 100).toFixed(3)}% pixels differ from ${ref} by >16 (max ${d.max})`);
  }
}
// natively the project is on disk
if (HOSTS.includes('native') && results.native.code === 0) {
  const disk = (p) => { try { return readFileSync(join(nativeProject, p), 'utf8'); } catch { return null; } };
  for (const p of ['models/lantern.gltf', 'models/lantern.bin', 'models/paint/lantern_paint.png', 'models/paint/lantern_paint.png.cwimport', 'models/broken.gltf'])
    if (disk(p) === null) bad(`native: ${p} is not on disk in ${nativeProject}`);
  if (disk('models/lantern_paint.png') !== null) bad('native: the renamed texture is still at its old path');
  if (!(disk('models/lantern.gltf') || '').includes('"paint/lantern_paint.png"')) bad('native: the glTF URI did not follow the rename');
  const scene = disk('main.cwscene') || '';
  const m = scene.match(/\[node name="lantern" parent="\." type="MeshInstance3D"\]\nposition = vec3\([^)]*\)\nmesh = res\((0x[0-9a-f]+)\)/);
  const m2 = scene.match(/\[node name="LanternB" parent="Props" type="MeshInstance3D"\]\nposition = vec3\(-2\.4, 0, 1\.2\)\nmesh = res\((0x[0-9a-f]+)\)/);
  if (!m || !m2 || m[1] !== m2[1]) bad('native: main.cwscene on disk lacks the two placed lanterns with the same mesh UID:\n' + scene);
}

// control 1: stopped before the drag, the session's checks must fail and the frame differ
{
  const shot = join(OUT, 'control.png'), state = join(OUT, 'control.json');
  const r = await runCart(['--host', 'native', '--app', 'editor_import', '--frames', String(FRAMES), '--shot', shot, '--state', state],
    { CW_PROJECT: freshProject('control-project'), CW_IMPORT_STOP: '24' });
  if (r.code !== 0) bad('control run failed: ' + r.err);
  else {
    const p = checkState('control', JSON.parse(readFileSync(state, 'utf8')));
    if (!p.length) bad('CONTROL PASSED: a session stopped before the drag met every expectation');
    const d = refImg ? diffImages(refImg, readImage(shot)) : { gt16: 1 };
    if (!(d.gt16 > 0.01)) bad('CONTROL PASSED: the stopped session\'s frame matches the full one');
  }
}
// control 2: the pixel measurement can say zero (a hidden node), checked by EXPECT pixels_hidden = 0 above

// ── 2. the same over the debug channel (agent workflow) ─────────────────
// the files go in as an agent sends them: requests are capped at 4 KiB, so
// each file is written in base64 pieces (vfs write, then vfs append)
const pushes = [];
for (const n of ['lantern.gltf', 'lantern.bin', 'lantern_paint.png', 'broken.gltf']) {
  const b = readFileSync(join(ROOT, 'editor/tests/import', n)).toString('base64');
  for (let o = 0; o < b.length; o += 3000) pushes.push(`vfs ${o ? 'append' : 'write'} models/${n} ${b.slice(o, o + 3000)}`);
}
const requests = [
  ...pushes,
  'ed fs.refresh',
  'ed fs.list dir=models',
  'ed import.info models/lantern.gltf',
  'ed import.info models/broken.gltf',
  'ed preview.render models/lantern.gltf',
  'ed preview.render models/lantern.gltf#material/0',
  'ed preview.render models/lantern.gltf#mesh/0/0',
  'ed fs.select models/lantern.gltf#mesh/0/0',
  'ed fs.place file=models/lantern.gltf#mesh/0/0 position=vec3(-2,0,2)',
  'ed fs.deps models/lantern_paint.png',
  'ed fs.place file=models/lantern.gltf parent=Props',
  'ed fs.place file=models/lantern.gltf name=Second position=vec3(2.5,0,2)',
  'ed import.set models/lantern.gltf mesh.lods 0.5',
  'ed import.info models/lantern.gltf',
  'ed edit.undo scope=editor',
  'ed scene.save',
  'ed preview.node_pixels Props/lantern',
  'ed fs.rename from=models/lantern_paint.png to=models/lantern_tex.png',
  'ed fs.deps models/lantern_tex.png',
];
const mustFail = ['ed fs.place file=models/nothing.gltf', 'ed fs.delete models/lantern.bin', 'ed import.set models/lantern.gltf mesh.bogus 1'];
const all = [...requests, ...mustFail];
const nenv = { ...process.env, CW_FRAMES: '30', SDL_AUDIODRIVER: 'dummy', CW_PROJECT: freshProject('channel-project'), CW_DEBUG_CMDS: all.join(';') };
delete nenv.CW_EDITOR_SETTINGS;
const nr = spawnSync(join(ROOT, 'build/native/editor/editor'), [], { encoding: 'utf8', env: nenv, timeout: 120000 });
const native = (nr.stdout || '').trim().split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));
if (nr.status !== 0 || native.length !== all.length) bad(`native channel run: exit ${nr.status}, ${native.length} replies for ${all.length}`);
let cartReplies = [];
if (HOSTS.includes('romdev')) {
  const S = `cw-editor-import-${process.pid}`;
  const call = async (tool, a) => {
    const res = await fetch(`http://127.0.0.1:${port}/tool/${tool}`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'x-romdev-session': S }, body: JSON.stringify(a) });
    const t = await res.text();
    try { return JSON.parse(t); } catch { return { raw: t }; }
  };
  const cmd = async (text) => (await call('wasm', { op: 'command', text })).reply;
  await call('loadMedia', { platform: 'wasmcart', path: join(ROOT, 'build/cart/carts/editor.wasc'), deterministicSeed: 1 });
  await call('frame', { op: 'step', frames: 29 });
  for (const q of all) cartReplies.push(await cmd(q));
  // the saved scene comes out of the cart's project layer
  const exp = await cmd('vfs export');
  const ar = Buffer.from(exp?.base64 || '', 'base64');
  const files = {};
  for (let p = 7; p < ar.length;) {
    const nl = ar.indexOf(10, p), name = ar.subarray(p, nl).toString();
    const nl2 = ar.indexOf(10, nl + 1), lenTxt = ar.subarray(nl + 1, nl2).toString();
    if (lenTxt === 'removed') { files[name] = null; p = nl2 + 1; continue; }
    files[name] = ar.subarray(nl2 + 1, nl2 + 1 + +lenTxt).toString();
    p = nl2 + 1 + +lenTxt;
  }
  if (!/\[node name="lantern" parent="Props" type="MeshInstance3D"\]\nmesh = res\(0x[0-9a-f]+\)/.test(files['main.cwscene'] || ''))
    bad('romdev: vfs export main.cwscene lacks the placed lantern:\n' + files['main.cwscene']);
  if (!(files['models/lantern.gltf'] || '').includes('"lantern_tex.png"')) bad('romdev: the exported glTF does not name the renamed texture');
  if (!('models/lantern_paint.png' in files) || files['models/lantern_paint.png'] !== null) {
    if (files['models/lantern_paint.png'] !== undefined) bad('romdev: the renamed texture is still exported at its old path');
  }
  await call('host', { op: 'shutdown' });
  const norm = (o) => JSON.stringify(o)?.replace(/"uid":\d+/g, '"uid":0').replace(/"layer":"(disk|memory)"/g, '"layer":"project"')
    .replace(/"texture":\d+/g, '"texture":0').replace(/"changed_files":\d+/, '"changed_files":0').replace(/"files":\d+\}/, '"files":0}')
    .replace(/"to_disk":(true|false)/, '"to_disk":0'); // where a save lands is the host's by design
  all.forEach((q, i) => {
    const a = norm(native[i]), b = norm(cartReplies[i]);
    if (a !== b) bad(`${q.slice(0, 60)}\n  native ${a?.slice(0, 500)}\n  cart   ${b?.slice(0, 500)}`);
  });
}
const rep = (q, src) => src[all.indexOf(q)];
for (const [src, who] of [[native, 'native'], ...(cartReplies.length ? [[cartReplies, 'romdev']] : [])]) {
  requests.forEach((q) => { if (!q.startsWith('vfs') && rep(q, src)?.ok !== true) bad(`${who}: ${q.slice(0, 60)} did not succeed: ${JSON.stringify(rep(q, src))?.slice(0, 300)}`); });
  mustFail.forEach((q) => { if (rep(q, src)?.ok !== false) bad(`CONTROL PASSED (${who}): ${q} should have failed`); });
  if (rep('ed import.info models/lantern.gltf', src)?.status !== 'ok') bad(`${who}: lantern.gltf did not import`);
  const br = rep('ed import.info models/broken.gltf', src);
  if (br?.status !== 'error' || !/accessor 0 reads past its data/.test(br?.error || '')) bad(`${who}: the corrupt glTF did not report a clear import error: ${JSON.stringify(br)}`);
  for (const q of ['ed preview.render models/lantern.gltf', 'ed preview.render models/lantern.gltf#material/0', 'ed preview.render models/lantern.gltf#mesh/0/0'])
    if (!(rep(q, src)?.coverage > 0.05)) bad(`${who}: ${q}: empty thumbnail`);
  if (rep('ed fs.place file=models/lantern.gltf#mesh/0/0 position=vec3(-2,0,2)', src)?.path !== 'lantern_mesh_0_0') bad(`${who}: the sub-mesh was not placed as lantern_mesh_0_0`);
  if (!(rep('ed preview.node_pixels Props/lantern', src)?.pixels > 50)) bad(`${who}: the placed lantern covers no pixels`);
  if (!JSON.stringify(rep('ed fs.deps models/lantern_tex.png', src)?.used_by || []).includes('models/lantern.gltf')) bad(`${who}: the renamed texture lost its user`);
  if (!/fs\.delete|used by/.test(rep('ed fs.delete models/lantern.bin', src)?.error || '')) bad(`${who}: the refused delete does not name its user`);
}
writeFileSync(join(OUT, 'channel.native.json'), JSON.stringify(native, null, 1));
if (cartReplies.length) writeFileSync(join(OUT, 'channel.romdev.json'), JSON.stringify(cartReplies, null, 1));

console.log(fail ? `editor-import: ${fail} failures` : `ok   editor-import (session on ${HOSTS.join(', ')}; ${requests.length} channel requests + ${mustFail.length} must-fail, native vs romdev; controls fail)`);
process.exit(fail ? 1 : 0);
