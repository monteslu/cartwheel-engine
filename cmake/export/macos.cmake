# macOS universal export (arm64 + x86_64). RUN ON A MAC with Xcode: Apple's
# SDK cannot be fetched or used elsewhere. tools/cwexport's macOS export
# writes a build-on-mac.sh that configures with this file (cmake -G Xcode).
# GLES 3.0 through ANGLE's Metal backend (deps/export/angle-macos, universal
# dylibs shipped in the app's Contents/Frameworks).
get_filename_component(CW_EXPORT_DEPS ${CMAKE_CURRENT_LIST_DIR}/../../deps/export REALPATH)
set(CW_EXPORT_TARGET macos)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
set(CW_SDL2_FROM_SOURCE ON CACHE BOOL "" FORCE)
set(CW_ANGLE_INCLUDE ${CW_EXPORT_DEPS}/angle-macos/include CACHE PATH "" FORCE)
set(CW_ANGLE_GLESV2 ${CW_EXPORT_DEPS}/angle-macos/libGLESv2.dylib CACHE FILEPATH "" FORCE)
set(CMAKE_BUILD_RPATH "@executable_path/../Frameworks" CACHE STRING "" FORCE)
set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks" CACHE STRING "" FORCE)
