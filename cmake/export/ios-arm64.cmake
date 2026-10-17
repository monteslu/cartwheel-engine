# iOS arm64: devices, or with -DCW_IOS_SIM=ON the simulator on an Apple
# silicon Mac. Built on a Mac with Xcode's toolchain: Ninja for the engine's
# libraries (scripts/build-sdk.sh ios-arm64 / ios-arm64-simulator) and for a
# plain build, Xcode (cmake -G Xcode) for an app bundle that signs, archives
# and runs (tools/cwexport's ios export).
#
# Rendering: GLES 3.0 through ANGLE's Metal backend. SDL2's UIKit driver has
# no EGL, so src/platform/egl_uikit.c drives ANGLE's EGL itself on the
# window's CAMetalLayer. ANGLE comes as kivy/angle-builder's xcframeworks
# (scripts/deps.lock export/angle-ios: libGLESv2 and libEGL, a device slice
# and a simulator slice); the slice for this build is linked and embedded in
# the app (cmake/app.cmake). They declare iOS 16 as their minimum, so that is
# the deployment target. WebGPU (Dawn on Metal) joins when native-dawn's
# ios-arm64 build is fetched (deps/export/native-dawn-ios-arm64).
set(CW_EXPORT_NATIVE_APPLE ON)
option(CW_IOS_SIM "build for the iOS simulator (arm64) instead of devices" OFF)
get_filename_component(_cw_root ${CMAKE_CURRENT_LIST_DIR}/../.. REALPATH)
# cwpack runs on the Mac: the macOS export tree's (scripts/build-sdk.sh and
# the exporter build it first; common.cmake would name the dev tree's)
set(CW_CWPACK ${_cw_root}/build/export/macos/tools/cwpack/cwpack CACHE FILEPATH "host cwpack")
include(${CMAKE_CURRENT_LIST_DIR}/common.cmake)
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "16.0" CACHE STRING "" FORCE)
if(CW_IOS_SIM)
  set(CMAKE_OSX_SYSROOT iphonesimulator CACHE STRING "" FORCE)
  set(CW_EXPORT_TARGET ios-arm64-simulator)
  set(_cw_angle_slice ios-arm64_x86_64-simulator)
else()
  set(CMAKE_OSX_SYSROOT iphoneos CACHE STRING "" FORCE)
  set(CW_EXPORT_TARGET ios-arm64)
  set(_cw_angle_slice ios-arm64)
endif()
cw_export_dawn(${CW_EXPORT_TARGET})
set(_cw_angle ${CW_EXPORT_DEPS}/angle-ios)
if(NOT EXISTS ${_cw_angle}/libGLESv2.xcframework/${_cw_angle_slice}/libGLESv2.framework)
  message(FATAL_ERROR "deps/export/angle-ios is missing: run scripts/fetch-deps.sh --only export/angle-ios")
endif()
set(CW_ANGLE_INCLUDE ${_cw_angle}/include CACHE PATH "" FORCE)
# the frameworks of this build's slice: linked by cw_gles (cmake/deps.cmake)
# and embedded in every app bundle (cmake/app.cmake)
set(CW_ANGLE_GLESV2 ${_cw_angle}/libGLESv2.xcframework/${_cw_angle_slice}/libGLESv2.framework CACHE FILEPATH "" FORCE)
set(CW_ANGLE_EGL ${_cw_angle}/libEGL.xcframework/${_cw_angle_slice}/libEGL.framework CACHE FILEPATH "" FORCE)
# cw_add_app makes each app an iOS bundle (Info.plist: CW_APPLE_INFO_PLIST)
set(CW_APPLE_BUNDLE ON CACHE BOOL "" FORCE)
