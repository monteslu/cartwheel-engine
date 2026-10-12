// The offline baker (tools/cwbake) against the runtime one:
//   1. cwbake bakes samples/lightmap_room twice: the two .cwlm files are the
//      same bytes (a reproducible bake);
//   2. their fingerprint equals the one lightmap_room's LightmapGI bakes at
//      runtime (lm.hash_*, native run): the tool and the engine bake alike;
//   3. lightmap_room loading the tool's file (LightmapGI.data) shows that
//      fingerprint, bakes nothing (lm.same 0) and renders the same frame as
//      the runtime bake (no pixel off by more than 2 levels);
//   4. control: --seed 2 gives a different fingerprint (a check that cannot
//      tell bakes apart would pass it).
import { spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync, copyFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'lightmap-tool');
mkdirSync(join(OUT, 'assets'), { recursive: true });
const tool = join(ROOT, 'build/native/cwbake'), app = join(ROOT, 'build/native/samples/lightmap_room');
const scene = join(ROOT, 'samples/lightmap_room/assets/main.cwscene');
let fail = 0;
const bake = (out, extra = []) => {
  const r = spawnSync(tool, [scene, out, ...extra], { encoding: 'utf8' });
  if (r.status !== 0) { fail++; console.log('FAIL cwbake: ' + (r.stderr || '').trim()); return null; }
  return JSON.parse(r.stdout.trim().split('\n').pop());
};
const runApp = (assets, tag) => {
  const state = join(OUT, `${tag}.json`), shot = join(OUT, `${tag}.png`);
  const env = { ...process.env, CW_FRAMES: '30', CW_DEBUG_DUMP: state, CW_SCREENSHOT: shot };
  if (assets) env.CW_ASSETS = assets;
  const r = spawnSync(app, [], { env, encoding: 'utf8', cwd: dirname(app) });
  if (r.status !== 0) { fail++; console.log(`FAIL ${tag} run: ${r.stderr.trim().split('\n').slice(-2).join(' | ')}`); return null; }
  return { state: JSON.parse(readFileSync(state, 'utf8')), shot };
};
const hex = (s) => (BigInt(s['lm.hash_hi']) << 32n | BigInt(s['lm.hash_lo'])).toString(16).padStart(16, '0');

const a = bake(join(OUT, 'a.cwlm')), b = bake(join(OUT, 'b.cwlm'));
if (a && b) {
  const same = Buffer.compare(readFileSync(join(OUT, 'a.cwlm')), readFileSync(join(OUT, 'b.cwlm'))) === 0;
  if (!same || a.hash !== b.hash) { fail++; console.log(`FAIL two bakes differ: ${a.hash} vs ${b.hash}`); }
  else console.log(`ok   two cwbake runs: identical bytes, ${a.hash} (${a.texels} texels, ${a.probes} probes, ${a.rays} rays)`);
}
const runtime = runApp(null, 'runtime');
if (a && runtime) {
  if (hex(runtime.state) !== a.hash) { fail++; console.log(`FAIL runtime bake ${hex(runtime.state)} != cwbake ${a.hash}`); }
  else console.log('ok   the runtime LightmapGI bake has the same fingerprint');
}
// the app loading the tool's file
writeFileSync(join(OUT, 'assets', 'main.cwscene'),
  readFileSync(scene, 'utf8').replace('bake_twice = true', 'bake_twice = false\ndata = "room.cwlm"'));
if (a) copyFileSync(join(OUT, 'a.cwlm'), join(OUT, 'assets', 'room.cwlm'));
const loaded = runApp(join(OUT, 'assets'), 'loaded');
if (a && loaded && runtime) {
  if (hex(loaded.state) !== a.hash || loaded.state['lm.same'] !== 0 || loaded.state['lm.texels'] !== a.texels) {
    fail++;
    console.log(`FAIL loaded: hash ${hex(loaded.state)} same ${loaded.state['lm.same']} texels ${loaded.state['lm.texels']}`);
  } else {
    const d = diffImages(readImage(runtime.shot), readImage(loaded.shot));
    if (!d.comparable || d.gt2 > 0.0005) { fail++; console.log(`FAIL loaded frame differs: ${d.reason || (d.gt2 * 100).toFixed(3) + '% > 2'}`); }
    else console.log('ok   lightmap_room loading the .cwlm: same fingerprint, no bake, the same frame');
  }
}
const c = bake(join(OUT, 'c.cwlm'), ['--seed', '2']);
if (a && c) {
  if (c.hash === a.hash) { fail++; console.log('FAIL CONTROL PASSED: seed 2 gave the same fingerprint'); }
  else console.log(`ok   control: seed 2 bakes ${c.hash}`);
}
process.exit(fail ? 1 : 0);
