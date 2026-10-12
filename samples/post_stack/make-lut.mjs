// Writes assets/grade.cube: a generated 17^3 "teal shadows, warm highlights"
// colour grade (CC0, made by this script) for the post_stack sample's LUT
// pass. Input and output are display values (sRGB-encoded) in 0..1.
//   node samples/post_stack/make-lut.mjs
import { writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const N = 17;
const clamp = (x) => Math.min(1, Math.max(0, x));
const lines = ['TITLE "cartwheel post_stack teal-warm"', `LUT_3D_SIZE ${N}`, 'DOMAIN_MIN 0 0 0', 'DOMAIN_MAX 1 1 1'];
for (let b = 0; b < N; b++)
  for (let g = 0; g < N; g++)
    for (let r = 0; r < N; r++) {
      let c = [r / (N - 1), g / (N - 1), b / (N - 1)];
      const l = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
      // split toning: shadows toward teal, highlights toward warm
      const sh = (1 - l) * (1 - l), hi = l * l;
      c = [c[0] - 0.06 * sh + 0.08 * hi, c[1] + 0.02 * sh + 0.03 * hi, c[2] + 0.08 * sh - 0.07 * hi];
      // a gentle S-curve and a little extra saturation
      c = c.map((x) => x + 0.12 * Math.sin(2 * Math.PI * x) * -0.5 * 0.5);
      const l2 = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
      c = c.map((x) => clamp(l2 + (x - l2) * 1.15));
      lines.push(c.map((x) => x.toFixed(6)).join(' '));
    }
writeFileSync(join(dirname(fileURLToPath(import.meta.url)), 'assets', 'grade.cube'), lines.join('\n') + '\n');
