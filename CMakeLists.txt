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
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(DEPS ${CMAKE_SOURCE_DIR}/deps)

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
  set(CW_CART ON)
  # cwpack is a native tool: the cart tree runs the native tree's build of it
  # (scripts/build.sh builds native first)
  set(CW_CWPACK ${CMAKE_SOURCE_DIR}/build/native/tools/cwpack/cwpack CACHE FILEPATH "native cwpack for cart packs")
else()
  set(CW_CART OFF)
endif()
# Exports (tools/cwexport, cmake/export/*.cmake) build SDL2 from deps/sdl2;
# the dev build uses the system's
option(CW_SDL2_FROM_SOURCE "build SDL2 from deps/sdl2 and link it statically" OFF)
# a cross build (an export) runs the native tree's cwpack
if(CMAKE_CROSSCOMPILING AND NOT CW_CART)
  set(CW_CWPACK ${CMAKE_SOURCE_DIR}/build/native/tools/cwpack/cwpack CACHE FILEPATH "host cwpack")
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
  set(CW_WASM_OPT ${CMAKE_SOURCE_DIR}/deps/binaryen/bin/wasm-opt CACHE FILEPATH "wasm-opt for carts (empty: none)")
  # A cart's link (ThinLTO's backends) and its wasm-opt each use several cores:
  # at most two carts link at once, and wasm-opt gets 4 threads (BINARYEN_CORES),
  # so a full build stays near the machine's core count instead of ninja's -j
  # times every core
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS cw_cart_link=2)
  set(CW_CART_LINK_JOBS 4 CACHE STRING "threads a cart link (ThinLTO) and its wasm-opt may each use")
