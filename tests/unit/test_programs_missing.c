/* An app that leaves a program state no table has: three kinds of light on one box, a light vector the unit tests'
 * table lacks. It exists for tests/programs-carts.mjs, which surveys its DUAL cart (tools/programs.mjs --carts) and
 * must find the state; the app is not a gate (a gate fails on a missing state, which is the point here). The pipeline
 * reports the state in the debug field cw.three.missing_states, which is what the survey reads on every host. */
#include "cartwheel/pipeline.h"
#include "cartwheel/render3d.h"
#include "test.h"

static void test_leave_a_missing_state(void) {
  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  cw_node_add_child(cw_tree_root(t), cw_node_new("Camera3D", "Cam"));
  cw_node box = cw_node_new("MeshInstance3D", "Box");
  cw_node3d_set_position(box, (t3_vec3){ 0, 0, -3 });
  cw_node_add_child(cw_tree_root(t), box);
  cw_node_add_child(cw_tree_root(t), cw_node_new("DirectionalLight3D", "Sun"));
  cw_node_add_child(cw_tree_root(t), cw_node_new("OmniLight3D", "Lamp"));
  cw_node_add_child(cw_tree_root(t), cw_node_new("SpotLight3D", "Spot"));
  cw_pipeline_render(t, 32, 32, 0);
  /* the tree is kept: the debug field keeps what the last render reported */
}

CW_TEST_SUITE("programs_missing", { "leave_a_missing_state", test_leave_a_missing_state })
