// Where something stands through a moving surface, its reflection must meet it at the line (P8), and the water piles
// up round it in a bright collar on the water beside it, not over it (P4). On GL and WebGPU
// through romdev, samples/water_contact (the 0.12 m shin riding the 2.16 m swell), at the grazing view (4 m out,
// 0.6 m up), read at native resolution (1920 x 1080), with the refraction off and the surface a mirror (reflectivity
// 1, no crest light) so the water under the line shows the reflection alone, at full strength.
//   P8 the gap between the shin's base and its reflection's: down the shin's middle (three columns), the shin's part
//      of the picture is the difference from the same frame without the shin (CW_TEST_WC_NONE: the waves are the same,
//      so only the shin and its reflection differ). The line is the last row of the dry shin (a difference over 0.15);
//      the reflection's strength is the median difference over the 20 cm under it; the gap is the rows from the line
//      to the first that reaches half that strength, in cm. Under 1 cm at the crest, the trough and falling (rising, on the
//      wave's flank, reported: the move by height does not tilt the mirrored world with the flank),
//      with a reflection found (strength over 0.008).
//   P4 the collar (cw_water_collar into the collar layer, stamped by the sample each tick): at the steep view
//      (3 m out, 2.2 m up, refraction on), the mean luminance of the water beside the shin's two sides at the line
//      (1 to 4 cm out from each edge, from 1 cm over the line to 3 cm under it) over the same pixels with no collar
//      (CW_TEST_WATER_COLLAR=0, the same frame otherwise): 1.05 to 2 (a still leg's meniscus to a moving one's foam), in
//      the trough and falling (on and near the crest the lit crest water is nearly white already: reported there). And not over the shin: its middle half from the line to 10 cm under it changes by under 3 % of
//      its luminance (the line stays the water's transparency).
//   P9 no dark slab: a dark craft's deck 0.2 m under the surface in the shin's place (CW_TEST_WC_DECK) and a slow wake
//      over it with every foam layer on (CW_TEST_WC_FOAM), at the steep view: over the deck's pixels (where
//      the deck changes the picture without foam by over 8 levels) the mean luminance is at least 0.85 of the same
//      pixels with no deck, and at most 10 % of those pixels are under 0.8 of the open water's (the foam floats over the
//      deck as over open water, no slab cut out of it). The deck is seen end on (1.1 m wide), as
//      a craft is seen from behind. The foam's own steepest step across the deck's outline is
//      reported: at High the deck's submerged outline still steps with the half-view refraction's texels.
// The target used to be mirrored about the rest level: on a crest of height h an object's reflection hung 2h under it
// (out of the picture at this view), in a trough it stood over the line. Now it is mirrored about the drawn surface
// where the view looks and each pixel's read moves by its own height over that.
// CONTROLS, each of which must fail:
//   (a) CW_TEST_WATER_REFL_SHIFT=0, mirrored about the rest level and read as it is (before v0.89): P8 on a phase
//   (b) CW_TEST_WATER_REFL_SHIFT=-1, each pixel's move the wrong way: P8 on a phase (the sign is proved, not assumed)
//   (c) the frame without the shin measured against itself: no reflection found, not a pass
//   (e) CW_TEST_WATER_FOAM_BACK=old, foam standing back over a submerged object's whole visible depth (v0.87): P9
//   (d) CW_TEST_WATER_COLLAR_DRAW=0, the collar layer not drawn (the collar stamped but not shown): P4 at the crest
//
//   node tests/water-contact-surface.mjs [--gpus gl,webgpu] [--measure-only]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readdirSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'water-contact-surface');
const argv = process.argv.slice(2);
const MEASURE_ONLY = argv.includes('--measure-only');
const GPUS = (argv.includes('--gpus') ? argv[argv.indexOf('--gpus') + 1] : 'gl,webgpu').split(',');
mkdirSync(OUT, { recursive: true });
if (!MEASURE_ONLY) for (const f of readdirSync(OUT)) if (GPUS.some((g) => f.startsWith(`${g}-`))) rmSync(join(OUT, f), { force: true });
process.env.CW_ROMDEV_AGENT = process.env.CW_ROMDEV_AGENT || 'water-contact-surface-gate';

