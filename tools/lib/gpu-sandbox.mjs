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
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { testGpu } from './wgpu-pin.mjs';
import { killTree } from './gpu-guard.mjs';

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
  return [bwrap, '--die-with-parent', '--dev-bind', '/', '/', '--tmpfs', '/dev/dri', ...nodes.flatMap((n) => ['--dev-bind', n, n]),
    ...(existsSync('/dev/kfd') ? ['--ro-bind', '/dev/null', '/dev/kfd'] : []), ...masks];
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
  const argv = sandboxArgv(gpu.pci);
  const child = spawn(argv[0], [...argv.slice(1), process.execPath, ...process.execArgv, ...process.argv.slice(1)],
    { stdio: 'inherit', env: { ...process.env, CW_GPU_SANDBOXED: gpu.pci } });
  for (const s of ['SIGINT', 'SIGTERM', 'SIGHUP']) process.on(s, () => child.kill(s));
  const code = await new Promise((res) => child.on('close', (c, sig) => res(c ?? (sig ? 128 + ({ SIGINT: 2, SIGTERM: 15, SIGHUP: 1, SIGKILL: 9 }[sig] || 0) : 1))));
  process.exit(code);
}
