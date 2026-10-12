// Scratch directories that remove themselves. A run makes a few (the input
// script, the pinned-Chromium wrapper, wasmcart-run's working copy); made with
// a bare mkdtempSync they outlived every run, and the gates left gigabytes of
// them in out/. tempDir() makes one under `parent` and deletes it when the
// process exits, however it exits (an uncaught error included; a signal exits
// through process.exit in the suites' handlers).
import { mkdtempSync, mkdirSync, rmSync } from 'node:fs';
import { join } from 'node:path';

const dirs = new Set();
let hooked = false;
const cleanup = () => { for (const d of dirs) { try { rmSync(d, { recursive: true, force: true }); } catch { /* best effort */ } } dirs.clear(); };

/** a fresh directory `<parent>/<prefix>XXXXXX`, removed at process exit */
export function tempDir(parent, prefix) {
  mkdirSync(parent, { recursive: true });
  const d = mkdtempSync(join(parent, prefix));
  dirs.add(d);
  if (!hooked) {
    hooked = true;
    process.on('exit', cleanup);
    for (const sig of ['SIGINT', 'SIGTERM', 'SIGHUP'])
      if (!process.listenerCount(sig)) process.once(sig, () => { cleanup(); process.exit(128 + ({ SIGHUP: 1, SIGINT: 2, SIGTERM: 15 })[sig]); });
  }
  return d;
}
