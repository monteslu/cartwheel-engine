// The gate's input script (tests/platformer3d-input.json) as the C table the
// _auto build compiles in (games/platformer3d/script.inc). Rerun after
// editing the JSON:  node games/platformer3d/tools/gen-script.mjs
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..', '..', '..');
const BITS = { south: 0, east: 1, west: 2, north: 3, back: 4, guide: 5, start: 6, l3: 7, r3: 8, l: 9, r: 10, up: 11, down: 12, left: 13, right: 14 };
const script = JSON.parse(readFileSync(join(ROOT, 'tests', 'platformer3d-input.json'), 'utf8'));
const lines = script.map((s) => {
  const mask = (s.buttons || []).reduce((m, b) => m | (1 << BITS[b]), 0);
  const ax = s.axes || {};
  const fl = (v) => { const t = String(v || 0); return (/[.e]/.test(t) ? t : t + '.0') + 'f'; };
  if (s.keys || s.mouse || s.touches) throw new Error('the autopilot carries pad input only');
  return `  { ${s.frame}, ${mask}u, ${fl(ax.lx)}, ${fl(ax.ly)} },`;
});
writeFileSync(join(HERE, '..', 'script.inc'), `/* generated from tests/platformer3d-input.json by tools/gen-script.mjs */\n${lines.join('\n')}\n`);
console.log(`script.inc: ${lines.length} entries`);
