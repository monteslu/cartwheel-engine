/* msaa_hdr: multisampled antialiasing on an HDR scene whose later passes read the depth. A picket fence, wires,
 * slanted grass blades and thin rails against a low evening sky (every edge a staircase without MSAA), a wall
 * with MULTIPLY decals (they read the linear depth), ambient occlusion (it reads the depth), and the sky's sun
 * disc behind the fence (drawn after the opaque phase, depth-tested against the resolved scene depth). The
 * PostProcess asks for 4x MSAA on an RGBA16F scene. GL multisamples it; WebGPU does too on a core device, and
 * resolves the multisampled depth for the passes after it. The gate msaa-hdr compares GL and WebGPU.
 *
 * Switches (environment, native runs; a cart has none):
 *   CW_TEST_WGPU_NO_MSAA=1        (engine) every WebGPU scene single-sampled: the gate's control, must fail
 *   CW_WGPU_FEATURE_LEVEL=compat  (engine) a compatibility-mode device: the fallback (single-sampled here)
 *   MH_MSAA=0                     no MSAA on either backend (a cost baseline)
 *   MH_VIEWS=4                    four-way split screen, a camera per view (the cost with four views)
 *   MH_QUALITY=0..3               the Low / Medium / High / Ultra preset (cartwheel/quality.h)
 * Debug fields: mh.blades, mh.posts, mh.decals, mh.msaa (the PostProcess's samples), mh.sun (toward the sun, and its
 * elevation in degrees), mh.tick. */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/environment.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static int32_t n_blades, n_posts, n_decals, msaa;
static uint32_t tick;
static float sun[4]; /* direction toward the sun, elevation in degrees */

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

