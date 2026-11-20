/* static_batch: five thousand different props in a handful of draws
 * (MeshInstance3D `batch`, cartwheel/static_batch.h).
 *
 * A field of 5,000 boulders and crates, every one a geometry of its own
 * (each boulder a differently dented 20-sided stone, each crate its own
 * size), in three materials, under a cascaded sun whose shadows they cast and
 * receive. Automatic instancing cannot merge them (no two share a geometry):
 * unbatched, each is a draw in the scene pass and one more in every shadow
 * cascade that holds it. With `batch = true` each material's props are one
 * BatchedMesh and the ones in view that follow each other in its buffer are
 * one draw. The props are laid out row by row, so neighbours in the field are
 * neighbours in the buffer.
 *
 * The camera looks over the whole field, then from tick 120 swings to one
 * side, then turns back: props leave the view and come back, and the batches
 * draw only the ones in view (static.drawn falls and returns).
 *
 * static_batch_lit is the same field in the lighting a game has: clustered lighting with sixteen omni lights, the
 * cascaded sun and a probe grid's bounce light, so every prop's material carries the engine's cluster, csm and gi
 * shading parts (lit.cwscene). The batches form and draw the same way.
 *
 * STATIC_BATCH_OFF=1 leaves every prop a draw of its own (the gate's control,
 * and the "before" of the timing). STATIC_BATCH_COUNT=n props (default 5000).
 *
 * Debug command: `batch 0` / `batch 1` switches every prop while running (the timing's A and B).
 *
 * Debug fields: static.meshes, static.batches, static.geometries, static.draws
 * and static.drawn (cw_static_batch_get_stats), static.props, static.tick. */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/lighting.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/static_batch.h"

#define MAX_PROPS 20000
static int32_t meshes, batches, geometries, draws, drawn, props, tick32, clustered, lit_materials;
static int count = 5000;
static bool off;
static cw_node prop_nodes[MAX_PROPS];
static uint32_t seed = 12345;

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }
/* a small integer generator of the sample's own: the same props on every host */
static float rnd(void) {
  seed = seed * 1664525u + 1013904223u;
  return (float)(seed >> 8) * (1.0f / 16777216.0f);
}

/* a stone: an icosahedron with every corner pushed in or out, flat shaded (20 triangles, 60 vertices) */
static t3_geometry *stone(void) {
  static const float T = 1.618034f;
  static const float V[12][3] = { { -1, T, 0 }, { 1, T, 0 }, { -1, -T, 0 }, { 1, -T, 0 }, { 0, -1, T }, { 0, 1, T },
                                  { 0, -1, -T }, { 0, 1, -T }, { T, 0, -1 }, { T, 0, 1 }, { -T, 0, -1 }, { -T, 0, 1 } };
  static const uint8_t F[20][3] = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 },
                                    { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 },
                                    { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
  float v[12][3], pos[180], nrm[180], uv[120];
  float sx = 0.7f + 0.6f * rnd(), sy = 0.45f + 0.5f * rnd(), sz = 0.7f + 0.6f * rnd();
  for (int i = 0; i < 12; i++) {
    float k = (0.75f + 0.5f * rnd()) / 1.902f;
    v[i][0] = V[i][0] * k * sx, v[i][1] = V[i][1] * k * sy, v[i][2] = V[i][2] * k * sz;
  }
  for (int f = 0; f < 20; f++) {
    const float *a = v[F[f][0]], *b = v[F[f][1]], *c = v[F[f][2]];
    t3_vec3 n = t3_vec3_normalize(t3_vec3_cross(t3_v3(b[0] - a[0], b[1] - a[1], b[2] - a[2]), t3_v3(c[0] - a[0], c[1] - a[1], c[2] - a[2])));
    for (int k = 0; k < 3; k++) {
      const float *p = v[F[f][k]];
      pos[f * 9 + k * 3] = p[0], pos[f * 9 + k * 3 + 1] = p[1], pos[f * 9 + k * 3 + 2] = p[2];
      nrm[f * 9 + k * 3] = n.x, nrm[f * 9 + k * 3 + 1] = n.y, nrm[f * 9 + k * 3 + 2] = n.z;
      uv[f * 6 + k * 2] = 0.5f + 0.5f * p[0], uv[f * 6 + k * 2 + 1] = 0.5f + 0.5f * p[2];
    }
  }
  t3_geometry *g = t3_geometry_new();
  t3_attribute *ap = t3_attribute_new(T3_FLOAT32, pos, 60, 3), *an = t3_attribute_new(T3_FLOAT32, nrm, 60, 3), *au = t3_attribute_new(T3_FLOAT32, uv, 60, 2);
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, ap);
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, an);
  t3_geometry_set_attribute(g, T3_ATTR_UV, au);
  t3_release(ap); t3_release(an); t3_release(au);
  return g;
}

/* debug command: batch 0 | 1 (applied at the next tick): every prop out of, or back into, the batches */
static int want_batch = -1;
static void cmd_batch(int argc, const char **argv, cw_reply *r) {
  want_batch = argc > 1 ? atoi(argv[1]) != 0 : 1;
  cw_reply_printf(r, "{\"batch\":%d}", want_batch);
}

