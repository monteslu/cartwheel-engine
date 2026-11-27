// Where an object meets the water (cartwheel/waterline.h): a wet band that dries, a meniscus line, caustics under it.
//
// samples/waterline at 1920 x 1080: a grey fabric "shin" 0.12 m across stands in a gentle swell, the camera 3 m away
// and 1.2 m up, 25 degrees down. The gate reads a 3-column strip down the shin's middle at native scale, through the
// real frame on romdev, on GL and on WebGPU, at four phases of the swell. The waterline's row comes from the water's
// height at the shin (debug wl.line) through the camera; centimetres come from the shin's width in the same frame.
// It holds, at every phase:
//   band       over the line the shin is darker than its dry part for 4 to 30 cm (the wet band: the swash it saw plus the
//              wick), between the state's mark (wl.band) and the mark plus the wick, within 2 cm
//   darkens    1 to 4 cm over the line the shin is 0.25 to 0.8 of its dry brightness (fabric, porosity 0.6: Lagarde's
//              albedo factor 0.52 under the sun)
//   meniscus   the brightest row in the first 1.5 cm over the line (at least 2 rows) is at least 1.05 x the wet band
//              2 to 4 cm up
//   caustics   seen from under the water, the submerged shin carries moving lines of light: its strip differs from the
//              same view without the part by at least 3 levels on average and its rows spread more
//   api        apply, get, move and remove on a spare material leave it as it was (wl.api_ok), 3 materials carry it
// CONTROLS, each of which must break its check or the gate could not see it:
//   WATERLINE_OFF=1    no part (the look before it): 'band', 'darkens' and 'meniscus' fail
//   WATERLINE_DRY=1    the mark held at the line: 'band' fails (under 4 cm)
//
//   node tests/waterline.mjs [--gpus gl,webgpu] [--keep]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, writePng } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'waterline');
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const GPUS = (argv.includes('--gpus') ? argv[argv.indexOf('--gpus') + 1] : 'gl,webgpu').split(',');
process.env.CW_ROMDEV_AGENT = process.env.CW_ROMDEV_AGENT || 'waterline-gate';
const W = 1920, H = 1080, FOV = 50, CAM = [0, 1.2, 3], PITCH = 0.4363323, R = 0.06;
// four phases of the 46 m swell (period about 326 ticks): a quarter apart
const PHASES = [300, 381, 462, 543];

function run(gpu, tag, frames, env = {}, cmds = []) {
  const shot = join(OUT, `${gpu}-${tag}.png`), st = join(OUT, `${gpu}-${tag}.json`);
  const args = ['tools/run-cart.mjs', '--host', 'romdev', '--gpu', gpu, '--app', 'waterline', '--frames', String(frames), '--shot', shot, '--state', st, '--start-cmds', ['quality high', ...cmds].join(';')];
  if (Object.keys(env).length) args.push('--env', JSON.stringify(env));
  const r = spawnSync(process.execPath, args, { cwd: ROOT, env: process.env, encoding: 'utf8', timeout: 600000 });
  if (r.status !== 0) { console.log(`FAIL waterline ${gpu} ${tag}: exit ${r.status}: ${(r.stderr || r.stdout || '').slice(-600)}`); process.exit(1); }
  return { im: readImage(shot), st: JSON.parse(readFileSync(st, 'utf8')) };
}

// a world point to a pixel (x right, y down)
function project([x, y, z], cam = CAM, pitch = PITCH) {
  const v = [x - cam[0], y - cam[1], z - cam[2]];
  const c = Math.cos(pitch), s = Math.sin(pitch);
  const yc = v[1] * c - v[2] * s, zc = v[1] * s + v[2] * c;
  const t = Math.tan((FOV * Math.PI) / 360);
  return [(1 + v[0] / -zc / (t * (W / H))) * W / 2, (1 - yc / -zc / t) * H / 2];
}
const lum = (im, x, y) => { const o = (y * im.w + x) * 4; return 0.2126 * im.rgba[o] + 0.7152 * im.rgba[o + 1] + 0.0722 * im.rgba[o + 2]; };
const rowLum = (im, cx, y) => (lum(im, cx - 1, y) + lum(im, cx, y) + lum(im, cx + 1, y)) / 3;

