/* lighting_changes: the clustered lighting pass follows a scene that changes
 * under it (cartwheel/lighting.h), in two-view split screen with a cascaded
 * sun.
 *
 * The pass reads the scene's lights and meshes from an index it builds once
 * a frame (not once a view), takes the clustered lights out of three.c's
 * list once a frame and marks the lit materials with their shading parts
 * only when a mesh's materials or extensions change. This app changes the
 * scene the ways a game does, every few frames, and checks after each frame
 * that the last view clustered exactly the lights it should:
 *   OmniLight3D nodes from the scene file, one of them shown only in the
 *   first view (a view hook), and lights added with three.c directly (a game
 *   adding t3_point_light_new to its own group): added, added under the
 *   scene, reparented, hidden with their parent, moved to another layer,
 *   removed, a node freed, the parent shown again;
 *   a mesh's material swapped for a new one by writing mesh->materials[0]:
 *   the next frame it carries the "cluster" and "csm" parts (unmarked, its
 *   program state is missing from the table and it is not drawn);
 *   the sun's cascades in the second view (the pass once took the sun only
 *   in the first view of a frame: the second fell back to three.c's shadow).
 *
 * Debug fields: lc.tick, lc.checks (frames checked), lc.mismatch (frames
 * whose clustered count was not the expected one), lc.remark (1: the
 * swapped-in material carried both parts the next frame), lc.cascades_bad
 * (checked frames whose last view had no cascades), lc.clustered (the last
 * view's count now).
 * LC_STRESS=n with LC_VIEWS=1..4 (tests/profile-lighting.mjs) checks nothing: an n x n grid of
 * boxes with a material each under 64 added lights, for the profiler.
 * Controls (engine test switches): CW_TEST_LIGHTS_STALE=1 keeps the first
 * index (lights added later are missed: lc.mismatch > 0); CW_TEST_NO_REMARK=1
 * marks a mesh only the first time (lc.remark 0). */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/lighting.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/shading.h"

static int32_t tick, checks, mismatch, remark = -1, cascades_bad, clustered;
static int expected = -1;
static t3_object *group;
static t3_light *raw[3];
static t3_material *swapped, *replaced;
static cw_node n2;

static cw_tree *tree(void) { return cw_scene_app_tree(); }
static cw_node node(const char *path) { return cw_node_get(cw_tree_root(tree()), path); }

static t3_light *raw_light(t3_object *parent, float x, float z, uint32_t color) {
  t3_light *l = t3_point_light_new(color, 8, 6, 2);
  t3_object_set_position(l, x, 1.2f, z);
  t3_object_add(parent, l);
  t3_release(l);
  return l;
}

/* LC_STRESS=n (tests/profile-lighting.mjs): no checks; an n x n grid of boxes, each with a
 * material of its own, under 64 lights a game added, drawn in LC_VIEWS views (1 to 4) */
static int stress, views = 2;
static void build_stress(cw_tree *t, t3_scene *sc) {
  static const char *const cams[4] = { "World/Camera", "World/Camera2", "World/Camera3", "World/Camera4" };
  cw_node v[4];
  for (int i = 0; i < views; i++) v[i] = node(cams[i]);
  if (views > 1) cw_pipeline_split(cw_pipeline_of(t), v, views);
  t3_object *grp = t3_group_new();
  t3_object_add(&sc->base, grp);
  t3_release(grp);
  t3_geometry *g = t3_box_geometry_new(0.3f, 0.3f, 0.3f, 1, 1, 1);
  for (int i = 0; i < stress * stress; i++) {
    t3_material *m = t3_mesh_standard_material_new(0x808080 + (uint32_t)(i % 97));
    t3_mesh *mesh = t3_mesh_new(g, m);
    t3_object_set_position(mesh, -8 + 16.0f * (float)(i % stress) / (float)stress, 0.2f, -8 + 16.0f * (float)(i / stress) / (float)stress);
    t3_object_add(grp, mesh);
    t3_release(mesh);
    t3_release(m);
  }
  t3_release(g);
  for (int i = 0; i < 64; i++) raw_light(grp, -8 + 2.0f * (float)(i % 8), -8 + 2.0f * (float)(i / 8), 0x808080 + (uint32_t)i * 0x020101);
}

