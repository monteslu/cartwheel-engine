// A draw whose program state is in none of the app's tables is drawn, never skipped: three.c's cover
// (t3_renderer_set_cover) takes the nearest program that covers it, with the engine's rules for shading parts
// (src/render/shading.c), and the state is still reported (cw.three.missing_states, the error naming the node, the
// gates and the survey), so the table gets it. Only the banner, which says draws vanished, stays away.
//
// unit_programs_cover draws the programs-switch scene (tests/unit/test_programs_switch.c) with a table left incomplete
// on purpose (tests/unit/programs/programs_cover.txt):
//   1. High: standard+instbig+shadow+l1200 is drawn by its fog twin with the fog made inert, the same picture to the
//      byte as unit_programs_switch, whose table has the state (an exact cover);
//   2. High, then Very Low at frame 20: the plain states and the plain caster depth+xplain are drawn by programs
//      without the plain part (approximate for the colour program, exact for the caster): 0 skipped draws, 0 states
//      undrawn (no banner), every covered state still listed as missing, the log names the node and the program;
//   3. the dual cart through romdev on a GL host and a WebGPU host, the same (when build/cart-wgpu has the carts;
//      --no-cart skips it).
// CONTROL that must fail: CW_TEST_NO_COVER=1 (no cover, the skip of before) leaves those draws out: skipped draws,
// states undrawn.
//
//   node tests/programs-cover.mjs [--no-cart]
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'programs-cover');
const APPS = ['unit_programs_cover', 'unit_programs_switch'];
const CART = !process.argv.includes('--no-cart') && APPS.every((a) => existsSync(join(ROOT, 'build', 'cart-wgpu', 'carts', `${a}.wasc`)));
if (!CART) console.log('note: no dual carts of the apps in build/cart-wgpu (or --no-cart): the romdev legs do not run');
for (const a of APPS)
  if (!existsSync(join(ROOT, 'build', 'native', 'tests', 'unit', a))) { console.log(`FAIL build ${a} first (scripts/build.sh)`); process.exit(1); }
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
let failed = 0;
const fail = (m) => { failed++; console.log(`FAIL ${m}`); };
const ok = (m) => console.log(`ok   ${m}`);

/** the pipeline request's reply after the last frame (romdev), or null */
const pipelineReply = (file) => {
  if (!existsSync(file)) return null;
  const j = JSON.parse(readFileSync(file, 'utf8'));
  const r = j.at(-1)?.reply?.reply ?? j.at(-1)?.reply;
  return r && typeof r === 'object' ? r : null;
};
function run(app, cmds, host, gpu, tag, env = {}) {
  const base = join(OUT, `${app}.${host}.${gpu}.${tag}`);
  const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', app, '--frames', '40', '--gpu', gpu,
    '--state', base + '.json', '--shot', base + '.png', '--allow-skipped'];
  if (host === 'romdev' && gpu === 'gl') args.push('--dual-gl');
  // (romdev's state carries no bytes fields: the states come from the pipeline request's reply after the last frame)
  const replyFile = join(OUT, `${app}.${host}.${gpu}.${Math.random().toString(36).slice(2)}.reply.json`);
  if (host === 'romdev') args.push('--cmd-end', 'pipeline', '--cmd-out', replyFile);
  if (host === 'native') args.push('--log', base + '.log');
  if (cmds.length) args.push('--start-cmds', cmds.join(';'));
  if (Object.keys(env).length) args.push('--env', JSON.stringify(env)); // (a cart gets it over the debug channel)
  const r = spawnSync(process.execPath, args, { encoding: 'utf8', timeout: 300000 });
  if (r.status !== 0) return { error: (r.stderr || r.stdout).trim().split('\n').slice(-3).join(' | ') };
  const s = JSON.parse(readFileSync(base + '.json', 'utf8'));
  const rep = pipelineReply(replyFile);
  const field = { 'cw.three.missing_states': 'missing_states', 'cw.three.covered_states': 'covered_states' };
  const text = (k) => {
    if (rep) return String(rep[field[k]] ?? '');
    const v = String(s[k] || '');
    return (/^[0-9a-f]+$/i.test(v) ? Buffer.from(v, 'hex').toString('utf8') : v).replace(/\0+$/, '');
  };
  return { missing: s['cw.three.missing'] ?? -1, covered: s['cw.three.covered'] ?? -1, undrawn: s['cw.three.undrawn'] ?? -1,
    skipped: s['render.skipped'] ?? 0, omni: s['switch.omni'], states: text('cw.three.missing_states'), pairs: text('cw.three.covered_states'),
    png: base + '.png', log: existsSync(base + '.log') ? readFileSync(base + '.log', 'utf8') : '' };
}
const show = (r) => (r.error ? `run failed: ${r.error}` : `${r.missing} missing, ${r.covered} covered, ${r.undrawn} undrawn, ${r.skipped} skipped draws` +
  (r.pairs ? ` (${r.pairs.trim().replace(/\n/g, '; ')})` : ''));
