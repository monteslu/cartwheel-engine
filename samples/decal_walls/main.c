/* decal_walls: many box-projected decals on the faces of a side-on cliff, the way a 2.5D platformer dresses its
 * terrain. A row of rock columns of uneven height (front faces at z = 1.5, seen nearly head on) carries about two
 * hundred Decal3D: MULTIPLY stains from regions of one non-square sheet, spun a little, with a box 0.7 deep around
 * the face (edge_fade 0.3, normal_fade 0.5), plus a few projected down onto the column tops. The scene goes through
 * the full pipeline (MSAA, ambient occlusion, bloom, tone mapping), so on WebGPU the decals read the linear depth
 * of a bottom-up (flip_y) target and on GL the depth copy. GL and WebGPU must draw the same picture: the gate
 * compares them.
 *
 * Switches (environment, native runs; a cart has none):
 *   DW_NO_DECALS=1        no decals (the gate's frame control)
 *   DW_CONTROL_WGPU=1     on WebGPU only, every decal loses its fades (edge_fade and normal_fade 0: hard edges at the box ends, the
 *                       look of a lost fade): WebGPU must differ from GL
 * Debug fields: dw.decals, dw.visible, dw.draws (the decal pass's counts last frame), dw.tick. */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/decals.h"
#include "cartwheel/gpu.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

#define COLS 25

static int32_t n_decals, n_visible, n_draws;
static uint32_t tick;

static cw_node add(const char *type, cw_node parent, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}

static float hash01(int x, int y, int s) {
  uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)s * 83492791u;
  h ^= h >> 13, h *= 0x5bd1e995u, h ^= h >> 15;
  return (float)(h & 0xffff) / 65535.0f;
}

/* a decal on a column's front face (projected along -Z onto it) or on a top (projected down) */
static void decal(cw_node w, float x, float y, float size, int cell, float strength, bool down, float spin, bool nofade) {
  cw_node d = add("Decal3D", w, "Decal");
  cw_node_set(d, "texture", cw_val_string("decals/sheet.png"));
  cw_value r = { CW_T_VEC4 };
  r.v[0] = (float)cell * 128, r.v[1] = 0, r.v[2] = 128, r.v[3] = 128;
  cw_node_set(d, "region", r);
  cw_node_set(d, "blend", cw_val_enum("MULTIPLY"));
  cw_node_set(d, "color", cw_val_color(0.55f, 0.5f, 0.45f, strength));
  cw_node_set(d, "edge_fade", cw_val_float(nofade ? 0 : 0.3f));
  cw_node_set(d, "normal_fade", cw_val_float(nofade ? 0 : 0.5f));
  if (down) {
    cw_node_set(d, "size", cw_val_vec3(size, size, 0.5f));
    cw_node3d_set_position(d, (t3_vec3){ x, y + 0.12f, 0 });
    cw_node3d_set_rotation(d, (t3_vec3){ -1.5708f, spin, 0 });
  } else {
    cw_node_set(d, "size", cw_val_vec3(size, size, 0.7f));
    cw_node3d_set_position(d, (t3_vec3){ x, y, 1.75f });
    cw_node3d_set_rotation(d, (t3_vec3){ 0, 0, spin });
  }
  n_decals++;
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("dw.decals", &n_decals);
  cw_debug_i32("dw.visible", &n_visible);
  cw_debug_i32("dw.draws", &n_draws);
  cw_debug_u32("dw.tick", &tick);
  cw_node w = cw_node_get(cw_tree_root(cw_scene_app_tree()), "World");
  int height[COLS];
  for (int i = 0; i < COLS; i++) height[i] = 2 + (int)(hash01(i, 0, 7) * 5.0f);
  /* the columns: 1 m wide, 3 m deep, centred on z = 0 */
  for (int i = 0; i < COLS; i++) {
    cw_node c = add("MeshInstance3D", w, "Column");
    float x = (float)(i - COLS / 2), h = (float)height[i];
    cw_node3d_set_position(c, (t3_vec3){ x, h * 0.5f - 1.0f, 0 });
    cw_node_set(c, "size", cw_val_vec3(1, h, 3));
    float g = 0.45f + 0.1f * hash01(i, 1, 3);
    cw_node_set(c, "color", cw_val_color(g, g * 0.92f, g * 0.82f, 1));
    cw_node_set(c, "roughness", cw_val_float(0.9f));
    cw_node_set(c, "cast_shadow", cw_val_bool(true));
    cw_node_set(c, "receive_shadow", cw_val_bool(true));
  }
  const char *e = getenv("DW_NO_DECALS");
  if (e && *e == '1') return;
  e = getenv("DW_CONTROL_WGPU");
  bool nofade = e && *e == '1' && cw_gpu_backend() == CW_GPU_WEBGPU;
  /* about two hundred: one or two per face tile, a few on the tops */
  for (int i = 0; i < COLS; i++) {
    float x = (float)(i - COLS / 2);
    for (int y = 0; y < height[i]; y++) {
      float r = hash01(i, y, 900), spin = (hash01(i, y, 901) - 0.5f) * 0.6f;
      float fy = (float)y - 0.5f;
      decal(w, x + (r - 0.5f) * 0.4f, fy, 1.8f, (int)(r * 2) & 1, 0.7f, false, spin * 3, nofade);
      if (r > 0.3f) decal(w, x + 0.3f, fy + 0.4f, 1.1f, (int)(r * 7) & 1, 0.6f, false, spin, nofade);
      if (r > 0.6f) decal(w, x - 0.3f, fy - 0.3f, 0.9f, 1, 0.75f, false, -spin, nofade);
    }
    if (hash01(i, 0, 902) > 0.5f) decal(w, x, (float)height[i] - 1.0f, 1.6f, i & 1, 0.8f, true, (float)i, nofade);
  }
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  cw_decal_stats st;
  cw_decals_get_stats(cw_scene_app_tree(), &st);
  n_visible = st.visible;
  n_draws = st.draw_calls;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "decal_walls", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
