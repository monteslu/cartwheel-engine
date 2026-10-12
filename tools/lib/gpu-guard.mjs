// GPU safety guards for the suites (tests/gate.mjs, tests/all.mjs): the test
// GPU is the machine's discrete card, which also drives the desktop, so a test
// run must never fault it and never starve the compositor of VRAM.
//
//   (a) kernel-log watcher: `journalctl -k -f -n 0`; the FIRST amdgpu error line
//       for the pinned device (page fault, ring timeout, GPU reset, wedged,
//       "Not enough memory for command submission", "CS has been rejected", VM
//       fault) kills every child, releases the suite lock, prints the line and
//       exits 3 (infrastructure) without running another test.
//   (b) GPU memory accounting: mem_info_vram_used / mem_info_gtt_used before each
//       gate and every 250 ms during it, plus per-process drm-memory-vram /
//       drm-memory-gtt from /proc/<pid>/fdinfo for the process tree (and the
//       romdev server, which hosts romdev runs). out/gate/gpu-mem.json holds the
//       peaks per gate and host; the summary names the top consumers and
//       monotonic growth across gates in the VRAM the suite's OWN process tree
//       holds before each gate (a leak signal: a player, browser or runner that
//       lives on and keeps allocating). The card-wide rise is printed beside it
//       as context, never flagged: it moves with the desktop, the shared romdev
//       server's hosts (kept until shut down or evicted) and other sessions.
//   (c) budgets, in FREE VRAM (what the card has left), not a share of it: a
//       gate starts once free >= reserve + need (wait up to 10 minutes, then
//       exit 3), and the suite aborts (exit 3) when free drops below the reserve
//       during a gate. The reserve is what the desktop always keeps; the
//       compositor's own submissions fail with -ENOMEM when VRAM runs out and it
//       does not recover. A share of the card was the wrong measure: the desktop
//       alone (browser, video and chat apps, the romdev server) sits at 60-75%
//       of an 8 GB card, so a 60% start budget made every gate wait its full 10
//       minutes and then give up, while 2 GB were free.
//
// Test hooks (the guards' must-fail controls use them):
//   CW_GPU_GUARD_FAKE_KERNEL_FILE=<file>  lines appended to it are treated as kernel log lines
//   CW_GPU_GUARD_FAKE_VRAM_FILE=<file>    a byte count in it replaces the VRAM-used reading
//                                         (and the suite's own VRAM, unless the next one is set)
//   CW_GPU_GUARD_FAKE_OWN_FILE=<file>     a byte count in it replaces the suite's own VRAM
//   CW_GPU_GUARD_WAIT_MS=<ms>             the start-of-gate wait (default 10 minutes)
//   CW_GPU_GUARD_RESERVE_MB=<MiB>         free VRAM the desktop always keeps (default 512)
//   CW_GPU_GUARD_NEED_MB=<MiB>            free VRAM a gate needs on top of the reserve to start
//                                         (default 1280: a suite's runs peak at about 1.3 GiB)
//
// An integrated GPU (an APU such as the 890M: wgpu-pin's testGpu().discrete is
// false) has a small VRAM carve-out (512 MiB to 2 GiB) and allocates the rest
// in GTT, system memory. Its budget is the carve-out's free part plus the GTT
// it can still map, capped by the memory the system has available
// (MemAvailable); counting the carve-out alone made every gate on the 890M
// wait its full 10 minutes and give up (879 MiB free, 1792 MiB needed).
import { spawn } from 'node:child_process';
import { existsSync, readFileSync, readdirSync, readlinkSync, writeFileSync, mkdirSync, statSync, openSync, readSync, closeSync } from 'node:fs';
import { dirname, basename } from 'node:path';
import { testGpu } from './wgpu-pin.mjs';

export const EXIT_INFRA = 3;

/** kernel lines that mean the GPU (or its driver) is in trouble */
export const KERNEL_BAD = /page fault|ring \S+ timeout|GPU reset|gpu reset|wedged|command submission|CS has been rejected|VM fault|\[gfxhub\]|\[mmhub\]|amdgpu.*\*ERROR\*|\[drm\] \*ERROR\*|GPU hang|device lost|VRAM is lost|mode1 reset|mode2 reset/i;

