/* texture_color: an image file's texture resource sampled as colour (sRGB) or as data, measured on screen. Five
 * quads, each an unlit (MeshBasicMaterial) map of the same flat mid grey (byte 128), side by side:
 *
 *   0 grey.png          a PNG kept as it is, loaded plainly: data (three.js r187's TextureLoader), drawn about 188
 *   1 grey_claimed.png  the same PNG with cw_res_texture_set_srgb(path, true) before the load: colour, drawn 128
 *   2 grey_k.png        KTX2 from cwpack, texture.color = srgb: colour from the file's DFD, drawn 128
 *   3 grey_kl.png       KTX2 from cwpack, texture.color = linear: data, drawn about 188
 *   4 grey_kd.png       KTX2 from cwpack with no texture.color (cwpack's default, sRGB): colour, drawn 128
 *
 * A colour texture decodes from sRGB when sampled (GL: an sRGB internal format; WebGPU: an RGBA8UnormSrgb texture)
 * and the output encodes again, so 128 comes back as 128; a data texture's 0.5 is linear light and comes out as
 * 188. The quads load at frame 2 (when the test switches have arrived on every host) and are read back from frame 6.
 *
 * Debug fields: tc.cs0..tc.cs4 (each texture's color_space, 0 none or 1 sRGB), tc.px0..tc.px4 (the red byte at each
 * quad's middle, -1 until read), tc.kind0..tc.kind4 (what that byte says: 1 colour, 128 +- 10; 0 data, 188 +- 10;
 * -1 neither or unread), tc.read (1 once every quad was read).
 * Switch (cw_test_env): TC_CONTROL=1 loads grey_claimed.png without cw_res_texture_set_srgb, the path before
 * v0.84.0, which must fail tc.cs1 and tc.kind1. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/resource.h"
#include "three.h"

#define NQ 5
static const char *const kPaths[NQ] = { "textures/grey.png", "textures/grey_claimed.png", "textures/grey_k.png",
                                        "textures/grey_kl.png", "textures/grey_kd.png" };

typedef struct {
  t3_renderer *renderer;
  t3_scene *scene;
  t3_camera *camera;
  cw_res res[NQ];
  int32_t cs[NQ], px[NQ], kind[NQ], read;
  uint32_t ticket[NQ];
  bool loaded;
  int w, h;
} app_t;
static app_t app;

static void init(void *user) {
  app_t *a = user;
  cw_drawable_size(&a->w, &a->h);
  a->renderer = cw_render3d_window_renderer(a->w, a->h);
  t3_renderer_set_clear_color(a->renderer, 0x000000, 1.0f);
  a->scene = t3_scene_new();
  /* one unit per quad across, the five in a row */
  float aspect = (float)a->w / (float)a->h;
  a->camera = t3_orthographic_camera_new(-NQ * 0.5f, NQ * 0.5f, NQ * 0.5f / aspect, -NQ * 0.5f / aspect, 0.1f, 10);
  t3_object_set_position(a->camera, 0, 0, 5);
  for (int i = 0; i < NQ; i++) a->cs[i] = -1, a->px[i] = -1, a->kind[i] = -1;
  /* (the debug channel keeps the name pointers) */
  static const char *const cs_names[NQ] = { "tc.cs0", "tc.cs1", "tc.cs2", "tc.cs3", "tc.cs4" };
  static const char *const px_names[NQ] = { "tc.px0", "tc.px1", "tc.px2", "tc.px3", "tc.px4" };
  static const char *const kind_names[NQ] = { "tc.kind0", "tc.kind1", "tc.kind2", "tc.kind3", "tc.kind4" };
  for (int i = 0; i < NQ; i++) {
    cw_debug_i32(cs_names[i], &a->cs[i]);
    cw_debug_i32(px_names[i], &a->px[i]);
    cw_debug_i32(kind_names[i], &a->kind[i]);
  }
  cw_debug_i32("tc.read", &a->read);
}

static void load(app_t *a) {
  a->loaded = true;
  if (!cw_test_env("TC_CONTROL") && !cw_res_texture_set_srgb(kPaths[1], true)) cw_error("texture_color: claim refused");
  for (int i = 0; i < NQ; i++) {
    a->res[i] = cw_res_load(kPaths[i], CW_RES_TEXTURE);
    t3_texture *t = cw_res_texture(a->res[i]);
    if (!t) { cw_error("texture_color: %s did not load", kPaths[i]); continue; }
    a->cs[i] = t->color_space == T3_SRGB_COLOR_SPACE ? 1 : t->color_space == T3_NO_COLOR_SPACE ? 0 : 2;
    t3_material *m = t3_mesh_basic_material_new(0xffffff);
    t3_material_set_texture(m, T3_MAP, t);
    t3_mesh *q = t3_mesh_new(t3_plane_geometry_new(0.8f, 0.8f, 1, 1), m);
    t3_object_set_position(q, -NQ * 0.5f + 0.5f + (float)i, 0, 0);
    t3_object_add((t3_object *)a->scene, q);
  }
}

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  if (!a->loaded && cw_frame_index() >= 1) load(a);
  cw_render3d_window_target(a->renderer);
  t3_renderer_render(a->renderer, a->scene, a->camera);
  int done = 0;
  for (int i = 0; i < NQ; i++) {
    if (a->ticket[i]) {
      uint8_t px[4 * 4 * 4];
      int got = cw_gpu_read_result(a->ticket[i], px);
      if (got != 0) a->ticket[i] = 0;
      if (got == 1) {
        int v = a->px[i] = px[(2 * 4 + 2) * 4];
        a->kind[i] = v >= 118 && v <= 138 ? 1 : v >= 178 && v <= 198 ? 0 : -1;
      }
    }
    if (a->px[i] >= 0) done++;
    else if (!a->ticket[i] && a->loaded && cw_frame_index() >= 5) {
      int dw = 0, dh = 0;
      cw_drawable_size(&dw, &dh);
      int x = (int)((0.5f + (float)i) / NQ * (float)dw), y = dh / 2;
      a->ticket[i] = cw_gpu_read_window(x - 2, y - 2, 4, 4);
    }
  }
  a->read = done == NQ;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "cartwheel texture colour", .width = 640, .height = 360, .init = init, .render = render,
                        .debug = true, .user = &app };
}
