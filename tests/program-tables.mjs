// Program tables out of date with the extensions they were generated from, caught at build time and at run time.
//
// Build time (no GPU): every table in the tree is stamped with the extension sources its states use
// (tools/programs.mjs, tools/lib/program-sources.mjs) and its stamp matches them (cmake/program-stamp.cmake, the check
// cmake/programs.cmake runs at configure). Controls: the same table against a copy of the extensions with one helper
// changed (a shader changed after the table was made), against an SDK's program-sources.txt with one hash changed, and
// with its stamp removed (a table made by an engine before the check), each must fail with the regenerate command.
//
// Run time (native, GL and WebGPU): a value an extension hook sets that the draw's program lacks is reported
// (cw.three.dropped_values, which every gate fails on): water, terrain, clustered lights and their casters at each
// graphics preset report none; CW_TEST_EXT_MISS=1 (the water hook sets a value its program lacks) must report it.
// A Water3D whose program lacks the view rectangle (CW_TEST_WATER_STALE=1: a table made before split screen in one
// pass) draws the split screen view by view (sa.array 0), the per-view picture within 0.3%; the control
// (CW_TEST_WATER_STALE=2: the same program drawn in one pass) must miss the bound.
//   node tests/program-tables.mjs [--gpu gl,webgpu] [--no-run]
import { spawnSync } from 'node:child_process';
import { appendFileSync, cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { dirHash, engineExtensionDirs } from '../tools/lib/program-sources.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'out/gate/program-tables');
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
const argv = process.argv.slice(2);
const GPUS = (argv.includes('--gpu') ? argv[argv.indexOf('--gpu') + 1] : 'gl,webgpu').split(',');
let fail = 0;
const check = (what, ok, detail = '') => {
  if (!ok) fail++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${detail ? ': ' + detail : ''}`);
};
/** cmake/program-stamp.cmake on one manifest: { ok, text } */
const stampCheck = (root, manifest) => {
  const r = spawnSync('cmake', [`-DCW_ROOT=${root}`, `-DMANIFEST=${manifest}`, '-P', join(ROOT, 'cmake/program-stamp.cmake')], { encoding: 'utf8' });
  return { ok: r.status === 0, text: `${r.stdout}${r.stderr}`.replace(/\s+/g, ' ').trim() };
};

// ── build time ────────────────────────────────────────────────────────
const manifests = [];
const walk = (d) => {
  for (const e of readdirSync(d, { withFileTypes: true })) {
    if (e.isDirectory() && !['deps', 'build', 'out', 'node_modules', '.git'].includes(e.name)) walk(join(d, e.name));
    else if (e.isFile() && e.name.endsWith('.txt') && basename(d) === 'programs' && existsSync(join(d, e.name.replace(/\.txt$/, '_gl.c')))) manifests.push(join(d, e.name));
  }
};
walk(ROOT);
check('program tables found', manifests.length >= 10, `${manifests.length}`);
for (const m of manifests) {
  const r = stampCheck(ROOT, m);
  check(`${m.replace(ROOT + '/', '')} stamp matches its sources`, r.ok, r.ok ? '' : r.text.slice(0, 400));
}
// a table whose states use the water extension, against copies of the engine's extensions
const water = manifests.find((m) => readFileSync(m.replace(/\.txt$/, '_gl.c'), 'utf8').includes('cw-source engine src/water/extensions'));
check('a table using the water extension', !!water);
if (water) {
  const copy = join(OUT, 'engine');
  for (const d of engineExtensionDirs(ROOT)) cpSync(join(ROOT, d), join(copy, d), { recursive: true });
  const t = join(OUT, 'table');
  mkdirSync(t, { recursive: true });
  const name = basename(water, '.txt');
  for (const f of [`${name}.txt`, `${name}_gl.c`]) cpSync(join(dirname(water), f), join(t, f));
  const tm = join(t, `${name}.txt`);
  let r = stampCheck(copy, tm);
  check('the copy: the stamp matches unchanged sources', r.ok, r.text.slice(0, 300));
  appendFileSync(join(copy, 'src/water/extensions/water_shading.mjs'), '\n// changed after the table was generated\n');
  r = stampCheck(copy, tm);
  check('control: a water helper changed after the table was made fails', !r.ok && /Regenerate them all/.test(r.text) && r.text.includes('src/water/extensions'), r.text.slice(0, 300));
  // an SDK: no extension modules, their hashes in cmake/program-sources.txt
  const sdk = join(OUT, 'sdk');
  mkdirSync(join(sdk, 'cmake'), { recursive: true });
  const list = engineExtensionDirs(ROOT).map((d) => `${d} ${dirHash(join(ROOT, d))}\n`).join('');
  writeFileSync(join(sdk, 'cmake/program-sources.txt'), list);
  r = stampCheck(sdk, tm);
  check('an SDK\'s program-sources.txt matches the stamp', r.ok, r.text.slice(0, 300));
  writeFileSync(join(sdk, 'cmake/program-sources.txt'), list.replace(/^(src\/water\/extensions )([0-9a-f])/m, (_, a, c) => a + (c === '0' ? '1' : '0')));
  r = stampCheck(sdk, tm);
  check('control: an SDK whose water sources differ fails', !r.ok && /Regenerate them all/.test(r.text), r.text.slice(0, 300));
  writeFileSync(join(t, `${name}_gl.c`), readFileSync(join(t, `${name}_gl.c`), 'utf8').replace(/^\/\* cw-source[^\n]*\n/gm, ''));
  r = stampCheck(ROOT, tm);
  check('control: a table without a stamp fails', !r.ok && /not stamped/.test(r.text), r.text.slice(0, 300));
  // a configure names every out-of-date table in one run, with one command for them all: two unstamped tables
  const t2 = join(OUT, 'table2');
  mkdirSync(t2, { recursive: true });
  for (const f of [`${name}.txt`, `${name}_gl.c`]) cpSync(join(t, f), join(t2, f.replace(name, `${name}b`)));
  const proj = join(OUT, 'project');
  mkdirSync(proj, { recursive: true });
  writeFileSync(join(proj, 'CMakeLists.txt'), `cmake_minimum_required(VERSION 3.20)\nproject(stamp_two NONE)\nset(CW_ROOT ${ROOT})\n` +
    `include(${ROOT}/cmake/program-stamp.cmake)\ncw__check_table_stamp(${tm} ${t} ${name})\ncw__check_table_stamp(${join(t2, `${name}b.txt`)} ${t2} ${name}b)\n`);
  const c = spawnSync('cmake', ['-S', proj, '-B', join(proj, 'build')], { encoding: 'utf8' });
  const ct = `${c.stdout}${c.stderr}`.replace(/\s+/g, ' ');
  check('control: a configure with two stale tables names both and one command for both', c.status !== 0 && /2 program table\(s\)/.test(ct) &&
    ct.includes(`${name} (`) && ct.includes(`${name}b (`) && (ct.match(/--table /g) || []).length === 2, ct.slice(0, 400));
}

// ── run time ──────────────────────────────────────────────────────────
if (!argv.includes('--no-run')) {
  const { enterGpuSandbox } = await import('../tools/lib/gpu-sandbox.mjs');
  const { testEnv, wgpuPin } = await import('../tools/lib/wgpu-pin.mjs');
  const { readImage, diffImages } = await import('../tools/lib/png.mjs');
  await enterGpuSandbox();
  testEnv(process.env);
  /** one native run of a sample: its last frame and debug fields */
  const run = (app, gpu, tag, env) => {
    const shot = join(OUT, `${tag}-${gpu}.png`), dump = join(OUT, `${tag}-${gpu}.json`);
    const e = { ...process.env, CW_GPU: gpu, CW_FRAMES: '60', CW_SCREENSHOT: shot, CW_DEBUG_DUMP: dump, SDL_AUDIODRIVER: 'dummy', ...env };
    if (gpu === 'webgpu') Object.assign(e, wgpuPin().env, { CW_GPU: 'webgpu' });
    const r = spawnSync(join(ROOT, 'build/native/samples', app), [], { encoding: 'utf8', env: e, cwd: join(ROOT, 'build/native/samples') });
    if (r.status !== 0 || !existsSync(shot)) throw new Error(`${app} ${tag} (${gpu}) failed (${r.status}): ${(r.stderr || '').trim().split('\n').slice(-4).join(' | ')}`);
    const f = existsSync(dump) ? JSON.parse(readFileSync(dump, 'utf8')) : {};
    const v = f['cw.three.dropped_values'];
    const text = Array.isArray(v) ? String.fromCharCode(...v.filter((b) => b)) : typeof v === 'string' && /^[0-9a-f]*$/i.test(v) ? String.fromCharCode(...(v.match(/../g) || []).map((h) => parseInt(h, 16)).filter((b) => b)) : String(v || '');
    return { img: readImage(shot), f, dropped: text.split('\n').map((l) => l.trim()).filter(Boolean) };
  };
  const RUNS = [
    ['split_array', { SA_MODE: 'water' }], ['split_array', {}], ['water_open_sea', {}], ['water_shore', {}],
    ['water_see_through', {}], ['terrain_demo', {}],
  ];
  for (const gpu of GPUS) {
    for (const [app, env] of RUNS)
      for (const q of ['verylow', 'low', 'medium', 'high', 'ultra']) {
        const r = run(app, gpu, `${app}${env.SA_MODE ? '-' + env.SA_MODE : ''}-${q}`, { ...env, CW_QUALITY: q });
        check(`${app}${env.SA_MODE ? ' ' + env.SA_MODE : ''} ${q} (${gpu}): no value dropped`, !r.dropped.length, r.dropped.slice(0, 6).join('; '));
      }
    const m = run('split_array', gpu, 'miss', { SA_MODE: 'water', CW_QUALITY: 'high', CW_TEST_EXT_MISS: '1' });
    check(`control: a value the water program lacks is reported (${gpu})`, m.dropped.some((l) => /uniform waterNoSuchValue$/.test(l)), m.dropped.join('; ') || 'none');
    const per = run('split_array', gpu, 'stale-perview', { SA_MODE: 'water', CW_QUALITY: 'high', CW_TEST_VIEWS_ARRAY: '0', CW_TEST_WATER_STALE: '1' });
    const one = run('split_array', gpu, 'stale-onepass', { SA_MODE: 'water', CW_QUALITY: 'high', CW_TEST_VIEWS_ARRAY: '1', CW_TEST_WATER_STALE: '1' });
    const d1 = diffImages(per.img, one.img).gt16;
    check(`a water program without the view rectangle draws view by view (${gpu})`, one.f['sa.array'] === 0 && d1 <= 0.003, `sa.array ${one.f['sa.array']}, ${(100 * d1).toFixed(4)}%`);
    const ctl = run('split_array', gpu, 'stale-control', { SA_MODE: 'water', CW_QUALITY: 'high', CW_TEST_VIEWS_ARRAY: '1', CW_TEST_WATER_STALE: '2' });
    const d2 = diffImages(per.img, ctl.img).gt16;
    check(`control: drawn in one pass all the same it misses the bound (${gpu})`, ctl.f['sa.array'] === 1 && d2 > 0.003, `sa.array ${ctl.f['sa.array']}, ${(100 * d2).toFixed(4)}%`);
  }
}
console.log(fail ? `program-tables: ${fail} check(s) failed` : 'program-tables: ok');
process.exit(fail ? 1 : 0);
