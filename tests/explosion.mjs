// The explosion kit on GL and WebGPU: fx_explosion.
//   - while it plays: a flash light is lit from the pool, a scorch mark is in use, the shake at the camera is
//     above 0, particles are alive; the pool is the preset's size (Low 2, Medium 4, High 8) and never more
//     lights are lit than the pool holds;
//   - nothing leaks: with a short life, a long time after the blast the frame equals the frame of a run in
//     which the explosion never fired (flash light off, marks faded, particles dead, shake 0);
//     control: the same comparison early (while it plays) must differ widely;
//   - GL and WebGPU draw the same explosion (share of pixels differing by more than 16 levels under 1.5 %);
//   - the flash light swells and falls, no flicker (FXE_TRACE: its energy every frame): it starts under a quarter of
//     its peak, rises frame by frame for at least 3 frames (50 ms), then only falls, to 0 by flash_duration.
//     Control: the same judge on the old curve (the peak on the first frame, times 0.9 + 0.1 sin(70 t)) must fail.
//   node tests/explosion.mjs
import { spawnSync } from 'node:child_process';
import { readFileSync, mkdirSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox();
testEnv(process.env);
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'explosion');
mkdirSync(OUT, { recursive: true });

function run(gpu, name, frames, env) {
  const png = join(OUT, `${name}.${gpu}.png`), st = join(OUT, `${name}.${gpu}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--app', 'fx_explosion', '--frames', String(frames),
    '--gpu', gpu, '--shot', png, '--state', st], { env: { ...process.env, ...env }, encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`${gpu} ${name}: ${(r.stderr || '').trim().split('\n').slice(-2).join(' | ')}`);
  return { img: readImage(png), s: JSON.parse(readFileSync(st, 'utf8')) };
}
const share = (a, b, level) => {
  let n = 0;
  for (let i = 0; i < a.rgba.length; i += 4)
    if (Math.max(Math.abs(a.rgba[i] - b.rgba[i]), Math.abs(a.rgba[i + 1] - b.rgba[i + 1]), Math.abs(a.rgba[i + 2] - b.rgba[i + 2])) > level) n++;
  return n / (a.w * a.h);
};
let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const early = {};
for (const gpu of ['gl', 'webgpu']) {
  const e = run(gpu, 'early', 20, { FXE_SHAKE: '1' });
  early[gpu] = e.img;
  check(e.s['fxe.lit'] >= 1 && e.s['fxe.lit'] <= e.s['fxe.pool'], `${gpu}: a flash light is lit from the pool (${e.s['fxe.lit']} of ${e.s['fxe.pool']})`);
  check(e.s['fxe.scorch'] === 1 && e.s['fxe.shake'] > 0 && e.s['fxe.alive'] > 100 && e.s['fxe.playing'] === 1,
    `${gpu}: scorch ${e.s['fxe.scorch']}, shake ${e.s['fxe.shake'] / 1000}, alive ${e.s['fxe.alive']}, playing ${e.s['fxe.playing']}`);
  for (const [p, n] of [['low', 2], ['medium', 4], ['high', 8]]) {
    const q = run(gpu, `pool_${p}`, 20, { FXE_PRESET: p });
    check(q.s['fxe.pool'] === n && q.s['fxe.lit'] <= n, `${gpu}: ${p} keeps the flash pool at ${n} lights (made ${q.s['fxe.pool']}, lit ${q.s['fxe.lit']})`);
  }
  const never = run(gpu, 'never', 700, { FXE_NEVER: '1' }), late = run(gpu, 'late', 700, { FXE_SHORT: '1' });
  const s2 = share(never.img, late.img, 2), ctrl = share(run(gpu, 'never20', 20, { FXE_NEVER: '1' }).img, e.img, 16);
  check(late.s['fxe.lit'] === 0 && late.s['fxe.scorch'] === 0 && late.s['fxe.alive'] === 0 && late.s['fxe.shake'] === 0, `${gpu}: after its life nothing is lit, marked, alive or shaking`);
  check(s2 < 0.0005, `${gpu}: after the blast the frame equals the never-fired frame (${(s2 * 100).toFixed(4)} % of pixels differ by more than 2 levels)`);
  check(ctrl > 0.05, `${gpu}: CONTROL, while it plays the frame differs from the never-fired one (${(ctrl * 100).toFixed(1)} %): the comparison can see`);
}
// the flash curve: a swell and a fall, nothing that flickers
function judgeFlash(e) {
  const why = [], peak = Math.max(...e), first = e.findIndex((v) => v > 0), top = e.indexOf(peak);
  if (!(peak > 0) || first < 0) return ['no flash at all'];
  if (e[first] > peak * 0.25) why.push(`it starts at ${(e[first] / peak * 100).toFixed(0)} % of its peak (a jump, not a swell)`);
  if (top - first < 2) why.push(`it reaches its peak ${top - first + 1} frame(s) after it starts (under 3 frames: 50 ms)`);
  for (let i = first + 1; i <= top; i++) if (e[i] < e[i - 1]) { why.push(`it dips while rising (frame ${i})`); break; }
  for (let i = top + 1; i < e.length; i++) if (e[i] > e[i - 1] + 1e-6) { why.push(`it rises again while falling (frame ${i}): a flicker`); break; }
  if (e[e.length - 1] !== 0) why.push('it has not ended');
  return why;
}
{
  const log = join(OUT, 'trace.log');
  rmSync(log, { force: true });
  const r = spawnSync('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--app', 'fx_explosion', '--frames', '60', '--gpu', 'gl', '--log', log],
    { env: { ...process.env, FXE_SHORT: '1', FXE_TRACE: '1' }, encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`trace run: ${(r.stderr || '').trim().split('\n').slice(-2).join(' | ')}`);
  const e = [];
  for (const m of readFileSync(log, 'utf8').matchAll(/^fxe\.flash (\d+) ([-\d.]+)$/gm)) e[+m[1]] = +m[2];
  const curve = Array.from({ length: 60 }, (_, i) => e[i + 1] ?? NaN);
  const why = curve.some(Number.isNaN) ? ['the trace missed frames'] : judgeFlash(curve);
  const shown = curve.slice(0, 32).map((v) => v.toFixed(1)).join(' ');
  check(why.length === 0, `the flash swells and falls with no flicker (${why.join('; ') || 'ok'}): ${shown}`);
  // the control: the old curve (the whole flash on the blast's first frame, then (1 - u)^2 x (0.9 + 0.1 sin(70 t))) over the same frames
  const old = curve.map((_, i) => { const age = i / 60, u = age / 0.45; return u >= 1 ? 0 : 120 * (1 - u) ** 2 * (0.9 + 0.1 * Math.sin(age * 70)); });
  const owhy = judgeFlash(old);
  check(owhy.length > 0, `CONTROL: the old flash curve fails the same judge (${owhy.join('; ')})`);
}
const d = share(early.gl, early.webgpu, 16);
check(d < 0.015, `GL and WebGPU draw the same explosion (${(d * 100).toFixed(3)} % of pixels differ by more than 16 levels)`);
console.log(fail ? `explosion: ${fail} failures` : 'ok   explosion');
process.exit(fail ? 1 : 0);
