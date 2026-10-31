#!/usr/bin/env bash
# Link a game against a packed native SDK (docs/sdk.md), as a game repository
# does: unpack the tarball somewhere else and build samples/hello from it. A
# static library builds with symbols no target can resolve, so only linking
# shows them (v0.25.0's macOS SDK packed fine and no game could link with it).
#
#   scripts/sdk-link-check.sh <target> <sdk.tar.gz>
set -euo pipefail

T="${1:?target}"
TGZ="$(cd "$(dirname "${2:?sdk tarball}")" && pwd)/$(basename "$2")"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="$ROOT/out/sdk-link-check/$T"
rm -rf "$W"
mkdir -p "$W/sdk" "$W/game"
tar -xzf "$TGZ" -C "$W/sdk" --strip-components=1
cp "$ROOT/samples/hello/main.c" "$W/game/main.c"
cat > "$W/game/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(linkcheck C CXX)
find_package(cartwheel REQUIRED CONFIG)
cw_add_app(linkcheck SOURCES main.c)
EOF
cmake -S "$W/game" -B "$W/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$W/sdk/cmake/export/$T.cmake" \
  -Dcartwheel_DIR="$W/sdk/lib/cmake/cartwheel" -DCW_EXPORT_DEPS="$ROOT/deps/export" >/dev/null
ninja -C "$W/build" linkcheck
exe="$(find "$W/build" -maxdepth 1 -type f \( -name linkcheck -o -name linkcheck.exe -o -name 'liblinkcheck.so' \) | head -1)"
[ -n "$exe" ] || { echo "sdk-link-check $T: the build made no game" >&2; exit 1; }
rm -rf "$W"
echo "sdk-link-check $T: a game links against $(basename "$TGZ")"
