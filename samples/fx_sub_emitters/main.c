/* fx_sub_emitters: a GPUParticles3D that spawns from another's particles
 * (emission_shape PARENT). The parent's particles all sit still at (2, 1, 0);
 * the child's own node is at (-3, 0.5, 0). Every child particle must be born
 * at a parent particle, so the child's mean position is (2, 1, 0), not its
 * node's. FXS_OFF=1 (the control) makes the child an ordinary POINT emitter.
 *
 * Debug state: fxs.child.alive, fxs.child.gpu_alive, fxs.child.mean (the GPU's
 * position sum over its alive count: xyz), fxs.parent.alive. */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/particles.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static int32_t child_alive, child_gpu_alive, parent_alive;
static float child_mean[3];

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

static void init(void *u) {
  (void)u;
  cw_debug_i32("fxs.child.alive", &child_alive);
  cw_debug_i32("fxs.child.gpu_alive", &child_gpu_alive);
  cw_debug_i32("fxs.parent.alive", &parent_alive);
  cw_debug_f32_array("fxs.child.mean", child_mean, 3);
  if (getenv("FXS_OFF")) cw_node_set(node("World/Child"), "emission_shape", cw_val_enum("POINT"));
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  cw_node c = node("World/Child");
  child_alive = cw_particles_alive(c);
  parent_alive = cw_particles_alive(node("World/Parent"));
  cw_value v;
  if (cw_node_get_prop(c, "gpu_alive", &v)) child_gpu_alive = (int32_t)v.i;
  if (cw_node_get_prop(c, "gpu_sum", &v) && child_gpu_alive > 0)
    for (int k = 0; k < 3; k++) child_mean[k] = v.v[k] / (float)child_gpu_alive;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_sub_emitters", .width = 640, .height = 360, .debug = true, .init = init, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
