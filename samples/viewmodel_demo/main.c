/* viewmodel_demo: a first person weapon on its own render layer (Node3D
 * layers, Camera3D viewmodel_layers / viewmodel_fov, cartwheel/render3d.h).
 *
 * The camera has a wide lens (85 degrees); the gun, a long red barrel and a
 * grey stock, hangs under it on layer 2 and is drawn after the scene by the
 * viewmodel pass with a 50 degree lens. The barrel is longer than the distance
 * to the wall in front of the camera: drawn with the scene it would sink into
 * the wall and stretch at the screen's edge; on its own layer it is whole,
 * always in front, and shaped by its own lens. The scene's lights and shadow
 * still shade it.
 *
 * VIEWMODEL_OFF=1 puts the gun on the default layer and removes the camera's
 * viewmodel layers (the gate's control: the frame differs).
 *
 * Debug state: vm.on (1 with the viewmodel layers), vm.layers (the gun's
 * Node3D layers), vm.fov (the viewmodel lens), vm.tick. */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static int32_t on, gun_layers;
static float vm_fov;
static uint32_t tick;

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

static void init(void *u) {
  (void)u;
  cw_debug_i32("vm.on", &on);
  cw_debug_i32("vm.layers", &gun_layers);
  cw_debug_f32("vm.fov", &vm_fov);
  cw_debug_u32("vm.tick", &tick);
  cw_node cam = node("World/Camera"), gun = node("World/Camera/Gun");
  if (getenv("VIEWMODEL_OFF")) {
    cw_node_set(gun, "layers", cw_val_int(1));
    cw_node_set(cam, "viewmodel_layers", cw_val_int(0));
  }
  cw_value v;
  if (cw_node_get_prop(cam, "viewmodel_layers", &v)) on = v.i != 0;
  if (cw_node_get_prop(gun, "layers", &v)) gun_layers = (int32_t)v.i;
  if (cw_node_get_prop(cam, "viewmodel_fov", &v)) vm_fov = (float)v.f;
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "viewmodel_demo", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed_update };
  return cw_scene_app("main.cwscene", &base);
}
