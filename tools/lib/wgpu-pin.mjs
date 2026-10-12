// Which GPU test runs use, and how every host is held to it. One place, like
// tools/lib/chromium.mjs for the browser's GL.
//
// The GPU is the machine's test GPU: $XDG_CONFIG_HOME/cartwheel/chromium-gpu
// (a PCI address such as 0000:03:00.0; CW_CHROMIUM_GPU overrides). GPU faults
// seen during test runs are guarded against in tools/lib/gpu-guard.mjs.
// Pinning another GPU (an integrated one, say) is a config change only: its
// PCI address in chromium-gpu and a substring of its name in wgpu-adapter.
// Its vendor:device ids and its class (discrete / integrated, by dedicated
// VRAM) come from sysfs; the adapter a host reports is checked against
// $XDG_CONFIG_HOME/cartwheel/wgpu-adapter (a substring of the adapter's
// device name, e.g. "7600"; CW_WGPU_ADAPTER overrides). A run without both
// refuses to start.
//
// No test process may reach the desktop: scrubDisplay() drops DISPLAY,
// WAYLAND_DISPLAY and XAUTHORITY, so an attempt to open a window fails loudly
// (tests/gpu-no-display.mjs proves no run opens an X or Wayland client).
//
// Per host:
//   native    CW_GPU=webgpu; Mesa's Vulkan device-select layer shows Dawn only
//             the pinned GPU; the engine asks for the power preference of the
//             pinned class and refuses any adapter but CW_WGPU_EXPECT_DEVICE
//             (set here) before a device exists; no surface at all
//   wcnative  the same Mesa pin + WASMCART_WGPU_POWER; the player's
//             "WebGPU on <device>" / "GL: <renderer>" line is checked
//   romdev    loadMedia gpuPower of the pinned class; the load result's
//             gpuAdapter is checked before any frame runs
//   chromium  bubblewrap shows the browser only the pinned GPU (chromium.mjs);
//             adapterOptions of the pinned class; host.getGpuAdapterInfo() is
//             checked, and every DRM fd's drm-pdev is
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

const cfg = (name) => {
  const dir = process.env.XDG_CONFIG_HOME || (process.env.HOME ? join(process.env.HOME, '.config') : '');
  const f = dir && join(dir, 'cartwheel', name);
  try { return f && existsSync(f) ? readFileSync(f, 'utf8').trim() : ''; } catch { return ''; }
};

/**
 * The machine's test GPU, read from sysfs.
 * @returns {{ pci: string, vendor: string, device: string, match: string, discrete: boolean,
 *   power: 'high-performance' | 'low-power', card: string, vramTotal: number }}
 */
/** CW_TEST_GPU=software: no GPU hardware (CI). GL runs on Mesa's llvmpipe, WebGPU on lavapipe;
 * nothing is pinned, sandboxed or guarded, and the renderer checks expect llvmpipe. */
export const softwareGpu = () => process.env.CW_TEST_GPU === 'software';

/** Vulkan loader env that offers lavapipe only (Mesa's software Vulkan), when its ICD is installed */
function lavapipe() {
  const icd = ['/usr/share/vulkan/icd.d/lvp_icd.json', '/usr/share/vulkan/icd.d/lvp_icd.x86_64.json'].find(existsSync);
  return icd ? { VK_DRIVER_FILES: icd, VK_ICD_FILENAMES: icd } : {};
}

export function testGpu() {
  if (softwareGpu())
    return { pci: 'software', vendor: '', device: '', match: (process.env.CW_WGPU_ADAPTER || 'llvmpipe').toLowerCase(),
      discrete: false, power: 'low-power', card: '', vramTotal: 0, software: true };
  const pci = process.env.CW_CHROMIUM_GPU || process.env.CW_CHROMIUM_RENDER_NODE || cfg('chromium-gpu');
  const match = (process.env.CW_WGPU_ADAPTER || cfg('wgpu-adapter')).toLowerCase();
  if (!pci || pci === 'none' || pci.startsWith('/dev/') || !match)
    throw new Error('GPU test runs need the test GPU in ~/.config/cartwheel/chromium-gpu (a PCI address, or CW_CHROMIUM_GPU) and wgpu-adapter (one word of its device name, e.g. 7600, or CW_WGPU_ADAPTER)');
  const sys = `/sys/bus/pci/devices/${pci}`;
  const id = (f) => readFileSync(`${sys}/${f}`, 'utf8').trim().replace(/^0x/, '');
  const vendor = id('vendor'), device = id('device');
  let vramTotal = 0;
  try { vramTotal = +readFileSync(`${sys}/mem_info_vram_total`, 'utf8'); } catch { /* not amdgpu */ }
  // a discrete card has its own VRAM; an APU's carve-out is 0.5 to 2 GiB of system RAM
  const discrete = vramTotal === 0 || vramTotal >= 3 * 2 ** 30;
  let card = '';
  try { card = readdirSync(`${sys}/drm`).find((n) => /^card\d+$/.test(n)) || ''; } catch { /* no DRM node */ }
  return { pci, vendor, device, match, discrete, power: discrete ? 'high-performance' : 'low-power', card, vramTotal };
}

