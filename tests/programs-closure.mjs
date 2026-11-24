// A graphics preset switched in play draws with programs the table has, with no survey of the switch: the variant
// closure tools/programs.mjs adds when it regenerates a table (tools/lib/program-closure.mjs) pairs every surveyed
// material with every environment surveyed for its class, at the most lights its class reached.
//
//   1. the closure itself: of unit_programs_switch_start's surveyed states (each preset from the start, no switch) it
//      holds the state the switch asks for (standard+instbig+shadow+l1200+s100+xplain: High's omni lights drawn with
//      Very Low's plain model and sun-only shadows), adds nothing a second time (idempotent), and the checked-in
//      tests/unit/programs/programs_closure.txt is those states and their closure;
//   2. unit_programs_closure (the same scene, that table) switched from High to Very Low at frame 20, natively on GL
//      and WebGPU and as the dual cart through romdev on a GL and a WebGPU host (when build/cart-wgpu has the carts;
//      --no-cart skips them), with three.c's cover off (CW_TEST_NO_COVER=1: the closure alone): 0 states no table has.
// CONTROL that must fail: unit_programs_switch_start (the same surveyed states, "# closure: off") with the same switch
// misses the state.
//
//   node tests/programs-closure.mjs [--no-cart]
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { closure, featureOrder } from '../tools/lib/program-closure.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'programs-closure');
const APPS = ['unit_programs_closure', 'unit_programs_switch_start'];
const CART = !process.argv.includes('--no-cart') && APPS.every((a) => existsSync(join(ROOT, 'build', 'cart-wgpu', 'carts', `${a}.wasc`)));
if (!CART) console.log('note: no dual carts of the apps in build/cart-wgpu (or --no-cart): the romdev legs do not run');
let failed = 0;
const fail = (m) => { failed++; console.log(`FAIL ${m}`); };
const ok = (m) => console.log(`ok   ${m}`);
const SWITCHED = 'standard+instbig+shadow+l1200+s100+xplain';

// 1. the closure
const statesOf = (f) => readFileSync(f, 'utf8').split('\n').map((l) => l.replace(/#.*/, '').trim()).filter(Boolean);
const three = process.env.CW_THREE_DIR || join(ROOT, 'deps', 'three.c');
const order = featureOrder(readFileSync(join(three, 'src', 'gen', 'programs.h'), 'utf8'));
const surveyed = statesOf(join(ROOT, 'tests', 'unit', 'programs', 'programs_switch_start.txt'));
const c = closure(surveyed, order);
if (c.add.includes(SWITCHED)) ok(`the closure of the surveyed states holds ${SWITCHED} (+${c.add.length}: ${c.add.join(', ')})`);
else fail(`the closure of the surveyed states lacks ${SWITCHED}: ${c.add.join(', ')}`);
const again = closure([...surveyed, ...c.add], order);
if (!again.add.length) ok('the closure of a table with its closure adds nothing');
else fail(`the closure is not idempotent: ${again.add.join(', ')}`);
const table = statesOf(join(ROOT, 'tests', 'unit', 'programs', 'programs_closure.txt'));
const want = [...surveyed, ...c.add];
if (want.every((s) => table.includes(s)) && table.every((s) => want.includes(s))) ok('programs_closure.txt is the surveyed states and their closure');
else fail(`programs_closure.txt is not the surveyed states and their closure (regenerate: node tools/programs.mjs --table tests/unit/programs/programs_closure.txt): ${table.join(', ')}`);

// 2. the switch in play, the closure alone
for (const a of APPS)
  if (!existsSync(join(ROOT, 'build', 'native', 'tests', 'unit', a))) { console.log(`FAIL build ${a} first (scripts/build.sh)`); process.exit(1); }
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
/** the pipeline request's reply after the last frame (romdev), or null */
const pipelineReply = (file) => {
  if (!existsSync(file)) return null;
  const j = JSON.parse(readFileSync(file, 'utf8'));
  const r = j.at(-1)?.reply?.reply ?? j.at(-1)?.reply;
  return r && typeof r === 'object' ? r : null;
};
function run(app, host, gpu) {
  const state = join(OUT, `${app}.${host}.${gpu}.json`);
  const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', app, '--frames', '40', '--gpu', gpu, '--state', state,
    '--allow-skipped', '--start-cmds', '@20 quality verylow', '--env', JSON.stringify({ CW_TEST_NO_COVER: '1' })];
  if (host === 'romdev' && gpu === 'gl') args.push('--dual-gl');
  // (romdev's state carries no bytes fields: the states come from the pipeline request's reply after the last frame)
  const replyFile = join(OUT, `${app}.${host}.${gpu}.${Math.random().toString(36).slice(2)}.reply.json`);
  if (host === 'romdev') args.push('--cmd-end', 'pipeline', '--cmd-out', replyFile);
  const r = spawnSync(process.execPath, args, { encoding: 'utf8', timeout: 300000 });
  if (r.status !== 0) return { error: (r.stderr || r.stdout).trim().split('\n').slice(-3).join(' | ') };
  const s = JSON.parse(readFileSync(state, 'utf8'));
  const raw = String(s['cw.three.missing_states'] || '');
  const rep = pipelineReply(replyFile);
  const states = rep ? String(rep.missing_states ?? '') : (/^[0-9a-f]+$/i.test(raw) ? Buffer.from(raw, 'hex').toString('utf8') : raw).replace(/\0+$/, '');
  return { missing: s['cw.three.missing'] ?? -1, skipped: s['render.skipped'] ?? 0, omni: s['switch.omni'], states };
}
const show = (r) => (r.error ? `run failed: ${r.error}` : `${r.missing} missing, ${r.skipped} skipped draws${r.states ? ' (' + r.states.trim().replace(/\n/g, ', ') + ')' : ''}`);
for (const [host, gpus] of [['native', ['gl', 'webgpu']], ...(CART ? [['romdev', ['gl', 'webgpu']]] : [])])
  for (const gpu of gpus) {
    const r = run('unit_programs_closure', host, gpu);
    if (r.error || r.missing !== 0 || r.skipped || r.omni !== 2) fail(`unit_programs_closure, High then Very Low, no cover (${host} ${gpu}): ${show(r)}`);
    else ok(`unit_programs_closure, High then Very Low at frame 20, no cover (${host} ${gpu}): ${show(r)}`);
    const k = run('unit_programs_switch_start', host, gpu);
    if (!k.error && k.missing > 0 && k.states.includes(SWITCHED)) ok(`control, closure off (${host} ${gpu}): ${show(k)}`);
    else fail(`CONTROL PASSED, the table without its closure drew everything (${host} ${gpu}): ${show(k)}`);
  }
process.exit(failed ? 1 : 0);
