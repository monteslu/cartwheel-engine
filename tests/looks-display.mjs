// The DISPLAY stage and the dither tone options (cartwheel/look.h, src/render/looks.c) on the looks_onebit sample,
// natively on GL and WebGPU at 1920x1080. The look under test: an ORDERED blue noise dither at the DISPLAY stage, one
// dither pixel per screen pixel, every tone option on. Its reference: a game's own one-bit POST pass (the sample's
// "trial" pass, shaders line for line), run last in the chain on the same frame. Every check runs a control that must
// fail.
//
//   reproduce  the look and the reference, thresholded by the same blue noise (trial_em), differ in at most 0.2% of
//              the pixels; the same picture written as a CUSTOM look from the accessors too. The bound: the reference
//              rounds its tone to a half float (an R16F target) before the threshold and the look compares at full
//              precision, so a pixel whose tone lies within half a half-float step of its threshold (2^-12 near 1)
//              can go either way: about 1 pixel in 2000 (0.05% measured). With the reference's own blue noise the
//              patterns differ pixel by pixel, so there the tone is compared: the mean of every 16x16 tile, within
//              0.03. CONTROLS: the look with no tone options misses the reference (over 5% of the pixels, and its
//              tiles by more than 0.03); the reference's own matrix differs pixel by pixel (over 5%).
//   two        the look at DISPLAY over bloom, the AgX tone curve and a vignette: exactly two colours on screen, black
//              and white, each on at least 5%. CONTROL: the same look at the SCENE stage has more.
//   options    each tone option alone (hue, grey, sharpen, outline, gamma, contrast) and the Bayer pattern: the look
//              matches the reference with that option alone (within 0.2%). CONTROL: the look without it misses (by
//              over 0.3% of the pixels: the option does something), and the plain programs run without options
//              (look.tone 0) while any option runs the tone programs (look.tone 1).
//   pixel      looks_onebit_720 (1280x720): pixel 2 in output pixels makes every 2x2 block one colour. CONTROL: pixel 2
//              in 1080 line pixels is 1 output pixel there (blocks of two colours).
//   pair       a wipe between the look (SCENE stage) and the CUSTOM look (DISPLAY) draws each at its own stage:
//              about half the picture is the SCENE look alone's and half the DISPLAY look alone's. CONTROL: the old
//              rule (CW_TEST_LOOK_ONE_STAGE=1, the pair at DISPLAY whatever the weight) is not the SCENE look's.
//   split      two views each with its own post chain: inside each view (off the darkened seams) exactly two
//              colours; the shared chain (the look once over the laid-out views): two colours, one pass and view by
//              view within the bound. CONTROLS: the SCENE stage has more colours in both.
//   once       the tone options are worked out once a dither pixel (small targets: the brightness, the unsharp mask
//              and outlines, then the threshold): the picture is the one the tone in every screen pixel draws
//              (CW_TEST_LOOK_TONE_EACH_PIXEL=1, how the look worked to v0.65.1) within 0.02% of the pixels (a tone
//              on its threshold the compiler rounds differently in the two programs; 0.007% measured), for the
//              ordered dither at 2 (output and 1080 line) and 6 pixels with blue noise and Bayer, and the diffused at
//              2 and 3. One dither pixel a screen pixel keeps the tone in the screen pixels (nothing to share).
//              CONTROL: the brightness target without the unsharp mask's margin (CW_TEST_LOOK_TONE_NO_MARGIN=1)
//              differs along the view's edges.
//   xfade      a fade between looks of two stages (the one-bit look at DISPLAY and half blocks at SCENE, a
//              dissolve, either way) has no step at either end: the pair at weight 0.02 and 0.98 and the end look
//              alone (what a game sets when the fade is over) differ in no 32x32 tile by more than 2/255 on average
//              (the blend's own 2% is about 0.7). The fade over half a second with the camera orbiting passes the
//              flash check, both ways. CONTROL: the old rule (CW_TEST_LOOK_ONE_STAGE=1: the pair at DISPLAY
//              whatever the weight) steps at the SCENE look's end (over 4/255 in a tile).
//   still      a still camera: frames 60 and 61 are the same. CONTROL: CW_TEST_LOOK_CRAWL changes them.
//   flash      a fade from the finished picture into the one-bit look over half a second, the camera orbiting
//              (tools/flash-check.mjs): passes. CONTROL: the weight jumping every 4 frames fails.
//
//   node tests/looks-display.mjs [--gpus gl,webgpu] [--only reproduce,two,...]
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';
import { takeSuiteLock } from '../tools/lib/suite-lock.mjs';

