# Shared by the export toolchain files (tools/cwexport). The toolchains and
# runtimes come from deps/export/ (scripts/fetch-deps.sh --export).
get_filename_component(CW_EXPORT_DEPS ${CMAKE_CURRENT_LIST_DIR}/../../deps/export REALPATH)
set(CW_LLVM ${CW_EXPORT_DEPS}/llvm-mingw/bin)
if(NOT EXISTS ${CW_LLVM}/clang)
  message(FATAL_ERROR "deps/export/llvm-mingw is missing: run scripts/fetch-deps.sh --export")
endif()
# exports build SDL2 from deps/sdl2 and link it statically
set(CW_SDL2_FROM_SOURCE ON CACHE BOOL "" FORCE)
# cwpack is a host tool: the native tree's build of it packs for every target
get_filename_component(_cw_root ${CMAKE_CURRENT_LIST_DIR}/../.. REALPATH)
set(CW_CWPACK ${_cw_root}/build/native/tools/cwpack/cwpack CACHE FILEPATH "host cwpack")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
