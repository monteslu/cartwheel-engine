// Automatic exposure (src/render/pipeline_post.c "exposure"): it adapts, it adapts smoothly, and GL and WebGPU
// make the same sequence.
//
// post_stack's last segments (native, every frame captured with CW_FRAME_DUMP): the room adapted (frames 509 to
// 538), its lamps 30 times as bright from frame 539 (the picture jumps up with the lamps, then automatic
// exposure brings it back down), and the lamps as before from frame 629 (it jumps down, then comes back up).
// From each frame's mean display luminance (linear light, 0..1):
//   adapts    by the end of the bright segment the picture is back within 20% of the adapted room's, and by
//             the end of the dim one too (without adaptation it would stay many times brighter, then darker)
//   gradual   the lamps turned up brighten the picture at once (over 1.5 times) and it takes at least 10 frames
//             to come back within 25%
//   smooth    after each jump's own frame the mean never moves by more than 8% of itself from one frame to the
//             next, and keeps one direction until it has settled (a swell, never a flicker)
//   speeds    it comes down from the bright lamps in fewer frames than it comes back up from the dim ones
//             (auto_exposure_speed_up 3 against auto_exposure_speed_down 1)
//   backends  GL and WebGPU agree on every frame's mean within 2%
// CONTROL: CW_TEST_AUTO_EXPOSURE=snap (the state jumps to its goal every frame) must fail "gradual": the picture
// never brightens, the exposure follows the lamps within their own frame. The flash rules themselves are tools/flash-check.mjs's.
//
//   node tests/auto-exposure.mjs [--gpus gl,webgpu]
import { spawn } from 'node:child_process';
import { mkdirSync, existsSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readFrameDump } from '../tools/lib/flash.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { testEnv, wgpuPin } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox();
await takeSuiteLock('auto-exposure.mjs');
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'auto-exposure');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const gi = argv.indexOf('--gpus');
const GPUS = (gi >= 0 ? argv[gi + 1] : 'gl,webgpu').split(',');
const FROM = 520, ROOM = 538, UP = 539, BRIGHT_END = 628, DOWN = 629, END = 778, STEP = 0.08;

function run(gpu, tag, extra) {
  const exe = join(ROOT, 'build/native/samples/post_stack');
  if (!existsSync(exe)) throw new Error('no native build of post_stack (scripts/build.sh post_stack)');
  const dump = join(OUT, `${gpu}-${tag}.cwfd`);
  const env = { ...process.env, CW_GPU: gpu, CW_FRAMES: String(END), CW_FRAME_DUMP: dump, CW_FRAME_DUMP_FROM: String(FROM), SDL_AUDIODRIVER: 'dummy',
    CW_RENDER_SCALE: '1', ...(gpu === 'webgpu' ? wgpuPin().env : {}), ...extra };
  return new Promise((res, rej) => {
    const p = spawn('timeout', ['-s', 'KILL', '300', exe], { env, stdio: ['ignore', 'ignore', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', async (code) => {
      if (code !== 0) return rej(new Error(`post_stack ${gpu} ${tag} exited ${code}: ${err.slice(-800)}`));
      const d = await readFrameDump(dump);
      rmSync(dump, { force: true });
      res(d);
    });
  });
}
const toLin = new Float64Array(256);
for (let i = 0; i < 256; i++) { const c = i / 255; toLin[i] = c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; }
/** each frame's mean luminance in linear light, indexed by frame number */
function means(d) {
  const out = new Map();
  d.frames.forEach((f, k) => {
    let s = 0;
    for (let i = 0; i < f.length; i += 3) s += 0.2126 * toLin[f[i]] + 0.7152 * toLin[f[i + 1]] + 0.0722 * toLin[f[i + 2]];
    out.set(FROM + k, s / (f.length / 3));
  });
  return out;
}
/** the checks on one run's means; returns the problems */
function judge(m) {
  const bad = [], at = (f) => m.get(f), room = at(ROOM);
  const near = (f, what) => { if (Math.abs(at(f) / room - 1) > 0.2) bad.push(`adapts: ${what} the mean is ${at(f).toFixed(4)}, not within 20% of the adapted room's ${room.toFixed(4)}`); };
  near(BRIGHT_END, 'at the end of the bright segment');
  near(END, 'at the end of the dim segment');
  let worst = 0, worstAt = 0, turns = 0;
  for (const [from, to, dir] of [[UP, BRIGHT_END, -1], [DOWN, END, 1]]) {
    for (let f = from + 1; f <= to; f++) {
      const r = at(f) / at(f - 1) - 1;
      if (Math.abs(r) > worst) worst = Math.abs(r), worstAt = f;
      if (r * dir < -0.002) turns++; // a move against the adaptation's direction (beyond rounding)
    }
  }
  if (worst > STEP) bad.push(`smooth: frame ${worstAt} moves the mean by ${(worst * 100).toFixed(1)}% (over ${STEP * 100}%)`);
  if (turns) bad.push(`smooth: the mean turned back ${turns} time(s) while adapting`);
  const settle = (from, to) => { for (let f = from; f <= to; f++) if (Math.abs(at(f) / room - 1) < 0.25) return f - from; return Infinity; };
  const down = settle(UP, BRIGHT_END), up = settle(DOWN, END);
  if (!(at(UP) / at(UP - 1) > 1.5 && down >= 10)) bad.push(`gradual: the lamps turned up multiply the mean by ${(at(UP) / at(UP - 1)).toFixed(2)} and it is back within 25% after ${down} frames`);
  if (!(down < up)) bad.push(`speeds: back within 25% in ${down} frames from the bright lamps, ${up} from the dim ones (the first must be shorter)`);
  return { bad, room, jumpUp: at(UP) / at(UP - 1), jumpDown: at(DOWN) / at(DOWN - 1), worst, worstAt, down, up };
}

let fail = 0;
const series = {};
for (const gpu of GPUS) {
  const m = means(await run(gpu, 'adapt', {}));
  series[gpu] = m;
  const j = judge(m);
  console.log(`${gpu}: the adapted room's mean ${j.room.toFixed(4)}; the lamps turned up multiply it by ${j.jumpUp.toFixed(2)} and turned down by ${j.jumpDown.toFixed(2)}; ` +
    `back within 25% after ${j.down} frames (bright) and ${j.up} frames (dim); largest frame-to-frame move while adapting ${(j.worst * 100).toFixed(2)}% (frame ${j.worstAt})`);
  for (const b of j.bad) { console.log(`FAIL ${gpu}: ${b}`); fail++; }
  const c = judge(means(await run(gpu, 'snap', { CW_TEST_AUTO_EXPOSURE: 'snap' })));
  if (!c.bad.some((b) => b.startsWith('gradual'))) { console.log(`FAIL ${gpu}: CONTROL PASSED: with the state jumping to its goal the run still counts as gradual`); fail++; }
  else console.log(`ok   ${gpu}: control (no adaptation: the goal every frame) fails: ${c.bad.find((b) => b.startsWith('gradual'))}`);
}
if (series.gl && series.webgpu) {
  let worst = 0, at = 0;
  for (const [f, v] of series.gl) { const r = Math.abs(series.webgpu.get(f) / v - 1); if (r > worst) worst = r, at = f; }
  console.log(`GL against WebGPU: the largest difference of a frame's mean is ${(worst * 100).toFixed(2)}% (frame ${at})`);
  if (worst > 0.02) { console.log('FAIL backends: over 2%'); fail++; }
}
console.log(fail ? `FAIL auto-exposure (${fail})` : 'ok   auto-exposure');
process.exit(fail ? 1 : 0);