static cw_node box(cw_node w, const char *name, t3_vec3 pos, t3_vec3 rot, t3_vec3 size, float r, float g, float b) {
  cw_node n = add("MeshInstance3D", w, name);
  cw_node3d_set_position(n, pos);
  cw_node3d_set_rotation(n, rot);
  cw_node_set(n, "size", cw_val_vec3(size.x, size.y, size.z));
  cw_node_set(n, "color", cw_val_color(r, g, b, 1));
  cw_node_set(n, "roughness", cw_val_float(0.85f));
  cw_node_set(n, "cast_shadow", cw_val_bool(true));
  cw_node_set(n, "receive_shadow", cw_val_bool(true));
  return n;
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("mh.blades", &n_blades);
  cw_debug_i32("mh.posts", &n_posts);
  cw_debug_i32("mh.decals", &n_decals);
  cw_debug_i32("mh.msaa", &msaa);
  cw_debug_u32("mh.tick", &tick);
  cw_debug_f32_array("mh.sun", sun, 4);
  cw_node root = cw_tree_root(cw_scene_app_tree());
  cw_node w = cw_node_get(root, "World");
  cw_node post = cw_node_get(root, "World/Post");
  const char *e = getenv("MH_MSAA");
  if (e && *e && cw_node_valid(post)) cw_node_set(post, "msaa", cw_val_int(atoi(e)));
  if ((e = getenv("MH_QUALITY")) && *e) cw_quality_set((cw_quality)atoi(e), false);
  cw_value v;
  if (cw_node_valid(post) && cw_node_get_prop(post, "msaa", &v)) msaa = v.i;
  if ((e = getenv("MH_VIEWS")) && atoi(e) == 4) {
    static const t3_vec3 at[3][2] = { { { -4, 1.2f, 6 }, { 2, 1, -3 } }, { { 5, 0.4f, 4 }, { -2, 0.6f, 0 } },
                                       { { 0, 3.5f, 7 }, { 3, 2.5f, -3 } } };
    cw_node cams[4] = { cw_node_get(root, "World/Camera") };
    for (int i = 0; i < 3; i++) {
      cams[i + 1] = add("Camera3D", w, "SplitCam");
      cw_node_set(cams[i + 1], "fov", cw_val_float(60));
      cw_node3d_set_position(cams[i + 1], at[i][0]);
      cw_node3d_look_at(cams[i + 1], at[i][1], (t3_vec3){ 0, 1, 0 });
    }
    cw_pipeline_split(cw_pipeline_of(cw_scene_app_tree()), cams, 4);
  }
  /* the picket fence across the view: thin posts, two rails, wires above */
  for (int i = 0; i < 40; i++) {
    float x = -9.75f + (float)i * 0.5f;
    float h = 1.1f + 0.15f * hash01(i, 0, 1);
    box(w, "Post", (t3_vec3){ x, h * 0.5f, 0 }, (t3_vec3){ 0, 0, (hash01(i, 1, 2) - 0.5f) * 0.08f }, (t3_vec3){ 0.07f, h, 0.07f },
        0.55f, 0.45f, 0.35f);
    n_posts++;
  }
  box(w, "Rail", (t3_vec3){ 0, 0.35f, 0.06f }, (t3_vec3){ 0, 0, 0.012f }, (t3_vec3){ 20, 0.05f, 0.04f }, 0.5f, 0.42f, 0.33f);
  box(w, "Rail", (t3_vec3){ 0, 0.85f, 0.06f }, (t3_vec3){ 0, 0, -0.01f }, (t3_vec3){ 20, 0.05f, 0.04f }, 0.5f, 0.42f, 0.33f);
  for (int k = 0; k < 3; k++)
    box(w, "Wire", (t3_vec3){ 0, 2.6f + 0.25f * (float)k, -3 }, (t3_vec3){ 0, 0.05f, 0.02f * (float)(k - 1) },
        (t3_vec3){ 40, 0.015f, 0.015f }, 0.1f, 0.1f, 0.1f);
  /* a utility pole and its cross arm right in front of the sun disc: without the resolved depth the disc
   * would be drawn over them */
  box(w, "Pole", (t3_vec3){ 2.97f, 1.75f, -3.1f }, (t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0.08f, 3.5f, 0.08f }, 0.3f, 0.25f, 0.2f);
  box(w, "Arm", (t3_vec3){ 2.97f, 3.09f, -3.05f }, (t3_vec3){ 0, 0, 0.03f }, (t3_vec3){ 1.4f, 0.06f, 0.06f }, 0.3f, 0.25f, 0.2f);
  /* grass blades in front of the fence, slanted every way */
  for (int i = 0; i < 360; i++) {
    float x = -8 + 16 * hash01(i, 3, 4), z = 0.6f + 5.5f * hash01(i, 5, 6);
    float h = 0.25f + 0.35f * hash01(i, 7, 8);
    float g = 0.35f + 0.25f * hash01(i, 9, 10);
    box(w, "Blade", (t3_vec3){ x, h * 0.45f, z }, (t3_vec3){ (hash01(i, 11, 12) - 0.5f) * 0.7f, hash01(i, 13, 14) * 3.14f, (hash01(i, 15, 16) - 0.5f) * 0.9f },
        (t3_vec3){ 0.025f, h, 0.01f }, 0.25f * g, 0.6f * g + 0.15f, 0.12f);
    n_blades++;
  }
  /* MULTIPLY stains on the wall's face (projected along -Z) */
  for (int i = 0; i < 12; i++) {
    cw_node d = add("Decal3D", w, "Decal");
    cw_node_set(d, "texture", cw_val_string("decals/sheet.png"));
    cw_value r = { CW_T_VEC4 };
    r.v[0] = (float)(i & 1) * 128, r.v[1] = 0, r.v[2] = 128, r.v[3] = 128;
    cw_node_set(d, "region", r);
    cw_node_set(d, "blend", cw_val_enum("MULTIPLY"));
    cw_node_set(d, "color", cw_val_color(0.5f, 0.45f, 0.4f, 0.75f));
    cw_node_set(d, "edge_fade", cw_val_float(0.3f));
    cw_node_set(d, "normal_fade", cw_val_float(0.5f));
    cw_node_set(d, "size", cw_val_vec3(1.2f, 1.2f, 0.7f));
    cw_node3d_set_position(d, (t3_vec3){ -7 + 0.38f * (float)i, 0.5f + 1.4f * hash01(i, 17, 18), -1.8f });
    cw_node3d_set_rotation(d, (t3_vec3){ 0, 0, hash01(i, 19, 20) * 3 });
    n_decals++;
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
  cw_sky_state st;
  cw_sky_get_state(cw_scene_app_tree(), &st);
  sun[0] = st.sun_direction.x, sun[1] = st.sun_direction.y, sun[2] = st.sun_direction.z, sun[3] = st.sun_elevation;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "msaa_hdr", .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed,
                              .render = render };
  return cw_scene_app("main.cwscene", &base);
}