/**
 * Whether a kernel log line is a GPU error of the pinned device. A line that
 * names another PCI device is another card's trouble; one that names none
 * ("[drm] *ERROR* Not enough memory for command submission!") is counted.
 * @param {string} line @param {string} pci
 */
export function isGpuError(line, pci) {
  if (!KERNEL_BAD.test(line)) return false;
  const m = /(?:amdgpu|drm)[ :]+(?:\[)?(\d{4}:[0-9a-f]{2}:[0-9a-f]{2}\.\d)/i.exec(line);
  if (m && pci && m[1].toLowerCase() !== pci.toLowerCase()) return false;
  return true;
}

/** pids of every process under `root` (not including it) */
export function descendants(root = process.pid) {
  const kids = new Map();
  for (const p of readdirSync('/proc')) {
    if (!/^\d+$/.test(p)) continue;
    let st = '';
    try { st = readFileSync(`/proc/${p}/stat`, 'utf8'); } catch { continue; }
    const ppid = +st.slice(st.lastIndexOf(')') + 2).split(' ')[1];
    if (!kids.has(ppid)) kids.set(ppid, []);
    kids.get(ppid).push(+p);
  }
  const out = [];
  for (let i = -1, q = [root]; i < q.length - 1;) { i++; for (const c of kids.get(q[i]) || []) { out.push(c); q.push(c); } }
  return out;
}

export function killTree(sig = 'SIGKILL') {
  const pids = descendants();
  for (const p of pids.reverse()) { try { process.kill(p, sig); } catch { /* gone */ } }
  return pids.length;
}

const KIB = { '': 1 / 1024, B: 1 / 1024, KiB: 1, MiB: 1024, GiB: 1048576 };
const toKiB = (v, u) => parseFloat(v) * (KIB[u || ''] ?? 1);

/**
 * Per-process DRM memory (bytes) for the given pids on the pinned device:
 * drm-memory-vram / drm-memory-gtt of each distinct drm-client-id.
 * @returns {Map<number, { vram: number, gtt: number, comm: string }>}
 */
export function procDrmMemory(pids, pci) {
  const out = new Map();
  for (const pid of pids) {
    let fds = [];
    try { fds = readdirSync(`/proc/${pid}/fd`); } catch { continue; }
    const seen = new Set();
    let vram = 0, gtt = 0, any = false;
    for (const fd of fds) {
      let link = '';
      try { link = readlinkSync(`/proc/${pid}/fd/${fd}`); } catch { continue; }
      if (!link.startsWith('/dev/dri/')) continue;
      let t = '';
      try { t = readFileSync(`/proc/${pid}/fdinfo/${fd}`, 'utf8'); } catch { continue; }
      const dev = /drm-pdev:\s*(\S+)/.exec(t);
      if (pci && dev && dev[1] !== pci) continue;
      const cid = (/drm-client-id:\s*(\d+)/.exec(t) || [])[1];
      if (cid !== undefined) { if (seen.has(cid)) continue; seen.add(cid); }
      const g = (k) => { const m = new RegExp(`${k}:\\s*([\\d.]+)\\s*(\\w*)`).exec(t); return m ? toKiB(m[1], m[2]) * 1024 : 0; };
      vram += g('drm-memory-vram');
      gtt += g('drm-memory-gtt');
      any = true;
    }
    if (any) {
      let comm = '';
      try { comm = readFileSync(`/proc/${pid}/comm`, 'utf8').trim(); } catch { /* gone */ }
      out.set(pid, { vram, gtt, comm });
    }
  }
  return out;
}

/** which host a process belongs to, by its executable */
export function hostOf(comm, cmdline) {
  if (/chrome/i.test(comm)) return 'chromium';
  if (/wasmcart-run/.test(comm) || /wasmcart-run/.test(cmdline)) return 'wcnative';
  if (/romdevtools\/src\/mcp\/server\.js/.test(cmdline)) return 'romdev'; // node's comm may read MainThread
  return 'native';
}

function romdevPids() {
  const out = [];
  for (const p of readdirSync('/proc')) {
    if (!/^\d+$/.test(p)) continue;
    try { if (/romdevtools\/src\/mcp\/server\.js/.test(readFileSync(`/proc/${p}/cmdline`, 'utf8').replace(/\0/g, ' '))) out.push(+p); } catch { /* gone */ }
  }
  return out;
}

const num = (f) => { try { return +readFileSync(f, 'utf8'); } catch { return NaN; } };

/**
 * Arm the guards. Returns { gate(name), end(), finish(), pci, ... }.
 * @param {string} suite  name for messages
 * @param {{ outFile?: string, onFatal?: () => void }} [opts]
 */
export function armGuard(suite, opts = {}) {
  const gpu = testGpu();
  const dev = `/sys/bus/pci/devices/${gpu.pci}`;
  const MiB = 2 ** 20;
  const reserve = +(process.env.CW_GPU_GUARD_RESERVE_MB || 512) * MiB, need = +(process.env.CW_GPU_GUARD_NEED_MB || 1280) * MiB;
  const waitMs = +(process.env.CW_GPU_GUARD_WAIT_MS || 10 * 60 * 1000);
  const fakeVram = process.env.CW_GPU_GUARD_FAKE_VRAM_FILE;
  const total = num(`${dev}/mem_info_vram_total`);
  const gttTotal = num(`${dev}/mem_info_gtt_total`);
  /** memory the GPU can still allocate: VRAM, plus GTT on an integrated GPU */
  const freeOf = (s) => {
    const vramFree = total - s.vram;
    if (gpu.discrete) return vramFree;
    let avail = Infinity;
    try { avail = +(/^MemAvailable:\s+(\d+) kB/m.exec(readFileSync('/proc/meminfo', 'utf8')) || [])[1] * 1024 || Infinity; } catch { /* no /proc */ }
    return vramFree + Math.max(0, Math.min(gttTotal - s.gtt, avail));
  };
  const what = gpu.discrete ? 'VRAM' : 'VRAM + GTT';
  let fatalDone = false;

  function fatal(why, line) {
    if (fatalDone) return;
    fatalDone = true;
    const n = killTree();
    console.error(`\nGPU GUARD (${suite}): ${why}${line ? '\n  ' + line : ''}\n  killed ${n} child process(es); stopping without running further tests (exit ${EXIT_INFRA}: infrastructure, not a test failure)`);
    try { if (opts.onFatal) opts.onFatal(); } catch { /* best effort */ }
    try { writeOut(); } catch { /* best effort */ }
    process.exit(EXIT_INFRA); // the suite lock is released by its exit hook
  }

  // a suite that is stopped takes its tests with it: killing all.mjs used to leave gate.mjs and the
  // runs under it going (orphans holding the GPU after the lock was released)
  for (const sig of ['SIGINT', 'SIGTERM', 'SIGHUP'])
    process.once(sig, () => { killTree(); process.exit(128 + ({ SIGHUP: 1, SIGINT: 2, SIGTERM: 15 })[sig]); });

  // (a) the kernel log
  let journal = null, fakeTimer = null;
  const onLine = (l) => { if (isGpuError(l, gpu.pci)) fatal('kernel GPU error on ' + gpu.pci, l.trim()); };
  const lines = (cb) => { let buf = ''; return (d) => { buf += d; let i; while ((i = buf.indexOf('\n')) >= 0) { cb(buf.slice(0, i)); buf = buf.slice(i + 1); } }; };
  if (process.env.CW_GPU_GUARD_FAKE_KERNEL_FILE) {
    const f = process.env.CW_GPU_GUARD_FAKE_KERNEL_FILE;
    let pos = existsSync(f) ? statSync(f).size : 0;
    const feed = lines(onLine);
    fakeTimer = setInterval(() => {
      if (!existsSync(f)) return;
      const sz = statSync(f).size;
      if (sz <= pos) return;
      const fd = openSync(f, 'r'), b = Buffer.alloc(sz - pos);
      readSync(fd, b, 0, b.length, pos); closeSync(fd); pos = sz;
      feed(b.toString('utf8'));
    }, 50);
  } else {
    journal = spawn('journalctl', ['-k', '-f', '-n', '0', '--no-pager', '-o', 'short-iso'], { stdio: ['ignore', 'pipe', 'ignore'] });
    journal.stdout.on('data', lines(onLine));
    journal.on('error', () => { console.error('GPU GUARD: journalctl is unavailable: the kernel log is not watched'); });
  }

  // (b) accounting
  const readSys = () => {
    let vram = num(`${dev}/mem_info_vram_used`);
    if (fakeVram && existsSync(fakeVram)) { const v = parseFloat(readFileSync(fakeVram, 'utf8')); if (Number.isFinite(v)) vram = v; }
    return { vram, gtt: num(`${dev}/mem_info_gtt_used`) };
  };
  /* VRAM held by the suite's own process tree (players, browsers, runners it
   * spawned): the leak signal's measure. The card-wide figure also counts the
   * desktop, the shared romdev server's hosts and other sessions, so it rises
   * and falls with work this suite does not own. A fake VRAM file (tests)
   * stands in for this too, unless CW_GPU_GUARD_FAKE_OWN_FILE gives its own. */
  const fakeOwn = process.env.CW_GPU_GUARD_FAKE_OWN_FILE || fakeVram;
  const readOwn = () => {
    if (fakeOwn && existsSync(fakeOwn)) { const v = parseFloat(readFileSync(fakeOwn, 'utf8')); if (Number.isFinite(v)) return v; }
    let v = 0;
    for (const m of procDrmMemory(descendants(), gpu.pci).values()) v += m.vram;
    return v;
  };
  /** @type {any[]} */ const gates = [];
  /** @type {any} */ let cur = null;
  let timer = null;

  function sample() {
    if (!cur) return;
    const s = readSys();
    cur.peak.vram = Math.max(cur.peak.vram, s.vram);
    cur.peak.gtt = Math.max(cur.peak.gtt, s.gtt);
    const pids = [...descendants(), ...romdevPids()];
    const per = procDrmMemory(pids, gpu.pci);
    const hostSum = {};
    for (const [pid, m] of per) {
      let cmd = '';
      try { cmd = readFileSync(`/proc/${pid}/cmdline`, 'utf8').replace(/\0/g, ' '); } catch { /* gone */ }
      const h = hostOf(m.comm, cmd);
      const a = (hostSum[h] ||= { vram: 0, gtt: 0 });
      a.vram += m.vram; a.gtt += m.gtt;
      const key = `${m.comm}`;
      const t = (cur.procs[key] ||= { vram: 0, gtt: 0, host: h });
      t.vram = Math.max(t.vram, m.vram); t.gtt = Math.max(t.gtt, m.gtt);
    }
    for (const [h, a] of Object.entries(hostSum)) {
      const p = (cur.hosts[h] ||= { vram: 0, gtt: 0 });
      p.vram = Math.max(p.vram, a.vram); p.gtt = Math.max(p.gtt, a.gtt);
    }
    if (freeOf(s) < reserve)
      fatal(`free ${what} is ${(freeOf(s) / MiB).toFixed(0)} MiB (under the ${(reserve / MiB).toFixed(0)} MiB reserve) during gate ${cur.name}: the compositor starves at the limit`);
  }

  function writeOut() {
    if (!opts.outFile) return;
    mkdirSync(dirname(opts.outFile), { recursive: true });
    let prior = [];
    try { if (process.env.CW_SUITE_LOCK_HELD && process.env.CW_SUITE_LOCK_HELD !== String(process.pid)) prior = JSON.parse(readFileSync(opts.outFile, 'utf8')).gates || []; } catch { /* first writer */ }
    writeFileSync(opts.outFile, JSON.stringify({ gpu: { pci: gpu.pci, card: gpu.card, vramTotal: total }, gates: [...prior, ...gates] }, null, 1) + '\n');
  }

  async function startGate(name) {
    end();
    const t0 = Date.now();
    let said = false;
    for (;;) {
      const s = readSys();
      const free = freeOf(s);
      if (free >= reserve + need) break;
      if (!said) { console.error(`GPU GUARD: ${(free / MiB).toFixed(0)} MiB of ${what} free (${name} needs ${((reserve + need) / MiB).toFixed(0)} MiB: ${(need / MiB).toFixed(0)} plus the ${(reserve / MiB).toFixed(0)} MiB reserve); waiting`); said = true; }
      if (Date.now() - t0 > waitMs) fatal(`free ${what} stayed under ${((reserve + need) / MiB).toFixed(0)} MiB (${(free / MiB).toFixed(0)} MiB) for ${(waitMs / 60000).toFixed(1)} minutes: not starting ${name}`);
      await new Promise((r) => setTimeout(r, Math.min(5000, Math.max(50, waitMs / 4))));
    }
    const s = readSys();
    cur = { name, before: { ...s, own: readOwn() }, peak: { ...s }, hosts: {}, procs: {}, t0: Date.now() };
    sample();
    timer = setInterval(sample, 250);
  }
  function end() {
    if (!cur) return;
    clearInterval(timer);
    sample();
    cur.after = { ...readSys(), own: readOwn() };
    cur.ms = Date.now() - cur.t0;
    delete cur.t0;
    gates.push(cur);
    cur = null;
    writeOut();
  }

  /** growth across gates (leak signal): the VRAM the suite's OWN processes hold
   * before each gate should not climb (a player, browser or runner that lives on
   * across gates and keeps allocating). The card-wide figure is reported as
   * context only: it moves with the desktop, romdev's hosts and other sessions. */
  function growth() {
    const v = gates.map((g) => g.before.own ?? 0), card = gates.map((g) => g.before.vram);
    if (v.length < 6) return null;
    let up = 0;
    for (let i = 1; i < v.length; i++) if (v[i] > v[i - 1] + 8 * 2 ** 20) up++;
    const rise = v[v.length - 1] - v[0];
    return { gates: v.length, rise, steps_up: up, leak: rise > 256 * 2 ** 20 && up >= 0.6 * (v.length - 1), card_rise: card[card.length - 1] - card[0] };
  }

  function finish() {
    end();
    clearInterval(fakeTimer);
    if (journal) journal.kill();
    const mib = (b) => (b / 2 ** 20).toFixed(0) + ' MiB';
    const top = gates.map((g) => ({ name: g.name, vram: g.peak.vram, gtt: g.peak.gtt, hosts: g.hosts })).sort((a, b) => b.vram - a.vram).slice(0, 5);
    console.log(`GPU memory (${gpu.pci}, ${mib(total)} VRAM): top gates by peak VRAM in use on the card`);
    for (const t of top)
      console.log(`  ${t.name.padEnd(28)} vram ${mib(t.vram).padStart(9)} gtt ${mib(t.gtt).padStart(8)}  ${Object.entries(t.hosts).map(([h, a]) => `${h} ${mib(a.vram)}/${mib(a.gtt)}`).join(', ')}`);
    const hosts = {};
    for (const g of gates) for (const [h, a] of Object.entries(g.hosts)) { const p = (hosts[h] ||= { vram: 0, gtt: 0 }); p.vram = Math.max(p.vram, a.vram); p.gtt = Math.max(p.gtt, a.gtt); }
    console.log('  peak per host (vram/gtt): ' + (Object.entries(hosts).map(([h, a]) => `${h} ${mib(a.vram)}/${mib(a.gtt)}`).join(', ') || 'none seen'));
    const gr = growth();
    if (gr) console.log(`  growth across ${gr.gates} gates: ${gr.rise >= 0 ? '+' : ''}${mib(gr.rise)} VRAM held by the suite's own processes before the gates, ${gr.steps_up} rises${gr.leak ? '  LEAK SIGNAL: monotonic growth, find which gate' : ''} (card-wide ${gr.card_rise >= 0 ? '+' : ''}${mib(gr.card_rise)}: desktop, romdev hosts and other sessions included, context only)`);
    writeOut();
    return gr;
  }

  return { gate: startGate, end, finish, fatal, pci: gpu.pci, readSys, total, gates };
}
