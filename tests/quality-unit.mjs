// The graphics settings' choices and save words (cartwheel/quality.h), outside a deterministic run: unit_quality with
// CW_PLAYER_TIMING (a deterministic run locks a saved choice and Auto out), then each case's control, which must fail:
//   quality-resolution    75% stays on every preset and Auto, the preset's name shown, across a save round trip and a
//                         restart (a second process on the same save file); control CW_TEST_QUALITY_OLD_RES
//   quality-save-compat   every older word restores the choice it held; control CW_TEST_QUALITY_V2_ONLY
//   quality-levers        a lever's five preset values apply; a lever written with a four-entry array does not build
//                         (control: the same call with the struct builds)
//   quality-auto-jump     a 60 ms host on a 19 ms budget is on Very Low within 3 seconds of play; control
//                         CW_TEST_QUALITY_NO_JUMP (one rung every 150 frames) misses it
//   node tests/quality-unit.mjs [--build build/native]
import { spawnSync } from 'node:child_process';
import { mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const bi = process.argv.indexOf('--build');
const B = resolve(ROOT, bi > 0 ? process.argv[bi + 1] : 'build/native');
const OUT = join(ROOT, 'out', 'quality-unit');
mkdirSync(OUT, { recursive: true });
const b = spawnSync('ninja', ['-C', B, 'unit_quality'], { encoding: 'utf8' });
if (b.status !== 0) { console.log(b.stdout + b.stderr); process.exit(1); }
const SAVE = join(OUT, 'save.bin');
const run = (env) => {
  const e = { ...process.env, CW_FRAMES: '2', CW_PLAYER_TIMING: '1', CW_TEST_FRAME_MS: '60', SDL_VIDEODRIVER: 'offscreen', SDL_AUDIODRIVER: 'dummy',
    CW_GPU: 'gl', LIBGL_ALWAYS_SOFTWARE: '1', CW_SAVE_FILE: SAVE, ...env };
  for (const k of ['DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY']) delete e[k];
  const r = spawnSync(join(B, 'tests/unit/unit_quality'), [], { encoding: 'utf8', env: e });
  return (r.stdout || '') + (r.stderr || '');
};
const caseOk = (log, c) => new RegExp(`quality\\.${c}: ok`).test(log);
const failed = (log) => { const m = /quality: \d+ passed, (\d+) failed/.exec(log); return m ? Number(m[1]) : -1; };
let fail = 0;
const bad = (msg, log) => { fail++; console.log('FAIL ' + msg); if (log) console.log(log.split('\n').filter((l) => /CHECK|label|old word|auto_jump|quality\./.test(l)).slice(0, 20).join('\n')); };

rmSync(SAVE, { force: true });
const log = run({});
if (failed(log) !== 0) bad('unit_quality failed', log);
for (const c of ['resolution', 'save_compat', 'levers', 'auto_jump', 'cycle']) if (!caseOk(log, c)) bad(`${c} did not pass`, log);
if (!/auto_jump: Very Low after \d+ frames/.test(log)) bad('auto_jump ran nothing');
// the restart: a process that saves 75% and Medium, then a new one that loads them
rmSync(SAVE, { force: true });
const p1 = run({ QUALITY_PHASE: '1' }), p2 = run({ QUALITY_PHASE: '2' });
if (!caseOk(p1, 'restart') || !caseOk(p2, 'restart')) bad('the restart lost Resolution or the preset', p1 + p2);
// controls: each must fail its case
const controls = [
  ['CW_TEST_QUALITY_OLD_RES', 'resolution'],
  ['CW_TEST_QUALITY_V2_ONLY', 'save_compat'],
  ['CW_TEST_QUALITY_NO_JUMP', 'auto_jump'],
];
for (const [k, c] of controls) {
  rmSync(SAVE, { force: true });
  const l = run({ [k]: '1' });
  if (caseOk(l, c)) bad(`CONTROL PASSED: ${k} left ${c} passing`);
  else console.log(`ok   control ${k}: ${c} fails`);
}
{ // the old preset: a restart under it loses 75%
  rmSync(SAVE, { force: true });
  run({ QUALITY_PHASE: '1', CW_TEST_QUALITY_OLD_RES: '1' });
  if (caseOk(run({ QUALITY_PHASE: '2', CW_TEST_QUALITY_OLD_RES: '1' }), 'restart')) bad('CONTROL PASSED: the old preset kept 75% across a restart');
  else console.log('ok   control CW_TEST_QUALITY_OLD_RES: the restart loses 75%');
}
// a lever written for four presets must not build; the struct must
const cc = process.env.CC || 'cc';
const src = (arg) => `#include "cartwheel/quality.h"\nstatic const char *const V[] = { "Off", "On" };\nint f(void) {\n${arg}\n}\n`;
const old = join(OUT, 'lever_old.c'), now = join(OUT, 'lever_struct.c');
writeFileSync(old, src('  static const uint8_t P[4] = { 0, 0, 1, 1 };\n  return cw_quality_lever_add("Edges", V, 2, P);'));
writeFileSync(now, src('  return cw_quality_lever_add("Edges", V, 2, (cw_quality_lever_presets){ .very_low = 0, .low = 0, .medium = 1, .high = 1, .ultra = 1 });'));
const compile = (f) => spawnSync(cc, ['-std=c11', '-fsyntax-only', '-I', join(ROOT, 'include/cartwheel'), '-I', join(ROOT, 'include'), f], { encoding: 'utf8' });
const co = compile(old), cn = compile(now);
if (co.status === 0) bad('a lever with a four-entry array built');
else console.log('ok   a lever with a four-entry array does not build');
if (cn.status !== 0) bad('CONTROL: the struct lever does not build: ' + cn.stderr.slice(0, 400));
console.log(fail ? `quality-unit: ${fail} failures` : 'ok   quality-unit (resolution, save compat, levers, Auto jump, each control fails)');
process.exit(fail ? 1 : 0);
