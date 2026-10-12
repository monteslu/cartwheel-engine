// Summarise a .cpuprofile written by tools/perf-frame.mjs --profile: self time per
// function (ms per frame when --frames is given), and for a function name the callers
// that spend the most in it (--callers name).
//   node tools/lib/cpuprofile-top.mjs out/perf-frame/x.cpuprofile [--frames 600] [--top 40] [--callers name]
import { readFileSync } from 'node:fs';
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 ? argv[i + 1] : d; };
const p = JSON.parse(readFileSync(argv[0], 'utf8'));
const frames = +opt('frames', 1), top = +opt('top', 40), callers = opt('callers', '');
const byId = new Map(p.nodes.map((n) => [n.id, n]));
const parent = new Map();
for (const n of p.nodes) for (const c of n.children || []) parent.set(c, n.id);
const self = {}, incl = {}, via = {};
const name = (n) => `${n.callFrame.functionName || '(anon)'}${n.callFrame.url && !n.callFrame.url.startsWith('wasm') ? ' ' + n.callFrame.url.split('/').pop() + ':' + n.callFrame.lineNumber : ''}`;
for (let i = 0; i < p.samples.length; i++) {
  const ms = (p.timeDeltas[i + 1] ?? 0) / 1000;
  const n = byId.get(p.samples[i]);
  const nm = name(n);
  self[nm] = (self[nm] || 0) + ms;
  const seen = new Set();
  let leafHit = false;
  for (let cur = n.id, prev = null; cur != null; prev = cur, cur = parent.get(cur)) {
    const c = byId.get(cur), cn = name(c);
    if (!seen.has(cn)) { incl[cn] = (incl[cn] || 0) + ms; seen.add(cn); }
    if (callers && cn.startsWith(callers) && !leafHit) {
      leafHit = true;
      // the chain of 4 callers above it
      const chain = [];
      for (let k = parent.get(cur); k != null && chain.length < 4; k = parent.get(k)) chain.push(byId.get(k).callFrame.functionName);
      const key = chain.join(' < ');
      via[key] = (via[key] || 0) + ms;
    }
  }
}
const show = (o) => Object.entries(o).sort((a, b) => b[1] - a[1]).slice(0, top).map(([k, v]) => `${(v / frames).toFixed(3).padStart(8)}  ${k}`).join('\n');
console.log('== self (ms/frame)\n' + show(self));
console.log('\n== inclusive (ms/frame)\n' + show(incl));
if (callers) console.log(`\n== callers of ${callers}\n` + show(via));
