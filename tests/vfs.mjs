// Gate: edit a project inside a CART, carry it out, land it on DISK.
//   1. romdev runs the project_edit cart; over the debug channel we spawn a
//      light, set a property, `project save` the level into the project layer
//      and `vfs export` the changes (one base64 archive).
//   2. A native run imports that archive with CW_PROJECT=<dir> set, so the
//      file is written to disk; we compare it with the cart's text.
//   3. Control: an archive with one byte flipped must not import as the same file.
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, mkdirSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const port = process.env.ROMDEV_PORT || 7331;
const S = `cw-vfs-${process.pid}`;
const call = async (tool, a) => {
  const res = await fetch(`http://127.0.0.1:${port}/tool/${tool}`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'x-romdev-session': S }, body: JSON.stringify(a) });
  return JSON.parse(await res.text());
};
const cmd = async (text) => {
  const r = await call('wasm', { op: 'command', text });
  if (!r.reply || r.reply.error) throw new Error(`${text}: ${JSON.stringify(r)}`);
  return r.reply;
};
await call('loadMedia', { platform: 'wasmcart', path: join(ROOT, 'build/cart/carts/project_edit.wasc'), deterministicSeed: 1 });
await cmd('pause 1');
await cmd('spawn OmniLight3D /root/Level Lamp');
await cmd('set /root/Level/Lamp position vec3(0, 4, 0)');
await cmd('set /root/Level/Lamp energy 3');
await cmd('project save /root/Level levels/edited.cwscene');
const exp = await cmd('vfs export');
await call('host', { op: 'shutdown' });
const archive = Buffer.from(exp.base64, 'base64');
const marker = archive.indexOf('levels/edited.cwscene\n');
const lenEnd = archive.indexOf('\n', marker + 22);
const fileLen = +archive.slice(marker + 22, lenEnd).toString();
const cartText = archive.slice(lenEnd + 1, lenEnd + 1 + fileLen).toString();
let fail = 0;
if (!cartText.includes('[node name="Lamp" parent="." type="OmniLight3D"]') || !cartText.includes('energy = 3')) {
  fail++; console.log('FAIL: the exported scene does not contain the edit:\n' + cartText);
}

// native import (CW_DEBUG_CMDS has no 4 KB cap; a cart takes big files with `vfs append` in pieces)
const runImport = (b64) => {
  const dir = mkdtempSync(join(ROOT, 'out', 'proj-'));
  const r = spawnSync(join(ROOT, 'build/native/samples/project_edit'), [], {
    encoding: 'utf8', env: { ...process.env, CW_FRAMES: '2', CW_PROJECT: dir, CW_DEBUG_CMDS: [`vfs import ${b64}`].join(';') },
  });
  return { dir, r };
};
const { dir, r } = runImport(exp.base64);
if (r.status !== 0) { fail++; console.log('native failed', r.stderr); }
let diskText = '';
try { diskText = readFileSync(join(dir, 'levels/edited.cwscene'), 'utf8'); } catch (e) { fail++; console.log('FAIL: no file on disk', e.message); }
if (diskText !== cartText) { fail++; console.log('FAIL: disk file differs from the cart export'); }

// control: a corrupted archive must not reproduce the file
const bad = Buffer.from(archive);
bad[lenEnd + 5] ^= 0x20;
const ctl = runImport(bad.toString('base64'));
let ctlText = '';
try { ctlText = readFileSync(join(ctl.dir, 'levels/edited.cwscene'), 'utf8'); } catch {}
if (ctlText === cartText) { fail++; console.log('CONTROL PASSED: a corrupted archive produced the same file'); }

console.log(fail ? `vfs: ${fail} failures` : `ok   vfs (cart edit -> export ${archive.length} bytes -> native disk, identical; control differs)`);
process.exit(fail ? 1 : 0);
