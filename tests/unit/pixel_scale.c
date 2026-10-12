/* pixel_scale: the app tests/pixel-scale.mjs and tests/host-resolution.mjs
 * run for cw_app_desc.pixel_scale and crisp UI (ui_min_lines). Built as
 * four apps from this file (tests/unit/CMakeLists.txt):
 *
 *   pixel_n2, pixel_n3  pixel_scale 2 / 3 at render_scale 3.125: a 320x180
 *                       app's output is 1000x563, not a multiple of either,
 *                       so the picture has bars
 *   pixel_auto3         pixel_scale 3 at the auto render scale, crisp UI
 *                       offered at 540 lines: under a host preference
 *                       (libretro) the frame is the low frame times the
 *                       smallest whole k that reaches 540 lines
 *   pixel_auto3_low     the same without crisp UI: the cart hands the host the
 *                       low frame itself
 *
 * Crisp UI is off until the crisp_ui debug command turns it on.
 * The scene is three.c (lit boxes at angles: edges in every direction); the
 * UI is one 2D window pass: a text line, and a magenta 4x4 square at the
 * pointer (the test finds it in the frame). Debug fields: pointer.x/y (the
 * logical pointer the app sees). Static: the frame does not change over
 * time, so any frame of a run is the frame. */
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/render3d.h"
#include "three.h"

#ifndef PIXEL_N
#define PIXEL_N 2
#endif
#ifndef PIXEL_RS
#define PIXEL_RS 3.125f
#endif
#ifndef PIXEL_UI
#define PIXEL_UI 540
#endif

static struct {
  t3_renderer *renderer;
  t3_scene *scene;
  t3_camera *camera;
  cw_font font;
  float pointer[2];
} A;

static void init(void *user) {
  (void)user;
  int w, h;
  cw_drawable_size(&w, &h);
  A.renderer = cw_render3d_window_renderer(w, h);
  t3_renderer_set_clear_color(A.renderer, 0x203040, 1.0f);
  A.scene = t3_scene_new();
  A.camera = t3_perspective_camera_new(50.0f, 320.0f / 180.0f, 0.1f, 100.0f);
  t3_object_set_position(A.camera, 0, 2, 6);
  t3_object_look_at(A.camera, 0, 0, 0);
  static const unsigned colours[4] = { 0xff8833, 0x33aaff, 0x66dd44, 0xeeeeee };
  for (int i = 0; i < 4; i++) {
    t3_mesh *m = t3_mesh_new(t3_box_geometry_new(1, 1, 1, 1, 1, 1), t3_mesh_standard_material_new(colours[i]));
    t3_object_set_position(m, -2.4f + 1.6f * (float)i, 0, 0);
    t3_object_set_rotation(m, 0.4f + 0.3f * (float)i, 0.7f * (float)i + 0.2f, 0.1f);
    t3_object_add((t3_object *)A.scene, m);
  }
  t3_light *sun = t3_directional_light_new(0xffffff, 3.14159f);
  t3_object_set_position(sun, 3, 5, 2);
  t3_object_add((t3_object *)A.scene, sun);
  t3_object_add((t3_object *)A.scene, t3_ambient_light_new(0x404040, 3.14159f));
  A.font = cw_font_load("fonts/FiraSans-Regular.ttf", &(cw_font_desc){ .mode = CW_FONT_SDF, .bake_size = 48 });
  cw_debug_field("pointer", A.pointer, CW_DBG_F32, 2);
}

static void render(void *user, double alpha) {
  (void)user, (void)alpha;
  const cw_raw_input *in = cw_input_raw();
  A.pointer[0] = in->mouse_x;
  A.pointer[1] = in->mouse_y;
  cw_render3d_window_target(A.renderer);
  t3_renderer_render(A.renderer, A.scene, A.camera);
  cw_r2d_begin(&(cw_r2d_pass_desc){ 0 });
  cw_r2d_text(A.font, "Crisp UI text 0123", (cw_vec2){ 12, 140 }, &(cw_text_style){ .size = 14, .color = cw_hex(0xffffffff) });
  if (in->mouse_present) cw_r2d_rect(cw_rect_make(in->mouse_x - 2, in->mouse_y - 2, 4, 4), cw_hex(0xff00ffff));
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){
    .title = "pixel_scale",
    .width = 320,
    .height = 180,
    .render_scale = PIXEL_RS,
    .pixel_scale = PIXEL_N,
    .ui_min_lines = PIXEL_UI,
    .init = init,
    .render = render,
    .debug = true,
  };
}
