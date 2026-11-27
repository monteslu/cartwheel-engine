// Where things go into the water: the line where a shin and a hull cross a moving surface, measured against what real
// water does (a crisp line with little colour change right under it, then a fade whose length the water's clarity
// sets, and no sideways jump at the line), on GL and WebGPU through romdev, at four phases of an open swell.
//
// samples/water_contact: a grey shin 0.12 m across rides a 2.16 m swell, its middle held at the drawn surface; the
// camera follows its contact 3 m out and 2.2 m up (36 degrees down), aimed at the line, so the line is the picture's
// middle row. Each picture has a twin with the water hidden (the same frame): above the line the two agree. Read at
// native resolution down the shin's middle (three columns), with the scale taken from the camera and the shin's
// width checked in the dry picture (0.12 m within 25 %):
//   P1 line step    the shin's luminance 1 cm under the line over 1 cm above it: 0.75 to 1.1 (Fresnel transmission and
//                   a centimetre of water take a few per cent)
//   P2 fade length  the depth over which the shin's contrast against the water beside it falls from 90 % to 10 % of its
//                   value under the line, in cm, within the reference range of the water: murky harbour water 5 to
//                   25 cm, coastal 25 to 60 cm, clear tropical 60 to 150 cm (photographs of legs and hulls in such water)
//   P3 contact jump the shin's middle (from its two edges) 3 to 5 cm under the line against 3 to 5 cm above: under 0.5 cm
//                   (3 cm out: on a wave's flank the line crosses the 12 cm shin on a slant, up to 2 cm higher at one edge)
// The hull (2.8 x 1.1 m, drawing 0.25 m) is read for P1 down its side at the crest.
// Phases: crest, trough, rising and falling at the shin (CW_TEST_WC_PHASE), the swell's clock set for frame 90.
// CONTROLS, each of which must fail or the gate could not see it:
//   (a) CW_TEST_WATER_CONTACT=old + CW_TEST_WATER_REST_CLIP=1, the water before v0.87, at the crest: P1 or P2
//   (b) CW_TEST_WATER_REST_CLIP=1 alone, the refraction clipped at the rest level, at the crest: P1
//   (c) CW_TEST_WATER_CONTACT=offset, the refraction's offset whatever the depth, at the falling phase: P3
//   (d) CW_TEST_WC_NONE=1, no shin: the gate must find no object, not a pass
//   (e) the clear water's see_through on the harbour's clarity: the harbour's P2 range
//
//   node tests/water-contact.mjs [--gpus gl,webgpu] [--measure-only]   (--measure-only: measure the pictures the last run left)
import { spawnSync } from 'node:child_process';
import { mkdirSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'water-contact');
const argv = process.argv.slice(2);
const MEASURE_ONLY = argv.includes('--measure-only');
if (!MEASURE_ONLY) rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
const GPUS = (argv.includes('--gpus') ? argv[argv.indexOf('--gpus') + 1] : 'gl,webgpu').split(',');
process.env.CW_ROMDEV_AGENT = process.env.CW_ROMDEV_AGENT || 'water-contact-gate';

// the views: camera offset (up, out) from the contact it aims at, and the vertical field of view
const VIEWS = { steep: { up: 2.2, out: 3 - 0.06 }, hull: { up: 1.5, out: 4.5 - 0.55 } };
const W = 1920, H = 1080, MID = H / 2, TANH = Math.tan((25 * Math.PI) / 180);
const geo = (v) => {
  const D = Math.hypot(v.up, v.out), pxm = H / (2 * D * TANH), cosP = v.out / D;
  return { pxm, pxcmV: (pxm * cosP) / 100, pxcmH: pxm / 100 }; // px per cm down the object, and across it
};
// the waters: clarity, see_through, the reference fade range (cm)
const WATERS = {
  harbour: { clarity: 2, see: 0.07, range: [5, 25] },
  coastal: { clarity: 5, see: 0.2, range: [25, 60] },
  atoll: { clarity: 9, see: 0.5, range: [60, 150] },
  // (control e: the clear water's see_through on the harbour's clarity, held to the harbour's range)
  harbourClear: { clarity: 2, see: 0.5, range: [5, 25] },
};