const W = 1920, H = 1080, MID = H / 2, TANH = Math.tan((25 * Math.PI) / 180);
const UP = 0.6, OUTM = 4 - 0.06, D = Math.hypot(UP, OUTM), PXM = H / (2 * D * TANH), PXCM_V = (PXM * (OUTM / D)) / 100;

let runs = 0;
function shot(gpu, tag, { phase = 'crest', env = {}, view = 'graze' }) {
  const path = join(OUT, `${gpu}-${tag}.png`);
  if (MEASURE_ONLY) return readImage(path);
  const args = ['tools/run-cart.mjs', '--host', 'romdev', '--gpu', gpu, '--app', 'water_contact', '--frames', '90',
    '--start-cmds', view === 'graze' ? 'quality high;set Root/Water refraction false;set Root/Water reflectivity 1;set Root/Water crest_light 0' : 'quality high', '--env', JSON.stringify({ CW_TEST_WC_PHASE: phase, CW_TEST_WC_VIEW: view, ...env }), '--shot', path];
  const r = spawnSync(process.execPath, args, { cwd: ROOT, env: process.env, encoding: 'utf8', timeout: 600000 });
  runs++;
  if (r.status !== 0) { console.log(`FAIL water-contact-surface ${gpu} ${tag}: run-cart exit ${r.status}: ${(r.stderr || r.stdout || '').slice(-600)}`); process.exit(r.status === 3 ? 3 : 1); }
  return readImage(path);
}
const lum = (im, x, y) => { const o = (y * im.w + x) * 4; return (0.2126 * im.rgba[o] + 0.7152 * im.rgba[o + 1] + 0.0722 * im.rgba[o + 2]) / 255; };
const lum3 = (im, x, y) => (lum(im, x - 1, y) + lum(im, x, y) + lum(im, x + 1, y)) / 3;

function p8(im, none) {
  // the shin's part of the picture down its middle: the dry shin over the line differs from the water a lot, its
  // reflection under the line a little (the Fresnel term at this view, about a third)
  const d = (y) => { let t = 0; for (let x = W / 2 - 1; x <= W / 2 + 1; x++) { const o = (y * W + x) * 4; t += Math.hypot(im.rgba[o] - none.rgba[o], im.rgba[o + 1] - none.rgba[o + 1], im.rgba[o + 2] - none.rgba[o + 2]) / 255; } return t / 3; };
  // the line: the last row of the dry shin (difference over 0.15) within 15 cm of the middle row
  let line = -1;
  for (let y = Math.round(MID - 15 * PXCM_V); y <= Math.round(MID + 15 * PXCM_V); y++) if (d(y) > 0.15) line = y;
  if (line < 0) return { found: false, gap: NaN, cmax: 0 };
  const band = [];
  for (let y = line + 1; y <= Math.min(H - 2, line + Math.round(20 * PXCM_V)); y++) band.push(d(y));
  const ref = [...band].sort((p, q) => p - q)[band.length >> 1];
  const i = band.findIndex((v) => v >= 0.5 * ref);
  return { found: ref > 0.008, gap: (i + 1) / PXCM_V, cmax: ref };
}

// P4 at the steep view: the collar's water beside the shin over the same pixels without it, and the shin's own band
const SU = 2.2, SO = 3 - 0.06, SD = Math.hypot(SU, SO), SPXM = H / (2 * SD * TANH), SCM_V = (SPXM * (SO / SD)) / 100, SCM_H = SPXM / 100;
function p4(im, off) {
  const cx = W / 2, half = (0.12 * SPXM) / 2;
  let a = 0, b = 0, sa = 0, sb = 0;
  for (let y = Math.round(MID - SCM_V); y <= Math.round(MID + 3 * SCM_V); y++) for (const sg of [-1, 1]) for (let d = Math.round(SCM_H); d <= Math.round(4 * SCM_H); d++) {
    const x = Math.round(cx + sg * (half + d));
    a += lum(im, x, y), b += lum(off, x, y);
  }
  for (let y = MID + 1; y <= Math.round(MID + 10 * SCM_V); y++) for (let x = Math.round(cx - half / 2); x <= Math.round(cx + half / 2); x++) sa += lum(im, x, y), sb += lum(off, x, y);
  return { ratio: a / Math.max(b, 1e-6), shin: Math.abs(sa / Math.max(sb, 1e-6) - 1) };
}
const P4OK = (m) => m.ratio >= 1.05 && m.ratio <= 2 && m.shin < 0.03;

