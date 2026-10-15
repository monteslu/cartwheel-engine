# Target-wide settings every cartwheel build shares: the determinism flags,
# the cart (wasm32-wasip1-threads) code generation, where cwpack and wasm-opt
# come from. Included by the engine's CMakeLists.txt and by an installed SDK's
# cartwheelConfig.cmake (CW_ROOT: the checkout or the SDK prefix).
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
  set(CW_CART ON)
  # cwpack is a native tool: the cart tree runs the native tree's build of it
  # (scripts/build.sh builds native first)
  set(CW_CWPACK ${CW_ROOT}/build/native/tools/cwpack/cwpack CACHE FILEPATH "native cwpack for cart packs")
else()
  set(CW_CART OFF)
endif()
# Exports (tools/cwexport, cmake/export/*.cmake) build SDL2 from deps/sdl2;
# the dev build uses the system's
option(CW_SDL2_FROM_SOURCE "build SDL2 from deps/sdl2 and link it statically" OFF)
# a cross build (an export) runs the native tree's cwpack
if(CMAKE_CROSSCOMPILING AND NOT CW_CART)
  set(CW_CWPACK ${CW_ROOT}/build/native/tools/cwpack/cwpack CACHE FILEPATH "host cwpack")
endif()

# Flags that are part of the determinism contract on every target: no FMA
# contraction (Box3D requires it; the engine follows), no fast-math.
add_compile_options(-ffp-contract=off -fno-fast-math)
if(CW_CART)
  # -fno-vectorize: wasi-sdk's clang 22 loop vectorizer miscompiles under
  # -msimd128 at -O3 (a 3-trip fmaxf reduction drops an iteration; a game's
  # texture hash diverged from native), so a cart could silently compute
  # different state from native. Explicit wasm_simd128 code and the SLP
  # vectorizer are unaffected. Repro: tests/repro/wasm-loop-vectorize.c.
  add_compile_options(-msimd128 -fno-vectorize -pthread)
  add_link_options(-pthread)
  # ThinLTO: three.c, the physics libraries and the engine inline across their
  # archives into each cart (the determinism gates hold every host to native)
  option(CW_CART_LTO "carts: ThinLTO across the engine and its libraries" ON)
  if(CW_CART_LTO)
    add_compile_options(-flto=thin)
    # (not -flto at the link: clang would then link wasi-sdk's LTO libc, whose
    # __wasilibc_use_busy_futex fails to link; wasm-ld does the LTO of the bitcode objects itself)
    add_link_options(-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/lto-cache)
  endif()
  # Binaryen's wasm-opt over each linked cart (scripts/deps.lock: binaryen)
  set(CW_WASM_OPT ${CW_ROOT}/deps/binaryen/bin/wasm-opt CACHE FILEPATH "wasm-opt for carts (empty: none)")
  # A cart's link (ThinLTO's backends) and its wasm-opt each use several cores:
  # at most two carts link at once, and wasm-opt gets 4 threads (BINARYEN_CORES),
  # so a full build stays near the machine's core count instead of ninja's -j
  # times every core
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS cw_cart_link=2)
  set(CW_CART_LINK_JOBS 4 CACHE STRING "threads a cart link (ThinLTO) and its wasm-opt may each use")
endif()
