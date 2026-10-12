/* gltf_scene: Khronos sample models and a generated ground
 * texture, imported and packed by cwpack (project/roots.txt: meshopt
 * ordering + EXT_meshopt_compression, simplified LODs, KTX2 textures), loaded
 * asynchronously through the resource system and instanced several times:
 *   - five animated CesiumMan instances sharing one skinned mesh, receding
 *     from the camera, each drawn at the LOD whose simplification error
 *     stays under a quarter pixel (each with its own skeleton and animation mixer);
 *   - three spinning BoxTextured instances;
 *   - one WaterBottle whose four textures are KTX2 (UASTC), transcoded at
 *     load to what the context has;
 *   - a ground plane textured by a standalone texture resource (KTX2 ETC1S).
 * Loads start in init and finish in request order over the first frames; at
 * tick LOAD_BARRIER the app waits for the rest (a loading screen's end) and
 * builds the scene, so every host draws the same frames from there on.
 * Debug fields: resource counts and bytes, per-model mesh-data hashes,
 * instances, triangles drawn. Natively, CW_HOTRELOAD=1 watches the project
 * and reloads changed assets in place. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/resource.h"
#include "three.h"

#define LOAD_BARRIER 12
#define MEN 5
#define BOXES 3

typedef struct {
  t3_renderer *renderer;
  t3_scene *scene;
  t3_camera *camera;
  cw_res man, box, bottle, ground;
  t3_object *men[MEN], *boxes[BOXES], *bottle_inst;
  t3_animation_mixer *mixers[MEN];
  t3_mesh *floor;
  bool built;
  int32_t ticks, loaded_cb, res_ready, res_live, res_kb, instances, triangles, ktx2_textures, lod_levels;
  uint32_t man_hash[2], box_hash[2], bottle_hash[2];
  uint32_t versions;
  int32_t men_lod[MEN];
} app_t;

static app_t app;

/* #region on_loaded */
static void on_loaded(cw_res r, cw_res_state st, void *user) {
  app_t *a = user;
  if (st == CW_RES_STATE_READY) a->loaded_cb++;
  else cw_error("gltf_scene: res://%s failed to load", cw_res_path(r));
}
/* #endregion */

static void split(uint64_t h, uint32_t out[2]) {
  out[0] = (uint32_t)h;
  out[1] = (uint32_t)(h >> 32);
}

static int count_ktx2(cw_res scene) {
  const cw_gltf *g = cw_res_gltf(scene);
  int n = 0;
  char p[512];
  for (int i = 0; g && i < g->texture_count; i++) {
    snprintf(p, sizeof p, "%s#texture/%d", cw_res_path(scene), i);
    cw_res t = cw_res_find(p);
    cw_texture_info ti;
    if (cw_res_texture_info(t, &ti) && ti.from_ktx2) n++;
  }
  return n;
}

/* GLTF_SCENE_LINEAR_COLOR=1 (the GL-vs-WebGPU gate's control, on WebGPU,
 * where three.c uploads the texels at the first draw): every colour map is
 * read as data, so its sRGB decode is missing and the picture brightens */
static void linear_color(t3_object *o, void *ctx) {
  (void)ctx;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  for (int i = 0; i < m->material_count; i++) {
    t3_texture *maps[2] = { m->materials[i]->map, m->materials[i]->emissive_map };
    for (int k = 0; k < 2; k++)
      if (maps[k] && maps[k]->color_space == T3_SRGB_COLOR_SPACE) {
        maps[k]->color_space = T3_NO_COLOR_SPACE;
        maps[k]->version++;
      }
  }
}

