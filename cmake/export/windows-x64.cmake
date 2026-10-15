# Windows x64 export: llvm-mingw (clang + lld + mingw-w64 UCRT), WebGPU
# through Dawn (D3D12) and GLES 3.0 through ANGLE's D3D11 backend, both DLLs
# that ship beside the exe (tools/cwexport/windows.mjs). The exe links its C++
# runtime and winpthreads statically.
include(${CMAKE_CURRENT_LIST_DIR}/common.cmake)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CW_EXPORT_TARGET windows-x64)
set(CMAKE_C_COMPILER ${CW_LLVM}/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${CW_LLVM}/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER ${CW_LLVM}/x86_64-w64-mingw32-windres)
set(CMAKE_AR ${CW_LLVM}/llvm-ar)
set(CMAKE_RANLIB ${CW_LLVM}/llvm-ranlib)
set(CMAKE_STRIP ${CW_LLVM}/llvm-strip)
set(CMAKE_FIND_ROOT_PATH ${CW_EXPORT_DEPS}/llvm-mingw/x86_64-w64-mingw32)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
cw_export_dawn(win32-x64)
# ANGLE (native-gles' build: D3D11, static CRT) and its import library
set(CW_ANGLE_INCLUDE ${CW_EXPORT_DEPS}/angle-gles-win32-x64/include CACHE PATH "" FORCE)
set(CW_ANGLE_GLESV2 ${CW_EXPORT_DEPS}/angle-gles-win32-x64/lib/libGLESv2.lib CACHE FILEPATH "" FORCE)
# <Windows.h> as MSVC spells it (box3d) on a case-sensitive host
set(CMAKE_C_FLAGS_INIT "-idirafter ${CMAKE_CURRENT_LIST_DIR}/win-include")
set(CMAKE_CXX_FLAGS_INIT "-idirafter ${CMAKE_CURRENT_LIST_DIR}/win-include")
