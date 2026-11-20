/* custom_surface: three surfaces of the game's own that keep the engine's
 * lighting (cartwheel/surface.h; the guide is "Writing your own surface" in
 * docs/guides/rendering-3d.md).
 *
 *   dissolve  the chrome orb burns away along a noise field with a glowing
 *             edge; its shadow has the same holes, and what is left still
 *             mirrors the reflection probe
 *   rock      the slab's stone is laid on from three sides (triplanar) with
 *             patches of moss; the sun's cascades, the lamps, the light the
 *             red wall bounces and the sticker all land on it
 *   sway      forty reeds lean with a gust that travels over them, in the
 *             vertex stage; their shadows lean with them
 *
 * The modules are shaders/dissolve.mjs, rock.mjs and sway.mjs; the program
 * table programs/custom_surface.txt names them. The orb and the slab carry
 * their surface as a node in the scene file (DissolveSurface3D,
 * RockSurface3D: the node's properties are the parameters, and the orb's
 * amount is driven through cw_node_set each tick); the reeds share one
 * material, given its surface from C.
 *
 * custom_surface_field is the same program on another scene (field.cwscene):
 * two hundred reeds and no probes, where meshes with a surface are one
 * instanced draw.
 *
 * Around them: eight clustered lamps, a sun with three cascades, a
 * reflection probe, a light probe grid, a sticker clipped onto the slab and
 * one projected across the orb.
 *
 * Test switches (cw_test_env, read at tick 2):
 *   CS_MODE=stock     no surface at all: plain materials (the reference)
 *   CS_MODE=neutral   every surface on with its effect at zero: the picture
 *                     must be the reference's, on every preset and lighting
 *                     mode (tests/custom-surface.mjs)
 *   CS_LIGHTING=forward|clustered, CS_CASCADES=<n>, CS_SSR=1, CS_BATCH=1 (LightingSettings.batching),
 *   CS_IES=1 (the spot lamp takes an IES profile), CS_QUALITY=<preset>, CS_LATE_DECAL=1 (the slab's sticker placed at tick 8,
 *   after the surface is on)
 *
 * Debug state: cs.tick, cs.mode, cs.surfaces (surfaces in use), cs.surface_materials (materials carrying one),
 * cs.gi_materials, cs.lit_materials, cs.mesh_decals, cs.decal_tris,
 * cs.draws, cs.skipped, cs.amount (the orb's dissolve now), cs.api_ok (the
 * parameter calls answer as documented), cs.tex_updates, cs.gpu_ms. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/decals.h"
#include "cartwheel/gi.h"
#include "cartwheel/lighting.h"
#include "cartwheel/math.h"
#include "cartwheel/noise.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/render3d.h"
#include "cartwheel/resource.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/surface.h"

enum { MODE_FX, MODE_NEUTRAL, MODE_STOCK };
/* custom_surface_field (CS_FIELD): only the reeds, two hundred of them, in a scene with no probes */
#ifdef CS_FIELD
#define REEDS 200
#define REEDS_CAST false
#define REEDS_X 9.0f
#define REEDS_Z 6.0f
#define SCENE "field.cwscene"
#define TITLE "custom_surface_field"
#else
#define REEDS 40
#define REEDS_CAST true
#define REEDS_X 2.6f
#define REEDS_Z 3.0f
#define SCENE "main.cwscene"
#define TITLE "custom_surface"
#endif
static int32_t tick, mode, gi_materials, lit_materials, mesh_decals, decal_tris, draws, skipped;
static float gpu_ms, gpu_sum;
static int gpu_n;
static t3_texture *rock_tex;
static bool late_decal;
static int32_t surfaces, surface_materials, api_ok, tex_updates;
static float amount;

static cw_node node(const char *p) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), p); }

