/* split_array: split screen drawn in one pass (cw_pipeline_set_views_array). A small valley with every pass that draws
 * a view's part of the picture: a Terrain3D with rock and grass scatter, the sky's sun disk and its shadow, height
 * fog, a pond, a clustered lamp with a shadow and one without, a sorted particle fountain that collides with the
 * depth, a decal, SSAO and bloom. Two views side by side, or four (SA_VIEWS=4), each from its own camera; the last
 * view looks toward the sun.
 *
 * Natively: SA_VIEWS=2|4, SA_VFOG=1 (the fog volumetric), SA_CSM=1 (the sun's shadow in four cascades, which is drawn
 * view by view), SA_MODE=v4|vfog|csm (one of those by one name), SA_ONEPASS=0 (each view on its own; CW_TEST_VIEWS_ARRAY overrides either for the gates).
 *
 * Debug state: sa.views, sa.array (the last frame drew the views in one pass), sa.tick. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/environment.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static int32_t views = 2, array_used, tick;
static cw_node cams[CW_MAX_VIEWS];

static cw_node cam_at(cw_node world, const char *name, t3_vec3 at, t3_vec3 look) {
  cw_node c = cw_node_new("Camera3D", name);
  cw_node_add_child(world, c);
  cw_node_set(c, "fov", cw_val_float(60));
  cw_node_set(c, "near", cw_val_float(0.1f));
  cw_node_set(c, "far", cw_val_float(600));
  cw_node3d_set_position(c, at);
  cw_node3d_look_at(c, look, (t3_vec3){ 0, 1, 0 });
  return c;
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("sa.views", &views);
  cw_debug_i32("sa.array", &array_used);
  cw_debug_i32("sa.tick", &tick);
  cw_node root = cw_tree_root(cw_scene_app_tree());
  cw_node world = cw_node_get(root, "World");
  const char *e = getenv("SA_VIEWS"), *mode = getenv("SA_MODE"); /* SA_MODE=vfog|csm|v4: one switch (the program survey) */
  if (!mode) mode = "";
  views = (e && atoi(e) == 4) || !strcmp(mode, "v4") ? 4 : 2;
  if (((e = getenv("SA_VFOG")) && atoi(e)) || !strcmp(mode, "vfog")) cw_node_set(cw_node_get(world, "Fog"), "volumetric", cw_val_bool(true));
  if (((e = getenv("SA_CSM")) && atoi(e)) || !strcmp(mode, "csm")) {
    cw_node sun = cw_node_get(world, "Sun");
    cw_node_set(sun, "shadow_cascades", cw_val_int(4));
    cw_node_set(sun, "shadow_distance", cw_val_float(120));
  }
  cams[0] = cw_node_get(world, "Camera");
  cw_node3d_look_at(cams[0], (t3_vec3){ 0, 2, -4 }, (t3_vec3){ 0, 1, 0 });
  cams[1] = cam_at(world, "Camera2", (t3_vec3){ -16, 5, -14 }, (t3_vec3){ 2, 1, -6 });
  cams[2] = cam_at(world, "Camera3", (t3_vec3){ 14, 9, 10 }, (t3_vec3){ -4, 1, -8 });
  cams[3] = cam_at(world, "Camera4", (t3_vec3){ 0, 4, 0 }, (t3_vec3){ 0, 4, -10 });
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  cw_pipeline_split(p, cams, views);
  cw_pipeline_set_views_shared_post(p, true);
  e = getenv("SA_ONEPASS");
  cw_pipeline_set_views_array(p, !(e && *e == '0'));
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  /* the last view toward the sun (its disk and the fog's sun lobe), the second view drifting */
  cw_tree *t = cw_scene_app_tree();
  cw_sky_state sk;
  cw_sky_get_state(t, &sk);
  if (sk.active) {
    t3_vec3 d = sk.sun_direction;
    cw_node3d_look_at(cams[views - 1], (t3_vec3){ d.x * 100, 4 + d.y * 100, d.z * 100 }, (t3_vec3){ 0, 1, 0 });
  }
  float s = (float)tick * 0.01f;
  cw_node3d_set_position(cams[1], (t3_vec3){ -16 + s, 5, -14 + s * 0.5f });
  cw_node3d_look_at(cams[1], (t3_vec3){ 2, 1, -6 }, (t3_vec3){ 0, 1, 0 });
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  array_used = cw_pipeline_views_array_used(cw_pipeline_of(cw_scene_app_tree()), NULL) ? 1 : 0;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "split_array", .width = 1280, .height = 720, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