/**
 * env without any way to reach the desktop's X or Wayland server. A pinned test
 * process that tries to open a window then fails loudly instead of landing a
 * client on the user's compositor.
 * @param {Record<string, string | undefined>} env
 */
export function scrubDisplay(env) {
  for (const k of ['DISPLAY', 'WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'XAUTHORITY']) delete env[k];
  return env;
}

export function wgpuPin() {
  const g = testGpu();
  return {
    pci: g.pci, vendor: g.vendor, device: g.device, match: g.match, power: g.power, discrete: g.discrete,
    // Vulkan (Dawn natively, webgpu-node in wcnative): only this GPU is enumerated
    env: g.software
      ? { WASMCART_WGPU_POWER: g.power, CW_WGPU_POWER: 'low', SDL_VIDEODRIVER: process.env.SDL_VIDEODRIVER || 'offscreen', ...lavapipe() }
      : { MESA_VK_DEVICE_SELECT: `${g.vendor}:${g.device}`, MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE: '1',
        WASMCART_WGPU_POWER: g.power, CW_WGPU_POWER: g.discrete ? 'high' : 'low', CW_WGPU_EXPECT_DEVICE: '0x' + g.device,
        SDL_VIDEODRIVER: process.env.SDL_VIDEODRIVER || 'offscreen' },
    check(where, name) {
      if (!name || !String(name).toLowerCase().includes(g.match))
        throw new Error(`${where}: WebGPU adapter is "${name}", not the test GPU (${g.match}): refusing`);
      return name;
    },
  };
}

// GL test runs: the same machine test GPU, through Mesa's DRI_PRIME, checked
// against the GL renderer the host logs ("GL: ... | <renderer> | ..." natively,
// "wasmcart: GL: <renderer> (...)" in wasmcart-run). Windows are never opened:
// SDL's offscreen driver (EGL surfaceless). CW_TEST_GPU=desktop opts out.
export function glPin() {
  if (process.env.CW_TEST_GPU === 'desktop') return null;
  const g = testGpu();
  return {
    // inside the GPU sandbox (gpu-sandbox.mjs) the pinned GPU is the only one visible and DRI_PRIME would
    // name a node by a PCI id Mesa can no longer match; outside it, DRI_PRIME pins what it can
    env: g.software
      ? { LIBGL_ALWAYS_SOFTWARE: '1', SDL_VIDEODRIVER: process.env.SDL_VIDEODRIVER || 'offscreen' }
      : { ...(process.env.CW_GPU_SANDBOXED ? {} : { DRI_PRIME: 'pci-' + g.pci.replace(/[:.]/g, '_') }),
        SDL_VIDEODRIVER: process.env.SDL_VIDEODRIVER || 'offscreen' },
    check(where, log) {
      const m = /wasmcart: GL: (.*?) \(/.exec(log || '') || /GL: [^|\n]*\| ([^|]*) \|/.exec(log || '');
      if (!m) throw new Error(where + ': no GL renderer line in the log, so the GPU could not be checked');
      if (!m[1].toLowerCase().includes(g.match)) throw new Error(where + ': GL ran on ' + m[1].trim() + ', not the test GPU (' + g.match + ')');
      return m[1].trim();
    },
  };
}

/**
 * env for a test process that must stay off the desktop: SDL's offscreen driver
 * (Mesa's surfaceless EGL, no window), no display variables, and the GL test GPU
 * through DRI_PRIME. CW_TEST_WINDOW=1 leaves env alone (a real window, on purpose).
 * @param {Record<string, string | undefined>} env
 */
export function testEnv(env) {
  if (process.env.CW_TEST_WINDOW) return env;
  const gl = glPin();
  if (gl) Object.assign(env, gl.env);
  env.SDL_VIDEODRIVER ||= 'offscreen';
  return scrubDisplay(env);
}
