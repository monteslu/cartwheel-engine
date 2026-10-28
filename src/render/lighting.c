/* Lighting modes (cartwheel/lighting.h): clustered omni and spot lights.
 *
 * A SHADOW-phase pass ("lights") runs before the scene draws: it gives back
 * the lights it took last frame, walks the three.c scene, takes this frame's
 * clustered lights out of three.c's light list (layers 0: three.c skips the
 * light, its children still draw), writes them into the light texture in
 * view space, nearest first, bins them into the froxel
 * grid and marks every lit material with the "cluster" shading part. The
 * part's fill hook (fill below) hands the textures and the grid to each
 * draw. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/shading.h"
#include "cartwheel/lighting.h"
#include "cartwheel/math.h"
#include "cartwheel/profile.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/quality.h"
#include "cartwheel/render3d.h"
#include "lighting_internal.h"
#include "quality_internal.h"
#if CW_WGPU
#include "pipeline_wgpu.h"
#endif

/* WebGPU: the shadow passes recorded on the frame's encoder, timed as the
 * lights pass (GL times the pass itself) */
static void shadow_span(bool end) {
#if CW_WGPU
  if (cw_gpu_backend() == CW_GPU_WEBGPU) cw__pw_timing_span((void *)cw__pw_encoder(), end);
#else
  (void)end;
#endif
}

#define ROWS 10 /* texels a light (cluster_part.mjs LIGHT_ROWS) */
#define PART "cluster" /* the clustered lights' shading part; the sun's is "csm" (cartwheel/shading.h) */

typedef struct taken { t3_light *light; uint32_t layers; } taken;

typedef struct cl_light {
  t3_light *l;
  bool shadowed;  /* casts shadows: a tile of the atlas */
  float vz;       /* view z (sort key) */
  uint32_t id;    /* tie break */
} cl_light;

/* a mesh of the scene index: its material slots as last marked (a swapped
 * material or a changed extension changes the sum) */
typedef struct ix_mesh {
  t3_mesh *m;
  uint64_t sum;
  int counts; /* its slots that draw with a part */
} ix_mesh;

typedef struct lighting {
  cw_tree *tree;
  cw_lighting_settings s;
  cw_froxel_grid *grid;
  /* the omni and spot lights out of three.c's list this frame (layers 0 until
   * given back), with their own layers */
  CW_ARRAY(taken) taken;
  CW_ARRAY(cl_light) lights;
  /* The scene index: every omni and spot light and every mesh under the
   * scene, from one walk, so a pass reads arrays instead of walking the
   * scene. Built once a frame, not once a view. */
  t3_scene *ix_scene;
  CW_ARRAY(t3_light *) ix_lights;  /* in scene order */
  CW_ARRAY(ix_mesh) ix_meshes;
  int ix_on;                       /* the parts the meshes are marked with (cluster | csm << 1), -1 unmarked */
  /* the frame part ran for this pipeline frame (frames + 1; 0 none) */
  uint64_t frame_done;
  const cw_pipeline *frame_pipe;
  const t3_scene *frame_scene;
  /* this frame's cascaded sun, its own layers, and whether it is out of three.c's list */
  t3_light *sun;
  uint32_t sun_layers;
  bool sun_taken;
  cw__csm_cfg cfg;
  int materials;                   /* lit material slots marked this frame */
  cw_froxel_volume *vols;
  int vols_cap;
  t3_texture *light_tex;
  t3_renderer *hooked;
  int batching; /* the batching last handed to the hooked renderer (-1: none yet) */
  bool marked;  /* materials may carry the parts */
  bool warned_max;
  float grid_u[4], depth_u[4];
  t3_texture *rec_tex, *item_tex;
  /* the cascaded sun: the one its node registered this frame (render3d.c) */
  t3_light *sun_next;
  cw__csm_cfg cfg_next;
  cw__csm *csm;
  cw__atlas *atlas;
  t3_texture *shadow_tex;
  float atlas_px;
  cw__shadow_req *reqs;
  cw__shadow_out *outs;
  int *req_light;
  int reqs_cap;
  cw_lighting_stats stats;
} lighting;

static void lighting_free(void *p) {
  lighting *L = p;
  for (int i = 0; i < L->taken.count; i++) {
    L->taken.data[i].light->base.layers = L->taken.data[i].layers;
    t3_release(L->taken.data[i].light);
  }
  cw_array_free(&L->taken);
  if (L->sun_taken) {
    L->sun->base.layers = L->sun_layers;
    t3_release(L->sun);
  }
  cw_array_free(&L->lights);
  for (int i = 0; i < L->ix_lights.count; i++) t3_release(L->ix_lights.data[i]);
  cw_array_free(&L->ix_lights);
  cw_array_free(&L->ix_meshes);
  cw_free(L->vols);
  if (L->light_tex) t3_release(L->light_tex);
  cw_froxel_grid_free(L->grid);
  cw__csm_free(L->csm);
  cw__atlas_free(L->atlas);
  cw_free(L->reqs);
  cw_free(L->outs);
  cw_free(L->req_light);
  cw_free(L);
}

