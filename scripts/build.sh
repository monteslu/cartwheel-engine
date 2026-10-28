#!/usr/bin/env bash
# Build the native tree (build/native) and the cart tree (build/cart-wgpu):
# every app as a dual GL + WebGPU cart (cartwheel/gpu.h) that renders with
# WebGPU on a WebGPU host and with GL on a GL host, so one cart tests both.
# There is no GL-only cart tree: test GL by running the dual cart on a GL
# host (romdev wgpu:false, tools/run-cart.mjs --dual-gl, wasmcart-libretro).
#   BUILD_TYPE=Debug scripts/build.sh    TARGETS="native" scripts/build.sh
#   TARGETS="cart-wgpu" scripts/build.sh
# NINJA_JOBS=<n> caps ninja's parallel jobs (default: ninja's own).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BT="${BUILD_TYPE:-Release}"
# one build of a tree at a time (scripts/lib/build-lock.sh): another waits
source "$ROOT/scripts/lib/build-lock.sh"
"$ROOT/scripts/lib/check-deps.sh" "$ROOT" || exit 1 # deps/ older than the pins: stop with the fix named, not a compile error
JOBS=(); [ -n "${NINJA_JOBS:-}" ] && JOBS=(-j "$NINJA_JOBS")
for t in ${TARGETS:-native cart-wgpu}; do
  case "$t" in
    native|cart-wgpu) ;;
    cart) echo "scripts/build.sh: there is no GL-only cart tree (build/cart) any more: build cart-wgpu, the dual cart, and run it on a GL host for GL" >&2; exit 2 ;;
    *) echo "scripts/build.sh: unknown target $t (native, cart-wgpu)" >&2; exit 2 ;;
  esac
  if [ "$t" = cart-wgpu ] && ! { [ -d "$ROOT/deps/wasmcart-wgpu/wgpu-wasi" ] && [ -d "$ROOT/deps/emdawnwebgpu/webgpu" ]; }; then
    echo "scripts/build.sh: the cart tree needs deps/wasmcart-wgpu and deps/emdawnwebgpu: run scripts/fetch-deps.sh" >&2; exit 1
  fi
  build_lock "$ROOT/build/$t"
  if [ "$t" = cart-wgpu ]; then
    cmake -S "$ROOT" -B "$ROOT/build/cart-wgpu" -G Ninja -DCMAKE_BUILD_TYPE="$BT" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/cart.cmake" \
      -DCW_WGPU=ON -DCW_CWPACK="$ROOT/build/native/tools/cwpack/cwpack" >/dev/null
  else
    cmake -S "$ROOT" -B "$ROOT/build/native" -G Ninja -DCMAKE_BUILD_TYPE="$BT" >/dev/null
  fi
  ninja -C "$ROOT/build/$t" "${JOBS[@]}"
  ninja -C "$ROOT/build/$t" -t cleandead >/dev/null # outputs of targets that no longer exist (a removed sample)
  # a cart no target packs any more (a removed sample, a GL_ONLY app) must
  # not linger in carts/: tools would still find and run it
  if [ -f "$ROOT/build/$t/carts.txt" ] && [ -d "$ROOT/build/$t/carts" ]; then
    for f in "$ROOT/build/$t/carts/"*.wasc; do
      [ -e "$f" ] || continue
      n="$(basename "$f" .wasc)"
      grep -qxF "$n" "$ROOT/build/$t/carts.txt" || { echo "removing stale cart $n.wasc ($t)"; rm -f "$f"; }
    done
  fi
  build_unlock "$ROOT/build/$t"
done
