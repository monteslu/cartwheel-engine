# cw_add_app: an app (a game, a sample, a tool) as a native executable and as
# a cart. Included by the engine's CMakeLists.txt, and by an installed SDK's
# cartwheelConfig.cmake (cmake/sdk.cmake), where CW_ROOT is the SDK prefix:
# every engine file it names (src/platform/native_entry.c, tools/*.mjs,
# deps/wasmcart*) sits at the same path under both.

# A version for every cart's manifest that passes no VERSION (cw_add_app
# below); empty (the default), each cart takes its git checkout's version.
set(CW_APP_VERSION "" CACHE STRING "the version in each cart's manifest (semver); empty: the app's git checkout's (tools/lib/app-version.mjs)")

# ── cw_add_app(name SOURCES ... [ASSETS dir] [NET domain...] [PLUGINS dir] [NO_PLUGINS] [PROGRAMS table.txt...]
#               [ART dir] [TITLE text] [DESCRIPTION text] [AUTHOR text] [VERSION semver]) ──
# Native: an executable. Cart: <name>.wasm packed into <name>.wasc (with the
# asset directory) by wasmcart-pack, after the import audit. Every cart is
# the dual GL + WebGPU one, and every app draws through the engine on both
# APIs: an app cannot opt out of WebGPU (a native-only test binary is added
# only when NOT CW_CART). Every
# cart a build packs is listed in <build>/carts.txt. NET lists the
# hosts the cart may dial with wc_peer_open (the manifest net.domains grant);
# without NET, a project's `net.domains` key in project.cwproj gives them.
# ART <dir> is the cart's pictures for launchers (an icon, a cover, a
# background, a logo, a title screen, screenshots: wasmcart's docs/art.md),
# packed under art/ in the .wasc; TITLE (the game's name as people read it,
# the manifest's `title`; the cart's `name` stays its id), DESCRIPTION and
# AUTHOR go in its manifest beside them. Without them, the app a project names
# (`app` in project.cwproj) takes the project's `art` (a directory relative
# to the project), `title`, `description` and `author` keys. Native builds do not use them.
# The manifest's version is semver (1.4.2, 1.5.0-rc.1, 1.4.2-dev.g1a2b3c4),
# worked out on every build (tools/cart-version.mjs, the rule in
# tools/lib/app-version.mjs): VERSION, else CW_APP_VERSION, else the git
# checkout holding the calling directory (at a v<semver> tag with no changes
# to tracked files: the tag without its v; otherwise the last v* tag plus
# -dev.g<commit>, 0.0.0-dev.g<commit> before any tag), else the `version` key
# of the project.cwproj beside the call, else 0.1.0. When it changes (a new
# tag, a commit, an edit to a tagged tree) the cart is packed again with it,
# without a relink. A version that is not semver stops the configure or the
# build.
# Plugins (cmake/plugins.cmake): the runtime plugins of PLUGINS <dir>,
# default <the calling directory>/plugins when it exists, are compiled in.
function(cw_add_app name)
  cmake_parse_arguments(A "NO_PLUGINS;QUIET_PLUGINS;NO_PROJECT_EDITOR" "ASSETS;PACK;PLUGINS;ART;TITLE;DESCRIPTION;AUTHOR;VERSION" "SOURCES;NET;PROGRAMS" ${ARGN})
  if(A_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "cw_add_app(${name}): unknown arguments ${A_UNPARSED_ARGUMENTS} (every app is a dual GL + WebGPU cart: there is no opt-out)")
  endif()
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
  # the project's own app takes its cart art and credits from project.cwproj
  # (art = art, title = ..., description = ..., author = ...) when the call passes none
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/project.cwproj)
    cw__project_value(_cw_app app)
    if(_cw_app STREQUAL name)
      foreach(_k art title description author)
        string(TOUPPER ${_k} _K)
        if(NOT A_${_K})
          cw__project_value(A_${_K} ${_k})
        endif()
      endforeach()
    endif()
  endif()
  # the manifest's version: VERSION or CW_APP_VERSION when given (checked here); else the
  # git checkout's or project.cwproj's, worked out every build (tools/cart-version.mjs)
  if(NOT A_VERSION)
    set(A_VERSION "${CW_APP_VERSION}")
  endif()
  if(A_VERSION)
    cw__check_semver(_bad "${A_VERSION}")
    if(_bad)
      message(FATAL_ERROR "cw_add_app(${name}): version \"${A_VERSION}\" is not semver (${_bad}): use MAJOR.MINOR.PATCH, optionally -prerelease and +build, such as 1.4.2 or 1.4.2-dev.g1a2b3c4")
    endif()
  endif()
  set(_cwproj_version "")
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/project.cwproj)
    cw__project_value(_cwproj_version version)
  endif()
  if(A_ART AND NOT IS_ABSOLUTE "${A_ART}")
    set(A_ART ${CMAKE_CURRENT_SOURCE_DIR}/${A_ART})
  endif()
  if(CW_CART AND A_ART AND NOT IS_DIRECTORY "${A_ART}")
    message(FATAL_ERROR "cw_add_app(${name}): ART ${A_ART} is not a directory")
  endif()
  if(ANDROID)
    # SDLActivity loads the game as libmain.so (one per app directory) and
    # looks up SDL_main in it: the entry object goes in the library itself
    # (from the cartwheel_main archive nothing would pull it in)
    add_library(${name} SHARED ${A_SOURCES} ${CW_ROOT}/src/platform/native_entry.c)
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
  elseif(CMAKE_CROSSCOMPILING OR CW_FROM_SDK)
    # exports, and builds from an SDK (it ships no cwhot): no hot-reload module
    target_link_libraries(${name} PRIVATE cartwheel_main)
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
    set(_args --manifest ${A_PACK} --out ${_out} --name ${name} --engine-root ${CW_ROOT} -q)
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
    # the shared libraries it runs with (Dawn, ANGLE): copied beside the
    # binary in an export build, which looks for them there and nowhere else
    cw__runtime_files(_rt)
    if(_rt AND (CW_EXPORT_TARGET OR CW_FROM_SDK) AND NOT ANDROID)
      # once per output directory (the apps there share them; a copy per app
      # raced when two apps linked at once)
      get_target_property(_rtdir ${name} RUNTIME_OUTPUT_DIRECTORY)
      if(NOT _rtdir)
        set(_rtdir ${CMAKE_CURRENT_BINARY_DIR})
      endif()
      string(MD5 _rtkey "${_rtdir}")
      if(NOT TARGET cw_runtime_${_rtkey})
        add_custom_command(OUTPUT ${_rtdir}/.cw-runtime
          COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_rt} ${_rtdir}
          COMMAND ${CMAKE_COMMAND} -E touch ${_rtdir}/.cw-runtime
          DEPENDS ${_rt} COMMENT "runtime libraries into ${_rtdir}")
        add_custom_target(cw_runtime_${_rtkey} DEPENDS ${_rtdir}/.cw-runtime)
      endif()
      add_dependencies(${name} cw_runtime_${_rtkey})
      if(APPLE)
        set_target_properties(${name} PROPERTIES BUILD_WITH_INSTALL_RPATH ON
          INSTALL_RPATH "@executable_path/../Frameworks;@executable_path")
      elseif(NOT WIN32)
        set_target_properties(${name} PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
      endif()
    endif()
    list(JOIN _rt "\", \"" _rtj)
    if(_rtj)
      set(_rtj "\"${_rtj}\"")
    endif()
    file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/apps/${name}.json CONTENT
"{\"name\": \"${name}\", \"binary\": \"$<TARGET_FILE:${name}>\", \"pak\": \"${_pak}\", \"assets\": \"${A_ASSETS}\", \"manifest\": \"${A_PACK}\", \"programs\": [${_progs}], \"runtime\": [${_rtj}], \"dir\": \"${CMAKE_CURRENT_SOURCE_DIR}\"}\n")
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
      -Wl,--export=malloc
      # every allocation makes sure the thread it runs on sees the block: a
      # thread's view of the memory's size can lag another thread's growth,
      # and memset into the block would trap (src/core/mem.c)
      -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=aligned_alloc
      -Wl,--wrap=posix_memalign
      # a test's environment on a cart host: the debug channel's env request sets variables getenv reads (src/core/env.c)
      -Wl,--wrap=getenv)
    # the asset dir is staged with its bundle manifest (cwfiles.txt: the file
    # list cw_vfs_list_dir reads, since a cart cannot list its bundle)
    set(_assets "")
    set(_stage_cmd "")
    if(A_ASSETS)
      set(_stage ${CMAKE_BINARY_DIR}/stage/${name})
      set(_assets --assets ${_stage})
      set(_stage_cmd COMMAND node ${CW_ROOT}/tools/stage-assets.mjs ${A_ASSETS} ${_stage})
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
    # the cart's pictures, title and credits for launchers (manifest art, title, description, author)
    set(_meta "")
    if(A_ART)
      list(APPEND _meta --art ${A_ART})
      file(GLOB_RECURSE _art_files CONFIGURE_DEPENDS ${A_ART}/*)
      set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS ${_art_files})
    endif()
    if(A_TITLE)
      list(APPEND _meta --title "${A_TITLE}")
    endif()
    if(A_DESCRIPTION)
      list(APPEND _meta --description "${A_DESCRIPTION}")
    endif()
    if(A_AUTHOR)
      list(APPEND _meta --author "${A_AUTHOR}")
    endif()
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
    # wasmcart keeps the archive open and reads assets on demand. Replacing the
    # file in place can invalidate a running cart's ZIP offsets mid-ride.
    set(_wasc ${CMAKE_BINARY_DIR}/carts/${name}.wasc)
    # the version, every build before the link: written to <name>.version, and an
    # existing cart packed with another version is packed again with this one
    # (tools/cart-version.mjs); the link's pack reads the file
    set(_ver_file ${CMAKE_CURRENT_BINARY_DIR}/${name}.version)
    set(_ver_record ${CMAKE_CURRENT_BINARY_DIR}/${name}.pack.json)
    # a dev cart whose assets did not change gets its new cart.wasm spliced into the last archive
    # instead of every asset deflated again (tools/cart-version.mjs --reuse-assets); a release cart is
    # always packed in full
    set(_reuse "")
    if(CW_CART_DEV)
      set(_reuse --reuse-assets)
    endif()
    add_custom_target(${name}_version
      COMMAND node ${CW_ROOT}/tools/cart-version.mjs update --out ${_ver_file} --dir ${CMAKE_CURRENT_SOURCE_DIR}
              --set "${A_VERSION}" --fallback "${_cwproj_version}" --wasc ${_wasc} --record ${_ver_record}
      BYPRODUCTS ${_ver_file} VERBATIM)
    add_dependencies(${name} ${name}_version)
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/carts
      ${_opt_cmd}
      ${_stage_cmd}
      COMMAND node ${CW_ROOT}/tools/check-imports.mjs $<TARGET_FILE:${name}> ${_wgpu_check}
      COMMAND node ${CW_ROOT}/tools/cart-version.mjs pack --version-file ${_ver_file} --wasc ${_wasc} --record ${_ver_record} ${_reuse}
              -- node ${_wc_pack} --wasm $<TARGET_FILE:${name}> --name ${name} ${_assets} ${_files} ${_meta}
      COMMENT "cart ${name}.wasc")
  endif()
endfunction()

# cw__check_semver(<out> <version>): empty when <version> is a semver 2.0.0
# string, else what is wrong with it
function(cw__check_semver out v)
  set(_why "")
  set(_core "(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)")
  set(_ident "[0-9A-Za-z-]+")
  if(NOT v MATCHES "^${_core}(-(${_ident}(\\.${_ident})*))?(\\+(${_ident}(\\.${_ident})*))?$")
    set(_why "not MAJOR.MINOR.PATCH[-prerelease][+build], or a number with a leading zero")
  else()
    # a numeric prerelease identifier has no leading zero (1.0.0-01 is not semver)
    string(REGEX REPLACE "\\+.*$" "" _pre "${v}")
    if(_pre MATCHES "^[^-]*-(.*)$")
      string(REPLACE "." ";" _ids "${CMAKE_MATCH_1}")
      foreach(_id ${_ids})
        if(_id MATCHES "^0[0-9]+$")
          set(_why "prerelease identifier ${_id} is a number with a leading zero")
        endif()
      endforeach()
    endif()
  endif()
  set(${out} "${_why}" PARENT_SCOPE)
endfunction()

# cw__project_value(<out> <key>): a key's value in the calling directory's
# project.cwproj (surrounding quotes stripped, spaces trimmed); empty when the
# file has no such key
function(cw__project_value out key)
  set(_v "")
  string(REPLACE "." "\\." _kre "${key}")
  file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/project.cwproj _line REGEX "^[ \t]*${_kre}[ \t]*=")
  if(_line)
    list(GET _line 0 _line)
    string(REGEX REPLACE "^[ \t]*${_kre}[ \t]*=[ \t]*" "" _v "${_line}")
    string(STRIP "${_v}" _v)
    if(_v MATCHES "^\"(.*)\"$")
      set(_v "${CMAKE_MATCH_1}")
    endif()
  endif()
  set(${out} "${_v}" PARENT_SCOPE)
endfunction()

# <build>/carts.txt: the carts this build packs (scripts/build.sh removes any
# other .wasc from carts/; tests/wgpu-carts.mjs checks these). Called once,
# after every cw_add_app.
function(cw__write_carts_txt)
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
endfunction()

# cw__runtime_files(<out>): the shared libraries a native app loads at run
# time besides the system's: native-dawn's (WebGPU builds) and ANGLE's
# (Windows, macOS). Android packs its own (tools/cwexport/android.mjs).
function(cw__runtime_files out)
  set(_files "")
  if(NOT CW_CART AND CW_WGPU AND CW_NATIVE_DAWN)
    file(GLOB _d ${CW_NATIVE_DAWN}/lib/*.so ${CW_NATIVE_DAWN}/lib/*.dylib ${CW_NATIVE_DAWN}/bin/*.dll)
    list(APPEND _files ${_d})
  endif()
  if(NOT CW_CART AND CW_ANGLE_GLESV2 AND (WIN32 OR APPLE))
    get_filename_component(_a ${CW_ANGLE_GLESV2} DIRECTORY)
    file(GLOB _d ${_a}/libEGL.dll ${_a}/libGLESv2.dll ${_a}/d3dcompiler_47.dll ${_a}/libEGL.dylib ${_a}/libGLESv2.dylib)
    list(APPEND _files ${_d})
  endif()
  list(SORT _files)
  set(${out} "${_files}" PARENT_SCOPE)
endfunction()
