// A perf-frame report as a table: median per app / host / gpu / build of wall,
// frame CPU (Chromium) and the engine's render and sim CPU.
//   node tools/lib/perf-table.mjs out/perf-frame/report.json
import { readFileSync } from 'node:fs';
const r = JSON.parse(readFileSync(process.argv[2], 'utf8'));
const med = (a) => { const s = a.filter((x) => x != null).sort((x, y) => x - y); return s.length ? s[Math.floor((s.length - 1) / 2)] : null; };
const groups = new Map();
for (const x of r.rows) {
  const k = `${x.app}|${x.host}|${x.gpu}`;
  if (!groups.has(k)) groups.set(k, new Map());
  const g = groups.get(k);
  if (!g.has(x.build)) g.set(x.build, []);
  g.get(x.build).push(x);
}
const builds = [...new Set(r.rows.map((x) => x.build))];
console.log(['app', 'host', 'gpu', ...builds.map((b) => `${b} wall/cpu/render/sim/gpu`)].join('\t'));
for (const [k, g] of groups) {
  const cells = builds.map((b) => {
    const xs = g.get(b) || [];
    const f = (n) => { const v = med(xs.map((x) => x[n])); return v == null ? '-' : v.toFixed(2); };
    return `${f('wall')}/${f('frame_cpu')}/${f('render')}/${f('sim')}/${f('gpu_ms')}`;
  });
  console.log([...k.split('|'), ...cells].join('\t'));
}
