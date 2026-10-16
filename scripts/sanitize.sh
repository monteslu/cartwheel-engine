#!/usr/bin/env bash
# Native sanitizer runs: every unit suite and sample for a few frames under
# ASan + UBSan (and the job system under TSan with SAN=thread).
#   scripts/sanitize.sh            SAN=thread scripts/sanitize.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SAN="${SAN:-address,undefined}"
B="$ROOT/build/san-${SAN//,/-}"
source "$ROOT/scripts/lib/build-lock.sh"   # one build of a tree at a time
build_lock "$B"
cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCW_SANITIZE="$SAN" >/dev/null
ninja -C "$B" >/dev/null || { echo "build failed"; exit 1; }
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 TSAN_OPTIONS="halt_on_error=1 suppressions=$ROOT/scripts/tsan.supp"
# TSan audits the engine's threads: run without the desktop's uninstrumented display
# and audio libraries (X11/xcb/dbus/pulse), whose internal locking it cannot see.
# No suite opens a window on the desktop (tools/with-gpu-suite.mjs also drops DISPLAY / WAYLAND_DISPLAY / XAUTHORITY).
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}"
case "$SAN" in *thread*) export SDL_AUDIODRIVER=dummy ;; esac
fail=0
for exe in "$B"/tests/unit/unit_* "$B"/samples/*; do
  [ -x "$exe" ] && [ -f "$exe" ] || continue
  case "$exe" in *.so|*.assets|*hot*) continue ;; esac
  name=$(basename "$exe")
  frames=30; [ "$name" = physics_determinism ] && frames=400
  assets="$ROOT/samples/$name/assets"; [ -d "$exe.assets" ] && assets="$exe.assets" # the build's asset root, as run-cart uses
  # A hang is a failure, not a stalled suite. SIGTERM alone cannot end one: SDL
  # turns SIGTERM into a quit event that a stuck frame never polls (and TSan
  # defers signals until the thread makes an intercepted call), so KILL follows.
  out=$(CW_FRAMES=$frames CW_ASSETS="$assets" timeout -k 10 900 "$exe" 2>&1); rc=$?
  if [ $rc -eq 0 ]; then
    echo "ok   $name"
  elif [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
    echo "FAIL $name (timed out after 900 s)"; fail=1
  else
    echo "FAIL $name"; echo "$out" | grep -E "ERROR|runtime error|SUMMARY|#[0-9] " | head -20; fail=1
  fi
done
exit $fail
