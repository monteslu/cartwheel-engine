// The part names the engine owns (src/render/shading.c ENGINE_PARTS, which a game's registration of is refused) are
// exactly its extension modules (src/<module>/extensions/<part>.mjs, a helper with an underscore aside), each under its
// module. A new engine part not added to the list could be taken by a game again without a word.
// CONTROL that must fail: the same comparison with one module left out of the list.
//   node tests/shading-names.mjs
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const src = readFileSync(join(ROOT, 'src', 'render', 'shading.c'), 'utf8');
const block = /ENGINE_PARTS\[\] = \{([\s\S]*?)\n\};/.exec(src);
if (!block) { console.log('FAIL no ENGINE_PARTS list in src/render/shading.c'); process.exit(1); }
const listed = [...block[1].matchAll(/\{ "([a-z0-9]+)", "([a-z0-9]+)" \}/g)].map((m) => `${m[2]}/${m[1]}`);
const files = readdirSync(join(ROOT, 'src'), { withFileTypes: true }).filter((d) => d.isDirectory())
  .flatMap((d) => { const dir = join(ROOT, 'src', d.name, 'extensions'); return existsSync(dir) ? readdirSync(dir).filter((f) => /^[a-z0-9]+\.mjs$/.test(f)).map((f) => `${d.name}/${f.slice(0, -4)}`) : []; });
const differ = (list) => [files.filter((f) => !list.includes(f)), list.filter((l) => !files.includes(l))];
const [missing, extra] = differ(listed);
let failed = 0;
if (missing.length || extra.length) {
  failed++;
  console.log(`FAIL the engine's part list and its extension modules differ: not listed ${missing.join(', ') || '-'}; listed without a module ${extra.join(', ') || '-'}`);
} else console.log(`ok   the engine owns ${listed.length} part names, one per extension module`);
const [cm] = differ(listed.slice(0, -1));
if (cm.length) console.log(`ok   control: the list less ${cm.join(', ')} is caught`);
else { failed++; console.log('FAIL CONTROL PASSED: the list less one module matched the modules'); }
process.exit(failed ? 1 : 0);
