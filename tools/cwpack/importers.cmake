# cw_importers: cwpack's importers (glTF, textures/KTX2, audio) as a library,
# linked by cwpack and, in process, by the editor (natively and as a cart).
# It brings no engine core of its own: cwpack links cw_toolcore, the editor
# the engine. In a cart the Basis Universal encoder is a stub that reports
# why KTX2 encoding is unavailable (basis_stub.c).
set(_cwp ${CMAKE_SOURCE_DIR}/tools/cwpack)
set(_cwp_srcs ${_cwp}/import_gltf.c ${_cwp}/import_texture.c ${_cwp}/import_audio.c ${_cwp}/util.c)
if(CW_CART)
  list(APPEND _cwp_srcs ${_cwp}/basis_stub.c)
else()
  list(APPEND _cwp_srcs ${_cwp}/basis_enc.cpp)
endif()
add_library(cw_importers STATIC ${_cwp_srcs})
target_include_directories(cw_importers PUBLIC ${_cwp} ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/resource
  ${CW_THREE_DIR}/include)
target_compile_options(cw_importers PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers>)
target_link_libraries(cw_importers PUBLIC three meshoptimizer cw_opus cw_ogg cw_miniaudio)
if(NOT CW_CART)
  target_link_libraries(cw_importers PUBLIC cw_basisu_encoder) # defined in tools/cwpack/CMakeLists.txt
  # The encoder archive carries its own (full) Basis transcoder and the whole
  # of zstd; the engine links the trimmed transcoder + zstd's decoder. In an
  # app with both (the native editor) the encoder's copies must be found
  # first, so they serve the engine too and the engine's archive adds nothing
  # (else: duplicate symbols). LINK_ONLY orders the link without passing the
  # engine's BASISD_* definitions to the encoder's consumers.
  set_property(TARGET cw_basisu_encoder APPEND PROPERTY INTERFACE_LINK_LIBRARIES $<LINK_ONLY:cw_basisu_transcoder>)
endif()
