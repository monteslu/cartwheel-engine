// The HRTF table tool (tools/hrtf/import-kemar.mjs) against the table the
// engine embeds (src/audio/hrtf/kemar.cwhrtf):
//   1. it rebuilds the committed bytes exactly from deps/kemar (twice: a
//      reproducible build, and the file in the repository is what the tool
//      makes);
//   2. the table is sane: 711 directions, 1418 triangles, 64 taps at 48 kHz,
//      a left-right mirror (the direction 90 degrees right has the right
//      ear's delay 0 and the left ear's about 0.6 ms);
//   3. control: --taps 32 must give different bytes (a check that cannot
//      tell two tables apart would pass it).
import { spawnSync } from 'node:child_process';
import { readFileSync, mkdirSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'hrtf-tool');
mkdirSync(OUT, { recursive: true });
const tool = join(ROOT, 'tools/hrtf/import-kemar.mjs');
const committed = join(ROOT, 'src/audio/hrtf/kemar.cwhrtf');
const src = join(ROOT, 'deps/kemar');
if (!existsSync(src)) {
  console.log('FAIL hrtf-tool: deps/kemar is missing (scripts/fetch-deps.sh --only kemar)');
  process.exit(1);
}
let fail = 0;
const sha = (f) => createHash('sha256').update(readFileSync(f)).digest('hex');
const run = (out, extra = []) => {
  const r = spawnSync('node', [tool, src, out, ...extra], { encoding: 'utf8' });
  if (r.status !== 0) { fail++; console.log(`FAIL import-kemar: ${(r.stderr || '').trim()}`); }
  return existsSync(out) ? sha(out) : '';
};
const a = run(join(OUT, 'a.cwhrtf')), b = run(join(OUT, 'b.cwhrtf'));
if (!a || a !== b) { fail++; console.log('FAIL two runs differ'); }
if (a !== sha(committed)) { fail++; console.log(`FAIL the tool makes ${a.slice(0, 12)}, the repository has ${sha(committed).slice(0, 12)}`); }

const t = readFileSync(committed);
const [rate, taps, count, tris] = [8, 12, 16, 20].map((o) => t.readUInt32LE(o));
if (t.toString('ascii', 0, 8) !== 'CWHRTF01' || rate !== 48000 || taps !== 64 || count !== 711 || tris !== 1418) {
  fail++;
  console.log(`FAIL header: ${rate} Hz, ${taps} taps, ${count} directions, ${tris} triangles`);
}
// the direction nearest (1, 0, 0): the listener's right
let best = -1, bestDot = -2;
for (let d = 0; d < count; d++) {
  const o = 32 + d * 12, x = t.readInt16LE(o) / 32767, y = t.readInt16LE(o + 2) / 32767, z = t.readInt16LE(o + 4) / 32767;
  if (x > bestDot && Math.abs(y) < 0.01 && Math.abs(z) < 0.01) { bestDot = x; best = d; }
}
const dl = t.readInt16LE(32 + best * 12 + 6) / 256, dr = t.readInt16LE(32 + best * 12 + 8) / 256;
if (best < 0 || dr !== 0 || dl / 48000 < 0.0005 || dl / 48000 > 0.0008) {
  fail++;
  console.log(`FAIL 90 degrees right: left delay ${dl} frames, right ${dr}`);
}
const ctl = run(join(OUT, 'control.cwhrtf'), ['--taps', '32']);
if (ctl === a) { fail++; console.log('FAIL CONTROL PASSED: --taps 32 made the same bytes'); }
console.log(fail ? `FAIL hrtf-tool (${fail})` : `ok   hrtf-tool (${a.slice(0, 12)}, right ear leads by ${(dl / 48).toFixed(3)} ms at 90 degrees)`);
process.exit(fail ? 1 : 0);
