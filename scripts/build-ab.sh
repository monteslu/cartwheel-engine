#!/usr/bin/env bash
# A second build tree for A/B perf runs (tools/perf-frame.mjs --build <dir>):
# the same configuration as scripts/build.sh, in <dir>, optionally against
# another three.c checkout (CW_THREE_DIR).
#   CW_THREE_DIR=../three.c-fix scripts/build-ab.sh build-b [native cart-wgpu]
# Only the games' autopilot builds (APPS overrides; the full tree is ~4 GB a copy).
# Targets in order: native first (its cwpack packs the carts).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTDIR="$ROOT/${1:?usage: build-ab.sh <dir> [targets]}"
shift
BT="${BUILD_TYPE:-Release}"
for t in ${@:-native cart-wgpu}; do
  case "$t" in
    cart-wgpu) cmake -S "$ROOT" -B "$OUTDIR/cart-wgpu" -G Ninja -DCMAKE_BUILD_TYPE="$BT" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/cart.cmake" \
      -DCW_WGPU=ON -DCW_CWPACK="$OUTDIR/native/tools/cwpack/cwpack" >/dev/null ;;
    *) cmake -S "$ROOT" -B "$OUTDIR/native" -G Ninja -DCMAKE_BUILD_TYPE="$BT" >/dev/null ;;
  esac
  # (the native tree builds cwpack too: the carts' packs use this tree's, so no build/ is needed)
  ninja -C "$OUTDIR/$t" $([ "$t" = native ] && echo tools/cwpack/cwpack) $(for a in ${APPS:-platformer3d_auto action2d_auto}; do echo "$a"; done)
done
