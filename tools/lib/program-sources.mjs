// What a program table was generated from: the directories of the extension modules its states use, each as one
// hash over the directory's .mjs files (an extension, the helpers it imports and the parts beside it). tools/programs.mjs
// stamps a table's generated C with them; cmake/programs.cmake compares the stamp with the sources the build has (the
// engine tree's files, or an SDK's cmake/program-sources.txt, which tools/sdk-pack.mjs writes) and fails on a
// difference: a table generated before an extension changed draws with the old programs.
//
// The hash, which cmake/programs.cmake computes the same way: SHA-256 over, for each .mjs file of the directory in
// byte order of its name, the name, a newline, the file with CRLF taken to LF, and a newline.
import { createHash } from 'node:crypto';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { join, relative } from 'node:path';

/** @param {string} dir @returns {string} the directory's hash (hex) */
export function dirHash(dir) {
  const h = createHash('sha256');
  const files = readdirSync(dir).filter((f) => f.endsWith('.mjs')).sort(); // (ASCII names: code unit order is byte order)
  for (const f of files) h.update(f + '\n' + readFileSync(join(dir, f), 'utf8').replace(/\r\n/g, '\n') + '\n');
  return h.digest('hex');
}

/** @param {string} root the engine tree @returns {string[]} its extension directories, relative (src/<module>/extensions) */
export function engineExtensionDirs(root) {
  return readdirSync(join(root, 'src'), { withFileTypes: true }).filter((d) => d.isDirectory())
    .map((d) => `src/${d.name}/extensions`).filter((d) => existsSync(join(root, d))).sort();
}

/** the line of a stamp: kind is engine (relative to the engine tree) or table (relative to the manifest's directory) */
export const stampLine = (kind, rel, hash) => `/* cw-source ${kind} ${rel} ${hash} */`;

/**
 * The stamp of a table whose states use these extension modules.
 * @param {string[]} modules absolute paths of the extension modules the states use
 * @param {string} root the engine tree
 * @param {string} tableDir the manifest's directory
 * @returns {string} the stamp's lines (each ending in a newline), a "stamped" line first
 */
export function tableStamp(modules, root, tableDir) {
  const dirs = new Map();
  for (const m of modules) {
    const dir = join(m, '..');
    const rel = relative(root, dir);
    const engine = !rel.startsWith('..') && /^src\/[^/]+\/extensions$/.test(rel.split('\\').join('/'));
    dirs.set(dir, engine ? ['engine', rel.split('\\').join('/')] : ['table', relative(tableDir, dir).split('\\').join('/') || '.']);
  }
  const lines = ['/* cw-sources stamped (tools/programs.mjs): regenerate the table when one of these changes */'];
  for (const [dir, [kind, rel]] of [...dirs].sort((a, b) => a[1][1].localeCompare(b[1][1]))) lines.push(stampLine(kind, rel, dirHash(dir)));
  return lines.map((l) => l + '\n').join('');
}
