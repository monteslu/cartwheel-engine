// Every romdev loadMedia in this repository's tools and tests names the test
// GPU's power preference (gpuPower), GL (wgpu:false) loads included: without
// it romdev's host takes the high-performance adapter, on a two-GPU desktop
// the GPU driving the display (a test there has hung the desktop before).
// The check reads each loadMedia call's argument object in tools/ and tests/
// and fails on one with no gpuPower. Control: a call written without it must
// be found.
//   node tests/romdev-load-args.mjs
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const SKIP = new Set(['node_modules', 'out', 'build', 'deps']);

/** the lines of the loadMedia calls in a source whose arguments name no gpuPower */
function unpinned(src) {
  const bad = [];
  const re = /(['"`])loadMedia\1\s*,\s*/g;
  for (let m; (m = re.exec(src));) {
    let i = m.index + m[0].length, arg;
    if (src[i] === '{') { // the object literal, braces balanced
      let d = 0, e = i;
      for (; e < src.length; e++) { if (src[e] === '{') d++; else if (src[e] === '}' && !--d) break; }
      arg = src.slice(i, e + 1);
    } else arg = src.slice(i, src.indexOf(')', i)); // a variable or call: only a helper naming gpuPower passes
    if (!/gpuPower/.test(arg)) bad.push(src.slice(0, m.index).split('\n').length);
  }
  return bad;
}

const files = [];
const walk = (d) => {
  for (const n of readdirSync(d)) {
    if (SKIP.has(n)) continue;
    const p = join(d, n);
    if (statSync(p).isDirectory()) walk(p);
    else if (/\.(mjs|js)$/.test(n)) files.push(p);
  }
};
walk(join(ROOT, 'tools'));
walk(join(ROOT, 'tests'));

let fail = 0, calls = 0;
for (const f of files) {
  const src = readFileSync(f, 'utf8');
  calls += (src.match(/(['"`])loadMedia\1\s*,/g) || []).length;
  for (const line of unpinned(src)) {
    console.log(`FAIL ${relative(ROOT, f)}:${line}: loadMedia with no gpuPower (romdev would take the display GPU)`);
    fail++;
  }
}
// control: a GL load without gpuPower must be found
if (unpinned("await call('load" + "Media', { platform: 'wasmcart', path, wgpu: false });").length !== 1) {
  console.log('FAIL control: a loadMedia with no gpuPower was not found: CONTROL PASSED');
  fail++;
}
console.log(fail ? `romdev-load-args: ${fail} problem(s)` : `ok   romdev-load-args: ${calls} loadMedia calls pass gpuPower; the control is caught`);
process.exit(fail ? 1 : 0);