// the shin's width in pixels on a row (grey against the water behind it): the run of near-grey pixels through the middle
function widthAt(im, cx, y) {
  const grey = (x) => { const o = (y * im.w + x) * 4, r = im.rgba[o], g = im.rgba[o + 1], b = im.rgba[o + 2]; return Math.max(r, g, b) - Math.min(r, g, b) < 18; };
  let a = cx, b = cx;
  while (a > cx - 200 && grey(a - 1)) a--;
  while (b < cx + 200 && grey(b + 1)) b++;
  return b - a + 1;
}

function measure(im, st) {
  const line = st['wl.line'];
  const [cxf, ly] = project([0, line, R]);
  const cx = Math.round(cxf), lineRow = Math.round(ly);
  // pixels per centimetre up the shin at the line (through the camera), and the shin's width 35 cm up as a check of the
  // projection (its 12 cm in the frame)
  const pxPerCm = project([0, line, R])[1] - project([0, line + 0.01, R])[1];
  const [, y35] = project([0, line + 0.35, R]);
  const widthCm = widthAt(im, cx, Math.round(y35)) / ((project([0.06, line + 0.35, R])[0] - project([-0.06, line + 0.35, R])[0]) / 12);
  const row = (cm) => Math.round(project([0, line + cm / 100, R])[1]);
  let dry = 0, n = 0;
  for (let y = row(45); y <= row(35); y++) dry += rowLum(im, cx, y), n++;
  dry /= n;
  // the band: up from 1 cm over the line while the strip is under 0.9 x dry (a 2-row gap allowed)
  let top = row(1), gap = 0;
  for (let y = row(1); y > row(45); y--) {
    if (rowLum(im, cx, y) < 0.9 * dry) { top = y; gap = 0; } else if (++gap > 2) break;
  }
  const bandCm = (lineRow - top) / pxPerCm;
  let wet = 0; n = 0;
  for (let y = row(4); y <= row(1); y++) wet += rowLum(im, cx, y), n++;
  wet /= n;
  let band24 = 0; n = 0;
  for (let y = row(4); y <= row(2); y++) band24 += rowLum(im, cx, y), n++;
  band24 /= n;
  let men = 0;
  const menRows = Math.max(2, Math.round(1.5 * pxPerCm));
  for (let y = lineRow - menRows; y < lineRow; y++) men = Math.max(men, rowLum(im, cx, y));
  return { cx, lineRow, pxPerCm, widthCm, dry, bandCm, darken: wet / dry, meniscus: men / band24, row };
}
// the submerged shin seen from under the water (camera UCAM, level): the strip from 15 to 45 cm under the rest level, its
// mean and the spread of its row means (the caustics' moving lines)
const UCAM = [0, -0.3, 1.0];
function under(im) {
  const r0 = Math.round(project([0, -0.15, R], UCAM, 0)[1]), r1 = Math.round(project([0, -0.45, R], UCAM, 0)[1]);
  const cx = Math.round(project([0, -0.3, R], UCAM, 0)[0]), rows = [];
  for (let y = r0; y <= r1; y++) rows.push(rowLum(im, cx, y));
  const mean = rows.reduce((a, b) => a + b, 0) / rows.length;
  return { rows, mean, sd: Math.sqrt(rows.reduce((a, b) => a + (b - mean) ** 2, 0) / rows.length) };
}
function crop(im, m, file) {
  const w = 240, h = 360, x0 = m.cx - w / 2, y0 = m.lineRow - 240, px = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const sx = Math.min(im.w - 1, Math.max(0, x0 + x)), sy = Math.min(im.h - 1, Math.max(0, y0 + y));
    for (let k = 0; k < 4; k++) px[(y * w + x) * 4 + k] = im.rgba[(sy * im.w + sx) * 4 + k];
  }
  writePng(file, w, h, px);
}

let fail = 0;
const ok = (c, msg) => { console.log(`${c ? 'ok  ' : 'FAIL'} ${msg}`); if (!c) fail++; };
const mustFail = (c, msg) => { console.log(`${c ? 'ok  ' : 'FAIL'} control fails as it must: ${msg}`); if (!c) fail++; };
const f2 = (v) => v.toFixed(2);
const bandOk = (m) => m.bandCm >= 4 && m.bandCm <= 30;
const darkOk = (m) => m.darken >= 0.25 && m.darken <= 0.8;
const menOk = (m) => m.meniscus >= 1.05;

