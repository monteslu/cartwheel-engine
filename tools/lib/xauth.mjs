// X authority for native hosts launched from a shell that has none (an
// agent's or CI shell on a GNOME/Wayland desktop): without it an X client
// such as wasmcart-run waits on the display instead of failing.
import { readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';

/**
 * The newest mutter Xwayland auth file of this user, or '' when there is none.
 * @returns {string}
 */
export function findXauthority() {
  const dir = `/run/user/${process.getuid ? process.getuid() : 1000}`;
  try {
    return readdirSync(dir).filter((f) => f.startsWith('.mutter-Xwaylandauth'))
      .map((f) => join(dir, f)).sort((a, b) => statSync(b).mtimeMs - statSync(a).mtimeMs)[0] || '';
  } catch { return ''; }
}

/**
 * env with XAUTHORITY filled in when it is unset and one can be found, or
 * replaced when it names an older mutter auth file than the newest (mutter
 * writes a new one when Xwayland restarts; a shell started before that keeps
 * the stale cookie, and wasmcart-run then hangs on the display).
 * @param {Record<string, string | undefined>} env
 */
export function withXauthority(env) {
  const cur = env.XAUTHORITY || '';
  const newest = findXauthority();
  if (newest && (!cur || (cur.includes('.mutter-Xwaylandauth') && cur !== newest))) env.XAUTHORITY = newest;
  return env;
}
