// Regenerates the streaming suite's cell files (made here, no third-party
// content): node tests/unit/stream_assets_gen.mjs
// An 8 x 8 world: cells/c_<x>_<z>.cell.zst, each a zstd frame of a short
// text: "cell <x> <z> <checksum>\n" and a deterministic filler line.
import { writeFileSync, mkdirSync } from 'node:fs';
import { zstdCompressSync } from 'node:zlib';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const D = join(dirname(fileURLToPath(import.meta.url)), 'stream_assets', 'cells');
mkdirSync(D, { recursive: true });
for (let z = 0; z < 8; z++)
  for (let x = 0; x < 8; x++) {
    const sum = (x * 131 + z * 977 + 17) % 65521;
    const filler = Array.from({ length: 64 }, (_, i) => String.fromCharCode(97 + ((x * 7 + z * 13 + i * 5) % 26))).join('');
    writeFileSync(join(D, `c_${x}_${z}.cell.zst`), zstdCompressSync(Buffer.from(`cell ${x} ${z} ${sum}\n${filler}\n`)));
  }