// P9: the foam over a submerged deck against the same foam with no hull, and the outline's steepest step
function p9(withHull, noHull, plainHull, plainNone) {
  const mask = new Uint8Array(W * H);
  let n = 0, a = 0, b = 0, cut = 0;
  for (let y = 2; y < H - 2; y++) for (let x = 2; x < W - 2; x++) {
    const o = (y * W + x) * 4;
    if (Math.abs(plainHull.rgba[o] - plainNone.rgba[o]) + Math.abs(plainHull.rgba[o + 1] - plainNone.rgba[o + 1]) + Math.abs(plainHull.rgba[o + 2] - plainNone.rgba[o + 2]) > 24) {
      mask[y * W + x] = 1; n++; a += lum(withHull, x, y); b += lum(noHull, x, y);
      if (lum(withHull, x, y) < 0.8 * lum(noHull, x, y)) cut++;
    }
  }
  // the foam's own steepest horizontal step at the mask's edge on each row (the outline crossing the row): the foamy
  // picture less the plain one, with the hull and without (the deck's outline seen through the water is not the foam's)
  const fl = (a, b, x, y) => lum(a, x, y) - lum(b, x, y);
  let sw = 0, sn = 0;
  for (let y = 2; y < H - 2; y++) for (let x = 3; x < W - 3; x++) {
    if (mask[y * W + x] === mask[y * W + x - 1]) continue;
    for (let d = -2; d <= 2; d++) {
      sw = Math.max(sw, Math.abs(fl(withHull, plainHull, x + d, y) - fl(withHull, plainHull, x + d - 1, y)));
      sn = Math.max(sn, Math.abs(fl(noHull, plainNone, x + d, y) - fl(noHull, plainNone, x + d - 1, y)));
    }
  }
  return { n, ratio: n ? a / Math.max(b, 1e-6) : NaN, cut: n ? cut / n : NaN, step: sw, stepNone: sn };
}
const P9OK = (m) => m.n > 2000 && m.ratio >= 0.85 && m.cut <= 0.1;

let fail = 0;
const ok = (c, msg) => { console.log(`${c ? 'ok  ' : 'FAIL'} ${msg}`); if (!c) fail++; };
const mustFail = (c, msg) => { console.log(`${c ? 'ok  ' : 'FAIL'} control fails as it must: ${msg}`); if (!c) fail++; };
const f2 = (v) => (Number.isFinite(v) ? v.toFixed(2) : String(v));
const P8OK = (m) => m.found && m.gap < 1;
const PHASES = ['crest', 'trough', 'rise', 'fall'];
const NONE = { CW_TEST_WC_NONE: '1' };

