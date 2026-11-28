// The edge of what shows under the surface: does an object's outline under the line follow the object, or the
// refraction target's texel grid? On GL and WebGPU through romdev, samples/water_contact, the shin (0.12 m across)
// leaning 20 degrees sideways in the picture (CW_TEST_WC_TILT: a slanted outline; a vertical one cannot show a
// staircase) on the 2.16 m swell at four phases, read at native resolution (1920 x 1080, no downscale). The planar
// reflection is off: the sky's clouds in it cross the outline, and this asks about the refraction alone.
//
// The measure: down each side of the shin, 2.5 cm to 8 cm under the line (under the slant where the line crosses it
// on a wave's flank), the edge's column on every row (where the colour crosses halfway from the shin's to the water's
// beside it, sub-pixel), a quadratic fitted through those columns, and the jitter: the RMS of the columns about it,
// in pixels. A smooth cylinder's outline is a smooth curve; a target under the view's size read texel by texel puts
// the edge on its texels' columns (at half the view a staircase of 2 px steps along the slant). The same measure on
// the shin's dry part, 2.5 to 8 cm over the line (drawn at full size, aliased by a pixel), is the frame's own floor.
//   pass, at Ultra (the refraction the whole view): on both sides of every phase the submerged jitter at most
//         EDGE_OVER px over the dry outline's (taken as at least EDGE_FLOOR)
//   reported: High (half the view, as before v0.89: the step is still there; a larger target costs 0.66 ms there)
// CONTROLS, each of which must fail:
//   (a) Ultra with CW_TEST_WATER_REFR_SCALE=old: the refraction at the reflection's scale, 0.75 of the view, as Ultra
//       had it before v0.89: over the bound on at least one phase
//   (b) CW_TEST_WC_NONE=1, no shin: no edge found, not a pass
//
//   node tests/water-contact-edge.mjs [--gpus gl,webgpu] [--measure-only] [--cart path]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readdirSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'water-contact-edge');
const argv = process.argv.slice(2);
const MEASURE_ONLY = argv.includes('--measure-only');
const CART = argv.includes('--cart') ? resolve(argv[argv.indexOf('--cart') + 1]) : null;
const GPUS = (argv.includes('--gpus') ? argv[argv.indexOf('--gpus') + 1] : 'gl,webgpu').split(',');
// (only this run's backends' pictures are cleared: a run per backend may share the directory)
mkdirSync(OUT, { recursive: true });
if (!MEASURE_ONLY) for (const f of readdirSync(OUT)) if (GPUS.some((g) => f.startsWith(`${g}-`))) rmSync(join(OUT, f), { force: true });
process.env.CW_ROMDEV_AGENT = process.env.CW_ROMDEV_AGENT || 'water-contact-edge-gate';
const EDGE_FLOOR = 0.25, EDGE_OVER = 0.1, TILT = 20;

// the steep view (tests/water-contact.mjs): 3 m out, 2.2 m up, 50 degrees of field
const W = 1920, H = 1080, MID = H / 2, TANH = Math.tan((25 * Math.PI) / 180);
const UP = 2.2, OUTM = 3 - 0.06, D = Math.hypot(UP, OUTM), PXM = H / (2 * D * TANH), COSP = OUTM / D, PXCM_V = (PXM * COSP) / 100;
// (the lean: a point s down the shin is s sin(tilt) to the side and s cos(tilt) down, the latter foreshortened by the view's pitch)
const SLOPE = Math.tan((TILT * Math.PI) / 180) / COSP; // columns per row

