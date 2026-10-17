#!/usr/bin/env bash
# Build one prebuilt engine SDK (docs/sdk.md): the engine's libraries for one
# target, packed with their headers, CMake package and tools.
#
#   scripts/build-sdk.sh <target>
#   BUILD_TYPE=Debug scripts/build-sdk.sh <target>     a debug SDK (-debug in its name)
#
# Targets and what each needs fetched first (scripts/fetch-deps.sh, plus
# --only <name> for each export entry):
#   linux-x64       export/llvm-mingw export/sysroot-linux-x64 export/native-dawn-linux-x64
#   linux-arm64     export/llvm-mingw export/sysroot-linux-arm64 export/native-dawn-linux-arm64
#   windows-x64     export/llvm-mingw export/native-dawn-win32-x64 export/angle-gles-win32-x64
#   android-arm64   export/native-dawn-android-arm64 (and an NDK: ANDROID_NDK_HOME or ANDROID_HOME)
#   macos-arm64     export/native-dawn-darwin-arm64 export/angle-gles-darwin-arm64 (on a Mac)
#   ios-arm64       export/angle-ios, plus the macOS line above for the cwpack it packs with
#                   (on a Mac with Xcode); WebGPU when export/native-dawn-ios-arm64 is fetched too
#   ios-arm64-simulator  the same, built for the simulator (CW_IOS_SIM)
#   cart            (wasi-sdk, binaryen: the default fetch)
#   cart-wgpu       (the same, plus wasmcart-wgpu and emdawnwebgpu)
#
# Output: out/sdk/cartwheel-sdk-<version>-<target>[-debug]/ and its .tar.gz
# with a .sha256 beside it. The build tree is build/sdk/<target>[-debug].
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="${1:?usage: scripts/build-sdk.sh <target>}"
BT="${BUILD_TYPE:-Release}"
SUFFIX=""; [ "$BT" = Release ] || SUFFIX="-$(echo "$BT" | tr '[:upper:]' '[:lower:]')"
B="$ROOT/build/sdk/$T$SUFFIX"
source "$ROOT/scripts/lib/build-lock.sh"   # one build of a tree at a time
build_lock "$B"
ARGS=(-G Ninja -DCMAKE_BUILD_TYPE="$BT" -DCW_ENGINE_ONLY=ON -DCW_SDK=ON -DCW_THREE_BUILDER=OFF)
case "$T" in
  linux-x64|linux-arm64|windows-x64|android-arm64|macos-arm64)
    ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/export/$T.cmake" -DCW_WGPU=ON) ;;
  ios-arm64|ios-arm64-simulator)
    # cwpack (a host tool) from the macOS export tree first: the iOS tree packs with it
    build_lock "$ROOT/build/export/macos"
    cmake -S "$ROOT" -B "$ROOT/build/export/macos" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCW_ENGINE_ONLY=ON \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/export/macos-arm64.cmake" >/dev/null
    ninja -C "$ROOT/build/export/macos" cwpack
    build_unlock "$ROOT/build/export/macos"
    ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/export/ios-arm64.cmake")
    [ "$T" = ios-arm64-simulator ] && ARGS+=(-DCW_IOS_SIM=ON)
    # WebGPU only with native-dawn built for iOS (scripts/deps.lock export/native-dawn-$T)
    if [ -d "$ROOT/deps/export/native-dawn-$T/lib/cmake/Dawn" ]; then ARGS+=(-DCW_WGPU=ON); else ARGS+=(-DCW_WGPU=OFF); fi ;;
  cart) ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/cart.cmake") ;;
  cart-wgpu) ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/cart.cmake" -DCW_WGPU=ON) ;;
  *) echo "unknown SDK target: $T (linux-x64 linux-arm64 windows-x64 android-arm64 macos-arm64 ios-arm64 ios-arm64-simulator cart cart-wgpu)" >&2; exit 2 ;;
esac
cmake -S "$ROOT" -B "$B" "${ARGS[@]}" >/dev/null
ninja -C "$B" cw_sdk
# the version in the name: CW_SDK_VERSION (CI: the tag), else git describe
VER="${CW_SDK_VERSION:-$(git -C "$ROOT" describe --tags --always --dirty)}"
VER="${VER#v}"
NAME="cartwheel-sdk-$VER-$T$SUFFIX"
mkdir -p "$ROOT/out/sdk"
PACK=(--build "$B" --target "$T" --out "$ROOT/out/sdk/$NAME" --tar "$ROOT/out/sdk/$NAME.tar.gz")
case "$T" in
  cart|cart-wgpu) # the same libraries without ThinLTO, for dev carts (CW_CART_DEV): lib/nolto/
    build_lock "$B-nolto"
    cmake -S "$ROOT" -B "$B-nolto" "${ARGS[@]}" -DCW_CART_LTO=OFF >/dev/null
    ninja -C "$B-nolto" cw_sdk
    PACK+=(--nolto-build "$B-nolto") ;;
esac
node "$ROOT/tools/sdk-pack.mjs" "${PACK[@]}"
if command -v sha256sum >/dev/null; then sum=(sha256sum); else sum=(shasum -a 256); fi
(cd "$ROOT/out/sdk" && "${sum[@]}" "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
ls -l "$ROOT/out/sdk/$NAME.tar.gz"
