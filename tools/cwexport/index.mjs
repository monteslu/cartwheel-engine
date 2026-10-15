// cartwheel's exporters as a library: what tools/cwexport.mjs runs and what
// the editor's export panel calls, so both produce identical output.
//
//   import { exportPresets } from './tools/cwexport/index.mjs';
//   const results = await exportPresets({ project: 'path/to/project', presets: ['linux', 'web'] });
//
// A project is a directory holding project.cwproj (flat key = value lines):
//   app = "<cw_add_app target>"          required
//   name = "<display name>"              the title (default: app)
//   version = "1.0.0"                    default 0.1.0 (a preset may override)
//   export.<preset>.platform = "<platform>"   one per preset (PLATFORMS below)
//   export.<preset>.output = "<dir>"     default exports/<preset> (relative to the project)
//   export.<preset>.<key> = ...          per platform: app_id, version, title, icon,
//                                        coi_shim (web), appimage (linux), keystore + keystore_alias
//                                        (android), orientation (android)
// A project with a CMakeLists.txt (its cw_add_app calls) is built with the
// engine (-DCW_PROJECT_DIR); one without names an app the cartwheel tree
// itself defines (the samples and games).
import { existsSync, readFileSync, statSync } from 'node:fs';
import { join, resolve, isAbsolute, relative } from 'node:path';
import { parseCwproj, mkdirp, ExportError, ROOT } from './util.mjs';
import * as linux from './linux.mjs';
import * as windows from './windows.mjs';
import * as android from './android.mjs';
import * as web from './web.mjs';
import * as apple from './apple.mjs';

/** platform name -> { validate(ctx), run(ctx) -> files, built: what "ok" means } */
export const PLATFORMS = {
  'linux-x64': { validate: linux.validate, run: linux.run, what: 'tarball + AppImage (built)' },
  'linux-arm64': { validate: linux.validate, run: linux.run, what: 'tarball + AppImage (built)' },
  'windows-x64': { validate: windows.validate, run: windows.run, what: 'zip with the exe and ANGLE (built)' },
  'android-arm64': { validate: android.validate, run: android.runAndroid, what: 'APK + Gradle project (built)' },
  web: { validate: web.validate, run: web.runWeb, what: 'static site + zip (built)' },
  wasc: { validate: web.validate, run: web.runCart, what: '.wasc cart (built)' },
  macos: { validate: apple.validateMac, run: apple.runMac, what: 'arm64 .app zip, signed ad hoc (built on a Mac only)' },
  ios: { validate: apple.validateIos, run: apple.runIos, what: 'Xcode project generator + Info.plist + pack (NOT built: needs a Mac)' },
};

const PRESET_RE = /^[a-z0-9_-]+$/;

/**
 * Read a project's settings and its export presets.
 * @param {string} project the directory holding project.cwproj
 */
export function readProject(project) {
  const dir = resolve(project);
  const file = join(dir, 'project.cwproj');
  if (!existsSync(file)) throw new ExportError(`${dir} has no project.cwproj`);
  const keys = parseCwproj(readFileSync(file, 'utf8'));
  const presets = {};
  for (const [k, v] of Object.entries(keys)) {
    const m = /^export\.([^.]+)\.(.+)$/.exec(k);
    if (!m) continue;
    (presets[m[1]] ||= {})[m[2]] = v;
  }
  return { dir, keys, presets, isProject: existsSync(join(dir, 'CMakeLists.txt')) };
}

function makeCtx(proj, preset, opts) {
  const p = proj.presets[preset];
  const app = proj.keys.app;
  const opt = (k, d) => (p[k] !== undefined ? p[k] : proj.keys[k] !== undefined && !['platform', 'output'].includes(k) ? proj.keys[k] : d);
  const out = opts.out ? resolve(opts.out, preset) : resolve(proj.dir, p.output || join('exports', preset));
  return {
    preset, platform: p.platform, app, projectDir: proj.dir, isProject: proj.isProject, keys: proj.keys,
    title: opt('title', proj.keys.name || app), version: opts.version || opt('version', '0.1.0'), opt, outDir: out,
    verbose: !!opts.verbose, threeBuilder: !!opts.threeBuilder, buildDir: opts.build || null, log: opts.onLog || (() => {}), progress: (pct, stage) => (opts.onProgress || (() => {}))(preset, pct, stage),
  };
}

/** Problems with one preset that need no build, as a message (null when fine). */
function check(proj, preset) {
  if (!PRESET_RE.test(preset)) return `preset name "${preset}": use [a-z0-9_-]`;
  const p = proj.presets[preset];
  if (!p) return `no preset "${preset}" in project.cwproj (have: ${Object.keys(proj.presets).join(', ') || 'none'}; add export.${preset}.platform = "<platform>")`;
  if (!p.platform) return `preset "${preset}" has no platform (export.${preset}.platform = one of ${Object.keys(PLATFORMS).join(', ')})`;
  if (!PLATFORMS[p.platform]) return `preset "${preset}": unknown platform "${p.platform}" (one of ${Object.keys(PLATFORMS).join(', ')})`;
  if (!proj.keys.app || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(proj.keys.app)) return 'project.cwproj needs app = "<the cw_add_app target name>"';
  if (proj.presets[preset].version && !/^\d+(\.\d+){0,2}$/.test(proj.presets[preset].version)) return `version "${proj.presets[preset].version}": use 1, 1.2 or 1.2.3`;
  return null;
}

/**
 * Export presets of a project. Never throws for a failed preset: each gets a
 * result {preset, ok, platform, files} or {preset, ok:false, error}.
 * @param {{project: string, presets?: string[], validate?: boolean, out?: string, verbose?: boolean, threeBuilder?: boolean, build?: string, version?: string,
 *          onLog?: (line: string) => void, onProgress?: (preset: string, pct: number, stage: string) => void}} o
 * @returns {Promise<Array<{preset: string, ok: boolean, platform?: string, files?: string[], error?: string, what?: string}>>}
 */
export async function exportPresets(o) {
  let proj;
  try { proj = readProject(o.project); } catch (e) { return [{ preset: '*', ok: false, error: e.message }]; }
  const presets = o.presets && o.presets.length ? o.presets : Object.keys(proj.presets);
  if (!presets.length) return [{ preset: '*', ok: false, error: 'project.cwproj defines no export presets (export.<preset>.platform = ...)' }];
  const results = [];
  for (const preset of presets) {
    const bad = check(proj, preset);
    if (bad) { results.push({ preset, ok: false, error: bad }); continue; }
    const ctx = makeCtx(proj, preset, o);
    const plat = PLATFORMS[ctx.platform];
    try {
      plat.validate(ctx);
      if (o.validate) { results.push({ preset, ok: true, platform: ctx.platform, files: [], what: plat.what }); continue; }
      ctx.progress(0, 'start');
      mkdirp(ctx.outDir);
      const files = await plat.run(ctx);
      ctx.progress(100, 'done');
      const rel = (f) => { const r = relative(proj.dir, f); return r.startsWith('..') || isAbsolute(r) ? f : r; };
      results.push({ preset, ok: true, platform: ctx.platform, files: files.map(rel), what: plat.what });
    } catch (e) {
      if (!(e instanceof ExportError) && o.verbose) ctx.log(e.stack);
      results.push({ preset, ok: false, platform: ctx.platform, error: e.message });
    }
  }
  return results;
}

export { ROOT };