let runs = 0;
function shot(gpu, tag, { phase = 'crest', env = {}, quality = 'high' }) {
  const path = join(OUT, `${gpu}-${tag}.png`);
  if (MEASURE_ONLY) return readImage(path);
  const args = ['tools/run-cart.mjs', '--host', 'romdev', '--gpu', gpu, '--app', 'water_contact', '--frames', '90',
    '--start-cmds', `quality ${quality};set Root/Water reflection false`, '--env', JSON.stringify({ CW_TEST_WC_PHASE: phase, CW_TEST_WC_TILT: String(TILT), ...env }),
    '--shot', path, ...(CART ? ['--cart', CART] : [])];
  const r = spawnSync(process.execPath, args, { cwd: ROOT, env: process.env, encoding: 'utf8', timeout: 600000 });
  runs++;
  if (r.status !== 0) { console.log(`FAIL water-contact-edge ${gpu} ${tag}: run-cart exit ${r.status}: ${(r.stderr || r.stdout || '').slice(-600)}`); process.exit(r.status === 3 ? 3 : 1); }
  return readImage(path);
}
const px = (im, x, y) => { const o = (y * im.w + x) * 4; return [im.rgba[o] / 255, im.rgba[o + 1] / 255, im.rgba[o + 2] / 255]; };
const avg = (cs) => [0, 1, 2].map((c) => cs.reduce((p, q) => p + q[c], 0) / cs.length);
const sub3 = (p, q) => [p[0] - q[0], p[1] - q[1], p[2] - q[2]], dot3 = (p, q) => p[0] * q[0] + p[1] * q[1] + p[2] * q[2];
const mean = (a) => a.reduce((p, q) => p + q, 0) / a.length;

// the RMS of points about their least-squares quadratic x(y)
function quadRms(pts) {
  const my = mean(pts.map((p) => p.y));
  const S = [0, 0, 0, 0, 0], T = [0, 0, 0];
  for (const p of pts) { const t = p.y - my; let tk = 1; for (let k = 0; k < 5; k++) { S[k] += tk; if (k < 3) T[k] += tk * p.x; tk *= t; } }
  const A = [[S[0], S[1], S[2]], [S[1], S[2], S[3]], [S[2], S[3], S[4]]], b = [...T];
  for (let i = 0; i < 3; i++) for (let j = i + 1; j < 3; j++) { const f = A[j][i] / A[i][i]; for (let k = i; k < 3; k++) A[j][k] -= f * A[i][k]; b[j] -= f * b[i]; }
  const c = [0, 0, 0];
  for (let i = 2; i >= 0; i--) { let s = b[i]; for (let k = i + 1; k < 3; k++) s -= A[i][k] * c[k]; c[i] = s / A[i][i]; }
  return Math.sqrt(mean(pts.map((p) => { const t = p.y - my; return (p.x - c[0] - c[1] * t - c[2] * t * t) ** 2; })));
}
// one side of the shin over rows (from the line outward), from about x0 on the first row. The edge on a row is where
// the colour crosses halfway from the shin's (4 to 7 px inside) to the water's (4 to 7 px outside), each pixel's colour
// projected on that axis (the shin's lit side has about the water's luminance, its dark side about the water's red:
// colour, not one channel), sub-pixel by linear interpolation; each row's search follows the row before.
function side(im, rows, x0, left) {
  const pts = [];
  let weak = Infinity, x = x0, first = true;
  const out = left ? -1 : 1;
  for (const y of rows) {
    const g = (k) => Math.hypot(...sub3(px(im, k + 1, y), px(im, k - 1, y)));
    const w = first ? 8 : 3;
    let best = Math.round(x), bv = -1;
    for (let k = Math.round(x) - w; k <= Math.round(x) + w; k++) if (g(k) > bv) bv = g(k), best = k;
    const cin = avg([4, 5, 6, 7].map((dd) => px(im, best - out * dd, y))), cout = avg([4, 5, 6, 7].map((dd) => px(im, best + out * dd, y)));
    const axis = sub3(cin, cout), len2 = Math.max(dot3(axis, axis), 1e-9);
    const u = (k) => dot3(sub3(px(im, k, y), cout), axis) / len2; // 1 the shin, 0 the water
    let xe = best, dbest = Infinity;
    for (let k = best - 3; k < best + 3; k++) {
      const p = u(k) - 0.5, q = u(k + 1) - 0.5;
      if ((p <= 0 && q > 0) || (p >= 0 && q < 0)) { const t = p / (p - q), xc = k + t; if (Math.abs(xc - best) < dbest) dbest = Math.abs(xc - best), xe = xc; }
    }
    pts.push({ x: xe, y });
    weak = Math.min(weak, Math.sqrt(len2));
    x = xe, first = false;
  }
  return { rms: quadRms(pts), weak };
}
function measure(im) {
  const half = (0.12 * PXM) / 2;
  // (the shin's sides on a row y: its middle leans SLOPE columns a row from the contact, which is the middle row)
  const edgeX = (y, left) => W / 2 + (y - MID) * SLOPE + (left ? -1 : 1) * half / Math.cos((TILT * Math.PI) / 180);
  const r = (cm) => Math.round(MID + cm * PXCM_V);
  const range = (a, b, dir) => { const o = []; for (let y = a; dir > 0 ? y <= b : y >= b; y += dir) o.push(y); return o; };
  const down = range(r(2.5), r(8), 1), up = range(r(-2.5), r(-8), -1);
  const sub = [true, false].map((l) => side(im, down, edgeX(down[0], l), l));
  const dry = [true, false].map((l) => side(im, up, edgeX(up[0], l), l));
  // (an edge whose step is under 0.02 of luminance on some row is no edge)
  const found = sub.every((s) => s.weak > 0.02);
  return { found, sub: sub.map((s) => s.rms), dry: dry.map((s) => s.rms), worst: Math.max(...sub.map((s) => s.rms)),
    // (how far each side's jitter under the line is over the same side's above it, which is never taken under EDGE_FLOOR)
    over: Math.max(...sub.map((s, i) => s.rms - Math.max(dry[i].rms, EDGE_FLOOR))) };
}