await takeSuiteLock('looks-display.mjs');

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'gate', 'looks-display');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const opt = (n, d) => { const i = argv.indexOf(n); return i >= 0 ? argv[i + 1] : d; };
const GPUS = opt('--gpus', 'gl,webgpu').split(',');
const ONLY = opt('--only', '').split(',').filter(Boolean);
const FRAMES = 40;
const BOUND = 0.002; /* the share of pixels the look and the reference may differ in (see reproduce) */
const ONCE_BOUND = 0.0002; /* once: a tone the compiler rounds differently (fused multiply-adds in one program, not the other) on its threshold */

function run(gpu, tag, env, { frames = FRAMES, app = 'looks_onebit' } = {}) {
  const shot = join(OUT, `${gpu}-${tag}.png`), state = join(OUT, `${gpu}-${tag}.json`);
  const args = [join(ROOT, 'tools/run-cart.mjs'), '--host', 'native', '--app', app, '--frames', String(frames), '--gpu', gpu,
    '--env', JSON.stringify(env), '--shot', shot, '--state', state];
  return new Promise((res, rej) => {
    const p = spawn('node', args, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    p.stdout.on('data', (d) => (out += d));
    p.stderr.on('data', (d) => (out += d));
    p.on('close', (code) => {
      if (code !== 0) return rej(new Error(`${app} ${gpu} ${tag} exited ${code}: ${out.slice(-800)}`));
      const s = JSON.parse(readFileSync(state, 'utf8'));
      res({ img: readImage(shot), state: s.state || s });
    });
  });
}

function differ(a, b) {
  let n = 0;
  for (let i = 0; i < a.w * a.h; i++)
    if (a.rgba[i * 4] !== b.rgba[i * 4] || a.rgba[i * 4 + 1] !== b.rgba[i * 4 + 1] || a.rgba[i * 4 + 2] !== b.rgba[i * 4 + 2]) n++;
  return n / (a.w * a.h);
}
function colours(img) {
  const m = new Map();
  for (let i = 0; i < img.w * img.h; i++) {
    const c = (img.rgba[i * 4] << 16) | (img.rgba[i * 4 + 1] << 8) | img.rgba[i * 4 + 2];
    m.set(c, (m.get(c) || 0) + 1);
  }
  return m;
}
const showColours = (m) => {
  const total = [...m.values()].reduce((a, b) => a + b, 0);
  return `${m.size} colours (${[...m.entries()].sort((x, y) => y[1] - x[1]).slice(0, 3).map(([c, v]) => `#${c.toString(16).padStart(6, '0')} ${(v / total * 100).toFixed(1)}%`).join(', ')})`;
};
/* the worst difference of two one-bit pictures' 32x32 tile means (the share of white in each tile) */
function tileTone(a, b, T = 32) {
  let worst = 0, sum = 0, n = 0;
  for (let ty = 0; ty + T <= a.h; ty += T)
    for (let tx = 0; tx + T <= a.w; tx += T) {
      let sa = 0, sb = 0;
      for (let y = ty; y < ty + T; y++)
        for (let x = tx; x < tx + T; x++) sa += a.rgba[(y * a.w + x) * 4], sb += b.rgba[(y * a.w + x) * 4];
      const d = Math.abs(sa - sb) / (255 * T * T);
      worst = Math.max(worst, d), sum += d, n++;
    }
  return { worst, mean: sum / n };
}
const pc = (x) => `${(x * 100).toFixed(3)}%`;

let fails = 0;
const ok = (msg) => console.log(`ok   ${msg}`);
const fail = (msg) => { console.log(`FAIL ${msg}`); fails++; };
const check = (cond, msg) => (cond ? ok(msg) : fail(msg));
const want = (name) => !ONLY.length || ONLY.includes(name);

for (const gpu of GPUS) {
  if (want('reproduce')) {
    const look = await run(gpu, 'look', { ONEBIT: 'look' });
    const ref = (await run(gpu, 'trial-em', { ONEBIT: 'trial_em' })).img;
    const own = (await run(gpu, 'trial', { ONEBIT: 'trial' })).img;
    const custom = (await run(gpu, 'custom', { ONEBIT: 'custom' })).img;
    const bare = await run(gpu, 'look-bare', { ONEBIT: 'look', ONEBIT_ONLY: 'none' });
    const d = differ(look.img, ref), dc = differ(custom, ref);
    check(d <= BOUND && dc <= BOUND && look.state['look.stage'] === 1 && look.state['look.tone'] === 1,
      `${gpu} reproduce: the look and the reference differ in ${pc(d)} of the pixels, the CUSTOM look and the reference in ${pc(dc)} (bound ${pc(BOUND)}); stage ${look.state['look.stage']}, tone programs ${look.state['look.tone']}`);
    const t = tileTone(look.img, own);
    check(t.worst <= 0.03, `${gpu} reproduce: against the reference's own blue noise every 32x32 tile's tone within ${t.worst.toFixed(4)} (mean ${t.mean.toFixed(4)}; bound 0.03)`);
    const cb = differ(bare.img, ref), ct = tileTone(bare.img, own), co = differ(look.img, own);
    check(cb > 0.05 && ct.worst > 0.03 && co > 0.05 && bare.state['look.tone'] === 0,
      `${gpu} reproduce: controls: the look without tone options misses the reference in ${pc(cb)} of the pixels (tiles by up to ${ct.worst.toFixed(3)}, plain programs: tone ${bare.state['look.tone']}); the reference's own matrix differs pixel by pixel in ${pc(co)}`);
  }

  if (want('two')) {
    const m = colours((await run(gpu, 'two', { ONEBIT: 'look' })).img);
    const v = [...m.values()], total = v.reduce((a, b) => a + b, 0);
    check(m.size === 2 && m.has(0) && m.has(0xffffff) && Math.min(...v) > total * 0.05, `${gpu} two: DISPLAY over bloom, AgX and a vignette: ${showColours(m)}`);
    const c = colours((await run(gpu, 'two-control', { ONEBIT: 'scene' })).img);
    check(c.size > 2, `${gpu} two: control (the SCENE stage, bloom and the tone curve after it): ${showColours(c)}`);
  }

  if (want('options')) {
    const bare = (await run(gpu, 'opt-none', { ONEBIT: 'look', ONEBIT_ONLY: 'none' })).img;
    for (const o of ['hue', 'grey', 'sharpen', 'outline', 'gamma', 'contrast', 'bayer']) {
      /* (the unsharp mask at 3 times the reference's gain: at 0.9 it moves few pixels, nearly all on their threshold) */
      const env = o === 'bayer' ? { ONEBIT_PATTERN: 'bayer' } : o === 'sharpen' ? { ONEBIT_ONLY: o, ONEBIT_SHARPEN: '2.7' } : { ONEBIT_ONLY: o };
      const look = await run(gpu, `opt-${o}`, { ONEBIT: 'look', ...env });
      const ref = (await run(gpu, `opt-${o}-ref`, { ONEBIT: 'trial_em', ...env })).img;
      const d = differ(look.img, ref);
      const off = o === 'bayer' ? (await run(gpu, 'opt-bayer-off', { ONEBIT: 'look' })).img : bare;
      /* the pixels the option changes: the reference sides with the look there, not with the look without it */
      let changed = 0, agree = 0;
      for (let i = 0; i < ref.w * ref.h; i++) {
        if (look.img.rgba[i * 4] === off.rgba[i * 4]) continue;
        changed++;
        if (look.img.rgba[i * 4] === ref.rgba[i * 4]) agree++;
      }
      check(d <= BOUND && changed >= 1000 && agree >= changed * 0.9 && (o === 'bayer' || o === 'contrast' || look.state['look.tone'] === 1),
        `${gpu} options ${o}: the look and the reference with it alone differ in ${pc(d)} (bound ${pc(BOUND)}); of the ${changed} pixels it changes the reference has ${pc(agree / Math.max(changed, 1))} as the look; tone programs ${look.state['look.tone']}`);
      const ctl = (changed - agree) / Math.max(changed, 1); /* the look without it, judged the same way */
      check(ctl < 0.9, `${gpu} options ${o}: control (the look without it, judged the same way) has ${pc(ctl)} of those pixels as the reference: fails`);
    }
  }

  if (want('pixel')) {
    const blocks = (img) => {
      let mixed = 0, n = 0;
      for (let y = 0; y + 2 <= img.h; y += 2)
        for (let x = 0; x + 2 <= img.w; x += 2) {
          const v = [[x, y], [x + 1, y], [x, y + 1], [x + 1, y + 1]].map(([i, j]) => img.rgba[(j * img.w + i) * 4]);
          n++;
          if (v.some((k) => k !== v[0])) mixed++;
        }
      return mixed / n;
    };
    const p = (await run(gpu, 'pixel-out', { ONEBIT: 'look', ONEBIT_PIXEL: '2', ONEBIT_OUTPX: '1' }, { app: 'looks_onebit_720' })).img;
    const c = (await run(gpu, 'pixel-1080', { ONEBIT: 'look', ONEBIT_PIXEL: '2', ONEBIT_OUTPX: '0' }, { app: 'looks_onebit_720' })).img;
    const mp = blocks(p), mc = blocks(c);
    check(p.h === 720 && mp === 0, `${gpu} pixel: at ${p.w}x${p.h}, pixel 2 in output pixels: ${pc(mp)} of the 2x2 blocks hold both colours`);
    check(mc > 0.2, `${gpu} pixel: control (pixel 2 in 1080 line pixels, 1 output pixel at 720): ${pc(mc)} of the blocks hold both`);
  }

  if (want('pair')) {
    /* the share of pixels within 3/255 of another picture's */
    const near = (a, b) => {
      let n = 0;
      for (let i = 0; i < a.w * a.h; i++)
        if (Math.abs(a.rgba[i * 4] - b.rgba[i * 4]) <= 3 && Math.abs(a.rgba[i * 4 + 1] - b.rgba[i * 4 + 1]) <= 3 && Math.abs(a.rgba[i * 4 + 2] - b.rgba[i * 4 + 2]) <= 3) n++;
      return n / (a.w * a.h);
    };
    const env = { ONEBIT: 'scene', ONEBIT_PAIR: '1', ONEBIT_T: '0.5', ONEBIT_HOW: 'wipe' };
    const w = await run(gpu, 'pair-wipe', env);
    const sceneAlone = (await run(gpu, 'pair-scene-alone', { ONEBIT: 'scene' })).img, dispAlone = (await run(gpu, 'pair-display-alone', { ONEBIT: 'custom' })).img;
    const ns = near(w.img, sceneAlone), nd = near(w.img, dispAlone);
    check(ns >= 0.45 && nd >= 0.45 && w.state['look.stage'] === 2 && w.state['look.ran'] === 7,
      `${gpu} pair: a SCENE look wiped into a DISPLAY one draws each at its own stage (look.stage ${w.state['look.stage']}, ran ${w.state['look.ran']}): ${pc(ns)} of the pixels are the SCENE look alone's, ${pc(nd)} the DISPLAY look alone's (each at least 45%)`);
    const c = await run(gpu, 'pair-wipe-one-stage', { ...env, CW_TEST_LOOK_ONE_STAGE: '1' });
    const cs = near(c.img, sceneAlone);
    check(cs < 0.45, `${gpu} pair: control (the pair at DISPLAY whatever the weight, the old rule): ${pc(cs)} of the pixels are the SCENE look alone's`);
  }

  if (want('split')) {
    /* the views' pixels away from the seams (each view's picture is drawn into the output with its edges darkened) */
    const inner = (img, seam = 4) => {
      const m = new Map();
      for (let y = seam; y < img.h - seam; y++)
        for (let x = seam; x < img.w - seam; x++) {
          if (Math.abs(x - img.w / 2) < seam + 1) continue;
          const i = (y * img.w + x) * 4, c = (img.rgba[i] << 16) | (img.rgba[i + 1] << 8) | img.rgba[i + 2];
          m.set(c, (m.get(c) || 0) + 1);
        }
      return m;
    };
    const own = await run(gpu, 'split-own', { ONEBIT: 'look', ONEBIT_SPLIT: '2' });
    const mo = inner(own.img);
    check(mo.size === 2 && own.state['look.views'] === 2, `${gpu} split: two views each with its own chain, the look at DISPLAY in each (${own.state['look.views']} views), inside the views: ${showColours(mo)}`);
    const oc = inner((await run(gpu, 'split-own-control', { ONEBIT: 'scene', ONEBIT_SPLIT: '2' })).img);
    check(oc.size > 2, `${gpu} split: control (the SCENE stage in each view): ${showColours(oc)}`);
    const env = { ONEBIT: 'look', ONEBIT_SPLIT: '1' };
    const one = await run(gpu, 'split-array', { ...env, CW_TEST_VIEWS_ARRAY: '1' });
    const each = await run(gpu, 'split-views', { ...env, CW_TEST_VIEWS_ARRAY: '0' });
    const d = differ(one.img, each.img), m1 = colours(one.img);
    check(one.state['onebit.array'] === 1 && each.state['onebit.array'] === 0 && d <= BOUND && m1.size === 2,
      `${gpu} split: the shared chain, the look once over the laid-out views after it: one pass (array ${one.state['onebit.array']}) and view by view (array ${each.state['onebit.array']}) differ in ${pc(d)} (bound ${pc(BOUND)}); ${showColours(m1)}`);
    const c = colours((await run(gpu, 'split-control', { ...env, ONEBIT: 'scene', CW_TEST_VIEWS_ARRAY: '1' })).img);
    check(c.size > 2, `${gpu} split: control (the SCENE stage under the shared chain): ${showColours(c)}`);
  }

  if (want('once')) {
    const cases = [
      ['ord-2', { ONEBIT_PIXEL: '2' }],
      ['ord-2-bayer', { ONEBIT_PIXEL: '2', ONEBIT_OUTPX: '0', ONEBIT_PATTERN: 'bayer' }],
      ['ord-6', { ONEBIT_PIXEL: '6', ONEBIT_OUTPX: '0', ONEBIT_SHARPEN: '2.7' }],
      ['dif-2', { ONEBIT_KIND: 'diffuse', ONEBIT_PIXEL: '2' }],
      ['dif-3', { ONEBIT_KIND: 'diffuse', ONEBIT_PIXEL: '3', ONEBIT_OUTPX: '0' }],
    ];
    for (const [tag, env] of cases) {
      const once = await run(gpu, `once-${tag}`, { ONEBIT: 'look', ...env });
      const each = await run(gpu, `once-${tag}-each`, { ONEBIT: 'look', ...env, CW_TEST_LOOK_TONE_EACH_PIXEL: '1' });
      const d = differ(once.img, each.img), m = colours(once.img);
      check(d <= ONCE_BOUND && m.size === 2 && once.state['look.tone_small'] === 1 && each.state['look.tone_small'] === 0,
        `${gpu} once ${tag}: the tone once a dither pixel and in every screen pixel differ in ${pc(d)} (bound ${pc(ONCE_BOUND)}); ${showColours(m)}; small targets ${once.state['look.tone_small']} / ${each.state['look.tone_small']}`);
    }
    {
      const one = await run(gpu, 'once-1', { ONEBIT: 'look' });
      check(one.state['look.tone'] === 1 && one.state['look.tone_small'] === 0, `${gpu} once: one dither pixel a screen pixel works the tone out in the screen pixels (small targets ${one.state['look.tone_small']})`);
    }
    const env = { ONEBIT: 'look', ONEBIT_PIXEL: '6', ONEBIT_OUTPX: '0', ONEBIT_SHARPEN: '2.7' };
    const a = (await run(gpu, 'once-margin', { ...env, CW_TEST_LOOK_TONE_NO_MARGIN: '1' })).img;
    const b = (await run(gpu, 'once-margin-each', { ...env, CW_TEST_LOOK_TONE_EACH_PIXEL: '1' })).img;
    const c = differ(a, b);
    check(c > 0, `${gpu} once: control (the brightness target without the unsharp mask's margin) differs in ${pc(c)}`);
  }

  if (want('xfade')) {
    /* the worst 32x32 tile's mean difference, 0..255 */
    const tileStep = (a, b, T = 32) => {
      let w = 0;
      for (let ty = 0; ty + T <= a.h; ty += T)
        for (let tx = 0; tx + T <= a.w; tx += T) {
          let s = 0;
          for (let y = ty; y < ty + T; y++)
            for (let x = tx; x < tx + T; x++) {
              const i = (y * a.w + x) * 4;
              s += Math.abs(a.rgba[i] - b.rgba[i]) + Math.abs(a.rgba[i + 1] - b.rgba[i + 1]) + Math.abs(a.rgba[i + 2] - b.rgba[i + 2]);
            }
          w = Math.max(w, s / (3 * T * T));
        }
      return w;
    };
    for (const rev of ['0', '1']) {
      const name = rev === '0' ? 'DISPLAY to SCENE' : 'SCENE to DISPLAY';
      const base = { ONEBIT: 'look', ONEBIT_XFADE: '30', ONEBIT_XREV: rev };
      const start = (await run(gpu, `xf${rev}-start`, { ...base, ONEBIT_XT: '0' })).img, end = (await run(gpu, `xf${rev}-end`, { ...base, ONEBIT_XT: '1' })).img;
      const near0 = await run(gpu, `xf${rev}-t02`, { ...base, ONEBIT_XT: '0.02' }), near1 = await run(gpu, `xf${rev}-t98`, { ...base, ONEBIT_XT: '0.98' });
      const s0 = tileStep(near0.img, start), s1 = tileStep(near1.img, end);
      check(s0 <= 2 && s1 <= 2 && near1.state['look.stage'] === 2,
        `${gpu} xfade ${name}: at weight 0.02 the start look alone differs by up to ${s0.toFixed(2)}/255 in a 32x32 tile, at 0.98 the end look alone by up to ${s1.toFixed(2)} (bound 2); both stages ran (look.stage ${near1.state['look.stage']})`);
      const ctl = await run(gpu, `xf${rev}-ctl`, { ...base, ONEBIT_XT: rev === '0' ? '0.98' : '0.02', CW_TEST_LOOK_ONE_STAGE: '1' });
      const cs = tileStep(ctl.img, rev === '0' ? end : start);
      check(cs > 4, `${gpu} xfade ${name}: control (the pair at DISPLAY whatever the weight) steps by ${cs.toFixed(2)}/255 at the half blocks' end`);
    }
  }

  if (want('still')) {
    const a = (await run(gpu, 'still-a', { ONEBIT: 'look' }, { frames: 60 })).img, b = (await run(gpu, 'still-b', { ONEBIT: 'look' }, { frames: 61 })).img;
    const d = differ(a, b);
    check(d === 0, `${gpu} still: frames 60 and 61 differ in ${pc(d)}`);
    const ca = (await run(gpu, 'still-ca', { ONEBIT: 'look', CW_TEST_LOOK_CRAWL: '1' }, { frames: 60 })).img;
    const cb = (await run(gpu, 'still-cb', { ONEBIT: 'look', CW_TEST_LOOK_CRAWL: '1' }, { frames: 61 })).img;
    const c = differ(ca, cb);
    check(c > 0.05, `${gpu} still: control (a pattern that moves with the frame) changes ${pc(c)}`);
  }
}

if (want('flash')) {
  for (const gpu of GPUS) {
    const flash = (env) => new Promise((res) => {
      const p = spawn('node', [join(ROOT, 'tools/flash-check.mjs'), '--app', 'looks_onebit', '--frames', '240', '--from', '30', '--gpu', gpu,
        '--env', JSON.stringify(env)], { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
      let out = '';
      p.stdout.on('data', (d) => (out += d));
      p.stderr.on('data', (d) => (out += d));
      p.on('close', (code) => res({ code, out }));
    });
    const ramp = await flash({ ONEBIT: 'look', ONEBIT_RAMP: '30', ONEBIT_ORBIT: '1' });
    check(ramp.code === 0, `${gpu} flash: a half-second fade into the one-bit look passes the flash check (${ramp.out.trim().split('\n').slice(-1)[0]})`);
    for (const rev of ['0', '1']) {
      const x = await flash({ ONEBIT: 'look', ONEBIT_XFADE: '30', ONEBIT_XREV: rev, ONEBIT_ORBIT: '1' });
      check(x.code === 0, `${gpu} flash: a half-second dissolve ${rev === '0' ? 'from the one-bit look (DISPLAY) into half blocks (SCENE)' : 'from half blocks (SCENE) into the one-bit look (DISPLAY)'} passes the flash check (${x.out.trim().split('\n').slice(-1)[0]})`);
    }
    const strobe = await flash({ ONEBIT: 'look', ONEBIT_FLICKER: '4', ONEBIT_ORBIT: '1' });
    check(strobe.code !== 0, `${gpu} flash: control (the weight jumping every 4 frames) fails it (${strobe.out.trim().split('\n').slice(-1)[0]})`);
  }
}
console.log(fails ? `FAIL looks-display (${fails})` : 'ok   looks-display');
process.exit(fails ? 1 : 0);
