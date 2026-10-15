#!/usr/bin/env node
// Export a cartwheel project with its project.cwproj presets (the editor's
// export button runs exactly this).
//
//   node tools/cwexport.mjs --project <dir> [--preset a,b] [--validate] [--out <dir>] [--build <dir>] [--version <v>] [--verbose] [--three-builder]
//
// --build: an already configured and built tree for the preset's platform (a
// game repository's own build of every app, e.g. build/windows-x64): only the
// app's target is rebuilt there and packaged. --version overrides the
// project's (a release's tag).
//
// --three-builder builds with three.c's runtime program builder (cmake
// CW_THREE_BUILDER, its own build tree): states three.c's table lacks are
// built on first use instead of skipped. A preview aid, never the default.
//   node tools/cwexport.mjs --platforms
//
// stdout: log lines; `@progress <preset> <0..100> <stage>`; one
// `@result {json}` per preset ({preset, ok, platform, files} or {preset, ok:false, error}).
// Exit status 0 only when every preset succeeded. Platforms, preset keys and
// what each export contains: tools/cwexport/index.mjs, docs/export.md.
import { exportPresets, PLATFORMS } from './cwexport/index.mjs';

const argv = process.argv.slice(2);
const flag = (n) => argv.includes(`--${n}`);
const val = (n) => { const i = argv.indexOf(`--${n}`); return i >= 0 && argv[i + 1] && !argv[i + 1].startsWith('--') ? argv[i + 1] : undefined; };

if (flag('platforms')) {
  for (const [k, v] of Object.entries(PLATFORMS)) console.log(`${k.padEnd(14)} ${v.what}`);
  process.exit(0);
}
const project = val('project');
if (!project) {
  console.error('usage: cwexport.mjs --project <dir> [--preset a,b] [--validate] [--out <dir>] [--verbose] | --platforms');
  process.exit(2);
}
const results = await exportPresets({
  project,
  presets: (val('preset') || '').split(',').map((s) => s.trim()).filter(Boolean),
  validate: flag('validate'),
  out: val('out'),
  build: val('build'),
  version: val('version'),
  verbose: flag('verbose'),
  threeBuilder: flag('three-builder'),
  onLog: (l) => console.log(l),
  onProgress: (p, pct, stage) => console.log(`@progress ${p} ${pct} ${stage}`),
});
for (const r of results) console.log(`@result ${JSON.stringify(r)}`);
process.exit(results.every((r) => r.ok) ? 0 : 1);