static void build(app_t *a) {
  cw_res_wait_all();
  const cw_gltf *man = cw_res_gltf(a->man), *box = cw_res_gltf(a->box), *bottle = cw_res_gltf(a->bottle);
  if (!man || !box || !bottle || !cw_res_texture(a->ground)) CW_FATAL("gltf_scene: assets missing (see the log)");
  a->triangles = 0;
  for (int i = 0; i < MEN; i++) {
    t3_object *o = cw_gltf_instantiate(a->man);
    t3_object_set_position(o, -3.6f + 1.8f * i, 0, 2.0f - 3.0f * i); /* receding: auto LOD picks coarser levels */
    t3_object_set_rotation(o, 0, 0.35f * (float)(2 - i), 0);
    t3_object_add((t3_object *)a->scene, o);
    t3_release(o); /* the scene holds it */
    a->men[i] = o;
    if (man->gltf->animation_count) {
      a->mixers[i] = t3_animation_mixer_new(o);
      t3_animation_action *act = t3_animation_mixer_clip_action(a->mixers[i], man->gltf->animations[0]);
      t3_animation_action_play(act);
      t3_animation_mixer_update(a->mixers[i], 0.17f * i); /* out of step with each other */
    }
  }
  for (int i = 0; i < BOXES; i++) {
    t3_object *o = cw_gltf_instantiate(a->box);
    t3_object_set_position(o, -2.0f + 2.0f * i, 2.4f, -2.0f);
    t3_object_set_scale(o, 0.7f, 0.7f, 0.7f);
    t3_object_add((t3_object *)a->scene, o);
    t3_release(o);
    a->triangles += cw_gltf_instance_set_lod(o, 0);
    a->boxes[i] = o;
  }
  a->bottle_inst = cw_gltf_instantiate(a->bottle);
  t3_object_set_position(a->bottle_inst, 3.2f, 0.65f, 1.0f);
  t3_object_set_scale(a->bottle_inst, 5, 5, 5);
  t3_object_add((t3_object *)a->scene, a->bottle_inst);
  t3_release(a->bottle_inst);
  a->triangles += cw_gltf_instance_set_lod(a->bottle_inst, 0);
  t3_texture *gt = cw_res_texture(a->ground);
  gt->repeat.x = gt->repeat.y = 4;
  /* colour data: sRGB already, from the KTX2 (cwpack texture.color) */
  t3_material *gm = t3_mesh_standard_material_new(0xffffff);
  t3_material_set_map(gm, gt);
  gm->roughness = 0.9f;
  a->floor = t3_mesh_new(t3_plane_geometry_new(14, 14, 1, 1), gm);
  t3_release(gm);
  t3_object_set_rotation(a->floor, -1.5707963f, 0, 0);
  t3_object_add((t3_object *)a->scene, a->floor);
  t3_release(a->floor);
  a->triangles += 2;
  const char *lin = getenv("GLTF_SCENE_LINEAR_COLOR");
  if (lin && *lin == '1') t3_object_traverse(a->scene, linear_color, NULL);
  split(man->mesh_hash, a->man_hash);
  split(box->mesh_hash, a->box_hash);
  split(bottle->mesh_hash, a->bottle_hash);
  a->ktx2_textures = count_ktx2(a->man) + count_ktx2(a->box) + count_ktx2(a->bottle);
  cw_texture_info ti;
  if (cw_res_texture_info(a->ground, &ti) && ti.from_ktx2) a->ktx2_textures++;
  a->lod_levels = man->lod_levels;
  a->instances = cw_gltf_instance_count(a->man) + cw_gltf_instance_count(a->box) + cw_gltf_instance_count(a->bottle);
  a->built = true;
  cw_res_log_report(); /* load times and memory: the resource report */
}

static void refresh_stats(app_t *a) {
  cw_res_stats s;
  cw_res_get_stats(&s);
  a->res_ready = s.ready;
  a->res_live = s.live;
  a->res_kb = (int32_t)(s.bytes / 1024);
  a->versions = cw_res_version(a->man) + cw_res_version(a->box) + cw_res_version(a->bottle) + cw_res_version(a->ground);
}

