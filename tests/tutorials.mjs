// tutorials gate: every tutorial's finished project (docs/tutorials/<slug>/project)
// builds native AND cart against this engine checkout and does what the
// tutorial says, on every host in its tutorial.json, so the docs cannot rot.
//
// Per tutorial (format: docs/AUTHORING.md "Tutorials"):
//   1. the project still starts from its template: CMakeLists.txt and
//      .gitignore are byte-identical to editor/templates/<template>/
//   2. every @include in index.md names a file of the project or the repo
//      (tools/gen-docs.mjs --check covers regions and links)
//   3. copy to out/tutorials/<slug>/project, tools/cwproject.mjs build ... all
//   4. the input script on every host: `fields` agree exactly with native,
//      `expect` matches and `atLeast` is reached on native
//   5. the control (no input, or env) must FAIL the expectations
//   or, with "gate": run docs/tutorials/<slug>/<gate> <copied project dir>.
//
//   node tests/tutorials.mjs [slug ...] [--hosts native,romdev]
import { spawnSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readImage, diffImages } from '../tools/lib/png.mjs';
import { enterGpuSandbox } from '../tools/lib/gpu-sandbox.mjs';
await enterGpuSandbox(); // only the test GPU is visible, no desktop (tools/lib/gpu-sandbox.mjs)

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const TUT = join(ROOT, 'docs/tutorials');
const OUT = join(ROOT, 'out/tutorials');
const argv = process.argv.slice(2);
const hostsArg = argv.includes('--hosts') ? argv[argv.indexOf('--hosts') + 1].split(',') : null;
const only = argv.filter((a, i) => !a.startsWith('--') && argv[i - 1] !== '--hosts');

let failed = 0;
const check = (name, ok, detail = '') => {
  console.log(ok ? `ok   ${name}` : `FAIL ${name}${detail ? ': ' + String(detail).slice(0, 1500) : ''}`);
  if (!ok) failed++;
  return ok;
};
const run = (args, env) => spawnSync('node', [join(ROOT, 'tools/run-cart.mjs'), ...args],
  { encoding: 'utf8', timeout: 600000, env: { ...process.env, ...(env || {}) } });
const readProj = (dir) => Object.fromEntries(readFileSync(join(dir, 'project.cwproj'), 'utf8').split('\n')
  .map((l) => l.trim()).filter((l) => l && !'#;['.includes(l[0]) && l.includes('='))
  .map((l) => [l.slice(0, l.indexOf('=')).trim(), l.slice(l.indexOf('=') + 1).trim().replace(/^"(.*)"$/, '$1')]));

/** what the expectations say about a state; returns the list of misses */
function misses(spec, s) {
  const out = [];
  for (const [k, v] of Object.entries(spec.expect || {})) if (JSON.stringify(s?.[k]) !== JSON.stringify(v)) out.push(`${k}=${JSON.stringify(s?.[k])} (want ${JSON.stringify(v)})`);
  for (const [k, v] of Object.entries(spec.atLeast || {})) if (!(typeof s?.[k] === 'number' && s[k] >= v)) out.push(`${k}=${JSON.stringify(s?.[k])} (want >= ${v})`);
  return out;
}

const slugs = readdirSync(TUT).filter((d) => existsSync(join(TUT, d, 'tutorial.json'))).filter((d) => !only.length || only.includes(d)).sort();
check(`tutorials found (${slugs.join(' ')})`, slugs.length > 0 && (!only.length || only.every((o) => slugs.includes(o))));
mkdirSync(OUT, { recursive: true });

