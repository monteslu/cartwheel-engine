/* instanced_attributes: one InstancedMesh with every optional vertex attribute at once.
 *
 * A grid of 5 x 4 boxes, one draw: the geometry carries positions, normals, two uv sets (uv and
 * uv1), vertex colours and tangents, each instance has its own colour, and the material (physical) uses a
 * colour map, a normal map (with the geometry's tangents), an ambient occlusion map, an emissive map and a
 * roughness map, under a shadow-casting sun. On WebGPU that is the most vertex buffers a draw binds: the
 * geometry's attributes, the instance matrix (one buffer) and the instance colours, inside the 8 every
 * WebGPU device has. GL and WebGPU draw the same picture.
 *
 * CW_IATTR_CONTROL=1 (the gate's control): every instance colour white, so the picture differs.
 *
 * Debug fields: calls (draw calls last frame), skipped (draws the renderer refused), instances. */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "three.h"

#define COLS 5
#define ROWS 4
#define TEX 64

typedef struct {
  t3_renderer *renderer;
  t3_scene *scene;
  t3_camera *camera;
  int32_t calls, skipped, instances;
} app_t;
static app_t app;

/* procedural maps (integer maths only: the same bytes on every host) */
static t3_texture *map_tex(int kind) {
  static uint8_t px[TEX * TEX * 4];
  for (int y = 0; y < TEX; y++)
    for (int x = 0; x < TEX; x++) {
      uint8_t *p = px + (y * TEX + x) * 4;
      int cx = x * 2 - TEX + 1, cy = y * 2 - TEX + 1, r2 = cx * cx + cy * cy, r = TEX * TEX;
      switch (kind) {
      case 0: { /* colour: a checker of two warm greys */
        int c = ((x >> 3) ^ (y >> 3)) & 1 ? 235 : 175;
        p[0] = (uint8_t)c, p[1] = (uint8_t)(c - 10), p[2] = (uint8_t)(c - 25);
        break; }
      case 1: { /* normal: a raised ring, tilted outward across its width (tangent space) */
        int nx = 0, ny = 0;
        if (r2 > r / 5 && r2 < r / 2) { nx = cx * 90 / TEX; ny = -cy * 90 / TEX; }
        p[0] = (uint8_t)(128 + nx), p[1] = (uint8_t)(128 + ny), p[2] = 230;
        break; }
      case 2: { /* ambient occlusion (red channel): darker toward the corners */
        int v = 255 - r2 * 120 / (2 * r);
        p[0] = p[1] = p[2] = (uint8_t)v;
        break; }
      case 3: /* emissive: a small glowing centre */
        p[0] = p[1] = p[2] = r2 < r / 25 ? 200 : 0;
        break;
      default: /* roughness (green channel): stripes of shiny and rough */
        p[0] = p[2] = 255, p[1] = (x >> 2) & 1 ? 230 : 70;
        break;
      }
      p[3] = 255;
    }
  t3_texture *t = t3_data_texture_new(TEX, TEX, px);
  t->generate_mipmaps = true;
  return t;
}

