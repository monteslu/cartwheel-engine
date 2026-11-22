// The game's Settings rows survive a restart through the shell's save (cartwheel/shell.h, settings_items): shell_settings
// natively with a save file and the save on (CW_PLAYER_TIMING: a deterministic test run neither loads nor writes saves).
// Run 1 opens Settings from the title and steps Visual style twice to the right (Fixed: Paper, 2); run 2, a new process
// on the same file with no input, must start with 2 and Hit sounds still at its default 1.
// A saved index out of range: the file is patched to hold 9 (and a label no choice has), and run 3 must start at the
// default, 0, with the shell's warning naming 9 (a slot the patch damaged would also read 0, but silently).
// Controls, each must fail: run 1 with CW_TEST_SHELL_SETTINGS_NO_SAVE=1 (the rows never reach the save: run 2 reads
// the default 0); run 3 without the patch reads 2, not the default.
//   node tests/shell-settings-save.mjs [--build build/native]
import { spawnSync } from 'node:child_process';
import { mkdirSync, rmSync, readFileSync, writeFileSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const bi = process.argv.indexOf('--build');
const B = resolve(ROOT, bi > 0 ? process.argv[bi + 1] : 'build/native');
const OUT = join(ROOT, 'out', 'shell-settings-save');
mkdirSync(OUT, { recursive: true });
if (!existsSync(join(B, 'samples', 'shell_settings'))) {
  const b = spawnSync('ninja', ['-C', B, 'shell_settings'], { encoding: 'utf8' });
  if (b.status !== 0) { console.log(b.stdout + b.stderr); process.exit(1); }
}
const SAVE = join(OUT, 'save.bin');
const run = (frames, input, env = {}) => {
  const state = join(OUT, 'state.json'), log = join(OUT, 'run.log');
  rmSync(state, { force: true });
  rmSync(log, { force: true });
  const r = spawnSync('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'native', '--app', 'shell_settings', '--exe', join(B, 'samples', 'shell_settings'), '--frames', String(frames),
    ...(input ? ['--input', join(ROOT, 'tests', input)] : []), '--state', state, '--log', log],
  { encoding: 'utf8', env: { ...process.env, CW_PLAYER_TIMING: '1', CW_SAVE_FILE: SAVE, ...env } });
  if (!existsSync(state)) { console.log(r.stdout + r.stderr); return {}; }
  return { ...JSON.parse(readFileSync(state, 'utf8')), log: existsSync(log) ? readFileSync(log, 'utf8') : '' };
};
let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
// the slot "shell:style" in the shell's store (the last 1024 bytes of the game area): its int32 index, then the label
const patchStyle = (index) => {
  const b = readFileSync(SAVE);
  const name = Buffer.from('shell:style');
  const at = b.indexOf(name);
  if (at < 12) return false;
  const head = at - 12, size = b.readUInt32LE(head + 4), dataAt = at + ((name.length + 3) & ~3);
  b.writeInt32LE(index, dataAt);
  b.fill(0x3f, dataAt + 4, dataAt + size); // the label: question marks, which no choice is
  // the slot's checksum: FNV-1a of the name then the data (cartwheel/save.h)
  let h = 2166136261;
  for (const x of [...name, ...b.subarray(dataAt, dataAt + size)]) h = Math.imul(h ^ x, 16777619) >>> 0;
  b.writeUInt32LE(h, head + 8);
  // the shell block's checksum covers the game area of a struct game: FNV-1a of everything from the settings on
  const from = 16; // magic, version, game_version, game_size, checksum
  let c = 2166136261;
  for (let i = from; i < b.length; i++) c = Math.imul(c ^ b[i], 16777619) >>> 0;
  b.writeUInt32LE(c, 12);
  writeFileSync(SAVE, b);
  return true;
};

rmSync(SAVE, { force: true });
const r1 = run(100, 'shell-settings-save.json');
check(r1['game.style'] === 2 && r1['game.calls'] === 2, `run 1 stepped Visual style to 2 (style ${r1['game.style']}, calls ${r1['game.calls']})`);
const r2 = run(5);
check(r2['game.style'] === 2 && r2['game.hits'] === 1, `run 2 loaded Visual style 2 and Hit sounds 1 (style ${r2['game.style']}, hits ${r2['game.hits']})`);
check(patchStyle(9), 'the save holds the slot shell:style');
const r3 = run(5);
check(r3['game.style'] === 0 && /holds 9, out of its 4 choices/.test(r3.log), `a saved 9 of 4 choices falls back to the default 0, said in the log (style ${r3['game.style']}; a slot dropped as damaged would read 0 without the line)`);
// controls
rmSync(SAVE, { force: true });
run(100, 'shell-settings-save.json', { CW_TEST_SHELL_SETTINGS_NO_SAVE: '1' });
const c2 = run(5);
check(c2['game.style'] === 0, `control: with no save of the rows run 2 reads the default (style ${c2['game.style']}; 2 would mean the check cannot fail)`);
rmSync(SAVE, { force: true });
run(100, 'shell-settings-save.json');
const c3 = run(5);
check(c3['game.style'] === 2, `control: without the patch run 3 reads the saved 2 (style ${c3['game.style']}), so the fallback is what made it 0`);
console.log(fail ? `shell-settings-save: ${fail} failed` : 'shell-settings-save: ok');
process.exit(fail ? 1 : 0);
