// The start angle of GPUParticles3D and GPUParticles2D (`angle`, `angle_random`, cartwheel/particles.h), read from the
// picture: samples/fx_particle_angle draws upright arrows (a red head over a green shaft), eight GPUParticles3D billboards
// in the middle of the screen while the camera circles and bobs, eight GPUParticles2D sprites along the bottom. Each arrow
// is found as a blob and its turn measured from its shaft's centre to its head's (counter-clockwise on screen, 0 = up).
// Per host and backend:
//   - angle_random 0 (the scene's): every arrow upright, at three frames of the camera's circle; CONTROL: angle_random 1
//     (the default) must fail it, in 3D and in 2D;
//   - angle 0.7: every 3D arrow at 0.7 counter-clockwise, every 2D arrow at 0.7 clockwise (y down, as Node2D rotation);
//   - spin still adds: spin 1.5 with angle_random 0 turns every arrow by the same amount, some each way (CONTROL: the
//     spin-free run turns none);
//   - STRETCHED ignores the start angle: samples/fx_particles with its stretched Sparks at angle 1.3 draws the default
//     picture pixel for pixel (CONTROL: the billboard Fountain at angle 1.3 changes it).
//   node tests/particle-angle.mjs [--hosts native,romdev] [--gpu gl,webgpu]   (romdev runs the dual cart, build/cart-wgpu)
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage } from '../tools/lib/png.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'particle-angle');
mkdirSync(OUT, { recursive: true });
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
// (CI, CW_TEST_GPU=software, has no romdev: native alone there)
const HOSTS = arg('--hosts', process.env.CW_TEST_GPU === 'software' ? 'native' : 'native,romdev').split(',');
const GPUS = arg('--gpu', 'gl,webgpu').split(',');
if (HOSTS.includes('native')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv } = await import('../tools/lib/wgpu-pin.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
}

function run(host, gpu, app, name, frames, { env, cmds } = {}) {
  const png = join(OUT, `${name}.${host}.${gpu}.png`), st = join(OUT, `${name}.${host}.${gpu}.json`);
  const a = [join(ROOT, 'tools', 'run-cart.mjs'), '--host', host, '--app', app, '--frames', String(frames), '--gpu', gpu, '--shot', png, '--state', st];
  if (env) a.push('--env', JSON.stringify(env));
  if (cmds) a.push('--start-cmds', cmds.join(';'));
  const r = spawnSync('node', a, { encoding: 'utf8', env: process.env });
  if (r.status !== 0) throw new Error(`${host} ${gpu} ${name}: ${((r.stderr || '') + (r.stdout || '')).trim().split('\n').slice(-3).join(' | ')}`);
  return readImage(png);
}

// the arrows of a picture: blobs of red or green pixels; per blob its turn from the shaft's centre to the head's
// (radians, counter-clockwise on screen, 0 = up), split at five sixths of the height (y 300 of the app's 360) into the
// 3D arrows (above) and the 2D ones
function arrows(img) {
  const { w, h, rgba } = img, SPLIT = (h * 5) / 6;
  const cls = new Uint8Array(w * h); // 1 red (head), 2 green (shaft)
  for (let i = 0; i < w * h; i++) {
    const r = rgba[4 * i], g = rgba[4 * i + 1], b = rgba[4 * i + 2];
    // by hue, not level: the 3D arrows are tone mapped (paler) and their edges blend into the background
    if (r > g + 40 && r > b + 40) cls[i] = 1;
    else if (g > r + 25 && g > b + 20) cls[i] = 2;
  }
  const seen = new Uint8Array(w * h), blobs = [];
  for (let s = 0; s < w * h; s++) {
    if (!cls[s] || seen[s]) continue;
    const st = [s]; seen[s] = 1;
    const a = [[0, 0, 0], [0, 0, 0]]; // per class: n, sum x, sum y
    while (st.length) {
      const p = st.pop(), x = p % w, y = (p / w) | 0, c = a[cls[p] - 1];
      c[0]++; c[1] += x; c[2] += y;
      for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
        const nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        const q = ny * w + nx;
        if (cls[q] && !seen[q]) { seen[q] = 1; st.push(q); }
      }
    }
    if (a[0][0] < (w * h) / 20000 || a[1][0] < (w * h) / 20000) continue; // a sliver at the edge of the screen or a lone pixel
    const R = [a[0][1] / a[0][0], a[0][2] / a[0][0]], G = [a[1][1] / a[1][0], a[1][2] / a[1][0]];
    blobs.push({ n: a[0][0] + a[1][0], q: a[0][0] / a[1][0], y: (R[1] + G[1]) / 2, x: (R[0] + G[0]) / 2, ang: Math.atan2(-(R[0] - G[0]), -(R[1] - G[1])) });
  }
  // two arrows that overlap make one blob with more pixels, or a head to shaft ratio no single arrow has (the ratio
  // holds at any size): leave those out
  const med = (l, k) => { const s = l.map((b) => b[k]).sort((p, q) => p - q); return s[s.length >> 1] || 0; };
  const keep = (l) => { const m = med(l, 'n'), q = med(l, 'q'); return l.filter((b) => b.n < m * 2.2 && Math.abs(b.q / q - 1) < 0.3); };
  return { p3: keep(blobs.filter((b) => b.y < SPLIT)), p2: keep(blobs.filter((b) => b.y >= SPLIT)) };
}
const wrap = (a) => { a = (a + Math.PI) % (2 * Math.PI); if (a < 0) a += 2 * Math.PI; return a - Math.PI; };
const TOL = 0.09; // radians (about 5 degrees): the centroids of a 26-pixel arrow, filtered
const MIN = 5; // arrows that must be found (of eight): overlaps and the screen's edges take a few at some frames
const fmt = (l) => l.map((b) => b.ang.toFixed(2)).join(' ');
// every arrow at `want`
const allAt = (l, want) => l.length >= MIN && l.every((b) => Math.abs(wrap(b.ang - want)) < TOL);

