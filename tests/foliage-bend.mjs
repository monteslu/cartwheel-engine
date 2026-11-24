// Grass bending around movers (cartwheel/foliage.h movers, TerrainScatter3D bend), measured from the picture of
// samples/foliage_field (bend.* debug fields), per host and backend, each check with a control that must fail:
//
//   bend      1 m stems in a band 4.4 to 5.6 m ahead before a plain background, a mover (radius 0.8, recover 1 s)
//             walking along the band at 4 m/s from x = -9 to 0, then removed. Each stem's presence: the share of the rows
//             of its tip window holding stem, against before the mover set off. With bend 1.1: every stem it passed
//             leaves its window (presence under 0.2), every stem 1.8 m and more past where it stopped stays (moves under
//             0.1), the passed ones are still down 0.25 s after it (1 - presence over 0.5) and stand again within 0.5 to
//             1.35 s of it passing (recover 1 s); with recover 2 they take 0.6 to 1.4 s longer. CONTROL: bend 0, the
//             stems it passes stand (presence over 0.8).
//   combined  the same walk with wind 0.08, TAA and fade_mode DITHER: the passed stems bend (presence under 0.2) and the
//             stems it never came near draw as without bend (each one's lowest presence within 0.05 of the run without
//             it). CONTROL: that run without bend, where the passed stems stand (presence over 0.6).
//   zero      the GRASS field with wind, root colours and DITHER, a mover standing 6 m ahead: bend 0 draws the picture
//             with no mover byte for byte. CONTROL: bend 1.1 changes it.
//   cards     the alpha-tested card field with smooth_alpha, DITHER and TAA, a mover standing in it: bend draws (no
//             skipped draws) and changes the picture. CONTROL: the same without bend (the picture both are told apart by).
//
//   node tests/foliage-bend.mjs [--hosts native,romdev] [--gpu gl,webgpu] [--only bend,combined,zero,cards]
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { runField } from './foliage-runs.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const SOFTWARE = process.env.CW_TEST_GPU === 'software';
const HOSTS = arg('--hosts', SOFTWARE ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
const ONLY = arg('--only', 'bend,combined,zero,cards').split(',');
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}
void ROOT;
let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const diff = (a, b) => { let n = 0; for (let i = 0; i < a.rgba.length; i += 4) if (a.rgba[i] !== b.rgba[i] || a.rgba[i + 1] !== b.rgba[i + 1] || a.rgba[i + 2] !== b.rgba[i + 2]) n++; return n; };
const f3 = (x) => (typeof x === 'number' ? x.toFixed(3) : String(x));
// the far stems' lowest presence (bend.tip: x, z, lowest presence, base rows per stem)
const farLows = (f) => { const t = f['bend.tip'], o = []; for (let k = 0; k + 3 < t.length; k += 4) if (t[k + 3] >= 4 && t[k] > 1.8) o.push(t[k + 2]); return o; };

