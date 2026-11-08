// Does a run strobe? Every frame of a native run (or frames already captured) checked against the photosensitive
// flash rules (WCAG 2.2 success criterion 2.3.1 and the Harding test's thresholds; tools/lib/flash.mjs has them and
// the measures). Each check also runs its CONTROL, which must fail: the same frames with an 8 Hz full-screen strobe.
//
//   node tools/flash-check.mjs --app <app> --frames N [--from K] [--input script.json] [--start-cmds "a;b"]
//        [--gpu gl|webgpu] [--scale 8] [--keep run.cwfd] [--label text]
//                         runs the app natively for N frames on the fixed 1/60 s clock (tools/run-cart.mjs, with
//                         its input script and start requests, as a gate gives them) and checks frames K..N (default:
//                         from frame 2), box-filtered down by --scale. The environment passes through (a game's own
//                         variables). Takes the GPU suite lock. CW_PROJECT_ROOT: an app of a games repository
//   node tools/flash-check.mjs --dump run.cwfd    checks a CW_FRAME_DUMP file (src/platform/native.c)
//   node tools/flash-check.mjs --dir frames/      checks a directory of PNG frames, one a frame, in name order
// Bounds (defaults in tools/lib/flash.mjs FLASH_DEFAULTS): --max-rise X (bound the hardest two-frame rise of a sixth of
// the screen, off by default), --flicker-amp X, --max-flashes N, --fps N. --json FILE writes the numbers.
// Exit 0: the run passes and the control fails; 1 otherwise.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { flashGate, readFrameDump, readPngFrames } from './lib/flash.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const arg = (k, d) => (argv.includes(`--${k}`) ? argv[argv.indexOf(`--${k}`) + 1] : d);
const opts = {};
if (arg('max-rise')) opts.maxRise = +arg('max-rise');
if (arg('flicker-amp')) opts.flickerAmp = +arg('flicker-amp');
if (arg('max-flashes')) opts.maxFlashes = +arg('max-flashes');
if (arg('fps')) opts.fps = +arg('fps');

let data, label = arg('label');
if (arg('dump')) data = await readFrameDump(resolve(arg('dump'))), label ||= arg('dump');
else if (arg('dir')) data = await readPngFrames(resolve(arg('dir'))), label ||= arg('dir');
else if (arg('app')) {
  const app = arg('app'), frames = +arg('frames', 0), from = +arg('from', 2), gpu = arg('gpu', 'gl');
  if (!(frames > from)) { console.error('flash-check: --frames N (more than --from) is required with --app'); process.exit(2); }
  const PROJ = process.env.CW_PROJECT_ROOT ? resolve(process.env.CW_PROJECT_ROOT) : ROOT;
  const keep = arg('keep');
  const dir = join(PROJ, 'out', 'flash');
  mkdirSync(dir, { recursive: true });
  const dump = keep ? resolve(keep) : join(dir, `${app}-${process.pid}.cwfd`);
  rmSync(dump, { force: true });
  const { takeSuiteLock } = await import('./lib/suite-lock.mjs');
  const { enterGpuSandbox } = await import('./lib/gpu-sandbox.mjs');
  const { testEnv } = await import('./lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  await takeSuiteLock('flash-check');
  testEnv(process.env);
  const args = [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', app, '--frames', String(frames), '--gpu', gpu];
  if (arg('input')) args.push('--input', resolve(arg('input')));
  if (arg('start-cmds')) args.push('--start-cmds', arg('start-cmds'));
  const r = spawnSync('node', args, {
    stdio: ['ignore', 'pipe', 'inherit'], encoding: 'utf8',
    env: { ...process.env, CW_FRAME_DUMP: dump, CW_FRAME_DUMP_FROM: String(from), CW_FRAME_DUMP_SCALE: arg('scale', '8'),
      CW_RUN_TIMEOUT_MS: process.env.CW_RUN_TIMEOUT_MS || String(Math.max(120000, frames * 400)) },
  });
  if (r.status !== 0 || !existsSync(dump)) { console.error(`flash-check: the run failed (${r.status}): ${(r.stdout || '').slice(-1500)}`); process.exit(1); }
  data = await readFrameDump(dump);
  if (data.frames.length !== frames - from + 1) { console.error(`flash-check: ${data.frames.length} frames in the dump, want ${frames - from + 1}`); process.exit(1); }
  label ||= `${app} frames ${from}-${frames} (${gpu})`;
  if (!keep) rmSync(dump, { force: true });
} else {
  console.error('usage: flash-check.mjs --app <app> --frames N [--from K] [--input f.json] | --dump f.cwfd | --dir pngs/');
  process.exit(2);
}
const g = flashGate(data.w, data.h, data.frames, opts, { label });
console.log(g.lines.join('\n'));
if (arg('json')) writeFileSync(resolve(arg('json')), JSON.stringify({ ok: g.ok, problems: g.problems, result: { ...g.result, opts: undefined }, control: { ...g.control, opts: undefined } }, null, 1));
process.exit(g.ok ? 0 : 1);
