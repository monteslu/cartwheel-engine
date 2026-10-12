// Debug channel gate: the same requests over romdev's wasm({op:'command'})
// (cart) and CW_DEBUG_CMDS (native) after the same number of frames must
// give the same replies. Also checks a request that must FAIL fails.
//   node tests/channel.mjs [app] [frames]     ROMDEV_PORT (default 7331)
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const app = process.argv[2] || 'scene3d', frames = +(process.argv[3] || 30);
const port = process.env.ROMDEV_PORT || 7331;
const requests = ['tree /root', 'get /root/Level/Ball position', 'props /root/Level/Floor', 'group crates', 'types',
  'set /root/Level/Floor color color(1, 0, 0)', 'save /root/Level'];
const mustFail = 'get /root/Nope position';

const exe = join(ROOT, 'build/native/samples', app);
const r = spawnSync(exe, [], { encoding: 'utf8', env: { ...process.env, CW_FRAMES: String(frames), CW_DEBUG_CMDS: [...requests, mustFail].join(';') } });
if (r.status !== 0) { console.error('native failed', r.stderr); process.exit(1); }
const native = r.stdout.trim().split('\n').map((l) => JSON.parse(l));

const S = `cw-channel-${process.pid}`;
const call = async (tool, a) => {
  const res = await fetch(`http://127.0.0.1:${port}/tool/${tool}`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'x-romdev-session': S }, body: JSON.stringify(a) });
  return JSON.parse(await res.text());
};
const cart = join(ROOT, 'build/cart/carts', `${app}.wasc`);
if (!existsSync(cart)) { console.error('no cart', cart); process.exit(1); }
await call('loadMedia', { platform: 'wasmcart', path: cart, deterministicSeed: 1 });
// romdev's load ran frame 1; the native run answered after `frames` frames,
// and every command here runs one more frame: pause first so the tree holds still
await call('frame', { op: 'step', frames: frames - 1 });
const out = [];
for (const q of [...requests, mustFail]) out.push((await call('wasm', { op: 'command', text: q })).reply);
await call('host', { op: 'shutdown' });

let fail = 0;
// the native run answers all requests in one frame; the cart answers one per
// frame, so only requests whose answer cannot change between frames compare
// exactly (the scene3d ball moves: compare everything but its position)
requests.forEach((q, i) => {
  if (q.startsWith('get /root/Level/Ball')) return;
  const a = JSON.stringify(native[i]), b = JSON.stringify(out[i]);
  if (q.startsWith('tree')) { // positions of moving nodes differ by a frame
    const strip = (s) => s.replace(/"name":"Ball","type":"MeshInstance3D","uid":\d+,"position":\[[^\]]*\]/, '');
    if (strip(a) !== strip(b)) { fail++; console.log(`FAIL ${q}\n native ${a.slice(0, 300)}\n cart   ${b.slice(0, 300)}`); }
    return;
  }
  if (q.startsWith('save')) { // the moving ball's position line differs by a frame
    const strip = (x) => x.replace(/\\n\\n\[node name=\\"Ball\\"[^\[]*/, '');
    if (strip(a) !== strip(b)) { fail++; console.log(`FAIL ${q}\n native ${strip(a)}\n cart   ${strip(b)}`); }
    return;
  }
  if (a !== b) { fail++; console.log(`FAIL ${q}\n native ${a.slice(0, 300)}\n cart   ${b.slice(0, 300)}`); }
});
const last = out[out.length - 1], nlast = native[native.length - 1];
if (!last?.error || !nlast?.error) { fail++; console.log('CONTROL PASSED: a request for a missing node did not fail'); }
console.log(fail ? `channel: ${fail} failures` : `ok   channel (${requests.length} requests + control, native vs romdev cart)`);
process.exit(fail ? 1 : 0);
