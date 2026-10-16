// The CSG bake tool (tools/cwcsg) and what a game sees of its output:
//   1. cwcsg bakes samples/csg_greybox twice: identical bytes, and the same
//      bytes as the committed level.cwcsg (cwcsg --check passes);
//   2. csg_greybox (native) reports that file's FNV-1a 64 (computed here
//      from the bytes), the triangle count in the file header, baked 1,
//      stale 0, and the ball resting on the baked floor (collision reads the
//      baked mesh);
//   3. controls that must fail:
//      - the Door changed from SUBTRACTION to UNION bakes other bytes, and
//        cwcsg --check of the committed file against that scene fails;
//      - the game given that scene but the old file reports stale 1;
//      - the game with that scene's own bake draws another frame (the door
//        is shut) and reports another file hash;
//      - the game without a bake file draws the preview (baked 0, another
//        frame) and the ball falls through the level (no baked collision).
import { spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync, copyFileSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out', 'csg-tool');
rmSync(OUT, { recursive: true, force: true });
const tool = join(ROOT, 'build/native/cwcsg'), app = join(ROOT, 'build/native/samples/csg_greybox');
const SRC = join(ROOT, 'samples/csg_greybox/assets');
const scene = readFileSync(join(SRC, 'main.cwscene'), 'utf8');
const committed = readFileSync(join(SRC, 'level.cwcsg'));
let fail = 0;
const ok = (m) => console.log('ok   ' + m);
const bad = (m) => { fail++; console.log('FAIL ' + m); };

const fnv = (buf) => {
  let h = 0xcbf29ce484222325n;
  for (const b of buf) h = ((h ^ BigInt(b)) * 0x100000001b3n) & 0xffffffffffffffffn;
  return h.toString(16).padStart(16, '0');
};
// an asset dir holding a scene (and optionally a bake file)
const assets = (tag, text, bake) => {
  const d = join(OUT, tag);
  mkdirSync(d, { recursive: true });
  writeFileSync(join(d, 'main.cwscene'), text);
  if (bake) copyFileSync(bake, join(d, 'level.cwcsg'));
  return d;
};
const cwcsg = (dir, extra = []) => {
  const r = spawnSync(tool, [join(dir, 'main.cwscene'), '--assets', dir, ...extra], { encoding: 'utf8' });
  const level = r.stdout.trim().split('\n').map((l) => JSON.parse(l)).find((j) => j.root === 'Level');
  return { status: r.status, level, err: r.stderr };
};
const runApp = (dir, tag) => {
  const state = join(OUT, `${tag}.json`), shot = join(OUT, `${tag}.png`);
  const env = { ...process.env, CW_FRAMES: '180', CW_DEBUG_DUMP: state, CW_SCREENSHOT: shot, CW_ASSETS: dir };
  const r = spawnSync(app, [], { env, encoding: 'utf8', cwd: dirname(app) });
  if (r.status !== 0) { bad(`${tag} run: ${r.stderr.trim().split('\n').slice(-2).join(' | ')}`); return null; }
  return { s: JSON.parse(readFileSync(state, 'utf8')), shot };
};
const hex = (s, k) => ((BigInt(s[k + '_hi']) << 32n) | BigInt(s[k + '_lo'])).toString(16).padStart(16, '0');
const differs = (a, b) => { const d = diffImages(readImage(a), readImage(b)); return d.comparable && d.gt16 > 0.002; };

// 1. reproducible, and the committed file is current
const A = assets('a', scene), B = assets('b', scene);
const a = cwcsg(A), b = cwcsg(B);
if (a.status || b.status || !a.level || !b.level) bad(`cwcsg failed: ${a.err}${b.err}`);
else {
  const ba = readFileSync(join(A, 'level.cwcsg')), bb = readFileSync(join(B, 'level.cwcsg'));
  if (Buffer.compare(ba, bb) || a.level.hash !== fnv(ba)) bad(`two bakes differ: ${a.level.hash} vs ${b.level.hash}`);
  else ok(`two cwcsg runs: identical bytes, ${a.level.hash} (${a.level.triangles} triangles, ${a.level.bytes} bytes, ${a.level.ms} ms)`);
  if (Buffer.compare(ba, committed)) bad('samples/csg_greybox/assets/level.cwcsg is not what the scene bakes: run cwcsg on it');
  else ok('the committed level.cwcsg is what the scene bakes');
}
const chk = cwcsg(SRC, ['--check']);
if (chk.status !== 0) bad('cwcsg --check on the sample failed');
else ok('cwcsg --check on the sample passes');

// 2. the game sees the file
const tris = committed.readUInt32LE(12) / 3;
const ref = runApp(SRC, 'ref');
if (ref) {
  const s = ref.s;
  if (hex(s, 'csg.file') !== fnv(committed) || s['csg.triangles'] !== tris || s['csg.baked'] !== 1 || s['csg.stale'] !== 0 ||
      s['csg.sketch_baked'] !== 0 || Math.abs(s['ball.y'] - 0.65) > 0.02)
    bad(`game state: file ${hex(s, 'csg.file')} (want ${fnv(committed)}) triangles ${s['csg.triangles']}/${tris} baked ${s['csg.baked']} stale ${s['csg.stale']} ball.y ${s['ball.y']}`);
  else ok(`csg_greybox loads it: file hash ${fnv(committed)}, mesh hash ${hex(s, 'csg.mesh')}, ${tris} triangles, ball at rest on the baked floor (y ${s['ball.y'].toFixed(3)})`);
}

// 3. controls
const shut = scene.replace(/(\[node name="Door"[^\n]*\]\noperation = )SUBTRACTION/, '$1UNION');
if (shut === scene) bad('control setup: the Door operation was not found');
const C = assets('shut-stale', shut, join(SRC, 'level.cwcsg'));
if (cwcsg(C, ['--check']).status === 0) bad('CONTROL PASSED: cwcsg --check accepted the old file for a changed scene');
else ok('control: cwcsg --check rejects the old file for the changed scene');
const stale = runApp(C, 'shut-stale');
if (stale && stale.s['csg.stale'] !== 1) bad('CONTROL PASSED: the game did not notice the stale bake');
else if (stale) ok('control: the game reports the old file as stale');
const D = assets('shut', shut);
const d = cwcsg(D);
if (!d.level || d.level.hash === a.level?.hash) bad('CONTROL PASSED: the shut door baked the same bytes');
else ok(`control: the shut door bakes ${d.level.hash} (${d.level.triangles} triangles)`);
const shutRun = runApp(D, 'shut');
if (ref && shutRun) {
  if (hex(shutRun.s, 'csg.file') === hex(ref.s, 'csg.file') || shutRun.s['csg.stale'] !== 0 || !differs(ref.shot, shutRun.shot))
    bad('CONTROL PASSED: the shut-door bake looks or hashes the same in the game');
  else ok('control: the shut-door bake draws another frame and hash');
}
const E = assets('preview', scene.replace(/bake_file = "level.cwcsg"\n/, ''));
const prev = runApp(E, 'preview');
if (ref && prev) {
  if (prev.s['csg.baked'] !== 0 || !differs(ref.shot, prev.shot) || !(prev.s['ball.y'] < 0))
    bad(`CONTROL PASSED: the preview run: baked ${prev.s['csg.baked']} ball.y ${prev.s['ball.y']}`);
  else ok(`control: without a bake the preview draws (another frame) and the ball falls through (y ${prev.s['ball.y'].toFixed(2)})`);
}
process.exit(fail ? 1 : 0);