endif()

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
     src/audio/*.c src/render/*.c src/render2d/*.c src/scene/*.c src/fx/*.c src/visibility/*.c src/anim/*.c src/ui/*.c src/l10n/*.c src/resource/*.c src/debug/*.c src/remote/*.c
     src/net/*.c)
list(APPEND CW_CORE_SRCS src/resource/basis_glue.cpp) # the KTX2 transcoder's C side
set_source_files_properties(src/resource/basis_glue.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti")
set(CW_PLATFORM_SRCS src/platform/sdl_common.c src/platform/gpu.c src/platform/pixel.c)
if(CW_CART)
  list(APPEND CW_PLATFORM_SRCS src/platform/cart.c)
else()
  list(APPEND CW_PLATFORM_SRCS src/platform/native.c src/platform/input_script.c)
endif()
# src/render/programs/ holds the engine's generated program table, built as its own library (cmake/programs.cmake)
list(FILTER CW_CORE_SRCS EXCLUDE REGEX "/src/render/programs/")
add_library(cartwheel STATIC ${CW_CORE_SRCS} ${CW_PLATFORM_SRCS})
target_include_directories(cartwheel PUBLIC include ${CW_THREE_DIR}/include PRIVATE src src/resource ${DEPS}/stb ${CW_THREE_DIR}/third_party)
target_compile_options(cartwheel PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# #embed (src/ui/fonts.c) needs clang 19+; an older compiler (the Android
# NDK's) includes the same bytes generated as initializer lists
include(CheckCSourceCompiles)
file(WRITE ${CMAKE_BINARY_DIR}/embed-check/x.bin "x")
set(CMAKE_REQUIRED_FLAGS "-I${CMAKE_BINARY_DIR}/embed-check -Wno-c23-extensions")
check_c_source_compiles("static const unsigned char b[] = {\n#embed \"x.bin\"\n};\nint main(void) { return b[0] - 'x'; }" CW_HAVE_EMBED)
unset(CMAKE_REQUIRED_FLAGS)
if(NOT CW_HAVE_EMBED)
  foreach(_font FiraSans-Regular FiraMono-Regular)
    set(_inc ${CMAKE_BINARY_DIR}/embed/${_font}.ttf.inc)
    set(_src ${CMAKE_SOURCE_DIR}/src/ui/fonts/${_font}.ttf)
    if(NOT EXISTS ${_inc} OR ${_src} IS_NEWER_THAN ${_inc})
      file(READ ${_src} _hex HEX)
      string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
      file(WRITE ${_inc} "${_hex}\n")
    endif()
  endforeach()
  target_include_directories(cartwheel PRIVATE ${CMAKE_BINARY_DIR}/embed)
  target_compile_definitions(cartwheel PRIVATE CW_EMBED_INC=1)
endif()
target_link_libraries(cartwheel PUBLIC cw_sdl2 three box2d box3d cw_miniaudio cw_stb_vorbis cw_opus cw_ogg cw_xmp
  meshoptimizer cw_basisu_transcoder cw_kbts cw_wgpu)
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
  # docs/nodes.json: node types and properties as data (tools/gen-api.mjs runs it)
  add_executable(gen_nodes tools/gen_nodes/main.c)
  target_link_libraries(gen_nodes PRIVATE cartwheel)
  set(CW_HOT_INCLUDES ${CMAKE_SOURCE_DIR}/include ${CW_THREE_DIR}/include ${DEPS}/box2d/include ${DEPS}/box3d/include ${DEPS}/miniaudio)
endif()

include(cmake/plugins.cmake)

# ── cw_add_app(name SOURCES ... [ASSETS dir] [NET domain...] [PLUGINS dir] [NO_PLUGINS] [PROGRAMS table.txt...] [GL_ONLY]) ──
# Native: an executable. Cart: <name>.wasm packed into <name>.wasc (with the
# asset directory) by wasmcart-pack, after the import audit. GL_ONLY: an app
# that renders with GL itself (not only through the engine's ported paths)
# gets no dual cart: in the dual build (CW_WGPU, build/cart-wgpu) its target
# is left out of the default build and packs no cart, so no cart claims
# WebGPU it cannot draw with; its cart is the GL build's (build/cart). Every
# cart a build packs is listed in <build>/carts.txt. NET lists the
# hosts the cart may dial with wc_peer_open (the manifest net.domains grant);
# without NET, a project's `net.domains` key in project.cwproj gives them.
# Plugins (cmake/plugins.cmake): the runtime plugins of PLUGINS <dir>,
# default <the calling directory>/plugins when it exists, are compiled in.
function(cw_add_app name)
  cmake_parse_arguments(A "NO_PLUGINS;QUIET_PLUGINS;NO_PROJECT_EDITOR;GL_ONLY" "ASSETS;PACK;PLUGINS" "SOURCES;NET;PROGRAMS" ${ARGN})
  # a project (project.cwproj beside the calling CMakeLists.txt) grants its
  # cart's peers with `net.domains = a.example.com, b.example.com` when the
  # call passes no NET (templates call cw_add_app without one)
  if(NOT A_NET AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/project.cwproj)
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/project.cwproj _cw_net REGEX "^[ \t]*net\\.domains[ \t]*=")
    if(_cw_net)
      string(REGEX REPLACE "^[ \t]*net\\.domains[ \t]*=[ \t]*\"?([^\"]*)\"?.*$" "\\1" _cw_net "${_cw_net}")
      string(REGEX REPLACE "[ \t]*,[ \t]*" ";" A_NET "${_cw_net}")
      string(STRIP "${A_NET}" A_NET)
    endif()
  endif()
  if(ANDROID)
    # SDLActivity loads the game as libmain.so (one per app directory) and
    # looks up SDL_main in it: the entry object goes in the library itself
    # (from the cartwheel_main archive nothing would pull it in)
    add_library(${name} SHARED ${A_SOURCES} ${CMAKE_SOURCE_DIR}/src/platform/native_entry.c)
    set_target_properties(${name} PROPERTIES OUTPUT_NAME main LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/android/${name})
  else()
    add_executable(${name} ${A_SOURCES})
    if(WIN32)
      set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE ON) # no console window (SDL2main supplies WinMain)
    endif()
    if(CW_APPLE_BUNDLE)
      # iOS exports (cmake/export/ios.cmake, built on a Mac): an app bundle
      # embedding ANGLE; CW_APPLE_INFO_PLIST from the exported Xcode template
      set(CW_APPLE_INFO_PLIST "" CACHE FILEPATH "Info.plist for app bundles")
      get_filename_component(_angle_dir ${CW_ANGLE_GLESV2} DIRECTORY)
      set_target_properties(${name} PROPERTIES MACOSX_BUNDLE ON
        XCODE_EMBED_FRAMEWORKS "${CW_ANGLE_GLESV2};${_angle_dir}/libEGL.xcframework"
        XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY ON
        XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks")
      if(CW_APPLE_INFO_PLIST)
        set_target_properties(${name} PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${CW_APPLE_INFO_PLIST})
      endif()
    endif()
  endif()
  # three.c program tables (cmake/programs.cmake): PROGRAMS, or programs/<name>.txt beside the call
  cw__app_programs(${name} ${A_PROGRAMS})
  if(A_GL_ONLY)
    # the engine asks cw__app_gl_only() (src/platform/gpu.c): a native run
    # asked for WebGPU refuses at startup instead of trapping in a GL call
    set(_glo ${CMAKE_BINARY_DIR}/gen/cw_gl_only.c)
    if(NOT EXISTS ${_glo})
      file(WRITE ${_glo} "/* GENERATED by cw_add_app(... GL_ONLY): the app draws with GL itself. */\n#include <stdbool.h>\nbool cw__app_gl_only(void);\nbool cw__app_gl_only(void) { return true; }\n")
    endif()
    target_sources(${name} PRIVATE ${_glo})
  endif()
  if(NOT A_PLUGINS AND IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/plugins)
    set(A_PLUGINS ${CMAKE_CURRENT_SOURCE_DIR}/plugins)
  endif()
  if(A_PLUGINS AND NOT A_NO_PLUGINS)
    # an app's own plugins dir (a project): also an <app>_editor with them compiled in
    set(_want_editor OFF)
    if(NOT A_NO_PROJECT_EDITOR AND A_PLUGINS STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
      set(_want_editor ON)
    endif()
    cw__app_plugins(${name} ${A_PLUGINS} "${A_QUIET_PLUGINS}" ${_want_editor})
  endif()
  if(CW_CART)
    target_link_libraries(${name} PRIVATE cartwheel)
    if(CW_WGPU) # a dual cart: the port's C++ half, and the exports its host glue calls back into
      target_link_libraries(${name} PRIVATE cw_wgpu_cart)
    endif()
  elseif(CMAKE_CROSSCOMPILING)
    target_link_libraries(${name} PRIVATE cartwheel_main) # exports: no hot-reload module
  else()
    target_link_libraries(${name} PRIVATE cartwheel_main)
    # the same sources as a module for cwhot (engine symbols come from the host)
    add_library(${name}_hot MODULE ${A_SOURCES})
    set_target_properties(${name}_hot PROPERTIES PREFIX "" EXCLUDE_FROM_ALL ON)
    target_include_directories(${name}_hot PRIVATE ${CW_HOT_INCLUDES})
    target_compile_definitions(${name}_hot PRIVATE CW_HOT_MODULE=1)
    # its program tables, which cwhot registers (cmake/programs.cmake)
    get_target_property(_pgen ${name} CW_PROGRAMS_GEN)
    if(_pgen)
      get_target_property(_plibs ${name} CW_PROGRAMS_LIBS)
      target_sources(${name}_hot PRIVATE ${_pgen})
      target_link_libraries(${name}_hot PRIVATE ${_plibs})
    endif()
  endif()
  set(_files "")
  if(A_PACK)
    # cwpack imports what the manifest reaches: natively <exe>.cwpak, in a
    # cart the asset directory wasmcart-pack ships (with the files list)
    set(_out ${CMAKE_BINARY_DIR}/packs/${name})
    set(_args --manifest ${A_PACK} --out ${_out} --name ${name} --engine-root ${CMAKE_SOURCE_DIR} -q)
    if(CW_CART)
      set(_cwpack ${CW_CWPACK})
      set(_deps ${A_PACK} ${CW_CWPACK})
    elseif(CMAKE_CROSSCOMPILING)
      set(_cwpack ${CW_CWPACK})
      set(_deps ${A_PACK} ${CW_CWPACK})
      list(APPEND _args --pak ${CMAKE_CURRENT_BINARY_DIR}/${name}.cwpak)
    else()
      set(_cwpack $<TARGET_FILE:cwpack>)
      set(_deps ${A_PACK} cwpack)
      list(APPEND _args --pak ${CMAKE_CURRENT_BINARY_DIR}/${name}.cwpak)
      string(REPLACE ";" " " _argstr "${_args}")
      # what cw_res_hotreload_watch needs to rerun this same import (dev builds)
      target_compile_definitions(${name} PRIVATE CW_PACK_CWPACK="$<TARGET_FILE:cwpack>" CW_PACK_ARGS="${_argstr}"
        CW_PACK_OUT="${_out}")
    endif()
    add_custom_command(OUTPUT ${_out}/stamp
      COMMAND ${_cwpack} ${_args} --depfile ${_out}.d --dep-target ${_out}/stamp
      COMMAND ${CMAKE_COMMAND} -E touch ${_out}/stamp
      DEPENDS ${_deps} DEPFILE ${_out}.d COMMENT "cwpack ${name}")
    add_custom_target(${name}_pack DEPENDS ${_out}/stamp)
    add_dependencies(${name} ${name}_pack)
    if(CW_CART)
      set(A_ASSETS ${_out}/assets)
      set(_files --files)
    endif()
  endif()
  if(NOT CW_CART AND A_ASSETS AND NOT CMAKE_CROSSCOMPILING)
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E create_symlink ${A_ASSETS} $<TARGET_FILE:${name}>.assets)
  endif()
  # what an export ships (tools/cwexport reads it): the binary, and the
  # assets as either the cwpack'd pack or a loose directory
  if(NOT CW_CART)
    set(_pak "")
    if(A_PACK)
      set(_pak ${CMAKE_CURRENT_BINARY_DIR}/${name}.cwpak)
    endif()
    # its three.c program tables (tools/programs.mjs adds an app's missing states to the first)
    get_target_property(_progs ${name} CW_PROGRAMS)
    if(NOT _progs)
      set(_progs "")
    endif()
    list(JOIN _progs "\", \"" _progs)
    if(_progs)
      set(_progs "\"${_progs}\"")
    endif()
    file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/apps/${name}.json CONTENT
