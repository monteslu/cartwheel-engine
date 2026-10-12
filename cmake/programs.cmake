# ── three.c program tables (src/render/programs.c) ──────────────────────
# three.c draws a material with the program three.js r186 generates for its
# state (material kind, features, light counts), looked up in tables. The
# engine links three.c's core table only (CW_THREE_TABLE, cmake/deps.cmake):
# an app's material programs come from program tables.
#
# A table is a manifest of states, one per line (# comments), in <name>.txt,
# and the C three.c's tools/gen-project-table.mjs generates from it beside the
# manifest: <name>_gl.c and <name>_wgpu.c. All three are checked in, so a
# build needs only a C compiler.
#
#   cw_add_app(app SOURCES ... PROGRAMS path/to/<name>.txt [...])
#
# links the tables and registers them before the first render. Without
# PROGRAMS an app uses <its directory>/programs/<app>.txt when that exists,
# else the directory's CW_PROGRAMS (a list of manifests: samples/CMakeLists.txt
# sets the samples' shared table). Libraries register their own: the engine
# cw_engine (src/render/programs/cw_engine.txt, what it draws on its own
# behalf), the editor cw_editor (editor/programs/cw_editor.txt).
#
# Regenerating (Node, three.c's npm deps and the test GPU; never part of a
# plain build):
#   ninja <name>_programs     one table, from its manifest
#   ninja programs            every table
#   node tools/programs.mjs [app ...] [--gates]
#                             run the apps (and their gates), add the states
#                             they could not draw to their first table,
#                             regenerate
# A table whose generated C is missing builds as an empty table: its draws are
# skipped and reported, and the gates fail with the states to add.

if(NOT TARGET programs AND NOT CW_CART AND NOT CMAKE_CROSSCOMPILING)
  add_custom_target(programs COMMENT "three.c program tables")
endif()

function(cw__write_if_changed path content)
  set(_old "")
  if(EXISTS ${path})
    file(READ ${path} _old)
  endif()
  if(NOT _old STREQUAL content)
    file(WRITE ${path} "${content}")
  endif()
endfunction()

# cw_program_table(<manifest> <out-var>): the table library cwprog_<name> for
# a manifest (made once), its name in <out-var>
function(cw_program_table manifest out_name)
  get_filename_component(manifest ${manifest} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
  get_filename_component(tname ${manifest} NAME_WE)
  get_filename_component(tdir ${manifest} DIRECTORY)
  if(NOT tname MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR "program table ${manifest}: the name ${tname} is not a C identifier")
  endif()
  set(${out_name} ${tname} PARENT_SCOPE)
  if(TARGET cwprog_${tname})
    get_target_property(_was cwprog_${tname} CW_MANIFEST)
    if(NOT _was STREQUAL manifest)
      message(FATAL_ERROR "program table ${tname}: two manifests (${_was}, ${manifest})")
    endif()
    return()
  endif()
  if(NOT EXISTS ${manifest})
    message(FATAL_ERROR "program table ${manifest} does not exist")
  endif()
  set(_srcs "")
  foreach(be gl wgpu)
    if(be STREQUAL "wgpu" AND NOT CW_WGPU)
      continue()
    endif()
    set(_c ${tdir}/${tname}_${be}.c)
    if(NOT EXISTS ${_c})
      # not generated yet: an empty table, so the app builds and reports what it lacks
      set(_c ${CMAKE_BINARY_DIR}/programs/${tname}_${be}.c)
      cw__write_if_changed(${_c} "/* GENERATED placeholder (cmake/programs.cmake): ${tname}_${be}.c is not generated yet. */\n#include \"gen/programs.h\"\nconst t3_gen_table ${tname}_${be} = { 0, 0 };\n")
    endif()
    list(APPEND _srcs ${_c})
  endforeach()
  add_library(cwprog_${tname} STATIC ${_srcs})
  # position-independent: an app's hot module (cwhot) links its tables too
  set_target_properties(cwprog_${tname} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(cwprog_${tname} PRIVATE ${CW_THREE_DIR}/src ${CW_THREE_DIR}/include)
  target_link_libraries(cwprog_${tname} PRIVATE cw_gles) # three.c's backend.h includes the GLES headers
  target_compile_options(cwprog_${tname} PRIVATE -O2 -w)
  set_target_properties(cwprog_${tname} PROPERTIES CW_MANIFEST ${manifest})
  # a new or removed generated file changes the sources: reconfigure when the manifest changes
  set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${manifest})
  if(TARGET programs)
    # tools/programs.mjs enters the GPU sandbox and runs three.c's generator
    add_custom_target(${tname}_programs
      COMMAND node ${CMAKE_SOURCE_DIR}/tools/programs.mjs --table ${manifest} --build ${CMAKE_BINARY_DIR}
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} USES_TERMINAL
      COMMENT "three.c program table ${tname}")
    add_dependencies(programs ${tname}_programs)
  endif()
