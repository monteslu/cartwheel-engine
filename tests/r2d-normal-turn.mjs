// A turned or flipped normal-mapped sprite is lit by its turned normals, on GL and WebGPU (natively, the test GPU).
// samples/r2d_normal_turn: one sun over a lit layer; each row is a turn (30, 90, 137 and 200 degrees, flip x, flip y,
// both flips, flip x then 57 degrees). In each row:
//   A  a sprite of a tilted flat surface (normal n0), turned and flipped by the row
//   I  the same as a sprite instance (cw_r2d_instances)
//   B  an unturned sprite whose normal map holds n0 turned on the CPU: the answer
//   C  an unturned sprite with n0: what A was lit as before (a turned sprite lit as if unturned)
// Checks, on the mean colour of each square's centre (12 x 12 pixels):
//   A and I match B: at most 3 levels apart in every channel;
//   CONTROL: C differs from B by at least 12 levels in every row (the comparison can see the old fault; a build
//   without the turn fails the A and I checks);
//   GL and WebGPU agree on every square (at most 3 levels).
// Holds the GPU suite lock. Run: node tests/r2d-normal-turn.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
if (!process.env.CW_SUITE_LOCK_HELD) {
  const r = spawnSync('node', [join(ROOT, 'tools/with-gpu-suite.mjs'), 'r2d-normal-turn', 'node', fileURLToPath(import.meta.url), ...process.argv.slice(2)], { stdio: 'inherit' });
  process.exit(r.status ?? 1);
}
const OUT = join(ROOT, 'out', 'gate', 'r2d-normal-turn');
mkdirSync(OUT, { recursive: true });
const ROWS = 8, COLS = { A: 60, I: 140, B: 220, C: 300 };
let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };

function run(gpu) {
  const png = join(OUT, `turn.${gpu}.png`), st = join(OUT, `turn.${gpu}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', 'r2d_normal_turn', '--frames', '3', '--gpu', gpu,
    '--shot', png, '--state', st], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`${gpu}: run failed: ${(r.stderr || r.stdout || '').trim().split('\n').slice(-3).join(' | ')}`);
  return { img: readImage(png), s: JSON.parse(readFileSync(st, 'utf8')) };
}
function mean(img, cx, cy) {
  const k = img.w / 640, x0 = Math.round((cx - 6) * k), y0 = Math.round((cy - 6) * k), n = Math.round(12 * k), m = [0, 0, 0];
  for (let y = y0; y < y0 + n; y++) for (let x = x0; x < x0 + n; x++) for (let c = 0; c < 3; c++) m[c] += img.rgba[(y * img.w + x) * 4 + c];
  return m.map((v) => v / (n * n));
}
const dist = (a, b) => Math.max(...a.map((v, i) => Math.abs(v - b[i])));
const fmt = (m) => m.map((v) => v.toFixed(1)).join(',');

const got = {};
for (const gpu of ['gl', 'webgpu']) {
  const { img, s } = run(gpu);
  check(s['nt.normal_ok'] === 1 && s['nt.rows'] === ROWS, `${gpu}: the normal maps took (normal_ok ${s['nt.normal_ok']}, rows ${s['nt.rows']})`);
  got[gpu] = [];
  for (let k = 0; k < ROWS; k++) {
    const cy = 36 + 40 * k, m = Object.fromEntries(Object.entries(COLS).map(([n, x]) => [n, mean(img, x, cy)]));
    got[gpu].push(m);
    const dA = dist(m.A, m.B), dI = dist(m.I, m.B), dC = dist(m.C, m.B);
    check(dA <= 3, `${gpu} row ${k}: the turned sprite is lit as its turned normals (A ${fmt(m.A)} vs B ${fmt(m.B)}: ${dA.toFixed(1)} levels)`);
    check(dI <= 3, `${gpu} row ${k}: the turned instance is lit as its turned normals (I ${fmt(m.I)} vs B: ${dI.toFixed(1)} levels)`);
    check(dC >= 12, `${gpu} row ${k}: CONTROL, unturned normals (C ${fmt(m.C)}) are ${dC.toFixed(1)} levels from B: the check can see the old lighting`);
  }
}
for (let k = 0; k < ROWS; k++) {
  const d = Math.max(...Object.keys(COLS).map((n) => dist(got.gl[k][n], got.webgpu[k][n])));
  check(d <= 3, `row ${k}: GL and WebGPU agree (${d.toFixed(1)} levels)`);
}
console.log(fail ? `r2d-normal-turn: ${fail} failures` : 'ok   r2d-normal-turn');
process.exit(fail ? 1 : 0);
