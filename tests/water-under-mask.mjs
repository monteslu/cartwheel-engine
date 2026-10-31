// The view from under the water knows the wave mask (samples/water_open_sea): a low camera in the bay, over a swell the mask holds down to
// about 0.2 m where the open sea would stand 1.5 m. With the under-water view on or off the picture is the same, at four moments, on GL and
// WebGPU. Control: a camera really under the surface sees under water, so the two pictures differ.
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { diffImages, readImage } from '../tools/lib/png.mjs';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..'), OUT = join(ROOT, 'out/under-mask');
mkdirSync(OUT, { recursive: true });
function shot(gpu, low, frames, off) {
  const f = join(OUT, `${gpu}-${low}-${frames}-${off ? 'off' : 'on'}.png`);
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'water_open_sea', '--frames', String(frames), '--shot', f, '--state', f + '.json', ...(off ? ['--start-cmds', 'set Root/Water underwater false'] : [])], { encoding: 'utf8', env: { ...process.env, WATER_OPEN_SEA_LOW: String(low) }, cwd: ROOT, maxBuffer: 1 << 26 });
  if (r.status !== 0) { console.error(r.stdout.slice(-400), r.stderr.slice(-400)); process.exit(3); }
  return readImage(f);
}
const differ = (a, b) => { const d = diffImages(a, b); if (!d.comparable) { console.error(d.reason); process.exit(3); } return d.gt16; };
let fail = 0;
const check = (name, ok, detail) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}: ${detail}`); if (!ok) fail++; };
for (const gpu of ['gl', 'webgpu']) {
  const d = [90, 150, 210, 270].map((fr) => differ(shot(gpu, 0.75, fr, false), shot(gpu, 0.75, fr, true)));
  check(`${gpu}: over the bay's held-down swell the camera is above the water`, Math.max(...d) < 0.002, `pixels changed by the under-water view: ${d.map((v) => (v * 100).toFixed(2) + '%').join(' ')}`);
  const u = differ(shot(gpu, -2, 90, false), shot(gpu, -2, 90, true));
  check(`${gpu} control: really under the surface it does change the picture`, u > 0.3, `${(u * 100).toFixed(1)}%`);
}
process.exit(fail ? 1 : 0);