static void pass_lights(cw_pass_ctx *ctx, void *user);
static void cmd_lighting(int argc, const char **argv, cw_reply *r);

static lighting *lighting_of(cw_tree *t) {
  lighting *L = cw_tree_slot(t, "lighting");
  if (L) return L;
  L = cw_calloc(1, sizeof *L, CW_MEM_RENDER);
  L->tree = t;
  L->s = (cw_lighting_settings){ .mode = CW_LIGHTING_FORWARD, .max_lights = 1024, .tile_px = 32, .z_slices = 24,
                                 .max_per_cluster = 64, .cluster_spot_lights = true, .cluster_far = 0,
                                 .light_cutoff = 0.005f, .shadow_atlas = true,
                                 .shadow_atlas_size = 4096, .shadow_updates_per_frame = 24, .shadow_cache = true,
                                 .batching = false };
  L->grid = cw_froxel_grid_new();
  L->ix_on = -1;
  cw_tree_set_slot(t, "lighting", L, lighting_free);
  cw_pipeline_add_pass(cw_pipeline_of(t), &(cw_pass_desc){ .name = "lights", .phase = CW_PHASE_SHADOW, .order = 0,
                                                           .execute = pass_lights, .user = L, .webgpu = true });
  static bool cmd_done;
  if (!cmd_done) {
    cmd_done = true;
    cw_debug_command("lighting", "lighting: the lighting mode, clustered light counts and the froxel grid this frame",
                     cmd_lighting);
  }
  return L;
}

cw_lighting_settings *cw_lighting_settings_of(cw_tree *t) { return &lighting_of(t)->s; }

void cw_lighting_get_stats(cw_tree *t, cw_lighting_stats *out) {
  lighting *L = cw_tree_slot(t, "lighting");
  if (L) *out = L->stats;
  else memset(out, 0, sizeof *out);
}

cw_froxel_grid *cw_lighting_froxels(cw_tree *t) {
  lighting *L = cw_tree_slot(t, "lighting");
  return L && L->s.mode == CW_LIGHTING_CLUSTERED && L->stats.grid.nx ? L->grid : NULL;
}

static void cmd_lighting(int argc, const char **argv, cw_reply *r) {
  (void)argc;
  (void)argv;
  cw_tree *t = cw_current_tree();
  lighting *L = t ? cw_tree_slot(t, "lighting") : NULL;
  if (!L) {
    cw_reply_printf(r, "{\"mode\":\"FORWARD\",\"clustered\":0}");
    return;
  }
  const cw_lighting_stats *s = &L->stats;
  cw_reply_printf(r,
                  "{\"mode\":\"%s\",\"clustered\":%d,\"clustered_spots\":%d,\"forward\":%d,\"dropped\":%d,"
                  "\"materials\":%d,\"grid\":[%d,%d,%d],\"references\":%d,\"max_in_cluster\":%d,"
                  "\"overflowed\":%d,\"overflow_dropped\":%d,\"bin_ms\":%.3f,\"cascades\":%d,\"cascades_rendered\":%d}",
                  L->s.mode == CW_LIGHTING_CLUSTERED ? "CLUSTERED" : "FORWARD", s->clustered, s->clustered_spots,
                  s->forward, s->dropped, s->materials, s->grid.nx, s->grid.ny, s->grid.nz, s->grid.references,
                  s->grid.max_in_froxel, s->grid.overflowed, s->grid.dropped, s->grid.bin_ms, s->cascades, s->cascades_rendered);
}

/* ── materials ──────────────────────────────────────────────────────── */

static bool lit_type(t3_material_type t) {
  return t == T3_MESH_LAMBERT_MATERIAL || t == T3_MESH_PHONG_MATERIAL || t == T3_MESH_STANDARD_MATERIAL ||
         t == T3_MESH_PHYSICAL_MATERIAL;
}

/* every lit material in the scene carries the parts that are on (cluster,
 * csm) and not the others, composed with the parts it already has
 * (cartwheel/shading.h): terrain's surface parts and the GI parts compose
 * with them; a material with any other extension of its own keeps it alone */
static bool composes(const char *ext) {
  static const char *const ok[] = { "terrain", "terrainlow", "terrainfar", PART, "csm", "gi", "meshdecal", "foliage" };
  if (!ext) return true;
  for (const char *p = ext; *p;) {
    const char *e = strchr(p, '_');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    bool known = false;
    for (size_t i = 0; i < sizeof ok / sizeof ok[0] && !known; i++) known = strlen(ok[i]) == len && !strncmp(ok[i], p, len);
    if (!known && len < 16) { /* a project part (cartwheel/shading.h) registered with the renderer composes too */
      char part[16];
      memcpy(part, p, len);
      part[len] = 0;
      known = cw_shading_part_known(part);
    }
    if (!known) return false;
    if (!e) break;
    p = e + 1;
  }
  return true;
}

/* What marking does to a material is a function of its extension and the
 * parts that are on, so it is remembered: an extension id that marking (with
 * these parts on) leaves as it is, and whether it counts, indexed by the id.
 * A city's thousands of materials then cost an array read each, not a
 * composed name each. (Extension ids are small: three.c numbers them as
 * they are first named.) */
