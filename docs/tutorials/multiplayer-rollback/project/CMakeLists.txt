# A cartwheel project. The engine tree builds it (any target, any toolchain):
#   node <cartwheel>/tools/cwproject.mjs build <this directory> [native|cart]
# configures cartwheel with -DCW_PROJECT_DIR=<this directory> -DCW_ENGINE_ONLY=ON
# into .cwbuild/<target>/, which adds this file as a subdirectory, so all it
# does is declare the game with cw_add_app (cartwheel's CMakeLists.txt):
# natively an executable (+ <app>.cwpak), as a cart <app>.wasc.
# The app's name is `app` in project.cwproj; its sources are src/*.c; what
# ships is what roots.txt reaches (cwpack follows scene references).
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/project.cwproj _cw_app REGEX "^app *=")
string(REGEX REPLACE "^app *= *\"?([A-Za-z_][A-Za-z0-9_]*)\"?.*$" "\\1" CW_APP "${_cw_app}")
if(NOT CW_APP MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
  message(FATAL_ERROR "project.cwproj: set `app = <name>` (a C identifier: the executable and cart name)")
endif()
file(GLOB CW_APP_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
cw_add_app(${CW_APP} SOURCES ${CW_APP_SRCS} PACK ${CMAKE_CURRENT_SOURCE_DIR}/roots.txt)
