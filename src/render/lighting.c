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
#include "cartwheel/lighting.h"
#include "cartwheel/math.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/quality.h"
#include "cartwheel/render3d.h"

#define ROWS 4 /* texels a light (cluster_part.mjs LIGHT_ROWS) */
#define PART "cluster"

typedef struct taken { t3_light *light; uint32_t layers; } taken;

typedef struct cl_light {
  t3_light *l;
  float vz;       /* view z (sort key) */
  uint32_t id;    /* tie break */
} cl_light;

typedef struct lighting {
  cw_tree *tree;
  cw_lighting_settings s;
  cw_froxel_grid *grid;
  CW_ARRAY(taken) taken;
  CW_ARRAY(cl_light) lights;
  cw_froxel_volume *vols;
  int vols_cap;
  t3_texture *light_tex;
  t3_renderer *hooked;
  bool marked;            /* materials carry the part */
  bool warned_max;
  float grid_u[4], depth_u[4];
  t3_texture *rec_tex, *item_tex;
  cw_lighting_stats stats;
} lighting;

static void lighting_free(void *p) {
  lighting *L = p;
  for (int i = 0; i < L->taken.count; i++) {
    L->taken.data[i].light->base.layers = L->taken.data[i].layers;
    t3_release(L->taken.data[i].light);
  }
  cw_array_free(&L->taken);
  cw_array_free(&L->lights);
  cw_free(L->vols);
  if (L->light_tex) t3_release(L->light_tex);
  cw_froxel_grid_free(L->grid);
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
                                 .light_cutoff = 0.005f };
  L->grid = cw_froxel_grid_new();
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
                  "\"overflowed\":%d,\"overflow_dropped\":%d,\"bin_ms\":%.3f}",
                  L->s.mode == CW_LIGHTING_CLUSTERED ? "CLUSTERED" : "FORWARD", s->clustered, s->clustered_spots,
                  s->forward, s->dropped, s->materials, s->grid.nx, s->grid.ny, s->grid.nz, s->grid.references,
                  s->grid.max_in_froxel, s->grid.overflowed, s->grid.dropped, s->grid.bin_ms);
}

/* ── materials ──────────────────────────────────────────────────────── */

static bool lit_type(t3_material_type t) {
  return t == T3_MESH_LAMBERT_MATERIAL || t == T3_MESH_PHONG_MATERIAL || t == T3_MESH_STANDARD_MATERIAL ||
         t == T3_MESH_PHYSICAL_MATERIAL;
}

/* every lit material in the scene gets the part (on) or loses it (off);
 * materials with another extension keep theirs */
static int mark_materials(t3_object *o, bool on) {
  int n = 0;
  if (o->type == T3_MESH || o->type == T3_INSTANCED_MESH || o->type == T3_SKINNED_MESH) {
    t3_mesh *m = (t3_mesh *)o;
    for (int i = 0; i < m->material_count; i++) {
      t3_material *mt = m->materials[i];
      if (!mt || !lit_type(mt->type)) continue;
      const char *ext = t3_material_get_extension(mt);
      if (on) {
        if (!ext) t3_material_set_extension(mt, PART);
        if (!ext || !strcmp(ext, PART)) n++;
      } else if (ext && !strcmp(ext, PART))
        t3_material_set_extension(mt, NULL);
    }
  }
  for (int i = 0; i < o->child_count; i++) n += mark_materials(o->children[i], on);
  return n;
}

/* ── the fill hook ──────────────────────────────────────────────────── */

static void fill(t3_extension_ctx *c, void *user) {
  lighting *L = user;
  t3_extension_set_uniform(c, "clusterGrid", L->grid_u, 4);
  t3_extension_set_uniform(c, "clusterDepth", L->depth_u, 4);
  t3_extension_set_texture(c, "clusterLights", L->light_tex);
  t3_extension_set_texture(c, "clusterRecords", L->rec_tex);
  t3_extension_set_texture(c, "clusterItems", L->item_tex);
}

/* ── gathering ──────────────────────────────────────────────────────── */

static void give_back(lighting *L) {
  for (int i = 0; i < L->taken.count; i++) {
    L->taken.data[i].light->base.layers = L->taken.data[i].layers;
    t3_release(L->taken.data[i].light);
  }
  cw_array_clear(&L->taken);
}

