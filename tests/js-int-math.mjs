// Fails on 32-bit integer maths done in doubles: a multiplication by a large
// constant inside an expression that is then truncated to 32 bits (`>>> 0`,
// `| 0`), without Math.imul. The product overflows 2^53, low bits are lost,
// and a "random" generator collapses into a short cycle (a make-audio.mjs
// LCG looped every 419 samples: every noise sound became a 114.6 Hz buzz).
// Use Math.imul or tools/lib/rng.mjs. A line that really means it carries
// `int-math-ok: <reason>` (on it, or on the line just above).
//
//   node tests/js-int-math.mjs [dir ...]
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const SKIP = new Set(['node_modules', 'deps', 'build', 'out', '.git', 'vendor', 'third_party']);
const roots = process.argv.slice(2).length ? process.argv.slice(2).map((d) => resolve(d)) : [ROOT];

// a constant of 2^21 or more (7+ digits, or hex with 6+ digits) multiplied in
const BIG_MUL = /(?<![\w.])(?:[\w$\])]+)\s*\*\s*(?:\d{7,}|0x[0-9a-f]{6,})\b|(?:\d{7,}|0x[0-9a-f]{6,})\s*\*\s*[\w$(]/i;
const TRUNC = />>>\s*0|\|\s*0\b|>>>\s*\d/;

const bad = [];
function walk(dir) {
  for (const name of readdirSync(dir)) {
    if (SKIP.has(name)) continue;
    const p = join(dir, name);
    let st;
    try { st = statSync(p); } catch { continue; }
    if (st.isDirectory()) { walk(p); continue; }
    if (!/\.(mjs|cjs|js)$/.test(name) || name.endsWith('.min.js')) continue;
    const lines = readFileSync(p, 'utf8').split('\n');
    lines.forEach((line, i) => {
      if (line.includes('int-math-ok') || (i > 0 && lines[i - 1].includes('int-math-ok')) || /^\s*(\/\/|\*)/.test(line)) return;
      const code = line.replace(/Math\.imul\([^()]*(?:\([^()]*\)[^()]*)*\)/g, 'IMUL');
      if (BIG_MUL.test(code) && TRUNC.test(code)) bad.push(`${relative(ROOT, p)}:${i + 1}: ${line.trim()}`);
    });
  }
}
for (const r of roots) walk(r);
if (bad.length) {
  console.error(`js-int-math: ${bad.length} 32-bit multiplication(s) done in doubles (they overflow 2^53 and lose bits;`);
  console.error('use Math.imul(a, b) or tools/lib/rng.mjs, or mark a deliberate one with `int-math-ok: <reason>`):');
  for (const b of bad) console.error('  ' + b);
  process.exit(1);
}
console.log('js-int-math: ok');
