// Accessibility over the debug channel: the l10n demo's accessibility tree
// (roles, names, values, states, bounds), focus, event log and l10n state,
// read natively (CW_DEBUG_CMDS) and from the cart in romdev
// (wasm({op:'command'})) at the same frame, must be identical and must say
// what the Arabic screen shows. Requests that must FAIL fail.
//   node tests/a11y-channel.mjs [frames]     ROMDEV_PORT (default 7331)
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const app = 'l10n_demo', frames = +(process.argv[2] || 170); // frame 170: the Arabic screen
const port = process.env.ROMDEV_PORT || 7331;
const requests = ['a11y tree', 'a11y focus', 'l10n', 'l10n tr menu.play', 'a11y events 0'];
const mustFail = ['l10n tr no.such.key', 'a11y set bogus 1'];

const exe = join(ROOT, 'build/native/samples', app);
const r = spawnSync(exe, [], { encoding: 'utf8', env: { ...process.env, SDL_AUDIODRIVER: 'dummy', CW_FRAMES: String(frames), CW_DEBUG_CMDS: [...requests, ...mustFail].join(';') } });
if (r.status !== 0) { console.error('native failed', r.stderr); process.exit(1); }
const native = r.stdout.trim().split('\n').map((l) => JSON.parse(l));

const S = `cw-a11y-${process.pid}`;
const call = async (tool, a) => {
  const res = await fetch(`http://127.0.0.1:${port}/tool/${tool}`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'x-romdev-session': S }, body: JSON.stringify(a) });
  return JSON.parse(await res.text());
};
const cart = join(ROOT, 'build/cart/carts', `${app}.wasc`);
if (!existsSync(cart)) { console.error('no cart', cart); process.exit(1); }
await call('loadMedia', { platform: 'wasmcart', path: cart, deterministicSeed: 1 });
await call('frame', { op: 'step', frames: frames - 1 });
await call('wasm', { op: 'command', text: 'pause 1' }); // each request costs a frame: hold the screen still
const out = [];
for (const q of [...requests, ...mustFail]) out.push((await call('wasm', { op: 'command', text: q })).reply);
await call('host', { op: 'shutdown' });

let fail = 0;
const bad = (m) => { fail++; console.log('FAIL ' + m); };
requests.forEach((q, i) => {
  const a = JSON.stringify(native[i]), b = JSON.stringify(out[i]);
  if (q.startsWith('l10n') && !q.includes('tr')) {
    // lookups counters advance with every frame the cart spent answering
    const strip = (s) => s.replace(/"lookups":\d+,/, '').replace(/"fallback_hits":\d+,/, '');
    if (strip(a) !== strip(b)) bad(`${q}\n native ${a.slice(0, 400)}\n cart   ${b.slice(0, 400)}`);
    return;
  }
  if (a !== b) bad(`${q}\n native ${a.slice(0, 400)}\n cart   ${b.slice(0, 400)}`);
});
// what the tree must say on the Arabic screen
const tree = native[0];
const find = (role, name) => tree?.nodes?.find((n) => n.role === role && (name === undefined || n.name === name));
if (!tree?.nodes || tree.count < 15) bad('a11y tree: too few nodes ' + JSON.stringify(tree).slice(0, 200));
const play = find('button', 'العب');
if (!play || !play.states.includes('rtl') || !play.states.includes('focusable')) bad('no RTL focusable button named العب: ' + JSON.stringify(play));
if (play && play.description !== 'ابدأ لعبة جديدة') bad('the Play button lacks its translated tooltip as description: ' + JSON.stringify(play));
if (!find('textfield', 'اسم اللاعب')) bad('the name field is not named by its label');
const combo = find('combobox', 'اللغة');
if (!combo || combo.value !== 'العربية' || !combo.states.includes('haspopup')) bad('language combobox: ' + JSON.stringify(combo));
const slider = find('slider');
if (!slider || JSON.stringify(slider.range) !== '[0,70,100]' || slider.value !== '٧٠') bad('slider value/range: ' + JSON.stringify(slider));
const chk = find('checkbox', 'إظهار الترجمة');
if (!chk || !chk.states.includes('checked')) bad('subtitles checkbox: ' + JSON.stringify(chk));
if (!find('caption')) bad('no caption region');
if (!find('image', 'شعار اللغة الحالية')) bad('the banner image lacks its translated name');
// bounds are mirrored: the Play button sits right of the Quit button in RTL
const quit = find('button', 'خروج');
if (!play || !quit || !(play.bounds[0] > quit.bounds[0])) bad('RTL order of the buttons: play ' + JSON.stringify(play?.bounds) + ' quit ' + JSON.stringify(quit?.bounds));
const l10n = native[2];
if (l10n?.locale !== 'ar' || JSON.stringify(l10n.chain) !== '["ar","en"]' || !l10n.rtl || l10n.missing !== 0) bad('l10n state: ' + JSON.stringify(l10n));
if (native[3]?.value !== 'العب') bad('l10n tr menu.play: ' + JSON.stringify(native[3]));
const ev = native[4]?.events || [];
if (!ev.some((e) => e.type === 'announce' && e.text === 'مرحبًا بك في عرض التوطين.')) bad('the caption was not announced: ' + JSON.stringify(ev).slice(0, 300));
// controls: requests that must fail
mustFail.forEach((q, k) => {
  const i = requests.length + k;
  if (!native[i]?.error || !out[i]?.error) bad(`CONTROL PASSED: "${q}" did not fail (native ${JSON.stringify(native[i])}, cart ${JSON.stringify(out[i])})`);
});
console.log(fail ? `a11y-channel: ${fail} failures` : `ok   a11y-channel (${requests.length} requests + ${mustFail.length} controls, native vs romdev cart, frame ${frames})`);
process.exit(fail ? 1 : 0);
