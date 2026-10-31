/* warm_culled: the warm-up's shader phase must build the programs of what the camera cannot see yet. A mesh behind the camera
 * has a program of its own; the camera turns round at tick 20 and the count of compiled programs must not rise. Control
 * WARM_LATE=1: a mesh with another program appears at tick 20, and the count does rise. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static int32_t ticks, base, grown;

static void init(void *user) {
  (void)user;
  cw_debug_i32("warm.tick", &ticks);
  cw_debug_i32("warm.base", &base);
  cw_debug_i32("warm.new", &grown);
}

static void fixed(void *user, double dt) {
  (void)user, (void)dt;
  cw_tree *t = cw_scene_app_tree();
  cw_node root = cw_tree_root(t), scene = cw_node_get(root, "Root");
  ticks++;
  if (ticks == 20) {
    cw_node3d_set_rotation(cw_node_get(scene, "Camera"), (t3_vec3){ 0, 3.14159265f, 0 });
    if (getenv("WARM_LATE")) {
      cw_node n = cw_node_new("MeshInstance3D", "Late");
      const cw_prop *pp = cw_node_find_prop(n, "primitive", NULL);
      cw_value prim = { CW_T_ENUM };
      for (int k = 0; pp && pp->enum_names && pp->enum_names[k]; k++) if (!strcmp(pp->enum_names[k], "BOX")) prim.i = k;
      cw_node_set(n, "primitive", prim);
      if (!cw_node_set(n, "sheen", cw_val_float(1))) cw_warn("sheen refused");
      cw_node3d_set_position(n, (t3_vec3){ 0, 1, 12 });
      cw_node_add_child(scene, n);
    }
  }
  cw_render3d_stats st;
  cw_render3d_get_stats(t, &st);
  if (ticks == 3) base = st.programs;
  if (ticks > 3) grown = st.programs - base;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  cw_scene_register_builtin_types();
  static cw_app_desc base_app = { .title = "warm_culled", .width = 480, .height = 270, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base_app);
}
