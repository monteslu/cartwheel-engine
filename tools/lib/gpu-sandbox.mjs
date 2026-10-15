// Run a suite (or one tool) inside a bubblewrap mount namespace that shows it
// ONLY the test GPU, and no way to the desktop. Mesa's EGL device platform
// (SDL's offscreen driver) ignores DRI_PRIME and takes the first render node
// (on a machine with two GPUs, often not the one wanted), so an environment variable cannot pin
// a native GL run; a process that cannot open any other GPU's node cannot land
// there. Every child of the suite inherits the namespace (native, wasmcart-run,
// the editors, and Chromium's own pin inside it).
//
//   /dev/dri holds only the pinned GPU's nodes; /dev/kfd is /dev/null;
//   the X socket directory and the Wayland socket are masked, so an attempt to
//   reach the desktop fails loudly (and DISPLAY / WAYLAND_DISPLAY / XAUTHORITY
//   are dropped by wgpu-pin.mjs's scrubDisplay).
//
// enterGpuSandbox() is called first thing by tests/gate.mjs, tests/all.mjs,
// tools/run-cart.mjs, tools/with-gpu-suite.mjs and the tests that start GPU
// processes: outside the sandbox it re-runs the same script inside it (and
// exits with its status); inside (CW_GPU_SANDBOXED) it checks the view and
// returns. CW_TEST_GPU=desktop or CW_TEST_WINDOW=1 skips it.
import { spawn } from 'node:child_process';
import { existsSync, readdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { testGpu } from './wgpu-pin.mjs';
import { killTree } from './gpu-guard.mjs';

const SYS_ONLINE = '/sys/devices/system/cpu/online';
let onlineFile = ''; // this suite's online CPU list (removed when the suite ends)
/** "0-<n>" for CW_WORKERS = n (n + 1 CPUs), or undefined when it is not set */
const onlineList = (w) => (w === undefined || w === '' ? undefined : ((n) => (n > 1 ? `0-${n - 1}\n` : '0\n'))(Math.max(0, Math.min(63, parseInt(w, 10) || 0)) + 1));

/**
 * Inside the sandbox: show the processes started next (wasmcart-run) CW_WORKERS + 1 online CPUs, or the
 * machine's list when CW_WORKERS is not set. Outside it (or opted out) it does nothing: wasmcart-run then
 * reports the machine's threads.
 */
export function showCartWorkers() {
  if (!process.env.CW_GPU_SANDBOXED || process.env.CW_CPU_ONLINE === undefined) return;
  const want = onlineList(process.env.CW_WORKERS) ?? process.env.CW_CPU_ONLINE + '\n';
  try { if (readFileSync(SYS_ONLINE, 'utf8') !== want) writeFileSync(SYS_ONLINE, want); } catch { /* not the suite's file */ }
}

/** the bwrap argv (without the command) that shows only the GPU at `pci` */
export function sandboxArgv(pci) {
  const bwrap = ['/usr/bin/bwrap', '/bin/bwrap'].find(existsSync);
  if (!bwrap) throw new Error('the GPU sandbox needs bubblewrap (bwrap)');
  const drm = `/sys/bus/pci/devices/${pci}/drm`;
  if (!existsSync(drm)) throw new Error(`no DRM device at PCI ${pci}`);
  const nodes = readdirSync(drm).filter((n) => /^(card|renderD)\d+$/.test(n)).map((n) => `/dev/dri/${n}`);
  if (!nodes.some((n) => n.includes('renderD'))) throw new Error(`${pci} has no render node`);
  const uid = process.getuid ? process.getuid() : 1000;
  const run = process.env.XDG_RUNTIME_DIR || `/run/user/${uid}`;
  const masks = [];
  if (existsSync('/tmp/.X11-unix')) masks.push('--tmpfs', '/tmp/.X11-unix');
  try { for (const f of readdirSync(run).filter((n) => /^wayland-\d+(\.lock)?$/.test(n))) masks.push('--ro-bind', '/dev/null', `${run}/${f}`); } catch { /* no runtime dir */ }
  // The online CPU list (what wasmcart-run reports to a cart as its hardware threads; a cart's job pool
  // is that minus one) is a file of the suite's own, so a wcnative run can show its cart CW_WORKERS + 1
  // CPUs: the pool size native runs take from CW_WORKERS (Chromium's carts get it from tools/lib/workers.mjs).
  // It starts as CW_WORKERS says, or as the machine's list; showCartWorkers() sets it per run.
  const cpus = [];
  if (existsSync(SYS_ONLINE)) {
    const real = readFileSync(SYS_ONLINE, 'utf8').trim();
    onlineFile = `${run}/cartwheel-cpu-online-${process.pid}`;
    writeFileSync(onlineFile, onlineList(process.env.CW_WORKERS) ?? real + '\n');
    process.env.CW_CPU_ONLINE = real;
    cpus.push('--bind', onlineFile, SYS_ONLINE);
  }
  return [bwrap, '--die-with-parent', '--dev-bind', '/', '/', '--tmpfs', '/dev/dri', ...nodes.flatMap((n) => ['--dev-bind', n, n]),
    ...(existsSync('/dev/kfd') ? ['--ro-bind', '/dev/null', '/dev/kfd'] : []), ...masks, ...cpus];
}

/** true when this process is meant to run unsandboxed (an explicit opt-out) */
export const sandboxOptOut = () => process.env.CW_TEST_GPU === 'desktop' || process.env.CW_TEST_GPU === 'software' || !!process.env.CW_TEST_WINDOW;

/** the DRM nodes this process can open */
export const visibleNodes = () => { try { return readdirSync('/dev/dri').filter((n) => /^(card|renderD)\d+$/.test(n)).sort(); } catch { return []; } };

/**
 * Re-run this script inside the sandbox, or (already inside) verify the view.
 * Returns only when the caller is inside the sandbox or opted out.
 */
export async function enterGpuSandbox() {
  if (sandboxOptOut()) return;
  const gpu = testGpu();
  if (process.env.CW_GPU_SANDBOXED) {
    if (process.env.CW_GPU_SANDBOXED !== gpu.pci) throw new Error(`GPU sandbox is for ${process.env.CW_GPU_SANDBOXED}, the test GPU is ${gpu.pci}`);
    // bwrap's own child: bwrap dies with the script outside (--die-with-parent) but does not take its
    // child along, which then ran on with no signal (a stopped suite left gate.mjs and its runs going).
    // When the parent is gone, end everything under this process and exit.
    let parent = '';
    try { parent = readFileSync(`/proc/${process.ppid}/comm`, 'utf8').trim(); } catch { /* gone already */ }
    if (parent === 'bwrap') {
      const ppid = process.ppid;
      setInterval(() => { if (process.ppid !== ppid) { killTree(); process.exit(129); } }, 500).unref();
    }
    return;
  }
  const argv = sandboxArgv(gpu.pci); // (sets CW_CPU_ONLINE for the child)
  const child = spawn(argv[0], [...argv.slice(1), process.execPath, ...process.execArgv, ...process.argv.slice(1)],
    { stdio: 'inherit', env: { ...process.env, CW_GPU_SANDBOXED: gpu.pci } });
  for (const s of ['SIGINT', 'SIGTERM', 'SIGHUP']) process.on(s, () => child.kill(s));
  const code = await new Promise((res) => child.on('close', (c, sig) => res(c ?? (sig ? 128 + ({ SIGINT: 2, SIGTERM: 15, SIGHUP: 1, SIGKILL: 9 }[sig] || 0) : 1))));
  if (onlineFile) rmSync(onlineFile, { force: true });
  process.exit(code);
}