let runs = 0;
function shot(gpu, tag, { phase = 'crest', view = 'steep', water = 'coastal', env = {}, dry = false, cmds: more = [] }) {
  const path = join(OUT, `${gpu}-${tag}${dry ? '-dry' : ''}.png`), wt = WATERS[water];
  const cmds = [`set Root/Water clarity ${wt.clarity}`, `set Root/Water see_through ${wt.see}`, ...more, ...(dry ? ['set Root/Water visible false'] : [])];
  const e = { CW_TEST_WC_PHASE: phase, CW_TEST_WC_VIEW: view, ...env };
  if (MEASURE_ONLY) return readImage(path);
  const args = ['tools/run-cart.mjs', '--host', 'romdev', '--gpu', gpu, '--app', 'water_contact', '--frames', '90',
    '--start-cmds', cmds.join(';'), '--env', JSON.stringify(e), '--shot', path];
  const r = spawnSync(process.execPath, args, { cwd: ROOT, env: process.env, encoding: 'utf8', timeout: 600000 });
  runs++;
  if (r.status !== 0) { console.log(`FAIL water-contact ${gpu} ${tag}: run-cart exit ${r.status}: ${(r.stderr || r.stdout || '').slice(-600)}`); process.exit(r.status === 3 ? 3 : 1); }
  return readImage(path);
}
const lum = (im, x, y) => { const o = (y * im.w + x) * 4; return (0.2126 * im.rgba[o] + 0.7152 * im.rgba[o + 1] + 0.0722 * im.rgba[o + 2]) / 255; };
const lum3 = (im, x, y) => (lum(im, x - 1, y) + lum(im, x, y) + lum(im, x + 1, y)) / 3;
const median = (a) => { const s = [...a].sort((p, q) => p - q); return s[s.length >> 1]; };

// the object's two edges on a row: the strongest luminance step (either way) on each side of the middle, sub-pixel by a parabola
function edges(im, y, cx, half) {
  const g = (x) => Math.abs(lum(im, x + 1, y) - lum(im, x - 1, y));
  const find = (a, b) => {
    let best = a, bv = -1;
    for (let x = a; x <= b; x++) { const v = g(x); if (v > bv) bv = v, best = x; }
    const l = g(best - 1), c = bv, r = g(best + 1), den = l - 2 * c + r;
    return { x: best + (den < 0 ? (0.5 * (l - r)) / den : 0), v: bv };
  };
  return [find(Math.round(cx - half * 1.8), Math.round(cx - half * 0.4)), find(Math.round(cx + half * 0.4), Math.round(cx + half * 1.8))];
}

/* the measurements on one pair (with the water, and dry) */
function measure(wet, dry, view, shinWidth) {
  const g = geo(VIEWS[view]), cx = W / 2;
  // the object in the dry picture, 3 cm over the line: its width
  const yA = Math.round(MID - 3 * g.pxcmV);
  const [el, er] = edges(dry, yA, cx, (shinWidth * g.pxm) / 2);
  const widthM = (er.x - el.x) / g.pxm, found = el.v > 0.012 && er.v > 0.012 && Math.abs(widthM / shinWidth - 1) < 0.25;
  if (!found) return { found, widthM };
  const cm = g.pxcmV, above = Math.round(MID - 1 - cm), below = Math.round(MID + 1 + cm);
  const p1 = lum3(wet, cx, below) / Math.max(lum3(wet, cx, above), 1e-3);
  // the shin against the water beside it (two shin widths out each side), row by row down from under the line
  const side = Math.round(shinWidth * g.pxm * 2);
  const C = [];
  for (let y = below; y < Math.min(H - 2, below + Math.round(160 * cm)); y++) {
    let s = 0;
    for (let k = -2; k <= 2; k++) s += Math.abs(lum3(wet, cx, y + k) - (lum3(wet, cx - side, y + k) + lum3(wet, cx + side, y + k)) / 2);
    C.push(s / 5);
  }
  const c0 = median(C.slice(0, 3));
  const i90 = C.findIndex((v) => v <= 0.9 * c0), i10 = C.findIndex((v) => v <= 0.1 * c0);
  const p2 = i90 >= 0 && i10 >= 0 ? (i10 - i90) / cm : i90 >= 0 ? Infinity : NaN;
  // the middle from the two edges: 3 to 5 cm over the line against 3 to 5 cm under it (the mean over the rows)
  const mids = (y0, y1) => { const m = []; for (let y = y0; y <= y1; y++) { const [l, r] = edges(wet, y, cx, (shinWidth * g.pxm) / 2); m.push((l.x + r.x) / 2); } return m.reduce((p, q) => p + q, 0) / m.length; };
  const up = mids(Math.round(MID - 5 * cm), Math.round(MID - 3 * cm)), dn = mids(Math.round(MID + 3 * cm), Math.round(MID + 5 * cm));
  const p3 = Math.abs(dn - up) / g.pxcmH;
  return { found, widthM, p1, p2, p3, c0 };
}

let fail = 0;
const ok = (c, msg) => { console.log(`${c ? 'ok  ' : 'FAIL'} ${msg}`); if (!c) fail++; };
const mustFail = (c, msg) => { console.log(`${c ? 'ok  ' : 'FAIL'} control fails as it must: ${msg}`); if (!c) fail++; };
const f2 = (v) => (Number.isFinite(v) ? v.toFixed(2) : String(v));
const f1 = (v) => (Number.isFinite(v) ? v.toFixed(1) : String(v));
const P1OK = (m) => m.p1 >= 0.75 && m.p1 <= 1.1;
const P2OK = (m, w) => m.p2 >= WATERS[w].range[0] && m.p2 <= WATERS[w].range[1];
const P3OK = (m) => m.p3 < 0.5;

