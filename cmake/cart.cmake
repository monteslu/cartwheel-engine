# Cart toolchain: wasi-sdk, wasm32-wasip1-threads + simd128 (the target
# wasmcart-libs builds for). deps/wasi-sdk comes from scripts/fetch-deps.sh.
#
# What wasi-sdk's share/cmake/wasi-sdk-pthread.cmake sets, with its flags as
# CMAKE_*_FLAGS_INIT instead of appended to CMAKE_*_FLAGS: CMake reads a
# toolchain file more than once on the first configure, so the appending form
# put "-pthread -pthread" into the first configure's flags and "-pthread" into
# every later one, and the first rebuild after a clean configure recompiled
# every cart object (the whole engine in an engine checkout). The compiler
# sees the same options either way.
set(WASI_SDK_PREFIX ${CMAKE_CURRENT_LIST_DIR}/../deps/wasi-sdk)
get_filename_component(WASI_SDK_PREFIX ${WASI_SDK_PREFIX} REALPATH)

set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR wasm32)
set(CMAKE_C_FLAGS_INIT "-pthread")
set(CMAKE_CXX_FLAGS_INIT "-pthread")
# wasi-threads requires --import-memory; wasi requires --export-memory
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--import-memory -Wl,--export-memory")

if(WIN32)
  set(WASI_HOST_EXE_SUFFIX ".exe")
else()
  set(WASI_HOST_EXE_SUFFIX "")
endif()
set(CMAKE_C_COMPILER ${WASI_SDK_PREFIX}/bin/clang${WASI_HOST_EXE_SUFFIX})
set(CMAKE_CXX_COMPILER ${WASI_SDK_PREFIX}/bin/clang++${WASI_HOST_EXE_SUFFIX})
set(CMAKE_ASM_COMPILER ${WASI_SDK_PREFIX}/bin/clang${WASI_HOST_EXE_SUFFIX})
set(CMAKE_AR ${WASI_SDK_PREFIX}/bin/llvm-ar${WASI_HOST_EXE_SUFFIX})
set(CMAKE_RANLIB ${WASI_SDK_PREFIX}/bin/llvm-ranlib${WASI_HOST_EXE_SUFFIX})
set(CMAKE_C_COMPILER_TARGET wasm32-wasip1-threads)
set(CMAKE_CXX_COMPILER_TARGET wasm32-wasip1-threads)
set(CMAKE_ASM_COMPILER_TARGET wasm32-wasi-threads) # as wasi-sdk sets it

# Don't look in the sysroot for executables to run during the build
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# Only look in the sysroot (not in the host paths) for the rest
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
