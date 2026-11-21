/* split_array: split screen drawn in one pass (cw_pipeline_set_views_array). A small valley with every pass that draws
 * a view's part of the picture: a Terrain3D with rock and grass scatter, the sky's sun disk and its shadow, height
 * fog, a pond, a clustered lamp with a shadow and one without, a sorted particle fountain that collides with the
 * depth, a decal, SSAO and bloom. Two views side by side, or four (SA_VIEWS=4), each from its own camera; the last
 * view looks toward the sun.
 *
 * Natively: SA_VIEWS=2|4, SA_VFOG=1 (the fog volumetric), SA_CSM=1 (the sun's shadow in four cascades, which is drawn
 * view by view), SA_GI=1 (a polished metal slab in a reflection probe's box and a light probe grid, with screen-space
 * reflections and bounce light: each view's G-buffer, reflections and bounce light over its own rectangle),
 * SA_PREPASS=1 (a depth prepass before the scene), SA_WATER=1 (the pond with its planar reflection and refraction),
 * SA_DISTORT=1 (a shockwave ring and a heat haze bending the picture),
 * SA_MODE=v4|vfog|csm|gi|prepass|water|distort (one of those by one name), SA_ONEPASS=0 (each view on its own; CW_TEST_VIEWS_ARRAY overrides either for the gates).
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
  if (((e = getenv("SA_GI")) && atoi(e)) || !strcmp(mode, "gi")) {
    cw_node post = cw_node_get(world, "Post");
    cw_node_set(post, "ssr", cw_val_bool(true));
    cw_node_set(post, "ssr_max_distance", cw_val_float(12));
    cw_node_set(post, "ssgi", cw_val_bool(true));
    cw_node slab = cw_node_new("MeshInstance3D", "Slab"); /* the mirror the views see each other's parts of the valley in */
    cw_node_add_child(world, slab);
    cw_node3d_set_position(slab, (t3_vec3){ 0, 1.1f, -3 });
    cw_node_set(slab, "size", cw_val_vec3(12, 0.2f, 9));
    cw_node_set(slab, "color", cw_val_color(0.8f, 0.8f, 0.82f, 1));
    cw_node_set(slab, "metalness", cw_val_float(1));
    cw_node_set(slab, "roughness", cw_val_float(0.12f));
    cw_node_set(slab, "receive_shadow", cw_val_bool(true));
    cw_node probe = cw_node_new("ReflectionProbe3D", "Probe");
    cw_node_add_child(world, probe);
    cw_node3d_set_position(probe, (t3_vec3){ 0, 4, -3 });
    cw_node_set(probe, "size", cw_val_vec3(30, 10, 30));
    cw_node_set(probe, "box_projection", cw_val_bool(true));
    cw_node_set(probe, "far", cw_val_float(120));
    cw_node grid = cw_node_new("LightProbeGrid3D", "Bounce");
    cw_node_add_child(world, grid);
    cw_node3d_set_position(grid, (t3_vec3){ 0, 3, -3 });
    cw_node_set(grid, "size", cw_val_vec3(24, 6, 20));
    cw_node_set(grid, "probes", cw_val_vec3(5, 3, 5));
    cw_node_set(grid, "bounces", cw_val_int(1));
  }
  if (((e = getenv("SA_PREPASS")) && atoi(e)) || !strcmp(mode, "prepass")) cw_node_set(cw_node_get(world, "Post"), "depth_prepass", cw_val_bool(true));
  if (((e = getenv("SA_WATER")) && atoi(e)) || !strcmp(mode, "water")) {
    cw_node pond = cw_node_get(world, "Pond");
    cw_node_set(pond, "reflection", cw_val_bool(true));
    cw_node_set(pond, "refraction", cw_val_bool(true));
  }
  if (((e = getenv("SA_DISTORT")) && atoi(e)) || !strcmp(mode, "distort")) {
    cw_node wave = cw_node_new("Shockwave3D", "Wave");
    cw_node_add_child(world, wave);
    cw_node3d_set_position(wave, (t3_vec3){ 1, 3, -2 });
    cw_node_set(wave, "max_radius", cw_val_float(6));
    cw_node_set(wave, "duration", cw_val_float(1.2f));
    cw_node_set(wave, "thickness", cw_val_float(1.2f));
    cw_node_set(wave, "strength", cw_val_float(0.6f));
    cw_node_set(wave, "loop", cw_val_bool(true));
    cw_node_set(wave, "autostart", cw_val_bool(true));
    cw_node haze = cw_node_new("GPUParticles3D", "Haze");
    cw_node_add_child(world, haze);
    cw_node3d_set_position(haze, (t3_vec3){ -3, 1.5f, -4 });
    cw_node_set(haze, "amount", cw_val_int(300));
    cw_node_set(haze, "lifetime", cw_val_float(1.5f));
    cw_node_set(haze, "emission_shape", cw_val_enum("SPHERE"));
    cw_node_set(haze, "emission_radius", cw_val_float(0.8f));
    cw_node_set(haze, "speed_min", cw_val_float(1));
    cw_node_set(haze, "speed_max", cw_val_float(2));
    cw_node_set(haze, "gravity", cw_val_vec3(0, 1, 0));
    cw_node_set(haze, "size", cw_val_float(1.2f));
    cw_node_set(haze, "blend", cw_val_enum("DISTORT"));
    cw_node_set(haze, "distort_strength", cw_val_float(0.4f));
    cw_node_set(haze, "seed", cw_val_int(2));
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
