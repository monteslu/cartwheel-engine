/* fog_transparents: transparent and additive surfaces in height fog take the fog at their own distance
 * (cartwheel/environment.h). A flat plain reaching 400 m away under a clear sky that the fog leaves alone
 * (sky_distance 0); the camera at eye height looks along it, so the horizon crosses the middle of the picture.
 * Standing on the ground 38 m away, each from 0 to 4 m high so it crosses the horizon:
 *   Glass  a lit alpha-blended panel (MeshStandardMaterial, opacity 0.5)
 *   Add    an additive panel (MeshBasicMaterial, AdditiveBlending)
 *   Fire   additive GPUParticles3D flames rising from the ground
 *   Smoke  alpha-blended GPUParticles3D
 * Each must be fogged as anything 38 m away is, above the horizon (over the sky) and below it (over ground 80 to
 * 400 m away) alike: the fog of the far ground behind it must not reach it.
 *
 * Test switches (cw_test_env, from frame 2):
 *   FT_FOG=0       the fog's density 0 (the reference without fog)
 *   FT_HIDE=1      the four subjects hidden (the background alone)
 *   FT_VFOG=1      the fog volumetric (the froxel grid)
 *   FT_NOPANELS=1  no Glass and Add panels (the particles alone)
 *   FT_OPTOUT=1    the fog opt-out: the four subjects hidden, the fog's max_opacity 0.9, and four unlit
 *                  transparent panels writing no depth (MeshBasicMaterial, opacity 1, colour 0.7 0.12 0.06): NearOff
 *                  (fog = false) and NearOn (fog = true) 38 m away from 0 to 4 m high, FarOff and FarOn 900 m away
 *                  from 30 to 130 m high over the sky (a sky ring's mountains). A fog = false one takes no fog.
 *   FT_MODE=split     two views side by side, the same camera in each
 *          backdrop  the world drawn as a Backdrop of an empty scene (cartwheel/backdrop.h), its fog its own
 *
 * Debug state: ft.mode (0 plain, 1 split, 2 backdrop), ft.trans (the "transparents" pass ran last frame),
 * ft.fog (a fog pass ran last frame: height_fog or volumetric_fog), ft.applied, ft.gpu_ms (CW_GPU_TIMERS=1, host
 * timers: the GPU ms of every pass, the scene pass, the fog passes, the transparents pass and the particles,
 * averaged). */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/backdrop.h"
#include "cartwheel/debug.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"
#include "three.h"

static cw_node root, world, backdrop;
static int32_t mode, trans_ran, fog_ran, applied, quads_done;
static float gpu_ms[5]; /* GPU ms averaged (CW_GPU_TIMERS=1): every pass, the scene, the fog passes, transparents, particles */

static void init(void *u) {
  (void)u;
  cw_debug_i32("ft.mode", &mode);
  cw_debug_i32("ft.trans", &trans_ran);
  cw_debug_i32("ft.fog", &fog_ran);
  cw_debug_i32("ft.applied", &applied);
  cw_debug_f32_array("ft.gpu_ms", gpu_ms, 5); /* host timers: never compared */
  root = cw_tree_root(cw_scene_app_tree());
  world = cw_scene_instance_path("world.cwscene");
  cw_node_add_child(root, world);
}

/* the two panels: three.c meshes under the world's Glass and Add nodes */
static void optout_panel(cw_node w, bool fog, float x, float y, float z, float size) {
  t3_geometry *g = t3_plane_geometry_new(size, size, 1, 1);
  t3_material *m = t3_mesh_basic_material_new(0x000000);
  m->color = (t3_color){ 0.7f, 0.12f, 0.06f };
  m->transparent = true;
  m->depth_write = false;
  m->fog = fog;
  t3_mesh *me = t3_mesh_new(g, m);
  t3_object_set_position(me, x, y, z);
  t3_object_add(cw_node3d_object(w), me);
  t3_release(me);
  t3_release(g);
  t3_release(m);
}

static void add_panels(cw_node w) {
  t3_geometry *g = t3_plane_geometry_new(4, 4, 1, 1);
  t3_material *glass = t3_mesh_standard_material_new(0x3080c0);
  glass->transparent = true;
  glass->opacity = 0.5f;
  glass->roughness = 0.6f;
  glass->side = T3_DOUBLE_SIDE;
  t3_mesh *mg = t3_mesh_new(g, glass);
  t3_object_set_position(mg, 0, 2, 0);
  t3_object_add(cw_node3d_object(cw_node_get(w, "Glass")), mg);
  t3_material *add = t3_mesh_basic_material_new(0x000000);
  add->color = (t3_color){ 0.36f, 0.16f, 0.07f };
  add->transparent = true;
  add->blending = T3_ADDITIVE_BLENDING;
  add->depth_write = false;
  add->side = T3_DOUBLE_SIDE;
  t3_mesh *ma = t3_mesh_new(g, add);
  t3_object_set_position(ma, 0, 2, 0);
  t3_object_add(cw_node3d_object(cw_node_get(w, "Add")), ma);
  t3_release(mg);
  t3_release(ma);
  t3_release(g);
  t3_release(glass);
  t3_release(add);
}

