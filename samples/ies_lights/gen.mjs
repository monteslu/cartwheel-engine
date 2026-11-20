// Generates the sample's IES photometric files (CC0: made here, from code; no fixture's data).
//   node samples/ies_lights/gen.mjs
// project/lights/ring.ies     one horizontal angle: a soft centre inside a bright ring (a downlight with a reflector)
// project/lights/street.ies   0 to 180 degrees of twist, mirrored by the loader: a street lamp that throws forward
// project/lights/lantern.ies  0 to 90 degrees, mirrored to four lobes, tilt 0 to 180: a lantern lit all round, for an omni light
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const D = join(dirname(fileURLToPath(import.meta.url)), 'project', 'lights');
mkdirSync(D, { recursive: true });
const g = (x, c, w) => Math.exp(-((x - c) * (x - c)) / (2 * w * w));
function ies(name, note, ver, hor, cd) {
  const rows = hor.map((h) => ver.map((v) => Math.max(0, Math.round(cd(v, h)))).join(' '));
  const text = ['IESNA:LM-63-2002', `[TEST] cartwheel samples/ies_lights ${name}`, `[OTHER] ${note}`, 'TILT=NONE',
    `1 1000 1 ${ver.length} ${hor.length} 1 2 0 0 0`, '1 1 100', ver.join(' '), hor.join(' '), ...rows, ''].join('\n');
  writeFileSync(join(D, name + '.ies'), text);
  console.log(`wrote lights/${name}.ies: ${ver.length} x ${hor.length} angles`);
}
const step = (a, b, s) => { const o = []; for (let v = a; v <= b; v += s) o.push(v); return o; };
ies('ring', 'generated, CC0', step(0, 90, 5), [0], (v) => 260 * g(v, 0, 9) + 1000 * g(v, 27, 5) + 90 * g(v, 48, 10));
ies('street', 'generated, CC0', step(0, 90, 5), step(0, 180, 15),
  (v, h) => { const f = 0.5 + 0.5 * Math.cos((h * Math.PI) / 180); return 180 * g(v, 0, 14) + 1000 * f * f * g(v, 58, 9) + 120 * (1 - f) * g(v, 25, 10); });
ies('lantern', 'generated, CC0', step(0, 180, 10), step(0, 90, 15),
  (v, h) => { const bars = 0.25 + 0.75 * Math.abs(Math.cos((h * Math.PI) / 90)) ** 6; return 1000 * bars * (0.35 + 0.65 * g(v, 95, 28)) * (1 - g(v, 180, 14)); });
