# Cart toolchain: wasi-sdk, wasm32-wasip1-threads + simd128 (the target
# wasmcart-libs builds for). deps/wasi-sdk comes from scripts/fetch-deps.sh.
set(WASI_SDK_PREFIX ${CMAKE_CURRENT_LIST_DIR}/../deps/wasi-sdk)
get_filename_component(WASI_SDK_PREFIX ${WASI_SDK_PREFIX} REALPATH)
include(${WASI_SDK_PREFIX}/share/cmake/wasi-sdk-pthread.cmake)
set(CMAKE_C_COMPILER_TARGET wasm32-wasip1-threads)
set(CMAKE_CXX_COMPILER_TARGET wasm32-wasip1-threads)
