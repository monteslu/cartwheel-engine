// A surface of the game's own keeps the engine's lighting (samples/custom_surface, cartwheel/surface.h).
//
// The sample's three surfaces (dissolve, triplanar rock, wind sway) sit among clustered lamps, under the sun's
// cascades, inside a reflection probe and a light probe grid, with a sticker clipped onto one of them.
//
// 1. Lighting kept. With every effect at zero (CS_MODE=neutral) the programs that carry the surfaces must draw
//    the picture plain materials draw (CS_MODE=stock): at most 0.05 % of the pixels differ by more than 16, on
//      clustered      clustered lamps + the sun's cascades + probes (the sample as it is)
//      forward        three.c's own lights, no cascades: the surface alone on its material, probes on top
//      verylow        the Very Low preset: one more lighting part on every material
//      late-decal     the sticker placed after the surface is on its material
//    natively on GL and WebGPU. Control: CW_TEST_SURFACE_ALONE=1 treats a surface part as any game part was
//    treated before surfaces existed (the probes refuse it, no decal is clipped from it, a material takes four
//    parts): forward, verylow and late-decal must then FAIL.
// 2. The effects draw (CS_MODE unset), on every host: the orb has holes and so has its shadow; the slab's stone
//    differs from its plain colour; the reeds are somewhere else 30 frames later, and so are their shadows.
// 3. Screen-space reflections see the surface: with CS_SSR=1 the G-buffer is drawn with the surface's own
//    vertices and cut-outs. Control (CW_TEST_SURFACE_ALONE=1): the G-buffer drops them and the picture changes.
// 4. The parameter calls answer as documented (cs.api_ok), a texture changed after it was made is uploaded
//    again (cs.tex_updates and the slab's picture), nothing is skipped.
// 5. Draws (custom_surface_field: two hundred reeds, no probes): the reeds, one material with a surface, are
//    instanced as plain ones are, on forward lighting and on clustered lighting with LightingSettings.batching,
//    and they still sway.
//
//   node tests/custom-surface.mjs [--hosts native,romdev,chromium]
// The table (samples/custom_surface/programs/custom_surface.txt) holds every state these runs draw. After a
// change that asks for new ones:
//   CW_PROGRAMS_LOG_DIR=$PWD/out/custom-surface/states node tests/custom-surface.mjs --hosts native
//   node tools/programs.mjs custom_surface --no-run --logs out/custom-surface/states
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { readImage } from '../tools/lib/png.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'custom-surface');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const hosts = argv.includes('--hosts') ? argv[argv.indexOf('--hosts') + 1].split(',')
  : process.env.CW_TEST_GPU === 'software' ? ['native'] : ['native', 'romdev', 'chromium'];
const SURVEY = !!process.env.CW_PROGRAMS_LOG_DIR; // collecting states: skipped draws are the point
if (SURVEY) mkdirSync(process.env.CW_PROGRAMS_LOG_DIR, { recursive: true });
let failed = false;
const report = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failed = true; };

function run(host, gpu, tag, env, frames = 40, app = 'custom_surface') {
  const shot = join(OUT, `${tag}.png`), state = join(OUT, `${tag}.json`);
  const args = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--gpu', gpu, '--app', app, '--frames', String(frames),
    '--shot', shot, '--state', state, ...(Object.keys(env).length ? ['--env', JSON.stringify(env)] : []), ...(SURVEY ? ['--allow-skipped'] : []),
    ...(host === 'romdev' && gpu === 'gl' ? ['--dual-gl'] : [])];
  const r = spawnSync(process.execPath, args, { encoding: 'utf8' }); // (--env: the switches reach a cart host as they reach a native run)
  if (r.status !== 0) return { err: (r.stderr || r.stdout).trim().split('\n').slice(-3).join(' | ') };
  return { img: readImage(shot), st: JSON.parse(readFileSync(state, 'utf8')) };
}
/** share of pixels (of the box, in fractions of the picture) whose worst channel differs by more than 16 */
function differ(a, b, box = [0, 0, 1, 1]) {
  if (a.w !== b.w || a.h !== b.h) return 1;
  const x0 = Math.floor(box[0] * a.w), y0 = Math.floor(box[1] * a.h), x1 = Math.ceil(box[2] * a.w), y1 = Math.ceil(box[3] * a.h);
  let n = 0;
  for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) {
    const o = 4 * (y * a.w + x);
    if (Math.max(Math.abs(a.rgba[o] - b.rgba[o]), Math.abs(a.rgba[o + 1] - b.rgba[o + 1]), Math.abs(a.rgba[o + 2] - b.rgba[o + 2])) > 16) n++;
  }
  return n / ((x1 - x0) * (y1 - y0));
}
const pct = (v) => `${(v * 100).toFixed(3)}%`;
// where things are in the picture (fractions of it: x0, y0, x1, y1)
const ORB = [0.21, 0.36, 0.38, 0.64], ORB_SHADOW = [0.17, 0.52, 0.27, 0.66], SLAB = [0.42, 0.33, 0.58, 0.58],
  REEDS = [0.69, 0.40, 0.95, 0.68], REED_SHADOWS = [0.60, 0.52, 0.72, 0.66], FLOOR_NEAR_ORB = [0.08, 0.66, 0.38, 0.77];
