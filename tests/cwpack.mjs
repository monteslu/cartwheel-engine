// cwpack's own gate (a native tool, so native only):
//   - the import cache: a changed source, a new source and a changed setting
//     each reimport, and an unchanged one does not; a deleted referenced
//     file fails the pack naming the file and who referenced it;
//   - determinism: two packs of the same project with separate (cold)
//     caches are byte-identical (.cwpak, every asset, res.index, manifest);
//   - the packed set is only what the roots reach.
// Each check has a control that must fail, so a check that cannot see
// anything is caught.
//
//   node tests/cwpack.mjs      (after scripts/build.sh)
import { spawnSync } from 'node:child_process';
import { cpSync, mkdirSync, readFileSync, rmSync, writeFileSync, readdirSync, statSync, existsSync, appendFileSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
import { testEnv } from '../tools/lib/wgpu-pin.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)
testEnv(process.env); // what it starts renders offscreen and cannot reach the desktop

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const CWPACK = join(ROOT, 'build/native/tools/cwpack/cwpack');
const W = join(ROOT, 'out/cwpack-test');
rmSync(W, { recursive: true, force: true });
mkdirSync(W, { recursive: true });
let failed = 0;
const check = (name, ok, detail = '') => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok || !detail ? '' : ': ' + detail}`);
  if (!ok) failed++;
};

// a project: the resource suite's generated assets plus a glTF with an external image
const P = join(W, 'project');
cpSync(join(ROOT, 'tests/unit/res_project'), P, { recursive: true });
rmSync(join(P, '.cwcache'), { recursive: true, force: true });
writeFileSync(join(P, 'unreferenced.txt'), 'nobody asks for me\n');
writeFileSync(join(P, 'roots.txt'), [
  'set mesh.compress true',
  'models/quad.gltf', // reaches textures/checker.png through its image uri
  'textures/small.png',
  'text/hello.txt',
  '',
].join('\n'));

function pack(out, extra = [], cache = join(P, '.cwcache')) {
  const stats = join(W, `${out}.stats.json`);
  const r = spawnSync(CWPACK, ['--manifest', join(P, 'roots.txt'), '--out', join(W, out), '--pak', join(W, out, 'game.cwpak'),
    '--cache', cache, '--stats', stats, '-q', ...extra], { encoding: 'utf8' });
  return { code: r.status, err: r.stderr, stats: r.status === 0 ? JSON.parse(readFileSync(stats, 'utf8')) : null };
}
const imported = (r) => (r.stats ? r.stats.imported.slice().sort() : []);
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

// 1. cold: everything imported
let r = pack('a');
check('cold pack imports every importable file', r.code === 0 && same(imported(r), ['models/quad.gltf', 'textures/checker.png', 'textures/small.png']), r.err || JSON.stringify(r.stats));
const files = (dir) => readdirSync(dir, { recursive: true }).filter((f) => statSync(join(dir, f)).isFile()).sort();
check('only referenced files are packed', same(files(join(W, 'a/assets')), ['models/quad.gltf', 'res.index', 'text/hello.txt', 'textures/checker.png', 'textures/small.png']),
  files(join(W, 'a/assets')).join(' '));
check('control: the unreferenced file exists in the project', existsSync(join(P, 'unreferenced.txt')));

// 2. unchanged: nothing imported
r = pack('b');
check('unchanged rerun imports nothing', r.code === 0 && imported(r).length === 0 && r.stats.cached.length === 3, JSON.stringify(r.stats));

// 3. a changed source reimports that file only
const small = join(P, 'textures/small.png');
const orig = readFileSync(small);
writeFileSync(small, Buffer.concat([orig, Buffer.from('trailing bytes change the file, not the image')]));
r = pack('c');
check('changed source reimports it (and only it)', same(imported(r), ['textures/small.png']), JSON.stringify(r.stats));
writeFileSync(small, orig);

// 4. a changed external input (the .gltf's .bin) reimports the glTF
const bin = join(P, 'models/quad.bin');
const ob = readFileSync(bin);
const nb = Buffer.from(ob);
nb.writeFloatLE(nb.readFloatLE(4) + 0.001, 4);
writeFileSync(bin, nb);
r = pack('d');
check('changed buffer reimports the glTF', same(imported(r), ['models/quad.gltf']), JSON.stringify(r.stats));
writeFileSync(bin, ob);

// 5. a new source is imported
appendFileSync(join(P, 'roots.txt'), 'textures/checker_k.png\n');
r = pack('e');
check('new source is imported', same(imported(r), ['textures/checker_k.png']), JSON.stringify(r.stats));

// 6. a changed setting reimports what reads it, nothing else
r = pack('f', ['--set', 'mesh.lods=0.3']);
check('changed mesh setting reimports the glTF only', same(imported(r), ['models/quad.gltf']), JSON.stringify(r.stats));
writeFileSync(join(P, 'textures/small.png.cwimport'), 'texture.format = ktx2\n');
r = pack('g');
check('changed sidecar setting reimports that texture only', same(imported(r), ['textures/small.png']), JSON.stringify(r.stats));
rmSync(join(P, 'textures/small.png.cwimport'));
// control: a setting nobody reads changes nothing
r = pack('h', ['--set', 'unrelated.key=1']);
check('control: an unrelated setting reimports nothing', imported(r).length === 0, JSON.stringify(r.stats));

// 7. a deleted referenced file errors clearly
const ck = join(P, 'textures/checker.png');
const ckb = readFileSync(ck);
rmSync(ck);
r = pack('i');
check('deleted referenced file fails the pack, naming it and its referrer',
  r.code === 1 && /res:\/\/textures\/checker\.png/.test(r.err) && /referenced by res:\/\/models\/quad\.gltf/.test(r.err), r.err);
writeFileSync(ck, ckb);

// 8. determinism: two cold packs, separate caches
const c1 = pack('det1', [], join(W, 'cache1')), c2 = pack('det2', [], join(W, 'cache2'));
check('cold packs with separate caches both import', imported(c1).length === 4 && imported(c2).length === 4, JSON.stringify([c1.stats, c2.stats]));
const cmp = (rel) => readFileSync(join(W, 'det1', rel)).equals(readFileSync(join(W, 'det2', rel)));
const all = ['game.cwpak', 'manifest.json', ...files(join(W, 'det1/assets')).map((f) => `assets/${f}`)];
const diff = all.filter((f) => !cmp(f));
check(`identical input, byte-identical output (${all.length} files)`, diff.length === 0, diff.join(' '));
const c3 = pack('det3', ['--set', 'mesh.lods=0.4'], join(W, 'cache3'));
check('control: a different setting gives a different pack',
  !readFileSync(join(W, 'det1/game.cwpak')).equals(readFileSync(join(W, 'det3/game.cwpak'))), c3.err);

// 9. res.index types match the engine's loader table for every audio
// container and for the key = value / scene text files
const T = join(W, 'types');
mkdirSync(T, { recursive: true });
const typed = { 'a.opus': 'OpusHead', 'b.xm': 'xm', 'c.ogg': 'OggS', 'p/project.cwproj': 'substeps = 4\n', 'p/level.cwscene': '[scene format=1]\n', 'd.json': '{}' };
for (const [k, v] of Object.entries(typed)) { mkdirSync(dirname(join(T, k)), { recursive: true }); writeFileSync(join(T, k), v); }
writeFileSync(join(T, 'roots.txt'), Object.keys(typed).join('\n') + '\n');
const tr = spawnSync(CWPACK, ['--manifest', join(T, 'roots.txt'), '--out', join(W, 'types-out'), '--cache', join(W, 'types-cache'), '-q'], { encoding: 'utf8' });
const index = tr.status === 0 ? readFileSync(join(W, 'types-out/assets/res.index'), 'utf8') : '';
const typeOf = (p) => (index.split('\n').find((l) => l.endsWith(' ' + p)) || '').split(' ')[1];
const want = { 'a.opus': 'audio_clip', 'b.xm': 'audio_clip', 'c.ogg': 'audio_clip', 'p/project.cwproj': 'text', 'p/level.cwscene': 'text', 'd.json': 'json' };
const wrong = Object.entries(want).filter(([p, t]) => typeOf(p) !== t).map(([p, t]) => `${p}: ${typeOf(p)} (want ${t})`);
check('res.index types: .opus/.xm audio clips, .cwproj/.cwscene text', tr.status === 0 && !wrong.length, tr.stderr + wrong.join('; '));
check('control: a type lookup sees a mismatch', typeOf('d.json') !== 'text');

// 9. scenes are roots: ext_scene paths and res(0x<uid>) values pull in what
//    they use (a sub-resource UID pulls in its file), nothing else
const fnv = (str) => { let h = 0xcbf29ce484222325n; for (const c of Buffer.from(str)) { h ^= BigInt(c); h = (h * 0x100000001b3n) & 0xffffffffffffffffn; } return h.toString(16).padStart(16, '0'); };
const S = join(W, 'scenes');
mkdirSync(join(S, 'levels'), { recursive: true });
mkdirSync(join(S, 'props'), { recursive: true });
mkdirSync(join(S, 'models'), { recursive: true });
for (const [d, sname] of [['box.glb', 'BoxTextured.glb'], ['man.glb', 'CesiumMan.glb'], ['unused.glb', 'AnimatedMorphCube.glb']])
  cpSync(join(ROOT, 'deps/three.c/test/assets', sname), join(S, 'models', d));
writeFileSync(join(S, 'levels/main.cwscene'), `[scene format=1]\n[ext_scene id=1 path="props/crate.cwscene"]\n\n[node name="Level" type="Node3D"]\n\n[node name="Box" parent="." type="MeshInstance3D"]\nmesh = res(0x${fnv('models/box.glb#mesh/0/0')})\n\n[node name="C" parent="." instance=1]\n`);
writeFileSync(join(S, 'props/crate.cwscene'), `[scene format=1]\n\n[node name="Crate" type="MeshInstance3D"]\nmesh = res(0x${fnv('models/man.glb')})\n`);
writeFileSync(join(S, 'roots.txt'), 'levels/main.cwscene\n');
const sp = spawnSync(CWPACK, ['--manifest', join(S, 'roots.txt'), '--out', join(W, 'scenes-out'), '-q'], { encoding: 'utf8' });
check('a scene root packs its sub-scenes and the files its UIDs name, nothing else',
  sp.status === 0 && same(files(join(W, 'scenes-out/assets')), ['levels/main.cwscene', 'levels/main.cwscene.preload', 'models/box.glb', 'models/man.glb', 'props/crate.cwscene', 'props/crate.cwscene.preload', 'res.index']),
  sp.stderr + files(join(W, 'scenes-out/assets')).join(' '));
check('a scene\'s preload list: dependencies first, the scene last',
  readFileSync(join(W, 'scenes-out/assets/levels/main.cwscene.preload'), 'utf8').split('\n').filter((l) => l && l[0] !== '#').join(' ') === 'models/man.glb props/crate.cwscene models/box.glb levels/main.cwscene');
check('UIDs are standard FNV-1a 64 of the path (any tool can compute one)',
  readFileSync(join(W, 'scenes-out/assets/res.index'), 'utf8').includes(`${fnv('models/box.glb#mesh/0/0')} mesh models/box.glb#mesh/0/0`));
