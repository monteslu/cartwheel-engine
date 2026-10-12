# the unit apps' three.c program table (cmake/programs.cmake)
set(CW_PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/programs/cw_tests.txt)
# Unit test suites: apps that run natively and as carts (see test.h).
cw_add_app(unit_core SOURCES test_core.c GL_ONLY)
cw_add_app(unit_math SOURCES test_math.c GL_ONLY)
cw_add_app(unit_physics SOURCES test_physics.c GL_ONLY)
cw_add_app(unit_vehicle3d SOURCES test_vehicle3d.c GL_ONLY)
cw_add_app(unit_scene SOURCES test_scene.c GL_ONLY)
cw_add_app(unit_timing SOURCES test_timing.c GL_ONLY)
cw_add_app(unit_input SOURCES test_input.c GL_ONLY)
cw_add_app(unit_audio SOURCES test_audio.c GL_ONLY)
cw_add_app(unit_physics_nodes SOURCES test_physics_nodes.c GL_ONLY)
cw_add_app(unit_resource SOURCES test_resource.c PACK ${CMAKE_CURRENT_SOURCE_DIR}/res_project/roots.txt GL_ONLY)
# M3.1: the render pipeline (pass graph, POST chain, pool, LUT parser)
cw_add_app(unit_pipeline SOURCES test_pipeline.c GL_ONLY)
# Hot reload watches files and reruns cwpack: native dev builds only.
if(NOT CW_CART)
  cw_add_app(unit_hotreload SOURCES test_hotreload.c)
  add_dependencies(unit_hotreload cwpack)
endif()
# M2.7 animation. The glTF sample model comes from deps (Khronos glTF sample,
# CC-BY-4.0, license copied beside it) and is staged at configure time, not
# committed.
set(CW_ANIM_TEST_ASSETS ${CMAKE_CURRENT_BINARY_DIR}/unit_anim_assets)
file(MAKE_DIRECTORY ${CW_ANIM_TEST_ASSETS})
configure_file(${CW_THREE_DIR}/test/assets/CesiumMan.glb ${CW_ANIM_TEST_ASSETS}/CesiumMan.glb COPYONLY)
configure_file(${CW_THREE_DIR}/test/assets/CesiumMan.LICENSE.md ${CW_ANIM_TEST_ASSETS}/CesiumMan.LICENSE.md COPYONLY)
cw_add_app(unit_anim SOURCES test_anim.c ASSETS ${CW_ANIM_TEST_ASSETS} GL_ONLY)
cw_add_app(unit_ui SOURCES test_ui.c GL_ONLY)
# M4.6: project.cwproj editing and settings (cartwheel/project.h), child processes (cartwheel/process.h)
cw_add_app(unit_project SOURCES test_project.c GL_ONLY)
# M3.4 / M3.6: effects and visibility internals (BVH vs brute force, curves,
# the particle emission function, MeshInstance3D LOD hooks)
cw_add_app(unit_fx SOURCES test_fx.c GL_ONLY)
target_include_directories(unit_fx PRIVATE ${CMAKE_SOURCE_DIR}/src)
# Native only: the solver spin-wait stress scripts/sanitize.sh runs (see the file)
if(NOT CW_CART)
  add_executable(unit_stress_solver_spin stress_solver_spin.c)
  target_link_libraries(unit_stress_solver_spin PRIVATE cartwheel)
endif()
# M5.4 networking: channels, snapshots, RPC + replication, rollback (cartwheel/net.h)
cw_add_app(unit_net SOURCES test_net.c GL_ONLY)
target_include_directories(unit_net PRIVATE ${CMAKE_SOURCE_DIR}/src)
# M5.5 localization + accessibility (catalogs, plurals vs CLDR samples, PO
# round trip, RTL layout, a11y tree, captions, shaping); assets: the l10n
# demo's pack (catalogs, Noto subsets)
cw_add_app(unit_l10n SOURCES test_l10n.c PACK ${CMAKE_SOURCE_DIR}/samples/l10n_demo/project/roots.txt GL_ONLY)
# M5.2 plugins: the build's plugin table (an accepted, a refused-by-version, a
# disabled and a refused-by-dependency fixture), version rules, unload/reload
cw_add_app(unit_plugin SOURCES test_plugin.c PLUGINS ${CMAKE_CURRENT_SOURCE_DIR}/plugin_fixtures QUIET_PLUGINS GL_ONLY)
# the 3D effects on both GPU backends from one scene,
# without the render pipeline (its own boxes, depth and pass context)
cw_add_app(fx_gpu SOURCES fx_gpu.c ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/fx_gpu_assets)
target_include_directories(fx_gpu PRIVATE ${CMAKE_SOURCE_DIR}/src)
# cw_app_desc.pixel_scale and crisp UI (tests/pixel-scale.mjs, tests/host-resolution.mjs):
# one source, four apps (pixel_scale.c says what each is for): name:N:render_scale:ui_min_lines
foreach(_px n2:2:3.125f:540 n3:3:3.125f:540 auto3:3:0:540 auto3_low:3:0:0)
  string(REPLACE ":" ";" _p ${_px})
  list(GET _p 0 _name)
  list(GET _p 1 _n)
  list(GET _p 2 _rs)
  list(GET _p 3 _ui)
  cw_add_app(pixel_${_name} SOURCES pixel_scale.c ASSETS ${CMAKE_SOURCE_DIR}/samples/r2d_assets)
  target_compile_definitions(pixel_${_name} PRIVATE PIXEL_N=${_n} PIXEL_RS=${_rs} PIXEL_UI=${_ui})
endforeach()
