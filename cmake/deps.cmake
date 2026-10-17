# Dependency targets built from deps/ sources (never prebuilt binaries).

# ── SDL2: system SDL2 natively, wasmcart-sdl2 (built by
#    scripts/build-sdl2-cart.sh) in carts ───────────────────────────────
add_library(cw_sdl2 INTERFACE)
if(CW_CART)
  set(_sdl_lib ${CMAKE_SOURCE_DIR}/build/sdl2-cart/libSDL2_wc.a)
  add_custom_command(OUTPUT ${_sdl_lib}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build-sdl2-cart.sh
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/build-sdl2-cart.sh
    COMMENT "SDL2 for carts")
  add_custom_target(cw_sdl2_build DEPENDS ${_sdl_lib})
  add_dependencies(cw_sdl2 cw_sdl2_build)
  target_include_directories(cw_sdl2 INTERFACE ${DEPS}/sdl2/include ${DEPS}/wasmcart-sdl2/sdl2_wc)
  target_compile_options(cw_sdl2 INTERFACE -include ${CMAKE_SOURCE_DIR}/cmake/cart/SDL_config_cartwheel.h
    -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_MMAN)
  target_link_libraries(cw_sdl2 INTERFACE ${_sdl_lib} wasi-emulated-signal wasi-emulated-process-clocks wasi-emulated-mman)
