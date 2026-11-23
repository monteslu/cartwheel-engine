// The shell's two-colour mode (cw_shell_desc.two_colour, cw_shell_set_two_colour, cw_shell_set_colours) natively on GL
// and WebGPU, at text sizes 100% and 200% with pad prompts in the hint rows, on the title, pause, Settings, Graphics and
// join screens (samples shell_settings and shell_join, their picture in the same two colours):
//   pure       every pixel is the ink or the paper, on a sheet of paper and over a one-bit picture (SHELL_TWO_BG=1:
//              text on paper plates), and the screen has ink on it. CONTROL: the shell with its mode off
//              (SHELL_TWO_SHELL=0) has other values;
//   readable   each line of the screen (a band of rows with text in it) has as many ink pixels, within 8%, as the
//              blended text's coverage there (CW_TEST_R2D_HARD_OFF=1: the same frame with blended edges), so the one-bit
//              raster keeps the strokes. CONTROL: a cut at 0.95 coverage (CW_TEST_R2D_HARD_THIN=1) loses a line;
//   switch     colours changed at the last frame (cw_shell_set_colours, SHELL_SWITCH_AT) are the frame's only colours
//              and shell.ink publishes them. CONTROL: the picture switches but the shell is not told (SHELL_NO_SETTER=1);
//   identity   the mode set off and the colours to their defaults every tick (SHELL_TWO_COLOUR=0) is the plain frame
//              byte for byte.
//
//   node tests/shell-two-colour.mjs [--gpus gl,webgpu] [--screens title,pause,...] [--scales 1,2]
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';

await takeSuiteLock('shell-two-colour.mjs');
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'shell-two-colour');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (n, d) => { const i = argv.indexOf(n); return i >= 0 ? argv[i + 1] : d; };
const GPUS = opt('--gpus', 'gl,webgpu').split(',');
const SCALES = opt('--scales', '1,2').split(',').map(Number);
const INK = [0xf0, 0xe8, 0xc8], PAPER = [0x18, 0x20, 0x28], INK2 = [0x40, 0xff, 0x90], PAPER2 = [0x00, 0x10, 0x08];