writeFileSync(join(S, 'props/crate.cwscene'), '[scene format=1]\n\n[node name="Crate" type="MeshInstance3D"]\nmesh = res(0x0123456789abcdef)\n');
const bad = spawnSync(CWPACK, ['--manifest', join(S, 'roots.txt'), '--out', join(W, 'scenes-bad'), '-q'], { encoding: 'utf8' });
check('control: a UID no file has fails the pack, naming the scene and line',
  bad.status === 1 && /res:\/\/props\/crate\.cwscene:4: res\(0x0123456789abcdef\) matches no file/.test(bad.stderr), bad.stderr);

// @engine/ source paths: engine-provided files (deps/fonts) from a project at any depth
const E = join(W, 'deep/a/b/engine-proj');
mkdirSync(E, { recursive: true });
writeFileSync(join(E, 'roots.txt'), 'fonts/license.txt = @engine/deps/fonts/noto-sans-jp-OFL.txt\n');
const er = spawnSync(CWPACK, ['--manifest', join(E, 'roots.txt'), '--out', join(W, 'engine-out'), '--engine-root', ROOT, '-q'], { encoding: 'utf8' });
check('@engine/ resolves against --engine-root',
  er.status === 0 && same(readFileSync(join(W, 'engine-out/assets/fonts/license.txt')), readFileSync(join(ROOT, 'deps/fonts/noto-sans-jp-OFL.txt'))), er.stderr);
const en = spawnSync(CWPACK, ['--manifest', join(E, 'roots.txt'), '--out', join(W, 'engine-bad'), '-q'], { encoding: 'utf8' });
check('control: @engine/ without --engine-root fails, naming the line', en.status === 1 && /roots\.txt:1: @engine\/deps\/fonts\/noto-sans-jp-OFL\.txt needs --engine-root/.test(en.stderr), en.stderr);

console.log(failed ? `${failed} FAILED` : 'cwpack: all checks passed');
process.exit(failed ? 1 : 0);