/* N2 is the first view's alone */
static void view_hook(int view, void *u) {
  (void)u;
  t3_object *o = cw_node3d_object(n2);
  if (o) o->visible = view == 0;
}

static void frame_hook(cw_tree *t, double dt, void *u) {
  (void)dt;
  (void)u;
  tick++;
  /* the frame just drawn */
  if (expected >= 0) {
    cw_lighting_stats st;
    cw_lighting_get_stats(t, &st);
    clustered = st.clustered;
    checks++;
    if (st.clustered != expected) {
      mismatch++;
      cw_error("lighting_changes: tick %d: %d clustered, expected %d", tick, st.clustered, expected);
    }
    if (st.cascades != 2) cascades_bad++;
    if (swapped && remark < 0) {
      remark = cw_shading_has(swapped, "cluster") && cw_shading_has(swapped, "csm");
      if (!remark) cw_error("lighting_changes: the swapped-in material carries no shading parts");
    }
  }
  t3_scene *sc = cw_render3d_scene(t);
  if (stress) {
    if (tick == 1) build_stress(t, sc);
    return;
  }
  switch (tick) {
  case 1: { /* two views, N2 in the first only: the last view sees N0 and N1 */
    cw_node cams[2] = { node("World/Camera"), node("World/Camera2") };
    cw_pipeline_split(cw_pipeline_of(t), cams, 2);
    cw_pipeline_set_view_hook(cw_pipeline_of(t), view_hook, NULL);
    n2 = node("World/N2");
    expected = 2;
    break;
  }
  case 3: /* a game's own group with two lights */
    group = t3_group_new();
    t3_object_add(&sc->base, group);
    t3_release(group);
    raw[0] = raw_light(group, -2, 3, 0xffcc88);
    raw[1] = raw_light(group, 2, 3, 0x88ccff);
    expected = 4;
    break;
  case 6: /* one more, straight under the scene */
    raw[2] = raw_light(&sc->base, 0, 4, 0xff88ff);
    expected = 5;
    break;
  case 9: /* reparented: from the group to the N0 node's light */
    t3_object_add(cw_node3d_object(node("World/N0")), raw[0]);
    expected = 5;
    break;
  case 12: /* the group hidden: raw[1] with it */
    group->visible = false;
    expected = 4;
    break;
  case 15: /* raw[2] on a layer no camera sees */
    raw[2]->base.layers = 2;
    expected = 3;
    break;
  case 18: /* raw[2] removed (and freed) */
    t3_object_remove(raw[2]->base.parent, raw[2]);
    raw[2] = NULL;
    expected = 3;
    break;
  case 21: /* a node freed: N1 */
    cw_node_free(node("World/N1"));
    expected = 2;
    break;
  case 24: { /* the ground's material swapped for a new one by a plain write */
    t3_object *o = cw_node3d_object(node("World/Ground"));
    while (o && o->type != T3_MESH && o->child_count) o = o->children[0]; /* (the node's mesh) */
    if (!o || o->type != T3_MESH) {
      cw_error("lighting_changes: no ground mesh");
      break;
    }
    t3_mesh *m = (t3_mesh *)o;
    swapped = t3_mesh_standard_material_new(0x8899aa);
    swapped->roughness = 0.6f;
    /* the mesh's reference to the old one is kept here: the node still knows that material */
    replaced = m->materials[0];
    m->materials[0] = swapped; /* (the mesh holds the new one's reference) */
    break;
  }
  case 27: /* the group shown again */
    group->visible = true;
    expected = 3;
    break;
  default: break;
  }
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("lc.tick", &tick);
  cw_debug_i32("lc.checks", &checks);
  cw_debug_i32("lc.mismatch", &mismatch);
  cw_debug_i32("lc.remark", &remark);
  cw_debug_i32("lc.cascades_bad", &cascades_bad);
  cw_debug_i32("lc.clustered", &clustered);
  if (getenv("LC_STRESS")) stress = atoi(getenv("LC_STRESS"));
  if (getenv("LC_VIEWS")) views = CW_CLAMP(atoi(getenv("LC_VIEWS")), 1, 4);
  cw_tree_add_hook(tree(), 2, 0, frame_hook, NULL);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "lighting_changes", .width = 640, .height = 360, .debug = true, .init = init };
  return cw_scene_app("main.cwscene", &base);
}