typedef struct mark_memo { uint8_t known, counts; } mark_memo; /* known, counts: bit `on` (cluster | csm << 1) */
static mark_memo *memo;
static int memo_cap;

static int mark_one(t3_material *mt, bool cluster, bool csm) {
  uint8_t on = (uint8_t)(cluster | csm << 1);
  uint16_t from = mt->extension;
  if (from < memo_cap && memo[from].known >> on & 1) return memo[from].counts >> on & 1;
  int counts = 0;
  const char *ext = t3_material_get_extension(mt);
  if (composes(ext)) {
    if (cluster) cw_shading_add(mt, PART);
    else cw_shading_remove(mt, PART);
    if (csm) cw_shading_add(mt, "csm");
    else cw_shading_remove(mt, "csm");
    counts = cluster || csm;
  }
  /* (only a fixed point is remembered: one that marking leaves as it is) */
  if (mt->extension == from) {
    if (from >= memo_cap) {
      int cap = CW_MAX(64, from * 2 + 1);
      memo = cw_realloc(memo, (size_t)cap * sizeof *memo, CW_MEM_RENDER);
      memset(memo + memo_cap, 0, (size_t)(cap - memo_cap) * sizeof *memo);
      memo_cap = cap;
    }
    memo[from].known |= (uint8_t)(1u << on);
    memo[from].counts |= (uint8_t)((unsigned)counts << on);
  }
  return counts;
}

/* ── the fill hook ──────────────────────────────────────────────────── */

static void fill_cluster(t3_extension_ctx *c, void *user) {
  lighting *L = user;
  t3_extension_set_uniform(c, "clusterGrid", L->grid_u, 4);
  t3_extension_set_uniform(c, "clusterDepth", L->depth_u, 4);
  t3_extension_set_texture(c, "clusterLights", L->light_tex);
  t3_extension_set_texture(c, "clusterRecords", L->rec_tex);
  t3_extension_set_texture(c, "clusterItems", L->item_tex);
  t3_extension_set_texture(c, "clusterShadows", L->shadow_tex);
}
static void fill_csm(t3_extension_ctx *c, void *user) {
  lighting *L = user;
  if (L->csm) cw__csm_fill(L->csm, c);
}

void cw__lighting_csm_light(cw_tree *t, t3_light *sun, const cw__csm_cfg *cfg) {
  lighting *L = lighting_of(t);
  if (L->sun_next) return;
  L->sun_next = sun;
  L->cfg_next = *cfg;
}

bool cw__lighting_takes(cw_tree *t, const t3_object *light) {
  lighting *L = t ? cw_tree_slot(t, "lighting") : NULL;
  if (!L) return false;
  for (int i = 0; i < L->taken.count; i++)
    if (&L->taken.data[i].light->base == light) return true;
  if (L->sun_taken && &L->sun->base == light) return true;
  return L->sun_next && &L->sun_next->base == light;
}

/* ── the scene index ────────────────────────────────────────────────── */

/* the taken lights back on their layers (a light whose layers the game set
 * while it was out keeps the game's) */
static void give_back(lighting *L) {
  for (int i = 0; i < L->taken.count; i++) {
    t3_object *o = &L->taken.data[i].light->base;
    if (!o->layers) o->layers = L->taken.data[i].layers;
    t3_release(L->taken.data[i].light);
  }
  cw_array_clear(&L->taken);
  if (L->sun_taken) {
    if (!L->sun->base.layers) L->sun->base.layers = L->sun_layers;
    t3_release(L->sun);
    L->sun_taken = false;
  }
}

static bool mesh_kind(const t3_object *o) {
  return o->type == T3_MESH || o->type == T3_INSTANCED_MESH || o->type == T3_SKINNED_MESH;
}

/* what a mesh's slots are now: their materials and extensions */
static uint64_t slots_sum(const t3_mesh *m) {
  uint64_t h = (uint64_t)m->material_count * 0x9E3779B97F4A7C15ull;
  for (int i = 0; i < m->material_count; i++) {
    const t3_material *mt = m->materials[i];
    h = (h ^ (uint64_t)(uintptr_t)mt) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ (mt ? (uint64_t)mt->extension << 1 | 1 : 0)) * 0x94D049BB133111EBull;
  }
  return h;
}

static int mark_mesh(t3_mesh *m, bool cluster, bool csm) {
  int n = 0;
  for (int i = 0; i < m->material_count; i++) {
    t3_material *mt = m->materials[i];
    if (mt && lit_type(mt->type)) n += mark_one(mt, cluster, csm);
  }
  return n;
}

/* every omni and spot light and every mesh, in scene order (invisible ones
 * too: a view's visibility is read when the view draws) */
