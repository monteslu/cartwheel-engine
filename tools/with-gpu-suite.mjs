// Run a command as a GPU suite: the machine-wide suite lock, the kernel-log /
// memory guards (tools/lib/gpu-guard.mjs) and an environment that cannot reach
// the desktop (SDL offscreen, no DISPLAY / WAYLAND_DISPLAY / XAUTHORITY, the
// test GPU through DRI_PRIME). Exit status is the command's; 3 = a guard fired.
//
//   node tools/with-gpu-suite.mjs <name> <command> [args...]
import { spawn } from 'node:child_process';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { takeSuiteLock } from './lib/suite-lock.mjs';
import { armGuard, EXIT_INFRA } from './lib/gpu-guard.mjs';
import { testEnv } from './lib/wgpu-pin.mjs';
import { enterGpuSandbox } from './lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const [name, cmd, ...args] = process.argv.slice(2);
if (!name || !cmd) { console.error('usage: with-gpu-suite.mjs <name> <command> [args...]'); process.exit(2); }
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
await takeSuiteLock(name);
testEnv(process.env);
const guard = armGuard(name, { outFile: join(ROOT, 'out', 'gate', `gpu-mem-${name}.json`) });
await guard.gate(name);
const code = await new Promise((res) => spawn(cmd, args, { stdio: 'inherit' }).on('close', (c, sig) => res(c ?? 128)));
guard.finish();
process.exit(code === EXIT_INFRA ? 1 : code);
