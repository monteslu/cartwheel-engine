# Shared by the export toolchain files (tools/cwexport). The toolchains and
# runtimes come from deps/export/ (scripts/fetch-deps.sh --export).
# CW_EXPORT_DEPS (a -D) names another deps/export, e.g. a host SDK's when this
# file is a target SDK's (docs/sdk.md)
if(NOT CW_EXPORT_DEPS)
  get_filename_component(CW_EXPORT_DEPS ${CMAKE_CURRENT_LIST_DIR}/../../deps/export REALPATH)
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CW_EXPORT_DEPS) # compiler checks load this file too
# llvm-mingw's clang + lld cross-compile every target but Apple's and
# Android's (macos-arm64.cmake builds on a Mac with Xcode's clang)
set(CW_LLVM ${CW_EXPORT_DEPS}/llvm-mingw/bin)
if(NOT CW_EXPORT_NATIVE_APPLE AND NOT EXISTS ${CW_LLVM}/clang)
  message(FATAL_ERROR "deps/export/llvm-mingw is missing: run scripts/fetch-deps.sh --export")
endif()
# exports build SDL2 from deps/sdl2 and link it statically
set(CW_SDL2_FROM_SOURCE ON CACHE BOOL "" FORCE)
# cwpack is a host tool: the native tree's build of it packs for every target
get_filename_component(_cw_root ${CMAKE_CURRENT_LIST_DIR}/../.. REALPATH)
if(EXISTS ${_cw_root}/CMakeLists.txt) # the engine checkout (an SDK's config finds its own)
  set(CW_CWPACK ${_cw_root}/build/native/tools/cwpack/cwpack CACHE FILEPATH "host cwpack")
endif()
# native WebGPU: the target's native-dawn (scripts/deps.lock export/native-dawn-<target>),
# set by each target's file; WebGPU is on when it is fetched (cmake/deps.cmake)
macro(cw_export_dawn name)
  if(EXISTS ${CW_EXPORT_DEPS}/native-dawn-${name}/lib/cmake/Dawn)
    set(CW_NATIVE_DAWN ${CW_EXPORT_DEPS}/native-dawn-${name} CACHE PATH "" FORCE)
  endif()
endmacro()
if(CW_EXPORT_NATIVE_APPLE) # not a cross build: the Mac's own SDK and frameworks
  return()
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
