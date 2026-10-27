// One GPU test suite at a time per GPU. On 2026-10-06 three suites ran at
// once, ran the desktop GPU out of memory for command submission and took the
// user's Xwayland down. gate.mjs / all.mjs take this lock (a directory under
// $XDG_RUNTIME_DIR, or ~/.cache) and wait for it; child processes inherit it
// through CW_SUITE_LOCK_HELD. A holder whose pid is gone is a stale lock.
// Waiters queue first-come-first-served: each writes a ticket (its start time
// and pid) in the sibling directory cartwheel-gpu-suite.lock.queue, and only
// the oldest LIVE ticket may take the lock (tickets of dead pids are pruned).
// Polling alone let whoever polled first win, starving an early waiter for an
// hour (2026-10-08).
import { mkdirSync, readFileSync, writeFileSync, rmSync, existsSync, statSync, readdirSync, readlinkSync } from 'node:fs';
import { join } from 'node:path';
import { killTree } from './gpu-guard.mjs';

// One lock per GPU: a suite pinned to another GPU than the machine's default test
// GPU (CW_CHROMIUM_GPU=<pci>, e.g. the idle integrated 890M) queues on that GPU's
// own lock and runs beside the default GPU's suites. The default GPU keeps the
// original lock name, so older checkouts still share it.
const pinnedPci = () => {
  const cfgDir = process.env.XDG_CONFIG_HOME || join(process.env.HOME || '.', '.config');
  let def = '';
  try { def = readFileSync(join(cfgDir, 'cartwheel', 'chromium-gpu'), 'utf8').trim(); } catch {}
  const pci = process.env.CW_CHROMIUM_GPU || process.env.CW_CHROMIUM_RENDER_NODE || def;
  return pci && pci !== def ? pci : '';
};
const dir = () => {
  const pci = pinnedPci();
  return join(process.env.XDG_RUNTIME_DIR || join(process.env.HOME || '.', '.cache'),
    'cartwheel-gpu-suite' + (pci ? '-' + pci.replace(/[^0-9a-zA-Z]/g, '_') : '') + '.lock');
};
const alive = (pid) => { try { process.kill(pid, 0); return true; } catch { return false; } };
const OWNERLESS_MS = 30000;
/** pids with `path` open (Linux /proc; elsewhere none can be told, so none) */
function openers(path) {
  const pids = [];
  let procs = [];
  try { procs = readdirSync('/proc').filter((n) => /^\d+$/.test(n)); } catch { return pids; }
  for (const p of procs) {
    let fds = [];
    try { fds = readdirSync(`/proc/${p}/fd`); } catch { continue; }
    for (const fd of fds) {
      try { if (readlinkSync(`/proc/${p}/fd/${fd}`) === path) { pids.push(+p); break; } } catch {}
    }
  }
  return pids;
}

const queueDir = () => dir() + '.queue';
// "<ms since epoch, 15 digits>-<pid>": names sort by arrival
const ticketPid = (t) => parseInt(t.split('-')[1], 10);
/** live tickets, oldest first; dead pids' tickets removed */
function liveTickets() {
  let names = [];
  try { names = readdirSync(queueDir()).filter((n) => /^\d+-\d+$/.test(n)).sort(); } catch { return []; }
  return names.filter((t) => {
    if (alive(ticketPid(t))) return true;
    try { rmSync(join(queueDir(), t), { force: true }); } catch {}
    return false;
  });
}