"{\"name\": \"${name}\", \"binary\": \"$<TARGET_FILE:${name}>\", \"pak\": \"${_pak}\", \"assets\": \"${A_ASSETS}\", \"manifest\": \"${A_PACK}\", \"programs\": [${_progs}], \"dir\": \"${CMAKE_CURRENT_SOURCE_DIR}\"}\n")
  endif()
  if(CW_CART AND CW_WGPU AND A_GL_ONLY)
    set_target_properties(${name} PROPERTIES SUFFIX ".wasm" EXCLUDE_FROM_ALL ON)
    return()
  endif()
  if(CW_CART)
    set_property(GLOBAL APPEND PROPERTY CW_CARTS ${name})
    set_target_properties(${name} PROPERTIES SUFFIX ".wasm")
    # three.c's runtime builder runs QuickJS, which wants 4 MB of stack (cmake/deps.cmake)
    set(_stack 1048576)
    # three.c's program tables are static data: a game on the full table with
    # the WebGPU programs too outgrows 64 MB of initial memory
    set(_initial 134217728)
    if(CW_THREE_BUILDER)
      set(_stack 4194304)
    endif()
    set_property(TARGET ${name} PROPERTY JOB_POOL_LINK cw_cart_link)
    if(CW_CART_LTO)
      target_link_options(${name} PRIVATE -Wl,--thinlto-jobs=${CW_CART_LINK_JOBS})
    endif()
    target_link_options(${name} PRIVATE -mexec-model=reactor -Wl,--import-memory -Wl,--shared-memory
      -Wl,--max-memory=1073741824 -Wl,--initial-memory=${_initial} -Wl,-z,stack-size=${_stack}
      -Wl,--export=wc_get_info -Wl,--export=wc_init -Wl,--export=wc_render
      # no DWARF in a cart (wasi-libc's alone was about 1.4 MB); the name section stays, for profiles
      -Wl,--strip-debug
      # the host allocates in the cart's heap through malloc (wasmcart's web GL
      # host puts glGetString results there); without the export it wrote them
      # into the top 16 KB of memory, the live dlmalloc heap, and a later free
      # crashed in dlfree)
      -Wl,--export=malloc)
    # the asset dir is staged with its bundle manifest (cwfiles.txt: the file
    # list cw_vfs_list_dir reads, since a cart cannot list its bundle)
    set(_assets "")
    set(_stage_cmd "")
    if(A_ASSETS)
      set(_stage ${CMAKE_BINARY_DIR}/stage/${name})
      set(_assets --assets ${_stage})
      set(_stage_cmd COMMAND node ${CMAKE_SOURCE_DIR}/tools/stage-assets.mjs ${A_ASSETS} ${_stage})
    endif()
    set(_wgpu_check "")
    set(_wc_pack ${DEPS}/wasmcart/bin/wasmcart-pack.js)
    if(CW_WGPU)
      set(_wgpu_check --wgpu ${DEPS}/wasmcart-wgpu/src/wgpu/host.js)
      set(_wc_pack ${DEPS}/wasmcart-wgpu/bin/wasmcart-pack.js)
    endif()
    # NET <domain>...: the manifest grant wc_peer_open needs (net.domains)
    foreach(_d ${A_NET})
      list(APPEND _assets --ws ${_d})
    endforeach()
    # The .wasc is written after the link, so an asset-only change must relink
    # too, or the cart keeps the old assets (a regenerated sound reached the
    # pack but never the cart): the pack's stamp, or every loose asset file.
    if(A_PACK)
      set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS ${_out}/stamp)
    elseif(A_ASSETS)
      file(GLOB_RECURSE _loose CONFIGURE_DEPENDS ${A_ASSETS}/*)
      set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS ${_loose})
    endif()
    set(_opt_cmd "")
    if(CW_WASM_OPT AND EXISTS ${CW_WASM_OPT})
      # -g keeps the name section; the other features come from the module's target_features section
      set(_opt_cmd COMMAND ${CMAKE_COMMAND} -E env BINARYEN_CORES=${CW_CART_LINK_JOBS}
        ${CW_WASM_OPT} -O3 -g --enable-threads --enable-simd $<TARGET_FILE:${name}> -o $<TARGET_FILE:${name}>)
    endif()
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/carts
      ${_opt_cmd}
      ${_stage_cmd}
      COMMAND node ${CMAKE_SOURCE_DIR}/tools/check-imports.mjs $<TARGET_FILE:${name}> ${_wgpu_check}
      COMMAND node ${_wc_pack} --wasm $<TARGET_FILE:${name}> --name ${name}
              ${_assets} ${_files} --version 0.1.0 --output ${CMAKE_BINARY_DIR}/carts/${name}.wasc > /dev/null
      COMMENT "cart ${name}.wasc")
  endif()
endfunction()

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
if(CW_CART)
  get_property(_cw_carts GLOBAL PROPERTY CW_CARTS)
  list(SORT _cw_carts)
  list(JOIN _cw_carts "\n" _cw_carts)
  set(_cw_carts "${_cw_carts}\n")
  set(_cw_old "")
  if(EXISTS ${CMAKE_BINARY_DIR}/carts.txt)
    file(READ ${CMAKE_BINARY_DIR}/carts.txt _cw_old)
  endif()
  if(NOT _cw_old STREQUAL _cw_carts)
    file(WRITE ${CMAKE_BINARY_DIR}/carts.txt "${_cw_carts}")
  endif()
endif()
