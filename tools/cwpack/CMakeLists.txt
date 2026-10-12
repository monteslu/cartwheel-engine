# cwpack: the native import + pack tool (never built for carts). It links
# the engine's core (not the platform layer), three.c for cgltf + stb_image,
# meshoptimizer, and Basis Universal's encoder.
set(_bu ${CW_BASISU})
set(_bu_enc backend basis_file comp enc etc frontend gpu_texture pvrtc1_4 resampler resample_filters ssim
    uastc_enc bc7e_scalar dds_export bc7enc kernels_sse bc15_spmd bc15_spmd_sse opencl uastc_hdr_4x4_enc
    astc_hdr_6x6_enc astc_hdr_common astc_ldr_common astc_ldr_encode astc_ldr_fencode xbc7_encode tinyexr)
list(TRANSFORM _bu_enc PREPEND ${_bu}/encoder/basisu_)
list(TRANSFORM _bu_enc APPEND .cpp)
add_library(cw_basisu_encoder STATIC ${_bu_enc} ${_bu}/encoder/jpgd.cpp ${_bu}/encoder/pvpngreader.cpp
  ${_bu}/encoder/3rdparty/android_astc_decomp.cpp ${_bu}/transcoder/basisu_transcoder.cpp ${_bu}/zstd/zstd.c)
target_include_directories(cw_basisu_encoder PUBLIC ${_bu}/encoder ${_bu}/transcoder PRIVATE ${_bu}/zstd)
target_compile_definitions(cw_basisu_encoder PUBLIC BASISU_SUPPORT_SSE=0 BASISU_SUPPORT_OPENCL=0 BASISU_SUPPORT_ASTCENC=0)
target_compile_options(cw_basisu_encoder PRIVATE -O2 -w -fno-strict-aliasing)
target_link_libraries(cw_basisu_encoder PUBLIC pthread)

file(GLOB _core ${CMAKE_SOURCE_DIR}/src/core/*.c)
add_library(cw_toolcore STATIC ${_core} ${CMAKE_SOURCE_DIR}/src/resource/json.c ${CMAKE_SOURCE_DIR}/src/l10n/catalog.c
  ${CMAKE_SOURCE_DIR}/src/resource/gltf_doc.c ${CMAKE_SOURCE_DIR}/src/resource/uid.c)
target_include_directories(cw_toolcore PUBLIC ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/resource ${CMAKE_SOURCE_DIR}/src/l10n
  ${CW_THREE_DIR}/include)
target_link_libraries(cw_toolcore PUBLIC meshoptimizer m pthread)

add_executable(cwpack main.c util.c import_gltf.c import_texture.c import_audio.c import_font.c basis_enc.cpp)
target_include_directories(cwpack PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${DEPS}/meshoptimizer/extern)
target_compile_options(cwpack PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers>)
target_link_libraries(cwpack PRIVATE cw_toolcore cw_basisu_encoder three cw_opus cw_ogg cw_miniaudio m)