endfunction()

# an app's tables: link them and generate <app>_programs.c, whose
# cw__app_programs() registers them (src/render/programs.c calls it)
function(cw__app_programs name)
  set(_manifests ${ARGN})
  if(NOT _manifests AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/programs/${name}.txt)
    set(_manifests ${CMAKE_CURRENT_SOURCE_DIR}/programs/${name}.txt)
  endif()
  if(NOT _manifests AND CW_PROGRAMS)
    set(_manifests ${CW_PROGRAMS})
  endif()
  set(_abs "")
  set(_libs "")
  set(_externs "")
  set(_calls "")
  set(_hot "")
  set(_i 0)
  foreach(m ${_manifests})
    get_filename_component(m ${m} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    list(APPEND _abs ${m})
    cw_program_table(${m} t)
    target_link_libraries(${name} PRIVATE cwprog_${t})
    list(APPEND _libs cwprog_${t})
    if(CW_WGPU)
      string(APPEND _externs "extern const struct t3_gen_table ${t}_gl, ${t}_wgpu;\n")
      string(APPEND _calls "  t3_register_program_table(&${t}_gl, &${t}_wgpu);\n")
      string(APPEND _hot "  if (${_i} < cap) gl[${_i}] = &${t}_gl, wgpu[${_i}] = &${t}_wgpu;\n")
    else()
      string(APPEND _externs "extern const struct t3_gen_table ${t}_gl;\n")
      string(APPEND _calls "  t3_register_program_table(&${t}_gl, 0);\n")
      string(APPEND _hot "  if (${_i} < cap) gl[${_i}] = &${t}_gl, wgpu[${_i}] = 0;\n")
    endif()
    math(EXPR _i "${_i} + 1")
  endforeach()
  set_property(TARGET ${name} PROPERTY CW_PROGRAMS "${_abs}")
  if(NOT _abs)
    return() # the engine's weak default registers none
  endif()
  # cw__app_programs() registers the tables in the app; in its hot module
  # (cw_add_app's <app>_hot) cwhot asks cw__app_program_tables() for them
  # instead and keeps them current across reloads (tools/cwhot/main.c)
  set(_gen ${CMAKE_CURRENT_BINARY_DIR}/${name}_programs.c)
  cw__write_if_changed(${_gen} "/* GENERATED by cw_add_app (cmake/programs.cmake): ${name}'s three.c program tables. */\nstruct t3_gen_table;\nvoid t3_register_program_table(const struct t3_gen_table *gl_table, const struct t3_gen_table *wgpu_table);\n${_externs}void cw__app_programs(void);\nvoid cw__app_programs(void) {\n${_calls}}\nint cw__app_program_tables(const struct t3_gen_table **gl, const struct t3_gen_table **wgpu, int cap);\nint cw__app_program_tables(const struct t3_gen_table **gl, const struct t3_gen_table **wgpu, int cap) {\n${_hot}  return ${_i};\n}\n")
  target_sources(${name} PRIVATE ${_gen})
  set_property(TARGET ${name} PROPERTY CW_PROGRAMS_GEN "${_gen}")
  set_property(TARGET ${name} PROPERTY CW_PROGRAMS_LIBS "${_libs}")
endfunction()

# the engine's own table (impostor baking and the like), registered by src/render/programs.c
cw_program_table(${CMAKE_SOURCE_DIR}/src/render/programs/cw_engine.txt _t)
target_link_libraries(cartwheel PUBLIC cwprog_cw_engine)