/* a rock albedo made here: grey stone with darker veins (sRGB bytes) */
static t3_texture *make_rock(void) {
  enum { N = 256 };
  uint8_t *px = malloc(N * N * 4);
  cw_noise n = cw_noise_default();
  n.frequency = 1.0f;
  for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++) {
      /* tileable: the noise on a torus */
      float a = (float)x / N * 6.2831853f, b = (float)y / N * 6.2831853f;
      float v = cw_noise_3d(&n, cw_cos(a) * 1.3f, cw_sin(a) * 1.3f + cw_cos(b) * 1.3f, cw_sin(b) * 1.3f);
      float w = cw_noise_3d(&n, cw_cos(a) * 4.1f + 7, cw_sin(a) * 4.1f + cw_cos(b) * 4.1f, cw_sin(b) * 4.1f + 3);
      float vein = 1.0f - CW_CLAMP(1.0f - fabsf(v) * 7.0f, 0, 1) * 0.6f;
      float g = CW_CLAMP((0.62f + 0.2f * w) * vein, 0.05f, 1);
      uint8_t *p = px + 4 * (y * N + x);
      p[0] = (uint8_t)(g * 255), p[1] = (uint8_t)(g * 246), p[2] = (uint8_t)(g * 232), p[3] = 255;
    }
  t3_texture *t = t3_texture_new(N, N, px);
  free(px);
  t->wrap_s = t->wrap_t = T3_REPEAT;
  t->color_space = T3_SRGB_COLOR_SPACE;
  return t;
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("cs.tick", &tick);
  cw_debug_i32("cs.mode", &mode);
  cw_debug_i32("cs.gi_materials", &gi_materials);
  cw_debug_i32("cs.lit_materials", &lit_materials);
  cw_debug_i32("cs.mesh_decals", &mesh_decals);
  cw_debug_i32("cs.decal_tris", &decal_tris);
  cw_debug_i32("cs.draws", &draws);
  cw_debug_i32("cs.skipped", &skipped);
  cw_debug_f32("cs.gpu_ms", &gpu_ms);
  cw_debug_i32("cs.surfaces", &surfaces);
  cw_debug_i32("cs.surface_materials", &surface_materials);
  cw_debug_i32("cs.api_ok", &api_ok);
  cw_debug_i32("cs.tex_updates", &tex_updates);
  cw_debug_f32("cs.amount", &amount);
  /* the stone and the moss patches are the same for every rock material: the surface's own values */
  rock_tex = make_rock();
  cw_surface_set_global_texture("rock", "albedo", rock_tex);
  static const float moss[16] = { -0.9f, 0.5f, -0.95f, 0.75f, 1.1f, 2.3f, -0.95f, 0.5f, 0, 0, 0, 0, 0, 0, 0, 0 };
  cw_surface_set_global("rock", "moss", moss, 16);
  { /* the parameter calls on a material of our own: defaults, a value of its own, the surface's value, an array
     * element, a wrong width refused, removal */
    t3_material *m = t3_mesh_standard_material_new(0xffffff);
    float v[16];
    bool ok = cw_surface_apply(m, "rock") && cw_surface_has(m, "rock") && cw_surface_material_count("rock") == 1;
    ok = ok && cw_surface_get(m, "rock", "repeat", v, 16) == 1 && v[0] == 0.45f; /* the registered default */
    ok = ok && cw_surface_set_float(m, "rock", "repeat", 2) && cw_surface_get(m, "rock", "repeat", v, 16) == 1 && v[0] == 2;
    cw_surface_unset(m, "rock", "repeat");
    ok = ok && cw_surface_get(m, "rock", "repeat", v, 16) == 1 && v[0] == 0.45f;
    ok = ok && cw_surface_get(m, "rock", "moss", v, 16) == 16 && v[3] == 0.75f; /* the surface's value */
    ok = ok && cw_surface_set_at(m, "rock", "moss", 2, (float[4]){ 5, 6, 7, 8 }) && cw_surface_get(m, "rock", "moss", v, 16) == 16 &&
         v[3] == 0.75f && v[8] == 5 && v[11] == 8;
    ok = ok && !cw_surface_set(m, "rock", "repeat", v, 3) && !cw_surface_set_float(m, "rock", "nothing", 1) &&
         !cw_surface_set_float(m, "lava", "amount", 1); /* each refused (and logged once) */
    cw_surface_remove(m, "rock");
    api_ok = ok && !cw_surface_has(m, "rock") && cw_surface_material_count("rock") == 0 && m->_refs == 1;
    t3_release(m);
  }
  /* the reeds: thin cylinders that share one material */
  cw_node reeds = node("World/Reeds");
  uint32_t s = 12345;
  for (int i = 0; i < REEDS; i++) {
    s = s * 1664525u + 1013904223u;
    float x = ((float)(s >> 8 & 0xffff) / 65535.0f - 0.5f) * REEDS_X;
    s = s * 1664525u + 1013904223u;
    float z = ((float)(s >> 8 & 0xffff) / 65535.0f - 0.5f) * REEDS_Z;
    cw_node r = cw_node_new("MeshInstance3D", "Reed");
    cw_node_set(r, "primitive", cw_val_enum("CYLINDER"));
    cw_node_set(r, "size", cw_val_vec3(0.09f, 1.7f, 0.09f));
    cw_node_set(r, "color", cw_val_color(0.3f, 0.55f, 0.22f, 1));
    cw_node_set(r, "roughness", cw_val_float(0.7f));
    /* (the field's reeds cast no shadow: three.c draws the casters of meshes it batched one by one, and the field
     * is there to count the draws of the visible pass) */
    cw_node_set(r, "cast_shadow", cw_val_bool(REEDS_CAST));
    cw_node_set(r, "receive_shadow", cw_val_bool(true));
    cw_node3d_set_position(r, (t3_vec3){ x, 0.85f, z });
    cw_node_add_child(reeds, r);
  }
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  if (tick > 2 && mode == MODE_FX) {
    /* the orb's dissolve breathes, through the node's property (what an AnimationPlayer track would set) */
    amount = 0.42f + 0.06f * cw_sin((float)tick * 0.05f);
    if (cw_node_valid(node("World/Orb/Dissolve"))) cw_node_set(node("World/Orb/Dissolve"), "amount", cw_val_float(amount));
    /* the wind's clock (render-only: counted in ticks) */
    float t = (float)tick / 60.0f;
    cw_surface_set_global("sway", "time", &t, 1);
    if (tick == 20) {
      /* a texture changed after it was made: a darker course of stone across the albedo, uploaded again */
      for (int y = 96; y < 112; y++)
        for (int x = 0; x < 256; x++)
          for (int k = 0; k < 3; k++) rock_tex->pixels[4 * (y * 256 + x) + k] = (uint8_t)(rock_tex->pixels[4 * (y * 256 + x) + k] / 3);
      rock_tex->version++;
      tex_updates++;
    }
  }
  if (tick == 8 && late_decal) {
    cw_node d = cw_node_new("Decal3D", "DecalSlab");
    cw_node3d_set_position(d, (t3_vec3){ -0.6f, 1.9f, -0.95f });
    cw_node_set(d, "size", cw_val_vec3(0.8f, 0.8f, 0.6f));
    cw_node_set(d, "texture", cw_val_string("decals/white.png"));
    cw_node_set(d, "color", cw_val_color(0.9f, 0.45f, 0.1f, 1));
    cw_node_set(d, "blend", cw_val_enum("MIX"));
    cw_node_set(d, "mode", cw_val_enum("MESH"));
    cw_node_set(d, "normal_fade", cw_val_float(0));
    cw_node_set(d, "edge_fade", cw_val_float(0));
    cw_node_add_child(node("World"), d);
  }
  if (tick != 2) return;
  const char *e = cw_test_env("CS_MODE");
  mode = e && !strcmp(e, "stock") ? MODE_STOCK : e && !strcmp(e, "neutral") ? MODE_NEUTRAL : MODE_FX;
  e = cw_test_env("CS_QUALITY");
  if (e) {
    static const char *const names[CW_QUALITY_COUNT] = { "verylow", "low", "medium", "high", "ultra" };
    for (int i = 0; i < CW_QUALITY_COUNT; i++)
      if (!strcmp(e, names[i])) cw_quality_set((cw_quality)i, true);
  }
  e = cw_test_env("CS_LIGHTING");
  if (e) cw_node_set(node("World/Lighting"), "mode", cw_val_enum(!strcmp(e, "forward") ? "FORWARD" : "CLUSTERED"));
  e = cw_test_env("CS_CASCADES");
  if (e) cw_node_set(node("World/Sun"), "shadow_cascades", cw_val_int(atoi(e)));
  e = cw_test_env("CS_SSR");
  if (e && *e == '1') {
    cw_node post = cw_node_new("PostProcess", "Post");
    cw_node_set(post, "ssr", cw_val_bool(true));
    cw_node_add_child(node("World"), post);
  }
  e = cw_test_env("CS_IES");
  if (e && *e == '1') { /* the spot lamp shaped by an IES profile: the clustered lights' part is then its IES form */
    cw_value v = { CW_T_RES };
    v.res = cw_res_path_uid("lights/ring.ies");
    cw_res_index_set(v.res, "lights/ring.ies"); /* (the sample's assets are loose files: no pack, so no index of UIDs) */
    cw_node_set(node("World/LampSpot"), "ies_profile", v);
  }
  e = cw_test_env("CS_BATCH");
  if (e && *e == '1') cw_node_set(node("World/Lighting"), "batching", cw_val_bool(true));
  e = cw_test_env("CS_LATE_DECAL");
  late_decal = e && *e == '1';
  if (late_decal) cw_node_free(node("World/DecalSlab"));
  if (mode == MODE_STOCK) {
    /* the reference: the surface nodes go, and their meshes draw their plain materials again */
    if (cw_node_valid(node("World/Orb/Dissolve"))) cw_node_free(node("World/Orb/Dissolve"));
    if (cw_node_valid(node("World/Slab/Rock"))) cw_node_free(node("World/Slab/Rock"));
    return;
  }
  /* the reeds share one material (MeshInstance3D primitives of one look): one call gives all forty the surface */
  cw_surface_apply_tree(cw_node3d_object(node("World/Reeds")), "sway");
  if (mode == MODE_NEUTRAL) {
    /* every effect at zero: the programs with the surfaces in them draw what plain materials draw */
    if (cw_node_valid(node("World/Orb/Dissolve"))) cw_node_set(node("World/Orb/Dissolve"), "amount", cw_val_float(0));
    if (cw_node_valid(node("World/Slab/Rock"))) cw_node_set(node("World/Slab/Rock"), "amount", cw_val_float(0));
    cw_surface_set_global("sway", "wind", (float[3]){ 0, 0, 0 }, 3);
  }
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  cw_tree *t = cw_scene_app_tree();
  cw_gi_stats g;
  cw_gi_get_stats(t, &g);
  gi_materials = g.materials;
  cw_lighting_stats l;
  cw_lighting_get_stats(t, &l);
  lit_materials = l.materials;
  cw_decal_stats d;
  cw_decals_get_stats(t, &d);
  mesh_decals = d.mesh_decals;
  decal_tris = d.mesh_triangles;
  cw_render3d_stats rs;
  cw_render3d_get_stats(t, &rs);
  draws = rs.draw_calls;
  surfaces = (cw_surface_material_count("dissolve") > 0) + (cw_surface_material_count("rock") > 0) + (cw_surface_material_count("sway") > 0);
  surface_materials = cw_surface_material_count("dissolve") + cw_surface_material_count("rock") + cw_surface_material_count("sway");
  skipped = rs.skipped;
  if (tick > 60) {
    float ms = cw_pipeline_gpu_ms(cw_pipeline_of(t));
    if (ms >= 0) gpu_sum += ms, gpu_ms = gpu_sum / (float)++gpu_n;
  }
}

