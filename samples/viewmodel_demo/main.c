/* viewmodel_demo: a first person weapon on its own render layer (Node3D
 * layers, Camera3D viewmodel_layers / viewmodel_fov, cartwheel/render3d.h).
 *
 * The camera has a wide lens (85 degrees); the gun, a long red barrel and a
 * grey stock, hangs under it on layer 2 and is drawn after the scene by the
 * viewmodel pass with a 50 degree lens. The barrel is longer than the distance
 * to the wall in front of the camera: drawn with the scene it would sink into
 * the wall and stretch at the screen's edge; on its own layer it is whole,
 * always in front, and shaped by its own lens. The scene's lights and shadow
 * still shade it. A ProceduralSky draws the background: the viewmodel pass must
 * not draw it again (a sky cube drawn without a depth test would cover the
 * whole picture the scene pass drew, leaving only the gun over the sky).
 *
 * VIEWMODEL_OFF=1 puts the gun on the default layer and removes the camera's
 * viewmodel layers (the gate's control: the frame differs).
 *
 * Debug state: vm.on (1 with the viewmodel layers), vm.layers (the gun's
 * Node3D layers), vm.fov (the viewmodel lens), vm.tick, and on GL vm.wall_ok:
 * a point of the low wall left of the gun reads its light stone colour (1; 0
 * when the sky was drawn over the scene, which reads darker there; -1 on
 * WebGPU, where it is not read), with the colour in vm.wall_px. */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/gpu.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static int32_t on, gun_layers, wall_ok = -1, wall_px[3];
static float vm_fov;
static uint32_t tick;

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

static void init(void *u) {
  (void)u;
  cw_debug_i32("vm.on", &on);
  cw_debug_i32("vm.layers", &gun_layers);
  cw_debug_f32("vm.fov", &vm_fov);
  cw_debug_u32("vm.tick", &tick);
  cw_debug_i32("vm.wall_ok", &wall_ok);
  cw_debug_field("vm.wall_px", wall_px, CW_DBG_I32, 3);
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

/* after the frame is drawn (GL): the line to (-1.5, 0, -2.5) on the floor meets the low wall, left of the gun */
static void render(void *u, double a) {
  (void)u;
  (void)a;
  if (cw_gpu_backend() != CW_GPU_GL) return;
  t3_camera *cam = cw_camera3d_t3(node("World/Camera"));
  t3_mat4 view, vp;
  t3_mat4_invert(&view, &cam->base.matrix_world);
  t3_mat4_multiply(&vp, &cam->projection_matrix, &view);
  t3_vec3 p = { -1.5f, 0, -2.5f };
  float x = vp.e[0] * p.x + vp.e[4] * p.y + vp.e[8] * p.z + vp.e[12];
  float y = vp.e[1] * p.x + vp.e[5] * p.y + vp.e[9] * p.z + vp.e[13];
  float w = vp.e[3] * p.x + vp.e[7] * p.y + vp.e[11] * p.z + vp.e[15];
  int W, H;
  cw_drawable_size(&W, &H);
  int px = (int)((x / w * 0.5f + 0.5f) * (float)W), py = (int)((y / w * 0.5f + 0.5f) * (float)H);
  uint8_t c[4] = { 0 };
  glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
  for (int i = 0; i < 3; i++) wall_px[i] = c[i];
  /* the lit wall reads about 173 169 163; the sky's lower half there about 141 131 115 */
  wall_ok = c[0] > 155 && c[1] > 150 && c[2] > 145;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "viewmodel_demo", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