static void init(void *user) {
  app_t *a = user;
  bool control = getenv("CW_IATTR_CONTROL") != NULL;
  int w, h;
  cw_drawable_size(&w, &h);
  a->renderer = cw_render3d_window_renderer(w, h);
  t3_renderer_set_clear_color(a->renderer, 0x1a2028, 1.0f);
  t3_renderer_set_shadow_map(a->renderer, true, T3_PCF_SHADOW_MAP);
  a->scene = t3_scene_new();
  a->camera = t3_perspective_camera_new(40, (float)w / (float)h, 0.1f, 50);
  t3_object_set_position(a->camera, 0, 5.5f, 9);
  t3_object_look_at(a->camera, 0, 0, 0);
  t3_object_add((t3_object *)a->scene, t3_ambient_light_new(0xffffff, 0.6f));
  t3_light *sun = t3_directional_light_new(0xfff4e0, 2.2f);
  t3_object_set_position(sun, 4, 8, 5);
  sun->base.cast_shadow = true;
  t3_object_add((t3_object *)a->scene, sun);

  /* the ground, to catch the shadows */
  t3_mesh *ground = t3_mesh_new(t3_plane_geometry_new(14, 10, 1, 1), t3_mesh_standard_material_new(0x506070));
  t3_object_set_rotation(ground, -1.5707964f, 0, 0);
  t3_object_set_position(ground, 0, -0.5f, 0);
  ground->base.receive_shadow = true;
  t3_object_add((t3_object *)a->scene, ground);

  /* a box with every optional per-vertex attribute: uv1 (a second uv set), vertex colours (top lighter than the
   * bottom), tangents (each face's u direction) */
  t3_geometry *g = t3_box_geometry_new(1, 1, 1, 2, 2, 2);
  t3_attribute *pos = g->attributes[T3_ATTR_POSITION], *nrm = g->attributes[T3_ATTR_NORMAL], *uv = g->attributes[T3_ATTR_UV];
  int n = pos->count;
  float *uv1 = malloc((size_t)n * 2 * sizeof(float)), *col = malloc((size_t)n * 3 * sizeof(float)), *tan = malloc((size_t)n * 4 * sizeof(float));
  const float *P = t3_attribute_f32(pos), *N = t3_attribute_f32(nrm), *U = t3_attribute_f32(uv);
  for (int i = 0; i < n; i++) {
    uv1[i * 2] = U[i * 2] * 0.5f + 0.25f, uv1[i * 2 + 1] = U[i * 2 + 1] * 0.5f + 0.25f;
    float k = 0.7f + 0.3f * (P[i * 3 + 1] + 0.5f);
    col[i * 3] = k, col[i * 3 + 1] = k, col[i * 3 + 2] = 0.85f * k + 0.15f;
    /* the face's u direction: +x on the z faces and the y faces, -z / +z on the x faces */
    float tx = 1, tz = 0;
    if (N[i * 3] > 0.5f) tx = 0, tz = -1;
    else if (N[i * 3] < -0.5f) tx = 0, tz = 1;
    else if (N[i * 3 + 2] < -0.5f) tx = -1;
    tan[i * 4] = tx, tan[i * 4 + 1] = 0, tan[i * 4 + 2] = tz, tan[i * 4 + 3] = 1;
  }
  t3_geometry_set_attribute(g, T3_ATTR_UV1, t3_attribute_new(T3_FLOAT32, uv1, n, 2));
  t3_geometry_set_attribute(g, T3_ATTR_COLOR, t3_attribute_new(T3_FLOAT32, col, n, 3));
  t3_geometry_set_attribute(g, T3_ATTR_TANGENT, t3_attribute_new(T3_FLOAT32, tan, n, 4));
  free(uv1), free(col), free(tan);

  t3_material *m = t3_mesh_physical_material_new(0xffffff);
  m->roughness = 1, m->metalness = 0.1f;
  m->vertex_colors = true;
  m->emissive = (t3_color){ 1, 0.8f, 0.4f };
  t3_material_set_texture(m, T3_MAP, map_tex(0));
  t3_material_set_texture(m, T3_NORMAL_MAP, map_tex(1));
  t3_material_set_texture(m, T3_AO_MAP, map_tex(2));
  t3_material_set_texture(m, T3_EMISSIVE_MAP, map_tex(3));
  t3_material_set_texture(m, T3_ROUGHNESS_MAP, map_tex(4));

  t3_instanced_mesh *im = t3_instanced_mesh_new(g, m, COLS * ROWS);
  im->mesh.base.cast_shadow = true;
  im->mesh.base.receive_shadow = true;
  static const uint32_t hues[6] = { 0xe04040, 0x40c060, 0x4070e0, 0xe0c040, 0xc050d0, 0x40c8c8 };
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      int i = r * COLS + c;
      t3_mat4 mx;
      t3_quat q = { 0, 0.2588190f, 0, 0.9659258f }; /* 30 degrees about y */
      t3_mat4_compose(&mx, (t3_vec3){ (float)(c - 2) * 1.6f, 0, (float)(r - 1.5f) * 1.6f }, q, (t3_vec3){ 1, 1, 1 });
      t3_instanced_mesh_set_matrix_at(im, i, &mx);
      t3_instanced_mesh_set_color_at(im, i, t3_color_hex(control ? 0xffffff : hues[i % 6]));
    }
  t3_object_add((t3_object *)a->scene, im);
  a->instances = COLS * ROWS;
  cw_debug_i32("calls", &a->calls);
  cw_debug_i32("skipped", &a->skipped);
  cw_debug_i32("instances", &a->instances);
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
  return (cw_app_desc){ .title = "cartwheel instanced attributes", .width = 640, .height = 360, .init = init, .render = render,
                        .debug = true, .user = &app };
}
