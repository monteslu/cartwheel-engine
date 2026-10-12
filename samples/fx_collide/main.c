/* fx_collide: GPU particles raining on a table top (with a sphere bump on
 * it) collide with the scene's depth buffer and come to rest on what the
 * camera sees; trails show the bounces.
 *
 * Debug state: collide.alive / collide.gpu_alive, collide.mean_y_x1000 (mean
 * height of the alive particles, read back from the GPU), collide.on_top
 * (1 when that mean lies on the table: 0.95..1.35). FX_NO_COLLIDE=1 turns the
 * collision off: the particles fall through the table and on_top fails (the
 * gate's control). */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/particles.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static int32_t alive, gpu_alive, mean_y, on_top;

static cw_node rain(void) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), "World/Rain"); }

static void init(void *u) {
  (void)u;
  cw_debug_i32("collide.alive", &alive);
  cw_debug_i32("collide.gpu_alive", &gpu_alive);
  cw_debug_i32("collide.mean_y_x1000", &mean_y);
  cw_debug_i32("collide.on_top", &on_top);
  const char *e = getenv("FX_NO_COLLIDE");
  if (e && *e == '1') cw_node_set(rain(), "collide_depth", cw_val_bool(false));
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  cw_value v;
  alive = cw_particles_alive(rain());
  if (cw_node_get_prop(rain(), "gpu_alive", &v)) gpu_alive = (int32_t)v.i;
  if (cw_node_get_prop(rain(), "gpu_sum", &v) && gpu_alive > 0) {
    float my = v.v[1] / (float)gpu_alive;
    mean_y = (int32_t)(my * 1000.0f);
    on_top = my > 0.95f && my < 1.35f;
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_collide", .width = 640, .height = 360, .debug = true, .init = init,
                              .render = render };
  return cw_scene_app("main.cwscene", &base);
}
