#!/usr/bin/env bash
# Link a game against a packed native SDK (docs/sdk.md), as a game repository
# does: unpack the tarball somewhere else and build samples/hello from it. A
# static library builds with symbols no target can resolve, so only linking
# shows them (v0.25.0's macOS SDK packed fine and no game could link with it).
#
#   scripts/sdk-link-check.sh <target> <sdk.tar.gz>
#
# A cart SDK (cart-wgpu) has no native link: its wasmcart-pack packs a cart
# with art from the SDK's own files instead (tests/cart-art.mjs --pack), so a
# file the pack tool imports and the SDK left out fails here (wasmcart
# 0.38.0's pack tool imports src/cartArt.js, 0.39.0's src/cartVersion.js).
set -euo pipefail

T="${1:?target}"
TGZ="$(cd "$(dirname "${2:?sdk tarball}")" && pwd)/$(basename "$2")"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="$ROOT/out/sdk-link-check/$T"
rm -rf "$W"
mkdir -p "$W/sdk" "$W/game"
tar -xzf "$TGZ" -C "$W/sdk" --strip-components=1
# the files a game's scripts and AGENTS.md name, and every module they import from the SDK itself
need=(AGENTS.md)
if [ "$T" = cart-wgpu ]; then need+=(tools/cart-version.mjs tools/lib/wasc-splice.mjs tools/lib/app-version.mjs tools/check-art.mjs)
else need+=(tests/gate.mjs tools/run-cart.mjs tools/programs.mjs tools/check-text.mjs tools/check-prompts.mjs tools/flash-check.mjs tools/check-art.mjs); fi
for f in "${need[@]}"; do
  [ -f "$W/sdk/$f" ] || { echo "sdk-link-check $T: the SDK has no $f" >&2; exit 1; }
  for imp in $(grep -oE "from '\.\.?/[^']+'" "$W/sdk/$f" | sed -E "s/from '(.*)'/\1/"); do
    [ -f "$(dirname "$W/sdk/$f")/$imp" ] || { echo "sdk-link-check $T: $f imports $imp, which the SDK lacks" >&2; exit 1; }
  done
done
if [ "$T" = cart-wgpu ]; then
  node "$ROOT/tests/cart-art.mjs" --pack "$W/sdk/deps/wasmcart-wgpu/bin/wasmcart-pack.js" --carts "$W/no-carts"
  rm -rf "$W"
  echo "sdk-link-check $T: the SDK's wasmcart-pack packs a cart with art"
  exit 0
fi
cp "$ROOT/samples/hello/main.c" "$W/game/main.c"
cat > "$W/game/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(linkcheck C CXX)
find_package(cartwheel REQUIRED CONFIG)
cw_add_app(linkcheck SOURCES main.c)
EOF
# the game packs nothing, but the package asks for a cwpack, and an Android SDK ships none
cmake -S "$W/game" -B "$W/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$W/sdk/cmake/export/$T.cmake" \
  -Dcartwheel_DIR="$W/sdk/lib/cmake/cartwheel" -DCW_EXPORT_DEPS="$ROOT/deps/export" -DCW_CWPACK="$(type -P true)" >/dev/null
ninja -C "$W/build" linkcheck
exe="$(find "$W/build" -type f \( -name linkcheck -o -name linkcheck.exe -o -name libmain.so \) | head -1)"
[ -n "$exe" ] || { echo "sdk-link-check $T: the build made no game" >&2; exit 1; }
rm -rf "$W"
echo "sdk-link-check $T: a game links against $(basename "$TGZ")"