static void index_walk(lighting *L, t3_object *o) {
  if (o->type == T3_POINT_LIGHT || o->type == T3_SPOT_LIGHT) {
    t3_retain(o); /* (read again by each view: one a view hook removes stays alive) */
    cw_array_push(&L->ix_lights, (t3_light *)o);
  }
  else if (mesh_kind(o)) cw_array_push(&L->ix_meshes, ((ix_mesh){ (t3_mesh *)o, 0, -1 }));
  for (int i = 0; i < o->child_count; i++) index_walk(L, o->children[i]);
}
static void index_build(lighting *L, t3_scene *sc) {
  CW_PROFILE_SCOPE("lighting.index");
  for (int i = 0; i < L->ix_lights.count; i++) t3_release(L->ix_lights.data[i]);
  cw_array_clear(&L->ix_lights);
  cw_array_clear(&L->ix_meshes);
  index_walk(L, &sc->base);
  L->ix_scene = sc;
}

/* every lit material of the index carries the parts that are on: a mesh is
 * looked at again only when its slots changed (a material swapped, an
 * extension changed) or the parts did. The count of lit slots that draw
 * with a part. */
static int index_mark(lighting *L, bool cluster, bool csm) {
  CW_PROFILE_SCOPE("lighting.materials");
  int on = cluster | csm << 1, n = 0;
  bool all = on != L->ix_on;
  static int once = -1; /* CW_TEST_NO_REMARK=1 (a test control): materials are marked the first frame only */
  if (once < 0) once = getenv("CW_TEST_NO_REMARK") != NULL;
  if (once && L->marked && on == L->ix_on) return L->materials;
  for (int i = 0; i < L->ix_meshes.count; i++) {
    ix_mesh *x = &L->ix_meshes.data[i];
    uint64_t sum = slots_sum(x->m);
    if (all || x->counts < 0 || sum != x->sum) {
      x->counts = mark_mesh(x->m, cluster, csm);
      x->sum = slots_sum(x->m); /* (marking changed the extensions) */
    }
    n += x->counts;
  }
  L->ix_on = on;
  return n;
}

/* the same, walking the scene: for a view whose parts differ from the
 * frame's (a sun on other layers), where the index may be out of date */
static int mark_walk(t3_object *o, bool cluster, bool csm) {
  int n = mesh_kind(o) ? mark_mesh((t3_mesh *)o, cluster, csm) : 0;
  for (int i = 0; i < o->child_count; i++) n += mark_walk(o->children[i], cluster, csm);
  return n;
}

/* the light is under the scene with every ancestor visible */
static bool shown_in(const t3_object *o, const t3_scene *sc) {
  for (; o; o = o->parent) {
    if (!o->visible) return false;
    if (o == &sc->base) return true;
  }
  return false;
}

/* the clustered lights leave three.c's list for the frame: the shadowless
 * omni (and spot) lights, and with shadow_atlas the shadowed ones too; the
 * rest stay three.c's (forward) */
static bool clusters(const lighting *L, const t3_light *l) {
  if (l->base.type == T3_SPOT_LIGHT && !L->s.cluster_spot_lights) return false;
  return !(l->base.cast_shadow && l->shadow) || L->s.shadow_atlas;
}
static void take_lights(lighting *L) {
  for (int i = 0; i < L->ix_lights.count; i++) {
    t3_light *l = L->ix_lights.data[i];
    if (!clusters(L, l)) continue;
    taken t = { l, l->base.layers };
    t3_retain(l);
    cw_array_push(&L->taken, t);
    l->base.layers = 0;
  }
}

static int cmp_light(const void *a, const void *b) {
  const cl_light *x = a, *y = b;
  /* nearest first (descending view z), ties by id: a full cluster keeps its nearest lights
   * (r186 sorts the other way for its z-range culling, which binning here does not need) */
  if (x->vz != y->vz) return x->vz > y->vz ? -1 : 1;
  return x->id < y->id ? -1 : x->id > y->id;
}

static int pow2_at_least(int v) {
  int p = 1;
  while (p < v) p <<= 1;
  return p;
}

static double wall_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

/* What does not depend on the view, once a frame (split screen runs the
 * pass once per view): the lights given back and taken again, the scene
 * index and the materials' parts. False: nothing to do this frame. */
static void frame_part(lighting *L, cw_pass_ctx *ctx) {
  CW_PROFILE_SCOPE("lighting.frame");
  give_back(L);
  L->sun = L->sun_next;
  L->cfg = L->cfg_next;
  L->sun_layers = L->sun ? L->sun->base.layers : 0;
  L->sun_next = NULL; /* (its node registers it again next frame) */
  bool clustered = L->s.mode == CW_LIGHTING_CLUSTERED;
  if (!clustered && !L->sun) {
    if (L->marked) mark_walk(&ctx->scene3d->base, false, false);
    L->marked = false;
    L->ix_on = -1;
    L->ix_scene = NULL;
    return;
  }
  if (ctx->scene3d->auto_update) t3_object_update_matrix_world(ctx->scene3d, false);
  if (L->ix_scene != ctx->scene3d) L->ix_on = -1;
  static int stale = -1; /* CW_TEST_LIGHTS_STALE=1 (a test control): the first index is kept */
  if (stale < 0) stale = getenv("CW_TEST_LIGHTS_STALE") != NULL;
  if (!stale || L->ix_scene != ctx->scene3d) index_build(L, ctx->scene3d);
  /* the parts as most views want them: cluster, and csm with a sun (a view
   * whose sun is on other layers, or whose cascades cannot be made, marks
   * again for itself) */
  L->materials = index_mark(L, clustered, L->sun != NULL);
  L->marked = true;
  if (clustered) take_lights(L);
}