/* the shadowless omni (and spot) lights three.c would draw for camera layers `layers` */
static void gather(lighting *L, t3_object *o, uint32_t layers) {
  if (!o->visible) return;
  if ((o->type == T3_POINT_LIGHT || (o->type == T3_SPOT_LIGHT && L->s.cluster_spot_lights)) && (o->layers & layers)) {
    t3_light *l = (t3_light *)o;
    if (!(o->cast_shadow && l->shadow)) {
      if (l->intensity > 0 && (l->color.r > 0 || l->color.g > 0 || l->color.b > 0)) {
        cl_light c = { l, 0, o->id };
        cw_array_push(&L->lights, c);
      }
      /* a dark light leaves three.c's list too: it would light nothing there either */
      taken t = { l, o->layers };
      t3_retain(l);
      cw_array_push(&L->taken, t);
    } else
      L->stats.forward++;
  } else if ((o->type == T3_POINT_LIGHT || o->type == T3_SPOT_LIGHT) && (o->layers & layers))
    L->stats.forward++;
  for (int i = 0; i < o->child_count; i++) gather(L, o->children[i], layers);
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

static void pass_lights(cw_pass_ctx *ctx, void *user) {
  lighting *L = user;
  double t0 = wall_ms(), tg = 0, tb = 0, tt = 0;
  give_back(L);
  memset(&L->stats, 0, sizeof L->stats);
  if (!ctx->renderer || !ctx->camera || !ctx->scene3d) return;
  if (L->s.mode != CW_LIGHTING_CLUSTERED) {
    if (L->marked) {
      mark_materials(&ctx->scene3d->base, false);
      L->marked = false;
    }
    return;
  }
  if (L->hooked != ctx->renderer) {
    t3_renderer_set_extension(ctx->renderer, PART, fill, L);
    L->hooked = ctx->renderer;
  }
  if (ctx->scene3d->auto_update) t3_object_update_matrix_world(ctx->scene3d, false);
  cw_array_clear(&L->lights);
  gather(L, &ctx->scene3d->base, ctx->camera->base.layers);
  for (int i = 0; i < L->taken.count; i++) L->taken.data[i].light->base.layers = 0;
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
  int w = pow2_at_least(CW_MAX(n, 16));
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
  L->depth_u[0] = zs, L->depth_u[1] = zb, L->depth_u[2] = (float)iw, L->depth_u[3] = 0;
  L->stats.clustered = n;
  L->stats.clustered_spots = spots;
  cw_froxel_grid_stats(L->grid, &L->stats.grid);
  double tm = wall_ms();
  L->stats.materials = mark_materials(&ctx->scene3d->base, true);
  L->marked = true;
  L->stats.cpu_ms = (float)(wall_ms() - t0);
  if (getenv("CW_LIGHTING_TRACE"))
    cw_info("lighting: gather %.3f, lights %.3f, bin %.3f, textures %.3f, materials %.3f ms", tg - t0, tb - tg, tt - tb,
            tm - tt, wall_ms() - tm);
}

/* ── LightingSettings ───────────────────────────────────────────────── */

static const char *const mode_names[] = { "FORWARD", "CLUSTERED", NULL };

static void ls_init(cw_node n, void *data) {
  (void)n;
  cw_lighting_settings *s = data;
  *s = (cw_lighting_settings){ .mode = CW_LIGHTING_CLUSTERED, .max_lights = 1024, .tile_px = 32, .z_slices = 24,
                               .max_per_cluster = 64, .cluster_spot_lights = true, .cluster_far = 0,
                               .light_cutoff = 0.005f };
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
  CW_PROP_END,
};

static cw_node_type ls_type = {
  .name = "LightingSettings", .parent = "Node", .data_size = sizeof(cw_lighting_settings), .props = ls_props,
  .doc = "The tree's lighting mode and clustered-lighting settings (cartwheel/lighting.h); CLUSTERED by default.",
  .init = ls_init, .pre_render = ls_pre_render, .exit_tree = ls_exit,
};

void cw__register_lighting_types(void) { cw_node_type_register(&ls_type); }

/* render3d.c's light cap check: the kinds the tree clusters (1 omni, 2 spot) */
int cw__lighting_cluster_kinds(cw_tree *t) {
  lighting *L = t ? cw_tree_slot(t, "lighting") : NULL;
  if (!L || L->s.mode != CW_LIGHTING_CLUSTERED) return 0;
  return 1 | (L->s.cluster_spot_lights ? 2 : 0);
}
