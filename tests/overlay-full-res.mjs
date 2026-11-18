// Overlays draw at the output's full size while the 3D scene renders at a share of it, and a game sets that share
// below the Resolution setting's 50% floor (cw_quality_scene_scale). samples/overlay_full_res on romdev, GL and WebGPU:
// the scene at 25% of the output, and an OVERLAY pass at order -200 (before the 2D canvas pass) that binds the output
// with raw pass code and draws a one-pixel checker (0 and 255) over the middle half of it.
// Checks, per backend:
//   - the pass saw the full output: ctx->width / height and ctx->output's size (debug field overlay.size) are the
//     captured frame's size;
//   - the checker is pixel exact: every pixel of the middle half is 0 or 255 and differs from its right and lower
//     neighbours (at least 99.9 % of the pairs; a drawing at the scene's size scaled up has none);
//   - the scene renders at 25 %: cw_render_size (debug field render.size) is a quarter of the frame each way;
//   - CONTROL, CW_TEST_OVERLAY_SCALED=1 (the overlays left on the scene's scaled target, as before 0.48): the pass
//     sees the scene's size and the checker check must fail;
//   - CONTROL, `scene_scale 0` (the scene given back to the Resolution setting, 100% in a test run): render.size is
//     the whole frame and the 25 % check must fail.
//   node tests/overlay-full-res.mjs [--gpu gl|webgpu] [--cart path]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'overlay-full-res');
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const arg = (k) => (argv.includes(`--${k}`) ? argv[argv.indexOf(`--${k}`) + 1] : null);
const gpus = arg('gpu') ? [arg('gpu')] : ['gl', 'webgpu'];
const cart = arg('cart');
const FRAMES = 20;

function run(gpu, name, { env, cmds } = {}) {
  const png = join(OUT, `${name}.${gpu}.png`), state = join(OUT, `${name}.${gpu}.json`);
  const a = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', 'romdev', '--app', 'overlay_full_res', '--frames', String(FRAMES),
    '--gpu', gpu, '--shot', png, '--state', state];
  if (env) a.push('--env', JSON.stringify(env));
  if (cmds) a.push('--start-cmds', cmds.join(';'));
  if (cart) a.push('--cart', cart);
  const r = spawnSync('node', a, { encoding: 'utf8', maxBuffer: 64 << 20 });
  if (r.status !== 0) throw new Error(`${gpu} ${name}: ${(r.stderr || r.stdout || '').trim().split('\n').slice(-3).join(' | ')}`);
  const img = readImage(png), st = JSON.parse(readFileSync(state, 'utf8'));
  rmSync(png, { force: true });
  rmSync(state, { force: true });
  const field = (n) => st[n] ?? (st.fields || []).find((f) => f.name === n)?.values ?? (st.fields || []).find((f) => f.name === n)?.value;
  return { img, overlay: field('overlay.size'), render: field('render.size') };
}

/** the middle half's checker: pixels that are pure 0 / 255 grey, and neighbour pairs that alternate */
function checker(img) {
  const { w, h, rgba } = img, x0 = Math.floor(w / 4), y0 = Math.floor(h / 4), x1 = w - x0, y1 = h - y0;
  const v = (x, y) => { const i = 4 * (y * w + x); return rgba[i] === rgba[i + 1] && rgba[i] === rgba[i + 2] ? rgba[i] : -1; };
  let pure = 0, n = 0, alt = 0, pairs = 0;
  for (let y = y0; y < y1; y++)
    for (let x = x0; x < x1; x++) {
      const c = v(x, y);
      n++;
      if (c === 0 || c === 255) pure++;
      if (x + 1 < x1) { pairs++; if ((c === 0 || c === 255) && v(x + 1, y) === 255 - c) alt++; }
      if (y + 1 < y1) { pairs++; if ((c === 0 || c === 255) && v(x, y + 1) === 255 - c) alt++; }
    }
  return { pure: pure / n, alt: alt / pairs };
}

const MIN_ALT = 0.999;
let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const pct = (x) => `${(x * 100).toFixed(2)} %`;
for (const gpu of gpus) {
  const m = run(gpu, 'full'), old = run(gpu, 'scaled', { env: { CW_TEST_OVERLAY_SCALED: '1' } });
  const back = run(gpu, 'back', { cmds: ['scene_scale 0'] });
  const { w, h } = m.img, qw = Math.round(w * 0.25), qh = Math.round(h * 0.25);
  const sizeOk = (o) => Array.isArray(o) && o[0] === w && o[1] === h && o[2] === w && o[3] === h;
  const quarter = (r) => Array.isArray(r) && r[0] === qw && r[1] === qh;
  const c = checker(m.img), co = checker(old.img);
  check(sizeOk(m.overlay), `${gpu}: the overlay pass saw the full output: ctx->width x height, ctx->output ${JSON.stringify(m.overlay)}, the frame ${w}x${h}`);
  check(c.alt >= MIN_ALT, `${gpu}: the one-pixel checker is pixel exact: ${pct(c.alt)} of the neighbour pairs alternate 0 / 255 (at least ${pct(MIN_ALT)}), ${pct(c.pure)} of its pixels pure`);
  check(quarter(m.render), `${gpu}: cw_quality_scene_scale(0.25): cw_render_size ${JSON.stringify(m.render)}, a quarter of ${w}x${h} is ${qw}x${qh}`);
  check(!sizeOk(old.overlay) && co.alt < MIN_ALT, `${gpu}: CONTROL, CW_TEST_OVERLAY_SCALED=1: the pass saw ${JSON.stringify(old.overlay)} and ${pct(co.alt)} of the pairs alternate (the check fails)`);
  check(!quarter(back.render) && Array.isArray(back.render) && back.render[0] === w && back.render[1] === h, `${gpu}: CONTROL, scene_scale 0: cw_render_size ${JSON.stringify(back.render)} is the Resolution setting's again (the 25 % check fails)`);
}
console.log(fail ? `overlay-full-res: ${fail} failures` : 'ok   overlay-full-res');
process.exit(fail ? 1 : 0);