/* The cluster part's textures before this view's are made: the materials are marked
 * before the shadow draws now, so the casters' programs carry the part, and its
 * hook leaving a texture unset skips the draw (the first frame's cascades and
 * tiles were drawn with no casters, then kept as unchanged) */
static void first_inputs(lighting *L, cw_pass_ctx *ctx) {
  if (!L->light_tex) {
    L->light_tex = t3_data_texture_new_typed(16, ROWS, NULL, T3_FLOAT_TYPE);
    memset(L->light_tex->pixels, 0, (size_t)16 * ROWS * 4 * sizeof(float));
  }
  if (!L->rec_tex || !L->item_tex) {
    int rw, iw;
    cw_froxel_grid_textures(L->grid, &L->rec_tex, &rw, &L->item_tex, &iw);
  }
  if (!L->shadow_tex) {
    if (!L->atlas) L->atlas = cw__atlas_new();
    cw__atlas_update(L->atlas, ctx, NULL, 0, L->s.shadow_atlas_size, 0, true, NULL);
    L->shadow_tex = cw__atlas_texture(L->atlas);
    L->atlas_px = (float)(L->shadow_tex ? L->shadow_tex->width : 1);
  }
}

static void pass_lights(cw_pass_ctx *ctx, void *user) {
  CW_PROFILE_SCOPE("lighting.pass");
  lighting *L = user;
  double t0 = wall_ms(), tg = 0, tb = 0, tt = 0;
  if (getenv("CW_LIGHTING_TRACE") && ctx->renderer) { const char *e = t3_renderer_last_error(ctx->renderer); if (e && *e) cw_info("three.c: %s", e); }
  memset(&L->stats, 0, sizeof L->stats);
  if (!ctx->renderer || !ctx->camera || !ctx->scene3d) {
    give_back(L);
    L->sun_next = NULL;
    return;
  }
  if (L->frame_done != ctx->frame + 1 || L->frame_pipe != ctx->pipeline || L->frame_scene != ctx->scene3d) {
    frame_part(L, ctx);
    L->frame_done = ctx->frame + 1;
    L->frame_pipe = ctx->pipeline;
    L->frame_scene = ctx->scene3d;
  }
  bool clustered = L->s.mode == CW_LIGHTING_CLUSTERED;
  t3_light *sun = L->sun;
  if (sun && !(L->sun_layers & ctx->camera->base.layers)) sun = NULL;
  if (!clustered && !L->sun) return;
  if (L->hooked != ctx->renderer) {
    cw_shading_part(ctx->renderer, PART, fill_cluster, L);
    cw_shading_part(ctx->renderer, "csm", fill_csm, L);
    L->hooked = ctx->renderer;
    L->batching = -1;
  }
  if (L->batching != (int)L->s.batching) {
    /* neither hook reads the object beyond receive_shadow, which a batch shares */
    cw_shading_part_batching(ctx->renderer, PART, L->s.batching);
    cw_shading_part_batching(ctx->renderer, "csm", L->s.batching);
    L->batching = L->s.batching;
  }
  /* the cascaded sun: drawn by the csm part, so out of three.c's list (it
   * stays out for the frame's other views; a view without cascades gives it
   * back) */
  bool csm = false;
  /* GL: the engine's own passes ran since three.c last drew (in split screen the
   * previous view's post chain): three.c's idea of what is bound (its vertex
   * array, programs, textures) is stale before the cascades and the atlas draw.
   * The cascades of a second view drew with another pass's vertex array (no
   * index buffer: a crash in the driver). */
  if (cw_gpu_backend() != CW_GPU_WEBGPU) t3_renderer_reset_bindings(ctx->renderer);
  if (clustered) first_inputs(L, ctx);
  if (sun) {
    if (!L->csm) L->csm = cw__csm_new();
    shadow_span(false);
    csm = cw__csm_update(L->csm, ctx, sun, &L->cfg);
    shadow_span(true);
    if (csm) {
      if (!L->sun_taken) {
        t3_retain(sun);
        sun->base.layers = 0;
        L->sun_taken = true;
      }
      cw__csm_info st;
      cw__csm_stats(L->csm, &st);
      L->stats.cascades = st.cascades;
      L->stats.cascades_rendered = st.rendered;
    }
  }
  if (!csm && L->sun_taken) {
    L->sun->base.layers = L->sun_layers;
    t3_release(L->sun);
    L->sun_taken = false;
  }
  int on = clustered | csm << 1;
  if (on != L->ix_on) {
    /* this view's parts differ from the frame's: every material again (the
     * index may be out of date: a walk) */
    L->stats.materials = mark_walk(&ctx->scene3d->base, clustered, csm);
    L->ix_on = -1;
  } else
    L->stats.materials = L->materials;
  if (!clustered) {
    L->stats.cpu_ms = (float)(wall_ms() - t0);
    return;
  }
  /* this view's clustered lights: the taken ones shown here, on its layers;
   * the forward ones only counted */
  cw_array_clear(&L->lights);
  uint32_t cam_layers = ctx->camera->base.layers;
  for (int i = 0; i < L->taken.count; i++) {
    const taken *tk = &L->taken.data[i];
    t3_light *l = tk->light;
    if (!(tk->layers & cam_layers) || !shown_in(&l->base, ctx->scene3d)) continue;
    /* a dark light lights nothing (and is out of three.c's list too) */
    if (!(l->intensity > 0 && (l->color.r > 0 || l->color.g > 0 || l->color.b > 0))) continue;
    cl_light c = { l, l->base.cast_shadow && l->shadow, 0, l->base.id };
    cw_array_push(&L->lights, c);
  }
  for (int i = 0; i < L->ix_lights.count; i++) {
    const t3_light *l = L->ix_lights.data[i];
    if (!clusters(L, l) && (l->base.layers & cam_layers) && shown_in(&l->base, ctx->scene3d)) L->stats.forward++;
  }
  tg = wall_ms();
  int n = L->lights.count;
  if (n > L->s.max_lights) {
    L->stats.dropped = n - L->s.max_lights;
    if (!L->warned_max) {
      L->warned_max = true;
      cw_warn("lighting: %d clustered lights, more than LightingSettings.max_lights (%d): the last %d in scene order "
              "light nothing; raise max_lights",
              n, L->s.max_lights, n - L->s.max_lights);
    }
    n = L->s.max_lights;
  }
  const t3_mat4 *view = &ctx->view;
  for (int i = 0; i < n; i++) {
    cl_light *c = &L->lights.data[i];
    c->vz = t3_vec3_apply_mat4(t3_mat4_get_position(&c->l->base.matrix_world), view).z;
  }
  qsort(L->lights.data, (size_t)n, sizeof *L->lights.data, cmp_light);
  /* the light texture: view-space data, one column a light */
  /* (never narrower than it was: split screen views differ in light counts, and a
   * texture made again each view would cost WebGPU every bind group that samples it) */
  int w = pow2_at_least(CW_MAX(n, 16));
  if (L->light_tex && L->light_tex->width > w) w = L->light_tex->width;
  if (!L->light_tex || L->light_tex->width != w) {
    if (L->light_tex) t3_release(L->light_tex);
    L->light_tex = t3_data_texture_new_typed(w, ROWS, NULL, T3_FLOAT_TYPE);
  }
  float *px = (float *)L->light_tex->pixels;
  memset(px, 0, (size_t)w * ROWS * 4 * sizeof(float));
  if (L->vols_cap < n) {
    L->vols_cap = n + 64;
    L->vols = cw_realloc(L->vols, (size_t)L->vols_cap * sizeof *L->vols, CW_MEM_RENDER);
  }
  int spots = 0;
  for (int i = 0; i < n; i++) {
    const t3_light *l = L->lights.data[i].l;
    t3_vec3 wp = t3_mat4_get_position(&l->base.matrix_world);
    t3_vec3 vp = t3_vec3_apply_mat4(wp, view);
    float inten = l->intensity * CW_MAX(l->color.r, CW_MAX(l->color.g, l->color.b));
    float range = l->distance > 0 ? l->distance : sqrtf(inten / CW_MAX(L->s.light_cutoff, 1e-6f));
    float *t0 = px + 4 * (size_t)i, *t1 = t0 + 4 * (size_t)w, *t2 = t1 + 4 * (size_t)w, *t3 = t2 + 4 * (size_t)w;
    t0[0] = vp.x, t0[1] = vp.y, t0[2] = vp.z, t0[3] = range;
    t1[0] = l->color.r * l->intensity, t1[1] = l->color.g * l->intensity, t1[2] = l->color.b * l->intensity;
    t1[3] = l->decay;
    cw_froxel_volume *v = &L->vols[i];
    v->x = vp.x, v->y = vp.y, v->z = vp.z, v->radius = range;
    v->cos_angle = -2;
    t2[3] = -2;
    if (l->base.type == T3_SPOT_LIGHT && l->target) {
      t3_vec3 tp = t3_mat4_get_position(&l->target->matrix_world);
      /* r186 lightTargetDirection: from the target to the light, in view space */
      t3_vec3 d = t3_vec3_normalize(t3_vec3_transform_direction(t3_vec3_sub(wp, tp), view));
      float cc = cw_cos(l->angle), pc = cw_cos(l->angle * (1 - l->penumbra));
      t2[0] = d.x, t2[1] = d.y, t2[2] = d.z, t2[3] = cc;
      t3[0] = pc;
      v->dir[0] = -d.x, v->dir[1] = -d.y, v->dir[2] = -d.z;
      v->cos_angle = cc;
      v->sin_angle = cw_sin(l->angle);
      spots++;
    }
  }
  /* the shadowed ones: a tile each in the atlas, sized by how big the light
   * is on screen (at most its shadow_size, a power of two) */
  if (L->reqs_cap < n) {
    L->reqs_cap = n + 16;
    L->reqs = cw_realloc(L->reqs, (size_t)L->reqs_cap * sizeof *L->reqs, CW_MEM_RENDER);
    L->outs = cw_realloc(L->outs, (size_t)L->reqs_cap * sizeof *L->outs, CW_MEM_RENDER);
    L->req_light = cw_realloc(L->req_light, (size_t)L->reqs_cap * sizeof *L->req_light, CW_MEM_RENDER);
  }
  int nreq = 0;
  float focal = ctx->proj.e[5] * (float)ctx->height * 0.5f;
  for (int i = 0; i < n; i++) {
    if (!L->lights.data[i].shadowed) continue;
    const t3_light *l = L->lights.data[i].l;
    const float *t0 = px + 4 * (size_t)i;
    float range = t0[3], dist = sqrtf(t0[0] * t0[0] + t0[1] * t0[1] + t0[2] * t0[2]);
    /* the light's sphere on screen: its radius in pixels (inside it: as big as the screen) */
    float rpx = dist > range * 1.001f ? range / sqrtf(dist * dist - range * range) * focal : (float)ctx->height;
    int base = 64;
    while (base * 2 <= CW_MAX(l->shadow->map_width, 64)) base *= 2;
    int size = 64;
    while (size < base && (float)size < 2 * rpx) size *= 2;
    L->reqs[nreq] = (cw__shadow_req){ l, l->base.type == T3_POINT_LIGHT, size, rpx, range };
    L->req_light[nreq++] = i;
  }
  if (!L->atlas) L->atlas = cw__atlas_new();
  int asize = L->s.shadow_atlas_size;
  if (cw__quality_shadow_level() <= 1) asize = CW_MIN(asize, 2048); /* Shadows Low / Medium */
  shadow_span(false);
  cw__atlas_update(L->atlas, ctx, L->reqs, nreq, asize, L->s.shadow_updates_per_frame, L->s.shadow_cache, L->outs);
  shadow_span(true);
  bool soft = cw__quality_soft_shadows();
  for (int k = 0; k < nreq; k++) {
    const cw__shadow_out *o = &L->outs[k];
    if (!o->type) continue;
    int i = L->req_light[k];
    const t3_light_shadow *sh = L->reqs[k].light->shadow;
    float *row[10];
    for (int r = 0; r < 10; r++) row[r] = px + 4 * ((size_t)i + (size_t)r * w);
    row[3][1] = (float)o->type, row[3][2] = sh->bias, row[3][3] = sh->normal_bias;
    if (o->type == 1) memcpy(row[4], o->mat, 4 * sizeof(float)), memcpy(row[5], o->mat + 4, 4 * sizeof(float)),
                      memcpy(row[6], o->mat + 8, 4 * sizeof(float)), memcpy(row[7], o->mat + 12, 4 * sizeof(float));
    else {
      row[4][0] = o->pos.x, row[4][1] = o->pos.y, row[4][2] = o->pos.z, row[4][3] = o->near;
      row[5][0] = o->far;
    }
    row[8][0] = o->rect[0], row[8][1] = o->rect[1], row[8][2] = o->rect[2], row[8][3] = soft ? sh->radius : 0;
    row[9][0] = sh->intensity;
    L->stats.shadowed++;
  }
  {
    cw__atlas_info ai;
    cw__atlas_get_info(L->atlas, &ai);
    L->stats.shadow_faces_rendered = ai.faces_rendered;
    L->stats.shadow_faces_cached = ai.faces_cached;
  }
  L->shadow_tex = cw__atlas_texture(L->atlas);
  L->atlas_px = (float)(L->shadow_tex ? L->shadow_tex->width : 1);
  L->light_tex->version++;
  /* the grid over this view (the unjittered projection) */
  /* effects detail keeps fewer lights a cluster on Low and Medium (cartwheel/quality.h) */
  int mpc = (int)((float)L->s.max_per_cluster * cw_quality_scale("lights") + 0.5f);
  cw_froxel_config fc = { L->s.tile_px, L->s.z_slices, CW_CLAMP(mpc, 1, 255) };
  t3_camera *cam = ctx->camera;
  float cn = cam->near, cf = L->s.cluster_far > 0 ? L->s.cluster_far : cam->far;
  cw_froxel_grid_setup(L->grid, &fc, ctx->width, ctx->height, ctx->proj.e, cn, cf);
  tb = wall_ms();
  cw_froxel_grid_bin(L->grid, L->vols, n);
  tt = wall_ms();
  int nx, ny, nz, rw, iw;
  float zs, zb;
  cw_froxel_grid_dims(L->grid, &nx, &ny, &nz, &zs, &zb);
  cw_froxel_grid_textures(L->grid, &L->rec_tex, &rw, &L->item_tex, &iw);
  L->grid_u[0] = (float)nx, L->grid_u[1] = (float)ny, L->grid_u[2] = (float)nz, L->grid_u[3] = (float)rw;
  L->depth_u[0] = zs, L->depth_u[1] = zb, L->depth_u[2] = (float)iw, L->depth_u[3] = L->atlas_px;
  L->stats.clustered = n;
  L->stats.clustered_spots = spots;
  cw_froxel_grid_stats(L->grid, &L->stats.grid);
  L->stats.cpu_ms = (float)(wall_ms() - t0);
  if (getenv("CW_LIGHTING_TRACE"))
    cw_info("lighting: gather %.3f, lights %.3f, bin %.3f, textures %.3f ms (%d lights, %d meshes indexed)", tg - t0, tb - tg,
            tt - tb, wall_ms() - tt, L->ix_lights.count, L->ix_meshes.count);
}