const same = (a, b) => existsSync(a) && existsSync(b) && Buffer.compare(readFileSync(a), readFileSync(b)) === 0;
const SWITCH = ['@20 quality verylow'];
const EXACT = 'standard+instbig+shadow+l1200 standard+fog+instbig+shadow+l1200';
const PLAIN = 'standard+instbig+shadow+l1200+s100+xplain standard+instbig+shadow+l1200+s100';
const CASTER = 'depth+xplain depth';

for (const [host, gpus] of [['native', ['gl', 'webgpu']], ...(CART ? [['romdev', ['gl', 'webgpu']]] : [])])
  for (const gpu of gpus) {
    const leg = `${host} ${gpu}`;
    // 1. High: the exact cover draws the picture of the complete table
    const hi = run('unit_programs_cover', [], host, gpu, 'high');
    const ref = run('unit_programs_switch', [], host, gpu, 'high');
    if (hi.error || hi.skipped || hi.undrawn !== 0 || !hi.pairs.includes(EXACT) || !hi.states.includes('standard+instbig+shadow+l1200\n') || hi.omni !== 2)
      fail(`High drawn through the cover (${leg}): ${show(hi)}`);
    else ok(`High drawn through the cover (${leg}): ${show(hi)}`);
    if (ref.error || ref.missing !== 0) fail(`the reference with the state in its table (${leg}): ${show(ref)}`);
    else if (!same(hi.png, ref.png)) fail(`the exact cover's frame differs from the frame of a table with the state (${leg}): ${hi.png} vs ${ref.png}`);
    else ok(`the exact cover's frame equals the complete table's to the byte (${leg})`);
    // 2. the switch: approximate and caster covers, nothing undrawn
    const sw = run('unit_programs_cover', SWITCH, host, gpu, 'switch');
    if (sw.error || sw.skipped || sw.undrawn !== 0 || ![EXACT, PLAIN, CASTER].every((p) => sw.pairs.includes(p)) || sw.missing !== 3)
      fail(`High then Very Low at frame 20 (${leg}): ${show(sw)}`);
    else ok(`High then Very Low at frame 20 (${leg}): ${show(sw)}`);
    if (host === 'native') {
      const named = /DRAWN WITH A NEAR PROGRAM: node "[^"]*Level\/(Ground|Box)", MeshStandardMaterial: its program state standard\+instbig\+shadow\+l1200\+s100\+xplain .*drawn with standard\+instbig\+shadow\+l1200\+s100/.exec(sw.log);
      const fix = /node tools\/programs\.mjs unit_programs_cover/.test(sw.log) && !/NOT DRAWN/.test(sw.log);
      if (named && fix) ok(`the covered draw's error names the node, the state, the program and the fix (${leg})`);
      else fail(`the covered draw's error does not name the node, program and fix, or a draw was not drawn (${leg}): ${sw.log.split('\n').filter((l) => /DRAWN|programs\.mjs/.test(l)).join(' | ').slice(0, 600)}`);
    }
    // CONTROL: no cover, the draws are skipped as before
    const c = run('unit_programs_cover', SWITCH, host, gpu, 'control', { CW_TEST_NO_COVER: '1' });
    if (!c.error && c.skipped > 0 && c.undrawn > 0) ok(`control, no cover (${leg}): ${show(c)}`);
    else fail(`CONTROL PASSED, no cover drew everything (${leg}): ${show(c)}`);
  }

process.exit(failed ? 1 : 0);
