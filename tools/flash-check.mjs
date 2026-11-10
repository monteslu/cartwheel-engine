// Does a run strobe? Every frame of a run (or frames already captured) checked against the photosensitive
// flash rules (WCAG 2.2 success criterion 2.3.1 and the Harding test's thresholds; tools/lib/flash.mjs has them and
// the measures). Each check also runs its CONTROL, which must fail: the same frames with an 8 Hz full-screen strobe.
//
//   node tools/flash-check.mjs --app <app> --frames N [--from K] [--input script.json] [--start-cmds "a;b"]
//        [--env '{"NAME":"v"}'] [--gpu gl|webgpu] --host romdev|native [--scale 8] [--keep run.cwfd] [--label text]
//                         runs the app for N frames on the fixed 1/60 s clock (with its input script, start
//                         requests and environment, as a gate gives them) and checks frames K..N (default: from frame
//                         2), each box-filtered down by --scale (8) in linear light. CW_PROJECT_ROOT: an app of a
//                         games repository.
//                         --host romdev (the default): the dual cart (build/cart-wgpu) on romdev. tools/run-cart.mjs
//                         --flash-from loads it and runs the start requests, environment and input to frame K-1, then
//                         ONE frame({op:'flash'}) call steps frames K..N and runs the same analysis and control on the
//                         server (romdev AGENTS.md "Flash check and ranged frame capture"); never a request per frame
//                         (romdev is shared). ROMDEV_URL picks another server than 127.0.0.1:7331.
//                         --host native: the native build through CW_FRAME_DUMP, checked here (takes the GPU suite lock)
//   node tools/flash-check.mjs --dump run.cwfd    checks a CW_FRAME_DUMP file (src/platform/native.c)
//   node tools/flash-check.mjs --dir frames/      checks a directory of PNG frames, one a frame, in name order
// Bounds (defaults in tools/lib/flash.mjs FLASH_DEFAULTS): --max-rise X (bound the hardest two-frame rise of a sixth of
// the screen, off by default), --flicker-amp X, --max-flashes N, --fps N. --json FILE writes the numbers.
// Exit 0: the run passes and the control fails; 1 otherwise.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
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
  const app = arg('app'), frames = +arg('frames', 0), from = +arg('from', 2), gpu = arg('gpu', 'gl'), host = arg('host', 'romdev');
  if (!(frames > from)) { console.error('flash-check: --frames N (more than --from) is required with --app'); process.exit(2); }
  if (host === 'romdev') {
    // romdev's frame({op:'flash'}) runs the same analysis (the same rules and measures as lib/flash.mjs) on the
    // server, over every frame of the range in ONE call, with its control; tools/run-cart.mjs --flash-from loads the
    // dual cart, runs the start requests, environment and input script to frame K-1, then makes that call
    const PROJ = process.env.CW_PROJECT_ROOT ? resolve(process.env.CW_PROJECT_ROOT) : ROOT;
    const dir = join(PROJ, 'out', 'flash');
    mkdirSync(dir, { recursive: true });
    const outFile = join(dir, `${app}-${gpu}-${process.pid}.json`);
    const args = [join(ROOT, 'tools/run-cart.mjs'), '--host', 'romdev', '--app', app, '--frames', String(frames), '--gpu', gpu,
      '--flash-from', String(from), '--flash-out', outFile];
    if (arg('input')) args.push('--input', resolve(arg('input')));
    if (arg('start-cmds')) args.push('--start-cmds', arg('start-cmds'));
    if (arg('env')) args.push('--env', arg('env'));
    if (arg('keep')) args.push('--flash-keep', resolve(arg('keep')));
    args.push('--scale', arg('scale', '8')); // by 8, as a native dump (CW_FRAME_DUMP_SCALE)
    if (Object.keys(opts).length) args.push('--flash-opts', JSON.stringify(opts));
    const r = spawnSync('node', args, { stdio: ['ignore', 'pipe', 'pipe'], encoding: 'utf8', env: process.env });
    if (r.status !== 0 || !existsSync(outFile)) {
      console.error(`flash-check: the romdev run failed (${r.status}): ${((r.stderr || '') + (r.stdout || '')).slice(-1500)}`);
      process.exit(r.status === 2 ? 2 : 1);
    }
    const f = JSON.parse(readFileSync(outFile, 'utf8'));
    rmSync(outFile, { force: true });
    label ||= `${app} frames ${from}-${frames} (romdev ${gpu})`;
    const want = frames - from + 1;
    const lines = [];
    const ctlFails = !!(f.control && f.control.failsAsItMust);
    let ok = f.pass && ctlFails && f.frames.captured === want;
    lines.push(`${f.pass ? 'ok  ' : 'FAIL'} ${label}: ${f.summary}`);
    for (const p of f.problems) lines.push(`     ${p}`);
    if (f.frames.captured !== want) lines.push(`     romdev captured ${f.frames.captured} frames, want ${want}${f.truncated ? ` (${f.truncated})` : ''}`);
    if (f.result.worstSecond) {
      const w = f.result.worstSecond;
      lines.push(`     worst second: frames ${from + w.from}-${from + w.to} (${w.measure}): flashes ${w.regions} (red ${w.regionsRed}), fields ${w.fields} (red ${w.fieldsRed})`);
    }
    lines.push(ctlFails ? `ok   control fails as it must (the run with an 8 Hz full-screen strobe): ${f.control.problems[0]}`
      : `CONTROL PASSED (a failure): an 8 Hz full-screen strobe over the run went unseen: ${f.control ? f.control.summary : 'no control ran'}`);
    lines.push(`     romdev: ${f.romdevCalls + 1} requests, ${f.frames.captured} frames at ${f.size.analysed.join('x')} from ${f.size.source.join('x')} in ${(f.timing.totalMs / 1000).toFixed(1)} s`);
    console.log(lines.join('\n'));
    if (arg('json')) writeFileSync(resolve(arg('json')), JSON.stringify({ ok, host: 'romdev', problems: f.problems, result: f.result, control: f.control, frames: f.frames, size: f.size, timing: f.timing, romdevCalls: f.romdevCalls + 1 }, null, 1));
    process.exit(ok ? 0 : 1);
  }
  if (host !== 'native') { console.error('flash-check: --host romdev|native'); process.exit(2); }
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
  if (arg('env')) args.push('--env', arg('env'));
  const r = spawnSync('node', args, {
    stdio: ['ignore', 'pipe', 'inherit'], encoding: 'utf8',
    env: { ...process.env, CW_FRAME_DUMP: dump, CW_FRAME_DUMP_FROM: String(from), CW_FRAME_DUMP_SCALE: arg('scale', '8'),
      CW_RUN_TIMEOUT_MS: process.env.CW_RUN_TIMEOUT_MS || String(Math.max(120000, frames * 400)) },
  });
  if (r.status !== 0 || !existsSync(dump)) { console.error(`flash-check: the run failed (${r.status}): ${(r.stdout || '').slice(-1500)}`); process.exit(1); }
  data = await readFrameDump(dump);
  if (data.frames.length !== frames - from + 1) { console.error(`flash-check: ${data.frames.length} frames in the dump, want ${frames - from + 1}`); process.exit(1); }
  label ||= `${app} frames ${from}-${frames} (native ${gpu})`;
  if (!keep) rmSync(dump, { force: true });
} else {
  console.error('usage: flash-check.mjs --app <app> --frames N [--from K] [--input f.json] | --dump f.cwfd | --dir pngs/');
  process.exit(2);
}
const g = flashGate(data.w, data.h, data.frames, opts, { label });
console.log(g.lines.join('\n'));
if (arg('json')) writeFileSync(resolve(arg('json')), JSON.stringify({ ok: g.ok, problems: g.problems, result: { ...g.result, opts: undefined }, control: { ...g.control, opts: undefined } }, null, 1));
process.exit(g.ok ? 0 : 1);
