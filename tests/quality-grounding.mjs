// Nothing floats on Very Low (cartwheel/quality.h: Shadows Minimal keeps the sun's shadow, small casters culled):
// in samples with props, every shadow caster big enough to read on screen (radius over distance >= --min-size) that
// has a shadow at its base on High still has one on Very Low. Each sample runs natively on High and on Very Low, each
// twice: with its casters' bases on screen (CW_SHADOW_CONTACTS=1, the debug command shadow_contacts) and with no
// light casting (CW_TEST_NO_SHADOWS=1). Around each base (a window a twentieth of the frame high) a shadow is there
// when at least 4% of the window's pixels are 12% darker than in the shadowless frame. A base the caster itself
// hides, or one in the air, shows no shadow on High and does not count; a caster Very Low culled counts as none.
// The base of a long caster (a wall, a fence, a row of stands: its footprint over 6 times as long as it is wide and
// as it is tall) is the point of its base line nearest the camera, and a caster whose box stands around the camera
// (a ring of walls, a floor: the camera is above its footprint) has no single base and is not judged; under the
// middle of their boxes nothing of them stands (--centre judges every caster there, the old rule; tests/quality-grounding-judge.mjs shows
// that rule wrong). The judge is tools/lib/grounding.mjs.
// An app passes when Very Low keeps at least --share of the shadows High draws. Control: Very Low with no light
// casting must fail.
//   node tests/quality-grounding.mjs [--apps open_world:200,platformer3d_auto:400] [--gpu gl] [--preset verylow] [--min-size 0.025] [--share 0.9]
//        [--centre] [--project <games tree>]
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { judge } from '../tools/lib/grounding.mjs';
import { readImage } from '../tools/lib/png.mjs';
import { wgpuPin, glPin, scrubDisplay } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const opt = (k, d) => (argv.includes('--' + k) ? argv[argv.indexOf('--' + k) + 1] : d);
const PROJ = resolve(opt('project', ROOT));
const OUT = join(PROJ, 'out', 'quality-grounding');
mkdirSync(OUT, { recursive: true });
const CENTRE = argv.includes('--centre');
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
let fail = 0;
for (const a of APPS) {
  const high = run(a, 'high', { CW_QUALITY: 'high' }), highRef = run(a, 'high-noshadows', { CW_QUALITY: 'high', CW_TEST_NO_SHADOWS: '1' });
  const subject = run(a, PRESET, {}), ref = run(a, 'noshadows', { CW_TEST_NO_SHADOWS: '1' });
  if (high.contacts.length !== subject.contacts.length) { fail++; console.log(`FAIL ${a.app}: ${high.contacts.length} casters on High, ${subject.contacts.length} on ${PRESET} (not the same frame)`); continue; }
  const J = { minSize: MIN, centre: CENTRE };
  const j = judge(high, highRef, subject, ref, J);
  const share = j.n ? j.shadowed / j.n : 0;
  const ok = j.n >= 2 && share >= SHARE;
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${a.app} (${PRESET}, ${GPU}): ${j.shadowed} of the ${j.n} shadows High draws at a caster's base are there (${(share * 100).toFixed(0)}%, at least ${SHARE * 100}%)${j.around && !CENTRE ? `; ${j.around} around the camera not judged` : ''}${j.missing.length ? '\n       none: ' + j.missing.slice(0, 12).join(', ') : ''}`);
  // the control: the frame with no shadows as the subject
  const c = judge(high, highRef, { contacts: subject.contacts, img: ref.img }, ref, J);
  if (c.n && c.shadowed / c.n >= SHARE) { fail++; console.log(`CONTROL PASSED (${a.app}): a frame with no shadows passed`); }
  else console.log(`ok   control (${a.app}): no shadows, ${c.shadowed} of ${c.n} pass`);
}
console.log(fail ? `quality-grounding: ${fail} failures` : 'ok   quality-grounding');
process.exit(fail ? 1 : 0);
