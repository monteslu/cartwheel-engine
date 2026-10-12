// The l10n demo's per-locale banners (CC0: made here, from code). Each
// locale gets a different pattern so a frame shows which variant
// cw_l10n_asset_path picked.
//   node samples/l10n_demo/make-art.mjs
import { mkdirSync, writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writePng } from '../../tools/lib/png.mjs';

const P = join(dirname(fileURLToPath(import.meta.url)), 'project');
const W = 200, H = 38;
function banner(path, paint) {
  const px = new Uint8Array(W * H * 4);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const [r, g, b] = paint(x, y), i = 4 * (y * W + x);
    px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255;
  }
  mkdirSync(dirname(path), { recursive: true });
  writePng(path, W, H, px);
}
// the base (English): blue with light diagonal stripes
banner(join(P, 'textures/banner.png'), (x, y) => ((x + y) >> 3) % 2 ? [52, 98, 200] : [196, 214, 246]);
// Arabic: teal with a dot grid
banner(join(P, 'l10n/assets/ar/textures/banner.png'), (x, y) => {
  const dx = (x % 16) - 8, dy = (y % 16) - 8;
  return dx * dx + dy * dy < 14 ? [240, 220, 120] : [18, 122, 116];
});
// Japanese: white with red rings
banner(join(P, 'l10n/assets/ja/textures/banner.png'), (x, y) => {
  const dx = (x % 38) - 19, dy = y - 19, d = Math.sqrt(dx * dx + dy * dy);
  return d > 9 && d < 14 ? [206, 32, 48] : [248, 246, 240];
});