elseif(CW_SDL2_FROM_SOURCE)
  # exports (cmake/export/*.cmake): SDL2 built from deps/sdl2, linked
  # statically, so a shipped game needs no SDL on the player's machine (video,
  # audio and input backends are dlopen'd at run time as SDL does by default)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
  set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
  # patches/sdl2/*.patch apply to a copy in the build tree (deps/ stays pristine)
  set(_sdl_src ${CMAKE_BINARY_DIR}/sdl2-src)
  file(GLOB _sdl_patches ${CMAKE_SOURCE_DIR}/patches/sdl2/*.patch)
  set(_sdl_want "")
  foreach(_p ${_sdl_patches})
    file(SHA256 ${_p} _h)
    string(APPEND _sdl_want "${_h}\n")
  endforeach()
  set(_sdl_have "")
  if(EXISTS ${_sdl_src}/.cw-stamp)
    file(READ ${_sdl_src}/.cw-stamp _sdl_have)
  endif()
  if(NOT _sdl_have STREQUAL _sdl_want)
    file(REMOVE_RECURSE ${_sdl_src})
    file(COPY ${DEPS}/sdl2/ DESTINATION ${_sdl_src})
    foreach(_p ${_sdl_patches})
      execute_process(COMMAND patch -s -p1 -i ${_p} WORKING_DIRECTORY ${_sdl_src} RESULT_VARIABLE _r)
      if(NOT _r EQUAL 0)
        message(FATAL_ERROR "sdl2: ${_p} does not apply")
      endif()
    endforeach()
    file(WRITE ${_sdl_src}/.cw-stamp "${_sdl_want}")
  endif()
  add_subdirectory(${_sdl_src} ${CMAKE_BINARY_DIR}/sdl2 EXCLUDE_FROM_ALL)
  target_link_libraries(cw_sdl2 INTERFACE SDL2::SDL2-static)
else()
  find_package(SDL2 REQUIRED)
  target_link_libraries(cw_sdl2 INTERFACE SDL2::SDL2)
endif()

# ── GLES 3.0 natively: the system's libGLESv2 on Linux, ANGLE on Windows /
#    macOS / iOS, the NDK's libGLESv3 on Android ─────────────────────────
add_library(cw_gles INTERFACE)
if(NOT CW_CART)
  if(ANDROID)
    target_link_libraries(cw_gles INTERFACE GLESv3 EGL)
  elseif(WIN32 OR APPLE)
    # CW_ANGLE_DIR: an ANGLE with include/ (Khronos + ANGLE headers) and the
    # libGLESv2 to link (an import library on Windows, a dylib on Apple)
    set(CW_ANGLE_INCLUDE "" CACHE PATH "Khronos GLES3/EGL headers for ANGLE builds")
    set(CW_ANGLE_GLESV2 "" CACHE FILEPATH "ANGLE libGLESv2 to link")
    if(NOT CW_ANGLE_INCLUDE OR NOT CW_ANGLE_GLESV2)
      message(FATAL_ERROR "Windows / Apple builds render through ANGLE: set CW_ANGLE_INCLUDE and CW_ANGLE_GLESV2 "
                          "(the export toolchains in cmake/export/ do)")
    endif()
    target_include_directories(cw_gles INTERFACE ${CW_ANGLE_INCLUDE})
    target_link_libraries(cw_gles INTERFACE ${CW_ANGLE_GLESV2})
    # iOS: libEGL as well (src/platform/egl_uikit.c calls egl* itself; on
    # Windows and macOS SDL loads ANGLE's EGL at run time)
    set(CW_ANGLE_EGL "" CACHE FILEPATH "ANGLE libEGL to link (iOS)")
    if(CW_ANGLE_EGL)
      target_link_libraries(cw_gles INTERFACE ${CW_ANGLE_EGL})
    endif()
  else()
    # by its soname: links with the runtime library alone (no -dev package)
    target_link_libraries(cw_gles INTERFACE -l:libGLESv2.so.2)
  endif()
endif()

# SIMD on wasm: Box2D/Box3D select their wasm_simd128.h solver paths themselves
# under -msimd128 (patches/box2d/0003, patches/box3d/0004), bit-identical to the
# SSE2 paths native x86 runs.

# ── Box2D v3 ─────────────────────────────────────────────────────────
file(GLOB _b2 ${DEPS}/box2d/src/*.c)
add_library(box2d STATIC ${_b2})
target_include_directories(box2d PUBLIC ${DEPS}/box2d/include PRIVATE ${DEPS}/box2d/src)
target_compile_options(box2d PRIVATE -O2 -w)
# Box2D's 8-wide AVX2 contact solver, on a native build for the machine it is
# built on when that CPU has AVX2 (Linux, not an export: exports and carts keep
# the 4-wide SSE2 / wasm SIMD solver). A wider solver gives the same bits (a
# graph color's constraints share no body); physics-determinism holds native
# to every cart's hashes.
if(NOT CW_CART AND NOT CW_EXPORT_TARGET AND NOT CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"
   AND EXISTS /proc/cpuinfo)
  file(STRINGS /proc/cpuinfo _cw_avx2 REGEX "^flags.* avx2( |$)" LIMIT_COUNT 1)
  if(_cw_avx2)
    target_compile_definitions(box2d PRIVATE BOX2D_AVX2)
    target_compile_options(box2d PRIVATE -mavx2)
  endif()
endif()

# ── Box3D ────────────────────────────────────────────────────────────
file(GLOB _b3 ${DEPS}/box3d/src/*.c)
add_library(box3d STATIC ${_b3})
target_include_directories(box3d PUBLIC ${DEPS}/box3d/include PRIVATE ${DEPS}/box3d/src)
target_compile_options(box3d PRIVATE -O2 -w)

# ── Recast / Detour / DetourTileCache (zlib): navmeshes (cartwheel/nav.h).
#    Compiled into the engine library itself (CMakeLists.txt), not a library
#    of their own: their math and allocator hooks (patches/recastnavigation,
#    src/nav/recast_hooks.h) call the engine. DetourCrowd is not used: the
#    crowd is the engine's (src/nav/crowd.c).
set(CW_RECAST ${DEPS}/recastnavigation)
if(NOT EXISTS ${CW_RECAST}/Detour/Source/DetourNavMesh.cpp)
  message(FATAL_ERROR "recastnavigation is missing: run scripts/fetch-deps.sh")
endif()
file(GLOB CW_RECAST_SRCS ${CW_RECAST}/Recast/Source/*.cpp ${CW_RECAST}/Detour/Source/*.cpp
     ${CW_RECAST}/DetourTileCache/Source/*.cpp)
set(CW_RECAST_INCLUDES ${CW_RECAST}/Recast/Include ${CW_RECAST}/Detour/Include ${CW_RECAST}/DetourTileCache/Include
    ${CMAKE_SOURCE_DIR}/src/nav)
set(CW_RECAST_DEFS RC_MATH_HEADER="recast_hooks.h" DT_MATH_HEADER="recast_hooks.h" RC_ALLOC_HEADER="recast_hooks.h"
    DT_ALLOC_HEADER="recast_hooks.h" DT_VIRTUAL_QUERYFILTER=1)

# ── three.c ──────────────────────────────────────────────────────────
# CW_THREE_DIR builds three.c from another checkout of the same API (a fix
# being verified before deps/ is refetched at a new pin); deps/three.c by
# default.
# The environment variable of the same name is the default too, so the builds a
# test configures itself (exported projects, templates) follow a worktree's pin.
# An explicitly set environment variable wins over a cached value (a build
# tree configured before the checkout moved keeps working).
if(DEFINED ENV{CW_THREE_DIR})
  set(CW_THREE_DIR $ENV{CW_THREE_DIR} CACHE PATH "three.c source tree the engine builds" FORCE)
else()
  set(CW_THREE_DIR ${DEPS}/three.c CACHE PATH "three.c source tree the engine builds")
endif()
# The engine relies on three.c fixes by pin (one keeps two renderers sharing a
# geometry from reading past a GPU buffer): an out-of-date checkout is an
# error, not a silent older build.
file(STRINGS ${CMAKE_SOURCE_DIR}/scripts/deps.lock _t3_lock REGEX "^three\\.c[ \t]")
string(REGEX MATCH "[0-9a-f]+$" _t3_pin "${_t3_lock}")
execute_process(COMMAND git -C ${CW_THREE_DIR} rev-parse HEAD OUTPUT_VARIABLE _t3_head OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE _t3_git)
# the lock's version column ("v0.186.0") must match three.h's T3_VERSION too
# (a source tree without git, e.g. an unpacked release, is checked by that alone)
if(_t3_lock MATCHES "[ \t]v([0-9]+\\.[0-9]+\\.[0-9]+)[ \t]")
  set(_t3_lock_ver ${CMAKE_MATCH_1})
  file(STRINGS ${CW_THREE_DIR}/include/three.h _t3_ver_line REGEX "^#define T3_VERSION ")
  string(REGEX MATCH "\"([^\"]*)\"" _t3_m "${_t3_ver_line}")
  if(NOT CMAKE_MATCH_1 STREQUAL _t3_lock_ver)
    message(FATAL_ERROR "three.c at ${CW_THREE_DIR} is version '${CMAKE_MATCH_1}', scripts/deps.lock pins v${_t3_lock_ver}: "
                        "run scripts/fetch-deps.sh (or point CW_THREE_DIR at a checkout of the pin)")
  endif()
endif()
if(_t3_git EQUAL 0 AND NOT _t3_head STREQUAL _t3_pin)
  message(FATAL_ERROR "three.c at ${CW_THREE_DIR} is ${_t3_head}, scripts/deps.lock pins ${_t3_pin}: "
                      "run scripts/fetch-deps.sh (or point CW_THREE_DIR at a checkout of the pin)")
endif()
# CW_THREE_TABLE: which of three.c's own program tables the engine links.
# core: only the programs the renderer draws with itself (output pass,
# backgrounds, PMREM, shadow casters); every material program comes from the
# app's own tables (cmake/programs.cmake, docs/guides/rendering-3d.md). full:
# three.c's whole table as well (tens of MB per backend).
set(CW_THREE_TABLE core CACHE STRING "three.c's own program table: core or full")
set_property(CACHE CW_THREE_TABLE PROPERTY STRINGS core full)
if(CW_THREE_TABLE STREQUAL "core")
  set(CW_T3_TBL _core)
elseif(CW_THREE_TABLE STREQUAL "full")
  set(CW_T3_TBL "")
else()
  message(FATAL_ERROR "CW_THREE_TABLE is core or full, not ${CW_THREE_TABLE}")
endif()
set(_t3 math core geometry curves animation raycaster loaders gltf renderer backend_gles gen_program gen/programs${CW_T3_TBL}_gl gen/programs_dfg)
list(TRANSFORM _t3 PREPEND ${CW_THREE_DIR}/src/)
list(TRANSFORM _t3 APPEND .c)
add_library(three STATIC ${_t3})
target_include_directories(three PUBLIC ${CW_THREE_DIR}/include PRIVATE ${CW_THREE_DIR}/src ${CW_THREE_DIR}/third_party)
target_compile_options(three PRIVATE -O2 -w)
target_compile_definitions(three PRIVATE _POSIX_C_SOURCE=200809L NDEBUG)
if(CW_THREE_TABLE STREQUAL "core")
  # three.c's table sources select themselves by this define (its build.sh
  # passes it with T3_TABLE=core); without it the core sources are empty
  target_compile_definitions(three PRIVATE T3_TABLE_CORE)
endif()
target_link_libraries(three PUBLIC cw_gles) # GLES3 headers (and the library) for the target
if(CW_CART)
  target_compile_definitions(three PRIVATE T3_WASMCART)
  target_include_directories(three PRIVATE ${DEPS}/wasmcart/include)
endif()
# CW_THREE_BUILDER: three.c's runtime program builder (T3_BUILDER: three.js
# r186 in an embedded QuickJS) builds a state no program table has on first
# use (a few hundred ms), so a draw is never skipped while developing. The
# states it builds are still reported (cw.three.missing_states, the gates fail
# on them): they belong in the app's table. On by default for native
# development builds; off for carts and project builds (cwproject, exports: about 4.7 MB of QuickJS and
# three.js, and a 4 MB stack). Needs ${CW_THREE_DIR}/tools/fetch-builder-deps.sh
# and Node at build time (to embed the JS).
set(_t3_qjs ${CW_THREE_DIR}/third_party/quickjs)
set(_t3_pkg ${CW_THREE_DIR}/third_party/three-0.186.1)
find_program(CW_NODE node)
set(_t3_bld_default OFF)
if(NOT CW_CART AND NOT CMAKE_CROSSCOMPILING AND NOT CW_ENGINE_ONLY AND CW_NODE AND EXISTS ${_t3_qjs}/quickjs.c
   AND EXISTS ${_t3_pkg}/build/three.webgpu.js AND EXISTS ${CW_THREE_DIR}/src/builder.c)
  set(_t3_bld_default ON)
endif()
option(CW_THREE_BUILDER "three.c's runtime program builder (development: missing program states are built, not skipped)" ${_t3_bld_default})
if(CW_THREE_BUILDER)
  if(NOT EXISTS ${_t3_qjs}/quickjs.c OR NOT EXISTS ${_t3_pkg}/build/three.webgpu.js)
    message(FATAL_ERROR "CW_THREE_BUILDER needs QuickJS and three.js in ${CW_THREE_DIR}/third_party: run ${CW_THREE_DIR}/tools/fetch-builder-deps.sh")
  endif()
  if(NOT CW_NODE)
    message(FATAL_ERROR "CW_THREE_BUILDER needs Node at build time (tools/embed-builder.mjs)")
  endif()
  set(_bjs ${CMAKE_BINARY_DIR}/three_builder/builder_js.c)
  file(GLOB _bjs_deps ${CW_THREE_DIR}/tools/builder-entry.mjs ${CW_THREE_DIR}/tools/capture-core.mjs ${CW_THREE_DIR}/tools/mock-gpu.mjs
       ${CW_THREE_DIR}/tools/program-model.mjs ${CW_THREE_DIR}/tools/gpu-answers.json ${CW_THREE_DIR}/tools/embed-builder.mjs)
  add_custom_command(OUTPUT ${_bjs}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/three_builder
    COMMAND ${CW_NODE} ${CW_THREE_DIR}/tools/embed-builder.mjs ${_t3_pkg} ${_bjs}
    DEPENDS ${_bjs_deps} ${_t3_pkg}/build/three.webgpu.js ${_t3_pkg}/build/three.core.js
    COMMENT "three.c builder: embedding three.js r186")
  add_library(three_qjs STATIC ${_t3_qjs}/dtoa.c ${_t3_qjs}/libregexp.c ${_t3_qjs}/libunicode.c ${_t3_qjs}/quickjs.c)
  target_include_directories(three_qjs PUBLIC ${_t3_qjs})
  target_compile_options(three_qjs PRIVATE -O2 -w)
  target_compile_definitions(three_qjs PRIVATE _GNU_SOURCE)
  target_sources(three PRIVATE ${CW_THREE_DIR}/src/builder.c ${_bjs})
  target_compile_definitions(three PRIVATE T3_BUILDER)
  target_link_libraries(three PRIVATE three_qjs)
endif()

# ── miniaudio: mixing + decoding only; SDL owns the device ────────────
add_library(cw_miniaudio STATIC ${CMAKE_SOURCE_DIR}/src/third_party/miniaudio_impl.c)
target_include_directories(cw_miniaudio PUBLIC ${DEPS}/miniaudio)
# No SIMD paths: the scalar code is what carts run (miniaudio has no wasm SIMD),
# and dr_mp3's SSE2 synthesis decodes differently from its scalar path.
# Scalar everywhere keeps every decode bit-identical across targets.
target_compile_definitions(cw_miniaudio PUBLIC MA_NO_DEVICE_IO MA_NO_THREADING
  PRIVATE MA_NO_SSE2 MA_NO_AVX2 MA_NO_NEON MA_DR_MP3_NO_SIMD MA_DR_FLAC_NO_SIMD)
target_compile_options(cw_miniaudio PRIVATE -O2 -w)
if(NOT CW_CART)
  target_link_libraries(cw_miniaudio PUBLIC ${CMAKE_DL_LIBS}) # ma_dlopen (libdl before glibc 2.34)
endif()

# ── meshoptimizer: the EXT_meshopt_compression decoder at load time (both
#    builds) and the mesh importer in cwpack (native) ──────────────────
file(GLOB _mo ${DEPS}/meshoptimizer/src/*.cpp)
add_library(meshoptimizer STATIC ${_mo})
target_include_directories(meshoptimizer PUBLIC ${DEPS}/meshoptimizer/src)
target_compile_options(meshoptimizer PRIVATE -O2 -w -fno-exceptions -fno-rtti)
if(CW_CART)
  target_compile_options(meshoptimizer PRIVATE -msimd128)
endif()

# ── Basis Universal (KTX2): the transcoder in both builds, the encoder in
#    cwpack only. Pinned in scripts/deps.lock like every dependency; a git
#    worktree whose shared deps/ predates it fetches it into
#    out/deps-extra/basis_universal (same commit):
#      git clone https://github.com/BinomialLLC/basis_universal.git out/deps-extra/basis_universal
#      git -C out/deps-extra/basis_universal checkout <commit from scripts/deps.lock>
if(EXISTS ${DEPS}/basis_universal/transcoder/basisu_transcoder.cpp)
  set(CW_BASISU ${DEPS}/basis_universal)
elseif(EXISTS ${CMAKE_SOURCE_DIR}/out/deps-extra/basis_universal/transcoder/basisu_transcoder.cpp)
  set(CW_BASISU ${CMAKE_SOURCE_DIR}/out/deps-extra/basis_universal)
else()
  message(FATAL_ERROR "basis_universal is missing: run scripts/fetch-deps.sh (see cmake/deps.cmake for worktrees)")
endif()
# formats no cartwheel target uploads are compiled out (PVRTC, ATC, FXT1);
# the unused source codecs (XUASTC, UASTC HDR) cannot be: v2.50 does not
# build without them
set(CW_BASISU_DEFS BASISD_SUPPORT_PVRTC1=0 BASISD_SUPPORT_PVRTC2=0 BASISD_SUPPORT_ATC=0 BASISD_SUPPORT_FXT1=0
    BASISD_SUPPORT_KTX2=1 BASISD_SUPPORT_KTX2_ZSTD=1)
add_library(cw_basisu_transcoder STATIC ${CW_BASISU}/transcoder/basisu_transcoder.cpp ${CW_BASISU}/zstd/zstddeclib.c)
target_include_directories(cw_basisu_transcoder PUBLIC ${CW_BASISU}/transcoder PRIVATE ${CW_BASISU}/zstd)
target_compile_definitions(cw_basisu_transcoder PUBLIC ${CW_BASISU_DEFS})
target_compile_options(cw_basisu_transcoder PRIVATE -O2 -w -fno-exceptions -fno-rtti -fno-strict-aliasing)

# ── audio codecs, from wasmcart-libs' vendored sources (the same code its
#    cart packages are built from), compiled here for both targets ───────
set(_wcl ${DEPS}/wasmcart-libs/packages)
add_library(cw_ogg STATIC ${_wcl}/ogg/vendor/src/bitwise.c ${_wcl}/ogg/vendor/src/framing.c)
target_include_directories(cw_ogg PUBLIC ${_wcl}/ogg/vendor/include)
target_compile_options(cw_ogg PRIVATE -O2 -w)

# Opus is built FIXED_POINT (wasmcart-libs ships the float build): the
# fixed-point decoder is integer arithmetic, so it decodes bit-identically on
# every target whatever its libm (the float build calls exp/log per frame).
# silk/float (encoder analysis) is swapped for its silk/fixed counterpart.
# The sources are copied into the build tree and patches/opus/*.patch applied
# there (deps/ stays pristine; it may be shared between checkouts).
set(_opus_src ${CMAKE_BINARY_DIR}/opus-src)
file(GLOB _opus_patches ${CMAKE_SOURCE_DIR}/patches/opus/*.patch)
set(_opus_stamp_want "")
foreach(_p ${_opus_patches})
  file(SHA256 ${_p} _h)
  string(APPEND _opus_stamp_want "${_h}\n")
endforeach()
set(_opus_stamp_have "")
if(EXISTS ${_opus_src}/.cw-stamp)
  file(READ ${_opus_src}/.cw-stamp _opus_stamp_have)
endif()
if(NOT _opus_stamp_have STREQUAL _opus_stamp_want)
  file(REMOVE_RECURSE ${_opus_src})
  file(COPY ${_wcl}/opus/vendor/src/ DESTINATION ${_opus_src})
  foreach(_p ${_opus_patches})
    execute_process(COMMAND patch -s -p1 -i ${_p} WORKING_DIRECTORY ${_opus_src} RESULT_VARIABLE _r)
    if(NOT _r EQUAL 0)
      message(FATAL_ERROR "opus: ${_p} does not apply")
    endif()
  endforeach()
  file(WRITE ${_opus_src}/.cw-stamp "${_opus_stamp_want}")
endif()
file(STRINGS ${_wcl}/opus/sources.txt _opus_srcs REGEX "^[^#].*\\.c$")
list(FILTER _opus_srcs EXCLUDE REGEX "^silk/float/")
file(GLOB _opus_fix RELATIVE ${_opus_src} ${_opus_src}/silk/fixed/*.c)
list(APPEND _opus_srcs ${_opus_fix})
list(TRANSFORM _opus_srcs PREPEND ${_opus_src}/)
add_library(cw_opus STATIC ${_opus_srcs})
target_include_directories(cw_opus PUBLIC ${_wcl}/opus/vendor/include/opus
  PRIVATE ${_opus_src}/celt ${_opus_src}/silk ${_opus_src}/silk/fixed ${_opus_src}/src ${_opus_src}/include)
target_compile_definitions(cw_opus PRIVATE OPUS_BUILD FIXED_POINT HAVE_LRINT HAVE_LRINTF VAR_ARRAYS NDEBUG)
target_compile_options(cw_opus PRIVATE -O2 -w)

file(STRINGS ${_wcl}/libxmp/sources.txt _xmp_srcs REGEX "^[^#].*\\.c$")
list(TRANSFORM _xmp_srcs PREPEND ${_wcl}/libxmp/vendor/src/)
add_library(cw_xmp STATIC ${_xmp_srcs})
target_include_directories(cw_xmp PUBLIC ${_wcl}/libxmp/vendor/include PRIVATE ${_wcl}/libxmp/vendor/src)
target_compile_definitions(cw_xmp PUBLIC LIBXMP_STATIC PRIVATE LIBXMP_CORE_PLAYER NDEBUG)
target_compile_options(cw_xmp PRIVATE -O2 -w)

# stb_vorbis (deps/stb), one translation unit
add_library(cw_stb_vorbis STATIC ${CMAKE_SOURCE_DIR}/src/third_party/stb_vorbis_impl.c)
target_include_directories(cw_stb_vorbis PUBLIC ${DEPS}/stb)
target_compile_options(cw_stb_vorbis PRIVATE -O2 -w)

# pl_mpeg (deps/pl_mpeg, MIT): MPEG-1 video and MP2 audio, one translation
# unit; the engine decodes cutscene video with it (src/video) and plays the
# sound as a stream (src/audio/decode.c). Integer IDCT; the MP2 synthesis is
# + - * only (no libm), so frames and samples match on every host.
add_library(cw_plmpeg STATIC ${CMAKE_SOURCE_DIR}/src/third_party/pl_mpeg_impl.c)
target_include_directories(cw_plmpeg PUBLIC ${DEPS}/pl_mpeg)
target_compile_options(cw_plmpeg PRIVATE -O2 -w)
# lz4 (deps/lz4/lib only: BSD-2): packed-resource decompression in the engine
# (src/resource/compress.c), compression in cwpack. Its xxhash is namespaced
# so it cannot meet another copy.
if(NOT EXISTS ${DEPS}/lz4/lib/lz4.c)
  message(FATAL_ERROR "lz4 is missing: run scripts/fetch-deps.sh")
endif()
add_library(cw_lz4 STATIC ${DEPS}/lz4/lib/lz4.c ${DEPS}/lz4/lib/lz4frame.c ${DEPS}/lz4/lib/lz4hc.c ${DEPS}/lz4/lib/xxhash.c)
target_include_directories(cw_lz4 PUBLIC ${DEPS}/lz4/lib)
target_compile_definitions(cw_lz4 PRIVATE XXH_NAMESPACE=CW_LZ4_)
target_compile_options(cw_lz4 PRIVATE -O2 -w)

# kb_text_shape (deps/kb, zlib): OpenType shaping + bidi segmentation for
# complex scripts (Arabic, Hebrew, Indic ...) in the 2D text renderer
add_library(cw_kbts STATIC ${CMAKE_SOURCE_DIR}/src/third_party/kb_text_shape_impl.c)
target_include_directories(cw_kbts PUBLIC ${DEPS}/kb)
target_compile_options(cw_kbts PRIVATE -O2 -w)

# ── WebGPU: cartwheel's second GPU backend (cartwheel/gpu.h), chosen at
#    startup (CW_GPU=gl|webgpu natively, WC_HOST_FLAG_GPU_WGPU in a dual
#    cart). Native: native-dawn's C SDK (deps/native-dawn, dawn::webgpu_dawn).
#    Carts: CW_WGPU=ON builds DUAL carts (gl + WebGPU imports, gpu_api 2) in
#    their own tree (build/cart-wgpu, scripts/build.sh target cart-wgpu) with
#    the wasmcart tier's wgpu-wasi kit and Dawn's emdawnwebgpu port; the
#    plain cart tree stays GL-only. ─────────────────────────────────────
if(CW_CART)
  option(CW_WGPU "dual GL + WebGPU carts (the wasmcart WebGPU tier)" OFF)
else()
  # native-dawn's prefix: deps/native-dawn for this machine, an export
  # toolchain's own for its target (cmake/export/common.cmake)
  set(CW_NATIVE_DAWN ${DEPS}/native-dawn CACHE PATH "native-dawn prefix (the native WebGPU backend)")
  set(_cw_wgpu_default OFF)
  if(EXISTS ${CW_NATIVE_DAWN}/lib/cmake/Dawn AND (CW_EXPORT_TARGET OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)))
    set(_cw_wgpu_default ON)
  endif()
  option(CW_WGPU "the native WebGPU backend (native-dawn)" ${_cw_wgpu_default})
endif()
add_library(cw_wgpu INTERFACE)
if(CW_WGPU)
  target_compile_definitions(cw_wgpu INTERFACE CW_WGPU=1)
  if(CW_CART)
    set(CW_WGPU_KIT ${DEPS}/wasmcart-wgpu/wgpu-wasi)
    set(CW_EMDAWNWEBGPU ${DEPS}/emdawnwebgpu)
    if(NOT EXISTS ${CW_WGPU_KIT}/wasmcart-wgpu-wasi.cmake OR NOT EXISTS ${CW_EMDAWNWEBGPU}/webgpu/src/webgpu.cpp)
      message(FATAL_ERROR "CW_WGPU carts need deps/wasmcart-wgpu and deps/emdawnwebgpu: run scripts/fetch-deps.sh")
    endif()
    include(${CW_WGPU_KIT}/wasmcart-wgpu-wasi.cmake)
    # the port's C++ half and the kit's support, compiled once into an OBJECT
    # library (the kit's own form for many carts); it carries the exports the
    # host's glue calls and -Wl,--allow-undefined to every cart that links it
    wasmcart_wgpu_wasi_library(cw_wgpu_cart EMDAWNWEBGPU_PKG ${CW_EMDAWNWEBGPU})
    target_compile_options(cw_wgpu_cart PRIVATE -O2 -w)
    target_include_directories(cw_wgpu INTERFACE ${CW_WGPU_KIT}/include ${CW_EMDAWNWEBGPU}/webgpu/include)
  else()
    find_package(Dawn REQUIRED CONFIG PATHS ${CW_NATIVE_DAWN} NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    target_link_libraries(cw_wgpu INTERFACE dawn::webgpu_dawn)
  endif()
  # three.c's WebGPU backend (three.js r186's generated WGSL): the scene pass
  # renders with it when the frame runs on WebGPU (t3_renderer_use_wgpu)
  target_sources(three PRIVATE ${CW_THREE_DIR}/src/gen_wgpu.c ${CW_THREE_DIR}/src/gen/programs${CW_T3_TBL}_wgpu.c)
  target_compile_definitions(three PRIVATE T3_WGPU)
  target_link_libraries(three PRIVATE cw_wgpu)
endif()
