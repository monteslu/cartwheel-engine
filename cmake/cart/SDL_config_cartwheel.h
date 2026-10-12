/* SDL2 config for cartwheel carts (wasi-sdk, wasm32-wasip1-threads):
 * wasmcart-sdl2's config, minus what WASI does not have. */
#include "SDL_config_wasmcart.h"
#undef HAVE_SIGNAL_H        /* no signals in WASI */
#undef HAVE_SIGACTION
#undef SDL_LOADSO_DLOPEN    /* no dlopen in WASI */
#define SDL_LOADSO_DUMMY 1
