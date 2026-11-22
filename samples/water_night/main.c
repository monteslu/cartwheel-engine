/* water_night: a night sea under a moon, with no ProceduralSky. The WorldEnvironment clears to black with no
 * ambient light; a DirectionalLight3D, the moon, shines from low in the sky toward the camera; the moon's disc, a
 * glow around it and along the horizon, and a row of lights on a far shore are emissive meshes drawn additively
 * (transparent, no depth write) over the black, on a layer of their own (2) that the cameras draw (cull_layers 3).
 * Water3D takes its glint, crest light and see-through light from the moon (the tree's first DirectionalLight3D, or
 * its `sun`), and its planar reflection mirrors what the camera draws, the backdrop's layer included.
 *
 * Environment: WN_SPLIT=1 two players' views of the same sea side by side (a second camera, split screen in one
 * pass).
 * Debug state: water.tick, water.see_light, water.sun_lit (1: lit by a scene light), split.array (1: the split
 * screen ran in one pass). */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/water.h"

static uint32_t tick;
static float see_light;
static int32_t sun_lit, array_used;
static bool built;

static t3_material *glow(t3_color c) {
  t3_material *m = t3_mesh_basic_material_new(0xffffff);
  m->color = c;
  m->transparent = true;
  m->depth_write = false;
  m->blending = T3_ADDITIVE_BLENDING;
  m->fog = false;
  return m;
}

static void put(t3_object *parent, t3_geometry *g, t3_material *m, t3_vec3 p, t3_vec3 s, int order) {
  t3_mesh *me = t3_mesh_new(g, m);
  me->base.position = p;
  me->base.scale = s;
  me->base.render_order = order;
  t3_object_set_layers(me, 2); /* the backdrop's own layer (the cameras draw 1 and 2: cull_layers 3) */
  t3_object_add(parent, me);
  t3_release(me);
}

static void build(void) {
  cw_tree *t = cw_scene_app_tree();
  cw_node root = cw_tree_root(t);
  cw_node sky = cw_node_get(root, "Root/Sky");
  cw_node moon = cw_node_get(root, "Root/Moon");
  if (!cw_node_valid(sky) || !cw_node_valid(moon)) return;
  t3_object *parent = cw_node3d_object(sky);
  /* the moon, low over the sea ahead: its light shines from there toward the camera */
  const t3_vec3 mp = { 0, 70, -900 };
  cw_node3d_look_at(moon, (t3_vec3){ -mp.x, -mp.y, -mp.z }, (t3_vec3){ 0, 1, 0 });
  t3_geometry *disc = t3_circle_geometry_new(1, 48);
  t3_material *mm = glow((t3_color){ 3.0f, 3.1f, 3.4f });
  put(parent, disc, mm, mp, (t3_vec3){ 22, 22, 1 }, 2);
  /* a soft glow around it and along the horizon: wide additive planes, dim */
  t3_geometry *quad = t3_plane_geometry_new(1, 1, 1, 1);
  t3_material *halo = glow((t3_color){ 0.05f, 0.07f, 0.14f });
  put(parent, disc, halo, (t3_vec3){ mp.x, mp.y, mp.z - 5 }, (t3_vec3){ 110, 110, 1 }, 1);
  t3_material *band = glow((t3_color){ 0.03f, 0.035f, 0.08f });
  put(parent, quad, band, (t3_vec3){ 0, 10, -950 }, (t3_vec3){ 4000, 70, 1 }, 0);
  /* lights along a far shore, 12 m over the water */
  t3_geometry *ball = t3_sphere_geometry_new(1, 16, 8);
  t3_material *lamp = glow((t3_color){ 4.0f, 1.6f, 0.35f });
  for (int i = 0; i < 9; i++) put(parent, ball, lamp, (t3_vec3){ -160 + 40 * (float)i, 6.0f, -700 }, (t3_vec3){ 3.0f, 3.0f, 3.0f }, 3);
  t3_release(disc), t3_release(quad), t3_release(ball);
  t3_release(mm), t3_release(halo), t3_release(band), t3_release(lamp);
  if (getenv("WN_SPLIT")) {
    cw_node c2 = cw_node_new("Camera3D", "Camera2");
    cw_node c1 = cw_node_get(root, "Root/Camera");
    cw_node_set(c2, "fov", cw_val_float(55));
    cw_node_set(c2, "far", cw_val_float(3000));
    cw_node_set(c2, "cull_layers", cw_val_int(3));
    cw_node_add_child(cw_node_get(root, "Root"), c2);
    cw_node3d_set_position(c2, cw_node3d_position(c1));
    cw_node3d_set_rotation(c2, (t3_vec3){ -0.02f, 0, 0 });
    cw_node cams[2] = { c1, c2 };
    cw_pipeline *p = cw_pipeline_of(t);
    cw_pipeline_split(p, cams, 2);
    cw_pipeline_set_views_shared_post(p, true); /* (one pass needs the shared post chain) */
    cw_pipeline_set_views_array(p, true);
  }
  built = true;
}

static void init(void *u) {
  (void)u;
  cw_debug_u32("water.tick", &tick);
  cw_debug_f32("water.see_light", &see_light);
  cw_debug_i32("water.sun_lit", &sun_lit);
  cw_debug_i32("split.array", &array_used);
}
static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_water_stats st;
  cw_water_get_stats(cw_scene_app_tree(), &st);
  see_light = st.see_light;
  sun_lit = st.sun_light;
  array_used = cw_pipeline_views_array_used(cw_pipeline_of(cw_scene_app_tree()), NULL) ? 1 : 0;
}
static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  /* (on the third tick: a cart host delivers a test's environment, WN_*, with its first requests) */
  if (!built && tick >= 2) build();
  tick++;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = {
    .title = "water_night", .width = 960, .height = 540, .debug = true, .init = init, .update = update, .render = render,
  };
  return cw_scene_app("main.cwscene", &base);
}
