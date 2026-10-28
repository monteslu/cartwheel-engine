// The CI test run: every gate that works without GPU hardware, on the native
// host, through Mesa's software renderers (CW_TEST_GPU=software: llvmpipe for
// GL, lavapipe for WebGPU). It runs
//   1. every app gate on GL (tests/gate.mjs --hosts native),
//   2. every gate with a "webgpu" entry on WebGPU (--gpu webgpu --hosts native),
//   3. the scripted gates below that need neither a GPU nor another host.
// Hosts CI does not have (romdev, Chromium, wasmcart-native, wasmcart-libretro)
// and anything that measures or guards real GPU hardware stay local: LOCAL_ONLY
// says why for each, and docs/guides/testing.md lists them.
//
//   CW_TEST_GPU=software node tests/ci.mjs [gl | gl-<k>of<n> | webgpu | webgpu-<k>of<n>] [--list]
//
// Both runs by default. gl-<k>of<n> (webgpu-<k>of<n>) is the k-th of n shares of the GL
// (WebGPU) run, balanced by each gate's measured CI time in tests/ci-times.json (gates
// not in it count as DEFAULT_SECONDS), so CI spreads a run over n jobs. Refresh the times
// with `node tools/ci-times.mjs` after gates are added or get slower.
import { spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
if (process.env.CW_TEST_GPU !== 'software') {
  console.error('tests/ci.mjs runs on software rendering only: set CW_TEST_GPU=software');
  process.exit(2);
}

/** scripted gates that do not run in CI, and why */
export const LOCAL_ONLY = {
  'gi-perf': 'GPU timings on the test GPU',
  'gi-probe': 'reads pixels of native runs on the test GPU (GL and WebGPU) against known answers',
  'gpu-guard': 'tests the GPU guards against a real card (VRAM, kernel log)',
  'gpu-no-display': 'runs wasmcart-native',
  'wcnative-pads': 'runs wasmcart-native',
  'gpu-cleanup': 'runs wasmcart-native',
  'pipeline-perf': 'GPU timings, and romdev and Chromium legs',
  'fx-perf-report': 'GPU timings, and romdev and Chromium legs',
  'fx-gpu-report': 'GPU frame-time budgets (100k particles in two frames)',
  'vis-perf-report': 'GPU timings, and romdev and Chromium legs',
  'nav-perf-report': 'timings, and Chromium legs',
  'stream-perf': 'frame times and memory on romdev and wasmcart-native',
  'nav-determinism': 'Chromium and wasmcart-native',
  'cloth-determinism': 'Chromium and wasmcart-native',
  'ik-determinism': 'Chromium',
  'froxel-determinism': 'Chromium and wasmcart-native',
  'vis-city-batch': 'Chromium (the leg compares against the native run on one machine)',
  'showcase-perf': 'GPU timings on the test GPU',
  'editor-channel': 'native editor sessions, not yet run in CI (its editor cart leg went with the GL-only cart)',
  'editor-play': 'romdev',
  net: 'romdev, Chromium and wasmcart-native',
  'project-templates': 'romdev, Chromium and wasmcart-native',
  'editor-import': 'native editor sessions, not yet run in CI (its cart legs went with the GL-only cart)',
  'a11y-channel': 'romdev',
  'profile-cart': 'romdev',
  'cart-audio-ring': 'romdev',
  'sdk-rebuild': 'an unpacked linux-x64 SDK from a game repository (CW_TEST_SDK)',
  'project-export': 'romdev, wasmcart-native and the export toolchains',
  export: 'the export toolchains, an Android emulator, Wine, Chromium, romdev',
  plugins: 'romdev, Chromium and wasmcart-native',
  docs: 'Chromium',
  tutorials: 'romdev and wasmcart-native',
  'noise-gpu': 'romdev and Chromium legs',
  'water-surface': 'romdev and Chromium legs',
  'boom-perf': 'GPU timing reports (GL and WebGPU on the test GPU)',
  spikes: 'WebGPU timestamp queries on the test GPU',
  'fx-explosion-kit': 'the pinned GPU sandbox (renders on GL and WebGPU and diffs them)',
  'fx-distort-regions': 'the pinned GPU sandbox (renders on GL and WebGPU and diffs them)',
};

/** measured seconds per gate in CI on llvmpipe / lavapipe (tools/ci-times.mjs), for balancing the shares */
const TIMES = JSON.parse(readFileSync(join(ROOT, 'tests/ci-times.json'), 'utf8'));
const DEFAULT_SECONDS = 30;

const gates = JSON.parse(readFileSync(join(ROOT, 'tests/gates.json'), 'utf8'));
const unknown = Object.keys(LOCAL_ONLY).filter((n) => !gates.some((g) => g.name === n));
if (unknown.length) { console.error(`tests/ci.mjs: LOCAL_ONLY names no gate: ${unknown.join(', ')}`); process.exit(2); }
const native = (g) => !g.hosts || g.hosts.includes('native');
const gl = gates.filter((g) => g.app && g.gl !== false && native(g)).map((g) => g.name);
const wgpu = gates.filter((g) => g.app && g.webgpu).map((g) => g.name);
const scripts = gates.filter((g) => g.script && !LOCAL_ONLY[g.name]).map((g) => g.name);

function gate(args) {
  return new Promise((res) => spawn('node', [join(ROOT, 'tests/gate.mjs'), ...args], { stdio: 'inherit' }).on('close', (c) => res(c ?? 1)));
}
const list = process.argv.includes('--list'); // print the share's gates and their measured seconds, run nothing
const part = process.argv.slice(2).find((a) => a !== '--list') || '';
const shard = /^(gl|webgpu)-(\d+)of(\d+)$/.exec(part);
const kind = shard ? shard[1] : part;
if (part && part !== 'gl' && part !== 'webgpu' && !(shard && +shard[2] >= 1 && +shard[2] <= +shard[3])) {
  console.error('usage: ci.mjs [gl | gl-<k>of<n> | webgpu | webgpu-<k>of<n>]');
  process.exit(2);
}
const glAll = [...gl, ...scripts];
/** the k-th (1-based) of n shares: longest first, each to the share with the least time so far */
function share(names, times, k, n) {
  const load = new Array(n).fill(0), pick = new Set();
  const order = names.map((name, i) => ({ name, i, t: times[name] ?? DEFAULT_SECONDS })).sort((a, b) => b.t - a.t || a.i - b.i);
  for (const g of order) {
    const j = load.indexOf(Math.min(...load));
    load[j] += g.t;
    if (j === k - 1) pick.add(g.name);
  }
  return names.filter((name) => pick.has(name));
}
const glNames = shard && kind === 'gl' ? share(glAll, TIMES.gl, +shard[2], +shard[3]) : glAll;
const wgpuNames = shard && kind === 'webgpu' ? share(wgpu, TIMES.webgpu, +shard[2], +shard[3]) : wgpu;
const runs = [
  ['GL', [...glNames, '--hosts', 'native']],
  ['WebGPU', [...wgpuNames, '--gpu', 'webgpu', '--hosts', 'native']],
].filter(([what]) => !part || (what === 'GL' ? kind === 'gl' : kind === 'webgpu'));
if (list) {
  for (const [what, args] of runs) {
    const names = args.filter((a) => !a.startsWith('--') && !['native', 'webgpu'].includes(a));
    const t = what === 'GL' ? TIMES.gl : TIMES.webgpu;
    console.log(`${what}: ${names.length} gates, about ${names.reduce((a, n) => a + (t[n] ?? DEFAULT_SECONDS), 0)} s`);
    for (const n of names) console.log(`  ${String(t[n] ?? DEFAULT_SECONDS).padStart(5)} s  ${n}`);
  }
  process.exit(0);
}
let failed = 0;
for (const [what, args] of runs) {
  console.log(`\n== ${what}: ${args.filter((a) => !a.startsWith('--') && !['native', 'webgpu'].includes(a)).length} gates`);
  if (await gate(args)) { failed++; console.log(`== ${what}: FAILED`); }
}
console.log(failed ? `ci: ${failed} run(s) failed` : `ok   ci ${part || 'all'} (${runs.map(([what, args]) => `${what} ${args.filter((a) => !a.startsWith('--') && !['native', 'webgpu'].includes(a)).length}`).join(', ')}; ${Object.keys(LOCAL_ONLY).length} scripted gates local only)`);
process.exit(failed ? 1 : 0);
