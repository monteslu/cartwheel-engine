// Stage a cart's asset directory for wasmcart-pack and add the bundle
// manifest: cwfiles.txt, one asset path per line (sorted), the files
// wasmcart-pack will ship. A cart has no directory API, so this list is how
// cw_vfs_list_dir (cartwheel/vfs.h) lists the bundle; no ABI is involved.
//
//   node tools/stage-assets.mjs <asset dir> <stage dir>
//
// The stage directory holds a symlink per top-level entry of the asset
// directory (wasmcart-pack follows them) plus cwfiles.txt; it is rebuilt from
// scratch every run, so removed assets never linger.
import { readdirSync, statSync, mkdirSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const [src, dst] = process.argv.slice(2).map((p) => resolve(p));
if (!src || !dst) { console.error('usage: stage-assets.mjs <asset dir> <stage dir>'); process.exit(2); }

// the same walk wasmcart-pack does: dotfiles skipped, symlinks followed
function walk(dir, base, out) {
  for (const name of readdirSync(dir)) {
    if (name.startsWith('.')) continue;
    const full = join(dir, name), rel = base ? `${base}/${name}` : name;
    let st;
    try { st = statSync(full); } catch { continue; }
    if (st.isDirectory()) walk(full, rel, out);
    else if (st.isFile()) out.push(rel);
  }
  return out;
}

rmSync(dst, { recursive: true, force: true });
mkdirSync(dst, { recursive: true });
for (const name of readdirSync(src)) {
  if (name.startsWith('.') || name === 'cwfiles.txt') continue;
  symlinkSync(join(src, name), join(dst, name));
}
const files = walk(src, '', []).filter((f) => f !== 'cwfiles.txt').sort();
writeFileSync(join(dst, 'cwfiles.txt'), files.join('\n') + (files.length ? '\n' : ''));
