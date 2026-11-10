/* decal_late: the first image into the decal atlas mid-run. A floor under a
 * camera and a Decal3D with no image (the built-in splat) from the start, so
 * the decals pass and its pipeline are warm; at tick 60 two Decal3D nodes
 * with decals/scorch.png (the atlas's first image) are stamped: a big one
 * and a far small one, which samples the atlas's mip 2. The frame of the
 * stamp must cost no more than any other, on GL and WebGPU (gate
 * decal-first-image, romdev's frame timing).
 *
 * Switches (cw_test_env, read from frame 2): DECAL_PRELOAD=1 puts the image
 * into the atlas at tick 1 (cw_decal_preload, what a game does at load for
 * the images it stamps in play), so the stamp costs nothing to load; without
 * it the stamp's frame decodes the image. DECAL_AT_LOAD=1 stamps them at the
 * start instead (how a game worked around the hitch: a decal with the image
 * drawn at load). The engine's CW_TEST_DECAL_FULL_MIPS=1 is the path before
 * v0.46.2 (the image loads at its first draw and every mip of the whole
 * atlas is rebuilt then): the gate's control, it must show the hitch.
 * CW_TEST_DECAL_NO_MIPS=1 leaves a new cell's mips empty: far_ok fails.
 *
 * Debug state: late.tick, late.atlas (images in the atlas), late.visible,
 * late.stamped (the frame of the stamp; -1 before it), late.load_ms and
 * late.decode_ms (what the image cost to put into the atlas, and its decode:
 * cw_decal_stats),
 * late.far_ok (the far scorch darkens the floor: its centre pixel is darker
 * than the bare floor beside it), late.far_px / late.ref_px (those pixels). */
#include "cartwheel/debug.h"
#include "cartwheel/decals.h"
#include "cartwheel/gpu.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static int32_t n_atlas, n_visible, stamped = -1, far_ok, far_px[3], ref_px[3];
static uint32_t tick;
static float load_ms, decode_ms;
static bool switches_read, at_load;

static cw_node world(void) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), "World"); }

static void init(void *u) {
  (void)u;
  cw_debug_u32("late.tick", &tick);
  cw_debug_i32("late.atlas", &n_atlas);
  cw_debug_i32("late.visible", &n_visible);
  cw_debug_i32("late.stamped", &stamped);
  cw_debug_f32("late.load_ms", &load_ms);
  cw_debug_f32("late.decode_ms", &decode_ms);
  cw_debug_i32("late.far_ok", &far_ok);
  cw_debug_field("late.far_px", far_px, CW_DBG_I32, 3);
  cw_debug_field("late.ref_px", ref_px, CW_DBG_I32, 3);
}

static cw_node scorch(const char *name, t3_vec3 pos, float size, float turn) {
  cw_node d = cw_node_new("Decal3D", name);
  cw_node3d_set_position(d, pos);
  cw_node3d_set_rotation(d, (t3_vec3){ -CW_PI / 2, 0, turn });
  cw_node_set(d, "size", cw_val_vec3(size, size, 1.2f));
  cw_node_set(d, "texture", cw_val_string("decals/scorch.png"));
  cw_node_set(d, "blend", cw_val_enum("MULTIPLY"));
  cw_node_set(d, "normal_fade", cw_val_float(0));
  cw_node_add_child(world(), d);
  return d;
}

/* the first image into the atlas: a big mark in the middle and a far small one (mip 2) */
static void stamp(void) {
  scorch("Late", (t3_vec3){ 1.5f, 0.5f, 1.0f }, 2.5f, 0.3f);
  scorch("LateFar", (t3_vec3){ -1.0f, 0.5f, -3.0f }, 0.6f, 0);
  stamped = (int32_t)cw_frame_index();
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  if (!switches_read && cw_frame_index() >= 1) {
    switches_read = true;
    const char *e = cw_test_env("DECAL_AT_LOAD");
    at_load = e && *e == '1';
    e = cw_test_env("DECAL_PRELOAD");
    if (e && *e == '1') cw_decal_preload("decals/scorch.png");
  }
  if (stamped < 0 && ((switches_read && at_load) || tick == 60)) stamp();
}

/* where a floor point lands on screen; false when off it */
static bool on_screen(t3_vec3 p, int *px, int *py) {
  t3_camera *cam = cw_camera3d_t3(cw_node_get(world(), "Camera"));
  t3_mat4 view, vp;
  t3_mat4_invert(&view, &cam->base.matrix_world);
  t3_mat4_multiply(&vp, &cam->projection_matrix, &view);
  float x = vp.e[0] * p.x + vp.e[4] * p.y + vp.e[8] * p.z + vp.e[12];
  float y = vp.e[1] * p.x + vp.e[5] * p.y + vp.e[9] * p.z + vp.e[13];
  float w = vp.e[3] * p.x + vp.e[7] * p.y + vp.e[11] * p.z + vp.e[15];
  int W, H;
  cw_drawable_size(&W, &H);
  *px = (int)((x / w * 0.5f + 0.5f) * (float)W);
  *py = H - 1 - (int)((y / w * 0.5f + 0.5f) * (float)H);
  return *px >= 0 && *px < W && *py >= 0 && *py < H;
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  cw_decal_stats st;
  cw_decals_get_stats(cw_scene_app_tree(), &st);
  n_atlas = st.atlas_images;
  n_visible = st.visible;
  load_ms = st.atlas_load_ms;
  decode_ms = st.atlas_decode_ms;
  /* the far mark's centre and a bare floor point beside it, read in turn; a
   * window read arrives a frame or more later (WebGPU), one in flight */
  static uint32_t ticket;
  static int which;
  uint8_t c[4] = { 0 };
  if (ticket && cw_gpu_read_result(ticket, c) != 0) {
    ticket = 0;
    int32_t *o = which ? ref_px : far_px;
    o[0] = c[0];
    o[1] = c[1];
    o[2] = c[2];
    int f = far_px[0] + far_px[1] + far_px[2], r = ref_px[0] + ref_px[1] + ref_px[2];
    far_ok = r > 60 && f * 10 < r * 6;
    which ^= 1;
  }
  int px, py;
  if (!ticket && on_screen(which ? (t3_vec3){ 1.0f, 0, -3.0f } : (t3_vec3){ -1.0f, 0, -3.0f }, &px, &py))
    ticket = cw_gpu_read_window(px, py, 1, 1);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "decal_late", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
