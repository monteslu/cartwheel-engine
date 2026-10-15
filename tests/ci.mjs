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
//   CW_TEST_GPU=software node tests/ci.mjs [gl | gl-<k>of<n> | webgpu]
//
// Both runs by default. gl-<k>of<n> is the k-th of n shares of the GL run, balanced by the
// rough CI times in SLOW (other gates count as SLOW_DEFAULT), so CI spreads it over n jobs.
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
  'gpu-guard': 'tests the GPU guards against a real card (VRAM, kernel log)',
  'gpu-no-display': 'runs wasmcart-native',
  'wcnative-pads': 'runs wasmcart-native',
  'gpu-cleanup': 'runs wasmcart-native',
  'pipeline-perf': 'GPU timings, and romdev and Chromium legs',
  'fx-perf-report': 'GPU timings, and romdev and Chromium legs',
  'fx-gpu-report': 'GPU frame-time budgets (100k particles in two frames)',
  'vis-perf-report': 'GPU timings, and romdev and Chromium legs',
  'nav-perf-report': 'timings, and Chromium legs',
  'nav-determinism': 'Chromium and wasmcart-native',
  'showcase-perf': 'GPU timings on romdev, Chromium and wasmcart-native',
  'editor-channel': 'romdev',
  'editor-play': 'romdev',
  net: 'romdev, Chromium and wasmcart-native',
  'project-templates': 'romdev, Chromium and wasmcart-native',
  'editor-import': 'romdev, Chromium and wasmcart-native',
  'a11y-channel': 'romdev',
  'project-export': 'romdev, wasmcart-native and the export toolchains',
  export: 'the export toolchains, an Android emulator, Wine, Chromium, romdev',
  plugins: 'romdev, Chromium and wasmcart-native',
  docs: 'Chromium',
  tutorials: 'romdev and wasmcart-native',
};

/** rough seconds a gate takes in CI on llvmpipe, for the slow ones (balancing the GL shares) */
const SLOW = {
  'platformer3d-replay': 660, 'showcase-cycle': 400, 'vis-city-check': 350, platformer3d: 330, 'platformer3d-auto': 310,
  'showcase-dusk': 290, 'showcase-night': 290, 'showcase-morning': 140, 'showcase-golden': 140, 'fx-perf': 130, 'post-aces-post': 110,
};
const SLOW_DEFAULT = 30;

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
const part = process.argv[2] || '';
const shard = /^gl-(\d+)of(\d+)$/.exec(part);
if (part && part !== 'gl' && part !== 'webgpu' && !(shard && +shard[1] >= 1 && +shard[1] <= +shard[2])) {
  console.error('usage: ci.mjs [gl | gl-<k>of<n> | webgpu]');
  process.exit(2);
}
const glAll = [...gl, ...scripts];
/** the k-th (1-based) of n shares: longest first, each to the share with the least time so far */
function share(names, k, n) {
  const load = new Array(n).fill(0), pick = new Set();
  const order = names.map((name, i) => ({ name, i, t: SLOW[name] || SLOW_DEFAULT })).sort((a, b) => b.t - a.t || a.i - b.i);
  for (const g of order) {
    const j = load.indexOf(Math.min(...load));
    load[j] += g.t;
    if (j === k - 1) pick.add(g.name);
  }
  return names.filter((name) => pick.has(name));
}
const glNames = shard ? share(glAll, +shard[1], +shard[2]) : glAll;
const runs = [
  ['GL', [...glNames, '--hosts', 'native']],
  ['WebGPU', [...wgpu, '--gpu', 'webgpu', '--hosts', 'native']],
].filter(([what]) => !part || (what === 'GL' ? part.startsWith('gl') : part === 'webgpu'));
let failed = 0;
for (const [what, args] of runs) {
  console.log(`\n== ${what}: ${args.filter((a) => !a.startsWith('--') && !['native', 'webgpu'].includes(a)).length} gates`);
  if (await gate(args)) { failed++; console.log(`== ${what}: FAILED`); }
}
console.log(failed ? `ci: ${failed} run(s) failed` : `ok   ci ${part || 'all'} (${runs.map(([what, args]) => `${what} ${args.filter((a) => !a.startsWith('--') && !['native', 'webgpu'].includes(a)).length}`).join(', ')}; ${Object.keys(LOCAL_ONLY).length} scripted gates local only)`);
process.exit(failed ? 1 : 0);
