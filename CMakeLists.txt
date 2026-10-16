# cartwheel: one build description, two targets.
#
#   native:  cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cart:    cmake -S . -B build/cart -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/cart.cmake
#
# scripts/build.sh does both. Dependencies come from deps/ (scripts/fetch-deps.sh).
cmake_minimum_required(VERSION 3.20)
project(cartwheel C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
# C++17 for the C++ dependencies (basisu, meshoptimizer): what GCC and LLVM clang
# default to, set for Apple clang, which defaults to older
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(DEPS ${CMAKE_SOURCE_DIR}/deps)
# the engine's files (cmake/app.cmake): this checkout, or an installed SDK's prefix
set(CW_ROOT ${CMAKE_SOURCE_DIR})

include(cmake/flags.cmake)

# CW_SANITIZE=address,undefined (native): the sanitizer build scripts/sanitize.sh runs
set(CW_SANITIZE "" CACHE STRING "native sanitizers, e.g. address,undefined or thread")
if(CW_SANITIZE AND NOT CW_CART)
  add_compile_options(-fsanitize=${CW_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=${CW_SANITIZE})
endif()

# Project builds: CW_PROJECT_DIR names a cartwheel project (a directory
# whose CMakeLists.txt calls cw_add_app) built with this engine tree, under any
# toolchain; CW_ENGINE_ONLY skips the repository's samples, games, editor and
# tests. tools/cwproject.mjs and the editor configure projects this way.
set(CW_PROJECT_DIR "" CACHE PATH "a cartwheel project to build with the engine")
option(CW_ENGINE_ONLY "build the engine and its tools only (project builds)" OFF)

include(cmake/deps.cmake)

# ── the engine ─────────────────────────────────────────────────────────
file(GLOB_RECURSE CW_CORE_SRCS CONFIGURE_DEPENDS src/core/*.c src/jobs/*.c src/input/*.c src/physics/*.c
     src/audio/*.c src/render/*.c src/render2d/*.c src/scene/*.c src/fx/*.c src/visibility/*.c src/terrain/*.c src/anim/*.c src/ui/*.c src/l10n/*.c src/resource/*.c src/debug/*.c src/remote/*.c
     src/net/*.c src/shell/*.c src/nav/*.c src/noise/*.c src/video/*.c src/gi/*.c src/water/*.c)
# FastNoiseLite hashes with signed int overflow: -fwrapv defines it as the wrap it expects
set_source_files_properties(src/noise/noise.c PROPERTIES COMPILE_OPTIONS "-fwrapv")
list(APPEND CW_CORE_SRCS src/resource/basis_glue.cpp) # the KTX2 transcoder's C side
set_source_files_properties(src/resource/basis_glue.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti")
# navigation: Recast, Detour and DetourTileCache compiled into the engine (cmake/deps.cmake: CW_RECAST_SRCS)
# with the navmesh shim; their math and allocator hooks are src/nav/recast_hooks.h, which calls the engine
list(APPEND CW_CORE_SRCS src/nav/navmesh.cpp ${CW_RECAST_SRCS})
set_source_files_properties(src/nav/navmesh.cpp ${CW_RECAST_SRCS} PROPERTIES
  COMPILE_OPTIONS "-fno-exceptions;-fno-rtti" COMPILE_DEFINITIONS "${CW_RECAST_DEFS}")
set_source_files_properties(${CW_RECAST_SRCS} PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti;-O2;-w")
set(CW_PLATFORM_SRCS src/platform/sdl_common.c src/platform/gpu.c src/platform/pixel.c)
if(CW_CART)
  list(APPEND CW_PLATFORM_SRCS src/platform/cart.c)
else()
  list(APPEND CW_PLATFORM_SRCS src/platform/native.c src/platform/input_script.c)
endif()
# src/render/programs/ holds the engine's generated program table, built as its own library (cmake/programs.cmake)
list(FILTER CW_CORE_SRCS EXCLUDE REGEX "/src/render/programs/")
add_library(cartwheel STATIC ${CW_CORE_SRCS} ${CW_PLATFORM_SRCS})
target_include_directories(cartwheel PUBLIC include ${CW_THREE_DIR}/include PRIVATE src src/resource ${DEPS}/stb ${CW_THREE_DIR}/third_party
  ${CW_RECAST_INCLUDES})
target_compile_options(cartwheel PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# the profiler's zones (cartwheel/profile.h): OFF compiles every CW_PROFILE_* macro out, the engine's and the game's
option(CW_PROFILER "compile the profiler's zones in (off at run time until asked for)" ON)
if(NOT CW_PROFILER)
  target_compile_definitions(cartwheel PUBLIC CW_PROFILE_ENABLED=0 PRIVATE CW_PROFILER_OFF=1)
endif()
# #embed (src/ui/fonts.c) needs clang 19+; an older compiler (the Android
# NDK's) includes the same bytes generated as initializer lists
include(CheckCSourceCompiles)
file(WRITE ${CMAKE_BINARY_DIR}/embed-check/x.bin "x")
set(CMAKE_REQUIRED_FLAGS "-I${CMAKE_BINARY_DIR}/embed-check -Wno-c23-extensions")
check_c_source_compiles("static const unsigned char b[] = {\n#embed \"x.bin\"\n};\nint main(void) { return b[0] - 'x'; }" CW_HAVE_EMBED)
unset(CMAKE_REQUIRED_FLAGS)
if(NOT CW_HAVE_EMBED)
  # the UI fonts (src/ui/fonts.c) and the built-in HRTF table (src/audio/hrtf.c)
  foreach(_embed ui/fonts/FiraSans-Regular.ttf ui/fonts/FiraMono-Regular.ttf audio/hrtf/kemar.cwhrtf)
    get_filename_component(_name ${_embed} NAME)
    set(_inc ${CMAKE_BINARY_DIR}/embed/${_name}.inc)
    set(_src ${CMAKE_SOURCE_DIR}/src/${_embed})
    if(NOT EXISTS ${_inc} OR ${_src} IS_NEWER_THAN ${_inc})
      file(READ ${_src} _hex HEX)
      string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
      file(WRITE ${_inc} "${_hex}\n")
    endif()
  endforeach()
  target_include_directories(cartwheel PRIVATE ${CMAKE_BINARY_DIR}/embed)
  target_compile_definitions(cartwheel PRIVATE CW_EMBED_INC=1)
endif()
# an exported game picks WebGPU when CW_GPU_AUTO (src/platform/gpu.c)
if(CW_EXPORT_TARGET AND CW_WGPU AND NOT CW_CART)
  target_compile_definitions(cartwheel PRIVATE CW_GPU_AUTO_WEBGPU=1)
endif()
target_link_libraries(cartwheel PUBLIC cw_sdl2 three box2d box3d cw_miniaudio cw_stb_vorbis cw_opus cw_ogg cw_xmp
  meshoptimizer cw_basisu_transcoder cw_kbts cw_plmpeg cw_lz4 cw_wgpu)
# zstd.h of the zstd decoder cw_basisu_transcoder links (src/resource/compress.c)
target_include_directories(cartwheel PRIVATE ${CW_BASISU}/zstd)
if(CW_CART)
  target_include_directories(cartwheel PUBLIC ${DEPS}/wasmcart/include)
else()
  target_link_libraries(cartwheel PUBLIC cw_gles)
  if(WIN32)
    target_link_libraries(cartwheel PUBLIC ws2_32 pthread) # the debug channel's socket; winpthreads
  elseif(NOT ANDROID)
    target_link_libraries(cartwheel PUBLIC m pthread)
  else()
    target_link_libraries(cartwheel PUBLIC m log android)
  endif()
endif()

include(cmake/programs.cmake)
# the engine's own table (impostor baking and the like), registered by src/render/programs.c
cw_program_table(${CMAKE_SOURCE_DIR}/src/render/programs/cw_engine.txt _t)
target_link_libraries(cartwheel PUBLIC cwprog_cw_engine)

# main() for native apps, in its own library so the hot-reload host (which has
# its own main) can link the whole engine without it
if(NOT CW_CART)
  add_library(cartwheel_main STATIC src/platform/native_entry.c)
  if((WIN32 OR IOS) AND CW_SDL2_FROM_SOURCE)
    target_link_libraries(cartwheel_main PUBLIC SDL2::SDL2main) # WinMain / UIApplicationMain -> SDL_main
  endif()
  target_link_libraries(cartwheel_main PUBLIC cartwheel)
endif()
if(NOT CW_CART AND NOT CMAKE_CROSSCOMPILING)

  # cwhot: runs an app built as a module and swaps in rebuilt code. It links
  # every engine archive whole and exports its symbols, so modules link
  # against the one engine in the host.
  add_executable(cwhot tools/cwhot/main.c)
  set_target_properties(cwhot PROPERTIES ENABLE_EXPORTS ON)
  target_link_libraries(cwhot PRIVATE "-Wl,--whole-archive" cartwheel three box2d box3d cw_miniaudio "-Wl,--no-whole-archive"
    cw_sdl2 GLESv2 m pthread dl)
  target_include_directories(cwhot PRIVATE ${CW_THREE_DIR}/src) # gen/programs.h: a module's program tables
  target_link_libraries(cwhot PRIVATE cw_gles) # (three.c's backend.h includes the GLES headers)
  # cwbake: bakes a scene's lightmaps and probe grid offline (cartwheel/lightmap.h)
  add_executable(cwbake tools/cwbake/main.c)
  target_link_libraries(cwbake PRIVATE cartwheel)
  # cwcsg: bakes CSG shapes into meshes with Manifold (cartwheel/csg.h); C++, never in a game.
  # Only when deps/manifold is fetched (an SDK build that fetches a subset goes without the tool)
  if(EXISTS ${DEPS}/manifold/src/manifold.cpp)
    include(tools/cwcsg/cwcsg.cmake)
  endif()
  # docs/nodes.json: node types and properties as data (tools/gen-api.mjs runs it)
  add_executable(gen_nodes tools/gen_nodes/main.c)
  target_link_libraries(gen_nodes PRIVATE cartwheel)
  set(CW_HOT_INCLUDES ${CMAKE_SOURCE_DIR}/include ${CW_THREE_DIR}/include ${DEPS}/box2d/include ${DEPS}/box3d/include ${DEPS}/miniaudio)
endif()

include(cmake/plugins.cmake)

include(cmake/app.cmake)

if(NOT CW_CART)
  add_subdirectory(tools/cwpack)
endif()
include(tools/cwpack/importers.cmake) # cwpack's importers as a library (the editor imports in process)

if(NOT CW_ENGINE_ONLY)
  enable_testing()
  add_subdirectory(samples)
  add_subdirectory(games)
  add_subdirectory(editor)
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
  endif()
endif()
# a project with plugins gets the editor LIBRARY too (its <app>_editor, below)
# (not in exports for other platforms: they build only the game)
if(CW_PROJECT_DIR AND CW_ENGINE_ONLY AND IS_DIRECTORY ${CW_PROJECT_DIR}/plugins AND (CW_CART OR NOT CMAKE_CROSSCOMPILING))
  set(CWED_LIBRARY_ONLY ON)
  add_subdirectory(editor)
endif()
if(CW_PROJECT_DIR)
  if(NOT EXISTS ${CW_PROJECT_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "CW_PROJECT_DIR=${CW_PROJECT_DIR}: no CMakeLists.txt there (a project calls cw_add_app in it)")
  endif()
  add_subdirectory(${CW_PROJECT_DIR} project)
endif()
# <app>_editor for every app built with a project's plugins (cmake/plugins.cmake)
cw__make_project_editors()
# the carts this build packs (scripts/build.sh removes any other .wasc from
# carts/; tests/wgpu-carts.mjs checks these)
cw__write_carts_txt()
# prebuilt SDK description (CW_SDK=ON; scripts/build-sdk.sh)
include(cmake/sdk.cmake)
