// A reflection probe that hides objects from its capture (cull_mask) must not take their
// shadows with it (samples/gi_shadow). A shadowed sun lights a crate on a floor; the probe,
// captured every second frame, does not draw the crate. The first render of a frame draws
// the sun's shadow maps, and a capture is that render, so on every capture frame the maps
// lacked the crate and the frame lost its shadow. Checked natively, on GL and WebGPU:
//   1. the shadow is there: dark floor pixels beside the crate, on two consecutive frames,
//      one of which is a capture frame (gi.captures counts them);
//   2. the two frames are the same picture (pixels over 16 levels apart < 0.1 %);
//   control: with the sun's shadow off the dark floor is gone, so check 1 can fail.
//
//   node tests/gi-shadow.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { readImage, diffImages } from '../tools/lib/png.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gi-shadow');
mkdirSync(OUT, { recursive: true });
let failed = false;
const report = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failed = true; };

function run(gpu, frames, tag, startCmds = null) {
  const shot = join(OUT, `${tag}.png`), state = join(OUT, `${tag}.json`);
  const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--gpu', gpu, '--app', 'gi_shadow', '--frames', String(frames), '--shot', shot, '--state', state];
  if (startCmds) args.push('--start-cmds', startCmds);
  const r = spawnSync(process.execPath, args, { encoding: 'utf8' });
  if (r.status !== 0) return { err: (r.stderr || r.stdout).trim().split('\n').slice(-3).join(' | ') };
  return { img: readImage(shot), captures: JSON.parse(readFileSync(state, 'utf8'))['gi.captures'] };
}
// the floor beside the crate in shadow: pixels much darker than the lit floor (the crate is
// orange, the sky grey-blue, the lit floor light grey)
const shadowPixels = (img) => {
  let lit = 0;
  for (let i = 0; i < img.rgba.length; i += 4) lit = Math.max(lit, img.rgba[i]);
  let n = 0;
  for (let i = 0; i < img.rgba.length; i += 4) {
    const r = img.rgba[i], g = img.rgba[i + 1], b = img.rgba[i + 2];
    if (Math.abs(r - g) < 6 && Math.abs(g - b) < 6 && r < lit * 0.65 && r > 30) n++;
  }
  return n / (img.w * img.h);
};

for (const gpu of ['gl', 'webgpu']) {
  const a = run(gpu, 20, `a.${gpu}`), b = run(gpu, 21, `b.${gpu}`);
  if (a.err || b.err) { report(false, `${gpu}: ${a.err || b.err}`); continue; }
  const sa = shadowPixels(a.img), sb = shadowPixels(b.img);
  report(b.captures === a.captures + 1 || a.captures === b.captures + 1, `${gpu}: one of the two frames is a capture frame (${a.captures}, ${b.captures} captures so far)`);
  report(sa > 0.02 && sb > 0.02, `${gpu}: the crate's shadow is on the floor on both frames (${(sa * 100).toFixed(1)}%, ${(sb * 100).toFixed(1)}% of the picture)`);
  const d = diffImages(a.img, b.img);
  report(d.comparable && d.gt16 < 0.001, `${gpu}: the capture frame and its neighbour are the same picture (${(d.gt16 * 100).toFixed(3)}% over 16 levels apart)`);
  const c = run(gpu, 20, `c.${gpu}`, 'set Scene/Sun shadow false');
  if (c.err) { report(false, `${gpu} control: ${c.err}`); continue; }
  const sc = shadowPixels(c.img);
  report(!(sc > 0.02), `${gpu}: control (sun shadow off) has no shadow on the floor (${(sc * 100).toFixed(1)}%)`);
}
process.exit(failed ? 1 : 0);
