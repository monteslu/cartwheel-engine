// Distortion stays colourless: no rainbow fringes round heat haze, shockwaves or lenses.
// fx_distort on romdev, GL and WebGPU (the dual cart, its deterministic clock, High as every test run starts), its scene
// turned grey by debug requests (stripes, floor, pole, sky and ground light in greys, so every surface's colour is
// neutral): one frame with every distortion on, one with every strength 0. Moving a grey picture leaves it grey; shifting
// red and blue apart (the old dispersion) paints colour where grey stripes meet. The fringe: pixels the distortion
// changed whose chroma (largest channel less smallest) is over 12 levels above the most the unbent frame shows.
// Checks, per backend:
//   - the distortion bends the picture (enough pixels change: a run that drew none would pass the fringe check);
//   - the fringe is at most 0.5 % of the changed pixels;
//   - CONTROL: the same frame with red and blue moved 3 pixels apart inside the changed footprint fails the same
//     measure (it can see a fringe). `--cart <old fx_distort.wasc>` measures another build: v0.39.1's fails it.
//   node tests/distort-fringe.mjs [--gpu gl|webgpu] [--cart path]
import { spawnSync } from 'node:child_process';
import { mkdirSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'distort-fringe');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const arg = (k) => (argv.includes(`--${k}`) ? argv[argv.indexOf(`--${k}`) + 1] : null);
const gpus = arg('gpu') ? [arg('gpu')] : ['gl', 'webgpu'];
const cart = arg('cart');
const FRAMES = 90;
const OFF = ['set World/Haze distort_strength 0', 'set World/Lens distort_strength 0', 'set World/Wave strength 0'];
const grey = (v) => `color(${v}, ${v}, ${v}, 1)`;
const GREY = [
  ...Array.from({ length: 24 }, (_, i) => `set World/Stripe${i} color ${grey(i % 2 ? 0.85 : 0.12)}`),
  `set World/Floor color ${grey(0.22)}`, `set World/Pole color ${grey(0.5)}`,
  `set World/Env background ${grey(0.06)}`, `set World/Env sky_color ${grey(0.6)}`, `set World/Env ground_color ${grey(0.3)}`,
];

function shot(gpu, name, cmds) {
  const png = join(OUT, `${name}.${gpu}.png`);
  const a = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'romdev', '--app', 'fx_distort', '--frames', String(FRAMES), '--gpu', gpu, '--shot', png];
  if (cmds.length) a.push('--start-cmds', cmds.join(';'));
  if (cart) a.push('--cart', cart);
  const r = spawnSync('node', a, { encoding: 'utf8', maxBuffer: 64 << 20 });
  if (r.status !== 0) throw new Error(`${gpu} ${name}: ${(r.stderr || r.stdout || '').trim().split('\n').slice(-3).join(' | ')}`);
  const img = readImage(png);
  rmSync(png, { force: true });
  return img;
}

const chroma = (p, i) => Math.max(p[i], p[i + 1], p[i + 2]) - Math.min(p[i], p[i + 1], p[i + 2]);
const changedAt = (on, off, i) => Math.max(Math.abs(on.rgba[i] - off.rgba[i]), Math.abs(on.rgba[i + 1] - off.rgba[i + 1]), Math.abs(on.rgba[i + 2] - off.rgba[i + 2])) > 16;
function fringe(on, off) {
  let base = 0;
  for (let i = 0; i < off.rgba.length; i += 4) base = Math.max(base, chroma(off.rgba, i));
  let changed = 0, odd = 0;
  for (let i = 0; i < on.rgba.length; i += 4) {
    if (!changedAt(on, off, i)) continue;
    changed++;
    if (chroma(on.rgba, i) > base + 12) odd++;
  }
  return { changed, odd, base, share: changed ? odd / changed : 0 };
}
// the control: red from 3 pixels right, blue from 3 pixels left, inside the footprint the distortion changed
function split(on, off, k = 3) {
  const { w, h } = on, rgba = Uint8Array.from(on.rgba);
  for (let y = 0; y < h; y++)
    for (let x = k; x < w - k; x++) {
      const i = (y * w + x) * 4;
      if (!changedAt(on, off, i)) continue;
      rgba[i] = on.rgba[i + 4 * k];
      rgba[i + 2] = on.rgba[i + 2 - 4 * k];
    }
  return { w, h, rgba };
}

const LIMIT = 0.005;
let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
for (const gpu of gpus) {
  const on = shot(gpu, 'on', GREY), off = shot(gpu, 'off', [...GREY, ...OFF]);
  const m = fringe(on, off), c = fringe(split(on, off), off);
  check(m.changed > 20000, `${gpu}: the distortion bends the picture (${m.changed} pixels changed)`);
  check(m.share <= LIMIT, `${gpu}: no colour fringe: ${m.odd} of the changed pixels (${(m.share * 100).toFixed(2)} %, at most ${LIMIT * 100} %) are coloured: chroma over ${m.base + 12} levels, the unbent grey frame's most ${m.base}`);
  check(c.share > LIMIT * 4, `${gpu}: CONTROL, red and blue moved 3 pixels apart: ${(c.share * 100).toFixed(2)} % of the changed pixels are fringe (the measure sees one)`);
}
console.log(fail ? `distort-fringe: ${fail} failures` : 'ok   distort-fringe');
process.exit(fail ? 1 : 0);
