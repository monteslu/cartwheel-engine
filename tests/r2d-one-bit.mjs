// A game's own HUD in two colours (cw_r2d_set_one_bit, cw_r2d_set_one_bit_paper, cw_prompt_set_two_colour; sample
// r2d_onebit: text with an outline and a shadow of its own, plain text, text with pad prompts, prompt glyphs, shapes
// and a rotated sprite over paper and a one-bit picture), natively and on romdev, GL and WebGPU, at 100% and 200% text:
//   pure       every pixel is the ink or the paper, with the player's Text background off, Outline and Panel, and the
//              screen has ink on it. CONTROL: the setters not called (ONEBIT=none) has other values;
//   readable   each line of the screen (a band of rows with ink in it) has as many ink pixels, within 8%, as the
//              blended picture's coverage there (ONEBIT=ref with CW_TEST_R2D_HARD_OFF=1: the same picture, the outline
//              and shadow drawn in paper by the style, blended edges), so the one-bit raster keeps the strokes.
//              CONTROL: a cut at 0.95 coverage (CW_TEST_R2D_HARD_THIN=1) loses a line;
//   identity   the setters called off every frame (ONEBIT=0) is the frame drawn without them, byte for byte.
//
//   node tests/r2d-one-bit.mjs [--hosts native,romdev] [--gpus gl,webgpu] [--scales 1,2]
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';

const argv = process.argv.slice(2);
const opt = (n, d) => { const i = argv.indexOf(n); return i >= 0 ? argv[i + 1] : d; };
const HOSTS = opt('--hosts', 'native,romdev').split(',');
const GPUS = opt('--gpus', 'gl,webgpu').split(',');
const SCALES = opt('--scales', '1,2').split(',').map(Number);
if (HOSTS.includes('native')) await takeSuiteLock('r2d-one-bit.mjs');
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'r2d-one-bit');
mkdirSync(OUT, { recursive: true });
const INK = [0xf0, 0xe8, 0xc8], PAPER = [0x18, 0x20, 0x28];
const FRAMES = 6;

