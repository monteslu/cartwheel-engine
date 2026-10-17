/* fx_lit_decals: mesh decals take the light of the surface they sit on.
 * Three identical grey stickers (Decal3D, MESH mode, MIX) on one wall: one in
 * the dark (a roof slab shades it from the sun, the lamp does not reach it),
 * one under a lamp, one in the sun. Each must be as dark or bright as the wall
 * around it: tests/lit-decals.mjs compares the pixels of each sticker with
 * the wall beside it.
 *
 * FX_LIT_FORWARD=1 uses FORWARD lighting (three.c's own light loop and
 * shadow maps) instead of CLUSTERED (the cluster and csm shading parts).
 * FX_LIT_GI=1 adds a reflection probe, so every Standard material carries the gi shading part.
 * CW_DECAL_UNLIT=1 draws mesh decals unlit as they were before (the control).
 * Two characters (Khronos CesiumMan, CC-BY-4.0, from deps/three.c's test assets), one in the dark and
 * one under the lamp, each carry a badge clipped onto the chest and skinned to the skeleton.
 *
 * Debug state: ld.tick, ld.decals, ld.tris. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/decals.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/quality.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static t3_gltf *gltf[2];

/* the character takes shadows (a glTF mesh does not unless told to) */
static void receive_shadows(t3_object *o) {
  if (o->type == T3_MESH || o->type == T3_SKINNED_MESH) o->receive_shadow = true;
  for (int i = 0; i < o->child_count; i++) receive_shadows(o->children[i]);
}
static uint32_t tick;
static int32_t n_decals, n_tris;
/* FX_LIT_COUNT=N spawns N more MIX decals on the wall and FX_LIT_QUALITY=low|medium|high|ultra
 * picks a preset: the cost of lit decals (ld.gpu_ms is the mean GPU time of a frame from frame
 * 60 on, with CW_GPU_TIMERS=1; never compared across hosts) */
static float gpu_ms, gpu_sum;
static int gpu_n;

static void init(void *u) {
  (void)u;
  const char *e = getenv("FX_LIT_FORWARD");
  cw_node w = cw_node_get(cw_tree_root(cw_scene_app_tree()), "World");
  if (!(e && *e == '1')) {
    cw_node lighting = cw_node_new("LightingSettings", "Lighting");
    cw_node_set(lighting, "mode", cw_val_enum("CLUSTERED"));
    cw_node_add_child(w, lighting);
    cw_node_set(cw_node_get(w, "Sun"), "shadow_cascades", cw_val_int(2));
  }
  e = getenv("FX_LIT_GI");
  if (e && *e == '1') {
    /* GI: every Standard material (the decals' too) carries the gi shading part and reflects the probe */
    cw_node probe = cw_node_new("ReflectionProbe3D", "Probe");
    cw_node3d_set_position(probe, (t3_vec3){ 0, 1.5, 2 });
    cw_node_set(probe, "size", cw_val_vec3(14, 4, 8));
    cw_node_set(probe, "update_mode", cw_val_enum("ONCE"));
    cw_node_add_child(w, probe);
  }
  /* the character, twice (a glTF's objects belong to one scene: parsed once for each) */
  size_t len;
  void *glb = cw_asset_load("CesiumMan.glb", &len);
  const char *heroes[2] = { "HeroDark/Model", "HeroLamp/Model" };
  for (int i = 0; glb && i < 2; i++) {
    gltf[i] = t3_gltf_parse(glb, len, NULL, NULL);
    receive_shadows(gltf[i]->scene);
    cw_node3d_set_object(cw_node_get(w, heroes[i]), gltf[i]->scene);
  }
  free(glb);
  e = getenv("FX_LIT_COUNT");
  for (int i = 0, n = e ? atoi(e) : 0; i < n; i++) {
    cw_node d = cw_node_new("Decal3D", "Extra");
    cw_node3d_set_position(d, (t3_vec3){ -3.6f + 0.65f * (float)(i % 12), 0.4f + 0.55f * (float)(i / 12), 0 });
    cw_node_set(d, "size", cw_val_vec3(0.3f, 0.3f, 0.6f));
    cw_node_set(d, "texture", cw_val_string("decals/white.png"));
    cw_node_set(d, "color", cw_val_color(0.55f, 0.4f, 0.25f, 1));
    cw_node_set(d, "blend", cw_val_enum("MIX"));
    cw_node_set(d, "mode", cw_val_enum("MESH"));
    cw_node_set(d, "normal_fade", cw_val_float(0));
    cw_node_add_child(w, d);
  }
  e = getenv("FX_LIT_QUALITY");
  if (e) {
    static const char *const names[] = { "low", "medium", "high", "ultra" };
    for (int i = 0; i < 4; i++)
      if (!strcmp(e, names[i])) cw_quality_set((cw_quality)i, true);
  }
  cw_debug_f32("ld.gpu_ms", &gpu_ms);
  cw_debug_u32("ld.tick", &tick);
  cw_debug_i32("ld.decals", &n_decals);
  cw_debug_i32("ld.tris", &n_tris);
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
  n_decals = st.mesh_decals;
  n_tris = st.mesh_triangles;
  if (tick > 60) {
    float g = cw_pipeline_gpu_ms(cw_pipeline_of(cw_scene_app_tree()));
    if (g >= 0) {
      gpu_sum += g;
      gpu_ms = gpu_sum / (float)++gpu_n;
    }
  }
}

static void shutdown(void *u) {
  (void)u;
  for (int i = 0; i < 2; i++)
    if (gltf[i]) t3_release(gltf[i]);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_lit_decals", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .render = render, .shutdown = shutdown };
  return cw_scene_app("main.cwscene", &base);
}
