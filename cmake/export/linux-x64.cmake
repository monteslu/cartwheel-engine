# Linux x64 export: clang + lld (llvm-mingw's, which target every OS) against
# the Debian bullseye sysroot (glibc 2.31: the ABI floor of the export).
include(${CMAKE_CURRENT_LIST_DIR}/common.cmake)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CW_EXPORT_TARGET linux-x64)
set(CMAKE_SYSROOT ${CW_EXPORT_DEPS}/sysroot-linux-x64)
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_C_COMPILER ${CW_LLVM}/clang)
set(CMAKE_CXX_COMPILER ${CW_LLVM}/clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_AR ${CW_LLVM}/llvm-ar)
set(CMAKE_RANLIB ${CW_LLVM}/llvm-ranlib)
set(CMAKE_STRIP ${CW_LLVM}/llvm-strip)
# llvm-mingw's clang defaults to its own runtimes: use the sysroot's GCC ones
set(CMAKE_C_FLAGS_INIT "")
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libstdc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld --rtlib=libgcc --unwindlib=libgcc")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld --rtlib=libgcc --unwindlib=libgcc")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld --rtlib=libgcc --unwindlib=libgcc")
# pkg-config (SDL's dependency probes) answers from the sysroot, never the host
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${CMAKE_SYSROOT})
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig:${CMAKE_SYSROOT}/usr/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
# X11 (and XWayland on Wayland desktops) + KMSDRM: the sysroot's wayland
# (1.18) predates what this host's wayland-scanner generates (1.20's
# wl_proxy_marshal_flags), so SDL's native Wayland backend is left out
set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
