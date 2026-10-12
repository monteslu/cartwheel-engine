/* hot_demo: native C hot reload (M2.8). A Spinner node type turns at
 * SPIN_SPEED radians per tick; the state (angle, ticks) lives in the node, so
 * swapping in code built with another SPIN_SPEED keeps it and changes only
 * the behaviour. tests/hot.mjs swaps at an exact frame under cwhot. */
#include "cartwheel/debug.h"
#include "cartwheel/scene_file.h"

#ifndef SPIN_SPEED
#define SPIN_SPEED 0.01f
#endif

/* #region node_type */
typedef struct { float angle; int32_t ticks; float speed_seen; } spinner;

static void spin_fixed(cw_node n, void *data, double dt) {
  (void)dt;
  spinner *s = data;
  s->angle += SPIN_SPEED;
  s->speed_seen = SPIN_SPEED;
  s->ticks++;
  cw_node3d_set_rotation(n, (t3_vec3){ 0, s->angle, 0 });
}

static cw_node_type spinner_type = { .name = "Spinner", .parent = "MeshInstance3D", .data_size = sizeof(spinner),
                                     .doc = "Turns about Y at a compile-time speed.", .fixed_update = spin_fixed };
/* #endregion */

/* Every node type this game defines and nothing else (no scene, no state):
 * the editor calls it to open the game's scenes, cwhot after a reload. */
/* #region register_types */
void cw_game_register_types(void);
void cw_game_register_types(void) { cw_node_type_register(&spinner_type); }
/* #endregion */

static void init(void *u) {
  (void)u;
  cw_game_register_types();
  cw_tree *t = cw_scene_app_tree();
  cw_node level = cw_node_new("Node3D", "Level");
  cw_node_add_child(cw_tree_root(t), level);
  cw_node cam = cw_node_new("Camera3D", "Camera");
  cw_node3d_set_position(cam, (t3_vec3){ 0, 1.5f, 4 });
  cw_node_set(cam, "rotation", cw_val_vec3(-0.3f, 0, 0));
  cw_node_add_child(level, cam);
  cw_node sun = cw_node_new("DirectionalLight3D", "Sun");
  cw_node_set(sun, "rotation", cw_val_vec3(-0.8f, 0.5f, 0));
  cw_node_add_child(level, sun);
  cw_node box = cw_node_new("Spinner", "Box");
  cw_node_set(box, "color", cw_val_color(0.78741f, 0.21404f, 0.0331f, 1));
  cw_node_add_child(level, box);
  spinner *s = cw_node_data(box, "Spinner");
  cw_debug_f32("spin.angle", &s->angle);
  cw_debug_i32("spin.ticks", &s->ticks);
  cw_debug_f32("spin.speed", &s->speed_seen);
}

/* #region main */
cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "hot_demo", .width = 320, .height = 180, .debug = true, .init = init };
  return cw_scene_app(NULL, &base);
}
/* #endregion */
