// macOS and iOS.
//   macOS: built on a Mac (Xcode's clang, cmake/export/macos-arm64.cmake):
//          an arm64 .app (the executable, Contents/Frameworks with Dawn's and
//          ANGLE's dylibs, the .cwpak in Resources), signed ad hoc, zipped.
//          Not notarized: distribution to other Macs needs signing with a
//          Developer ID and Apple's notarization (your Apple account), else
//          Gatekeeper asks the player to allow it (README.txt says how).
//   iOS:   an Xcode project generator: Info.plist, the .cwpak, the icon and
//          build-on-mac.sh (cmake -G Xcode with cmake/export/ios.cmake; the
//          app bundle embeds ANGLE's xcframeworks). Signing team: DEVELOPMENT_TEAM.
import { join } from 'node:path';
import { buildApp, packAssets, write, freshDir, copy, slug, needDep, run, strip, runtimeFiles, copyDawnLicenses, zipDir, ROOT, EXPORT_DEPS, ExportError } from './util.mjs';
import { basename } from 'node:path';
import { writeIcon } from './icon.mjs';
import { readFileSync } from 'node:fs';

export function validateMac(ctx) {
  if (process.platform !== 'darwin') throw new ExportError('the macOS export builds on a Mac (Xcode\'s clang and SDK exist only there)');
  bundleId(ctx);
}
export function validateIos(ctx) { needDep('angle-ios/libGLESv2.xcframework', 'angle-ios/libEGL.xcframework'); bundleId(ctx); }

