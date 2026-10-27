#!/usr/bin/env bash
# Build native (build/native) and cart (build/cart) trees, and the dual GL +
# WebGPU cart tree (build/cart-wgpu, cartwheel/gpu.h) when its dependencies
# are fetched (deps/wasmcart-wgpu, deps/emdawnwebgpu).
#   BUILD_TYPE=Debug scripts/build.sh    TARGETS="native" scripts/build.sh
#   TARGETS="cart-wgpu" scripts/build.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BT="${BUILD_TYPE:-Release}"
# one build of a tree at a time (scripts/lib/build-lock.sh): another waits
source "$ROOT/scripts/lib/build-lock.sh"
"$ROOT/scripts/lib/check-deps.sh" "$ROOT" || exit 1 # deps/ older than the pins: stop with the fix named, not a compile error
DEFAULT="native cart"
[ -d "$ROOT/deps/wasmcart-wgpu/wgpu-wasi" ] && [ -d "$ROOT/deps/emdawnwebgpu/webgpu" ] && DEFAULT="$DEFAULT cart-wgpu"
for t in ${TARGETS:-$DEFAULT}; do
  build_lock "$ROOT/build/$t"
  if [ "$t" = cart ]; then
    cmake -S "$ROOT" -B "$ROOT/build/cart" -G Ninja -DCMAKE_BUILD_TYPE="$BT" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/cart.cmake" >/dev/null
  elif [ "$t" = cart-wgpu ]; then
    cmake -S "$ROOT" -B "$ROOT/build/cart-wgpu" -G Ninja -DCMAKE_BUILD_TYPE="$BT" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/cart.cmake" \
      -DCW_WGPU=ON -DCW_CWPACK="$ROOT/build/native/tools/cwpack/cwpack" >/dev/null
  else
    cmake -S "$ROOT" -B "$ROOT/build/native" -G Ninja -DCMAKE_BUILD_TYPE="$BT" >/dev/null
  fi
  ninja -C "$ROOT/build/$t"
  ninja -C "$ROOT/build/$t" -t cleandead >/dev/null # outputs of targets that no longer exist (a removed sample)
  # a cart no target packs any more (a removed sample, a GL_ONLY app in the
  # dual build) must not linger in carts/: tools would still find and run it
  if [ -f "$ROOT/build/$t/carts.txt" ] && [ -d "$ROOT/build/$t/carts" ]; then
    for f in "$ROOT/build/$t/carts/"*.wasc; do
      [ -e "$f" ] || continue
      n="$(basename "$f" .wasc)"
      grep -qxF "$n" "$ROOT/build/$t/carts.txt" || { echo "removing stale cart $n.wasc ($t)"; rm -f "$f"; }
    done
  fi
  build_unlock "$ROOT/build/$t"
done
