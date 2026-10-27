/* vertex_colors: a mesh's per-vertex colours, as an rgb attribute (three floats a vertex) and as an rgba attribute
 * (four, what glTF exporters write for COLOR_0 by default). Both quads draw the same gradient: the alpha channel is
 * read as nothing (the left quad's alpha is 1, the right one's is 0.2, and neither fades). A colour attribute the
 * renderer cannot use (two floats a vertex) is not drawn and counts in `skipped`, which is what the control run
 * (CW_VCOL_CONTROL=1, native only) does to the right quad so the check can see the difference. Debug fields:
 * calls (draw calls last frame), skipped (draws the renderer refused). */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "three.h"

typedef struct {
  t3_renderer *renderer;
  t3_scene *scene;
  t3_camera *camera;
  int32_t calls, skipped;
} app_t;
static app_t app;

/* a quad of two triangles, one colour per corner: red, green, blue, yellow */
static t3_mesh *quad(float x0, int colour_items) {
  const float pos[12] = { -0.9f, -0.9f, 0, 0.9f, -0.9f, 0, 0.9f, 0.9f, 0, -0.9f, 0.9f, 0 };
  const float rgb[4][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 0 } };
  float col[16];
  for (int i = 0; i < 4; i++) {
    for (int c = 0; c < 3 && c < colour_items; c++) col[i * colour_items + c] = rgb[i][c];
    if (colour_items == 4) col[i * 4 + 3] = 0.2f; /* the alpha the renderer must not use */
  }
  uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
  t3_geometry *g = t3_geometry_new();
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, t3_attribute_new(T3_FLOAT32, pos, 4, 3));
  t3_geometry_set_attribute(g, T3_ATTR_COLOR, t3_attribute_new(T3_FLOAT32, col, 4, colour_items));
  t3_geometry_set_index(g, t3_attribute_new(T3_UINT16, idx, 6, 1));
  t3_material *m = t3_mesh_basic_material_new(0xffffff);
  m->vertex_colors = true;
  t3_mesh *mesh = t3_mesh_new(g, m);
  t3_object_set_position(mesh, x0, 0, 0);
  return mesh;
}

static void init(void *user) {
  app_t *a = user;
  int w, h;
  cw_drawable_size(&w, &h);
  a->renderer = cw_render3d_window_renderer(w, h);
  t3_renderer_set_clear_color(a->renderer, 0x202830, 1.0f);
  a->scene = t3_scene_new();
  a->camera = t3_orthographic_camera_new(-2, 2, 1.125f, -1.125f, 0.1f, 10);
  t3_object_set_position(a->camera, 0, 0, 5);
  t3_object_add((t3_object *)a->scene, quad(-1, 3));
  t3_object_add((t3_object *)a->scene, quad(1, getenv("CW_VCOL_CONTROL") ? 2 : 4));
  cw_debug_i32("calls", &a->calls);
  cw_debug_i32("skipped", &a->skipped);
}

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  cw_render3d_window_target(a->renderer);
  t3_renderer_render(a->renderer, a->scene, a->camera);
  const t3_render_info *i = t3_renderer_info(a->renderer);
  a->calls = (int32_t)i->calls;
  a->skipped = (int32_t)i->skipped;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "cartwheel vertex colors", .width = 640, .height = 360, .init = init, .render = render,
                        .debug = true, .user = &app };
}
