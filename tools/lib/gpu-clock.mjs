// The test GPU's shader clock while a run measures GPU time. An integrated GPU
// (the 890M) picks its clock from how busy it is: a run whose frames wait on
// the CPU (a game's simulation, a loaded machine) leaves the GPU idle part of
// every frame and its clock at the floor (600 to 700 MHz of 2900), and every
// pass then takes two to three times as long as it does at speed. A GPU time
// read without the clock beside it says as much about the machine as about the
// frame (docs/guides/profiling.md, "GPU timing on an integrated GPU").
//
//   const clk = gpuClock();          // starts sampling (a worker thread, so a
//   spawnSync(...)                   // blocking spawnSync does not stop it)
//   const c = await clk.stop();      // { samples, meanMhz, floorShare, levels }
//   console.log(clockLine(c));
//
// Linux amdgpu only (pp_dpm_sclk of the pinned card); elsewhere the result has
// samples 0 and clockLine says the clock was not read.
import { Worker } from 'node:worker_threads';
import { existsSync, readFileSync } from 'node:fs';
import { testGpu } from './wgpu-pin.mjs';

/** the pinned GPU's pp_dpm_sclk, or null */
export function sclkPath() {
  try {
    const g = testGpu();
    if (!g || g.software || !g.pci) return null;
    const p = `/sys/bus/pci/devices/${g.pci}/pp_dpm_sclk`;
    return existsSync(p) ? p : null;
  } catch { return null; }
}

/** the clock's levels (MHz) as the driver lists them, lowest first */
export function sclkLevels(path = sclkPath()) {
  if (!path) return [];
  try { return [...readFileSync(path, 'utf8').matchAll(/(\d+)Mhz/g)].map((m) => +m[1]); } catch { return []; }
}

const WORKER = `
const { parentPort, workerData } = process.getBuiltinModule('node:worker_threads');
const { readFileSync } = process.getBuiltinModule('node:fs');
const s = [];
const t = setInterval(() => {
  try { const m = /(\\d+)Mhz \\*/.exec(readFileSync(workerData.path, 'utf8')); if (m) s.push(+m[1]); } catch {}
}, workerData.ms);
parentPort.on('message', () => { clearInterval(t); parentPort.postMessage(s); });
`;

/**
 * Sample the clock every `ms` until stop(). floorShare: the share of samples at
 * the lowest level or within 100 MHz of it (the idle floor).
 */
export function gpuClock({ ms = 20, path = sclkPath() } = {}) {
  const levels = sclkLevels(path);
  if (!path) return { stop: async () => ({ samples: 0, meanMhz: NaN, floorShare: NaN, levels }) };
  const w = new Worker(WORKER, { eval: true, workerData: { path, ms } });
  return {
    stop: () => new Promise((res) => {
      w.once('message', (s) => {
        w.terminate();
        const floor = levels.length ? levels[0] + 100 : 0;
        res({ samples: s.length, meanMhz: s.length ? s.reduce((a, b) => a + b, 0) / s.length : NaN,
          floorShare: s.length ? s.filter((v) => v <= floor).length / s.length : NaN, levels });
      });
      w.postMessage('stop');
    }),
  };
}

/** "clock 1384 MHz mean (of 600..2900), 12% at the floor" */
export function clockLine(c) {
  if (!c.samples) return 'clock not read';
  const lv = c.levels.length ? ` (of ${c.levels[0]}..${c.levels[c.levels.length - 1]})` : '';
  return `clock ${Math.round(c.meanMhz)} MHz mean${lv}, ${Math.round(c.floorShare * 100)}% at the floor`;
}
