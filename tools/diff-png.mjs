// node tools/diff-png.mjs a.png|ppm b.png|ppm  -> JSON diff summary; exit 1 if not comparable
import { readImage, diffImages } from './lib/png.mjs';
const r = diffImages(readImage(process.argv[2]), readImage(process.argv[3]));
console.log(JSON.stringify(r));
process.exit(r.comparable ? 0 : 1);