static void init(void *u) {
  (void)u;
  cw_debug_command("batch", "batch <0|1>: every prop draws on its own (0) or through the static batches (1)", cmd_batch);
  cw_debug_i32("static.meshes", &meshes);
  cw_debug_i32("static.batches", &batches);
  cw_debug_i32("static.geometries", &geometries);
  cw_debug_i32("static.draws", &draws);
  cw_debug_i32("static.drawn", &drawn);
  cw_debug_i32("static.props", &props);
  cw_debug_i32("static.tick", &tick32);
  cw_debug_i32("static.clustered", &clustered);         /* clustered lights (static_batch_lit) */
  cw_debug_i32("static.lit_materials", &lit_materials); /* material slots carrying the engine's lighting parts */
  const char *e = getenv("STATIC_BATCH_COUNT");
  if (e && atoi(e) > 0) count = CW_MIN(atoi(e), MAX_PROPS);
  /* three materials: the props of each are one batch */
  t3_material *mats[3] = { t3_mesh_standard_material_new(0xffffff), t3_mesh_standard_material_new(0xffffff), t3_mesh_standard_material_new(0xffffff) };
  mats[0]->color = (t3_color){ 0.11f, 0.105f, 0.1f }, mats[0]->roughness = 0.9f;
  mats[1]->color = (t3_color){ 0.13f, 0.075f, 0.04f }, mats[1]->roughness = 0.85f;
  mats[2]->color = (t3_color){ 0.24f, 0.13f, 0.04f }, mats[2]->roughness = 0.7f;
  cw_node parent = node("World/Props");
  int cols = 100, rows = (count + cols - 1) / cols;
  for (int i = 0; i < count; i++) {
    int col = i % cols, row = i / cols;
    float kind = rnd();
    t3_geometry *g;
    int mi;
    float lift;
    if (kind < 0.8f) {
      g = stone();
      mi = kind < 0.5f ? 0 : 1;
      lift = 0.25f;
    } else { /* a crate of its own size */
      float w = 0.7f + 0.8f * rnd(), h = 0.6f + 0.7f * rnd(), d = 0.7f + 0.8f * rnd();
      g = t3_box_geometry_new(w, h, d, 1, 1, 1);
      mi = 2;
      lift = h * 0.5f;
    }
    t3_mesh *mesh = t3_mesh_new(g, mats[mi]);
    mesh->base.cast_shadow = mesh->base.receive_shadow = true;
    t3_release(g);
    cw_node n = cw_node_new("MeshInstance3D", "Prop");
    cw_mesh3d_set_procedural(n, mesh);
    t3_release(mesh);
    cw_node_add_child(parent, n);
    float s = kind < 0.8f ? 0.5f + 1.1f * rnd() * rnd() : 1.0f;
    cw_node3d_set_position(n, (t3_vec3){ ((float)col - (float)cols * 0.5f) * 1.9f + 1.2f * (rnd() - 0.5f), lift * s,
                                         ((float)row - (float)rows * 0.5f) * 1.9f + 1.2f * (rnd() - 0.5f) });
    cw_node3d_set_rotation(n, (t3_vec3){ 0, rnd() * 6.2831853f, 0 });
    cw_node3d_set_scale(n, (t3_vec3){ s, s, s });
    cw_node_set(n, "batch", cw_val_bool(true));
    prop_nodes[i] = n;
  }
  for (int k = 0; k < 3; k++) t3_release(mats[k]);
  props = count;
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick32++;
  if (tick32 == 2 && cw_test_env("STATIC_BATCH_OFF")) { /* the gate's switch, from the first frame every host can set it */
    off = true;
    for (int i = 0; i < count; i++) cw_node_set(prop_nodes[i], "batch", cw_val_bool(false));
  }
  if (want_batch >= 0) {
    for (int i = 0; i < count; i++) cw_node_set(prop_nodes[i], "batch", cw_val_bool(want_batch != 0));
    want_batch = -1;
  }
  /* the camera: over the field, a swing to the side from tick 120 and back by tick 360 (eased) */
  float t = tick32 < 120 ? 0 : tick32 > 360 ? 0 : (float)(tick32 - 120) / 240.0f;
  float yaw = 0.9f * cw_sin(t * CW_PI);
  cw_node_set(node("World/Camera"), "rotation", cw_val_vec3(-0.3f, yaw, 0));
  cw_static_batch_stats st;
  cw_static_batch_get_stats(cw_scene_app_tree(), &st);
  cw_lighting_stats ls;
  cw_lighting_get_stats(cw_scene_app_tree(), &ls);
  clustered = ls.clustered, lit_materials = ls.materials;
  meshes = st.meshes, batches = st.batches, geometries = st.geometries, draws = st.draws, drawn = st.drawn;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "static_batch", .width = 960, .height = 540, .debug = true, .init = init, .fixed_update = fixed };
#ifdef STATIC_BATCH_LIT
  return cw_scene_app("lit.cwscene", &base);
#else
  return cw_scene_app("main.cwscene", &base);
#endif
}
