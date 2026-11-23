// The post looks (cartwheel/look.h, src/render/looks.c) held to what each promises, on the looks sample, natively on
// GL and WebGPU. Every check runs a control that must fail.
//
//   none       no look set: the frame is the one drawn without looks (the sample never touching the API, and a look
//              set and cleared again), byte for byte, and the "looks" pass did not run.
//              CONTROL: a blend weight of 0.02 toward a look differs.
//   ascii      every pixel of the ASCII look's glyph probe (CW_TEST_LOOK_ASCII_IDS: each cell's glyph index as its
//              colour) is a glyph of the look's set or ramp, and the shape matcher used at least three of them.
//              CONTROL: CW_TEST_LOOK_ASCII_ANY (any glyph) draws glyphs outside the set.
//   dither     ordered (Bayer, blue noise), diffused and a two-colour diffused look: exactly two colours, each on at
//              least 5% of the frame. CONTROL: the same look with bloom on has more.
//   halfblock  every square pixel of the half block look is one colour, the mean of the plain scene over that square
//              (in linear light, where the scene is not clipped), within 2 levels. CONTROL: the means of squares one
//              pixel off the look's grid miss.
//   ends       a blend at weight 0 is the first look alone and at 1 the second alone, byte for byte.
//              CONTROL: weight 0.02 differs from the first look.
//   blend      each blend's middle frame is what it promises: FADE every pixel half way in linear light (within 2
//              levels); WIPE the smoothstep edge the header describes, pixel for pixel (within 3); DISSOLVE (hard)
//              every pixel one look or the other, about half of each, both in nearly every 240 pixel tile.
//              CONTROLS: a fade at 0.25 judged as 0.5, a wipe run the other way, and a wipe judged as a dissolve.
//   still      a still camera: the dithers' and the ASCII look's frames N and N + 1 are the same. CONTROL: CW_TEST_LOOK_CRAWL (the
//              pattern moves a pixel a frame, as one not anchored to the screen) changes. Under TAA the ASCII look's
//              glyphs change on under 0.3% of the pixels (a few cells on hard edges, where TAA's own output still
//              moves with its jitter). CONTROL: no hold (LOOK_HOLD=1) changes 1.3 times as many. Half blocks on a
//              palette at DISPLAY under TAA: frames 60 to 77 the same (a square on a palette boundary keeps its
//              colour). CONTROL: no hold (CW_TEST_HALFBLOCK_NO_HOLD=1) flips some.
//   flash      a blend from the plain scene into the two-colour dither over half a second, the camera orbiting, on romdev
//              (tools/flash-check.mjs, its own 8 Hz strobe control must fail): passes. CONTROL: the weight jumping
//              between 0 and 1 every 4 frames (LOOK_FLICKER) fails.
//   split      split screen in one pass (cw_pipeline_set_views_array) draws the looks as the views drawn one by one do,
//              within 1 level. CONTROL: CW_TEST_VIEWS_ARRAY_SHIFT (each view's passes a view off) differs.
//
//   node tests/looks.mjs [--gpus gl,webgpu] [--only none,ascii,...]
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';

await takeSuiteLock('looks.mjs');

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'looks');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (n, d) => { const i = argv.indexOf(n); return i >= 0 ? argv[i + 1] : d; };
const GPUS = opt('--gpus', 'gl,webgpu').split(',');
const ONLY = opt('--only', '').split(',').filter(Boolean);
const FRAMES = 40;

