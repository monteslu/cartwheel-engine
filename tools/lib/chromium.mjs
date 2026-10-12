// Chromium launch shared by every tool that runs carts in headless Chromium
// (Playwright). One place, so a GPU choice applies everywhere.
//
//   CW_CHROMIUM_GPU=0000:03:00.0  (a PCI address, or a render node such as
//       /dev/dri/renderD129; CW_CHROMIUM_RENDER_NODE is the same setting)
//       pins the browser to that GPU, Linux only. Chromium runs inside a
//       bubblewrap mount namespace whose /dev/dri holds only that GPU's nodes
//       (and /dev/kfd is /dev/null), so no process of it can open another
//       GPU. Chromium's own flags cannot do this: --use-angle=gl opens the
//       Wayland/X display and Mesa then renders on the GPU the compositor
//       uses, whatever --render-node-override or DRI_PRIME say (chrome-headless
//       has page-faulted a discrete GPU with --render-node-override). Pinned runs drop
//       DISPLAY / WAYLAND_DISPLAY and use ANGLE's GL backend over EGL
//       (--use-angle=gl-egl): the same Mesa GL driver, opened from /dev/dri.
//       verify(page) then fails the run unless WebGL reports a hardware
//       renderer and every DRM fd the browser's processes hold reports the
//       pinned device as drm-pdev.
//   Machine default: $XDG_CONFIG_HOME/cartwheel/chromium-gpu (see pinWanted);
//       CW_CHROMIUM_GPU=none runs unpinned.
//   CW_CHROMIUM_ARGS="--flag --flag2"   extra flags, appended.
//   CW_NO_CHROMIUM=1   refuse to launch Chromium at all (every launcher goes
//       through here): for times when browser GPU work is not safe to run.
import { tempDir } from './tmp.mjs';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, readlinkSync, realpathSync, writeFileSync } from 'node:fs';
import { basename, join } from 'node:path';

// A machine default lives outside the repo: $XDG_CONFIG_HOME/cartwheel/chromium-gpu
// (or ~/.config/cartwheel/chromium-gpu) holds the same value as CW_CHROMIUM_GPU,
// so a forgotten env var never runs the browser on the wrong GPU. The env var
// wins; CW_CHROMIUM_GPU=none runs unpinned.
function machineDefaultGpu() {
  const dir = process.env.XDG_CONFIG_HOME || (process.env.HOME ? join(process.env.HOME, '.config') : '');
  const f = dir && join(dir, 'cartwheel', 'chromium-gpu');
  try { return f && existsSync(f) ? readFileSync(f, 'utf8').trim() : ''; } catch { return ''; }
}
const pinWanted = () => {
  const v = process.env.CW_CHROMIUM_GPU || process.env.CW_CHROMIUM_RENDER_NODE || machineDefaultGpu();
  return v === 'none' ? '' : v;
};

/** True when launchChromium will pin the browser to one GPU (env or machine default). */
export const chromiumPinned = () => !!pinWanted();

export function chromiumArgs() {
  if (process.env.CW_NO_CHROMIUM) throw new Error('CW_NO_CHROMIUM is set: Chromium runs are disabled');
  // --disable-software-rasterizer: when the GPU process fails, WebGL fails
  // with it instead of Chromium relaunching it on SwiftShader (a CPU
  // renderer: slow, and not what any test means to measure)
  const a = ['--enable-gpu', '--ignore-gpu-blocklist', pinWanted() ? '--use-angle=gl-egl' : '--use-angle=gl',
    '--enable-unsafe-swiftshader=false', '--disable-software-rasterizer'];
  if (process.env.CW_CHROMIUM_ARGS) a.push(...process.env.CW_CHROMIUM_ARGS.split(/\s+/).filter(Boolean));
  return a;
}

