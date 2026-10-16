// An exclusive lock on a build tree, the same one scripts/lib/build-lock.sh
// takes (scripts/build.sh, build-sdk.sh, build-sdl2-cart.sh, sanitize.sh).
import { mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { join, dirname } from 'node:path';

/**
 * Run fn holding an exclusive lock on a build tree. Two builds of one project
 * at once (an editor's Build while a test or a script builds, or a cart build
 * that builds the native tree for its cwpack while a native build runs) would
 * otherwise run two ninjas in one tree: they race on the same outputs and a
 * link fails now and then. The lock is a directory (mkdir is atomic) holding
 * the owner's pid; a lock whose owner is gone is taken over.
 * @template T @param {string} bdir @param {() => Promise<T>} fn @returns {Promise<T>}
 */
export async function withBuildLock(bdir, fn) {
  const lock = `${bdir}.lock`;
  mkdirSync(dirname(lock), { recursive: true });
  let said = false;
  for (;;) {
    try {
      mkdirSync(lock);
      writeFileSync(join(lock, 'pid'), String(process.pid));
      break;
    } catch (e) {
      if (e.code !== 'EEXIST') throw e;
      let pid = 0;
      try { pid = +readFileSync(join(lock, 'pid'), 'utf8'); } catch { /* being written */ }
      let alive = true;
      if (pid) try { process.kill(pid, 0); } catch (k) { alive = k.code === 'EPERM'; }
      if (pid && !alive) { rmSync(lock, { recursive: true, force: true }); continue; }
      if (!said) { console.log(`== waiting for another build of ${bdir} (pid ${pid || '?'})`); said = true; }
      await new Promise((r) => setTimeout(r, 250));
    }
  }
  try { return await fn(); } finally { rmSync(lock, { recursive: true, force: true }); }
}
