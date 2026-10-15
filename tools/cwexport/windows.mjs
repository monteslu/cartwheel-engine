// Windows x64: a zip with the .exe (llvm-mingw, UCRT, C++ runtime and
// winpthreads static), its .cwpak, and beside it the shared libraries it runs
// with: Dawn (webgpu_dawn.dll with dxcompiler.dll and dxil.dll: WebGPU on
// Direct3D 12) and ANGLE (libEGL.dll, libGLESv2.dll, d3dcompiler_47.dll: GLES
// 3.0 on Direct3D 11, the fallback when WebGPU does not start).
// webgpu_dawn.dll is built with MSVC and needs Microsoft's Visual C++ runtime
// (MSVCP140.dll, VCRUNTIME140.dll, VCRUNTIME140_1.dll,
// MSVCP140_ATOMIC_WAIT.dll): the export ships it when CW_VCRUNTIME_DIR names
// a folder holding them (Visual Studio's VC\Redist\MSVC\<version>\x64\Microsoft.VC143.CRT,
// which may be redistributed beside an application); without it the player
// needs the Visual C++ Redistributable installed.
import { existsSync } from 'node:fs';
import { basename, join } from 'node:path';
import { buildApp, packAssets, strip, write, freshDir, copy, zipDir, needDep, slug, runtimeFiles, copyDawnLicenses, ROOT, ExportError } from './util.mjs';
import { writeIcon } from './icon.mjs';

const VCRUNTIME = ['msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'];

export function validate(ctx) {
  needDep('llvm-mingw/bin/x86_64-w64-mingw32-clang');
  const vc = process.env.CW_VCRUNTIME_DIR;
  if (vc) for (const f of VCRUNTIME) if (!existsSync(join(vc, f))) throw new ExportError(`CW_VCRUNTIME_DIR=${vc} has no ${f}`);
}

export async function run(ctx) {
  const app = await buildApp(ctx, 'windows-x64');
  const name = slug(ctx.app), top = `${name}-${ctx.version}-windows-x64`;
  const stage = freshDir(join(ctx.outDir, top));
  ctx.progress(60, 'stage');
  copy(app.binary, join(stage, `${name}.exe`));
  await strip(join(stage, `${name}.exe`), ctx.log);
  await packAssets(ctx, app, join(stage, `${name}.cwpak`));
  const libs = runtimeFiles(app);
  for (const f of libs) copy(f, join(stage, basename(f)));
  const dawn = libs.some((f) => /webgpu_dawn\.dll$/i.test(f));
  const vc = process.env.CW_VCRUNTIME_DIR;
  if (dawn && vc) for (const f of VCRUNTIME) copy(join(vc, f), join(stage, f));
  copy(join(ROOT, 'tools/cwexport/licenses/ANGLE.txt'), join(stage, 'licenses/ANGLE.txt'));
  copyDawnLicenses(libs, join(stage, 'licenses/dawn'));
  writeIcon(ctx, join(stage, `${name}.png`));
  write(join(stage, 'README.txt'), `${ctx.title} ${ctx.version} for Windows x64 (Windows 10 or later).

Run ${name}.exe. ${dawn ? `Renders with WebGPU (Dawn on Direct3D 12) and falls back to
OpenGL ES 3.0 through ANGLE on Direct3D 11 when WebGPU does not start; the
environment variable CW_GPU=gl or CW_GPU=webgpu picks one.${vc ? '' : `
Needs the Microsoft Visual C++ Redistributable (x64) for WebGPU.`}` : `Rendering is OpenGL ES 3.0 through ANGLE on Direct3D 11.`}
Keep every file in this folder together. Exported with cartwheel's tools/cwexport.
`.replace(/\n/g, '\r\n'));
  ctx.progress(80, 'zip');
  return [await zipDir(stage, top, join(ctx.outDir, `${top}.zip`)), stage];
}
