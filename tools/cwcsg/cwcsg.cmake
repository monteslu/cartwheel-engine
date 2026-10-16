# cwcsg: bakes CSG shapes into meshes (cartwheel/csg.h) with Manifold
# (deps/manifold, Apache-2.0), built here from its sources: the core, its
# C API (manifoldc) and the dependency-free 2D backend the C API needs
# (boolean2; no Clipper2, no TBB). Native tool only: games and carts never
# link Manifold or any C++ of it.
set(_mf ${DEPS}/manifold)
set(_mf_srcs boolean3 boolean_result constructors csg_tree edge_op execution_impl face_op impl manifold minkowski polygon
    properties quickhull sdf smoothing sort subdivision tree2d cross_section/cross_section_boolean2)
list(TRANSFORM _mf_srcs PREPEND ${_mf}/src/)
list(TRANSFORM _mf_srcs APPEND .cpp)
add_library(cw_manifold STATIC ${_mf_srcs} ${_mf}/bindings/c/manifoldc.cpp ${_mf}/bindings/c/conv.cpp
  ${_mf}/bindings/c/box.cpp ${_mf}/bindings/c/cross.cpp ${_mf}/bindings/c/rect.cpp)
target_include_directories(cw_manifold PUBLIC ${_mf}/include ${_mf}/bindings/c/include PRIVATE ${_mf}/bindings/c)
# MANIFOLD_PAR=-1: the sequential backend (one deterministic order, no TBB)
target_compile_definitions(cw_manifold PUBLIC MANIFOLD_CROSS_SECTION MANIFOLD_NO_IOSTREAM MANIFOLD_NO_FILESYSTEM MANIFOLD_PAR=-1)
target_compile_features(cw_manifold PUBLIC cxx_std_17)
target_compile_options(cw_manifold PRIVATE -O2 -w)

add_executable(cwcsg ${CMAKE_SOURCE_DIR}/tools/cwcsg/main.c)
target_link_libraries(cwcsg PRIVATE cartwheel cw_manifold m)
set_target_properties(cwcsg PROPERTIES LINKER_LANGUAGE CXX)
