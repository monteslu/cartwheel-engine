# macOS arm64, built on a Mac (Xcode's clang, Ninja): WebGPU through Dawn on
# Metal, GLES 3.0 through ANGLE on Metal (native-gles' build). Both dylibs
# ship in the app bundle's Contents/Frameworks (tools/cwexport/apple.mjs).
set(CW_EXPORT_NATIVE_APPLE ON)
include(${CMAKE_CURRENT_LIST_DIR}/common.cmake)
set(CW_EXPORT_TARGET macos-arm64)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
cw_export_dawn(darwin-arm64)
set(CW_ANGLE_INCLUDE ${CW_EXPORT_DEPS}/angle-gles-darwin-arm64/include CACHE PATH "" FORCE)
set(CW_ANGLE_GLESV2 ${CW_EXPORT_DEPS}/angle-gles-darwin-arm64/lib/libGLESv2.dylib CACHE FILEPATH "" FORCE)
# the bundle's Frameworks (Contents/MacOS/../Frameworks), and beside the
# executable for a build-tree run
set(CMAKE_BUILD_RPATH "@executable_path/../Frameworks;@executable_path" CACHE STRING "" FORCE)
set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks;@executable_path" CACHE STRING "" FORCE)