const SAME = 0.0005;

const CONFIGS = [
  ['clustered', {}],
  ['forward', { CS_LIGHTING: 'forward', CS_CASCADES: '0' }],
  ['verylow', { CS_QUALITY: 'verylow' }],
  ['late-decal', { CS_LATE_DECAL: '1' }],
];
const ALONE = { CW_TEST_SURFACE_ALONE: '1' };

for (const host of hosts) {
  const gpus = host === 'native' || host === 'romdev' ? ['gl', 'webgpu'] : ['gl'];
  for (const gpu of gpus) {
    const T = `${host}.${gpu}`;
    // ── 1. lighting kept ──
    for (const [name, env] of host === 'native' ? CONFIGS : CONFIGS.slice(0, 1)) {
      const s = run(host, gpu, `${T}.${name}.stock`, { ...env, CS_MODE: 'stock' }), n = run(host, gpu, `${T}.${name}.neutral`, { ...env, CS_MODE: 'neutral' });
      if (s.err || n.err) { report(false, `${T} ${name}: ${s.err || n.err}`); continue; }
      const d = differ(s.img, n.img);
      report(d <= SAME, `${T} ${name}: surfaces at zero draw what plain materials draw (${pct(d)} of the pixels differ; surfaces ${n.st['cs.surfaces']}, ` +
        `with probes ${n.st['cs.gi_materials']} of ${s.st['cs.gi_materials']}, mesh decals ${n.st['cs.mesh_decals']} of ${s.st['cs.mesh_decals']})`);
      report(n.st['cs.surfaces'] === 3 && s.st['cs.surfaces'] === 0, `${T} ${name}: three surfaces in use, none in the reference (${n.st['cs.surfaces']}, ${s.st['cs.surfaces']})`);
      if (host === 'native' && name !== 'clustered' && !SURVEY) {
        const c = run(host, gpu, `${T}.${name}.neutral.control`, { ...env, ...ALONE, CS_MODE: 'neutral' });
        if (c.err) { report(false, `${T} ${name} control: ${c.err}`); continue; }
        const dc = differ(s.img, c.img);
        report(dc > 4 * SAME, `${T} ${name} control (a surface treated as a part was before) is caught: ${pct(dc)} of the pixels differ ` +
          `(with probes ${c.st['cs.gi_materials']}, mesh decals ${c.st['cs.mesh_decals']})`);
      } else if (SURVEY && host === 'native' && name !== 'clustered') {
        run(host, gpu, `${T}.${name}.neutral.control`, { ...env, ...ALONE, CS_MODE: 'neutral' });
        run(host, gpu, `${T}.${name}.fx`, env);
      }
    }
    // ── 2. the effects draw ──
    const stock = run(host, gpu, `${T}.stock`, { CS_MODE: 'stock' }), fx = run(host, gpu, `${T}.fx`, {}), later = run(host, gpu, `${T}.fx70`, {}, 70);
    if (stock.err || fx.err || later.err) { report(false, `${T} effects: ${stock.err || fx.err || later.err}`); continue; }
    const st = fx.st;
    report(st['cs.api_ok'] === 1, `${T}: the parameter calls answer as documented (cs.api_ok ${st['cs.api_ok']})`);
    report(st['cs.surfaces'] === 3 && st['cs.skipped'] === 0, `${T}: three surfaces, nothing skipped (${st['cs.surfaces']}, skipped ${st['cs.skipped']})`);
    report(st['cs.mesh_decals'] === 1 && st['cs.decal_tris'] > 0, `${T}: the sticker is clipped from the slab's mesh (${st['cs.mesh_decals']} decal, ${st['cs.decal_tris']} triangles)`);
    const orb = differ(stock.img, fx.img, ORB), sh = differ(stock.img, fx.img, ORB_SHADOW), slab = differ(stock.img, fx.img, SLAB);
    report(orb > 0.15, `${T}: the orb is dissolved (${pct(orb)} of its box differs from the plain orb)`);
    report(sh > 0.03, `${T}: the orb's shadow has the holes (${pct(sh)} of the shadow's box differs)`);
    report(slab > 0.3, `${T}: the slab draws its stone (${pct(slab)} of its box differs from the plain slab)`);
    const sway = differ(fx.img, later.img, REEDS), swayShadow = differ(fx.img, later.img, REED_SHADOWS);
    report(sway > 0.02, `${T}: the reeds moved between frame 40 and frame 70 (${pct(sway)} of their box)`);
    report(swayShadow > 0.005, `${T}: the reeds' shadows moved with them (${pct(swayShadow)} of the shadows' box)`);
    // ── 4. a texture changed in place (tick 20) reached the GPU: the slab at frame 15 has no dark course ──
    const before = run(host, gpu, `${T}.fx15`, {}, 15);
    if (before.err) report(false, `${T} texture update: ${before.err}`);
    else {
      const d = differ(before.img, fx.img, SLAB);
      report(st['cs.tex_updates'] === 1 && before.st['cs.tex_updates'] === 0 && d > 0.02,
        `${T}: the stone texture changed after it was made shows on the slab (${pct(d)} of its box differs from frame 15; updates ${st['cs.tex_updates']})`);
    }
    if (host !== 'native') continue;
    // ── 3. screen-space reflections see the surface ──
    const ssr = run(host, gpu, `${T}.ssr.fx`, { CS_SSR: '1' });
    if (ssr.err) report(false, `${T} ssr: ${ssr.err}`);
    else {
      report(ssr.st['cs.skipped'] === 0, `${T} ssr: nothing skipped with the G-buffer drawn (${ssr.st['cs.skipped']})`);
      if (!SURVEY) {
        const c = run(host, gpu, `${T}.ssr.fx.control`, { CS_SSR: '1', ...ALONE });
        if (c.err) report(false, `${T} ssr control: ${c.err}`);
        else {
          const d = differ(ssr.img, c.img, FLOOR_NEAR_ORB);
          report(d > 0.002, `${T} ssr: the floor mirrors the dissolved orb; the control (the G-buffer drops the surface) mirrors a whole one (${pct(d)} of the floor's box differs)`);
        }
      } else run(host, gpu, `${T}.ssr.fx.control`, { CS_SSR: '1', ...ALONE });
    }
    // ── 5. draws (custom_surface_field: two hundred reeds, no probes) ──
    for (const [name, env] of [['forward lighting', {}], ['clustered lighting with batching', { CS_LIGHTING: 'clustered', CS_BATCH: '1' }]]) {
      const tag = name.split(' ')[0];
      const a = run(host, gpu, `${T}.field.${tag}.stock`, { ...env, CS_MODE: 'stock' }, 40, 'custom_surface_field'),
        b = run(host, gpu, `${T}.field.${tag}.fx`, env, 40, 'custom_surface_field'), c = run(host, gpu, `${T}.field.${tag}.fx70`, env, 70, 'custom_surface_field');
      if (a.err || b.err || c.err) { report(false, `${T} draws, ${name}: ${a.err || b.err || c.err}`); continue; }
      report(b.st['cs.draws'] <= a.st['cs.draws'] + 2 && b.st['cs.draws'] < 30,
        `${T} draws, ${name}, no probes: two hundred reeds with a surface are instanced as plain ones are (${b.st['cs.draws']} draws a frame, ${a.st['cs.draws']} with plain materials)`);
      const moved = differ(b.img, c.img);
      report(moved > 0.01, `${T} draws, ${name}: the instanced reeds sway (${pct(moved)} of the picture differs between frame 40 and frame 70)`);
    }
  }
}
process.exit(failed ? 1 : 0);