let fail = 0;
const ok = (c, msg) => { console.log(`${c ? 'ok  ' : 'FAIL'} ${msg}`); if (!c) fail++; };
const mustFail = (c, msg) => { console.log(`${c ? 'ok  ' : 'FAIL'} control fails as it must: ${msg}`); if (!c) fail++; };
const f3 = (v) => (Number.isFinite(v) ? v.toFixed(3) : String(v));
const desc = (m) => `submerged L ${f3(m.sub[0])} R ${f3(m.sub[1])} px, dry L ${f3(m.dry[0])} R ${f3(m.dry[1])} px (found ${m.found})`;
const PHASES = ['crest', 'trough', 'rise', 'fall'];

for (const gpu of GPUS) {
  const tag = `water-contact-edge ${gpu}`;
  for (const phase of PHASES) {
    const m = measure(shot(gpu, `ultra-${phase}`, { phase, quality: 'ultra' }));
    ok(m.found && m.over <= EDGE_OVER, `${tag} ultra ${phase}: ${desc(m)}, over the dry outline ${f3(m.over)} px <= ${EDGE_OVER}`);
  }
  for (const phase of PHASES) {
    const m = measure(shot(gpu, `high-${phase}`, { phase, quality: 'high' }));
    console.log(`${tag} high (half the view, reported) ${phase}: ${desc(m)}, over the dry outline ${f3(m.over)} px`);
  }
  const ctl = PHASES.map((phase) => [phase, measure(shot(gpu, `ctl-old-${phase}`, { phase, quality: 'ultra', env: { CW_TEST_WATER_REFR_SCALE: 'old' } }))]);
  mustFail(ctl.some(([, m]) => m.found && m.over > EDGE_OVER), `${tag} (a) Ultra's refraction as before v0.89 (0.75 of the view): over the dry outline ${ctl.map(([, m]) => f3(m.over)).join(', ')} px, > ${EDGE_OVER} on a phase`);
  const d = measure(shot(gpu, 'ctl-none', { env: { CW_TEST_WC_NONE: '1' } }));
  mustFail(!d.found, `${tag} (b) no shin: found ${d.found}`);
}
console.log(`${runs} runs`);
process.exit(fail ? 1 : 0);
