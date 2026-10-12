# Windows x64 export: llvm-mingw (clang + lld + mingw-w64 UCRT), GLES 3.0
# through ANGLE's D3D11 backend (MSYS2's clang64 build; its DLLs ship beside
# the exe). The exe links its C++ runtime and winpthreads statically.
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
set(CW_ANGLE_INCLUDE ${CW_EXPORT_DEPS}/angle-macos/include CACHE PATH "" FORCE) # ANGLE's Khronos + ANGLE headers
set(CW_ANGLE_GLESV2 ${CW_EXPORT_DEPS}/angle-win-x64/lib/libGLESv2.dll.a CACHE FILEPATH "" FORCE)
# what ships beside the exe (tools/cwexport copies these)
set(CW_EXPORT_RUNTIME_DLLS
  ${CW_EXPORT_DEPS}/angle-win-x64/bin/libEGL.dll ${CW_EXPORT_DEPS}/angle-win-x64/bin/libGLESv2.dll
  ${CW_EXPORT_DEPS}/zlib-win-x64/bin/zlib1.dll
  ${CW_EXPORT_DEPS}/llvm-mingw/x86_64-w64-mingw32/bin/libc++.dll
  ${CW_EXPORT_DEPS}/llvm-mingw/x86_64-w64-mingw32/bin/libunwind.dll)
# <Windows.h> as MSVC spells it (box3d) on a case-sensitive host
set(CMAKE_C_FLAGS_INIT "-idirafter ${CMAKE_CURRENT_LIST_DIR}/win-include")
set(CMAKE_CXX_FLAGS_INIT "-idirafter ${CMAKE_CURRENT_LIST_DIR}/win-include")
