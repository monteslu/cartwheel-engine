// What lies just under the water shows through the surface cloudy and fades with depth, by day and much less by night.
//
// samples/water_see_through, seen from straight above: five slabs lie with their tops 0.05, 0.2, 0.5, 1 and 2 m under
// the surface over a floor 8 m down. For each slab the gate reads the mean colour of a 60 px window over it against the
// mean of two windows of open water beside it (the mean difference over the three channels, 0..255), through the real
// frame on romdev, on GL and on WebGPU. It holds:
//   visible    by day the slab at 0.05 m differs by at least 40 and the one at 0.2 m by at least 30
//   fades      the difference falls with depth (0.05 > 0.2 > 0.5), and at 1 m is under a quarter of the 0.05 m one, at 2 m
//              under an eighth: gone by about a metre
//   no shoal   no pixel over the 0.05 m and 0.2 m slabs is white foam (a slab is not a shoal: its depth must not make shore foam)
//   no glare   a bright lit float just under the surface is not blown out to white (it is capped and tinted by the water)
//   night      with a lamp on each slab and the sun gone, the 0.2 m slab differs by under a quarter of its day value
//   dusk       the light reaching the water (cw_water_get_stats see_light) is 1 by day, small at night and between the two never steps
//              by more than 0.45 across a half hour of the sky's clock
// CONTROLS, each of which must break its check or the gate could not see it:
//   refraction false    the surface is opaque, as it was for anything hidden from the refraction: 'visible' fails
//   see_through 0       the old look, the refraction unchanged: 'fades' (a slab at 1 m still shows) and 'no shoal' fail
//   see_night 1         the light ignored, night as bright as day: 'night' fails
//
//   node tests/water-see-through.mjs [--gpus gl,webgpu]
import { spawnSync } from 'node:child_process';
import { mkdirSync, existsSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'water-see-through');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const GPUS = (argv.includes('--gpus') ? argv[argv.indexOf('--gpus') + 1] : 'gl,webgpu').split(',');
process.env.CW_ROMDEV_AGENT = process.env.CW_ROMDEV_AGENT || 'water-see-through-gate';

const TOP = ['set Root/Camera position vec3(0, 14, 0.001)', 'set Root/Camera rotation vec3(-1.5707963, 0, 0)'];
const LAMP = ['P005', 'P02', 'P05', 'P10', 'P20'].map((n) => `set Root/${n} emissive color(0.8, 0.1, 0.06, 1)`);
const PPM = 540 / (14 * Math.tan((25 * Math.PI) / 180)); // pixels per metre at the water, camera 14 m up, fov 50
const SLABS = [[0.05, -6], [0.2, -3], [0.5, 0], [1.0, 3], [2.0, 6]];

function run(gpu, tag, tod, extra = [], frames = 90, state = false) {
  const shot = join(OUT, `${gpu}-${tag}.png`), st = join(OUT, `${gpu}-${tag}.json`);
  const args = ['tools/run-cart.mjs', '--host', 'romdev', '--gpu', gpu, '--app', 'water_see_through', '--frames', String(frames),
    '--start-cmds', [`set Root/Sky time_of_day ${tod}`, ...TOP, ...extra].join(';'), ...(state ? ['--state', st] : ['--shot', shot])];
  const r = spawnSync(process.execPath, args, { cwd: ROOT, env: process.env, encoding: 'utf8', timeout: 600000 });
  if (r.status !== 0) { console.log(`FAIL water-see-through ${gpu} ${tag}: exit ${r.status}: ${(r.stderr || '').slice(-500)}`); process.exit(1); }
  return state ? JSON.parse(readFileSync(st, 'utf8')) : readImage(shot);
}

function win(im, cx, cy, h) {
  const m = [0, 0, 0]; let n = 0, white = 0, peak = 0;
  for (let y = cy - h; y < cy + h; y++) for (let x = cx - h; x < cx + h; x++) {
    const o = (y * im.w + x) * 4, r = im.rgba[o], g = im.rgba[o + 1], b = im.rgba[o + 2];
    m[0] += r; m[1] += g; m[2] += b; n++;
    if (Math.min(r, g, b) >= 140) white++;
    peak = Math.max(peak, Math.min(r, g, b));
  }
  return { mean: m.map((v) => v / n), white: white / n, peak };
}
function slabs(im) {
  const out = {};
  for (const [d, X] of SLABS) {
    const cx = Math.round(960 + X * PPM);
    const p = win(im, cx, 540, 30), a = win(im, cx, Math.round(540 + 1.7 * PPM), 20), b = win(im, cx, Math.round(540 - 1.7 * PPM), 20);
    const ref = a.mean.map((v, i) => (v + b.mean[i]) / 2);
    out[d] = { diff: (Math.abs(p.mean[0] - ref[0]) + Math.abs(p.mean[1] - ref[1]) + Math.abs(p.mean[2] - ref[2])) / 3, white: win(im, cx, 540, 50).white };
  }
  return out;
}
const floatPeak = (im) => win(im, Math.round(960 + 10 * PPM), Math.round(540 - 4 * PPM), 12).peak;

