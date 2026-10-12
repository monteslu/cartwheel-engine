#!/usr/bin/env bash
# SDL2 for carts: upstream SDL2 (deps/sdl2) + wasmcart-sdl2's backends
# (deps/wasmcart-sdl2/sdl2_wc), compiled with wasi-sdk for
# wasm32-wasip1-threads + simd128, the same target wasmcart-libs uses.
#
# wasmcart-sdl2's own build script uses Emscripten; Emscripten's standalone
# wasm cannot use wasi-threads, so cartwheel builds the same sources the same
# way with wasi-sdk instead. The source list and the backend-registration
# patches mirror deps/wasmcart-sdl2/sdl2_wc/build_sdl2_wc.sh; the SDL tree is
# copied before patching, so deps/ stays pristine.
#
# Output: build/sdl2-cart/libSDL2_wc.a
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WASI="$ROOT/deps/wasi-sdk"
WC="$ROOT/deps/wasmcart-sdl2/sdl2_wc"
OUT="$ROOT/build/sdl2-cart"
SRC="$OUT/src"
CC="$WASI/bin/clang"
AR="$WASI/bin/llvm-ar"

if [ -f "$OUT/libSDL2_wc.a" ] && [ "${FORCE:-0}" != 1 ]; then echo "sdl2 (cart): up to date"; exit 0; fi
rm -rf "$OUT" && mkdir -p "$OUT/obj"
cp -r "$ROOT/deps/sdl2" "$SRC"
# A patch the source tree already carries (deps/sdl2 checked out with it
# applied) is skipped: its reverse applies cleanly, so applying it again would
# fail as "Reversed (or previously applied)".
for p in "$ROOT"/patches/sdl2/*.patch; do
  if patch -s -d "$SRC" -p1 -R --dry-run -f < "$p" >/dev/null 2>&1; then
    echo "sdl2 (cart): $(basename "$p") already applied"
  else
    patch -s -d "$SRC" -p1 < "$p"
  fi
done

# Register the wasmcart backends (the same edits build_sdl2_wc.sh makes).
sed -i '/extern VideoBootStrap Emscripten_bootstrap;/a extern VideoBootStrap WASMCART_bootstrap;' "$SRC/src/video/SDL_video.c" "$SRC/src/video/SDL_sysvideo.h"
sed -i '/#ifdef SDL_VIDEO_DRIVER_DUMMY/i #ifdef SDL_VIDEO_DRIVER_WASMCART\n    \&WASMCART_bootstrap,\n#endif' "$SRC/src/video/SDL_video.c"
sed -i '/extern AudioBootStrap EMSCRIPTENAUDIO_bootstrap;/a extern AudioBootStrap WASMCARTAUDIO_bootstrap;' "$SRC/src/audio/SDL_audio.c" "$SRC/src/audio/SDL_sysaudio.h"
sed -i '/#ifdef SDL_AUDIO_DRIVER_DUMMY/i #ifdef SDL_AUDIO_DRIVER_WASMCART\n    \&WASMCARTAUDIO_bootstrap,\n#endif' "$SRC/src/audio/SDL_audio.c"
sed -i '/^static SDL_JoystickDriver \*SDL_joystick_drivers\[\] = {/a #ifdef SDL_JOYSTICK_WASMCART\n    \&SDL_WASMCART_JoystickDriver,\n#endif' "$SRC/src/joystick/SDL_joystick.c"
sed -i '/extern SDL_JoystickDriver SDL_EMSCRIPTEN_JoystickDriver;/a extern SDL_JoystickDriver SDL_WASMCART_JoystickDriver;' "$SRC/src/joystick/SDL_sysjoystick.h"
mkdir -p "$SRC/src/video/wasmcart" "$SRC/src/audio/wasmcart" "$SRC/src/joystick/wasmcart"
cp "$WC/SDL_wasmcart_video.c" "$WC/SDL_wasmcart_video.h" "$SRC/src/video/wasmcart/"
cp "$WC/SDL_wasmcart_audio.c" "$WC/SDL_wasmcartaudio.h" "$SRC/src/audio/wasmcart/"
cp "$WC/SDL_wasmcart_joystick.c" "$WC/SDL_wasmcart_joystick.h" "$SRC/src/joystick/wasmcart/"

# Source list from build_sdl2_wc.sh (renderers for other platforms compile to
# nothing under this config, as they do there).
SRCS=$(sed -n '/^SDL2_CORE_SRCS="/,/^"/p' "$WC/build_sdl2_wc.sh" | sed '1d;$d')
SRCS=$(echo "$SRCS" | sed "s|loadso/dlopen/SDL_sysloadso.c|loadso/dummy/SDL_sysloadso.c|")
SRCS="$SRCS video/wasmcart/SDL_wasmcart_video.c audio/wasmcart/SDL_wasmcart_audio.c joystick/wasmcart/SDL_wasmcart_joystick.c"

CFLAGS="--target=wasm32-wasip1-threads --sysroot=$WASI/share/wasi-sysroot -pthread -msimd128 -O2 -DNDEBUG
  -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_MMAN
  -fwrapv-pointer -Wno-everything
  -I$SRC/include -I$SRC/src -I$ROOT/deps/wasmcart/include -I$WC -I$SRC/src/video/khronos -include $ROOT/cmake/cart/SDL_config_cartwheel.h"

pids=(); fail=0
for s in $SRCS; do
  o="$OUT/obj/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
  $CC $CFLAGS -c "$SRC/src/$s" -o "$o" &
  pids+=($!)
  while [ "$(jobs -r | wc -l)" -ge "$(nproc)" ]; do sleep 0.05; done
done
for p in "${pids[@]}"; do wait "$p" || fail=1; done
[ $fail = 0 ] || { echo "sdl2 (cart): a compile failed" >&2; exit 1; }
$AR rcs "$OUT/libSDL2_wc.a" "$OUT"/obj/*.o
echo "sdl2 (cart): $OUT/libSDL2_wc.a"
