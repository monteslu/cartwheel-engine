# Prebuilt engine SDKs

Every release (`v*` tag) attaches one SDK per target, built by
`.github/workflows/sdk.yml`. A game builds against the SDK for its engine
version and compiles only its own code: the engine, three.c, Box2D, Box3D,
the audio codecs, SDL2 and the rest come prebuilt.

| SDK | Built on | Contents beyond the static libraries |
|---|---|---|
| `cartwheel-sdk-<v>-linux-x64` | Ubuntu x64, clang + lld against the Debian bullseye sysroot | `bin/cwpack`, native-dawn (`libwebgpu_dawn.so`) |
| `cartwheel-sdk-<v>-linux-arm64` | Ubuntu arm64, the same toolchain built for arm64 hosts | `bin/cwpack`, native-dawn |
| `cartwheel-sdk-<v>-windows-x64` | Ubuntu x64, llvm-mingw (UCRT) | `bin/cwpack.exe`, native-dawn (`webgpu_dawn.dll`, `dxcompiler.dll`, `dxil.dll`), ANGLE (D3D11) |
| `cartwheel-sdk-<v>-macos-arm64` | macOS arm64, Xcode's clang | `bin/cwpack`, native-dawn (`libwebgpu_dawn.dylib`), ANGLE (Metal) |
| `cartwheel-sdk-<v>-android-arm64` | Ubuntu x64, Android NDK 27.2 (API 24) | native-dawn (`libwebgpu_dawn.so`), SDL2's Java activity |
| `cartwheel-sdk-<v>-cart` | Ubuntu x64, wasi-sdk (wasm32-wasip1-threads) | GL carts: wasmcart-pack and the import checker |
| `cartwheel-sdk-<v>-cart-wgpu` | Ubuntu x64, wasi-sdk | dual GL + WebGPU carts: the emdawnwebgpu port, wasmcart-pack |

The native SDKs come as Release builds and, with `-debug` in the name, Debug
builds. Each tarball has a `.sha256` beside it.

Inside each SDK:

- `lib/`: the static libraries; `lib/cmake/cartwheel/`: the CMake package
  (`find_package(cartwheel CONFIG)`), with every library as an imported target
  under the name the engine's own build uses.
- `include/`, `deps/*/include`: the headers those targets expose.
- `cmake/`: `cw_add_app` and its helpers (the same files the engine builds
  with), and the target's toolchain file (`cmake/export/<target>.cmake`, or
  `cmake/cart.cmake`).
- `tools/`: in desktop SDKs `cwexport` (packaging), `programs.mjs` and
  three.c's generator (`deps/three.c/tools`) for program tables; in cart SDKs
  the import checker and asset stager the cart link runs.
- `scripts/fetch-deps.sh` and `scripts/deps.lock`: fetch the pinned toolchain
  into the SDK (`--only export/llvm-mingw,export/sysroot-linux-x64`, or
  `--only wasi-sdk,binaryen` for carts).
- `manifest.json`: the engine version and commit, the build type, the pinned
  toolchain (llvm-mingw, sysroot, NDK, Xcode or wasi-sdk), the compiler, the
  dependency revisions, native-dawn's and ANGLE's versions, the libraries and
  `runtime`: the shared libraries a game runs with, which exports copy beside
  the executable.

## Versions

An engine release's SDKs never change once published: the release workflow
refuses to upload an SDK the release already has, so a fix is a new version
(a new tag), never a replaced file. A game pins a version and each SDK's
sha256 (cartwheel-examples' `scripts/deps.lock` does), and builds the same
forever. Each SDK's `manifest.json` names the engine commit it was built
from.

## Building a game against an SDK

```cmake
cmake_minimum_required(VERSION 3.20)
project(mygame C CXX)
find_package(cartwheel REQUIRED CONFIG)
cw_add_app(mygame SOURCES main.c PACK ${CMAKE_CURRENT_SOURCE_DIR}/assets/roots.txt)
```

```sh
# Linux x64, this machine's compiler
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release -Dcartwheel_DIR=<sdk>/linux-x64/lib/cmake/cartwheel
# any target with its toolchain (here Windows from Linux; CW_EXPORT_DEPS: where llvm-mingw is)
cmake -S . -B build/windows-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<sdk>/windows-x64/cmake/export/windows-x64.cmake \
  -Dcartwheel_DIR=<sdk>/windows-x64/lib/cmake/cartwheel \
  -DCW_CWPACK=<sdk>/linux-x64/bin/cwpack -DCW_EXPORT_DEPS=<sdk>/linux-x64/deps/export
# carts (wasi-sdk fetched into the cart SDK)
cmake -S . -B build/cart-wgpu -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<sdk>/cart-wgpu/cmake/cart.cmake \
  -Dcartwheel_DIR=<sdk>/cart-wgpu/lib/cmake/cartwheel -DCW_CWPACK=<sdk>/linux-x64/bin/cwpack
```

Cart SDKs carry the libraries twice: ThinLTO bitcode (`lib/`, for release
carts: whole-program inlining, then wasm-opt) and plain objects
(`lib/nolto/`). `-DCW_CART_DEV=ON` links dev carts from the second, with no
ThinLTO and no wasm-opt: a games-only cart builds in seconds instead of a
minute, with the same simulation (BikeMesa's and Pugball's replays give the
same state hashes as release carts); release builds leave it off.

The package checks what it can: the build must target the SDK's system, a
cart SDK's libraries (ThinLTO bitcode) need the exact clang that wrote them
(the pinned wasi-sdk), and flags that would break the cross-platform
determinism contract (`-ffast-math`, `-Ofast`, `-ffp-contract=fast` or `on`,
the unsafe-math family) are an error. The determinism flags
(`-ffp-contract=off -fno-fast-math`) apply to every target in the directory
that finds the package.

In an export build (a toolchain file) each app gets the shared libraries it
runs with copied beside it, and an rpath of `$ORIGIN` (Linux) or
`@executable_path/../Frameworks` and `@executable_path` (macOS), so the build
tree's binaries run as shipped.

## Packaging

`node <host sdk>/tools/cwexport.mjs --project <dir> --preset <name>` exports a
project as the engine's own tree does (docs/export.md), with each target's
SDK found beside the host's (`<dir>/linux-x64`, `<dir>/windows-x64`, ...;
`CW_SDK_WINDOWS_X64=<dir>` and so on name others). `--build <dir>` packages
from a tree already configured and built for that platform.

## What an SDK does not carry

- The editor and its plugins' project editors (`<app>_editor`), the hot-reload
  host (`cwhot`) and three.c's runtime program builder: they need the engine's
  sources. Build the engine from source for those (a game repository keeps
  that as an option).
- The web export's player (wasmcart's web host): the web export runs from the
  engine's checkout.
