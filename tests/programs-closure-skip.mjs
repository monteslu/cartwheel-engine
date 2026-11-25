// A table's variant closure never fails its regeneration, and a game's own part composes with the engine's lighting
// parts in it (tools/programs.mjs, tools/lib/program-closure.mjs). Captures run on three.c's mock GPU: Node only.
//
//   1. a game part with engine lighting parts: a table of a skinned physical material with the game's part (sheen,
//      tests/fixtures/closure-skip) and the engine's skinshade under the cascaded sun, and a material in a scene
//      with no sun (hemisphere light only). The closure adds the skinned material without the sun
//      (physical+vcol+skin+eon+l0001+xsheen_skinshade, the state an example game's table failed on: skinshade's values
//      live in the lighting model's direct(), which a program without a direct light never runs); it regenerates
//      with no state skipped, its program has no skinshade values and the sunlit one has both;
//   2. a closure state whose capture fails (needsun, a game part that assumes the cascaded sun, paired by the closure
//      with the sunless scene) is skipped: the regeneration succeeds, says SKIPPED with the capture's own error (not
//      an empty one), lists it in its summary and in a "# closure skipped" line, and the table lacks it;
// CONTROL that must fail: the same state surveyed (in a table with "# closure: off") fails the regeneration, naming
// the capture's error.
//
//   node tests/programs-closure-skip.mjs
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'programs-closure-skip');
const FIX = join(ROOT, 'tests', 'fixtures', 'closure-skip');
let failed = 0;
const fail = (m) => { failed++; console.log(`FAIL ${m}`); };
const ok = (m) => console.log(`ok   ${m}`);
const statesOf = (f) => readFileSync(f, 'utf8').split('\n').map((l) => l.replace(/#.*/, '').trim()).filter(Boolean);

rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
/** a table in OUT: its manifest's lines; regenerated with tools/programs.mjs --table */
function regen(name, lines) {
  const dir = join(OUT, name);
  mkdirSync(dir, { recursive: true });
  const manifest = join(dir, `${name}.txt`);
  writeFileSync(manifest, lines.join('\n') + '\n');
  const r = spawnSync(process.execPath, [join(ROOT, 'tools', 'programs.mjs'), '--table', manifest, '--build', join(OUT, 'build'), '--fast', '--no-wgsl-check'],
    { encoding: 'utf8', timeout: 900000, maxBuffer: 256 << 20 });
  const text = `${r.stdout || ''}\n${r.stderr || ''}`;
  writeFileSync(join(OUT, `${name}.log`), text);
  return { status: r.status, text, manifest, dir, cache: join(OUT, 'build', 'programs-cache') };
}
/** the captured program json of a state (either backend), from the table's capture cache */
function captured(cacheRoot, state, be) {
  for (const d of readdirSync(cacheRoot)) {
    const f = join(cacheRoot, d, `${state}.${be}.json`);
    if (existsSync(f)) return JSON.parse(readFileSync(f, 'utf8'));
  }
  return null;
}
const has = (j, prop) => !!j && Object.prototype.hasOwnProperty.call(j.properties || {}, prop);

// 1. a game part with the engine's lighting parts
const SUNLIT = 'physical+vcol+skin+eon+l0001+xcsm_sheen_skinshade';
const SUNLESS = 'physical+vcol+skin+eon+l0001+xsheen_skinshade';
{
  const r = regen('closure_game', [`# extend: ${join(FIX, 'sheen.mjs')}`, SUNLIT, 'standard+l0001']);
  const states = existsSync(r.manifest) ? statesOf(r.manifest) : [];
  if (r.status !== 0) fail(`the table of a game part and the engine's lighting parts does not regenerate: ${r.text.trim().split('\n').slice(-4).join(' | ')}`);
  else if (/SKIPPED/.test(r.text)) fail(`the table of a game part and the engine's lighting parts skipped a closure state: ${r.text.split('\n').filter((l) => /SKIPPED|^\s+\S+:/.test(l)).join(' | ')}`);
  else if (!states.includes(SUNLESS)) fail(`the closure lacks ${SUNLESS}: ${states.join(', ')}`);
  else if (!['gl', 'wgpu'].every((be) => existsSync(join(r.dir, `closure_game_${be}.c`)))) fail('closure_game_gl.c / _wgpu.c not written');
  else ok(`a game part with the engine's lighting parts regenerates with its closure (${states.length} states, ${SUNLESS} among them, none skipped)`);
  for (const be of ['gl', 'wgpu']) {
    const lit = captured(r.cache, SUNLIT, be), dark = captured(r.cache, SUNLESS, be);
    if (!lit || !dark) { fail(`${be}: no capture of ${!lit ? SUNLIT : SUNLESS}`); continue; }
    if (has(lit, 'ext.skinScatter') && has(lit, 'ext.skinTranslucency') && !has(dark, 'ext.skinScatter') && !has(dark, 'ext.skinTranslucency'))
      ok(`${be}: the sunlit program has skinshade's values, the program with no direct light has none`);
    else fail(`${be}: skinshade's values: sunlit ${has(lit, 'ext.skinScatter')}/${has(lit, 'ext.skinTranslucency')}, no direct light ${has(dark, 'ext.skinScatter')}/${has(dark, 'ext.skinTranslucency')}`);
  }
}

// 2. a closure state whose capture fails is skipped and reported
const BROKEN = 'standard+l0001+xneedsun';
const WHY = /needsun: \S+ has no cascaded sun/;
{
  const r = regen('closure_skip', [`# extend: ${join(FIX, 'needsun.mjs')}`, 'standard+l0001+xcsm_needsun', 'standard+l0001']);
  const text = existsSync(r.manifest) ? readFileSync(r.manifest, 'utf8') : '';
  const states = existsSync(r.manifest) ? statesOf(r.manifest) : [];
  if (r.status !== 0) fail(`a failing closure state failed the regeneration: ${r.text.trim().split('\n').slice(-4).join(' | ')}`);
  else ok('a failing closure state does not fail the regeneration');
  const said = r.text.split('\n').find((l) => l.includes(`closure state ${BROKEN} SKIPPED`)) || '';
  if (WHY.test(said)) ok(`it is reported with the capture's own error: ${said.trim()}`);
  else fail(`no SKIPPED line for ${BROKEN} with the capture's error (${said.trim() || 'none'})`);
  if (new RegExp(`closure state\\(s\\) SKIPPED[^\\n]*\\n\\s+${BROKEN.replace(/\+/g, '\\+')}: `).test(r.text)) ok('and in the summary');
  else fail('no summary of the skipped closure states');
  if (states.includes(BROKEN)) fail(`the table lists ${BROKEN}`);
  else if (!new RegExp(`^# closure skipped \\(tools/programs\\.mjs\\): ${BROKEN.replace(/\+/g, '\\+')}: .*needsun`, 'm').test(text)) fail('no "# closure skipped" line naming it in the manifest');
  else ok('the table lacks it, and its manifest says why');
  if (!['gl', 'wgpu'].every((be) => existsSync(join(r.dir, `closure_skip_${be}.c`)))) fail('closure_skip_gl.c / _wgpu.c not written');
}

// CONTROL: the same state surveyed fails the regeneration
{
  const r = regen('closure_skip_control', ['# closure: off', `# extend: ${join(FIX, 'needsun.mjs')}`, BROKEN]);
  if (r.status === 0) fail(`CONTROL: the surveyed state ${BROKEN} regenerated, its failure was swallowed`);
  else if (!WHY.test(r.text)) fail(`CONTROL: the surveyed state's failure does not name the capture's error: ${r.text.trim().split('\n').slice(-3).join(' | ')}`);
  else ok(`CONTROL: the surveyed state ${BROKEN} fails the regeneration, naming the capture's error`);
}

if (failed) { console.log(`programs-closure-skip: ${failed} FAILED (logs in ${OUT})`); process.exit(1); }
rmSync(OUT, { recursive: true, force: true });
console.log('programs-closure-skip: ok');
