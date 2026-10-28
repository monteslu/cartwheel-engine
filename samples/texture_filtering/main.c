/* texture_filtering: fine texture detail at grazing angles, the case where texture sampling settings show most. A long
 * tiled floor seen from low down and a bumpy wall lit from the side, with fine generated patterns (an sRGB albedo, a
 * normal map, a roughness map), anisotropic filtering 8 and full mip chains. The floor's textures are made from
 * pixels in code; the wall's albedo and normal map come through the resource system (gen://noise), the path a
 * glTF's images take, and get their filtering set after they loaded. GL and WebGPU must draw the same picture: the
 * gate compares them.
 *
 * Switches (environment, native runs; a cart has none):
 *   TF_ANISO=n          anisotropic filtering level on every texture (default 8)
 *   TF_MIN=linear       no mip chain (minification filter LINEAR)
 *   TF_FLAT=1           unlit: the albedo maps only, no lighting (sampling alone)
 *   TF_CONTROL_WGPU=1   the control: on WebGPU only, the textures ask for no anisotropic filtering, so WebGPU
 *                       must differ from GL
 * Debug fields: aniso (the level asked for), mips (1 when the textures have a mip chain), wall_ready (the wall's
 * resource textures loaded), calls (draw calls last frame). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/resource.h"
#include "three.h"

#define TN 256

typedef struct {
  t3_renderer *renderer;
  t3_scene *scene;
  t3_camera *camera;
  t3_material *wall_mat;
  cw_res wall_albedo, wall_normal;
  int32_t aniso, mips, wall_ready, calls;
  bool flat;
} app_t;
static app_t app;

static int env_int(const char *k, int d) {
  const char *v = getenv(k);
  return v && *v ? atoi(v) : d;
}

static void sampler(app_t *a, t3_texture *t) {
  t->wrap_s = t->wrap_t = T3_REPEAT;
  t->mag_filter = T3_LINEAR;
  t->min_filter = a->mips ? T3_LINEAR_MIPMAP_LINEAR : T3_LINEAR;
  t->generate_mipmaps = a->mips != 0;
  t->anisotropy = (float)a->aniso;
}

static uint8_t b8(float x) { return (uint8_t)((x < 0 ? 0 : x > 1 ? 1 : x) * 255.0f + 0.5f); }
static uint32_t hash(uint32_t x) {
  x ^= x >> 16, x *= 0x7feb352dU, x ^= x >> 15, x *= 0x846ca68bU, x ^= x >> 16;
  return x;
}

/* floor: tiles with dark grout every 32 texels, per texel speckle, and a height field (grout low, speckle bumps)
 * made into a normal map; roughness follows the speckle */
static float floor_h(int x, int y) {
  x &= TN - 1, y &= TN - 1;
  bool grout = (x & 31) < 2 || (y & 31) < 2;
  float s = (float)(hash((uint32_t)(y * TN + x) * 2654435761U) & 255) / 255.0f;
  return grout ? 0.0f : 0.6f + 0.4f * s;
}
static void make_floor_maps(app_t *a, t3_texture **albedo, t3_texture **normal, t3_texture **rough) {
  uint8_t *al = malloc(TN * TN * 4), *nm = malloc(TN * TN * 4), *ro = malloc(TN * TN * 4);
  for (int y = 0; y < TN; y++)
    for (int x = 0; x < TN; x++) {
      int i = (y * TN + x) * 4;
      float h = floor_h(x, y);
      bool checker = ((x >> 5) + (y >> 5)) & 1;
      float base = h == 0 ? 0.08f : checker ? 0.75f * h : 0.45f * h;
      al[i] = b8(base), al[i + 1] = b8(base * 0.9f), al[i + 2] = b8(base * (checker ? 0.7f : 1.0f)), al[i + 3] = 255;
      float dx = (floor_h(x + 1, y) - floor_h(x - 1, y)) * 2.0f, dy = (floor_h(x, y + 1) - floor_h(x, y - 1)) * 2.0f;
      float l = sqrtf(dx * dx + dy * dy + 1);
      nm[i] = b8(-dx / l * 0.5f + 0.5f), nm[i + 1] = b8(-dy / l * 0.5f + 0.5f), nm[i + 2] = b8(1 / l * 0.5f + 0.5f), nm[i + 3] = 255;
      ro[i] = ro[i + 1] = ro[i + 2] = b8(0.25f + 0.7f * h), ro[i + 3] = 255;
    }
  t3_texture *ts[3] = { t3_texture_new(TN, TN, al), t3_texture_new(TN, TN, nm), t3_texture_new(TN, TN, ro) };
  for (int i = 0; i < 3; i++) sampler(a, ts[i]);
  ts[0]->color_space = T3_SRGB_COLOR_SPACE;
  *albedo = ts[0], *normal = ts[1], *rough = ts[2];
  free(al), free(nm), free(ro);
}

