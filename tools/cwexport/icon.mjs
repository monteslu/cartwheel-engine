// The app icon an export ships: the project's `icon` key (a PNG, relative to
// the project), else a generated placeholder (a wheel), so every export has
// one without a binary file in the repo.
import { existsSync, copyFileSync } from 'node:fs';
import { join, isAbsolute } from 'node:path';
import { writePng, readImage } from '../lib/png.mjs';
import { ExportError, mkdirp } from './util.mjs';
import { dirname } from 'node:path';

/** Draw the placeholder icon (size x size RGBA). */
function wheel(size) {
  const px = new Uint8Array(size * size * 4);
  const c = (size - 1) / 2, R = size * 0.46;
  for (let y = 0; y < size; y++)
    for (let x = 0; x < size; x++) {
      const dx = x - c, dy = y - c, r = Math.hypot(dx, dy), a = Math.atan2(dy, dx);
      const i = (y * size + x) * 4;
      let col = null;
      if (r < R) col = [32, 40, 56];
      if (r < R && r > R * 0.78) col = [245, 182, 39]; // rim
      if (r < R * 0.2) col = [245, 182, 39]; // hub
      const spoke = Math.abs(((a / (Math.PI / 3)) % 1 + 1) % 1 - 0.5) * (Math.PI / 3) * r;
      if (r < R * 0.8 && spoke > (Math.PI / 6) * r - size * 0.025) col = [245, 182, 39];
      if (col) { px[i] = col[0]; px[i + 1] = col[1]; px[i + 2] = col[2]; px[i + 3] = 255; }
    }
  return px;
}

/**
 * Write the icon PNG to dst.
 * @param {{projectDir: string, keys: Record<string,string>}} ctx
 */
export function writeIcon(ctx, dst, size = 256) {
  mkdirp(dirname(dst));
  const own = ctx.opt('icon');
  if (own) {
    const p = isAbsolute(own) ? own : join(ctx.projectDir, own);
    if (!existsSync(p)) throw new ExportError(`icon ${own}: no such file (relative to ${ctx.projectDir})`);
    readImage(p); // must decode
    copyFileSync(p, dst);
    return dst;
  }
  writePng(dst, size, size, wheel(size));
  return dst;
}
