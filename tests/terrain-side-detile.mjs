// Terrain3D side_detile (samples/terrain_demo): on, the picture changes where the ground is steep; a run repeated changes nothing, so the difference
// is the option's. On GL and WebGPU, natively. Whether it removes a cliff's grid is measured where there are real cliffs (more-cartwheel-examples'
// jetski-plaid: repeat 0.27 to 0.09 on the same wall).
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { diffImages, readImage } from '../tools/lib/png.mjs';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..'), OUT = join(ROOT, 'out/side-detile');
mkdirSync(OUT, { recursive: true });
let n = 0;
function shot(gpu, env) {
  const f = join(OUT, `${gpu}${n++}.png`);
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'terrain_demo', '--frames', '120', '--shot', f, '--state', f + '.json'], { encoding: 'utf8', env: { ...process.env, ...env }, cwd: ROOT, maxBuffer: 1 << 26 });
  if (r.status !== 0) { console.error(r.stdout.slice(-400), r.stderr.slice(-400)); process.exit(3); }
  return readImage(f);
}
let fail = 0;
const check = (name, ok, d) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}: ${d}`); if (!ok) fail++; };
for (const gpu of ['gl', 'webgpu']) {
  const a = shot(gpu, { TERRAIN_MODE: 'fly' }), b = shot(gpu, { TERRAIN_MODE: 'fly' }), c = shot(gpu, { TERRAIN_MODE: 'fly', TERRAIN_DETILE: '1' });
  const same = diffImages(a, b).gt16, on = diffImages(a, c).gt16;
  check(`${gpu} control: the run repeated`, same < 0.002, `${(same * 100).toFixed(3)}% changed`);
  check(`${gpu}: side_detile changes the steep ground`, on > same + 0.002, `${(on * 100).toFixed(2)}% changed`);
}
process.exit(fail ? 1 : 0);