static void shutdown(void *u) {
  (void)u;
  if (rock_tex) t3_release(rock_tex);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  /* the surfaces: the same names and parameter lists as shaders/<name>.mjs. Registered here, before the scene
   * file is read, since two of them are node types it uses */
  static const cw_surface_param dissolve[] = {
    { "amount", CW_SURFACE_FLOAT },
    { "edge", CW_SURFACE_COLOR, .value = { 6.0f, 1.6f, 0.2f, 0.06f } },
    { "scale", CW_SURFACE_FLOAT, .value = { 7.0f } },
  };
  static const cw_surface_param rock[] = {
    { "amount", CW_SURFACE_FLOAT, .value = { 1 } },
    { "repeat", CW_SURFACE_FLOAT, .value = { 0.45f } },
    { "sharp", CW_SURFACE_FLOAT, .value = { 4 } },
    { "albedo", CW_SURFACE_TEXTURE },
    { "moss", CW_SURFACE_VEC4, .count = 4 },
    { "mossTint", CW_SURFACE_COLOR, .value = { 0.10f, 0.22f, 0.05f, 1 } },
  };
  static const cw_surface_param sway[] = {
    { "time", CW_SURFACE_FLOAT },
    { "wind", CW_SURFACE_VEC3, .value = { 0.22f, 0.35f, 0.4f } },
  };
  cw_surface_register(&(cw_surface_desc){ .name = "dissolve", .params = dissolve, .param_count = 3, .node_type = "DissolveSurface3D" });
  cw_surface_register(&(cw_surface_desc){ .name = "rock", .params = rock, .param_count = 6, .node_type = "RockSurface3D" });
  cw_surface_register(&(cw_surface_desc){ .name = "sway", .params = sway, .param_count = 2, .moves_vertices = true });
  static cw_app_desc base = { .title = TITLE, .width = 960, .height = 540, .debug = true, .init = init,
                              .fixed_update = fixed, .render = render, .shutdown = shutdown };
  return cw_scene_app(SCENE, &base);
}
