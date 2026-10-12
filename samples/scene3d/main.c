/* scene3d: a level built only from .cwscene files (main.cwscene instances
 * crate.cwscene twice and overrides one crate's colour), run by the scene
 * runner. The ball orbits on fixed ticks; its position and the tree's node
 * count are debug fields. */
#include "cartwheel/debug.h"
#include "cartwheel/scene_file.h"

static float ball_pos[3];
static int32_t crates;

static void init(void *u) {
  (void)u;
  cw_debug_f32_array("ball", ball_pos, 3);
  cw_debug_i32("crates", &crates);
  cw_node *list;
  crates = cw_tree_group(cw_scene_app_tree(), "crates", &list);
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  cw_tree *t = cw_scene_app_tree();
  cw_node ball = cw_node_get(cw_tree_root(t), "Level/Ball");
  float a = (float)cw_tree_tick(t) * 0.02f;
  t3_vec3 p = { 2.5f * cw_cos(a), 1.2f + 0.3f * cw_sin(a * 2), 2.5f * cw_sin(a) };
  cw_node3d_snapshot(ball);
  cw_node3d_set_position(ball, p);
  ball_pos[0] = p.x;
  ball_pos[1] = p.y;
  ball_pos[2] = p.z;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "scene3d", .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
