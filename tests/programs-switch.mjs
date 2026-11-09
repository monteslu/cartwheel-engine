// A graphics preset changed in play must not make draws vanish. Content a game builds on one preset (more lights,
// denser scatter) drawn with another preset's renderer state asks for program states that no run started on a
// single preset asks for, so a program table surveyed only from each preset's start showed "N draw state(s) have no
// program" after a mid-game switch. tools/programs.mjs now also switches the preset half way through its runs
// (High to each other preset and back).
//
// One scene (tests/unit/test_programs_switch.c: two shadow-casting omni lights on High and above, none below), two
// tables: unit_programs_switch's surveyed as the tool ships, unit_programs_switch_start's with --no-switch.
//   1. each preset from the start, both apps: 0 missing states (the start table is complete for those runs);
//   2. High, then Very Low at frame 20: unit_programs_switch draws everything (0 missing, 0 skipped);
//   3. the same through romdev, the dual cart on a GL host and on a WebGPU host (when build/cart-wgpu has both carts;
//      --no-cart skips it).
// CONTROL that must fail: unit_programs_switch_start with the same switch reports the missing state(s).
//
//   node tests/programs-switch.mjs [--no-cart]
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'programs-switch');
const CART = !process.argv.includes('--no-cart') &&
  ['unit_programs_switch', 'unit_programs_switch_start'].every((a) => existsSync(join(ROOT, 'build', 'cart-wgpu', 'carts', `${a}.wasc`)));
if (!CART) console.log('note: no dual carts of the two apps in build/cart-wgpu (or --no-cart): the romdev legs do not run');
for (const a of ['unit_programs_switch', 'unit_programs_switch_start'])
  if (!existsSync(join(ROOT, 'build', 'native', 'tests', 'unit', a))) { console.log(`FAIL build ${a} first (scripts/build.sh)`); process.exit(1); }
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
let failed = 0;
const fail = (m) => { failed++; console.log(`FAIL ${m}`); };
const ok = (m) => console.log(`ok   ${m}`);

function run(app, cmds, host = 'native', gpu = 'gl', tag = '') {
  const state = join(OUT, `${app}.${host}.${gpu}.${tag}.json`);
  const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', app, '--frames', '40', '--gpu', gpu, '--state', state, '--allow-skipped'];
  if (host === 'romdev' && gpu === 'gl') args.push('--dual-gl');
  const logFile = state.replace(/\.json$/, '.log');
  if (host === 'native') args.push('--log', logFile); // the run's log: the errors that name a missing draw
  if (cmds.length) args.push('--start-cmds', cmds.join(';'));
  const r = spawnSync(process.execPath, args, { encoding: 'utf8', timeout: 300000 });
  if (r.status !== 0) return { error: (r.stderr || r.stdout).trim().split('\n').slice(-3).join(' | ') };
  const s = JSON.parse(readFileSync(state, 'utf8'));
  // cw.three.missing_states is a byte field: hex in the state file, the states one per line
  const raw = String(s['cw.three.missing_states'] || '');
  const states = /^[0-9a-f]+$/i.test(raw) ? Buffer.from(raw, 'hex').toString('utf8').replace(/\0+$/, '') : raw;
  return { missing: s['cw.three.missing'] ?? -1, states, skipped: s['render.skipped'] ?? 0, omni: s['switch.omni'], log: existsSync(logFile) ? readFileSync(logFile, 'utf8') : '' };
}
const show = (r) => (r.error ? `run failed: ${r.error}` : `${r.missing} missing, ${r.skipped} skipped draws, ${r.omni} omni${r.states ? ' (' + String(r.states).trim().replace(/\n/g, ', ') + ')' : ''}`);

// 1. each preset from the start
for (const app of ['unit_programs_switch', 'unit_programs_switch_start'])
  for (const p of ['verylow', 'low', 'medium', 'high', 'ultra']) {
    const r = run(app, p === 'high' ? [] : [`quality ${p}`], 'native', 'gl', p);
    if (r.error || r.missing !== 0 || r.skipped) fail(`${app} on ${p} from the start: ${show(r)}`);
  }
if (!failed) ok('every preset from the start: 0 missing states on both tables');

// 2. the switch in play, natively on GL and WebGPU
const SWITCH = ['@20 quality verylow'];
for (const gpu of ['gl', 'webgpu']) {
  const r = run('unit_programs_switch', SWITCH, 'native', gpu, 'switch');
  if (r.error || r.missing !== 0 || r.skipped || r.omni !== 2) fail(`unit_programs_switch, High then Very Low at frame 20 (native ${gpu}): ${show(r)}`);
  else ok(`unit_programs_switch, High then Very Low at frame 20 (native ${gpu}): ${show(r)}`);
  const c = run('unit_programs_switch_start', SWITCH, 'native', gpu, 'switch');
  if (!c.error && c.missing > 0 && c.omni === 2) ok(`control (native ${gpu}): the table surveyed without the switch misses ${show(c)}`);
  else fail(`CONTROL PASSED (native ${gpu}): the table surveyed without the switch drew everything: ${show(c)}`);
  // the error names what vanished and the fix: the node, its material, the command for this app, the guide section
  const named = /NOT DRAWN: node "[^"]*Level\/(Ground|Box)", MeshStandardMaterial: its program state \S+/.exec(c.log || '');
  const fix = /node tools\/programs\.mjs unit_programs_switch_start/.test(c.log || '') && /rendering-3d\.md, "Program tables"/.test(c.log || '');
  if (named && fix) ok(`the missing draw's error names it (${named[0]}), the command and the guide section (native ${gpu})`);
  else fail(`the missing draw's error does not name the node, material, command and guide (native ${gpu}): ${(c.log || '').split('\n').filter((l) => /NOT DRAWN|programs\.mjs/.test(l)).join(' | ').slice(0, 600)}`);
  if (/NOT DRAWN/.test(r.log || '')) fail(`control: unit_programs_switch drew everything yet logged NOT DRAWN (native ${gpu})`);
}

// 3. the dual cart through romdev, on a GL host and a WebGPU host
if (CART)
  for (const gpu of ['gl', 'webgpu']) {
    const r = run('unit_programs_switch', SWITCH, 'romdev', gpu, 'switch');
    if (r.error || r.missing !== 0 || r.skipped || r.omni !== 2) fail(`cart through romdev (${gpu}), High then Very Low: ${show(r)}`);
    else ok(`cart through romdev (${gpu}), High then Very Low at frame 20: ${show(r)}`);
    const c = run('unit_programs_switch_start', SWITCH, 'romdev', gpu, 'switch');
    if (!c.error && c.missing > 0) ok(`control, cart through romdev (${gpu}): ${show(c)}`);
    else fail(`CONTROL PASSED, cart through romdev (${gpu}): ${show(c)}`);
  }

process.exit(failed ? 1 : 0);
