// tools/lib/flash.mjs against synthetic runs whose answers are known (no GPU): a moving picture, swells, sparkles and
// a slow pulse must pass; a full-screen strobe, a red strobe on a quarter of the screen, small lights switching
// together over a third of the screen, a small light flickering regularly, a hard onset (with maxRise) and a frozen
// run must each fail, on the measure that should catch it.
// The box filter a cart host's frames go through (boxDown) against sums worked by hand.
// Then a real run when the native samples are built: tools/flash-check.mjs on fx_explosion (its blasts' pooled flash
// lights, smoke and sparks) through CW_FRAME_DUMP must pass, with its control (an 8 Hz strobe over it) failing, and
// the dump must hold every frame asked for.
//   node tests/flash-check.mjs [--synthetic]     (--synthetic: no GPU, the synthetic runs only)
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { analyseFrames, judgeFlash, describeFlash, flashGate, withStrobe, boxDown } from '../tools/lib/flash.mjs';

const W = 192, H = 108, FPS = 60, N = 240;
// a scrolling, high-contrast textured scene: bright and dark stripes and blobs moving 2 px a frame
function scene(t) {
  const f = new Uint8Array(W * H * 3);
  for (let y = 0; y < H; y++)
    for (let x = 0; x < W; x++) {
      const u = x + 2 * t, v = y + t;
      const s = 0.5 + 0.25 * Math.sin(u * 0.21) * Math.cos(v * 0.17) + 0.2 * ((Math.floor(u / 9) + Math.floor(v / 13)) % 2 ? 1 : -1);
      const c = Math.max(0, Math.min(255, Math.round(s * 200)));
      f.set([c, Math.round(c * 0.9), Math.round(c * 0.8)], (y * W + x) * 3);
    }
  return f;
}
const base = Array.from({ length: N }, (_, t) => scene(t));
/** brighten (mix toward a colour) where `where(x, y, t)` gives a weight 0..1 */
const paint = (frames, where, color = [255, 255, 255]) => frames.map((f, t) => {
  const o = Uint8Array.from(f);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const k = where(x, y, t);
    if (k > 0) for (let c = 0; c < 3; c++) { const i = (y * W + x) * 3 + c; o[i] = Math.round(o[i] * (1 - k) + color[c] * k); }
  }
  return o;
});

let bad = 0;
function expect(label, frames, want, opts = {}) {
  const m = analyseFrames(W, H, frames, { fps: FPS, ...opts });
  const why = judgeFlash(m);
  const hit = want === 'pass' ? why.length === 0 : why.some((w) => w.includes(want));
  console.log(`${hit ? 'ok  ' : 'FAIL'} ${label}: want ${want === 'pass' ? 'a pass' : `"${want}"`}; ${describeFlash(m)}${why.length ? `\n       ${why.join('\n       ')}` : ''}`);
  if (!hit) bad++;
}