let fail = 0;
const check = (ok, msg) => { if (!ok) fail++; console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); };
const diff = (a, b) => { let n = 0; for (let i = 0; i < a.rgba.length; i += 4) if (a.rgba[i] !== b.rgba[i] || a.rgba[i + 1] !== b.rgba[i + 1] || a.rgba[i + 2] !== b.rgba[i + 2]) n++; return n; };

for (const host of HOSTS)
  for (const gpu of GPUS) {
    const tag = `${host} ${gpu}`;
    const step = (what, fn) => { try { fn(); } catch (e) { check(false, `${tag}: ${what}: ${e.message}`); } };
    // upright, as the camera moves
    for (const f of [40, 130, 220])
      step(`upright at frame ${f}`, () => {
        const a = arrows(run(host, gpu, 'fx_particle_angle', `upright-${f}`, f));
        check(allAt(a.p3, 0), `${tag}: frame ${f} (camera yaw ${(f * 0.02).toFixed(1)}): every 3D arrow upright with angle_random 0 (${a.p3.length} found: ${fmt(a.p3)})`);
        check(allAt(a.p2, 0), `${tag}: frame ${f}: every 2D arrow upright with angle_random 0 (${a.p2.length} found: ${fmt(a.p2)})`);
      });
    step('control: angle_random 1', () => {
      const a = arrows(run(host, gpu, 'fx_particle_angle', 'random-control', 130, { env: { PA_RANDOM: '1' } }));
      check(a.p3.length >= MIN && !allAt(a.p3, 0), `${tag}: CONTROL: angle_random 1 turns the 3D arrows every way (${a.p3.length} found: ${fmt(a.p3)})`);
      check(a.p2.length >= MIN && !allAt(a.p2, 0), `${tag}: CONTROL: angle_random 1 turns the 2D arrows every way (${a.p2.length} found: ${fmt(a.p2)})`);
    });
    step('angle 0.7', () => {
      const a = arrows(run(host, gpu, 'fx_particle_angle', 'angle', 130, { env: { PA_ANGLE: '0.7' } }));
      check(allAt(a.p3, 0.7), `${tag}: angle 0.7: every 3D arrow turned 0.7 counter-clockwise (${a.p3.length} found: ${fmt(a.p3)})`);
      check(allAt(a.p2, -0.7), `${tag}: angle 0.7: every 2D arrow turned 0.7 clockwise (${a.p2.length} found: ${fmt(a.p2)})`);
    });
    step('spin', () => {
      // the arrows spawn together (explosiveness 1) and turn at 1.5 rad/s, each way at random: by frame 60 about 0.9
      const a = arrows(run(host, gpu, 'fx_particle_angle', 'spin', 60, { env: { PA_SPIN: '1.5', PA_STILL: '1' } }));
      const still = arrows(run(host, gpu, 'fx_particle_angle', 'spin-control', 60, { env: { PA_STILL: '1' } }));
      for (const [k, d] of [['p3', '3D'], ['p2', '2D']]) {
        const l = a[k], m = l.length ? l.reduce((s, b) => s + Math.abs(b.ang), 0) / l.length : 0;
        const ok = l.length >= MIN && m > 0.4 && l.every((b) => Math.abs(Math.abs(b.ang) - m) < TOL) && l.some((b) => b.ang > 0) && l.some((b) => b.ang < 0);
        check(ok, `${tag}: spin adds to the start angle: every ${d} arrow turned by the same amount, some each way (${l.length} found: ${fmt(l)})`);
        const c = still[k], cm = c.length ? c.reduce((s, b) => s + Math.abs(b.ang), 0) / c.length : 0;
        check(c.length >= MIN && !(cm > 0.4), `${tag}: CONTROL: without spin the ${d} arrows stay upright (${fmt(c)})`);
      }
    });
    step('STRETCHED', () => {
      const base = run(host, gpu, 'fx_particles', 'stretched-base', 120);
      const st = run(host, gpu, 'fx_particles', 'stretched-angle', 120, { cmds: ['set World/Sparks angle 1.3', 'set World/Sparks angle_random 0'] });
      const ctl = run(host, gpu, 'fx_particles', 'stretched-control', 120, { cmds: ['set World/Fountain angle 1.3', 'set World/Fountain angle_random 0'] });
      const d = diff(base, st), c = diff(base, ctl);
      check(d === 0, `${tag}: STRETCHED sparks at angle 1.3, angle_random 0 draw the default picture (${d} pixels differ)`);
      check(c > 0, `${tag}: CONTROL: the billboard fountain at angle 1.3, angle_random 0 changes it (${c} pixels differ)`);
    });
  }
console.log(fail ? `particle-angle: ${fail} failures` : 'ok   particle-angle');
process.exit(fail ? 1 : 0);
