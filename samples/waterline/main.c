/* waterline: where objects meet the water (cartwheel/waterline.h). A grey fabric "shin" (a cylinder 0.12 m across), a
 * bare skin calf beside it and a painted hull with a 0.25 m draft stand in a gentle swell. Each wears the waterline
 * part: a wet band under a high-water mark that falls back as the water drops, darker by its porosity (the fabric most,
 * the paint hardly) and, for the skin and the paint, glossier with beaded drops; a thin line of sky light at the
 * contact; caustics on what is under the surface, which the water's refraction shows. tests/waterline.mjs reads a
 * strip down the shin at native scale on GL and WebGPU.
 *
 * Switches (cw_test_env, from the second tick): WATERLINE_OFF=1 puts no part on anything (the look before it, the
 * gate's control); WATERLINE_DRY=1 keeps the mark at the line (no band: the band check's control).
 * Debug state: wl.tick, wl.materials (cw_waterline_count), wl.line (the water's world height at the shin), wl.level,
 * wl.mark, wl.band (the shin's, metres), wl.hull_band, wl.api_ok (apply, get, move and remove on a spare material). */
#include "cartwheel/debug.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/shading.h"
#include "cartwheel/water.h"
#include "cartwheel/waterline.h"

static uint32_t tick;
static int32_t materials, api_ok;
static float line_y, level, mark, band, hull_band;
static bool off, dry;
static cw_waterline *shin_wl, *calf_wl, *hull_wl;

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

static void init(void *u) {
  (void)u;
  cw_debug_u32("wl.tick", &tick);
  cw_debug_i32("wl.materials", &materials);
  cw_debug_i32("wl.api_ok", &api_ok);
  cw_debug_f32("wl.line", &line_y);
  cw_debug_f32("wl.level", &level);
  cw_debug_f32("wl.mark", &mark);
  cw_debug_f32("wl.band", &band);
  cw_debug_f32("wl.hull_band", &hull_band);
  { /* on a spare material: apply, read back, move to another object, remove; the count goes up by one and back */
    t3_material *m = t3_mesh_standard_material_new(0xffffff);
    cw_waterline *a = cw_waterline_new(NULL), *b = cw_waterline_new(NULL);
    int before = cw_waterline_count();
    cw_waterline *got = NULL;
    cw_waterline_look lk;
    bool on = cw_waterline_apply(a, m, &(cw_waterline_look){ 0.8f, 0 }) && cw_waterline_count() == before + 1 &&
              cw_waterline_get(m, &got, &lk) && got == a && lk.porosity == 0.8f;
    bool moved = cw_waterline_apply(b, m, NULL) && cw_waterline_count() == before + 1 && cw_waterline_get(m, &got, &lk) && got == b &&
                 lk.porosity == 0.5f;
    cw_waterline_free(b); /* takes the part off m */
    api_ok = on && moved && cw_waterline_count() == before && !cw_waterline_get(m, NULL, NULL) && m->_refs == 1 &&
             !cw_shading_has(m, "waterline");
    cw_waterline_free(a);
    t3_release(m);
  }
}

static void apply(const char *path, cw_waterline *w, cw_waterline_look lk) {
  t3_object *o = cw_node3d_object(node(path));
  if (o) cw_waterline_apply_tree(w, o, &lk);
}

static void fixed(void *u, double dt) {
  (void)u;
  tick++;
  cw_node water = cw_water_find(cw_scene_app_tree());
  if (tick == 2) { /* the gate's switches, from the first frame every host can set them; the meshes exist by now */
    off = cw_test_env("WATERLINE_OFF") != NULL;
    dry = cw_test_env("WATERLINE_DRY") != NULL;
    if (!off) {
      shin_wl = cw_waterline_new(NULL);
      calf_wl = cw_waterline_new(NULL);
      hull_wl = cw_waterline_new(NULL);
      apply("Root/Shin", shin_wl, (cw_waterline_look){ .porosity = 0.6f, .shine = 0 });   /* fabric: darkens, stays matte */
      apply("Root/Calf", calf_wl, (cw_waterline_look){ .porosity = 0.15f, .shine = 0.4f }); /* skin: a sheen, little darker */
      apply("Root/Hull", hull_wl, (cw_waterline_look){ .porosity = 0.0f, .shine = 0.8f });  /* gelcoat: glossy, drops bead */
    }
  }
  t3_vec3 shin = cw_node3d_global_position(node("Root/Shin"));
  line_y = cw_water_height_at(water, shin.x, shin.z, cw_water_time(water));
  if (off || !shin_wl) return;
  if (dry) { /* the mark held at the line: no band */
    cw_waterline_desc d = cw_waterline_default();
    d.fall_rate = 1000, d.wick = 0;
    cw_waterline_set_desc(shin_wl, &d);
  }
  cw_waterline_update(shin_wl, water, shin, 0, (float)dt);
  cw_waterline_update(calf_wl, water, cw_node3d_global_position(node("Root/Calf")), 0, (float)dt);
  cw_waterline_update(hull_wl, water, cw_node3d_global_position(node("Root/Hull")), 1.2f, (float)dt);
  cw_waterline_state s;
  cw_waterline_state_of(shin_wl, &s);
  level = s.level, mark = s.mark, band = s.band;
  cw_waterline_state_of(hull_wl, &s);
  hull_band = s.band;
  materials = cw_waterline_count();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "waterline", .width = 1920, .height = 1080, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