expect('a moving high-contrast scene', base, 'pass');
// a lightning-like swell: up over 0.15 s, down over 0.6 s, twice, 1.5 s apart, over the whole screen
const swell = (t) => { const s = (t % 90) / FPS; return s < 0.15 ? 0.6 * Math.sin((s / 0.15) * Math.PI / 2) : s < 0.75 ? 0.6 * (1 - (s - 0.15) / 0.6) ** 2 : 0; };
expect('a full-screen swell and fade, every 1.5 s', paint(base, (x, y, t) => swell(t)), 'pass');
expect('a full-screen pulse at 2 Hz', paint(base, (x, y, t) => 0.4 * (0.5 - 0.5 * Math.cos(2 * Math.PI * 2 * t / FPS))), 'pass');
let seed = 7;
const rnd = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296);
const sparks = Array.from({ length: 400 }, () => ({ x: rnd() * W, y: rnd() * H, t: Math.floor(rnd() * N), life: 6 + Math.floor(rnd() * 20) }));
expect('sparkles: 400 small sparks at random times', paint(base, (x, y, t) => sparks.some((s) => t >= s.t && t < s.t + s.life && Math.abs(x - s.x) < 2 && Math.abs(y - s.y) < 2) ? 1 : 0), 'pass');
expect('a full-screen strobe at 8 Hz', withStrobe(base, { hz: 8 }), 'a sixth of the screen:');
expect('a strobe at 5 Hz on a quarter of the screen', paint(base, (x, y, t) => x < W / 2 && y < H / 2 && Math.floor(t * 10 / FPS) % 2 ? 0.7 : 0), 'a sixth of the screen:');
expect('a red strobe at 5 Hz on a quarter of the screen', paint(base.map((f) => f.map((v, i) => (i % 3 ? v >> 3 : v >> 1))), (x, y, t) => x < W / 2 && y < H / 2 && Math.floor(t * 10 / FPS) % 2 ? 0.9 : 0, [255, 0, 0]), 'red:');
expect('dots over 44% of the screen switching together at 6 Hz', paint(base, (x, y, t) => (x % 3 < 2 && y % 3 < 2 && Math.floor(t * 12 / FPS) % 2 ? 0.8 : 0)), '10-degree field together');
expect('a small light (a twelfth by a twelfth) flickering at 8.6 Hz', paint(base, (x, y, t) => x >= 96 && x < 112 && y >= 45 && y < 54 ? 0.4 + 0.4 * Math.sin(2 * Math.PI * 8.6 * t / FPS) : 0), 'regular flicker');
expect('the same small light pulsing at 3 Hz (the bound itself)', paint(base, (x, y, t) => x >= 96 && x < 112 && y >= 45 && y < 54 ? 0.4 + 0.4 * Math.sin(2 * Math.PI * 3 * t / FPS) : 0), 'pass');
expect('the same small light flickering at 4 Hz', paint(base, (x, y, t) => x >= 96 && x < 112 && y >= 45 && y < 54 ? 0.4 + 0.4 * Math.sin(2 * Math.PI * 4 * t / FPS) : 0), 'regular flicker');
expect('the same small light, steady', paint(base, (x, y) => x >= 96 && x < 112 && y >= 45 && y < 54 ? 0.5 : 0), 'pass');
expect('a hard onset, with maxRise 0.3', paint(base, (x, y, t) => (t >= 100 && t < 140 ? 0.8 : 0)), 'a hard onset', { maxRise: 0.3 });
expect('a frozen run', Array.from({ length: N }, () => base[0]), 'proves nothing');

// the gate: a pass whose control fails
const g = flashGate(W, H, base, { fps: FPS }, { label: 'gate on the moving scene' });
console.log(g.lines.join('\n'));
if (!g.ok) { console.log('FAIL the gate did not pass the moving scene with a failing control'); bad++; }
const g2 = flashGate(W, H, withStrobe(base, { hz: 8 }), { fps: FPS }, { label: 'gate on a strobing run' });
if (g2.ok) { console.log('FAIL the gate passed a strobing run'); bad++; } else console.log('ok   the gate fails a strobing run');
// boxDown: a 2x2 block of black and white averages to half the light (188 in sRGB), not half the code (128); a solid
// colour stays itself; a 4x2 frame halves to 2x1
{
  const px = Uint8Array.from([0, 0, 0, 255, 255, 255, 255, 255, 10, 20, 30, 255, 10, 20, 30, 255, 255, 255, 255, 255, 0, 0, 0, 255, 10, 20, 30, 255, 10, 20, 30, 255]);
  const d = boxDown(px, 4, 2, 2);
  const want = [188, 188, 188, 10, 20, 30];
  if (d.w !== 2 || d.h !== 1 || want.some((v, i) => Math.abs(d.rgb[i] - v) > 0)) { console.log(`FAIL boxDown: ${d.w}x${d.h} ${Array.from(d.rgb)}, want 2x1 ${want}`); bad++; }
  else console.log('ok   boxDown averages in linear light (black and white to 188, a solid colour unchanged)');
}
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
if (process.argv.includes('--synthetic')) console.log('note --synthetic: the fx_explosion leg did not run');
else if (existsSync(join(ROOT, 'build/native/samples/fx_explosion'))) {
  const r = spawnSync('node', [join(ROOT, 'tools/flash-check.mjs'), '--host', 'native', '--app', 'fx_explosion', '--frames', '300', '--from', '2'], { encoding: 'utf8' });
  process.stdout.write(r.stdout || '');
  if (r.status !== 0) { console.log(`FAIL fx_explosion's run: ${(r.stderr || '').slice(-1500)}`); bad++; }
  else if (!/299 frames/.test(r.stdout)) { console.log('FAIL fx_explosion: the dump did not hold frames 2-300'); bad++; }
} else console.log('note no native build: the fx_explosion leg did not run');
process.exit(bad ? 1 : 0);
