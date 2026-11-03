// tools/check-prompts.mjs, with its controls: a fixture with one of each kind of maker's button name must report
// exactly the lines marked FLAG (C strings, a scene's quoted strings, a catalog's values), a fixture of the forms that
// must pass (the article "A", markup, Start, keyboard keys, prompt-ok) must pass, and the engine's own sources pass.
// Then cwpack --cwtr-check: a translation that moves its markup passes, one that drops a glyph or leaves a brace open fails.
//
//   node tests/check-prompts.mjs
import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const TOOL = join(ROOT, 'tools', 'check-prompts.mjs');
const FIX = join(ROOT, 'tests', 'check-prompts');
const run = (...a) => spawnSync('node', [TOOL, ...a], { cwd: ROOT, encoding: 'utf8' });
const problems = [];

// the control that must fail: the lines it reports are the lines marked FLAG, no more, no fewer
const listed = run('--list', join(FIX, 'bad')).stdout.split('\n').filter(Boolean).map((l) => l.split(': ')[0]);
const want = [];
for (const f of ['hud.c', 'menu.cwscene', 'en.cwtr']) {
  const lines = readFileSync(join(FIX, 'bad', f), 'utf8').split('\n');
  lines.forEach((l, i) => {
    const scene = f.endsWith('.cwscene') && /^text = /.test(l), tr = f.endsWith('.cwtr') && /^hint\.join/.test(l);
    if (/FLAG/.test(l) && !/^\s*\/?\*/.test(l) || scene || tr) want.push(`tests/check-prompts/bad/${f}:${i + 1}`);
  });
}
const got = [...new Set(listed)];
for (const w of want) if (!got.includes(w)) problems.push(`missed ${w}`);
for (const g of got) if (!want.includes(g)) problems.push(`reported ${g}, which is not marked FLAG`);
const bad = run(join(FIX, 'bad'));
if (bad.status !== 1) problems.push(`the bad fixture exited ${bad.status}, not 1`);
// the forms that must pass
const good = run(join(FIX, 'good'));
if (good.status !== 0) problems.push(`the good fixture failed:\n${good.stderr}`);
// the engine: its shell, UI, samples, games, editor and headers
const eng = run('src', 'games', 'samples', 'editor', 'include');
if (eng.status !== 0) problems.push(`the engine's own text names buttons:\n${eng.stderr}`);

// the catalog check: a translation keeps the source's {...} (placeholders and prompt markup), well formed
const cwpack = join(ROOT, 'build', 'native', 'tools', 'cwpack', 'cwpack');
const cat = (f) => spawnSync(cwpack, ['--cwtr-check', join(FIX, 'catalog', f), '--po-source', join(FIX, 'catalog', 'en.cwtr')], { encoding: 'utf8' });
const okCat = cat('fr.cwtr'), badCat = cat('fr-broken.cwtr');
if (okCat.error) problems.push(`cwpack did not run (${okCat.error.message}): build build/native first`);
else {
  if (okCat.status !== 0) problems.push(`cwpack --cwtr-check failed a translation that moved its markup:\n${okCat.stderr}`);
  const n = (badCat.stderr.match(/cwpack: .*fr-broken/g) || []).length;
  if (badCat.status !== 1 || n !== 3) problems.push(`cwpack --cwtr-check on a translation with its glyph dropped and a brace open: exit ${badCat.status}, ${n} findings (want 1 and 3: the dropped glyph, the open brace, and the pair it hides)`);
}

if (problems.length) {
  console.error(`check-prompts: ${problems.length} problem(s)\n  ${problems.join('\n  ')}`);
  process.exit(1);
}
console.log(`check-prompts: ok (control: ${want.length} findings reported exactly; the passing forms and the engine clean)`);
