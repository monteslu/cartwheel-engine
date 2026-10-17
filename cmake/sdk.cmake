# Prebuilt engine SDKs (scripts/build-sdk.sh, docs/sdk.md). With CW_SDK=ON a
# build tree also writes <build>/sdk/targets.json: every library a game links,
# with its file and its interface (include directories, definitions, options,
# link libraries) as this build resolved them. tools/sdk-pack.mjs turns that
# into the SDK's imported targets, copying the files and headers they name.

option(CW_SDK "write the SDK description (<build>/sdk/targets.json) for tools/sdk-pack.mjs" OFF)
if(NOT CW_SDK)
  return()
endif()

set(_cw_sdk_libs cartwheel three box2d box3d cw_miniaudio cw_stb_vorbis cw_opus cw_ogg cw_xmp meshoptimizer
    cw_basisu_transcoder cw_kbts cw_plmpeg cw_lz4 cwprog_cw_engine)
set(_cw_sdk_ifaces cw_sdl2 cw_gles cw_wgpu)
if(NOT CW_CART)
  list(APPEND _cw_sdk_libs cartwheel_main)
endif()
if(CW_SDL2_FROM_SOURCE AND TARGET SDL2-static)
  list(APPEND _cw_sdk_libs SDL2-static)
  if(TARGET SDL2main)
    list(APPEND _cw_sdk_libs SDL2main)
  endif()
endif()
if(CW_CART AND CW_WGPU)
  # the WebGPU port's objects as an archive (in-tree it is an OBJECT library):
  # the SDK links it whole, so its exports stay in every cart
  add_library(cw_wgpu_cart_archive STATIC $<TARGET_OBJECTS:cw_wgpu_cart>)
  list(APPEND _cw_sdk_libs cw_wgpu_cart_archive)
endif()

set(_json "{\n  \"cart\": \"${CW_CART}\",\n  \"wgpu\": \"${CW_WGPU}\",\n  \"system\": \"${CMAKE_SYSTEM_NAME}\",\n  \"processor\": \"${CMAKE_SYSTEM_PROCESSOR}\",\n")
string(APPEND _json "  \"c_compiler\": \"${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}\",\n  \"build_type\": \"${CMAKE_BUILD_TYPE}\",\n")
string(APPEND _json "  \"source_dir\": \"${CMAKE_SOURCE_DIR}\",\n  \"binary_dir\": \"${CMAKE_BINARY_DIR}\",\n")
string(APPEND _json "  \"three_dir\": \"${CW_THREE_DIR}\",\n  \"three_table\": \"${CW_THREE_TABLE}\",\n")
if(CW_WGPU AND NOT CW_CART)
  string(APPEND _json "  \"native_dawn\": \"${CW_NATIVE_DAWN}\",\n")
endif()
if(CW_ANGLE_GLESV2)
  string(APPEND _json "  \"angle\": \"${CW_ANGLE_GLESV2}\",\n")
endif()
if(TARGET cw_wgpu_cart)
  string(APPEND _json "  \"wgpu_cart_link_options\": \"$<TARGET_PROPERTY:cw_wgpu_cart,INTERFACE_LINK_OPTIONS>\",\n")
endif()
# cwpack ships in the SDKs of desktop targets (a host tool there)
set(_cw_sdk_host_tools OFF)
if(TARGET cwpack AND NOT CW_CART AND NOT ANDROID AND NOT IOS)
  set(_cw_sdk_host_tools ON)
endif()
if(_cw_sdk_host_tools)
  string(APPEND _json "  \"cwpack\": \"$<TARGET_FILE:cwpack>\",\n")
endif()
string(APPEND _json "  \"targets\": [\n")
set(_sep "")
foreach(t ${_cw_sdk_libs} ${_cw_sdk_ifaces})
  get_target_property(_type ${t} TYPE)
  set(_file "")
  if(NOT _type STREQUAL "INTERFACE_LIBRARY")
    set(_file "$<TARGET_FILE:${t}>")
  endif()
  string(APPEND _json "${_sep}    {\"name\": \"${t}\", \"type\": \"${_type}\", \"file\": \"${_file}\",\n"
    "     \"includes\": \"$<TARGET_PROPERTY:${t},INTERFACE_INCLUDE_DIRECTORIES>\",\n"
    "     \"defines\": \"$<TARGET_PROPERTY:${t},INTERFACE_COMPILE_DEFINITIONS>\",\n"
    "     \"options\": \"$<TARGET_PROPERTY:${t},INTERFACE_COMPILE_OPTIONS>\",\n"
    "     \"link_options\": \"$<TARGET_PROPERTY:${t},INTERFACE_LINK_OPTIONS>\",\n"
    "     \"links\": \"$<TARGET_PROPERTY:${t},INTERFACE_LINK_LIBRARIES>\"}")
  set(_sep ",\n")
endforeach()
string(APPEND _json "\n  ]\n}\n")
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/sdk/targets.json CONTENT "${_json}")

# `ninja cw_sdk`: everything the SDK ships from this tree
add_custom_target(cw_sdk)
add_dependencies(cw_sdk ${_cw_sdk_libs})
if(_cw_sdk_host_tools)
  add_dependencies(cw_sdk cwpack)
endif()
