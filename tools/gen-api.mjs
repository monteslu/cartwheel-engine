// API docs as data: every public header's functions, types, macros and the
// comment above each, as JSON (docs/api.json) for agents and the editor.
// The headers are the documentation; this only makes them queryable.
//
//   node tools/gen-api.mjs            write docs/api.json
//   node tools/gen-api.mjs --check    exit 1 if docs/api.json, docs/editor_api.json or
//                                     docs/nodes.json is stale, or if a public function
//                                     (cartwheel/*.h or cwed/*.h) has no comment of its
//                                     own and is not in docs/api-undocumented.txt
//
// The undocumented list is a ratchet: it may only shrink. A function listed
// there that now has a comment fails --check too (delete its line), so the
// count can never grow back. It is empty today: every public function is
// documented, and a new one must arrive with its comment.
import { readFileSync, writeFileSync, readdirSync, existsSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const INC = join(ROOT, 'include', 'cartwheel');

function strip(c) {
  return c.replace(/^\/\*+|\*+\/$/g, '').split('\n').map((l) => l.replace(/^\s*\* ?/, '').trimEnd()).join('\n').trim();
}

/** Parse one header: { header, summary, functions, types, macros }. Also used by tools/gen-docs.mjs. */
export function parseHeader(file, dir = INC, prefix = 'cartwheel') {
  const src = readFileSync(join(dir, file), 'utf8');
  const lines = src.split('\n');
  const out = { header: `${prefix}/${file}`, summary: '', functions: [], types: [], macros: [] };
  const top = src.match(/^\/\*[\s\S]*?\*\//);
  if (top) out.summary = strip(top[0]);
  let pending = '';      // comment block right above the current line
  let blankSince = true;
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    const t = line.trim();
    if (t.startsWith('/*') && !t.startsWith('/**<')) {
      let block = line;
      while (!block.includes('*/') && i + 1 < lines.length) block += '\n' + lines[++i];
      const after = block.slice(block.indexOf('*/') + 2).trim();
      if (after) { pending = ''; continue; } // trailing code after the comment: not a doc block
      pending = strip(block.slice(block.indexOf('/*')));
      blankSince = false;
      continue;
    }
    if (t === '') { pending = blankSince ? '' : pending; blankSince = true; continue; }
    if (t.startsWith('#define ')) {
      const m = t.match(/^#define\s+([A-Za-z_]\w*)(\([^)]*\))?/);
      if (m && !m[1].endsWith('_H') && !m[1].startsWith('CW__')) out.macros.push({ name: m[1] + (m[2] || ''), doc: pending });
      pending = '';
      continue;
    }
    if (t.startsWith('typedef ') || /^(struct|enum)\s+\w+\s*\{/.test(t)) {
      let decl = t;
      let depth = (t.match(/\{/g) || []).length - (t.match(/\}/g) || []).length;
      while ((depth > 0 || !decl.includes(';')) && i + 1 < lines.length) {
        const nl = lines[++i];
        decl += '\n' + nl;
        depth += (nl.match(/\{/g) || []).length - (nl.match(/\}/g) || []).length;
      }
      const name = (decl.match(/\}\s*(\w+)\s*;\s*$/) || decl.match(/typedef[^;{]*?\b(\w+)\s*;/) || decl.match(/typedef .*\(\*(\w+)\)/) || [])[1];
      if (name) out.types.push({ name, doc: pending, decl: decl.length < 2000 ? decl : decl.slice(0, 2000) + ' ...' });
      pending = '';
      continue;
    }
    // function prototype (possibly multi-line), not static inline bodies
    if (/^[A-Za-z_][\w\s\*]*\b(cwed_\w+|cw_\w+|t3_\w+)\s*\(/.test(t) && !t.startsWith('return')) {
      let decl = t;
      while (!/\)\s*(__attribute__\(\(.*\)\))?\s*;\s*(\/\*.*\*\/)?\s*$/.test(decl) && !decl.includes('{') && i + 1 < lines.length) decl += ' ' + lines[++i].trim();
      const inline = decl.includes('{');
      if (inline) { // skip the body of a static inline
        let depth = (decl.match(/\{/g) || []).length - (decl.match(/\}/g) || []).length;
        while (depth > 0 && i + 1 < lines.length) { const nl = lines[++i]; depth += (nl.match(/\{/g) || []).length - (nl.match(/\}/g) || []).length; }
      }
      const m = decl.match(/\b(cwed_\w+|cw_\w+|t3_\w+)\s*\(/);
      const trailing = (decl.match(/;\s*\/\*\s*(.*?)\s*\*\/\s*$/) || [])[1] || '';
      const signature = (inline ? decl.slice(0, decl.indexOf('{')).trim() : decl.replace(/;\s*(\/\*.*\*\/)?\s*$/, '').trim())
        .replace(/\s+__attribute__\(\(.*\)\)/, '');
      out.functions.push({ name: m[1], signature, doc: [pending, trailing].filter(Boolean).join('\n'), inline });
      pending = '';
      continue;
    }
    pending = '';
  }
  return out;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main();

function main() {
const headers = readdirSync(INC).filter((f) => f.endsWith('.h')).sort();
const api = { generated_by: 'tools/gen-api.mjs', headers: headers.map((f) => parseHeader(f)) };
const text = JSON.stringify(api, null, 1) + '\n';
const outPath = join(ROOT, 'docs', 'api.json');
const undocOf = (a) => a.headers.flatMap((h) => h.functions.filter((f) => !f.doc && !f.name.startsWith('cw__')).map((f) => `${h.header}: ${f.name}`));
// node types: run the native gen_nodes tool (built by scripts/build.sh)
const genNodes = join(ROOT, 'build/native/gen_nodes');
let nodesText = null;
if (existsSync(genNodes)) {
  const r = spawnSync(genNodes, [], { encoding: 'utf8' });
  if (r.status === 0) nodesText = JSON.stringify(JSON.parse(r.stdout), null, 1) + '\n';
  else console.log('gen_nodes failed: ' + r.stderr);
} else console.log('build/native/gen_nodes missing: run scripts/build.sh for docs/nodes.json');
const nodesPath = join(ROOT, 'docs', 'nodes.json');
// the editor's extension API (editor/include/cwed): docs/editor_api.json
const EDINC = join(ROOT, 'editor', 'include', 'cwed');
const edApi = { generated_by: 'tools/gen-api.mjs', headers: readdirSync(EDINC).filter((f) => f.endsWith('.h')).sort().map((f) => parseHeader(f, EDINC, 'cwed')) };
const edText = JSON.stringify(edApi, null, 1) + '\n';
const edPath = join(ROOT, 'docs', 'editor_api.json');
const undocumented = [...undocOf(api), ...undocOf(edApi)];
const allowPath = join(ROOT, 'docs', 'api-undocumented.txt');
const allowed = new Set(existsSync(allowPath) ? readFileSync(allowPath, 'utf8').split('\n').map((l) => l.trim()).filter((l) => l && !l.startsWith('#')) : []);
const newUndoc = undocumented.filter((u) => !allowed.has(u));
const nowDocumented = [...allowed].filter((u) => !undocumented.includes(u));
if (process.argv.includes('--check')) {
  if (nodesText && (!existsSync(nodesPath) || readFileSync(nodesPath, 'utf8') !== nodesText)) { console.log('docs/nodes.json is stale: run node tools/gen-api.mjs'); process.exit(1); }
  const stale = !existsSync(outPath) || readFileSync(outPath, 'utf8') !== text || !existsSync(edPath) || readFileSync(edPath, 'utf8') !== edText;
  if (stale) console.log('docs/api.json or docs/editor_api.json is stale: run node tools/gen-api.mjs');
  if (newUndoc.length) console.log(`${newUndoc.length} public function(s) without a comment of their own (put a /* ... */ directly above the prototype or after its ;):\n  ` + newUndoc.join('\n  '));
  if (nowDocumented.length) console.log(`docs/api-undocumented.txt lists ${nowDocumented.length} function(s) that now have a comment (or no longer exist): delete their lines (the list only shrinks):\n  ` + nowDocumented.join('\n  '));
  if (!stale && !newUndoc.length && !nowDocumented.length) console.log(`gen-api --check: ok (${undocumented.length} undocumented, all grandfathered)`);
  process.exit(stale || newUndoc.length || nowDocumented.length ? 1 : 0);
}
writeFileSync(outPath, text);
writeFileSync(edPath, edText);
if (nodesText) writeFileSync(nodesPath, nodesText);
const nf = api.headers.reduce((a, h) => a + h.functions.length, 0);
console.log(`docs/api.json: ${headers.length} headers, ${nf} functions, ${undocumented.length} without a comment of their own`);
}
