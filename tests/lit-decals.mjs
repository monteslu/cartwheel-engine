// A mesh decal takes the light of the surface it sits on (samples/fx_lit_decals).
// Three identical stickers on one wall: in the dark (the sun's shadow, out of the lamp's reach),
// under a lamp, in the sun. For each, the sticker's pixels are compared with the wall beside it:
// sticker / wall (in linear light) is the sticker's albedo over the wall's, the same everywhere
// if the sticker is lit as the wall is. Checked:
//   1. the three places differ: the wall is at least 2x brighter under the lamp and in the sun
//      than in the dark;
//   2. the three ratios agree within 20 % (the wall is grey, the sticker an orange of about the same
//      luminance, so the ratio is about 1 wherever the light is; a sticker that is missing is caught
//      by its hue);
//   3. the sticker in the dark is under half as bright as the one under the lamp;
//   4. the badges clipped onto two skinned characters, one in the dark and one under the lamp: the
//      first is dark, the second at least 2.5x brighter.
// Control: CW_DECAL_UNLIT=1 draws the decals unlit as before; the ratios then differ by far more
// than 20 % and checks 2 and 3 must FAIL.
// Runs natively on GL and WebGPU, in CLUSTERED lighting (the cluster and csm shading parts) and
// FORWARD lighting (FX_LIT_FORWARD=1), and with --hosts also on the cart hosts (default lighting
// only; their ratios must match the native GL ones within 10 %).
//
//   node tests/lit-decals.mjs [--hosts native,chromium,romdev]
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { readImage } from '../tools/lib/png.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'lit-decals');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const hosts = argv.includes('--hosts') ? argv[argv.indexOf('--hosts') + 1].split(',') : ['native', 'romdev', 'chromium'];
let failed = false;
const report = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failed = true; };

// the camera: at (0, 1.5, 6) looking down -z, 40 degrees of vertical field; the wall is at z = 0
const PLACES = [['dark', -1.9, 1.5], ['lamp', 0, 1.5], ['sun', 2.8, 2.6]];
const lin = (v) => { const c = v / 255; return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); };
function lumAt(img, x, y, half, chroma = false) {
  const k = img.h / 2 / (6 * Math.tan(20 * Math.PI / 180));
  const cx = Math.round(img.w / 2 + x * k), cy = Math.round(img.h / 2 - (y - 1.5) * k), r = Math.round(half * k);
  let s = 0, n = 0, R = 0, B = 0;
  for (let j = cy - r; j <= cy + r; j++) for (let i = cx - r; i <= cx + r; i++) {
    const o = 4 * (j * img.w + i);
    s += 0.2126 * lin(img.rgba[o]) + 0.7152 * lin(img.rgba[o + 1]) + 0.0722 * lin(img.rgba[o + 2]);
    R += lin(img.rgba[o]); B += lin(img.rgba[o + 2]);
    n++;
  }
  return chroma ? R / B : s / n;
}
/** per place: the sticker's luminance, the wall's beside it (left and right), their ratio */
function measure(img) {
  return PLACES.map(([name, x, y]) => {
    const d = lumAt(img, x, y, 0.12);
    const w = (lumAt(img, x - 0.33, y, 0.03) + lumAt(img, x + 0.33, y, 0.03)) / 2;
    // red over blue of the sticker against the wall's (a grey wall, an orange sticker): a sticker
    // that is not drawn leaves 1
    const hue = lumAt(img, x, y, 0.12, true) / lumAt(img, x + 0.33, y, 0.03, true);
    return { name, decal: d, wall: w, ratio: d / w, hue };
  });
}
// the badge on a character's chest (skinned): the orange pixels near its place, their luminance
const HEROES = [['hero in the dark', -2.75], ['hero under the lamp', 0.9]];
function heroLum(img, x) {
  const k = img.h / 2 / (4.4 * Math.tan(20 * Math.PI / 180));
  const cx = Math.round(img.w / 2 + x * k), cy = Math.round(img.h / 2 - (1.05 - 1.5) * k), r = Math.round(0.16 * k);
  let s = 0, n = 0;
  for (let j = cy - r; j <= cy + r; j++) for (let i = cx - r; i <= cx + r; i++) {
    if (i < 0 || i >= img.w) continue;
    const o = 4 * (j * img.w + i), R = lin(img.rgba[o]), B = lin(img.rgba[o + 2]);
    if (R / B < 1.3) continue;
    s += 0.2126 * R + 0.7152 * lin(img.rgba[o + 1]) + 0.0722 * B;
    n++;
  }
  return { lum: n ? s / n : NaN, n };
}
function run(host, gpu, tag, env) {
  const shot = join(OUT, `${tag}.png`), state = join(OUT, `${tag}.json`);
  const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--gpu', gpu, '--app', 'fx_lit_decals', '--frames', '30', '--shot', shot, '--state', state];
  const r = spawnSync(process.execPath, args, { encoding: 'utf8', env: { ...process.env, ...env } });
  if (r.status !== 0) return { err: (r.stderr || r.stdout).trim().split('\n').slice(-3).join(' | ') };
  const img = readImage(shot);
  return { m: measure(img), heroes: HEROES.map(([, x]) => heroLum(img, x)) };
}
const fmt = (m) => m.map((p) => `${p.name} ${p.decal.toFixed(3)}/${p.wall.toFixed(3)}=${p.ratio.toFixed(2)}`).join(', ');
const spread = (m) => { const r = m.map((p) => p.ratio); return Math.max(...r) / Math.min(...r) - 1; };
const varies = (m) => m[1].wall > 2 * m[0].wall && m[2].wall > 2 * m[0].wall;
const dark = (m) => m[0].decal < 0.5 * m[1].decal;
const drawn = (m) => m.every((p) => p.hue > 1.4);

