// Linux x86_64 / arm64: a tarball (the executable, its .cwpak, a launcher, a
// .desktop entry and icon, native-dawn's libwebgpu_dawn.so beside the
// executable, found through its $ORIGIN rpath) and an AppImage. Built with
// clang + lld against the Debian bullseye sysroot, SDL2 static; WebGPU is Dawn
// on Vulkan, GL the player's libGLESv2 (Mesa or a vendor driver through
// libglvnd), the fallback when WebGPU does not start.
import { existsSync, readFileSync, writeFileSync, chmodSync } from 'node:fs';
import { join } from 'node:path';
import { buildApp, packAssets, strip, write, freshDir, copy, tarGz, needDep, slug, run, runtimeFiles, copyDawnLicenses, EXPORT_DEPS, ExportError } from './util.mjs';
import { basename } from 'node:path';
import { writeIcon } from './icon.mjs';

const ARCH = { 'linux-x64': 'x86_64', 'linux-arm64': 'aarch64' };
const RUNTIME = { 'linux-x64': 'appimage-runtime-x64/runtime-x86_64', 'linux-arm64': 'appimage-runtime-arm64/runtime-aarch64' };
const SYSROOT = { 'linux-x64': 'sysroot-linux-x64', 'linux-arm64': 'sysroot-linux-arm64' };

export function validate(ctx) {
  needDep('llvm-mingw/bin/clang', SYSROOT[ctx.platform]);
  if (ctx.opt('appimage', 'true') !== 'false') needDep(RUNTIME[ctx.platform]);
}

export async function run_(ctx) {
  const app = await buildApp(ctx, ctx.platform);
  const name = slug(ctx.app), ver = ctx.version, arch = ARCH[ctx.platform];
  const top = `${name}-${ver}-${ctx.platform}`;
  const stage = freshDir(join(ctx.outDir, top));
  ctx.progress(60, 'stage');
  copy(app.binary, join(stage, name));
  await strip(join(stage, name), ctx.log);
  const libs = runtimeFiles(app);
  for (const f of libs) copy(f, join(stage, basename(f)));
  copyDawnLicenses(libs, join(stage, 'licenses/dawn'));
  const pak = await packAssets(ctx, app, join(stage, `${name}.cwpak`));
  writeIcon(ctx, join(stage, `${name}.png`));
  // the launcher: runs the game from wherever the folder was unpacked
  write(join(stage, `run.sh`), `#!/bin/sh
# ${ctx.title}: run from anywhere (the game finds ${name}.cwpak beside its executable)
here="$(dirname "$(readlink -f "$0")")"
exec "$here/${name}" "$@"
`, 0o755);
  write(join(stage, `${name}.desktop`), desktopEntry(ctx, name, `run.sh`));
  write(join(stage, 'README.txt'), `${ctx.title} ${ver} for Linux ${arch}.

Run ./run.sh (or ./${name}). ${libs.length ? `Renders with WebGPU (Dawn on Vulkan) and
falls back to OpenGL ES 3.0 (libGLESv2.so.2: Mesa or your GPU vendor's driver)
when WebGPU does not start; CW_GPU=gl or CW_GPU=webgpu picks one. Needs glibc
2.34 and GCC 11's libstdc++ or newer (Dawn's floor); X11 or XWayland.` : `Needs OpenGL ES 3.0 (libGLESv2.so.2: Mesa or
your GPU vendor's driver) and glibc 2.31 or newer; X11 or XWayland.`}
Exported with cartwheel's tools/cwexport.
`);
  const files = [];
  ctx.progress(75, 'tarball');
  files.push(await tarGz(stage, top, join(ctx.outDir, `${top}.tar.gz`)));
  if (ctx.opt('appimage', 'true') !== 'false') {
    ctx.progress(85, 'AppImage');
    files.push(await appImage(ctx, name, stage, pak, join(ctx.outDir, `${top}.AppImage`), libs.map((f) => basename(f))));
  }
  files.push(stage);
  return files;
}

function desktopEntry(ctx, name, exec) {
  return `[Desktop Entry]
Type=Application
Name=${ctx.title}
Exec=${exec}
Icon=${name}
Categories=Game;
Terminal=false
`;
}

/** runtime + a squashfs of the AppDir (what appimagetool does, without it) */
async function appImage(ctx, name, stage, pak, out, libs) {
  const appdir = freshDir(join(ctx.outDir, `.appdir-${name}-${ctx.platform}`));
  copy(join(stage, name), join(appdir, 'usr/bin', name));
  for (const l of libs) copy(join(stage, l), join(appdir, 'usr/bin', l));
  if (pak) copy(pak, join(appdir, 'usr/bin', `${name}.cwpak`));
  copy(join(stage, `${name}.png`), join(appdir, `${name}.png`));
  copy(join(stage, `${name}.png`), join(appdir, 'usr/share/icons/hicolor/256x256/apps', `${name}.png`));
  write(join(appdir, `${name}.desktop`), desktopEntry(ctx, name, name));
  write(join(appdir, 'AppRun'), `#!/bin/sh
here="$(dirname "$(readlink -f "$0")")"
exec "$here/usr/bin/${name}" "$@"
`, 0o755);
  const sq = join(ctx.outDir, `.${name}-${ctx.platform}.squashfs`);
  await run('mksquashfs', [appdir, sq, '-root-owned', '-noappend', '-no-progress', '-quiet', '-comp', 'gzip', '-all-time', '315619200', '-mkfs-time', '315619200', '-no-xattrs'], { log: ctx.log });
  const runtime = readFileSync(join(EXPORT_DEPS, RUNTIME[ctx.platform]));
  writeFileSync(out, Buffer.concat([runtime, readFileSync(sq)]));
  chmodSync(out, 0o755);
  freshDir(appdir);
  const { rmSync } = await import('node:fs');
  rmSync(appdir, { recursive: true, force: true });
  rmSync(sq, { force: true });
  if (!existsSync(out)) throw new ExportError(`no AppImage at ${out}`);
  return out;
}

export { run_ as run };
