// Streaming perf gate: samples/open_world flies across its 16.4 km world
// (OPEN_WORLD "ow fly": 150 m/s, cells loading ahead and unloading behind,
// the origin moving every kilometre) on each host, and reports per host:
// stalls (an activation that had to wait for its cell), the worst frame and
// the 99th percentile (wall clock, frame to frame), frames over the 60 Hz
// budget plus 1 ms, the main thread's own CPU time per frame (natively; a
// cart has no thread clock) and memory
// over the flight (natively the process's resident set, in a cart its linear
// memory). The native run is a player-timed run (CW_PLAYER_TIMING: loads land when
// they are decoded, as in a game) without GPU timer queries, whose polling adds
// stalls of its own; per-preset GPU time is in docs/guides/streaming.md.
// Checks:
//   - no stalls on any host;
//   - memory flat: over the last third of the run the process grows under 5 %
//     (the first two thirds fill the caches and the allocator's high-water mark;
//     the engine's own tracked bytes stay bounded throughout);
//   - native and romdev (deterministic runs) agree on the activation history
//     (stream.hash), the origin and the cells active at the end.
// Controls that must fail:
//   - the same flight with a load radius of 620 m (cells asked for 20 m before
//     they must be active) must stall;
//   - with a residency cache that never evicts ("ow cache 4096"), memory must
//     grow past the flat check.
// Frame times are reported, not checked: on a shared machine another
// process's load moves them (the main thread's CPU time moves least).
//
//   node tests/stream-perf.mjs [--hosts native,romdev,wcnative] [--frames 6800] [--mode fly|drive]
// (the drive is slower: --mode drive --frames 36000 drives 14 km, 10 minutes of game time)
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, writeFileSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
testEnv(process.env);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'perf');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : d);
const hosts = opt('--hosts', 'native,romdev,wcnative').split(',');
const FRAMES = +opt('--frames', 6800);
const MODE = opt('--mode', 'fly');
const FLAT = 0.05; // the last third: caches and the allocator's high-water mark fill over the first two

/** one run: { host, tag, state (fields; from the log on wcnative), error } */
function run(host, tag, cmds, extraEnv = {}, frames = FRAMES) {
  return new Promise((res) => {
    const state = join(OUT, `open_world.${tag}.json`), log = join(OUT, `open_world.${tag}.log`);
    const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'open_world', '--frames', String(frames),
      '--start-cmds', cmds.join(';'), ...(host === 'wcnative' ? ['--log', log] : ['--state', state]),
      ...(host === 'native' ? ['--log', log] : [])];
    const env = { ...process.env, CW_RUN_TIMEOUT_MS: '1800000', ...extraEnv };
    const p = spawn('node', args, { stdio: ['ignore', 'pipe', 'pipe'], env });
    let err = '';
    p.stderr.on('data', (d) => (err += d));
    p.on('close', (code) => {
      if (code !== 0) return res({ host, tag, error: err.trim().split('\n').slice(-2).join(' | ') });
      res({ host, tag, state: host === 'wcnative' ? fromLog(readFileSync(log, 'utf8')) : JSON.parse(readFileSync(state, 'utf8')) });
    });
  });
}

