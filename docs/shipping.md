# Ship your game

From a game repository to published builds for every platform. The quickest
start is [cartwheel-starter](https://github.com/monteslu/cartwheel-starter),
a template repository with all of this set up; this page says what it does,
so the same steps work in any repository.

## Pick the SDKs

A game builds against prebuilt engine SDKs (docs/sdk.md), one per target,
attached to each engine release:

| You want | SDK | Built on |
|---|---|---|
| to build and run on your Linux PC | `linux-x64` (or `linux-arm64`) | that machine |
| to build and run on your Mac | `macos-arm64` | that Mac |
| Windows builds | `windows-x64` | Linux (cross-compiled with llvm-mingw) |
| Android builds | `android-arm64` | Linux (the Android NDK and SDK) |
| `.wasc` carts (browser, wasmcart players, RetroArch) | `cart-wgpu` (GL and WebGPU in one cart) or `cart` (GL only) | Linux x64 |

A build always needs the SDK of the machine it runs on too: it carries
`cwpack`, the asset packer, and `cwexport`, the packager.

## Pin an engine version

Pin one engine release and the sha256 of each SDK you use, so a build fetches
exactly the files it was tested with. Published SDKs never change (docs/sdk.md,
"Versions"): a pinned version builds the same forever, and moving to a new
engine is a deliberate change of the pin. cartwheel-examples' and the
starter's `scripts/deps.lock` look like this:

```
cartwheel-sdk:linux-x64 https://github.com/monteslu/cartwheel-engine/releases/download/v0.5.3/cartwheel-sdk-0.5.3-linux-x64.tar.gz v0.5.3 <sha256>
```

Each tarball's `.sha256` sits beside it on the release page. Each SDK's
`scripts/fetch-deps.sh --only <names>` then fetches the toolchain that SDK
was built with (wasi-sdk for carts, llvm-mingw and a sysroot for Linux and
Windows builds), pinned the same way.

## Build locally

```cmake
cmake_minimum_required(VERSION 3.20)
project(mygame C CXX)
find_package(cartwheel REQUIRED CONFIG)
cw_add_app(mygame SOURCES main.c PACK ${CMAKE_CURRENT_SOURCE_DIR}/assets/roots.txt)
```

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release -Dcartwheel_DIR=<sdk>/linux-x64/lib/cmake/cartwheel
ninja -C build/native
```

Only the game compiles. Carts link in seconds as dev carts
(`-DCW_CART_DEV=ON`: no ThinLTO, no wasm-opt, the same simulation); release
carts leave it off. docs/sdk.md has the cross builds' and carts' configure
lines.

## Tag a release

Releases come from CI on a `v*` tag: build each platform with its SDK,
package each with `cwexport`, run what the runner can run, attach the files
to the GitHub release.

```sh
git tag v1.0.0 && git push origin v1.0.0
```

`cwexport --project <dir> --preset <name> --build <tree> --version <v>`
packages a tree a build already made; the presets live in the game's
`project.cwproj` (docs/export.md).

## What each platform gets

| Platform | Package | Renders with |
|---|---|---|
| Linux x64, arm64 | `.tar.gz` (the game, its `.cwpak`, `libwebgpu_dawn.so`, `run.sh`, a `.desktop` file) and an `.AppImage` | WebGPU (Dawn on Vulkan), else the system's OpenGL ES 3.0; needs glibc 2.34 |
| Windows x64 | `.zip`: the `.exe`, its `.cwpak`, Dawn's and ANGLE's DLLs, and Microsoft's Visual C++ runtime DLLs when the build adds them | WebGPU (Dawn on Direct3D 12), else ANGLE (Direct3D 11) |
| macOS arm64 | `.zip` holding the `.app` (Dawn and ANGLE in Contents/Frameworks) | WebGPU (Dawn on Metal), else ANGLE (Metal) |
| Android arm64 | `.apk` | WebGPU (Dawn on Vulkan), else the system's OpenGL ES 3.0 |
| wasmcart | `.wasc` | what the host offers: WebGPU, else WebGL 2 / GLES |

An exported game picks WebGPU itself and falls back to GL when WebGPU does
not start on the player's machine; `CW_GPU=gl` or `CW_GPU=webgpu` in the
environment picks one.

## Signing

- **macOS**: exports are signed ad hoc, which runs on the Mac that built it
  and, after the player allows it once (right-click, Open), on others.
  Distributing without that prompt needs a Developer ID certificate
  (`CODESIGN_IDENTITY` when exporting) and Apple's notarization with your
  Apple account (`xcrun notarytool submit ... --wait`, then
  `xcrun stapler staple`).
- **Android**: without a keystore an APK is signed with Android's debug key:
  it installs for testing and sideloading, not on a store. For a store build
  set `keystore` and `keystore_alias` in the preset and the passwords in
  `CW_ANDROID_STORE_PASSWORD` and `CW_ANDROID_KEY_PASSWORD` (docs/export.md).
- **Windows**: exports are not code-signed; SmartScreen may warn the first
  time a player runs one. Sign the `.exe` with your certificate
  (`signtool`, or `osslsigncode` on Linux) before zipping if you have one.