/** one native run of the looks sample: the last frame and the debug state */
function run(gpu, tag, env, frames = FRAMES) {
  const shot = join(OUT, `${gpu}-${tag}.png`), state = join(OUT, `${gpu}-${tag}.json`);
  const args = [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', 'looks', '--frames', String(frames), '--gpu', gpu,
    '--env', JSON.stringify(env), '--shot', shot, '--state', state];
  return new Promise((res, rej) => {
    const p = spawn('node', args, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    p.stdout.on('data', (d) => (out += d));
    p.stderr.on('data', (d) => (out += d));
    p.on('close', (code) => {
      if (code !== 0) return rej(new Error(`looks ${gpu} ${tag} exited ${code}: ${out.slice(-800)}`));
      const s = JSON.parse(readFileSync(state, 'utf8'));
      res({ img: readImage(shot), state: s.state || s });
    });
  });
}

const lin = (v) => { const x = v / 255; return x <= 0.04045 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4; };
const enc = (x) => { x = Math.min(Math.max(x, 0), 1); return 255 * (x <= 0.0031308 ? x * 12.92 : 1.055 * x ** (1 / 2.4) - 0.055); };
function diffCount(a, b, tol = 0) {
  let n = 0, mx = 0;
  for (let i = 0; i < a.w * a.h; i++) {
    let d = 0;
    for (let k = 0; k < 3; k++) d = Math.max(d, Math.abs(a.rgba[i * 4 + k] - b.rgba[i * 4 + k]));
    if (d > tol) n++;
    if (d > mx) mx = d;
  }
  return { n, mx, share: n / (a.w * a.h) };
}
function colours(img) {
  const m = new Map();
  for (let i = 0; i < img.w * img.h; i++) {
    const c = (img.rgba[i * 4] << 16) | (img.rgba[i * 4 + 1] << 8) | img.rgba[i * 4 + 2];
    m.set(c, (m.get(c) || 0) + 1);
  }
  return m;
}

let fails = 0;
const ok = (msg) => console.log(`ok   ${msg}`);
const fail = (msg) => { console.log(`FAIL ${msg}`); fails++; };
const check = (cond, msg) => (cond ? ok(msg) : fail(msg));
const want = (name) => !ONLY.length || ONLY.includes(name);
const PLAIN = { LOOK_PLAIN: '1' };

for (const gpu of GPUS) {
  if (want('none')) {
    const never = await run(gpu, 'none-never', { ...PLAIN });
    const cleared = await run(gpu, 'none-cleared', { ...PLAIN, LOOK_A: 'ascii', LOOK_B: 'bayer', LOOK_T: '0.4', LOOK_OFF_AT: '20' });
    const nudge = await run(gpu, 'none-control', { ...PLAIN, LOOK_A: 'none', LOOK_B: 'bayer', LOOK_T: '0.02' });
    const d = diffCount(never.img, cleared.img);
    check(d.n === 0 && never.state['look.pass_ran'] === 0 && cleared.state['look.pass_ran'] === 0,
      `${gpu} none: a look set and cleared draws the frame without looks byte for byte (${d.n} pixels differ), the pass ran: never ${never.state['look.pass_ran']}, cleared ${cleared.state['look.pass_ran']}`);
    const c = diffCount(never.img, nudge.img);
    check(c.n > 1000, `${gpu} none: control (weight 0.02 toward a dither) differs in ${c.n} pixels`);
  }

  if (want('ascii')) {
    const GLYPHS = '|-/\\_()';
    const RAMP = ' .:-=+*#%@';
    const allowed = new Set([...GLYPHS, ...RAMP].map((ch) => ch.charCodeAt(0) - 32));
    const judge = (img) => {
      const seen = new Map();
      let bad = 0, grey = 0;
      for (let i = 0; i < img.w * img.h; i++) {
        const r = img.rgba[i * 4], g = img.rgba[i * 4 + 1], b = img.rgba[i * 4 + 2];
        if (r !== g || g !== b) grey++;
        seen.set(r, (seen.get(r) || 0) + 1);
        if (!allowed.has(r)) bad++;
      }
      const shapes = [...seen.keys()].filter((k) => GLYPHS.includes(String.fromCharCode(k + 32)));
      return { bad, grey, glyphs: seen.size, shapes: shapes.length };
    };
    const env = { ...PLAIN, LOOK_A: 'ascii', LOOK_GLYPHS: GLYPHS, CW_TEST_LOOK_ASCII_IDS: '1' };
    const j = judge((await run(gpu, 'ascii-ids', env)).img);
    check(j.bad === 0 && j.grey === 0 && j.shapes >= 3,
      `${gpu} ascii: every cell's glyph is from the set or the ramp (${j.bad} pixels outside, ${j.grey} not a probe value); ${j.glyphs} glyphs drawn, ${j.shapes} shapes of the set`);
    const c = judge((await run(gpu, 'ascii-ids-control', { ...env, CW_TEST_LOOK_ASCII_ANY: '1' })).img);
    check(c.bad > 0, `${gpu} ascii: control (shape matching over every glyph) draws ${c.bad} pixels of glyphs outside the set`);
  }

  if (want('dither')) {
    for (const k of ['bayer', 'bluenoise', 'diffuse', 'duotone']) {
      const m = colours((await run(gpu, `dither-${k}`, { ...PLAIN, LOOK_A: k })).img);
      const n = [...m.values()], total = n.reduce((a, b) => a + b, 0);
      check(m.size === 2 && Math.min(...n) > total * 0.05,
        `${gpu} dither ${k}: ${m.size} colours (${[...m.entries()].map(([c, v]) => `#${c.toString(16).padStart(6, '0')} ${(v / total * 100).toFixed(1)}%`).slice(0, 4).join(', ')})`);
    }
    const c = colours((await run(gpu, 'dither-control', { LOOK_A: 'bayer' })).img);
    check(c.size > 2, `${gpu} dither: control (bloom on) has ${c.size} colours`);
  }

  if (want('halfblock')) {
    const hb = (await run(gpu, 'halfblock', { ...PLAIN, LOOK_A: 'halfblock' })).img;
    const plain = (await run(gpu, 'halfblock-plain', { ...PLAIN })).img;
    const half = Math.round(Math.round(hb.h / 54) / 2); /* rows 54 over the output's height: cells of ch, pixels ch / 2 */
    const judge = (off) => {
      let squares = 0, uneven = 0, wrong = 0, worst = 0;
      for (let y0 = 0; y0 + half <= hb.h; y0 += half)
        for (let x0 = 0; x0 + half <= hb.w; x0 += half) {
          const first = (y0 * hb.w + x0) * 4;
          let even = true, clipped = false;
          const sum = [0, 0, 0];
          for (let y = y0; y < y0 + half; y++)
            for (let x = x0; x < x0 + half; x++) {
              const i = (y * hb.w + x) * 4;
              for (let k = 0; k < 3; k++) if (hb.rgba[i + k] !== hb.rgba[first + k]) even = false;
              const py = Math.min(hb.h - 1, y + off), px = Math.min(hb.w - 1, x + off), q = (py * hb.w + px) * 4;
              for (let k = 0; k < 3; k++) { if (plain.rgba[q + k] >= 254) clipped = true; sum[k] += lin(plain.rgba[q + k]); }
            }
          squares++;
          if (!even) uneven++;
          if (clipped) continue;
          let d = 0;
          for (let k = 0; k < 3; k++) d = Math.max(d, Math.abs(enc(sum[k] / (half * half)) - hb.rgba[first + k]));
          if (d > 2) wrong++;
          worst = Math.max(worst, d);
        }
      return { squares, uneven, wrong, worst };
    };
    const j = judge(0);
    check(j.squares > 1000 && j.uneven === 0 && j.wrong === 0,
      `${gpu} halfblock: ${j.squares} squares of ${half} px, ${j.uneven} not one colour, ${j.wrong} off the scene's mean by more than 2 levels (worst ${j.worst.toFixed(2)})`);
    const c = judge(1);
    check(c.wrong >= 200, `${gpu} halfblock: control (means one pixel off the grid) misses ${c.wrong} squares`);
  }

  if (want('ends') || want('blend')) {
    const A = { ...PLAIN, LOOK_A: 'halfblock' }, B = { ...PLAIN, LOOK_A: 'bayer' };
    const a = (await run(gpu, 'end-a', A)).img, b = (await run(gpu, 'end-b', B)).img;
    if (want('ends')) {
      const t0 = (await run(gpu, 'end-t0', { ...PLAIN, LOOK_A: 'halfblock', LOOK_B: 'bayer', LOOK_T: '0' })).img;
      const t1 = (await run(gpu, 'end-t1', { ...PLAIN, LOOK_A: 'halfblock', LOOK_B: 'bayer', LOOK_T: '1' })).img;
      const d0 = diffCount(t0, a), d1 = diffCount(t1, b);
      check(d0.n === 0 && d1.n === 0, `${gpu} ends: weight 0 is the first look (${d0.n} pixels differ), weight 1 the second (${d1.n})`);
      const c = diffCount((await run(gpu, 'end-control', { ...PLAIN, LOOK_A: 'halfblock', LOOK_B: 'bayer', LOOK_T: '0.02' })).img, a);
      check(c.n > 0, `${gpu} ends: control (weight 0.02) differs from the first look in ${c.n} pixels`);
    }
    if (want('blend')) {
      const blendOf = (w) => (i) => w(i);
      const judgeMix = (img, wfn, tol) => {
        let bad = 0, n = 0, worst = 0;
        for (let i = 0; i < img.w * img.h; i++) {
          const w = wfn(i % img.w, (i / img.w) | 0);
          let skip = false;
          for (let k = 0; k < 3; k++) if (a.rgba[i * 4 + k] >= 254 || b.rgba[i * 4 + k] >= 254) skip = true;
          if (skip) continue;
          n++;
          let d = 0;
          for (let k = 0; k < 3; k++) d = Math.max(d, Math.abs(enc(lin(a.rgba[i * 4 + k]) * (1 - w) + lin(b.rgba[i * 4 + k]) * w) - img.rgba[i * 4 + k]));
          if (d > tol) bad++;
          worst = Math.max(worst, d);
        }
        return { bad, n, worst };
      };
      void blendOf;
      /* FADE */
      const fade = (await run(gpu, 'blend-fade', { ...PLAIN, LOOK_A: 'halfblock', LOOK_B: 'bayer', LOOK_T: '0.5' })).img;
      const jf = judgeMix(fade, () => 0.5, 2);
      check(jf.bad === 0 && jf.n > fade.w * fade.h * 0.5, `${gpu} blend fade: ${jf.bad} of ${jf.n} pixels off half way by more than 2 levels (worst ${jf.worst.toFixed(2)})`);
      const fc = judgeMix((await run(gpu, 'blend-fade-control', { ...PLAIN, LOOK_A: 'halfblock', LOOK_B: 'bayer', LOOK_T: '0.25' })).img, () => 0.5, 2);
      check(fc.bad > jf.n * 0.1, `${gpu} blend fade: control (0.25 judged as 0.5) misses ${fc.bad} pixels`);
      /* WIPE: the header's edge, angle 0.4, softness 0.15 */
      const wipeW = (img, t, s, ang) => {
        const dx = Math.cos(ang), dy = Math.sin(ang), W = img.w, H = img.h;
        const lo = Math.min(0, dx * W) + Math.min(0, dy * H), hi = Math.max(0, dx * W) + Math.max(0, dy * H);
        const e = t * (1 + 2 * s) - s;
        return (x, y) => {
          const f = ((x + 0.5) * dx + (H - (y + 0.5)) * dy - lo) / (hi - lo);
          const u = Math.min(Math.max((f - (e - s)) / (2 * s), 0), 1);
          return 1 - u * u * (3 - 2 * u);
        };
      };
      const wenv = { ...PLAIN, LOOK_A: 'halfblock', LOOK_B: 'bayer', LOOK_T: '0.5', LOOK_HOW: 'wipe', LOOK_SOFT: '0.15', LOOK_ANGLE: '0.4' };
      const wipe = (await run(gpu, 'blend-wipe', wenv)).img;
      const jw = judgeMix(wipe, wipeW(wipe, 0.5, 0.15, 0.4), 3);
      check(jw.bad <= jw.n * 0.0005, `${gpu} blend wipe: ${jw.bad} of ${jw.n} pixels off the edge's blend by more than 3 levels (worst ${jw.worst.toFixed(2)})`);
      const wc = judgeMix((await run(gpu, 'blend-wipe-control', { ...wenv, LOOK_ANGLE: String(0.4 + Math.PI) })).img, wipeW(wipe, 0.5, 0.15, 0.4), 3);
      check(wc.bad > jw.n * 0.1, `${gpu} blend wipe: control (the wipe run the other way) misses ${wc.bad} pixels`);
      /* DISSOLVE (hard edge) */
      const judgeDissolve = (img) => {
        let neither = 0, nb = 0, n = 0;
        const T = 240, tiles = new Map();
        for (let i = 0; i < img.w * img.h; i++) {
          let da = 0, db = 0;
          for (let k = 0; k < 3; k++) {
            da = Math.max(da, Math.abs(img.rgba[i * 4 + k] - a.rgba[i * 4 + k]));
            db = Math.max(db, Math.abs(img.rgba[i * 4 + k] - b.rgba[i * 4 + k]));
          }
          if (da === 0 && db === 0) continue; /* the looks agree here */
          n++;
          const isB = db <= 1 && db < da, isA = da <= 1 && da <= db;
          if (!isA && !isB) neither++;
          if (isB) nb++;
          const key = `${((i % img.w) / T) | 0},${((i / img.w / T) | 0)}`;
          const t = tiles.get(key) || [0, 0];
          t[isB ? 1 : 0]++;
          tiles.set(key, t);
        }
        const both = [...tiles.values()].filter(([x, y]) => x > 50 && y > 50).length;
        return { neither, share: nb / n, both: both / tiles.size };
      };
      const denv = { ...PLAIN, LOOK_A: 'halfblock', LOOK_B: 'bayer', LOOK_T: '0.5', LOOK_HOW: 'dissolve' };
      const jd = judgeDissolve((await run(gpu, 'blend-dissolve', denv)).img);
      check(jd.neither === 0 && jd.share > 0.35 && jd.share < 0.65 && jd.both > 0.9,
        `${gpu} blend dissolve: ${jd.neither} pixels neither look, ${(jd.share * 100).toFixed(1)}% the second, ${(jd.both * 100).toFixed(0)}% of 240 px tiles hold both`);
      const dc = judgeDissolve((await run(gpu, 'blend-dissolve-control', { ...denv, LOOK_HOW: 'wipe' })).img);
      check(dc.both < 0.9, `${gpu} blend dissolve: control (a wipe judged as a dissolve) has both looks in ${(dc.both * 100).toFixed(0)}% of tiles`);
    }
  }

  if (want('still')) {
    for (const k of ['bayer', 'diffuse', 'ascii']) {
      const f0 = (await run(gpu, `still-${k}-a`, { ...PLAIN, LOOK_A: k }, 60)).img, f1 = (await run(gpu, `still-${k}-b`, { ...PLAIN, LOOK_A: k }, 61)).img;
      const d = diffCount(f0, f1);
      check(d.n === 0, `${gpu} still ${k}: frames 60 and 61 differ in ${d.n} pixels`);
      if (k !== 'ascii') {
        const c0 = (await run(gpu, `still-${k}-ca`, { ...PLAIN, LOOK_A: k, CW_TEST_LOOK_CRAWL: '1' }, 60)).img;
        const c1 = (await run(gpu, `still-${k}-cb`, { ...PLAIN, LOOK_A: k, CW_TEST_LOOK_CRAWL: '1' }, 61)).img;
        const c = diffCount(c0, c1);
        check(c.share > 0.05, `${gpu} still ${k}: control (a pattern that moves with the frame) changes ${(c.share * 100).toFixed(1)}% of the pixels`);
      }
    }
    /* the glyph hold under TAA's jitter: the glyph probe's cells that change between frames 60 and 61 */
    const flips = async (tag, extra) => {
      const env = { ...PLAIN, LOOK_A: 'ascii', LOOK_TAA: '1', CW_TEST_LOOK_ASCII_IDS: '1', ...extra };
      const a0 = (await run(gpu, `${tag}-a`, env, 60)).img, a1 = (await run(gpu, `${tag}-b`, env, 61)).img;
      return diffCount(a0, a1).share;
    };
    const held = await flips('still-taa', {}), loose = await flips('still-taa-control', { LOOK_HOLD: '1' });
    check(held < 0.003, `${gpu} still ascii under TAA: the glyphs of ${(held * 100).toFixed(3)}% of the pixels change between frames 60 and 61`);
    check(loose > held * 1.3, `${gpu} still ascii under TAA: control (no hold) changes ${(loose * 100).toFixed(3)}%`);
    /* half blocks on a palette at DISPLAY under TAA: the finished picture moves with the jitter, and a square whose
     * mean sits on a palette boundary would flip (in the sample: two frames in eight). Frames 60 to 77, each against
     * the one before */
    const steps = async (tag, extra) => {
      const env = { LOOK_A: 'palette', LOOK_STAGE: 'display', LOOK_TAA: '1', ...extra };
      let prev = null, moved = 0, worst = 0;
      for (let fr = 60; fr < 78; fr++) {
        const img = (await run(gpu, `${tag}-${fr}`, env, fr)).img;
        if (prev) { const d = diffCount(prev, img).n; worst = Math.max(worst, d); if (d) moved++; }
        prev = img;
      }
      return { moved, worst };
    };
    const ph = await steps('still-palette', {}), pc = await steps('still-palette-control', { CW_TEST_HALFBLOCK_NO_HOLD: '1' });
    check(ph.moved === 0, `${gpu} still palette at DISPLAY under TAA: ${ph.moved} of 17 frames change (up to ${ph.worst} pixels)`);
    check(pc.moved > 0, `${gpu} still palette at DISPLAY under TAA: control (no hold) changes ${pc.moved} of 17 frames (up to ${pc.worst} pixels)`);
  }

  if (want('split')) {
    const env = { ...PLAIN, LOOK_A: 'ascii', LOOK_B: 'diffuse', LOOK_T: '0.5', LOOK_HOW: 'wipe', LOOK_SOFT: '0.1', LOOK_SPLIT: '1', LOOK_BOXES: '1' };
    const one = await run(gpu, 'split-array', { ...env, CW_TEST_VIEWS_ARRAY: '1' });
    const each = await run(gpu, 'split-views', { ...env, CW_TEST_VIEWS_ARRAY: '0' });
    const d = diffCount(one.img, each.img, 1);
    check(one.state['look.array'] === 1 && each.state['look.array'] === 0 && d.n === 0,
      `${gpu} split: one pass (array ${one.state['look.array']}) and view by view (array ${each.state['look.array']}) differ by more than 1 level in ${d.n} pixels (max ${d.mx})`);
    const c = await run(gpu, 'split-control', { ...env, CW_TEST_VIEWS_ARRAY: '1', CW_TEST_VIEWS_ARRAY_SHIFT: '1' });
    const dc = diffCount(c.img, each.img, 1);
    check(dc.share > 0.05, `${gpu} split: control (each view's passes a view off) differs in ${(dc.share * 100).toFixed(1)}% of the pixels`);
  }
}
if (want('flash')) {
  for (const gpu of GPUS) {
    const flash = (env) => new Promise((res) => {
      const p = spawn('node', [join(ROOT, 'tools/flash-check.mjs'), '--app', 'looks', '--frames', '240', '--from', '30', '--gpu', gpu,
        '--env', JSON.stringify(env)], { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
      let out = '';
      p.stdout.on('data', (d) => (out += d));
      p.stderr.on('data', (d) => (out += d));
      p.on('close', (code) => res({ code, out }));
    });
    const ramp = await flash({ LOOK_A: 'none', LOOK_B: 'duotone', LOOK_RAMP: '30', LOOK_RAMP_AT: '60', LOOK_ORBIT: '1' });
    check(ramp.code === 0, `${gpu} flash: a half-second blend into a two-colour dither passes the flash check (${ramp.out.trim().split('\n').slice(-1)[0]})`);
    const strobe = await flash({ LOOK_A: 'none', LOOK_B: 'duotone', LOOK_FLICKER: '4', LOOK_RAMP_AT: '60', LOOK_ORBIT: '1' });
    check(strobe.code !== 0, `${gpu} flash: control (the weight jumping every 4 frames) fails it (${strobe.out.trim().split('\n').slice(-1)[0]})`);
  }
}
console.log(fails ? `FAIL looks (${fails})` : 'ok   looks');
process.exit(fails ? 1 : 0);
