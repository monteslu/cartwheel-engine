// Which wasmcart-run the wcnative host runs, in one place (run-cart, and the
// tool gates that start players themselves).
//
// The player is wasmcart-native's main build beside the checkout (or beside
// the main checkout when this is a git worktree under worktrees/), built with
// WebGPU (a wgpu/ directory beside wasmcart-run). CW_WCNATIVE names the GL
// player, CW_WCNATIVE_WGPU the WebGPU one. There is no other fallback: a
// missing or WebGPU-less player throws, so a run can never silently take an
// old build from some other directory.
import { existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const builds = [join(ROOT, '..'), join(ROOT, '..', '..')].map((d) => join(d, 'wasmcart-native', 'build'));

function named(envName) {
  const v = process.env[envName];
  if (!existsSync(v)) throw new Error(`wcnative: ${envName}=${v} does not exist`);
  return v;
}

function mainBuild() {
  const b = builds.find((d) => existsSync(join(d, 'wasmcart-run')));
  if (!b) throw new Error(`wcnative: no wasmcart-run in ${builds.join(' or ')} (build wasmcart-native, or set CW_WCNATIVE / CW_WCNATIVE_WGPU)`);
  return b;
}

/** the wasmcart-run for a WebGPU run (built with WebGPU: wgpu/src/wgpu/host.js beside it); throws if there is none */
export function wcnativeWgpuPlayer() {
  if (process.env.CW_WCNATIVE_WGPU) return named('CW_WCNATIVE_WGPU');
  const b = mainBuild();
  if (!existsSync(join(b, 'wgpu', 'src', 'wgpu', 'host.js')))
    throw new Error(`wcnative: ${b} was built without WebGPU (no wgpu/src/wgpu/host.js); rebuild it with WebGPU or set CW_WCNATIVE_WGPU`);
  return join(b, 'wasmcart-run');
}

/** the wasmcart-run for a GL run; throws if there is none */
export function wcnativePlayer() {
  if (process.env.CW_WCNATIVE) return named('CW_WCNATIVE');
  return join(mainBuild(), 'wasmcart-run');
}
