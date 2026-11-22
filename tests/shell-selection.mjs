// The shell's selection styles (cw_shell_desc.selection, cw_shell_set_selection) on every list the shell draws, natively
// on GL and WebGPU: the title (shell_settings), the Settings screen, the Graphics page, and the join screen's taken
// seats (shell_join). The selected row is found in the COLOUR frame (the default: its text in the accent); then
//   MARKER     accent pixels to the left of the row's text (a caret);
//   UNDERLINE  accent pixels in a band under the row's text (a bar at least half its width);
//   INVERSE    the row's box mostly accent (a plate);
// and each style changes nothing above or below the selected row's band (two text heights; the join screen: outside
// the seats' boxes). COLOUR set by the switch
// (SHELL_SELECTION=0) is the frame with no switch, byte for byte. CONTROL: the COLOUR frame judged as each style fails.
//
//   node tests/shell-selection.mjs [--gpus gl,webgpu]
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';

await takeSuiteLock('shell-selection.mjs');
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'shell-selection');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (n, d) => { const i = argv.indexOf(n); return i >= 0 ? argv[i + 1] : d; };
const GPUS = opt('--gpus', 'gl,webgpu').split(',');

function run(gpu, tag, app, frames, input, env) {
  const shot = join(OUT, `${gpu}-${tag}.png`), state = join(OUT, `${gpu}-${tag}.json`);
  const args = [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', app, '--frames', String(frames), '--gpu', gpu,
    '--env', JSON.stringify(env), '--shot', shot, '--state', state];
  if (input) args.push('--input', join(ROOT, 'tests', input));
  return new Promise((res, rej) => {
    const p = spawn('node', args, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    p.stdout.on('data', (d) => (out += d));
    p.stderr.on('data', (d) => (out += d));
    p.on('close', (code) => {
      if (code !== 0) return rej(new Error(`${app} ${gpu} ${tag} exited ${code}: ${out.slice(-800)}`));
      const s = JSON.parse(readFileSync(state, 'utf8'));
      res({ img: readImage(shot), state: s.state || s });
    });
  });
}
/* the default accent, a warm yellow (1, 0.78, 0.25) */
const accent = (img, i) => Math.abs(img.rgba[i * 4] - 255) <= 8 && Math.abs(img.rgba[i * 4 + 1] - 199) <= 10 && Math.abs(img.rgba[i * 4 + 2] - 64) <= 12;
function bbox(img) {
  let x0 = 1e9, y0 = 1e9, x1 = -1, y1 = -1;
  for (let i = 0; i < img.w * img.h; i++)
    if (accent(img, i)) {
      const x = i % img.w, y = (i / img.w) | 0;
      x0 = Math.min(x0, x), y0 = Math.min(y0, y), x1 = Math.max(x1, x), y1 = Math.max(y1, y);
    }
  if (x1 < 0) return null;
  let cols = 0; /* the columns the selected text covers */
  for (let x = x0; x <= x1; x++) {
    let any = false;
    for (let y = y0; y <= y1 && !any; y++) any = accent(img, y * img.w + x);
    if (any) cols++;
  }
  return { x0, y0, x1, y1, h: y1 - y0 + 1, w: x1 - x0 + 1, cols };
}
function count(img, x0, y0, x1, y1) {
  let n = 0;
  x0 = Math.floor(x0), y0 = Math.floor(y0), x1 = Math.floor(x1), y1 = Math.floor(y1);
  for (let y = Math.max(0, y0); y <= Math.min(img.h - 1, y1); y++)
    for (let x = Math.max(0, x0); x <= Math.min(img.w - 1, x1); x++) if (accent(img, y * img.w + x)) n++;
  return n;
}
/* each style's promise about the selected row b (from the COLOUR frame) */
const JUDGE = {
  marker: (img, b) => { const n = count(img, b.x0 - b.h * 1.2, b.y0, b.x0 - 3, b.y1); return { pass: n >= 30, what: `${n} accent pixels left of the row` }; },
  underline: (img, b) => {
    let best = 0;
    for (let y = b.y1 + 1; y <= b.y1 + b.h * 0.6; y++) best = Math.max(best, count(img, b.x0, y, b.x1, y));
    return { pass: best >= b.cols * 0.8, what: `an accent line of ${best} px under the row (its text covers ${b.cols} columns)` };
  },
  /* the plate: the bands beside the text, left and right, inside the row's height */
  inverse: (img, b) => {
    const m = b.h * 0.3, n = count(img, b.x0 - m, b.y0, b.x0 - 3, b.y1) + count(img, b.x1 + 3, b.y0, b.x1 + m, b.y1);
    const a = 2 * (Math.floor(b.x0 - 3) - Math.floor(b.x0 - m) + 1) * b.h;
    return { pass: n >= a * 0.9, what: `${(n / a * 100).toFixed(0)}% of the bands beside the row's text accent` };
  },
};
/* pixels changed outside the selected row's band (the rows above and below; the join screen: outside the seats) */
function farDiff(a, c, b, band) {
  const m = b.h * 2, lo = band ? band[0] * a.h : b.y0 - m, hi = band ? band[1] * a.h : b.y1 + m;
  let n = 0;
  for (let y = 0; y < a.h; y++)
    for (let x = 0; x < a.w; x++) {
      if (y >= lo && y <= hi) continue;
      const i = (y * a.w + x) * 4;
      if (a.rgba[i] !== c.rgba[i] || a.rgba[i + 1] !== c.rgba[i + 1] || a.rgba[i + 2] !== c.rgba[i + 2]) n++;
    }
  return n;
}
const SCREENS = [
  { name: 'title', app: 'shell_settings', frames: 8 },
  { name: 'settings', app: 'shell_settings', frames: 28, input: 'shell-settings-rows.json' },
  { name: 'graphics', app: 'shell_settings', frames: 44, input: 'shell-graphics-page.json' },
  { name: 'join', app: 'shell_join', frames: 76, input: 'shell-join.json', band: [0.29, 0.71] },
];
let fails = 0;
const check = (c, m) => { console.log(`${c ? 'ok  ' : 'FAIL'} ${m}`); if (!c) fails++; };
for (const gpu of GPUS)
  for (const s of SCREENS) {
    const plain = await run(gpu, `${s.name}-none`, s.app, s.frames, s.input, {});
    const colour = await run(gpu, `${s.name}-colour`, s.app, s.frames, s.input, { SHELL_SELECTION: '0' });
    let same = 0;
    for (let i = 0; i < plain.img.rgba.length; i++) if (plain.img.rgba[i] !== colour.img.rgba[i]) same++;
    const b = bbox(colour.img);
    check(same === 0 && b && colour.state['shell.selection'] === 0,
      `${gpu} ${s.name}: COLOUR by the switch is the default frame (${same} bytes differ); the selected row at ${b ? `${b.x0},${b.y0} ${b.w}x${b.h}` : 'nowhere'} (screen ${colour.state['shell.screen']})`);
    if (!b) continue;
    for (const [k, st] of [['marker', 1], ['underline', 2], ['inverse', 3]]) {
      const r = await run(gpu, `${s.name}-${k}`, s.app, s.frames, s.input, { SHELL_SELECTION: String(st) });
      const j = JUDGE[k](r.img, b), far = farDiff(r.img, colour.img, b, s.band);
      check(j.pass && far === 0 && r.state['shell.selection'] === st, `${gpu} ${s.name} ${k}: ${j.what}; ${far} pixels changed away from the row`);
      const c = JUDGE[k](colour.img, b);
      check(!c.pass, `${gpu} ${s.name} ${k}: control (the COLOUR frame judged the same way): ${c.what}`);
    }
  }
console.log(fails ? `FAIL shell-selection (${fails})` : 'ok   shell-selection');
process.exit(fails ? 1 : 0);
