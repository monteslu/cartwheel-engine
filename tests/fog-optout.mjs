// A transparent material with fog = false takes no fog (src/render/fog.c): samples/fog_transparents with FT_OPTOUT=1,
// four unlit transparent panels that write no depth (MeshBasicMaterial, opacity 1) in the sample's height fog with
// max_opacity 0.9 under a sky the fog leaves alone (sky_distance 0): NearOff (fog = false) and NearOn (fog = true)
// 38 m away, FarOff and FarOn 900 m away over the sky (the far mountains of a sky ring). Each configuration is drawn
// with the fog and without it (FT_FOG=0), and over the middle of each panel:
//   Off  keeps its colour: within 2/255 of the run without fog (mean of the worst channel)
//   On   is fogged at its own distance: its colour x (1 - f) + the fog's colour x f, f the height fog's opacity
//        there (capped at max_opacity), within 3/255; in volumetric fog (whose medium three.c's materials light by
//        the grid's ambient) at least 20/255 from its colour
// Analytic fog on High and Very Low (where the fog otherwise runs inside the tonemap pass) and volumetric on High,
// natively and as the dual cart on romdev, GL and WebGPU.
// CONTROL that must fail: CW_TEST_FOG_IGNORE_OPTOUT=1 (v0.70.1 to v0.77.0: every transparent material fogged, the
// fog = false panels too).
//   node tests/fog-optout.mjs [--hosts native,romdev] [--gpu gl,webgpu] [--presets high,verylow]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'fog-optout');
mkdirSync(OUT, { recursive: true });
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const HOSTS = arg('--hosts', process.env.CW_TEST_GPU === 'software' ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
const PRESETS = arg('--presets', 'high,verylow').split(',');
const FRAMES = 30;
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

function run(host, gpu, name, env, preset) {
  const png = join(OUT, `${name}.${host}.${gpu}.png`), st = join(OUT, `${name}.${host}.${gpu}.json`);
  const r = spawnSync('node', [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', 'fog_transparents', '--frames', String(FRAMES),
    '--gpu', gpu, '--shot', png, '--state', st, '--env', JSON.stringify({ FT_OPTOUT: '1', ...env }), '--start-cmds', `quality ${preset}`],
  { encoding: 'utf8', env: process.env });
  if (r.status !== 0) throw new Error(`${host} ${gpu} ${name}: ${((r.stderr || '') + (r.stdout || '')).trim().split('\n').slice(-3).join(' | ')}`);
  const s = JSON.parse(readFileSync(st, 'utf8'));
  return { img: readImage(png), s: s.fields || s };
}
const v = (r, k) => { const x = r.s[k]; return typeof x === 'object' && x !== null && 'value' in x ? x.value : x; };

const LIN = new Float64Array(256);
for (let i = 0; i < 256; i++) { const c = i / 255; LIN[i] = c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; }
const SRGB = (x) => { x = Math.min(1, Math.max(0, x)); return 255 * (x <= 0.0031308 ? 12.92 * x : 1.055 * x ** (1 / 2.4) - 0.055); };

// the world (samples/fog_transparents: world.cwscene and FT_OPTOUT's panels)
const CAM = [0, 1.6, 0], FOV = 50;
const FOG = { density: 0.02, falloff: 0.05, base: 0, color: [0.42, 0.36, 0.3], maxOpacity: 0.9 };
const PANELS = {
  NearOff: { fog: false, x: -6, y: 2, z: -38, half: 1.5 },
  NearOn: { fog: true, x: 6, y: 2, z: -38, half: 1.5 },
  FarOff: { fog: false, x: -300, y: 80, z: -900, half: 40 },
  FarOn: { fog: true, x: 300, y: 80, z: -900, half: 40 },
};
/** the fog's opacity at a point (the fog pass's analytic height fog) */
function fogAt(p) {
  const r = [p[0] - CAM[0], p[1] - CAM[1], p[2] - CAM[2]], l = Math.hypot(...r), vy = r[1] / l;
  const a = FOG.density * Math.exp(-FOG.falloff * (CAM[1] - FOG.base)), k = FOG.falloff * vy * l;
  const tau = a * l * (Math.abs(k) > 1e-4 ? (1 - Math.exp(-k)) / k : 1 - 0.5 * k);
  return Math.min(1 - Math.exp(-tau), FOG.maxOpacity);
}
/** the pixels of a panel's middle (a square of half size `half` on the plane z), with their world points */
function pixels(img, P) {
  const t = Math.tan((FOV / 2) * Math.PI / 180), asp = img.w / img.h, d = -P.z;
  const col = (x) => (x / d / (t * asp) * 0.5 + 0.5) * img.w, row = (y) => (0.5 - (y - CAM[1]) / d / t * 0.5) * img.h;
  const out = [];
  for (let py = Math.ceil(row(P.y + P.half)); py <= Math.floor(row(P.y - P.half)); py++)
    for (let px = Math.ceil(col(P.x - P.half)); px <= Math.floor(col(P.x + P.half)); px++) {
      const ndx = ((px + 0.5) / img.w - 0.5) * 2, ndy = (0.5 - (py + 0.5) / img.h) * 2;
      out.push({ px, py, p: [ndx * t * asp * d, CAM[1] + ndy * t * d, P.z] });
    }
  return out;
}
const byte = (img, px, py, k) => img.rgba[4 * (py * img.w + px) + k];

/** the problems of one configuration (fog, no fog): empty when it holds; also the measures, for the log */
function judge(fogged, clear, vfog) {
  const bad = [], meas = {};
  for (const [name, P] of Object.entries(PANELS)) {
    const px = pixels(fogged, P);
    if (px.length < 4) { bad.push(`${name}: no pixels`); continue; }
    let errOff = 0, errOn = 0, fromOwn = 0;
    for (const q of px) {
      let wOff = 0, wOn = 0, own = 0;
      const f = fogAt(q.p);
      for (let k = 0; k < 3; k++) {
        const got = byte(fogged, q.px, q.py, k), ref = byte(clear, q.px, q.py, k);
        wOff = Math.max(wOff, Math.abs(got - ref));
        wOn = Math.max(wOn, Math.abs(got - SRGB(LIN[ref] * (1 - f) + FOG.color[k] * f)));
        own = Math.max(own, Math.abs(got - ref));
      }
      errOff += wOff, errOn += wOn, fromOwn += own;
    }
    errOff /= px.length, errOn /= px.length, fromOwn /= px.length;
    meas[name] = P.fog ? (vfog ? fromOwn : errOn) : errOff;
    if (!P.fog && !(errOff <= 2)) bad.push(`${name} (fog = false): ${errOff.toFixed(2)}/255 from its colour without fog (at most 2)`);
    if (P.fog && !vfog && !(errOn <= 3)) bad.push(`${name} (fog = true): ${errOn.toFixed(2)}/255 from its colour fogged at its distance (at most 3)`);
    if (P.fog && vfog && !(fromOwn >= 20)) bad.push(`${name} (fog = true, volumetric): ${fromOwn.toFixed(2)}/255 from its colour (at least 20)`);
  }
  return { bad, meas };
}

let fail = 0, ctlRuns = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const fmt = (m) => Object.entries(m).map(([k, x]) => `${k} ${x.toFixed(2)}`).join(', ');

const configs = PRESETS.map((p) => ({ tag: `analytic-${p}`, preset: p, env: {} }));
if (PRESETS.includes('high')) configs.push({ tag: 'vfog-high', preset: 'high', env: { FT_VFOG: '1' }, vfog: true });
for (const host of HOSTS)
  for (const gpu of GPUS)
    for (const c of configs) {
      const tag = `${host} ${gpu} ${c.tag}`;
      let shown, clear;
      try {
        shown = run(host, gpu, `${c.tag}-fog`, c.env, c.preset);
        clear = run(host, gpu, `${c.tag}-nofog`, { ...c.env, FT_FOG: '0' }, c.preset).img;
      } catch (e) {
        check(false, `${tag}: ${e.message}`);
        continue;
      }
      check(v(shown, 'ft.trans') === 1 && v(shown, 'ft.fog') === 1, `${tag}: the fog drew, then the transparent queue after it (ft.fog ${v(shown, 'ft.fog')}, ft.trans ${v(shown, 'ft.trans')})`);
      const j = judge(shown.img, clear, c.vfog);
      check(!j.bad.length, `${tag}: fog = false panels keep their colour, fog = true ones are fogged (${fmt(j.meas)})${j.bad.length ? '\n       ' + j.bad.join('\n       ') : ''}`);
      let ctl;
      try {
        ctl = judge(run(host, gpu, `${c.tag}-ctl`, { ...c.env, CW_TEST_FOG_IGNORE_OPTOUT: '1' }, c.preset).img, clear, c.vfog);
      } catch (e) {
        check(false, `${tag} CONTROL: ${e.message}`);
        continue;
      }
      ctlRuns++;
      check(ctl.bad.length > 0, `${tag}: CONTROL CW_TEST_FOG_IGNORE_OPTOUT=1 breaks it (${ctl.bad.length} problems: ${ctl.bad[0] || 'none'})`);
    }
console.log(fail ? `fog-optout: ${fail} FAILED` : `fog-optout: all held (${ctlRuns} controls failed as they must)`);
process.exit(fail ? 1 : 0);