for (const host of HOSTS)
  for (const gpu of GPUS) {
    const tag = `${host} ${gpu}`;
    const step = (what, fn) => { try { fn(); } catch (e) { check(false, `${tag}: ${what}: ${e.message}`); } };
    const run = (frames, env, name, o) => runField(host, gpu, frames, env, name, o);
    const B = { FOL_SCENE: 'bend', FOL_FROM: '40' };
    if (ONLY.includes('bend'))
      step('bend', () => {
        const on = run(300, { ...B, FOL_BEND: '1.1' }, 'bend-on', { shot: false }).fields;
        const off = run(300, B, 'bend-off', { shot: false }).fields;
        const slow = run(360, { ...B, FOL_BEND: '1.1', FOL_RECOVER: '2' }, 'bend-slow', { shot: false }).fields;
        check(on['bend.near_n'] >= 3 && on['bend.far_n'] >= 2, `${tag}: ${on['bend.near_n']} stems passed and ${on['bend.far_n']} far ones tracked (3 and 2 at least)`);
        check(on['bend.near_min'] < 0.2, `${tag}: every stem the mover passed bends out of its window (presence ${f3(on['bend.near_min'])} at most, under 0.2)`);
        check(off['bend.near_min'] > 0.8, `${tag}: CONTROL: bend 0: the stems it passes stand (presence ${f3(off['bend.near_min'])}, over 0.8)`);
        check(on['bend.far_dev'] < 0.1, `${tag}: the stems it never came near stay (moved ${f3(on['bend.far_dev'])}, under 0.1)`);
        check(on['bend.held'] > 0.5, `${tag}: still down 0.25 s after it passed (1 - presence ${f3(on['bend.held'])}, over 0.5)`);
        check(on['bend.ret'] >= 0.5 && on['bend.ret'] <= 1.35, `${tag}: they stand again ${f3(on['bend.ret'])} s after it passed (recover 1 s: 0.5 to 1.35)`);
        const d = slow['bend.ret'] - on['bend.ret'];
        check(d >= 0.6 && d <= 1.4, `${tag}: recover 2 s: ${f3(slow['bend.ret'])} s, ${f3(d)} s longer (0.6 to 1.4)`);
      });
    if (ONLY.includes('combined'))
      step('combined', () => {
        const C = { ...B, FOL_WIND: '0.08', FOL_TAA: '1', FOL_FADE: 'dither' };
        const on = run(300, { ...C, FOL_BEND: '1.1' }, 'bend-comb-on', { shot: false }).fields;
        const off = run(300, C, 'bend-comb-off', { shot: false }).fields;
        check(on['bend.near_min'] < 0.2, `${tag}: with wind, TAA and DITHER the passed stems bend (presence ${f3(on['bend.near_min'])}, under 0.2)`);
        check(off['bend.near_min'] > 0.6, `${tag}: CONTROL: without bend they stand in the wind (presence ${f3(off['bend.near_min'])}, over 0.6)`);
        const a = farLows(on), b = farLows(off);
        const worst = a.length === b.length && a.length ? Math.max(...a.map((x, k) => Math.abs(x - b[k]))) : Infinity;
        check(a.length >= 2 && worst < 0.05, `${tag}: the ${a.length} far stems draw as without bend (lowest presence within ${f3(worst)}, under 0.05)`);
      });
    if (ONLY.includes('zero'))
      step('zero', () => {
        const F = { FOL_WIND: '0.08', FOL_ROOT: '1', FOL_FADE: 'dither' };
        const none = readImage(run(60, F, 'bend-none').png);
        const zero = readImage(run(60, { ...F, FOL_MOVER: '1' }, 'bend-zero').png);
        const on = readImage(run(60, { ...F, FOL_MOVER: '1', FOL_BEND: '1.1' }, 'bend-mover').png);
        const dz = diff(none, zero), don = diff(none, on);
        check(dz === 0, `${tag}: bend 0 with a mover draws the picture without one byte for byte (${dz} pixels differ)`);
        check(don > 500, `${tag}: CONTROL: bend 1.1 changes it (${don} pixels)`);
      });
    if (ONLY.includes('cards'))
      step('cards', () => {
        const K = { FOL_SCENE: 'cards', FOL_ALPHA: 'smooth', FOL_FADE: 'dither', FOL_TAA: '1', FOL_MOVER: '1' };
        const r = run(40, { ...K, FOL_BEND: '1.1' }, 'bend-cards'), c = run(40, K, 'bend-cards-off');
        const d = diff(readImage(r.png), readImage(c.png));
        check(r.fields['render.skipped'] === 0, `${tag}: smooth_alpha cards with bend: ${r.fields['render.skipped']} draws skipped`);
        check(d > 500, `${tag}: bend changes the card field (${d} pixels against the CONTROL without it)`);
      });
  }
console.log(fail ? `foliage-bend: ${fail} failures` : 'ok   foliage-bend');
process.exit(fail ? 1 : 0);
