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
//   CW_TEST_GPU=software node tests/ci.mjs
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
  'gpu-cleanup': 'runs wasmcart-native',
  'pipeline-perf': 'GPU timings, and romdev and Chromium legs',
  'fx-perf-report': 'GPU timings, and romdev and Chromium legs',
  'fx-gpu-report': 'GPU frame-time budgets (100k particles in two frames)',
  'vis-perf-report': 'GPU timings, and romdev and Chromium legs',
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
const runs = [
  ['GL', [...gl, ...scripts, '--hosts', 'native']],
  ['WebGPU', [...wgpu, '--gpu', 'webgpu', '--hosts', 'native']],
];
let failed = 0;
for (const [what, args] of runs) {
  console.log(`\n== ${what}: ${args.filter((a) => !a.startsWith('--') && !['native', 'webgpu'].includes(a)).length} gates`);
  if (await gate(args)) { failed++; console.log(`== ${what}: FAILED`); }
}
console.log(failed ? `ci: ${failed} run(s) failed` : `ok   ci (${gl.length} GL gates, ${scripts.length} scripted gates, ${wgpu.length} WebGPU gates; ${Object.keys(LOCAL_ONLY).length} scripted gates local only)`);
process.exit(failed ? 1 : 0);
