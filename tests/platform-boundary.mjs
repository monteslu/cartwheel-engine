// Platform boundary: only the platform layer (src/platform) talks to SDL.
// Every other engine module reaches the OS through cartwheel/platform.h or
// src/platform/hooks.h, so a module never depends on SDL directly (the l10n
// module once read the system locale with SDL itself). Vendored code under
// src/third_party is exempt.
//   Control that MUST fail: the same scan over a synthetic module that
//   includes SDL.
//
//   node tests/platform-boundary.mjs
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const SRC = join(ROOT, 'src');
const EXEMPT = ['platform', 'third_party'];
const SDL_INCLUDE = /^\s*#\s*include\s*[<"](SDL2?\/)?SDL[^>"]*\.h[>"]/m;

/** @param {string} dir @returns {string[]} */
const walk = (dir) => readdirSync(dir).flatMap((n) => {
  const p = join(dir, n);
  return statSync(p).isDirectory() ? walk(p) : /\.(c|h)$/.test(n) ? [p] : [];
});

/** @param {{path:string,text:string}[]} files @returns {string[]} offenders */
const scan = (files) => files.filter((f) => SDL_INCLUDE.test(f.text)).map((f) => f.path);

let failed = 0;
const check = (name, ok, detail = '') => {
  console.log(ok ? `ok   ${name}` : `FAIL ${name}${detail ? ': ' + detail : ''}`);
  if (!ok) failed++;
};

const files = walk(SRC)
  .filter((p) => !EXEMPT.includes(relative(SRC, p).split(/[\\/]/)[0]))
  .map((p) => ({ path: relative(ROOT, p), text: readFileSync(p, 'utf8') }));
check(`${files.length} engine sources outside src/platform`, files.length > 50);
const bad = scan(files);
check('no engine module outside src/platform includes SDL', bad.length === 0, bad.join(', '));
// control: a module that includes SDL must be caught (both spellings)
const ctl = scan([{ path: 'src/l10n/fake.c', text: '#include <stdio.h>\n#include <SDL.h>\n' },
                  { path: 'src/ui/fake.c', text: '#include "SDL2/SDL_locale.h"\n' }]);
check('control: an SDL include outside src/platform is caught', ctl.length === 2, JSON.stringify(ctl));
process.exit(failed ? 1 : 0);
