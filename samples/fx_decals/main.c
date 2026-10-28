/* fx_decals: Decal3D on uneven geometry: a scorch over three steps
 * (MULTIPLY), an arrow over a ramp and the floor (MIX), cracks wrapping a
 * sphere and the floor around it, splats from one sheet image (region) on the
 * floor and on a wall, a scorch folded over a wall corner, a spinning glow
 * ring (ADD) over the steps' edge, and 240 small splats scattered over a
 * field larger than the view (instancing and frustum culling).
 *
 * Debug state: decal.decals / visible / culled / draws / atlas (counts),
 * decal.red_ok (the red splat's centre pixel reads red: the decal really
 * landed where its box meets the floor), decal.tick. FX_NO_DECALS=1 removes
 * every Decal3D at start (the gate's control: red_ok fails). */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/decals.h"
#include "cartwheel/gpu.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static int32_t n_decals, n_visible, n_culled, n_draws, n_atlas, red_ok, red_px[3];
static uint32_t tick;

static cw_node world(void) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), "World"); }

static void init(void *u) {
  (void)u;
  cw_debug_i32("decal.decals", &n_decals);
  cw_debug_i32("decal.visible", &n_visible);
  cw_debug_i32("decal.culled", &n_culled);
  cw_debug_i32("decal.draws", &n_draws);
  cw_debug_i32("decal.atlas", &n_atlas);
  cw_debug_i32("decal.red_ok", &red_ok);
  cw_debug_field("decal.red_px", red_px, CW_DBG_I32, 3);
  cw_debug_u32("decal.tick", &tick);
  /* a field of small splats, wider than the view */
  /* #region rng_spawn */
  cw_rng rng;
  cw_rng_seed(&rng, 1234);
  for (int i = 0; i < 240; i++) {
    cw_node d = cw_node_new("Decal3D", "Splat");
    float x = cw_rng_range(&rng, -14, 14), z = cw_rng_range(&rng, -2, 4.5f);
    cw_node3d_set_position(d, (t3_vec3){ x, 0.5f, z });
    cw_node3d_set_rotation(d, (t3_vec3){ -CW_PI / 2, 0, cw_rng_range(&rng, 0, CW_TAU) });
    float s = cw_rng_range(&rng, 0.15f, 0.4f);
    cw_node_set(d, "size", cw_val_vec3(s, s, 1.2f));
    /* one draw per statement: C leaves the order of arguments open */
    float cr = cw_rng_range(&rng, 0.2f, 0.5f);
    float cg = cw_rng_range(&rng, 0.3f, 0.6f);
    float cb = cw_rng_range(&rng, 0.1f, 0.3f);
    cw_node_set(d, "color", cw_val_color(cr, cg, cb, 0.8f));
    cw_node_set(d, "blend", cw_val_enum("MULTIPLY"));
    cw_node_add_child(world(), d);
  }
  /* #endregion */

  const char *e = getenv("FX_NO_DECALS");
  if (e && *e == '1') {
    cw_node w = world();
    for (int i = cw_node_child_count(w) - 1; i >= 0; i--)
      if (cw_node_is(cw_node_child(w, i), "Decal3D")) cw_node_free(cw_node_child(w, i));
  }
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  cw_node g = cw_node_get(world(), "Glow");
  if (cw_node_valid(g)) cw_node3d_set_rotation(g, (t3_vec3){ -CW_PI / 2, 0, (float)tick * 0.03f });
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  cw_decal_stats st;
  cw_decals_get_stats(cw_scene_app_tree(), &st);
  n_decals = st.decals;
  n_visible = st.visible;
  n_culled = st.culled;
  n_draws = st.draw_calls;
  n_atlas = st.atlas_images;
  /* where the red splat's centre meets the floor (0.9, 0, 2.4), on screen */
  t3_camera *cam = cw_camera3d_t3(cw_node_get(world(), "Camera"));
  t3_mat4 view, vp;
  t3_mat4_invert(&view, &cam->base.matrix_world);
  t3_mat4_multiply(&vp, &cam->projection_matrix, &view);
  t3_vec3 p = { 0.9f, 0, 2.4f };
  float x = vp.e[0] * p.x + vp.e[4] * p.y + vp.e[8] * p.z + vp.e[12];
  float y = vp.e[1] * p.x + vp.e[5] * p.y + vp.e[9] * p.z + vp.e[13];
  float w = vp.e[3] * p.x + vp.e[7] * p.y + vp.e[11] * p.z + vp.e[15];
  int W, H;
  cw_drawable_size(&W, &H);
  int px = (int)((x / w * 0.5f + 0.5f) * (float)W), py = (int)((y / w * 0.5f + 0.5f) * (float)H);
  /* the window read arrives a frame or more later (on WebGPU): one in
   * flight at a time, the fields keep the newest delivered pixel */
  static uint32_t ticket;
  uint8_t c[4] = { 0 };
  if (ticket && cw_gpu_read_result(ticket, c) != 0) {
    ticket = 0;
    red_px[0] = c[0];
    red_px[1] = c[1];
    red_px[2] = c[2];
    red_ok = c[0] > 140 && c[0] > c[1] + 70 && c[0] > c[2] + 70;
  }
  if (!ticket && px >= 0 && px < W && py >= 0 && py < H) ticket = cw_gpu_read_window(px, H - 1 - py, 1, 1);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_decals", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