for (const gpu of GPUS) {
  for (const f of PHASES) {
    const tag = `waterline ${gpu} t${f}`;
    const on = run(gpu, `on-${f}`, f), m = measure(on.im, on.st);
    crop(on.im, m, join(OUT, `${gpu}-on-${f}-crop.png`));
    const debugCm = on.st['wl.band'] * 100;
    console.log(`${tag}: line row ${m.lineRow}, ${f2(m.pxPerCm)} px/cm (shin ${f2(m.widthCm)} cm wide), dry ${f2(m.dry)}, band ${f2(m.bandCm)} cm (debug ${f2(debugCm)}), darken ${f2(m.darken)}, meniscus ${f2(m.meniscus)}`);
    if (f === PHASES[0]) ok(on.st['wl.api_ok'] === 1 && on.st['wl.materials'] === 3, `${tag}: api (api_ok ${on.st['wl.api_ok']}, materials ${on.st['wl.materials']})`);
    ok(bandOk(m), `${tag}: a wet band over the line, ${f2(m.bandCm)} cm (4 to 30)`);
    // (the state's band is the mark over the line; the picture adds up to the fabric's wick, 0.12 m x porosity 0.6, less
    // the soft drying top that the 0.9 x dry threshold does not count)
    ok(m.bandCm >= debugCm - 2 && m.bandCm <= debugCm + 7.2 + 2, `${tag}: the picture's band ${f2(m.bandCm)} cm lies between the state's mark ${f2(debugCm)} cm and the mark plus the 7.2 cm wick, within 2`);
    ok(darkOk(m), `${tag}: 1 to 4 cm over the line it is ${f2(m.darken)} of dry (0.25 to 0.8)`);
    ok(menOk(m), `${tag}: a meniscus line at the contact, ${f2(m.meniscus)} x the band over it (>= 1.05)`);
    const off = run(gpu, `off-${f}`, f, { WATERLINE_OFF: '1' }), mo = measure(off.im, off.st);
    crop(off.im, mo, join(OUT, `${gpu}-off-${f}-crop.png`));
    console.log(`${tag} OFF: band ${f2(mo.bandCm)} cm, darken ${f2(mo.darken)}, meniscus ${f2(mo.meniscus)}`);
    mustFail(!bandOk(mo) && !darkOk(mo) && !menOk(mo), `${tag}: without the part (band ${f2(mo.bandCm)} cm, darken ${f2(mo.darken)}, meniscus ${f2(mo.meniscus)})`);
    if (f === PHASES[1]) {
      const dr = run(gpu, `dry-${f}`, f, { WATERLINE_DRY: '1' }), md = measure(dr.im, dr.st);
      mustFail(!bandOk(md), `${tag}: the mark held at the line, band ${f2(md.bandCm)} cm`);
    }
  }
  // caustics on the submerged part, seen from under the water: the strip's rows vary (moving lines of light) and differ
  // from the same view without the part
  const ucmd = [`set Root/Camera position vec3(${UCAM.join(', ')})`, 'set Root/Camera rotation vec3(0, 0, 0)'];
  const uo = under(run(gpu, 'under-on', PHASES[0], {}, ucmd).im), uf = under(run(gpu, 'under-off', PHASES[0], { WATERLINE_OFF: '1' }, ucmd).im);
  const dU = uo.rows.reduce((s, v, i) => s + Math.abs(v - uf.rows[i]), 0) / uo.rows.length;
  console.log(`waterline ${gpu} under: mean ${f2(uo.mean)} (without ${f2(uf.mean)}), row spread ${f2(uo.sd)} (without ${f2(uf.sd)}), ${f2(dU)} levels apart`);
  ok(dU >= 3 && uo.sd >= uf.sd + 1, `waterline ${gpu}: caustics on the submerged shin (${f2(dU)} levels from without the part, >= 3; row spread ${f2(uo.sd)} over ${f2(uf.sd)} + 1)`);
}
if (!argv.includes('--keep')) for (const gpu of GPUS) for (const f of PHASES) { for (const t of ['on', 'off', 'dry']) rmSync(join(OUT, `${gpu}-${t}-${f}.png`), { force: true }); for (const t of ['under-on', 'under-off']) rmSync(join(OUT, `${gpu}-${t}.png`), { force: true }); }
process.exit(fail ? 1 : 0);
