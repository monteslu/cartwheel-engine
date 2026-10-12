// macOS (universal) and iOS: what can be made without Apple's tools. Apple's
// SDK, Xcode, code signing and notarization exist only on a Mac and need the
// developer's Apple account, so these exports produce:
//   macOS: the .app bundle with everything but the executable (Info.plist,
//          icon, the .cwpak in Resources, ANGLE's universal Metal dylibs in
//          Frameworks) and build-on-mac.sh, which builds the universal
//          executable with cmake -G Xcode (cmake/export/macos.cmake), puts it
//          in the bundle, signs it (CODESIGN_IDENTITY, ad hoc by default) and
//          prints the notarization command.
//   iOS:   an Xcode project generator: Info.plist, the .cwpak, the icon and
//          build-on-mac.sh (cmake -G Xcode with cmake/export/ios.cmake; the
//          app bundle embeds ANGLE's xcframeworks). Signing team: DEVELOPMENT_TEAM.
// Neither is built or run here.
import { join, relative } from 'node:path';
import { buildApp, packAssets, write, freshDir, copy, slug, needDep, EXPORT_DEPS, ExportError } from './util.mjs';
import { writeIcon } from './icon.mjs';
import { readFileSync } from 'node:fs';

export function validateMac(ctx) { needDep('angle-macos/libGLESv2.dylib', 'angle-macos/libEGL.dylib'); bundleId(ctx); }
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
  const name = slug(ctx.app), top = `${name}-${ctx.version}-macos`;
  const dir = freshDir(join(ctx.outDir, top));
  const app = join(dir, `${ctx.title.replace(/[/:]/g, '-')}.app`);
  ctx.progress(30, 'pack');
  await pack(ctx, join(app, 'Contents/Resources', `${name}.cwpak`));
  ctx.progress(70, 'bundle');
  write(join(app, 'Contents/Info.plist'), infoPlist(ctx, name, false));
  writeIcon(ctx, join(dir, '.icon.png'));
  write(join(app, 'Contents/Resources', `${name}.icns`), icns(readFileSync(join(dir, '.icon.png'))));
  const { rmSync } = await import('node:fs');
  rmSync(join(dir, '.icon.png'));
  for (const f of ['libEGL.dylib', 'libGLESv2.dylib']) copy(join(EXPORT_DEPS, 'angle-macos', f), join(app, 'Contents/Frameworks', f));
  copy(join(EXPORT_DEPS, 'angle-macos/LICENSE'), join(app, 'Contents/Resources/licenses/ANGLE.txt'));
  write(join(app, 'Contents/MacOS/.placeholder'), 'build-on-mac.sh puts the executable here\n');
  const appName = relative(dir, app);
  write(join(dir, 'build-on-mac.sh'), `#!/bin/sh
# Build ${ctx.title}'s universal (arm64 + x86_64) executable into the bundle,
# then sign it. Needs a Mac with Xcode and CMake, and a cartwheel checkout
# with deps fetched (scripts/fetch-deps.sh --only export/angle-macos).
#   CARTWHEEL=/path/to/cartwheel CODESIGN_IDENTITY="Developer ID Application: ..." ./build-on-mac.sh
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
CARTWHEEL="\${CARTWHEEL:?set CARTWHEEL to the cartwheel checkout}"
build="$CARTWHEEL/build/export/macos"
${cmakeLine(ctx, 'macos.cmake', '$build')}
cmake --build "$build" --config Release --target ${ctx.app}
exe="$(find "$build" -path '*Release*' -name '${ctx.app}' -type f -perm -u+x | head -1)"
app="$here/${appName}"
cp "$exe" "$app/Contents/MacOS/${name}"
rm -f "$app/Contents/MacOS/.placeholder"
install_name_tool -add_rpath @executable_path/../Frameworks "$app/Contents/MacOS/${name}" 2>/dev/null || true
codesign --force --options runtime --timestamp --sign "\${CODESIGN_IDENTITY:--}" "$app/Contents/Frameworks/"*.dylib
codesign --force --options runtime --timestamp --sign "\${CODESIGN_IDENTITY:--}" "$app"
echo "built $app"
echo "notarize (needs your Apple account):"
echo "  ditto -c -k --keepParent \\"$app\\" app.zip && xcrun notarytool submit app.zip --keychain-profile <profile> --wait && xcrun stapler staple \\"$app\\""
`, 0o755);
  write(join(dir, 'README.txt'), `${ctx.title} ${ctx.version} for macOS 11+ (universal: Apple silicon and Intel).

NOT BUILT: macOS executables need Apple's SDK (Xcode), which exists only on a
Mac. This folder holds the finished app bundle minus its executable; on a Mac
run ./build-on-mac.sh to build it, put it in the bundle and sign it.
Distribution outside the App Store also needs notarization with your Apple
account (the script prints the command). Rendering: OpenGL ES 3.0 through
ANGLE on Metal (Contents/Frameworks).
`);
  return [dir];
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
# (scripts/fetch-deps.sh --only export/angle-ios --only export/angle-macos).
#   CARTWHEEL=/path/to/cartwheel DEVELOPMENT_TEAM=ABCDE12345 ./build-on-mac.sh
# Then open the generated project in Xcode to run on a device or archive.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
CARTWHEEL="\${CARTWHEEL:?set CARTWHEEL to the cartwheel checkout}"
# cwpack (a host tool) from the macOS build first
${cmakeLine(ctx, 'macos.cmake', '$CARTWHEEL/build/export/macos')}
cmake --build "$CARTWHEEL/build/export/macos" --config Release --target cwpack
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
