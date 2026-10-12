# Games: complete projects built on the engine (each one also a gate).

# platformer3d, a 3D physics platformer (title, two levels,
# end screen). Assets are packed by cwpack from assets/roots.txt;
# platformer3d_auto plays the gate's input script itself (input-free hosts).
set(P3D ${CMAKE_CURRENT_SOURCE_DIR}/platformer3d)
set(CW_PROGRAMS ${P3D}/programs/platformer3d.txt) # its three.c program table (cmake/programs.cmake), editors included
cw_add_app(platformer3d SOURCES ${P3D}/main.c ${P3D}/nodes.c PACK ${P3D}/assets/roots.txt)
cw_add_app(platformer3d_auto SOURCES ${P3D}/main.c ${P3D}/nodes.c PACK ${P3D}/assets/roots.txt)
target_compile_definitions(platformer3d_auto PRIVATE PLATFORMER3D_AUTOPILOT=1)
# both packs share the import cache beside roots.txt: run them one after another
add_dependencies(platformer3d_auto_pack platformer3d_pack)
# the editor's bundle for this game: the game's assets plus the project
# templates (cwtemplates/, as editor/CMakeLists.txt stages them for the
# editor), so native and cart editors show the same Projects panel
set(P3D_EDITOR_ASSETS ${CMAKE_CURRENT_BINARY_DIR}/platformer3d_editor_assets)
file(MAKE_DIRECTORY ${P3D_EDITOR_ASSETS})
file(GLOB _p3d_assets RELATIVE ${P3D}/assets ${P3D}/assets/*)
foreach(f ${_p3d_assets})
  file(CREATE_LINK ${P3D}/assets/${f} ${P3D_EDITOR_ASSETS}/${f} SYMBOLIC)
endforeach()
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/editor/templates ${P3D_EDITOR_ASSETS}/cwtemplates SYMBOLIC)
file(GLOB_RECURSE _p3d_tpl RELATIVE ${CMAKE_SOURCE_DIR}/editor/templates CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/editor/templates/*)
list(SORT _p3d_tpl)
list(FILTER _p3d_tpl EXCLUDE REGEX "(^|/)\\.")
string(REPLACE ";" "\n" _p3d_tpl "${_p3d_tpl}")
file(CONFIGURE OUTPUT ${P3D_EDITOR_ASSETS}/cwtemplates.txt CONTENT "${_p3d_tpl}\n" @ONLY)
# the editor with platformer3d's node types, so the game's scenes open,
# edit and save in it (gate editor-level rebuilds level 1 with it)
cw_add_app(platformer3d_editor SOURCES ${CMAKE_SOURCE_DIR}/editor/app/main.c ${P3D}/nodes.c ${P3D}/editor_types.c ASSETS ${P3D_EDITOR_ASSETS} GL_ONLY)
target_link_libraries(platformer3d_editor PRIVATE cartwheel_editor)
target_compile_definitions(platformer3d_editor PRIVATE CWED_APP_TYPES=platformer3d_editor_types)
# Gate apps: the editor with platformer3d's types playing a scripted
# session (editor/tests/session_*.inc): the 3D view, a 2D scene, a pointer
# gizmo session
foreach(s view3d view2d gizmo pointer)
  cw_add_app(platformer3d_editor_${s} SOURCES ${CMAKE_SOURCE_DIR}/editor/app/main.c ${P3D}/nodes.c ${P3D}/editor_types.c ASSETS ${P3D_EDITOR_ASSETS} GL_ONLY)
  target_link_libraries(platformer3d_editor_${s} PRIVATE cartwheel_editor)
  target_compile_definitions(platformer3d_editor_${s} PRIVATE CWED_APP_TYPES=platformer3d_editor_types EDITOR_SESSION=1
                             EDITOR_SESSION_INC="../tests/session_${s}.inc")
endforeach()

# action2d ("Crypt of Embers"): a top-down action game from .cwscene
# files; _auto plays the gate's pad script itself (input-free hosts).
set(CW_PROGRAMS "")
set(ACTION2D_SRC action2d/main.c action2d/actors.c action2d/bot.c)
cw_add_app(action2d SOURCES ${ACTION2D_SRC} ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/action2d/assets)
cw_add_app(action2d_auto SOURCES ${ACTION2D_SRC} ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/action2d/assets)
target_compile_definitions(action2d_auto PRIVATE ACTION2D_AUTOPILOT=1)

set(CW_PROGRAMS "")
