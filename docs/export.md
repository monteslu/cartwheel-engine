# Exporting a game

`tools/cwexport.mjs` turns a project into shippable builds, driven by the
export presets in its `project.cwproj`. The editor's Export panel runs exactly
this command, so a shell export and a one-click export give the same files.

```sh
scripts/fetch-deps.sh --export            # once: the exporters' toolchains (deps/export/, pinned with sha256)
node tools/cwexport.mjs --platforms       # what each platform produces
node tools/cwexport.mjs --project <dir> [--preset a,b] [--validate] [--out <dir>] [--verbose]
```

Output: log lines, `@progress <preset> <0..100> <stage>`, and one
`@result {json}` per preset (`{"preset","ok":true,"platform","files":[...]}` or
`{"preset","ok":false,"error"}`). Exit status 0 only when every preset
succeeded. `--validate` checks presets and toolchains without building. The
same function is a library: `exportPresets()` in `tools/cwexport/index.mjs`.

## Presets

`project.cwproj` is flat `key = value` lines:

```
name = "My Game"
app = "mygame"                       ; the cw_add_app target
version = "1.0.0"                    ; default 0.1.0
export.linux.platform = "linux-x64"
export.web.platform = "web"
export.android.platform = "android-arm64"
export.android.app_id = "com.example.mygame"
```

Per preset: `platform` (required), `output` (default `exports/<preset>`,
relative to the project), and any of `version`, `title`, `app_id` (Android
package / Apple bundle id, default `org.cartwheel.<app>`), `icon` (a PNG;
default a generated one), `coi_shim = true` (web: ship a service worker that cross-origin isolates the page on static hosts that cannot send the COOP / COEP headers, such as GitHub Pages), `appimage = false` (Linux), `keystore` +
`keystore_alias` (Android release signing; passwords in
`CW_ANDROID_STORE_PASSWORD` / `CW_ANDROID_KEY_PASSWORD`), `orientation`
(Android). A key set at the top level applies to every preset.

A project directory with a `CMakeLists.txt` (its `cw_add_app` calls) is built
with the engine (`-DCW_PROJECT_DIR=<dir> -DCW_ENGINE_ONLY=ON`) in
`<project>/.cwbuild/<platform>`. One without names an app this tree defines
(`games/platformer3d/assets` carries presets).

## Platforms

| platform | produces | how it is built | status on the build machine |
|---|---|---|---|
| `linux-x64` | `.tar.gz` (game, its `.cwpak`, `run.sh`, `.desktop`, icon) and an AppImage | clang + lld (llvm-mingw) against Debian bullseye's sysroot: glibc 2.31 floor; SDL2 static; links only libc, libm, libdl, libpthread, libstdc++, libgcc_s and libGLESv2 | built + run |
| `linux-arm64` | the same for aarch64 | same, arm64 sysroot | built only (no arm64 machine or emulator) |
| `windows-x64` | `.zip`: `.exe` (GUI subsystem), `.cwpak`, ANGLE `libEGL.dll` + `libGLESv2.dll` (D3D11) and what they link (`libc++.dll`, `libunwind.dll`, `zlib1.dll`), licenses | llvm-mingw (UCRT), C++ runtime and winpthreads static | built + run under Wine (wined3d); not run on real Windows |
| `android-arm64` | APK (debug-signed without a keystore) + the Gradle project | the game as `libmain.so` (NDK, SDL2 static, GLES 3.0), SDL2's Java activity, Gradle 8.14 / AGP 8.11, minSdk 24 | built + run on the x86_64 AVD (NDK translation, SwiftShader GLES) |
| `web` | static site (`index.html`, `player.js`, `wasmcart/`, `game.wasm` + `game.wasc`, `_headers`, `serve.mjs`) + `.zip` | the dual cart (WebGPU where the browser has it, else WebGL 2) on wasmcart's web host; its wasm on its own (`game.wasm`, compiled while it downloads and kept compiled by the browser), the rest in `game.wasc` | built + run in Chromium |
| `wasc` | the `.wasc` cart | `cmake/cart.cmake` (wasi-sdk), the dual cart (`CW_WGPU`: the host picks WebGPU or GL) | built + run on romdev and wasmcart-native |
| `macos` | the `.app` bundle without its executable (Info.plist, icon, pack, ANGLE Metal dylibs) + `build-on-mac.sh` | on a Mac: `cmake -G Xcode` with `cmake/export/macos.cmake` (universal arm64 + x86_64), then codesign; notarization with your Apple account | not built (needs Apple's SDK) |
| `ios` | Info.plist, pack, icon + `build-on-mac.sh` (generates and builds the Xcode project) | on a Mac: `cmake -G Xcode` with `cmake/export/ios.cmake` | not built (needs Apple's SDK and signing) |

Both cart exports ship the dual cart (`build/cart-wgpu`): it imports GL and
WebGPU and runs on WebGPU where the host has it. An app built `GL_ONLY` (it
calls GL itself) has no dual cart and ships its GL cart.

The web export needs a cross-origin isolated page (threads use
`SharedArrayBuffer`): every file must be served with
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`. `_headers` does it on Netlify and
Cloudflare Pages, `serve.mjs` locally, and the site's `README.txt` lists nginx,
Apache and Caddy lines. Without them the page says what is missing instead of
starting; `file://` never works.

Debuggable Android builds accept an intent extra `cw_env` (`KEY=VALUE;...`)
that becomes the game's environment, e.g. `CW_REMOTE=tcp:127.0.0.1:7400` with
`adb reverse` for the debug channel. Release builds ignore it.

## Tests

`node tests/export.mjs [platform ...]` exports platformer3d
for every platform, runs each export this machine can run, probes the running
game through the debug channel, and runs must-fail controls (an export without
its asset pack, a project with a broken asset path, a site without COOP/COEP, a
truncated cart). Results land in `out/export-test/status.json`.