for (const gpu of GPUS) {
  const tag = `water-contact ${gpu}`;
  const dry = {};
  for (const phase of ['crest', 'trough', 'rise', 'fall']) {
    dry[phase] = shot(gpu, phase, { phase, dry: true });
    const m = measure(shot(gpu, phase, { phase }), dry[phase], 'steep', 0.12);
    ok(m.found, `${tag} ${phase}: the shin is found in the dry picture (width ${f2(m.widthM)} m, 0.12)`);
    if (!m.found) continue;
    console.log(`${tag} ${phase}: P1 ${f2(m.p1)}, P2 ${f1(m.p2)} cm, P3 ${f2(m.p3)} cm`);
    ok(P1OK(m), `${tag} ${phase}: P1 line step ${f2(m.p1)} in 0.75..1.1`);
    // (the range where the water's own colour is its body's: in the trough and going down. On and near the crest the
    // crest's lit water is several times brighter than the shin, and takes its contrast sooner: there the fade must
    // still be at least two fifths of the range's start, never a cut)
    if (phase === 'trough' || phase === 'fall') ok(P2OK(m, 'coastal'), `${tag} ${phase}: P2 fade ${f1(m.p2)} cm in coastal ${WATERS.coastal.range.join('..')} cm`);
    else ok(m.p2 >= WATERS.coastal.range[0] * 0.4, `${tag} ${phase}: P2 fade ${f1(m.p2)} cm at least ${WATERS.coastal.range[0] * 0.4} cm (crest light)`);
    ok(P3OK(m), `${tag} ${phase}: P3 contact jump ${f2(m.p3)} cm < 0.5`);
  }
  for (const water of ['harbour', 'atoll']) {
    const m = measure(shot(gpu, `trough-${water}`, { phase: 'trough', water }), dry.trough, 'steep', 0.12);
    console.log(`${tag} trough ${water}: P1 ${f2(m.p1)}, P2 ${f1(m.p2)} cm`);
    ok(m.found && P1OK(m) && P2OK(m, water), `${tag} trough ${water}: P1 ${f2(m.p1)}, P2 ${f1(m.p2)} cm in ${WATERS[water].range.join('..')} cm`);
  }
  {
    const hd = shot(gpu, 'hull', { view: 'hull', dry: true });
    const m = measure(shot(gpu, 'hull', { view: 'hull' }), hd, 'hull', 2.8);
    console.log(`${tag} hull crest: P1 ${f2(m.p1)} (width ${f2(m.widthM)} m)`);
  }
  // controls
  const a = measure(shot(gpu, 'ctl-old', { env: { CW_TEST_WATER_CONTACT: 'old', CW_TEST_WATER_REST_CLIP: '1' } }), dry.crest, 'steep', 0.12);
  mustFail(a.found && !(P1OK(a) && P2OK(a, 'coastal')), `${tag} (a) the water before v0.87 at the crest: P1 ${f2(a.p1)}, P2 ${f1(a.p2)} cm`);
  const b = measure(shot(gpu, 'ctl-restclip', { env: { CW_TEST_WATER_REST_CLIP: '1' } }), dry.crest, 'steep', 0.12);
  mustFail(b.found && !P1OK(b), `${tag} (b) the refraction clipped at the rest level, crest: P1 ${f2(b.p1)}`);
  // P3 again with a strong distortion (a harbour's chop, 0.1): it must hold, and the offset whatever the depth must not
  const D = ['set Root/Water distortion 0.1'];
  const cs = [];
  for (const phase of ['rise', 'fall']) {
    const m = measure(shot(gpu, `${phase}-distort`, { phase, cmds: D }), dry[phase], 'steep', 0.12);
    ok(m.found && P3OK(m), `${tag} ${phase}, distortion 0.1: P3 contact jump ${f2(m.p3)} cm < 0.5`);
    const c = measure(shot(gpu, `ctl-offset-${phase}`, { phase, cmds: D, env: { CW_TEST_WATER_CONTACT: 'offset' } }), dry[phase], 'steep', 0.12);
    cs.push([phase, c]);
  }
  mustFail(cs.some(([, c]) => c.found && !P3OK(c)), `${tag} (c) the offset whatever the depth, distortion 0.1: P3 ${cs.map(([p, c]) => `${p} ${f2(c.p3)} cm`).join(', ')}`);
  const d = measure(shot(gpu, 'ctl-none', { env: { CW_TEST_WC_NONE: '1' } }), shot(gpu, 'ctl-none', { env: { CW_TEST_WC_NONE: '1' }, dry: true }), 'steep', 0.12);
  mustFail(!d.found, `${tag} (d) no shin: found ${d.found} (width ${f2(d.widthM)} m)`);
  const e = measure(shot(gpu, 'ctl-clear-harbour', { phase: 'trough', water: 'harbourClear' }), dry.trough, 'steep', 0.12);
  mustFail(e.found && !P2OK(e, 'harbourClear'), `${tag} (e) clear water's see_through on the harbour: P2 ${f1(e.p2)} cm against 5..25`);
}
console.log(`${runs} runs`);
process.exit(fail ? 1 : 0);
