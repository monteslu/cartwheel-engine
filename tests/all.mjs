// Everything: the cross-host gate, the debug channel, the project round
// trip, hot reload and play-in-editor (remote), the WebGPU gates, pixel scale
// and the host's preferred resolution through wasmcart-libretro. ROMDEV_PORT picks the romdev server (default 7331).
//
//   node tests/all.mjs [--no-chromium | --chromium-only]
//
// --no-chromium leaves every Chromium leg out; --chromium-only runs only those
// (the only thing that has ever page-faulted the GPU: run them last and alone).
// Exit status: 0 pass, 1 test failures, 3 infrastructure (the GPU guards fired:
// a kernel GPU error, the VRAM budget, or a gate that could not start;
// tools/lib/gpu-guard.mjs) with no further tests run.
import { spawn } from 'node:child_process';
import { existsSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';
import { armGuard, EXIT_INFRA } from '../tools/lib/gpu-guard.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

await takeSuiteLock('all.mjs');
testEnv(process.env);
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const flags = argv.filter((a) => a === '--no-chromium' || a === '--chromium-only');
const chromiumOnly = flags.includes('--chromium-only');
rmSync(join(ROOT, 'out', 'gate', 'gpu-mem.json'), { force: true });
// this process watches the kernel log and the card's memory for the whole run (each gate.mjs adds its own accounting)
const guard = armGuard('all.mjs', { outFile: join(ROOT, 'out', 'gate', 'gpu-all.json') });
let failed = 0;
// the WebGPU axis (gate.mjs --gpu webgpu, and every dual cart on WebGPU in Node) when the dual cart tree is built
const wgpu = existsSync(join(ROOT, 'build', 'cart-wgpu', 'carts'));
const runs = [['gate.mjs', ...flags], ...(wgpu ? [['gate.mjs', '--gpu', 'webgpu', ...flags]] : []),
  ...(wgpu && !chromiumOnly ? [['wgpu-carts.mjs']] : []),
  // pixel_scale / crisp UI on each host and GPU API; carts at the host's preferred size through wasmcart-libretro
  ['pixel-scale.mjs', '--hosts', chromiumOnly ? 'chromium' : flags.includes('--no-chromium') ? 'native' : 'native,chromium', ...(wgpu ? [] : ['--gpus', 'gl'])],
  ...(chromiumOnly ? [] : [['host-resolution.mjs']]),
  // GL post fusion draws what the separate passes draw (native)
  ...(chromiumOnly ? [] : [['post-fusion.mjs']]),
  ...(chromiumOnly ? [] : [['deps-pins.mjs'], ['suite-lock.mjs'], ['channel.mjs'], ['vfs.mjs'], ['hot.mjs'], ['programs-carts.mjs'], ['remote.mjs'], ['js-int-math.mjs']])];
for (const [t, ...a] of runs) {
  await guard.gate(`${t} ${a.join(' ')}`.trim());
  const code = await new Promise((res) => spawn('node', [join(ROOT, 'tests', t), ...a], { stdio: 'inherit' }).on('close', (c) => res(c)));
  guard.end();
  if (code === EXIT_INFRA) { console.error(`${t} ended on a GPU guard: stopping`); process.exit(EXIT_INFRA); }
  if (code !== 0) failed++;
}
guard.finish();
process.exit(failed ? 1 : 0);