static void init(void *user) {
  app_t *a = user;
  int w, h;
  cw_drawable_size(&w, &h);
  a->renderer = cw_render3d_window_renderer(w, h); /* GL or WebGPU, whichever this run uses */
  t3_renderer_set_clear_color(a->renderer, 0x8fb4d8, 1.0f);
  t3_renderer_set_output_color_space(a->renderer, T3_SRGB_COLOR_SPACE);
  a->scene = t3_scene_new();
  a->camera = t3_perspective_camera_new(50.0f, (float)w / (float)h, 0.1f, 100.0f);
  t3_object_set_position(a->camera, 0, 3.2f, 8.5f);
  t3_object_look_at(a->camera, 0, 1.0f, 0);
  t3_light *sun = t3_directional_light_new(0xffffff, 5.02655f);
  t3_object_set_position(sun, 4, 8, 5);
  t3_object_add((t3_object *)a->scene, sun);
  t3_object_add((t3_object *)a->scene, t3_hemisphere_light_new(0xddeeff, 0x445533, 1.88496f));
  /* #region async_loads */
  /* requested in this order, finished in this order */
  a->man = cw_res_load_async("res://models/CesiumMan.glb", 0, on_loaded, a);
  a->box = cw_res_load_async("res://models/BoxTextured.glb", 0, on_loaded, a);
  a->bottle = cw_res_load_async("res://models/WaterBottle.glb", 0, on_loaded, a);
  a->ground = cw_res_load_async("res://textures/ground.png", 0, on_loaded, a);
  /* #endregion */
  cw_debug_i32("ticks", &a->ticks);
  cw_debug_i32("res.ready", &a->res_ready);
  cw_debug_i32("res.live", &a->res_live);
  cw_debug_i32("res.kb", &a->res_kb);
  cw_debug_i32("res.loaded_cb", &a->loaded_cb);
  cw_debug_u32("res.versions", &a->versions);
  cw_debug_field("hash.man", a->man_hash, CW_DBG_U32, 2);
  cw_debug_field("hash.box", a->box_hash, CW_DBG_U32, 2);
  cw_debug_field("hash.bottle", a->bottle_hash, CW_DBG_U32, 2);
  cw_debug_i32("instances", &a->instances);
  cw_debug_i32("triangles", &a->triangles);
  cw_debug_i32("ktx2_textures", &a->ktx2_textures);
  cw_debug_i32("lod_levels", &a->lod_levels);
  cw_debug_field("men_lod", a->men_lod, CW_DBG_I32, MEN);
  /* #region hotreload */
  if (getenv("CW_HOTRELOAD") && !CW_RES_HOTRELOAD_FROM_BUILD(20)) cw_warn("gltf_scene: hot reload unavailable here");
  /* #endregion */
}

static void fixed_update(void *user, double dt) {
  app_t *a = user;
  a->ticks++;
  if (a->ticks == LOAD_BARRIER) build(a);
  if (!a->built) return;
  for (int i = 0; i < MEN; i++)
    if (a->mixers[i]) t3_animation_mixer_update(a->mixers[i], (float)dt);
  float t = (float)a->ticks / 60.0f;
  for (int i = 0; i < BOXES; i++) t3_object_set_rotation(a->boxes[i], 0.4f * t + i, 0.9f * t + 0.5f * i, 0);
  t3_object_set_rotation(a->bottle_inst, 0, 0.5f * t, 0);
  /* LOD by screen-space error against the logical height (the same on every
   * host whatever the window scale): at most a quarter pixel */
  a->triangles = cw_gltf_auto_lod_all(a->camera, 360.0f, 0.25f) + 2;
  for (int i = 0; i < MEN; i++) a->men_lod[i] = cw_gltf_instance_lod(a->men[i]);
  refresh_stats(a);
}

static void render(void *user, double alpha) {
  app_t *a = user;
  cw_render3d_window_target(a->renderer);
  t3_renderer_render(a->renderer, a->scene, a->camera);
}

static void shutdown(void *user) {
  app_t *a = user;
  for (int i = 0; i < MEN; i++) t3_release(a->mixers[i]);
  t3_release(a->scene); /* the instances go with it, then their resources */
  t3_release(a->camera);
  cw_res_release(a->man);
  cw_res_release(a->box);
  cw_res_release(a->bottle);
  cw_res_release(a->ground);
  t3_renderer_destroy(a->renderer);
}

cw_app_desc cw_main(int argc, char **argv) {
  return (cw_app_desc){
    .title = "cartwheel glTF scene",
    .width = 640,
    .height = 360,
    .init = init,
    .fixed_update = fixed_update,
    .render = render,
    .shutdown = shutdown,
    .debug = true,
    .user = &app,
  };
}