/* ── LightingSettings ───────────────────────────────────────────────── */

static const char *const mode_names[] = { "FORWARD", "CLUSTERED", NULL };

static void ls_init(cw_node n, void *data) {
  (void)n;
  cw_lighting_settings *s = data;
  *s = (cw_lighting_settings){ .mode = CW_LIGHTING_CLUSTERED, .max_lights = 1024, .tile_px = 32, .z_slices = 24,
                               .max_per_cluster = 64, .cluster_spot_lights = true, .cluster_far = 0,
                               .light_cutoff = 0.005f, .shadow_atlas = true,
                                 .shadow_atlas_size = 4096, .shadow_updates_per_frame = 24, .shadow_cache = true,
                                 .batching = false };
}

static void ls_pre_render(cw_node n, void *data, double alpha) {
  (void)alpha;
  cw_tree *t = cw_node_tree(n);
  if (t) *cw_lighting_settings_of(t) = *(cw_lighting_settings *)data;
}

static void ls_exit(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  lighting *L = t ? cw_tree_slot(t, "lighting") : NULL;
  if (L) L->s.mode = CW_LIGHTING_FORWARD;
}

static const cw_prop ls_props[] = {
  { .name = "mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(cw_lighting_settings, mode),
    .size = sizeof(int), .enum_names = mode_names,
    .doc = "FORWARD: every light is three.c's (8 of a kind at most). CLUSTERED: shadowless omni and spot lights "
           "are binned into clusters, hundreds at a time (r186 ClusteredLighting)." },
  CW_PROP_FIELD_RANGE(cw_lighting_settings, max_lights, CW_T_INT, 1, 65535, "Clustered lights a frame at most."),
  CW_PROP_FIELD_RANGE(cw_lighting_settings, tile_px, CW_T_INT, 4, 1024, "Cluster tile size in pixels."),
  CW_PROP_FIELD_RANGE(cw_lighting_settings, z_slices, CW_T_INT, 1, 256, "Exponential depth slices."),
  CW_PROP_FIELD_RANGE(cw_lighting_settings, max_per_cluster, CW_T_INT, 1, 255, "Lights one cluster keeps."),
  CW_PROP_FIELD(cw_lighting_settings, cluster_spot_lights, CW_T_BOOL, CW_PROP_DEFAULT,
                "Shadowless spot lights are clustered too (beyond r186, which clusters point lights only)."),
  CW_PROP_FIELD_RANGE(cw_lighting_settings, cluster_far, CW_T_FLOAT, 0, 1e6, "Where the last slice ends; 0 = the camera's far."),
  CW_PROP_FIELD_RANGE(cw_lighting_settings, light_cutoff, CW_T_FLOAT, 1e-6, 10,
                      "A clustered light with range 0 ends where intensity / distance^2 falls under this."),
  CW_PROP_FIELD(cw_lighting_settings, shadow_atlas, CW_T_BOOL, CW_PROP_DEFAULT,
                "Shadowed omni and spot lights are clustered too, their shadows tiles of one atlas (else three.c draws them)."),
  CW_PROP_FIELD_RANGE(cw_lighting_settings, shadow_atlas_size, CW_T_INT, 512, 16384, "The shadow atlas, texels a side."),
  CW_PROP_FIELD_RANGE(cw_lighting_settings, shadow_updates_per_frame, CW_T_INT, 0, 10000,
                      "Atlas faces redrawn a frame at most (an omni light is 6; 0 = no limit)."),
  CW_PROP_FIELD(cw_lighting_settings, shadow_cache, CW_T_BOOL, CW_PROP_DEFAULT,
                "A tile is redrawn only when its light or a shadow caster in its range changed."),
  CW_PROP_FIELD(cw_lighting_settings, batching, CW_T_BOOL, CW_PROP_DEFAULT,
                "Meshes sharing geometry and material are one instanced draw under clustered lights and cascades "
                "(off, the default: one draw a mesh, front to back, as three.c draws any extension material)."),
  CW_PROP_END,
};

static cw_node_type ls_type = {
  .name = "LightingSettings", .parent = "Node", .data_size = sizeof(cw_lighting_settings), .props = ls_props,
  .doc = "The tree's lighting mode and clustered-lighting settings (cartwheel/lighting.h); CLUSTERED by default.",
  .init = ls_init, .pre_render = ls_pre_render, .exit_tree = ls_exit,
};

void cw__register_lighting_types(void) { cw_node_type_register(&ls_type); }

int cw__lighting_cluster_kinds(cw_tree *t) {
  lighting *L = t ? cw_tree_slot(t, "lighting") : NULL;
  if (!L || L->s.mode != CW_LIGHTING_CLUSTERED) return 0;
  return 1 | (L->s.cluster_spot_lights ? 2 : 0) | (L->s.shadow_atlas ? 4 : 0);
}