const heroOk = (h) => h[0].n > 200 && h[1].n > 200 && h[0].lum < 0.6 * h[1].lum;
const heroFmt = (h) => h.map((p, i) => `${HEROES[i][0]} ${p.lum.toFixed(3)} (${p.n} px)`).join(', ');
const ref = {};
for (const host of hosts) {
  for (const gpu of host === 'native' ? ['gl', 'webgpu'] : ['gl']) {
    for (const mode of host === 'native' ? ['clustered', 'forward', 'gi'] : ['clustered']) {
      const tag = `${host}.${gpu}.${mode}`, env = mode === 'forward' ? { FX_LIT_FORWARD: '1' } : mode === 'gi' ? { FX_LIT_GI: '1' } : {};
      const a = run(host, gpu, tag, env);
      if (a.err) { report(false, `${tag}: ${a.err}`); continue; }
      report(varies(a.m), `${tag}: the wall's light differs between the places (${a.m.map((p) => p.wall.toFixed(3)).join(' / ')})`);
      report(spread(a.m) < 0.2, `${tag}: sticker / wall agrees across the places within 20% (${fmt(a.m)}; spread ${(spread(a.m) * 100).toFixed(0)}%)`);
      report(drawn(a.m), `${tag}: the stickers are drawn (hue against the wall ${a.m.map((p) => p.hue.toFixed(2)).join(' / ')})`);
      report(heroOk(a.heroes), `${tag}: the skinned badges are lit like their surroundings (${heroFmt(a.heroes)})`);
      report(dark(a.m), `${tag}: the sticker in the dark is dark (${a.m[0].decal.toFixed(3)})`);
      if (host === 'native' && gpu === 'gl') ref[mode] = a.m;
      if (host !== 'native' && ref.clustered) {
        const worst = Math.max(...a.m.map((p, i) => Math.abs(p.ratio / ref.clustered[i].ratio - 1)));
        report(worst < 0.1, `${tag}: ratios match native GL within 10% (worst ${(worst * 100).toFixed(1)}%)`);
      }
      if (host === 'native') {
        const c = run(host, gpu, `${tag}.control`, { ...env, CW_DECAL_UNLIT: '1' });
        if (c.err) { report(false, `${tag} control: ${c.err}`); continue; }
        // the control must fail what the real run passes
        report(!heroOk(c.heroes), `${tag} control: the skinned badge in the dark is caught (${heroFmt(c.heroes)})`);
        report(!(spread(c.m) < 0.2) && !dark(c.m), `${tag} control (decals unlit) is caught: ${fmt(c.m)}; spread ${(spread(c.m) * 100).toFixed(0)}%`);
      }
    }
  }
}
process.exit(failed ? 1 : 0);
