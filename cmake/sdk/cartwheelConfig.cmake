# cartwheel's prebuilt engine SDK as a CMake package (docs/sdk.md).
#
#   cmake_minimum_required(VERSION 3.20)
#   project(mygame C CXX)
#   find_package(cartwheel REQUIRED CONFIG)   # CMAKE_PREFIX_PATH=<sdk>
#   cw_add_app(mygame SOURCES main.c PACK ${CMAKE_CURRENT_SOURCE_DIR}/assets/roots.txt)
#
# Configure with the SDK's toolchain for its target: cmake/export/<target>.cmake
# for a native SDK built with one (Linux, Windows, Android), cmake/cart.cmake
# for a cart SDK. The engine's libraries were built with the determinism flags
# (-ffp-contract=off -fno-fast-math); every target in the directory that finds
# this package gets them too, and flags that would undo them are an error.

get_filename_component(CW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include(${CMAKE_CURRENT_LIST_DIR}/cartwheel-sdk-info.cmake)
set(CW_FROM_SDK ON)
set(DEPS ${CW_ROOT}/deps)

# the toolchain must build for the SDK's target
if(NOT CMAKE_SYSTEM_NAME STREQUAL CW_SDK_SYSTEM)
  message(FATAL_ERROR "cartwheel SDK ${CW_SDK_TARGET} is for ${CW_SDK_SYSTEM}, this build targets ${CMAKE_SYSTEM_NAME}: "
                      "configure with its toolchain (-DCMAKE_TOOLCHAIN_FILE=${CW_ROOT}/cmake/...)")
endif()
if(CW_SDK_CART)
  # the cart libraries are ThinLTO bitcode: only the clang that wrote them reads them
  if(NOT "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}" STREQUAL CW_SDK_C_COMPILER)
    message(FATAL_ERROR "cartwheel SDK ${CW_SDK_TARGET} was built with ${CW_SDK_C_COMPILER} (ThinLTO bitcode), this build uses "
                        "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}: use the SDK's wasi-sdk (${CW_ROOT}/scripts/fetch-deps.sh --only wasi-sdk)")
  endif()
endif()

# determinism: what the engine was compiled with, for the game's code too
string(TOUPPER "${CMAKE_BUILD_TYPE}" _cw_bt)
foreach(_v CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_C_FLAGS_${_cw_bt} CMAKE_CXX_FLAGS_${_cw_bt})
  if("${${_v}}" MATCHES "(-ffast-math|-Ofast|-ffp-contract=(fast|on)|-funsafe-math-optimizations|-fassociative-math|-freciprocal-math)")
    message(FATAL_ERROR "${_v} has ${CMAKE_MATCH_1}: cartwheel's simulation is bit-identical across platforms only "
                        "without FMA contraction and fast-math (the engine is built with -ffp-contract=off -fno-fast-math)")
  endif()
endforeach()
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CW_WGPU ${CW_SDK_WGPU})
set(CW_THREE_DIR ${CW_ROOT}/deps/three.c CACHE PATH "three.c headers and tools (the SDK's)")
set(CW_THREE_TABLE ${CW_SDK_THREE_TABLE})
set(CW_THREE_BUILDER OFF)
# cwpack, the asset packer: this SDK's own when it ships one (a native SDK for
# a desktop platform); otherwise the host's, -DCW_CWPACK=<host sdk>/bin/cwpack
if(EXISTS ${CW_ROOT}/bin/cwpack OR EXISTS ${CW_ROOT}/bin/cwpack.exe)
  if(NOT CW_CWPACK OR NOT EXISTS "${CW_CWPACK}")
    file(GLOB _cw_cwpack ${CW_ROOT}/bin/cwpack ${CW_ROOT}/bin/cwpack.exe)
    set(CW_CWPACK ${_cw_cwpack} CACHE FILEPATH "host cwpack" FORCE)
  endif()
endif()
include(${CW_ROOT}/cmake/flags.cmake)
# the libraries: lib/, or for a cart linked without ThinLTO (CW_CART_DEV,
# CW_CART_LTO=OFF) the same code compiled without it, lib/nolto/
set(CW_SDK_LIBDIR lib)
if(CW_CART AND NOT CW_CART_LTO)
  if(NOT CW_SDK_NOLTO)
    message(FATAL_ERROR "cartwheel SDK ${CW_SDK_TARGET} has no libraries for carts linked without ThinLTO (lib/nolto)")
  endif()
  set(CW_SDK_LIBDIR lib/nolto)
endif()
if(NOT CW_CWPACK OR NOT EXISTS "${CW_CWPACK}")
  message(FATAL_ERROR "no cwpack: pass -DCW_CWPACK=<the host's cartwheel SDK>/bin/cwpack (a cart or Android SDK ships none)")
endif()
if(NOT CW_CART AND NOT CMAKE_CROSSCOMPILING AND NOT TARGET cwpack)
  add_executable(cwpack IMPORTED)
  set_target_properties(cwpack PROPERTIES IMPORTED_LOCATION ${CW_CWPACK})
endif()

# the SDK's own native-dawn and ANGLE (a toolchain file may have named others)
if(CW_WGPU AND NOT CW_CART)
  set(CW_NATIVE_DAWN ${CW_ROOT}/deps/native-dawn CACHE PATH "native-dawn prefix (the SDK's)" FORCE)
endif()
if(CW_SDK_ANGLE_GLESV2)
  set(CW_ANGLE_GLESV2 ${CW_ROOT}/${CW_SDK_ANGLE_GLESV2} CACHE FILEPATH "ANGLE libGLESv2 (the SDK's)" FORCE)
endif()
if(CW_WGPU AND NOT CW_CART)
  find_package(Dawn REQUIRED CONFIG PATHS ${CW_ROOT}/deps/native-dawn NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/cartwheelTargets.cmake)
foreach(_t cartwheel cartwheel_main)
  if(TARGET ${_t})
    set_property(TARGET ${_t} APPEND PROPERTY INTERFACE_COMPILE_OPTIONS -ffp-contract=off -fno-fast-math)
  endif()
endforeach()

include(${CW_ROOT}/cmake/programs.cmake)
include(${CW_ROOT}/cmake/plugins.cmake)
include(${CW_ROOT}/cmake/app.cmake)
# carts.txt once every cw_add_app has run
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL cw__write_carts_txt)
set(cartwheel_FOUND TRUE)