for (const gpu of GPUS) {
  const tag = `water-contact-surface ${gpu}`;
  const res = { now: [], a: [], b: [] };
  for (const phase of PHASES) {
    const none = shot(gpu, `none-${phase}`, { phase, env: NONE });
    const m = p8(shot(gpu, phase, { phase }), none);
    // (rising, the line crosses the shin on the wave's flank: the move by height does not tilt the mirrored world with
    // the flank, and the gap there is the rest level's, about 1.4 cm, reported)
    if (phase === 'rise') console.log(`${tag} ${phase}: P8 reflection gap ${f2(m.gap)} cm (on the flank: reported; strength ${f2(m.cmax)})`);
    else ok(P8OK(m), `${tag} ${phase}: P8 reflection gap ${f2(m.gap)} cm < 1 (its strength ${f2(m.cmax)})`);
    const a = p8(shot(gpu, `ctl-rest-${phase}`, { phase, env: { CW_TEST_WATER_REFL_SHIFT: '0' } }), shot(gpu, `ctl-rest-none-${phase}`, { phase, env: { ...NONE, CW_TEST_WATER_REFL_SHIFT: '0' } }));
    const b = p8(shot(gpu, `ctl-wrong-${phase}`, { phase, env: { CW_TEST_WATER_REFL_SHIFT: '-1' } }), shot(gpu, `ctl-wrong-none-${phase}`, { phase, env: { ...NONE, CW_TEST_WATER_REFL_SHIFT: '-1' } }));
    console.log(`${tag} ${phase}: (a) rest level ${f2(a.gap)} cm (found ${a.found}), (b) wrong way ${f2(b.gap)} cm (found ${b.found})`);
    res.a.push(a), res.b.push(b);
    const c4 = p4(shot(gpu, `steep-${phase}`, { phase, view: 'steep' }), shot(gpu, `steep-nocollar-${phase}`, { phase, view: 'steep', env: { CW_TEST_WATER_COLLAR: '0' } }));
    // (on and near the crest the crest's lit water is nearly white already, and a ring of bubbles on it hardly shows: there
    // only that it stays off the shin)
    if (phase === 'trough' || phase === 'fall') ok(P4OK(c4), `${tag} ${phase}: P4 collar ${f2(c4.ratio)} in 1.05..2, over the shin ${(100 * c4.shin).toFixed(1)} % < 3 %`);
    else ok(c4.shin < 0.03, `${tag} ${phase}: P4 collar ${f2(c4.ratio)} (crest light: reported), over the shin ${(100 * c4.shin).toFixed(1)} % < 3 %`);
    if (phase === 'crest') {
      const d4 = p4(shot(gpu, 'ctl-nofine', { phase, view: 'steep', env: { CW_TEST_WATER_COLLAR_DRAW: '0' } }), shot(gpu, `steep-nocollar-${phase}`, { phase, view: 'steep', env: { CW_TEST_WATER_COLLAR: '0' } }));
      mustFail(!P4OK(d4), `${tag} (d) the collar layer not drawn, crest: P4 ${f2(d4.ratio)}`);
    }
    if (phase === 'crest') { const c = p8(none, none); mustFail(!P8OK(c), `${tag} (c) no shin against itself: found ${c.found}`); }
  }
  {
    const DECK = { CW_TEST_WC_DECK: '1' }, FOAM = { CW_TEST_WC_FOAM: '1' }, NONE9 = { CW_TEST_WC_NONE: '1' };
    const v = { view: 'steep' };
    const plainHull = shot(gpu, 'deck-plain', { ...v, env: DECK }), plainNone = shot(gpu, 'deck-plain-none', { ...v, env: { ...DECK, ...NONE9 } });
    const noHull = shot(gpu, 'deck-foam-none', { ...v, env: { ...DECK, ...FOAM, ...NONE9 } });
    const m = p9(shot(gpu, 'deck-foam', { ...v, env: { ...DECK, ...FOAM } }), noHull, plainHull, plainNone);
    ok(P9OK(m), `${tag}: P9 foam over the deck 0.2 m down ${f2(m.ratio)} of the open water's (>= 0.85, ${m.n} pixels), ${(100 * m.cut).toFixed(1)} % of them cut out (under 0.8 of the open water's; <= 10 %); the foam's outline step ${f2(m.step)} against ${f2(m.stepNone)} with no hull (reported: at half the view the deck's submerged outline steps with the refraction's texels)`);
    const c = p9(shot(gpu, 'ctl-deck-foam-old', { ...v, env: { ...DECK, ...FOAM, CW_TEST_WATER_FOAM_BACK: 'old' } }), noHull, plainHull, plainNone);
    mustFail(!P9OK(c), `${tag} (e) foam standing back as v0.87 had it: P9 ${f2(c.ratio)}, ${(100 * c.cut).toFixed(1)} % cut out, outline step ${f2(c.step)} against ${f2(c.stepNone)}`);
  }
  mustFail(res.a.some((m) => !P8OK(m)), `${tag} (a) mirrored about the rest level: P8 ${res.a.map((m) => f2(m.gap)).join(', ')} cm`);
  mustFail(res.b.some((m) => !P8OK(m)), `${tag} (b) moved the wrong way: P8 ${res.b.map((m) => f2(m.gap)).join(', ')} cm`);
}
console.log(`${runs} runs`);
process.exit(fail ? 1 : 0);