static bool on(const char *name) {
  const char *e = cw_test_env(name);
  return e && atoi(e) != 0;
}

static void apply(void) {
  if (!cw_test_env("FT_MODE") && !cw_test_env("FT_FOG") && !cw_test_env("FT_HIDE") && !cw_test_env("FT_VFOG") && !cw_test_env("FT_OPTOUT") && cw_frame_index() < 2)
    return; /* (a test switch lands from frame 2: a native run has them from the start) */
  applied = 1;
  const char *m = cw_test_env("FT_MODE");
  mode = m && !strcmp(m, "split") ? 1 : m && !strcmp(m, "backdrop") ? 2 : 0;
  if (mode == 2) {
    /* the world behind an empty scene of its own: its camera, fog, particles and panels, its own pipeline */
    cw_node_free(world);
    world = (cw_node){ 0 };
    backdrop = cw_node_new("Backdrop", "Backdrop");
    cw_node_set(backdrop, "scene", cw_val_string("world.cwscene"));
    cw_node_add_child(root, backdrop);
    cw_node cam = cw_node_new("Camera3D", "Camera");
    cw_node_add_child(root, cam);
    cw_node_set(cam, "current", cw_val_bool(true));
    cw_node post = cw_node_new("PostProcess", "Post");
    cw_node_set(post, "tonemap", cw_val_enum("LINEAR"));
    cw_node_set(post, "bloom", cw_val_bool(false));
    cw_node_add_child(root, post);
  }
}

/* the world's switches, once it is there (a Backdrop's world is made as it enters the tree) */
static void setup_world(void) {
  cw_node w = mode == 2 ? cw_backdrop_root(backdrop) : world;
  if (!cw_node_valid(w)) return;
  if (!on("FT_NOPANELS") && !on("FT_OPTOUT")) add_panels(w);
  if (on("FT_OPTOUT")) {
    static const char *const hide[] = { "Glass", "Add", "Fire", "Smoke" };
    for (int i = 0; i < 4; i++) cw_node_set(cw_node_get(w, hide[i]), "visible", cw_val_bool(false));
    cw_node_set(cw_node_get(w, "Fog"), "max_opacity", cw_val_float(0.9f));
    optout_panel(w, false, -6, 2, -38, 4);
    optout_panel(w, true, 6, 2, -38, 4);
    optout_panel(w, false, -300, 80, -900, 100);
    optout_panel(w, true, 300, 80, -900, 100);
  }
  quads_done = 1;
  if (cw_test_env("FT_FOG") && !on("FT_FOG")) cw_node_set(cw_node_get(w, "Fog"), "density", cw_val_float(0));
  if (on("FT_VFOG")) cw_node_set(cw_node_get(w, "Fog"), "volumetric", cw_val_bool(true));
  if (on("FT_HIDE")) {
    static const char *const hide[] = { "Glass", "Add", "Fire", "Smoke" };
    for (int i = 0; i < 4; i++) cw_node_set(cw_node_get(w, hide[i]), "visible", cw_val_bool(false));
  }
  if (mode == 1) {
    cw_node cam = cw_node_get(w, "Camera");
    cw_node cam2 = cw_node_new("Camera3D", "Camera2");
    cw_node_set(cam2, "current", cw_val_bool(false));
    cw_node_set(cam2, "fov", cw_val_float(50));
    cw_node_set(cam2, "far", cw_val_float(1000));
    cw_node_add_child(w, cam2);
    cw_node3d_set_position(cam2, (t3_vec3){ 0, 1.6f, 0 });
    cw_node cams[2] = { cam, cam2 };
    cw_pipeline_split(cw_pipeline_of(cw_scene_app_tree()), cams, 2);
  }
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  if (!applied) apply();
  if (applied && !quads_done) setup_world();
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_tree *t = mode == 2 && cw_node_valid(backdrop) ? cw_backdrop_tree(backdrop) : cw_scene_app_tree();
  if (!t) return;
  const cw_pass_stat *st;
  int n = cw_pipeline_pass_stats(cw_pipeline_of(t), &st);
  trans_ran = fog_ran = 0;
  memset(gpu_ms, 0, sizeof gpu_ms);
  for (int i = 0; i < n; i++) {
    float g = st[i].ran && st[i].gpu_ms_avg > 0 ? st[i].gpu_ms_avg : 0;
    gpu_ms[0] += g;
    if (!strcmp(st[i].name, "scene")) gpu_ms[1] += g;
    if (!strncmp(st[i].name, "height_fog", 10) || !strncmp(st[i].name, "volumetric_fog", 14)) gpu_ms[2] += g;
    if (!strcmp(st[i].name, "transparents")) gpu_ms[3] += g;
    if (!strcmp(st[i].name, "particles")) gpu_ms[4] += g;
    if (!strcmp(st[i].name, "transparents")) trans_ran |= st[i].ran;
    if (!strcmp(st[i].name, "height_fog") || !strcmp(st[i].name, "volumetric_fog") || !strncmp(st[i].name, "height_fog.", 11) ||
        !strncmp(st[i].name, "volumetric_fog.", 15))
      fog_ran |= st[i].ran;
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fog_transparents", .width = 1280, .height = 720, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