export async function takeSuiteLock(name) {
  if (process.env.CW_SUITE_LOCK_HELD) return;
  const d = dir();
  let said = false;
  mkdirSync(queueDir(), { recursive: true });
  const ticket = `${String(Date.now()).padStart(15, '0')}-${process.pid}`;
  const ticketPath = join(queueDir(), ticket);
  writeFileSync(ticketPath, `${process.pid} ${name} ${new Date().toISOString()}\n`);
  const dropTicket = () => { try { rmSync(ticketPath, { force: true }); } catch {} };
  process.on('exit', dropTicket);
  for (;;) {
    // first come, first served: wait while an older live ticket is queued
    const q = liveTickets();
    if (!q.includes(ticket)) writeFileSync(ticketPath, `${process.pid} ${name} ${new Date().toISOString()}\n`); // pruned by mistake: back in line
    if (q.some((t) => t < ticket)) {
      if (!said) { console.error(`waiting for the GPU suite lock (queued behind ${q.filter((t) => t < ticket).length} earlier suite(s))`); said = true; }
      await new Promise((r) => setTimeout(r, 1000));
      continue;
    }
    try {
      mkdirSync(d);
      dropTicket();
      writeFileSync(join(d, 'owner'), `${process.pid} ${name} ${new Date().toISOString()}\n`);
      process.env.CW_SUITE_LOCK_HELD = String(process.pid);
      const release = () => { try { if (readFileSync(join(d, 'owner'), 'utf8').startsWith(process.pid + ' ')) rmSync(d, { recursive: true }); } catch {} };
      process.on('exit', release);
      // a stopped suite takes its tests with it (this handler runs before the guards' and exits)
      for (const s of ['SIGINT', 'SIGTERM', 'SIGHUP'])
        process.on(s, () => { killTree(); release(); process.exit(128 + (s === 'SIGINT' ? 2 : s === 'SIGTERM' ? 15 : 1)); });
      // a hold limit: one long suite must not starve the queue behind it (2026-10-27: a 2 hour suite held 16 others).
      // CW_SUITE_MAX_MINUTES (default 15, 0 = none, fractions allowed): the holder's tests are killed, the lock released
      // and the process exits 124 with a message that says why; split the work into suites that fit
      const maxMin = Number(process.env.CW_SUITE_MAX_MINUTES ?? 15);
      if (maxMin > 0) {
        const limit = setTimeout(() => {
          console.error(`GPU suite '${name}' held the lock for ${maxMin} minute(s) (CW_SUITE_MAX_MINUTES): stopping it and releasing the lock so the ` +
            'suites waiting behind it run. Split the work into suites that finish inside the limit (one gate or a few per call).');
          killTree(); release(); process.exit(124);
        }, maxMin * 60000);
        limit.unref();
      }
      return;
    } catch (e) {
      if (e.code !== 'EEXIST') throw new Error(`GPU suite lock ${d}: ${e.message}`);
      // not a directory (a plain file someone made with touch or flock): this lock is never a file. Stale
      // when no process has it open; otherwise fail fast rather than wait on it forever.
      let isDir = true;
      try { isDir = statSync(d).isDirectory(); } catch { continue; } // gone meanwhile: try again
      if (!isDir) {
        const users = openers(d);
        if (users.length)
          throw new Error(`GPU suite lock ${d} is a plain file, not this lock's directory, and is open in pid ${users.join(', ')}: ` +
            'something else uses that path; stop it or remove the file');
        console.error(`GPU suite lock ${d} was a plain file nobody had open: removed it`);
        try { rmSync(d, { force: true }); } catch {}
        continue;
      }
      let owner = '';
      try { owner = readFileSync(join(d, 'owner'), 'utf8').trim(); } catch {}
      const pid = parseInt(owner, 10);
      if (owner && !alive(pid)) { try { rmSync(d, { recursive: true }); } catch {} continue; }
      // a directory with no owner: a suite between mkdir and writing its owner takes milliseconds;
      // one that stays ownerless (a holder killed in between) is stale after OWNERLESS_MS
      if (!owner) {
        let age = 0;
        try { age = Date.now() - statSync(d).mtimeMs; } catch { continue; }
        if (age > OWNERLESS_MS) {
          console.error(`GPU suite lock ${d} had no owner for ${Math.round(age / 1000)} s: removed it as stale`);
          try { rmSync(d, { recursive: true }); } catch {}
          continue;
        }
      }
      if (!said) { console.error(`waiting for the GPU suite lock (held by ${owner || 'a starting suite'})`); said = true; }
      await new Promise((r) => setTimeout(r, 200)); // first in line: take it the moment it frees
    }
  }
}