static void init(void *user) {
  app_t *a = user;
  int w, h;
  cw_drawable_size(&w, &h);
  a->aniso = env_int("TF_ANISO", 8);
  if (getenv("TF_CONTROL_WGPU") && cw_gpu_backend() == CW_GPU_WEBGPU) a->aniso = 1;
  a->mips = getenv("TF_MIN") && !strcmp(getenv("TF_MIN"), "linear") ? 0 : 1;
  a->flat = env_int("TF_FLAT", 0) != 0;
  a->renderer = cw_render3d_window_renderer(w, h);
  t3_renderer_set_clear_color(a->renderer, 0x8090a0, 1.0f);
  a->scene = t3_scene_new();
  a->camera = t3_perspective_camera_new(60, (float)w / (float)h, 0.1f, 200);
  t3_object_set_position(a->camera, 0, 1.2f, 6);
  t3_object_look_at(a->camera, 0, 0.6f, -20);

  t3_texture *al, *nm, *ro;
  make_floor_maps(a, &al, &nm, &ro);
  t3_material *fm = a->flat ? t3_mesh_basic_material_new(0xffffff) : t3_mesh_standard_material_new(0xffffff);
  t3_material_set_texture(fm, T3_MAP, al);
  if (!a->flat) {
    t3_material_set_texture(fm, T3_NORMAL_MAP, nm);
    t3_material_set_texture(fm, T3_ROUGHNESS_MAP, ro);
    fm->roughness = 1, fm->metalness = 0;
  }
  t3_release(al), t3_release(nm), t3_release(ro);
  /* 80 x 80 m, the pattern repeated every 2 m: the far floor is many texels a pixel */
  t3_geometry *pg = t3_plane_geometry_new(80, 80, 1, 1);
  float *uv = pg->attributes[T3_ATTR_UV]->array;
  for (int i = 0; i < 8; i++) uv[i] *= 40;
  t3_mesh *floor = t3_mesh_new(pg, fm);
  t3_object_set_rotation(floor, -1.5707963f, 0, 0);
  t3_object_add((t3_object *)a->scene, floor);

  /* the wall: a long strip along the right side, its textures from the resource system (an albedo and a normal map
   * of fine noise). Their sampler fields are set after the load: both backends follow them */
  a->wall_albedo = cw_res_load("gen://noise?size=256&type=perlin&fractal=fbm&octaves=4&freq=0.12&seamless=1", CW_RES_TEXTURE);
  a->wall_normal = cw_res_load("gen://noise?size=256&type=perlin&fractal=fbm&octaves=3&freq=0.25&out=normal&strength=6&seamless=1", CW_RES_TEXTURE);
  t3_texture *wa = cw_res_texture(a->wall_albedo), *wn = cw_res_texture(a->wall_normal);
  a->wall_ready = wa && wn;
  a->wall_mat = a->flat ? t3_mesh_basic_material_new(0xc0b8a8) : t3_mesh_standard_material_new(0xc0b8a8);
  if (wa) { sampler(a, wa); t3_material_set_texture(a->wall_mat, T3_MAP, wa); }
  if (wn && !a->flat) { sampler(a, wn); t3_material_set_texture(a->wall_mat, T3_NORMAL_MAP, wn); }
  if (!a->flat) a->wall_mat->roughness = 0.45f, a->wall_mat->metalness = 0.6f;
  t3_geometry *wg = t3_plane_geometry_new(80, 6, 1, 1);
  uv = wg->attributes[T3_ATTR_UV]->array;
  for (int i = 0; i < 4; i++) uv[i * 2] *= 40, uv[i * 2 + 1] *= 3;
  t3_mesh *wall = t3_mesh_new(wg, a->wall_mat);
  t3_object_set_rotation(wall, 0, -1.5707963f, 0);
  t3_object_set_position(wall, 3.5f, 3, -30);
  t3_object_add((t3_object *)a->scene, wall);

  /* a low sun from the left (grazing on the wall's ribs and the floor's grout) and a soft fill */
  t3_light *sun = t3_directional_light_new(0xfff0d8, 3.0f);
  t3_object_set_position(sun, -10, 3, 2);
  t3_object_add((t3_object *)a->scene, sun);
  t3_object_add((t3_object *)a->scene, t3_hemisphere_light_new(0xb0c8ff, 0x403830, 0.6f));

  cw_debug_i32("aniso", &a->aniso);
  cw_debug_i32("mips", &a->mips);
  cw_debug_i32("wall_ready", &a->wall_ready);
  cw_debug_i32("calls", &a->calls);
}

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  cw_render3d_window_target(a->renderer);
  t3_renderer_render(a->renderer, a->scene, a->camera);
  a->calls = (int32_t)t3_renderer_info(a->renderer)->calls;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "cartwheel texture filtering", .width = 640, .height = 360, .init = init, .render = render,
                        .debug = true, .user = &app };
}