/** wasmcart-run reports no state: the app's periodic report lines carry the same numbers */
function fromLog(text) {
  const s = { 'mem.process_mb': [], 'mem.km': [] };
  for (const line of text.split('\n')) {
    const m = /open_world: frame (\d+), ([\d.]+) km: (\d+) cells loaded, (\d+) active; process ([\d.]+) MB.*frame max ([\d.]+) ms, p99 ([\d.]+), over [\d.]+ ms: (\d+) \(main thread CPU max ([\d.]+) p50 ([\d.]+) p99 ([\d.]+).*stalls (\d+)/.exec(line);
    if (!m) continue;
    Object.assign(s, { 'world.frame': +m[1], 'stream.loaded': +m[3], 'stream.active': +m[4], 'perf.max_ms': +m[6], 'perf.p99_ms': +m[7],
      'perf.over_budget': +m[8], 'perf.cpu_max_ms': +m[9], 'perf.cpu_p50_ms': +m[10], 'perf.cpu_p99_ms': +m[11], 'stream.stalls': +m[12] });
    s['mem.process_mb'].push(+m[5]);
    s['mem.km'].push(+m[2]);
  }
  s['mem.samples'] = s['mem.process_mb'].length;
  return s;
}

const mem = (s) => (s['mem.process_mb'] || []).slice(0, s['mem.samples'] || 0);
const growth = (s) => {
  const m = mem(s);
  if (m.length < 4) return NaN;
  const a = m[Math.floor((m.length * 2) / 3)], b = m[m.length - 1];
  return (b - a) / a;
};

const fly = MODE === 'fly' ? ['ow fly'] : ['ow drive'];
const results = [];
for (const h of hosts) results.push(await run(h, `${MODE}.${h}`, fly, h === 'native' ? { CW_PLAYER_TIMING: '1' } : {}));
const controls = [
  // the controls fly (the fastest the cells go by), whatever the mode
  await run('native', `${MODE}.native.radius620`, ['ow fly', 'ow radius 620'], {}, 6800),
  await run('native', `${MODE}.native.cache4096`, ['ow fly', 'ow cache 4096'], {}, 6800),
];

const pad = (s, n) => String(s).padEnd(n);
const num = (v, d = 2) => (typeof v === 'number' && Number.isFinite(v) ? v.toFixed(d) : '-');
const rows = [['stalls', 'stream.stalls', 0], ['start cells', 'stream.start_cells', 0], ['worst frame ms', 'perf.max_ms', 2],
  ['p99 frame ms', 'perf.p99_ms', 2], ['p50 frame ms', 'perf.p50_ms', 2], [`frames > 17.67 ms`, 'perf.over_budget', 0],
  ['main CPU worst ms', 'perf.cpu_max_ms', 2], ['main CPU p99 ms', 'perf.cpu_p99_ms', 2], ['main CPU p50 ms', 'perf.cpu_p50_ms', 2],
  ['longest on_load ms', 'stream.prepare_max_ms', 2], ['cells loaded (end)', 'stream.loaded', 0], ['rebases', 'stream.rebases', 0]];
const all = [...results, ...controls];
const lines = [`open_world ${MODE}: ${FRAMES} frames, 16.4 km world, 512 m cells`, pad('', 20) + all.map((r) => pad(r.tag.replace(`${MODE}.`, ''), 20)).join('')];
for (const [label, key, d] of rows) lines.push(pad(label, 20) + all.map((r) => pad(r.error ? '(failed)' : num(r.state[key], d), 20)).join(''));
lines.push(pad('memory MB over time', 20) + all.map((r) => pad(r.error ? '' : mem(r.state).filter((_, i, a) => i % Math.max(1, Math.floor(a.length / 4)) === 0 || i === a.length - 1).map((v) => v.toFixed(0)).join('>'), 20)).join(''));
lines.push(pad('growth, last third', 20) + all.map((r) => pad(r.error ? '' : (growth(r.state) * 100).toFixed(1) + ' %', 20)).join(''));
console.log(lines.join('\n'));
writeFileSync(join(OUT, `stream-report-${MODE}.txt`), lines.join('\n') + '\n');

let fail = 0;
const no = (why) => { fail++; console.log('FAIL ' + why); };
for (const r of all.filter((r) => r.error)) no(`${r.tag}: ${r.error}`);
for (const r of results.filter((r) => !r.error)) {
  if (r.state['stream.stalls'] !== 0) no(`${r.tag}: ${r.state['stream.stalls']} stalls`);
  if (!(Math.abs(growth(r.state)) <= FLAT)) no(`${r.tag}: memory grew ${(growth(r.state) * 100).toFixed(1)} % over the last third`);
}
const nat = results.find((r) => r.host === 'native' && !r.error), rom = results.find((r) => r.host === 'romdev' && !r.error);
if (nat && rom)
  for (const k of ['stream.hash', 'origin.x', 'origin.z', 'stream.active', 'stream.rebases'])
    if (nat.state[k] !== rom.state[k]) no(`romdev ${k} ${rom.state[k]} differs from native's ${nat.state[k]}`);
const [c1, c2] = controls;
if (!c1.error && !(c1.state['stream.stalls'] > 0)) { fail++; console.log('CONTROL PASSED: a 620 m load radius did not stall'); }
if (!c2.error && Math.abs(growth(c2.state)) <= FLAT) { fail++; console.log('CONTROL PASSED: a cache that never evicts kept memory flat'); }
console.log(fail ? `FAIL stream-perf (${fail})` : 'ok   stream-perf');
process.exit(fail ? 1 : 0);
