// Windows x64: a zip with the .exe (llvm-mingw, UCRT, C++ runtime and
// winpthreads static), its .cwpak, and ANGLE (D3D11) beside it: libEGL.dll,
// libGLESv2.dll and what they link (libc++.dll, libunwind.dll, zlib1.dll).
// d3dcompiler_47.dll is part of Windows 10 and later.
import { join } from 'node:path';
import { buildApp, packAssets, strip, write, freshDir, copy, zipDir, needDep, slug, EXPORT_DEPS } from './util.mjs';
import { writeIcon } from './icon.mjs';

/** what ships beside the exe: [source under deps/export, name in the zip] */
const RUNTIME = [
  ['angle-win-x64/bin/libEGL.dll', 'libEGL.dll'],
  ['angle-win-x64/bin/libGLESv2.dll', 'libGLESv2.dll'],
  ['zlib-win-x64/bin/zlib1.dll', 'zlib1.dll'],
  ['llvm-mingw/x86_64-w64-mingw32/bin/libc++.dll', 'libc++.dll'],
  ['llvm-mingw/x86_64-w64-mingw32/bin/libunwind.dll', 'libunwind.dll'],
];
const LICENSES = [
  ['angle-win-x64/share/licenses/angleproject/LICENSE', 'licenses/ANGLE.txt'],
  ['zlib-win-x64/share/licenses/zlib/LICENSE', 'licenses/zlib.txt'],
  ['llvm-mingw/LICENSE.TXT', 'licenses/llvm-mingw (libc++, libunwind).txt'],
];

export function validate(ctx) {
  needDep('llvm-mingw/bin/x86_64-w64-mingw32-clang', ...RUNTIME.map((r) => r[0]), 'angle-macos/include/GLES3/gl3.h');
}

export async function run(ctx) {
  const app = await buildApp(ctx, 'windows-x64');
  const name = slug(ctx.app), top = `${name}-${ctx.version}-windows-x64`;
  const stage = freshDir(join(ctx.outDir, top));
  ctx.progress(60, 'stage');
  copy(app.binary, join(stage, `${name}.exe`));
  await strip(join(stage, `${name}.exe`), ctx.log);
  await packAssets(ctx, app, join(stage, `${name}.cwpak`));
  for (const [src, dst] of [...RUNTIME, ...LICENSES]) copy(join(EXPORT_DEPS, src), join(stage, dst));
  writeIcon(ctx, join(stage, `${name}.png`));
  write(join(stage, 'README.txt'), `${ctx.title} ${ctx.version} for Windows x64 (Windows 10 or later).

Run ${name}.exe. Rendering is OpenGL ES 3.0 through ANGLE on Direct3D 11
(libEGL.dll, libGLESv2.dll beside the game). Keep every file in this folder
together. Exported with cartwheel's tools/cwexport.
`.replace(/\n/g, '\r\n'));
  ctx.progress(80, 'zip');
  return [await zipDir(stage, top, join(ctx.outDir, `${top}.zip`)), stage];
}