let fail = 0;
const ok = (c, msg) => { console.log(`${c ? 'ok  ' : 'FAIL'} ${msg}`); if (!c) fail++; };
const mustFail = (c, msg) => { console.log(`${c ? 'ok  ' : 'FAIL'} control fails as it must: ${msg}`); if (!c) fail++; };
const f1 = (v) => v.toFixed(1);

for (const gpu of GPUS) {
  const day = slabs(run(gpu, 'day', 12.5));
  const dayIm = readImage(join(OUT, `${gpu}-day.png`));
  const night = slabs(run(gpu, 'night', 0.5, LAMP));
  const tag = `water-see-through ${gpu}`;
  console.log(`${tag}: day ${SLABS.map(([d]) => `${d} m ${f1(day[d].diff)}`).join(', ')}; night ${SLABS.map(([d]) => `${d} m ${f1(night[d].diff)}`).join(', ')}`);
  const visible = (s) => s[0.05].diff >= 40 && s[0.2].diff >= 30;
  const fades = (s) => s[0.05].diff > s[0.2].diff && s[0.2].diff > s[0.5].diff && s[1].diff < s[0.05].diff / 4 && s[2].diff < s[0.05].diff / 8;
  const noShoal = (s) => s[0.05].white < 0.02 && s[0.2].white < 0.02;
  ok(visible(day), `${tag}: just under the surface it shows (0.05 m ${f1(day[0.05].diff)} >= 40, 0.2 m ${f1(day[0.2].diff)} >= 30)`);
  ok(fades(day), `${tag}: it fades with depth (1 m ${f1(day[1].diff)} < ${f1(day[0.05].diff / 4)}, 2 m ${f1(day[2].diff)} < ${f1(day[0.05].diff / 8)})`);
  ok(noShoal(day), `${tag}: no shore foam over a slab (white share ${(day[0.05].white * 100).toFixed(1)}%, ${(day[0.2].white * 100).toFixed(1)}%)`);
  ok(floatPeak(dayIm) < 215, `${tag}: a bright float under the surface is not blown out (weakest channel peak ${floatPeak(dayIm)} < 215)`);
  ok(night[0.2].diff < day[0.2].diff / 4, `${tag}: at night it is far weaker (0.2 m ${f1(night[0.2].diff)} < ${f1(day[0.2].diff / 4)})`);

  const opaque = slabs(run(gpu, 'opaque', 12.5, ['set Root/Water refraction false']));
  mustFail(!visible(opaque), `${tag}: refraction off, 0.05 m ${f1(opaque[0.05].diff)}`);
  const old = run(gpu, 'old', 12.5, ['set Root/Water see_through 0']);
  const oldS = slabs(old);
  mustFail(!fades(oldS), `${tag}: see_through 0, 1 m still ${f1(oldS[1].diff)} (0.05 m ${f1(oldS[0.05].diff)})`);
  mustFail(!noShoal(oldS), `${tag}: see_through 0, white share ${(oldS[0.05].white * 100).toFixed(1)}% over the 0.05 m slab`);
  mustFail(!(floatPeak(old) < 215), `${tag}: see_through 0, the float's weakest channel peaks at ${floatPeak(old)}`);
  const nd = slabs(run(gpu, 'nightday', 0.5, [...LAMP, 'set Root/Water see_night 1']));
  mustFail(!(nd[0.2].diff < day[0.2].diff / 4), `${tag}: see_night 1, 0.2 m ${f1(nd[0.2].diff)} at night against ${f1(day[0.2].diff)} by day`);
}

// the light reaching the water through the day (GL: the number does not depend on the backend)
const gpu0 = GPUS[0];
const series = [];
for (const t of [12.5, 17.75, 18.25, 18.75, 19.25, 19.75, 0.5]) series.push([t, run(gpu0, `light-${t}`, t, [], 20, true)['water.see_light']]);
console.log(`light reaching the water: ${series.map(([t, v]) => `${t}h ${v.toFixed(2)}`).join(', ')}`);
let step = 0;
for (let i = 1; i < series.length; i++) step = Math.max(step, Math.abs(series[i][1] - series[i - 1][1]));
ok(series[0][1] > 0.99 && series[series.length - 1][1] < 0.25, `light: ${series[0][1].toFixed(2)} by day, ${series[series.length - 1][1].toFixed(2)} at night`);
ok(step < 0.45, `light: no step above 0.45 across dusk (largest ${step.toFixed(2)})`);
process.exit(fail ? 1 : 0);