for (const slug of slugs) {
  const dir = join(TUT, slug);
  const spec = JSON.parse(readFileSync(join(dir, 'tutorial.json'), 'utf8'));
  const src = join(dir, spec.project || 'project');
  const work = join(OUT, slug);
  const proj = join(work, 'project');
  if (!check(`${slug}: has index.md and ${spec.project || 'project'}/project.cwproj`, existsSync(join(dir, 'index.md')) && existsSync(join(src, 'project.cwproj')))) continue;

  // 1. still the template's project skeleton
  if (spec.template) {
    const tdir = join(ROOT, 'editor/templates', spec.template);
    check(`${slug}: template ${spec.template} exists`, existsSync(join(tdir, 'template.cfg')));
    for (const f of ['CMakeLists.txt', '.gitignore']) {
      const a = existsSync(join(tdir, f)) ? readFileSync(join(tdir, f), 'utf8') : null;
      const b = existsSync(join(src, f)) ? readFileSync(join(src, f), 'utf8') : null;
      check(`${slug}: ${f} is the ${spec.template} template's`, a !== null && a === b);
    }
  }
  // 2. includes name real files
  const md = readFileSync(join(dir, 'index.md'), 'utf8');
  const incs = [...md.matchAll(/^@include\s+(\S+)\s*$/gm)].map((m) => m[1].split('#')[0]);
  check(`${slug}: ${incs.length} @include(s), every file exists`, incs.length > 0 && incs.every((p) => existsSync(join(ROOT, p))),
    incs.filter((p) => !existsSync(join(ROOT, p))).join(' '));

  // 3. copy (without build output) and build; only this tutorial's directory is cleared
  rmSync(work, { recursive: true, force: true });
  mkdirSync(work, { recursive: true });
  cpSync(src, proj, { recursive: true, filter: (p) => !/[\\/](\.cwbuild|\.cwcache|exports)([\\/]|$)/.test(p.slice(src.length)) });
  if (spec.gate) {
    const g = spawnSync('node', [join(dir, spec.gate), proj, ...(hostsArg ? ['--hosts', hostsArg.join(',')] : [])],
      { encoding: 'utf8', timeout: 3600000, cwd: ROOT });
    process.stdout.write((g.stdout || '').split('\n').filter(Boolean).map((l) => `  ${slug}| ${l}`).join('\n') + '\n');
    check(`${slug}: ${spec.gate} passes`, g.status === 0, (g.stderr || '').slice(-1500));
    continue;
  }
  const b = spawnSync('node', [join(ROOT, 'tools/cwproject.mjs'), 'build', proj, 'all'], { encoding: 'utf8', timeout: 3600000 });
  const results = (b.stdout || '').split('\n').filter((l) => l.startsWith('@result ')).map((l) => JSON.parse(l.slice(8)));
  const nat = results.find((r) => r.target === 'native'), crt = results.find((r) => r.target === 'cart');
  if (!check(`${slug}: builds native + cart`, b.status === 0 && nat?.ok && crt?.ok, results.length ? JSON.stringify(results) : (b.stdout || '').slice(-1500))) continue;
  const exe = join(proj, nat.path), cart = join(proj, crt.path), app = readProj(proj).app;
  const frames = String(spec.frames || 300);
  const input = spec.input ? join(dir, spec.input) : null;
  const hosts = (hostsArg || spec.hosts || ['native', 'romdev']).filter((h) => (spec.hosts || ['native', 'romdev']).includes(h) || h === 'native');
  if (!hosts.includes('native')) hosts.unshift('native');

  // 4. every host
  const states = {}, shots = {};
  for (const h of hosts) {
    const shot = join(work, `${h}.png`), state = join(work, `${h}.json`);
    const a = ['--host', h, '--app', app, '--exe', exe, '--cart', cart, '--frames', frames, '--shot', shot];
    if (h !== 'wcnative') a.push('--state', state);
    if (input && h !== 'wcnative') a.push('--input', input);
    const r = run(a, spec.env);
    if (!check(`${slug}: runs on ${h}`, r.status === 0 && existsSync(shot), (r.stderr || r.stdout || '').slice(-1200))) continue;
    shots[h] = shot;
    if (h !== 'wcnative') states[h] = JSON.parse(readFileSync(state, 'utf8'));
  }
  const ref = states.native;
  if (!ref) continue;
  const fields = spec.fields || [];
  const miss = misses(spec, ref);
  check(`${slug}: native does what the tutorial says (${[...Object.keys(spec.expect || {}), ...Object.keys(spec.atLeast || {})].map((k) => `${k}=${JSON.stringify(ref[k])}`).join(' ')})`,
    !miss.length && Object.keys({ ...spec.expect, ...spec.atLeast }).length > 0, miss.join('; '));
  check(`${slug}: every field is in the debug state`, fields.every((f) => f in ref), fields.filter((f) => !(f in ref)).join(' '));
  for (const h of Object.keys(states).filter((h) => h !== 'native')) {
    const diff = fields.filter((f) => JSON.stringify(states[h][f]) !== JSON.stringify(ref[f]));
    check(`${slug}: ${h} state equals native`, !diff.length, diff.map((f) => `${f}: ${JSON.stringify(states[h][f])} vs ${JSON.stringify(ref[f])}`).join('; '));
    const d = diffImages(readImage(shots.native), readImage(shots[h]));
    check(`${slug}: ${h} frame matches native (${(d.gt16 * 100).toFixed(3)}% > 16)`, d.gt16 <= (spec.maxGt16 ?? 0.002));
  }
  // 5. the control must fail
  const ctl = spec.control || { input: null };
  const cs = join(work, 'control.json');
  const ca = ['--host', 'native', '--app', app, '--exe', exe, '--frames', frames, '--state', cs, '--shot', join(work, 'control.png')];
  const cinput = ctl.input === undefined ? input : ctl.input ? join(dir, ctl.input) : null;
  if (cinput) ca.push('--input', cinput);
  const c = run(ca, { ...(spec.env || {}), ...(ctl.env || {}) });
  const cst = c.status === 0 && existsSync(cs) ? JSON.parse(readFileSync(cs, 'utf8')) : null;
  check(`${slug}: control (${JSON.stringify(ctl)}) must fail the expectations`, cst && misses(spec, cst).length > 0, cst ? 'control passed: ' + JSON.stringify(cst).slice(0, 400) : (c.stderr || '').slice(-800));
}
console.log(failed ? `tutorials: ${failed} check(s) failed` : 'tutorials: all checks passed');
process.exit(failed ? 1 : 0);