function run(host, gpu, tag, env, cmds) {
  const shot = join(OUT, `${host}-${gpu}-${tag}.png`), state = join(OUT, `${host}-${gpu}-${tag}.json`);
  const args = [join(ROOT, 'tools/run-cart.mjs'), '--host', host, '--app', 'r2d_onebit', '--frames', String(FRAMES), '--gpu', gpu,
    '--env', JSON.stringify(env), '--shot', shot, '--state', state, '--start-cmds', ['prompt device all pad', ...cmds].join(';')];
  return new Promise((res, rej) => {
    const p = spawn('node', args, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    p.stdout.on('data', (d) => (out += d));
    p.stderr.on('data', (d) => (out += d));
    p.on('close', (code) => {
      if (code !== 0) return rej(new Error(`r2d_onebit ${host} ${gpu} ${tag} exited ${code}: ${out.slice(-800)}`));
      const st = JSON.parse(readFileSync(state, 'utf8'));
      res({ img: readImage(shot), state: st.state || st });
    });
  });
}
const is = (px, i, c) => px[i] === c[0] && px[i + 1] === c[1] && px[i + 2] === c[2];
function purity(img) {
  let other = 0, inks = 0;
  const seen = new Map();
  for (let i = 0; i < img.w * img.h; i++) {
    const o = i * 4;
    if (is(img.rgba, o, INK)) inks++;
    else if (!is(img.rgba, o, PAPER)) {
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
/* the lines: bands of rows with ink in them, left of the picture (the checker would swamp a band), each band's
 * blended coverage and the one-bit frame's ink count there */
function lines(hard, soft) {
  const cov = coverage(soft), w = soft.w, xe = Math.floor(soft.w * 0.6), rows = [];
  for (let y = 0; y < soft.h; y++) {
    let c = 0;
    for (let x = 0; x < xe; x++) c += cov[y * w + x];
    rows.push(c);
  }
  const bands = [];
  for (let y = 0; y < soft.h;) {
    if (rows[y] < 0.5) { y++; continue; }
    let y1 = y;
    while (y1 + 1 < soft.h && rows[y1 + 1] >= 0.5) y1++;
    let c = 0, n = 0;
    for (let yy = y; yy <= y1; yy++)
      for (let x = 0; x < xe; x++) {
        c += cov[yy * w + x];
        if (is(hard.rgba, (yy * w + x) * 4, INK)) n++;
      }
    if (c >= 20) bands.push({ y0: y, y1, cov: c, ink: n, ratio: n / c });
    y = y1 + 1;
  }
  return bands;
}
let fails = 0;
const check = (c, m) => { console.log(`${c ? 'ok  ' : 'FAIL'} ${m}`); if (!c) fails++; };
for (const host of HOSTS)
  for (const gpu of GPUS) {
    for (const scale of SCALES) {
      const t = `${scale * 100}`, sc = [`text scale ${scale}`];
      const on = await run(host, gpu, `${t}-on`, {}, sc);
      const p = purity(on.img);
      check(p.other === 0 && p.inks > 2000 && on.state['onebit.on'] === 1 && on.state['onebit.prompt'] === 1,
        `${host} ${gpu} ${t}%: ${p.other} pixels neither ink nor paper${p.other ? ` (${p.top})` : ''}, ${p.inks} ink, onebit.on ${on.state['onebit.on']}`);
      for (const bd of ['outline', 'panel']) {
        const r = await run(host, gpu, `${t}-${bd}`, {}, [...sc, `text backdrop ${bd}`]);
        const pb = purity(r.img);
        check(pb.other === 0 && pb.inks > 2000, `${host} ${gpu} ${t}% Text background ${bd}: ${pb.other} pixels neither ink nor paper${pb.other ? ` (${pb.top})` : ''}`);
        if (bd === 'panel') {
          const rn = await run(host, gpu, `${t}-${bd}-none`, { ONEBIT: 'none' }, [...sc, `text backdrop ${bd}`]);
          check(purity(rn.img).other > 0, `${host} ${gpu} ${t}% Text background ${bd}: control (the setters not called): ${purity(rn.img).other} other pixels`);
        }
      }
      const none = await run(host, gpu, `${t}-none`, { ONEBIT: 'none' }, sc);
      const pn = purity(none.img);
      check(pn.other > 0, `${host} ${gpu} ${t}%: control (the setters not called): ${pn.other} pixels neither ink nor paper`);
      const soft = await run(host, gpu, `${t}-soft`, { ONEBIT: 'ref', CW_TEST_R2D_HARD_OFF: '1' }, sc);
      const L = lines(on.img, soft.img), bad = L.filter((b) => b.ratio < 0.92 || b.ratio > 1.08);
      check(L.length >= 4 && bad.length === 0, `${host} ${gpu} ${t}%: ${L.length} lines, ink against the blended coverage ${L.map((b) => b.ratio.toFixed(3)).join(' ')}`);
      const thin = await run(host, gpu, `${t}-thin`, { CW_TEST_R2D_HARD_THIN: '1' }, sc);
      const LT = lines(thin.img, soft.img), badT = LT.filter((b) => b.ratio < 0.92 || b.ratio > 1.08);
      check(badT.length > 0, `${host} ${gpu} ${t}%: control (cut at 0.95): ${badT.length} of ${LT.length} lines out of range (${LT.map((b) => b.ratio.toFixed(3)).join(' ')})`);
    }
    const plain = await run(host, gpu, 'plain', { ONEBIT: 'none' }, []), zero = await run(host, gpu, 'zero', { ONEBIT: '0' }, []);
    let diff = 0;
    for (let i = 0; i < plain.img.rgba.length; i++) if (plain.img.rgba[i] !== zero.img.rgba[i]) diff++;
    check(diff === 0 && zero.state['onebit.on'] === 0 && zero.state['onebit.prompt'] === 0,
      `${host} ${gpu}: the setters called off is the frame drawn without them (${diff} bytes differ)`);
  }
console.log(fails ? `FAIL r2d-one-bit (${fails})` : 'ok   r2d-one-bit');
process.exit(fails ? 1 : 0);
