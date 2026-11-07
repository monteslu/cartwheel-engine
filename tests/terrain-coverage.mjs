// The terrain covers the ground: terrain_demo's cracks run (TERRAIN_MODE=cracks:
// the flight over the terrain with a magenta clear colour, no sky and no
// scatter), natively on GL and on WebGPU. Below the horizon (the frame's
// lower 40%) at most 0.5% of pixels may show the clear colour: a crack
// between patches, or patches not drawn at all. terrain-drive compares hosts
// with each other, so a terrain every host fails to draw passes it; this
// gate looks at what is on screen. (three.c 0.186.12 to 0.186.15 skipped
// every near patch: 88% of these pixels were magenta.)
// Control: the same run with the terrain freed must FAIL the same check.
// The terrain obeys visibility: hidden (its node, or a Node3D above it)
// it draws nothing, so the ground shows the clear colour as when freed (an
// engine whose Terrain3D ignores visible, v0.38.0 and before, fails this);
// shown again mid-run it covers the ground as before.
//
//   node tests/terrain-coverage.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
await takeSuiteLock('terrain-coverage');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate');
mkdirSync(OUT, { recursive: true });
const MAX = 0.005;
let fail = 0;
const say = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} terrain-coverage: ${msg}`); if (!ok) fail++; };

function magenta(gpu, tag, cmds, env = {}) {
  const shot = join(OUT, `terrain-coverage.${gpu}.${tag}.png`);
  const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'terrain_demo', '--frames', '60', '--shot', shot];
  if (cmds) args.push('--start-cmds', cmds);
  const r = spawnSync(process.execPath, args, { env: { ...process.env, TERRAIN_MODE: 'cracks', ...env }, encoding: 'utf8', timeout: 300000 });
  if (r.status !== 0) throw new Error(`${gpu} ${tag}: run-cart failed\n${(r.stdout + r.stderr).slice(-2000)}`);
  const a = readImage(shot);
  let n = 0, t = 0;
  for (let y = Math.floor(a.h * 0.6); y < a.h; y++)
    for (let x = 0; x < a.w; x++, t++) {
      const i = (y * a.w + x) * 4, R = a.rgba[i], G = a.rgba[i + 1], B = a.rgba[i + 2];
      if (R > 150 && B > 150 && G < R - 50) n++; // the magenta clear colour (fogged and tone mapped)
    }
  return n / t;
}

for (const gpu of ['gl', 'webgpu']) {
  const m = magenta(gpu, 'terrain'), c = magenta(gpu, 'freed', 'free /root/World/Terrain');
  say(m <= MAX, `${gpu}: ${(m * 100).toFixed(3)}% of the ground shows the clear colour (at most ${MAX * 100}%)`);
  say(c > MAX, `${gpu}: CONTROL with the terrain freed, ${(c * 100).toFixed(3)}% shows it`);
  const hid = magenta(gpu, 'hidden', 'set /root/World/Terrain visible false');
  // (a Node3D of its own above it: hiding World would hide the camera too, and the clear colour would pass with any terrain)
  const up = magenta(gpu, 'parent-hidden', 'set /root/World/Ground visible false', { TERRAIN_WRAP: '1' });
  const wrapped = magenta(gpu, 'wrapped', '', { TERRAIN_WRAP: '1' });
  const back = magenta(gpu, 'shown-again', 'set /root/World/Terrain visible false;@30 set /root/World/Terrain visible true');
  say(hid > MAX, `${gpu}: the terrain hidden, ${(hid * 100).toFixed(3)}% shows the clear colour (more than ${MAX * 100}%)`);
  say(wrapped <= MAX, `${gpu}: under a Node3D of its own, ${(wrapped * 100).toFixed(3)}% shows the clear colour (at most ${MAX * 100}%)`);
  say(up > MAX, `${gpu}: that Node3D above it hidden, ${(up * 100).toFixed(3)}% shows the clear colour (more than ${MAX * 100}%)`);
  say(back <= MAX, `${gpu}: hidden, then shown again at frame 30, ${(back * 100).toFixed(3)}% shows it (at most ${MAX * 100}%)`);
}
console.log(fail ? `terrain-coverage: ${fail} failures` : 'ok   terrain-coverage');
process.exit(fail ? 1 : 0);
