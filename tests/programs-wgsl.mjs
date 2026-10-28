// tools/programs.mjs fails when Tint (Dawn's WGSL compiler) rejects a captured WebGPU program
// (tools/lib/wgsl-check.mjs): the program tables' captures run on a mock GPU that accepts any WGSL.
//
//   node tests/programs-wgsl.mjs
//
// 1. every capture directory of the build's program tables (build/native/programs-cache/*) compiles;
// 2. controls that MUST fail: a WGSL syntax error, and a pipeline Tint cannot link (the fragment reads a
//    value the vertex stage does not write);
// 3. vertex buffers as three.c binds them: an instanced program with every optional attribute (position,
//    normal, uv, color, tangent, the four instance matrix columns, the instance colour) is 7 buffers and
//    compiles; the control MUST fail: nine geometry attributes are over every device's 8, and the check
//    names it as over the device limits before Dawn does. (One buffer per attribute, the old layout, made
//    the first program 10 buffers: rejected, as three.c's own test_vertex_buffer_layout control shows.)
// 4. the known Tint internal error (a swizzle of a swizzle on a variable: v.xy.x) is reported with its
//    hint while the Dawn the engine ships still has it (informational once a Dawn update fixes it).
// Runs on lavapipe (software Vulkan): no GPU, no suite lock.
import { mkdirSync, readdirSync, rmSync, statSync, writeFileSync, existsSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { checkWgsl, capturedStates } from '../tools/lib/wgsl-check.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
let failed = 0;
const ok = (c, m) => { console.log(`${c ? 'ok  ' : 'FAIL'} ${m}`); if (!c) failed++; };

// 1. the real captures
const cacheRoot = join(ROOT, 'build', 'native', 'programs-cache');
// (one directory per table: the newest. Older ones hold captures of extension sources since changed)
const newest = new Map();
for (const d of existsSync(cacheRoot) ? readdirSync(cacheRoot) : []) {
  // (by its captures' times: the check's own stamps touch every directory it reads)
  const table = d.replace(/-[0-9a-f]{12}$/, '');
  const t = Math.max(0, ...readdirSync(join(cacheRoot, d)).filter((f) => f.endsWith('.json')).map((f) => statSync(join(cacheRoot, d, f)).mtimeMs));
  if (!newest.has(table) || newest.get(table).t < t) newest.set(table, { d: join(cacheRoot, d), t });
}
const dirs = [...newest.values()].map((x) => x.d).filter((d) => capturedStates(d).length);
if (!dirs.length) console.log('skip: no program captures in build/native/programs-cache (regenerate a table first)');
for (const d of dirs) {
  try { const r = checkWgsl(d); ok(true, `${d.slice(ROOT.length + 1)}: ${r.checked + r.cached} programs accepted`); }
  catch (e) { ok(false, e.message); }
}

// 2. 3. and 4. fixtures
const dir = join(ROOT, 'out', 'programs-wgsl');
rmSync(dir, { recursive: true, force: true });
mkdirSync(dir, { recursive: true });
const json = JSON.stringify({ backend: 'wgpu', features: [], stages: ['vertex', 'fragment'],
  attributes: [{ name: 'position', type: 'vec3', location: 0, itemSize: 3, array: 'Float32Array', instanced: false }] });
const vert = (extra = '') => `struct VO { @builtin(position) p: vec4<f32>, ${extra} };
@vertex fn main(@location(0) position: vec3<f32>) -> VO { var o: VO; o.p = vec4<f32>(position, 1.0); return o; }`;
const frag = (body, input = '') => `struct OutputStruct { @location(0) color: vec4<f32> };
@fragment fn main(${input}) -> OutputStruct { var output: OutputStruct; ${body} return output; }`;
const write = (st, v, f, j = json) => { writeFileSync(join(dir, st + '.wgpu.vert'), v); writeFileSync(join(dir, st + '.wgpu.frag'), f); writeFileSync(join(dir, st + '.wgpu.json'), j); };
// a vertex stage reading every attribute it is given (so none is dead and every location is in the pipeline)
const attrs = (list) => list.map(([name, type, instanced], location) => ({ name, type, location, itemSize: +type.slice(-1) || 1, array: 'Float32Array', instanced: !!instanced }));
const readAll = (list) => {
  const wgsl = { vec2: 'vec2<f32>', vec3: 'vec3<f32>', vec4: 'vec4<f32>' };
  const params = list.map((a) => `@location(${a.location}) ${a.name}: ${wgsl[a.type]}`).join(', ');
  const sum = list.map((a) => `${a.name}.x`).join(' + ');
  return `struct VO { @builtin(position) p: vec4<f32> };
@vertex fn main(${params}) -> VO { var o: VO; o.p = vec4<f32>(position, 1.0) + vec4<f32>(${sum}) * 0.0; return o; }`;
};
const fullInstanced = attrs([['position', 'vec3'], ['normal', 'vec3'], ['uv', 'vec2'], ['color', 'vec3'], ['tangent', 'vec4'],
  ['nodeAttribute0', 'vec4'], ['nodeAttribute1', 'vec4'], ['nodeAttribute2', 'vec4'], ['nodeAttribute3', 'vec4'], ['nodeAttribute4', 'vec3', true]]);
const nineVertex = attrs([['position', 'vec3'], ['normal', 'vec3'], ['uv', 'vec2'], ['color', 'vec3'], ['tangent', 'vec4'],
  ['uv1', 'vec2'], ['uv2', 'vec2'], ['uv3', 'vec2'], ['skinWeight', 'vec4']]);
const jsonOf = (a) => JSON.stringify({ backend: 'wgpu', features: [], stages: ['vertex', 'fragment'], attributes: a });
const plainFrag = frag('output.color = vec4<f32>(1.0, 1.0, 1.0, 1.0);');
write('instanced-all', readAll(fullInstanced), plainFrag, jsonOf(fullInstanced));
write('nine-buffers', readAll(nineVertex), plainFrag, jsonOf(nineVertex));
write('good', vert(), frag('var v = vec4<f32>(0.25, 0.5, 0.75, 1.0); let s = v.xy; output.color = vec4<f32>(s.x, s.y, 0.0, 1.0);'));
write('syntax', vert(), frag('output.color = vec4<f32>(1.0, 0.0, 0.0, 1.0)'));   // (no semicolon)
write('link', vert(), frag('output.color = vec4<f32>(uv, 0.0, 1.0);', '@location(3) uv: vec2<f32>'));
write('swizzle', vert(), frag('var v = vec4<f32>(0.25, 0.5, 0.75, 1.0); output.color = vec4<f32>(v.xy.x, v.xy.y, 0.0, 1.0);'));
const run = (states) => { try { checkWgsl(dir, states); return null; } catch (e) { return e.message; } };
ok(run(['good']) === null, 'a valid program is accepted');
ok(run(['good']) === null && existsSync(join(dir, 'good.wgpu.tint')), 'and stamped (not compiled again)');
const syn = run(['syntax']);
ok(syn && /syntax: /.test(syn) && /fragment \d+:\d+/.test(syn), 'control: a WGSL syntax error fails the check' + (syn ? '' : ' (it passed)'));
const link = run(['link']);
ok(link && /link: /.test(link), 'control: a pipeline that cannot link fails the check' + (link ? '' : ' (it passed)'));
const both = run(['good', 'syntax']);
ok(both && /syntax/.test(both) && !/good:/.test(both), 'a bad program fails the whole check and is named; the good one is not');
ok(run(['instanced-all']) === null, 'an instanced program with every optional attribute (7 vertex buffers as three.c binds them) is accepted');
const nine = run(['nine-buffers']);
ok(nine && /nine-buffers: over the device limits: 9 vertex buffers \(max 8\)/.test(nine) && /skinWeight/.test(nine),
  'control: a program needing 9 vertex buffers is reported as over the device limits, naming its attributes' + (nine ? '' : ' (it passed)'));
const sw = run(['swizzle']);
if (sw) ok(/swizzle view instruction/.test(sw) && /swizzle of a swizzle/.test(sw), 'the known Tint internal error (v.xy.x on a variable) is reported, with its hint');
else console.log('note: this Dawn compiles v.xy.x on a variable (the Tint internal error is fixed): the pitfall in docs/guides/rendering-3d.md can go');
console.log(failed ? `${failed} failed` : 'programs-wgsl: all ok');
process.exit(failed ? 1 : 0);
