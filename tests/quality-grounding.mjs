// Nothing floats on Very Low (cartwheel/quality.h: Shadows Minimal keeps the sun's shadow, small casters culled):
// in samples with props, every shadow caster big enough to read on screen (radius over distance >= --min-size) that
// has a shadow at its base on High still has one on Very Low. Each sample runs natively on High and on Very Low, each
// twice: with its casters' bases on screen (CW_SHADOW_CONTACTS=1, the debug command shadow_contacts) and with no
// light casting (CW_TEST_NO_SHADOWS=1). Around each base (a window a twentieth of the frame high) a shadow is there
// when at least 4% of the window's pixels are 12% darker than in the shadowless frame. A base the caster itself
// hides, or one in the air, shows no shadow on High and does not count; a caster Very Low culled counts as none.
// An app passes when Very Low keeps at least --share of the shadows High draws. Control: Very Low with no light
// casting must fail.
//   node tests/quality-grounding.mjs [--apps open_world:200,platformer3d_auto:400] [--gpu gl] [--min-size 0.025] [--share 0.9] [--project <games tree>]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { readImage } from '../tools/lib/png.mjs';
import { wgpuPin, glPin, scrubDisplay } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes('--' + k) ? argv[argv.indexOf('--' + k) + 1] : d);
const PROJ = resolve(opt('project', ROOT));
const OUT = join(PROJ, 'out', 'quality-grounding');
mkdirSync(OUT, { recursive: true });
const MIN = +opt('min-size', '0.025'), SHARE = +opt('share', '0.9'), GPU = opt('gpu', 'gl'), PRESET = opt('preset', 'verylow');
const APPS = opt('apps', 'open_world:200,platformer3d_auto:400').split(',').map((a) => { const [app, f] = a.split(':'); return { app, frames: f || '300' }; });
const exeOf = (app) => {
  const j = join(PROJ, 'build/native/apps', `${app}.json`);
  if (existsSync(j)) return JSON.parse(readFileSync(j, 'utf8')).binary;
  return join(PROJ, 'build/native/samples', app);
};
function run(a, tag, extra) {
  const exe = exeOf(a.app), shot = join(OUT, `${a.app}-${tag}.png`);
  const env = { ...process.env, SDL_AUDIODRIVER: 'dummy', CW_FRAMES: a.frames, CW_QUALITY: extra.CW_QUALITY || PRESET, CW_SHADOW_CONTACTS: '1', CW_DEBUG_CMDS: 'shadow_contacts',
    CW_SCREENSHOT: shot, CW_ASSETS: exe + '.assets', CW_PROJECT_ROOT: PROJ, CW_HOST_PREF: '1920x1080', ...extra };
  if (GPU === 'webgpu') Object.assign(env, wgpuPin().env, { CW_GPU: 'webgpu' });
  else { env.CW_GPU = 'gl'; const g = glPin(); if (g) Object.assign(env, g.env); }
  scrubDisplay(env);
  const r = spawnSync(exe, [], { env, encoding: 'utf8', maxBuffer: 1 << 26, timeout: 600e3 });
  const line = (r.stdout || '').split('\n').find((l) => l.startsWith('{"shadow_contacts"'));
  if (r.status !== 0 || !line) throw new Error(`${a.app} ${tag}: exit ${r.status}: ${(r.stderr || '').slice(-400)}`);
  return { contacts: JSON.parse(line).shadow_contacts, img: readImage(shot) };
}
const lum = (img, x, y) => { const i = 4 * (y * img.w + x); return 0.2126 * img.rgba[i] + 0.7152 * img.rgba[i + 1] + 0.0722 * img.rgba[i + 2]; };
/** the share of a window around (x, y) where a is 12% darker than b */
function darker(a, b, cx, cy) {
  const r = Math.max(4, Math.round(a.h / 40));
  let n = 0, d = 0;
  for (let y = Math.max(0, cy - r); y < Math.min(a.h, cy + r); y++)
    for (let x = Math.max(0, cx - r); x < Math.min(a.w, cx + r); x++) {
      n++;
      const la = lum(a, x, y), lb = lum(b, x, y);
      if (lb > 8 && la < lb * 0.88) d++;
    }
  return n ? d / n : 0;
}
/** the bases High shades, and which of them the subject (Very Low) shades too */
function judge(high, highRef, subject, ref) {
  const at = (c, img) => [Math.round(c.x * img.w), Math.round(c.y * img.h)];
  const list = high.contacts.map((c, i) => ({ c, v: subject.contacts[i] }))
    .filter(({ c }) => c.front && c.size >= MIN && c.x > 0.02 && c.x < 0.98 && c.y > 0.02 && c.y < 0.98)
    .filter(({ c }) => darker(high.img, highRef.img, ...at(c, high.img)) >= 0.04);
  let shadowed = 0;
  const missing = [];
  for (const { c, v } of list) {
    const [px, py] = at(c, subject.img);
    const culled = v && v.culled;
    if (!culled && darker(subject.img, ref.img, px, py) >= 0.04) shadowed++;
    else missing.push(`(${px},${py}) size ${c.size.toFixed(3)}${culled ? ' culled' : ''}`);
  }
  return { n: list.length, shadowed, missing };
}
let fail = 0;
for (const a of APPS) {
  const high = run(a, 'high', { CW_QUALITY: 'high' }), highRef = run(a, 'high-noshadows', { CW_QUALITY: 'high', CW_TEST_NO_SHADOWS: '1' });
  const subject = run(a, PRESET, {}), ref = run(a, 'noshadows', { CW_TEST_NO_SHADOWS: '1' });
  if (high.contacts.length !== subject.contacts.length) { fail++; console.log(`FAIL ${a.app}: ${high.contacts.length} casters on High, ${subject.contacts.length} on ${PRESET} (not the same frame)`); continue; }
  const j = judge(high, highRef, subject, ref);
  const share = j.n ? j.shadowed / j.n : 0;
  const ok = j.n >= 2 && share >= SHARE;
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${a.app} (${PRESET}, ${GPU}): ${j.shadowed} of the ${j.n} shadows High draws at a caster's base are there (${(share * 100).toFixed(0)}%, at least ${SHARE * 100}%)${j.missing.length ? '\n       none: ' + j.missing.slice(0, 12).join(', ') : ''}`);
  // the control: the frame with no shadows as the subject
  const c = judge(high, highRef, { contacts: subject.contacts, img: ref.img }, ref);
  if (c.n && c.shadowed / c.n >= SHARE) { fail++; console.log(`CONTROL PASSED (${a.app}): a frame with no shadows passed`); }
  else console.log(`ok   control (${a.app}): no shadows, ${c.shadowed} of ${c.n} pass`);
}
console.log(fail ? `quality-grounding: ${fail} failures` : 'ok   quality-grounding');
process.exit(fail ? 1 : 0);