function run(gpu, tag, s, env, scale) {
  const shot = join(OUT, `${gpu}-${tag}.png`), state = join(OUT, `${gpu}-${tag}.json`);
  const args = [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', s.app, '--frames', String(s.frames), '--gpu', gpu,
    '--env', JSON.stringify(env), '--shot', shot, '--state', state, '--start-cmds', `text scale ${scale};prompt device all pad`];
  if (s.input) args.push('--input', join(ROOT, 'tests', s.input));
  return new Promise((res, rej) => {
    const p = spawn('node', args, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    p.stdout.on('data', (d) => (out += d));
    p.stderr.on('data', (d) => (out += d));
    p.on('close', (code) => {
      if (code !== 0) return rej(new Error(`${s.app} ${gpu} ${tag} exited ${code}: ${out.slice(-800)}`));
      const st = JSON.parse(readFileSync(state, 'utf8'));
      res({ img: readImage(shot), state: st.state || st });
    });
  });
}
const is = (px, i, c) => px[i] === c[0] && px[i + 1] === c[1] && px[i + 2] === c[2];
/* pixels that are neither colour, the ink count, and a few of the others (x,y rgb) */
function purity(img, ink, paper) {
  let other = 0, inks = 0;
  const seen = new Map();
  for (let i = 0; i < img.w * img.h; i++) {
    const o = i * 4;
    if (is(img.rgba, o, ink)) inks++;
    else if (!is(img.rgba, o, paper)) {
      other++;
      const k = `${img.rgba[o]},${img.rgba[o + 1]},${img.rgba[o + 2]}`;
      seen.set(k, (seen.get(k) || 0) + 1);
    }
  }
  const top = [...seen].sort((a, b) => b[1] - a[1]).slice(0, 3).map(([k, n]) => `${k} x${n}`).join('; ');
  return { other, inks, top };
}
/* the blended frame's ink coverage at each pixel (its colour projected from paper to ink) */
function coverage(img) {
  const d = INK.map((v, k) => v - PAPER[k]), dd = d.reduce((a, v) => a + v * v, 0), cov = new Float32Array(img.w * img.h);
  for (let i = 0; i < img.w * img.h; i++) {
    let t = 0;
    for (let k = 0; k < 3; k++) t += (img.rgba[i * 4 + k] - PAPER[k]) * d[k];
    cov[i] = Math.min(1, Math.max(0, t / dd));
  }
  return cov;
}
/* the lines on screen: bands of rows with text in them (the blended frame's coverage), each band's coverage and the
 * one-bit frame's ink count there */
function lines(hard, soft) {
  const cov = coverage(soft), w = soft.w, rows = [];
  for (let y = 0; y < soft.h; y++) {
    let c = 0;
    for (let x = 0; x < w; x++) c += cov[y * w + x];
    rows.push(c);
  }
  const bands = [];
  for (let y = 0; y < soft.h;) {
    if (rows[y] < 0.5) { y++; continue; }
    let y1 = y;
    while (y1 + 1 < soft.h && rows[y1 + 1] >= 0.5) y1++;
    let c = 0, n = 0;
    for (let yy = y; yy <= y1; yy++)
      for (let x = 0; x < w; x++) {
        c += cov[yy * w + x];
        if (is(hard.rgba, (yy * w + x) * 4, INK)) n++;
      }
    if (c >= 20) bands.push({ y0: y, y1, cov: c, ink: n, ratio: n / c });
    y = y1 + 1;
  }
  return bands;
}
const SCREENS = [
  { name: 'title', app: 'shell_settings', frames: 8, screen: 1 },
  { name: 'pause', app: 'shell_settings', frames: 44, input: 'shell-two-colour-pause.json', screen: 4 },
  { name: 'settings', app: 'shell_settings', frames: 28, input: 'shell-settings-rows.json', screen: 5 },
  { name: 'graphics', app: 'shell_settings', frames: 44, input: 'shell-graphics-page.json', screen: 5 },
  { name: 'join', app: 'shell_join', frames: 76, input: 'shell-join.json', screen: 2 },
];
const want = opt('--screens', SCREENS.map((s) => s.name).join(',')).split(',');
let fails = 0;
const check = (c, m) => { console.log(`${c ? 'ok  ' : 'FAIL'} ${m}`); if (!c) fails++; };
const TWO = { SHELL_TWO_COLOUR: '1' };
for (const gpu of GPUS)
  for (const s of SCREENS.filter((x) => want.includes(x.name))) {
    for (const scale of SCALES) {
      const t = `${s.name}-${scale * 100}`;
      const two = await run(gpu, `${t}-two`, s, TWO, scale);
      const p = purity(two.img, INK, PAPER);
      check(p.other === 0 && p.inks > 200 && two.state['shell.two_colour'] === 1 && two.state['shell.screen'] === s.screen,
        `${gpu} ${t}: ${p.other} pixels neither ink nor paper${p.other ? ` (${p.top})` : ''}, ${p.inks} ink (screen ${two.state['shell.screen']})`);
      const bg = await run(gpu, `${t}-bg`, s, { ...TWO, SHELL_TWO_BG: '1' }, scale);
      const pb = purity(bg.img, INK, PAPER);
      check(pb.other === 0 && pb.inks > 200, `${gpu} ${t} over a one-bit picture: ${pb.other} pixels neither ink nor paper${pb.other ? ` (${pb.top})` : ''}`);
      const off = await run(gpu, `${t}-off`, s, { ...TWO, SHELL_TWO_SHELL: '0' }, scale);
      const po = purity(off.img, INK, PAPER);
      check(po.other > 0, `${gpu} ${t}: control (the shell's mode off): ${po.other} pixels neither ink nor paper`);
      const soft = await run(gpu, `${t}-soft`, s, { ...TWO, CW_TEST_R2D_HARD_OFF: '1' }, scale);
      const L = lines(two.img, soft.img), bad = L.filter((b) => b.ratio < 0.92 || b.ratio > 1.08);
      check(L.length >= 3 && bad.length === 0, `${gpu} ${t}: ${L.length} lines, ink against the blended coverage ${L.map((b) => b.ratio.toFixed(2)).join(' ')}`);
      const thin = await run(gpu, `${t}-thin`, s, { ...TWO, CW_TEST_R2D_HARD_THIN: '1' }, scale);
      const LT = lines(thin.img, soft.img), badT = LT.filter((b) => b.ratio < 0.92 || b.ratio > 1.08);
      check(badT.length > 0, `${gpu} ${t}: control (cut at 0.95): ${badT.length} of ${LT.length} lines out of range (${LT.map((b) => b.ratio.toFixed(2)).join(' ')})`);
    }
    const t = `${s.name}-100`;
    const sw = await run(gpu, `${t}-switch`, s, { ...TWO, SHELL_SWITCH_AT: String(s.frames - 1) }, 1);
    const ps = purity(sw.img, INK2, PAPER2);
    check(ps.other === 0 && ps.inks > 200 && sw.state['shell.ink'] === 0x40ff90 && sw.state['shell.paper'] === 0x001008,
      `${gpu} ${t}: colours switched at the last frame: ${ps.other} pixels not the new pair, ${ps.inks} new ink, shell.ink ${Number(sw.state['shell.ink']).toString(16)}`);
    const ns = await run(gpu, `${t}-nosetter`, s, { ...TWO, SHELL_SWITCH_AT: String(s.frames - 1), SHELL_NO_SETTER: '1' }, 1);
    const pn = purity(ns.img, INK2, PAPER2);
    check(pn.other > 0, `${gpu} ${t}: control (no cw_shell_set_colours): ${pn.other} pixels not the new pair`);
    const plain = await run(gpu, `${t}-plain`, s, {}, 1), unset = await run(gpu, `${t}-unset`, s, { SHELL_TWO_COLOUR: '0' }, 1);
    let diff = 0;
    for (let i = 0; i < plain.img.rgba.length; i++) if (plain.img.rgba[i] !== unset.img.rgba[i]) diff++;
    check(diff === 0 && unset.state['shell.two_colour'] === 0, `${gpu} ${t}: mode off and default colours by the setters is the plain frame (${diff} bytes differ)`);
  }
console.log(fails ? `FAIL shell-two-colour (${fails})` : 'ok   shell-two-colour');
process.exit(fails ? 1 : 0);