// The environment for the browser process: the caller's, minus what is meant
// for native runs only (EGL_PLATFORM=surfaceless is for headless native GL).
// Unpinned runs keep DISPLAY / WAYLAND_DISPLAY: ANGLE's GL backend opens its
// EGL display through them, and without either the GPU process cannot start.
export function chromiumEnv() {
  const env = { ...process.env };
  delete env.EGL_PLATFORM;
  // a pinned browser never reaches the desktop (bubblewrap unsets them too)
  if (pinWanted()) for (const k of ['DISPLAY', 'WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'XAUTHORITY']) delete env[k];
  return env;
}

// The GPU pin (null when CW_CHROMIUM_GPU is unset): { exe, pci, verify(page) }.
// outDir holds the generated launcher script.
export function chromiumGpuPin(chromium, outDir) {
  const want = pinWanted();
  if (!want) return null;
  if (process.platform !== 'linux') throw new Error('CW_CHROMIUM_GPU: Linux only');
  const pci = want.startsWith('/dev/') ? basename(realpathSync(`/sys/class/drm/${basename(want)}/device`)) : want;
  const drmDir = `/sys/bus/pci/devices/${pci}/drm`;
  if (!existsSync(drmDir)) throw new Error(`CW_CHROMIUM_GPU: no DRM device at PCI ${pci}`);
  const nodes = readdirSync(drmDir).filter((n) => /^(card|renderD)\d+$/.test(n)).map((n) => `/dev/dri/${n}`);
  if (!nodes.some((n) => n.includes('renderD'))) throw new Error(`CW_CHROMIUM_GPU: ${pci} has no render node`);
  const bwrap = ['/usr/bin/bwrap', '/bin/bwrap'].find(existsSync);
  if (!bwrap) throw new Error('CW_CHROMIUM_GPU needs bubblewrap (bwrap)');
  // the same headless shell Playwright launches by default, else full Chromium
  const full = chromium.executablePath();
  const shell = full.replace(/chromium-(\d+)\/chrome-linux64\/chrome$/, 'chromium_headless_shell-$1/chrome-headless-shell-linux64/chrome-headless-shell');
  const real = existsSync(shell) ? shell : full;
  const q = (a) => `'${a.replace(/'/g, `'\\''`)}'`;
  // inside the suites' GPU sandbox (tools/lib/gpu-sandbox.mjs) for this same GPU the view is already
  // narrowed, and bwrap cannot make a namespace inside it: the launcher only drops the display variables
  const nested = process.env.CW_GPU_SANDBOXED === pci;
  const bw = nested ? ['env', '-u', 'WAYLAND_DISPLAY', '-u', 'DISPLAY', real]
    : [bwrap, '--dev-bind', '/', '/', '--tmpfs', '/dev/dri', ...nodes.flatMap((n) => ['--dev-bind', n, n]),
      ...(existsSync('/dev/kfd') ? ['--ro-bind', '/dev/null', '/dev/kfd'] : []),
      '--unsetenv', 'WAYLAND_DISPLAY', '--unsetenv', 'DISPLAY', real];
  const dir = tempDir(outDir, 'gpupin-');
  const exe = join(dir, 'chrome');
  writeFileSync(exe, `#!/bin/sh\nexec ${bw.map(q).join(' ')} "$@"\n`, { mode: 0o755 });
  const verify = async (page) => {
    const gl = await page.evaluate(() => {
      const g = document.createElement('canvas').getContext('webgl2');
      const e = g && g.getExtension('WEBGL_debug_renderer_info');
      return g ? { renderer: e ? g.getParameter(e.UNMASKED_RENDERER_WEBGL) : g.getParameter(g.RENDERER) } : null;
    });
    if (!gl || /swiftshader|llvmpipe|software/i.test(gl.renderer)) throw new Error(`CW_CHROMIUM_GPU: no hardware WebGL (${gl && gl.renderer})`);
    // every process under this one: bwrap, the browser, its GPU and renderer processes
    const kids = new Map();
    for (const p of readdirSync('/proc').filter((d) => /^\d+$/.test(d))) {
      let st = '';
      try { st = readFileSync(`/proc/${p}/stat`, 'utf8'); } catch { continue; }
      const ppid = +st.slice(st.lastIndexOf(')') + 2).split(' ')[1];
      if (!kids.has(ppid)) kids.set(ppid, []);
      kids.get(ppid).push(+p);
    }
    const tree = [process.pid];
    for (let i = 0; i < tree.length; i++) tree.push(...(kids.get(tree[i]) || []));
    const pdevs = new Set();
    for (const p of tree) {
      let fds = [];
      try { fds = readdirSync(`/proc/${p}/fd`); } catch { continue; }
      for (const fd of fds) {
        let link = '';
        try { link = readlinkSync(`/proc/${p}/fd/${fd}`); } catch { continue; }
        if (!link.startsWith('/dev/dri/')) continue;
        let m = null;
        try { m = /drm-pdev:\s*(\S+)/.exec(readFileSync(`/proc/${p}/fdinfo/${fd}`, 'utf8')); } catch { continue; }
        pdevs.add(m ? m[1] : link);
      }
    }
    if (tree.length < 3 || pdevs.size === 0) throw new Error('CW_CHROMIUM_GPU: the browser holds no DRM device (no GPU process found)');
    if ([...pdevs].some((d) => d !== pci)) throw new Error(`CW_CHROMIUM_GPU: browser holds DRM devices ${[...pdevs].join(', ')}, not only ${pci}`);
    return gl.renderer;
  };
  return { exe, pci, verify };
}

// Launch (the only way tools should): returns { browser, verify(page) },
// verify being a no-op when unpinned. Call verify after the first page loads.
// extra: more flags for this launch (WebGPU runs add --enable-unsafe-webgpu)
export async function launchChromium(chromium, outDir, extra = []) {
  const pin = chromiumGpuPin(chromium, outDir);
  const browser = await chromium.launch({ args: [...chromiumArgs(), ...extra], env: chromiumEnv(), ...(pin ? { executablePath: pin.exe } : {}) });
  return { browser, pin, verify: async (page) => (pin ? pin.verify(page) : null) };
}
