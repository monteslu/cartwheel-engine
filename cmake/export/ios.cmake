# iOS export (arm64 devices). RUN ON A MAC with Xcode (cmake -G Xcode): Apple's
# SDK and signing exist only there. tools/cwexport's iOS export writes a
# build-on-mac.sh that uses this file. GLES 3.0 through ANGLE's Metal backend
# (deps/export/angle-ios xcframeworks, embedded in the app).
get_filename_component(CW_EXPORT_DEPS ${CMAKE_CURRENT_LIST_DIR}/../../deps/export REALPATH)
get_filename_component(_cw_root ${CMAKE_CURRENT_LIST_DIR}/../.. REALPATH)
set(CW_EXPORT_TARGET ios)
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "" FORCE)
set(CW_SDL2_FROM_SOURCE ON CACHE BOOL "" FORCE)
set(CW_ANGLE_INCLUDE ${CW_EXPORT_DEPS}/angle-ios/include CACHE PATH "" FORCE)
set(CW_ANGLE_GLESV2 ${CW_EXPORT_DEPS}/angle-ios/libGLESv2.xcframework CACHE FILEPATH "" FORCE)
# cwpack runs on the Mac: build/export/macos's (build-on-mac.sh builds it first)
set(CW_CWPACK ${_cw_root}/build/export/macos/tools/cwpack/cwpack CACHE FILEPATH "host cwpack")
# cw_add_app makes each app an iOS bundle with this Info.plist
set(CW_APPLE_BUNDLE ON CACHE BOOL "" FORCE)