function bundleId(ctx) {
  const id = ctx.opt('app_id', `org.cartwheel.${slug(ctx.app).replace(/[^a-z0-9-]/g, '-')}`);
  if (!/^[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$/.test(id)) throw new ExportError(`app_id "${id}" is not a bundle identifier (e.g. com.example.mygame)`);
  return id;
}

const xml = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;');

/** an .icns holding one 256x256 PNG ('ic08') */
function icns(png) {
  const entry = Buffer.alloc(8);
  entry.write('ic08', 0, 'ascii');
  entry.writeUInt32BE(8 + png.length, 4);
  const head = Buffer.alloc(8);
  head.write('icns', 0, 'ascii');
  head.writeUInt32BE(16 + png.length, 4);
  return Buffer.concat([head, entry, png]);
}

/** the assets pack, made here (no Apple tools needed): from the Linux x64 export build */
async function pack(ctx, dst) {
  needDep('llvm-mingw/bin/clang', 'sysroot-linux-x64');
  const app = await buildApp(ctx, 'linux-x64');
  return packAssets(ctx, app, dst);
}

function infoPlist(ctx, name, ios) {
  const keys = ios ? `
  <key>LSRequiresIPhoneOS</key><true/>
  <key>UIRequiresFullScreen</key><true/>
  <key>UIStatusBarHidden</key><true/>
  <key>UISupportedInterfaceOrientations</key>
  <array><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
  <key>UILaunchScreen</key><dict/>
  <key>MinimumOSVersion</key><string>14.0</string>` : `
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>CFBundleIconFile</key><string>${name}</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>`;
  return `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>${xml(ctx.title)}</string>
  <key>CFBundleDisplayName</key><string>${xml(ctx.title)}</string>
  <key>CFBundleIdentifier</key><string>${bundleId(ctx)}</string>
  <key>CFBundleExecutable</key><string>${name}</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${ctx.version}</string>
  <key>CFBundleVersion</key><string>${ctx.version}</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>${keys}
</dict>
</plist>
`;
}

function cmakeLine(ctx, toolchain, buildDir) {
  const proj = ctx.isProject ? ` -DCW_PROJECT_DIR="${ctx.projectDir}" -DCW_ENGINE_ONLY=ON` : '';
  return `cmake -S "$CARTWHEEL" -B "${buildDir}" -G Xcode -DCMAKE_TOOLCHAIN_FILE="$CARTWHEEL/cmake/export/${toolchain}"${proj}`;
}

export async function runMac(ctx) {
  const app = await buildApp(ctx, 'macos');
  const name = slug(ctx.app), top = `${name}-${ctx.version}-macos-arm64`;
  const dir = freshDir(join(ctx.outDir, top));
  const bundle = join(dir, `${ctx.title.replace(/[/:]/g, '-')}.app`);
  ctx.progress(60, 'bundle');
  const exe = join(bundle, 'Contents/MacOS', name);
  copy(app.binary, exe);
  const { chmodSync, rmSync } = await import('node:fs');
  chmodSync(exe, 0o755);
  await strip(exe, ctx.log);
  const libs = runtimeFiles(app);
  for (const f of libs) copy(f, join(bundle, 'Contents/Frameworks', basename(f)));
  await packAssets(ctx, app, join(bundle, 'Contents/Resources', `${name}.cwpak`));
  write(join(bundle, 'Contents/Info.plist'), infoPlist(ctx, name, false));
  writeIcon(ctx, join(dir, '.icon.png'));
  write(join(bundle, 'Contents/Resources', `${name}.icns`), icns(readFileSync(join(dir, '.icon.png'))));
  rmSync(join(dir, '.icon.png'));
  copy(join(ROOT, 'tools/cwexport/licenses/ANGLE.txt'), join(bundle, 'Contents/Resources/licenses/ANGLE.txt'));
  copyDawnLicenses(libs, join(bundle, 'Contents/Resources/licenses/dawn'));
  // ad hoc signatures (arm64 code must be signed to run): the dylibs, then the bundle
  ctx.progress(75, 'sign');
  for (const f of libs) await run('codesign', ['--force', '--sign', process.env.CODESIGN_IDENTITY || '-', join(bundle, 'Contents/Frameworks', basename(f))], { log: ctx.log });
  await run('codesign', ['--force', '--sign', process.env.CODESIGN_IDENTITY || '-', bundle], { log: ctx.log });
  write(join(dir, 'README.txt'), `${ctx.title} ${ctx.version} for macOS 11+ on Apple silicon (arm64).

Open ${basename(bundle)}. It renders with WebGPU (Dawn on Metal) and falls back
to OpenGL ES 3.0 through ANGLE on Metal when WebGPU does not start.

This build is signed ad hoc, not with a Developer ID, and not notarized: the
first time, macOS refuses to open it. Right-click the app, choose Open, then
Open again (or System Settings > Privacy & Security > Open Anyway). From a
terminal: xattr -dr com.apple.quarantine "${basename(bundle)}".
Exported with cartwheel's tools/cwexport.
`);
  ctx.progress(85, 'zip');
  return [await zipDir(dir, top, join(ctx.outDir, `${top}.zip`)), dir];
}

export async function runIos(ctx) {
  const name = slug(ctx.app), top = `${name}-${ctx.version}-ios`;
  const dir = freshDir(join(ctx.outDir, top));
  ctx.progress(30, 'pack');
  await pack(ctx, join(dir, 'Resources', `${name}.cwpak`));
  ctx.progress(70, 'xcode template');
  write(join(dir, 'Info.plist'), infoPlist(ctx, ctx.app, true));
  writeIcon(ctx, join(dir, 'Resources/AppIcon.png'), 1024);
  copy(join(EXPORT_DEPS, 'angle-ios/LICENSE'), join(dir, 'Resources/licenses/ANGLE.txt'));
  write(join(dir, 'build-on-mac.sh'), `#!/bin/sh
# Generate and build ${ctx.title}'s Xcode project for iOS devices (arm64).
# Needs a Mac with Xcode, CMake and a cartwheel checkout with deps fetched
# (scripts/fetch-deps.sh --only export/angle-ios,export/angle-gles-darwin-arm64,export/native-dawn-darwin-arm64).
#   CARTWHEEL=/path/to/cartwheel DEVELOPMENT_TEAM=ABCDE12345 ./build-on-mac.sh
# Then open the generated project in Xcode to run on a device or archive.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
CARTWHEEL="\${CARTWHEEL:?set CARTWHEEL to the cartwheel checkout}"
# cwpack (a host tool) from the macOS build first
cmake -S "$CARTWHEEL" -B "$CARTWHEEL/build/export/macos" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$CARTWHEEL/cmake/export/macos-arm64.cmake"
cmake --build "$CARTWHEEL/build/export/macos" --target cwpack
build="$CARTWHEEL/build/export/ios"
${cmakeLine(ctx, 'ios.cmake', '$build')} -DCW_APPLE_INFO_PLIST="$here/Info.plist" \\
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="\${DEVELOPMENT_TEAM:-}" \\
  -DCMAKE_XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER=${bundleId(ctx)}
cmake --build "$build" --config Release --target ${ctx.app} -- -allowProvisioningUpdates
app="$(find "$build" -path '*Release-iphoneos*' -name '${ctx.app}.app' -type d | head -1)"
cp "$here/Resources/${name}.cwpak" "$app/${ctx.app}.cwpak"
echo "built $app (re-sign after adding the pack: codesign --force --sign \\"Apple Development\\" \\"$app\\")"
echo "Xcode project: $build/cartwheel.xcodeproj"
`, 0o755);
  write(join(dir, 'README.txt'), `${ctx.title} ${ctx.version} for iOS 14+ (arm64 devices).

NOT BUILT: iOS apps need Apple's SDK (Xcode) and signing with your Apple
developer account, which exist only on a Mac. On a Mac, ./build-on-mac.sh
generates the Xcode project (cmake -G Xcode) and builds the app; the asset
pack (Resources/${name}.cwpak) goes in the bundle root. Rendering: OpenGL ES
3.0 through ANGLE on Metal (embedded xcframeworks).
`);
  return [dir];
}
