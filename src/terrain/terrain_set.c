/* Streamed terrain (cartwheel/terrain.h, "terrain sets"): one cw_terrain per
 * cell of a streamed world (cartwheel/stream.h), sharing one template's
 * layer textures, each with the set's scatter layers, physics attached while
 * the cell is active, all drawn through one SHADOW-phase pipeline pass. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/pipeline.h"
#include "cartwheel/render3d.h"
#include "terrain_internal.h"

typedef struct ts_entry {
  int id;
  cw_terrain *t;
  t3_renderer *bound;
  bool active;
} ts_entry;

struct cw_terrain_set {
  cw_terrain_set_desc d;
  cw_tree *tree;
  CW_ARRAY(ts_entry) e; /* ascending id */
  cw_terrain_set_stats st;
};

static int find(const cw_terrain_set *s, int id) {
  int lo = 0, hi = s->e.count;
  while (lo < hi) {
    int m = (lo + hi) / 2;
    if (s->e.data[m].id < id) lo = m + 1;
    else hi = m;
  }
  return lo < s->e.count && s->e.data[lo].id == id ? lo : -1;
}

static void set_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  cw_terrain_set *s = cw_tree_slot(ctx->tree, "terrainset");
  if (!s || !ctx->camera) return;
  t3_object *cam = &ctx->camera->base;
  t3_vec3 cp = { cam->matrix_world.e[12], cam->matrix_world.e[13], cam->matrix_world.e[14] };
  double t0 = tr_now_ms();
  cw_terrain_set_stats st = { 0 };
  for (int i = 0; i < s->e.count; i++) {
    ts_entry *e = &s->e.data[i];
    cw_terrain_add_to_scene(e->t, ctx->scene3d);
    if (ctx->renderer && e->bound != ctx->renderer) {
      cw_terrain_bind_renderer(e->t, ctx->renderer);
      e->bound = ctx->renderer;
    }
    cw_terrain_update(e->t, cp, &ctx->view_proj);
    cw_terrain_stats ts;
    cw_terrain_get_stats(e->t, &ts);
    st.patches += ts.patches + ts.patches_far;
    st.scatter_instances += ts.scatter_instances;
    st.scatter_draws += ts.scatter_draws;
    st.upload_ms += ts.upload_ms;
    st.terrains_drawn += ts.patches + ts.patches_far > 0;
  }
  st.terrains = s->e.count;
  st.update_ms = (float)(tr_now_ms() - t0);
  s->st = st;
}

static void set_slot_free(void *p) { (void)p; /* the set is the caller's: cw_terrain_set_destroy */ }

cw_terrain_set *cw_terrain_set_create(cw_tree *tree, const cw_terrain_set_desc *d) {
  if (!tree || !d || !d->layers) {
    cw_error("cw_terrain_set_create: needs a tree and a template terrain for the layers");
    return NULL;
  }
  if (cw_tree_slot(tree, "terrainset")) {
    cw_error("cw_terrain_set_create: this tree has a terrain set already (one per tree)");
    return NULL;
  }
  cw_terrain_set *s = cw_calloc(1, sizeof *s, CW_MEM_RENDER);
  s->d = *d;
  s->tree = tree;
  /* the set adds these to every cell it makes: it holds its own references */
  for (int i = 0; i < s->d.scatter_count; i++)
    for (int l = 0; l < CW_SCATTER_LODS; l++)
      for (int p = 0; p < CW_SCATTER_PARTS; p++) {
        if (s->d.scatter[i].geometry[l][p]) t3_retain(s->d.scatter[i].geometry[l][p]);
        if (s->d.scatter[i].material[l][p]) t3_retain(s->d.scatter[i].material[l][p]);
      }
  cw_tree_set_slot(tree, "terrainset", s, set_slot_free);
  cw_pipeline *p = cw_pipeline_of(tree);
  if (!cw_pipeline_has_pass(p, "terrainset"))
    cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "terrainset", .phase = CW_PHASE_SHADOW,
                                             .order = 1510, .execute = set_exec });
  return s;
}

void cw_terrain_set_destroy(cw_terrain_set *s) {
  if (!s) return;
  for (int i = 0; i < s->e.count; i++) cw_terrain_destroy(s->e.data[i].t);
  cw_array_free(&s->e);
  for (int i = 0; i < s->d.scatter_count; i++) cw_scatter_def_release(&s->d.scatter[i]);
  if (cw_tree_slot(s->tree, "terrainset") == s) cw_tree_set_slot(s->tree, "terrainset", NULL, NULL);
  cw_free(s);
}

cw_terrain *cw_terrain_set_add(cw_terrain_set *s, int id, t3_vec3 origin, const uint16_t *q, const uint32_t *ctl) {
  if (!s || find(s, id) >= 0) {
    if (s) cw_error("cw_terrain_set_add: cell %d is in the set already", id);
    return NULL;
  }
  cw_terrain_def def = s->d.def;
  def.origin = origin;
  cw_terrain *t = cw_terrain_create(&def);
  if (!t) return NULL;
  int nx = def.size_x + 1, nz = def.size_z + 1;
  if (q) cw_terrain_set_heights(t, 0, 0, nx, nz, q);
  if (ctl) cw_terrain_set_control(t, 0, 0, nx, nz, ctl);
  cw_terrain_share_layers(t, s->d.layers);
  for (int i = 0; i < s->d.scatter_count; i++) {
    cw_scatter_def sd = s->d.scatter[i];
    sd.seed ^= (uint32_t)id * 0x9E3779B1u; /* each cell its own placement */
    cw_terrain_scatter_add(t, &sd);
  }
  /* its height fields, texels and quadtree on a worker: it draws once that is done, and activation
   * only adds bodies */
  cw__terrain_build_start(t);
  int at = 0;
  while (at < s->e.count && s->e.data[at].id < id) at++;
  cw_array_push(&s->e, ((ts_entry){ 0 }));
  memmove(s->e.data + at + 1, s->e.data + at, (size_t)(s->e.count - 1 - at) * sizeof *s->e.data);
  s->e.data[at] = (ts_entry){ id, t, NULL, false };
  t3_scene *scene = cw_render3d_scene(s->tree);
  if (scene) cw_terrain_add_to_scene(t, scene);
  return t;
}

void cw_terrain_set_remove(cw_terrain_set *s, int id) {
  int i = s ? find(s, id) : -1;
  if (i < 0) return;
  cw_terrain_destroy(s->e.data[i].t);
  memmove(s->e.data + i, s->e.data + i + 1, (size_t)(s->e.count - i - 1) * sizeof *s->e.data);
  s->e.count--;
}

void cw_terrain_set_activate(cw_terrain_set *s, int id, bool on) {
  int i = s ? find(s, id) : -1;
  if (i < 0) return;
  ts_entry *e = &s->e.data[i];
  if (e->active == on) return;
  e->active = on;
  if (!cw_world3d_valid(s->d.world)) return;
  if (on) cw_terrain_attach_physics(e->t, s->d.world, &s->d.shape);
  else cw_terrain_detach_physics(e->t);
}

void cw_terrain_set_move(cw_terrain_set *s, int id, t3_vec3 origin) {
  int i = s ? find(s, id) : -1;
  if (i >= 0) cw_terrain_set_origin(s->e.data[i].t, origin);
}

cw_terrain *cw_terrain_set_get(const cw_terrain_set *s, int id) {
  int i = s ? find(s, id) : -1;
  return i >= 0 ? s->e.data[i].t : NULL;
}

cw_terrain *cw_terrain_set_at(const cw_terrain_set *s, float x, float z) {
  if (!s) return NULL;
  for (int i = 0; i < s->e.count; i++) {
    const cw_terrain_def *d = cw_terrain_get_def(s->e.data[i].t);
    float x1 = d->origin.x + (float)d->size_x * d->vertex_spacing, z1 = d->origin.z + (float)d->size_z * d->vertex_spacing;
    if (x >= d->origin.x && x <= x1 && z >= d->origin.z && z <= z1) return s->e.data[i].t;
  }
  return NULL;
}

void cw_terrain_set_get_stats(const cw_terrain_set *s, cw_terrain_set_stats *out) {
  memset(out, 0, sizeof *out);
  if (s) *out = s->st;
}

bool cw_terrain_set_ready(cw_terrain_set *s, int id, bool wait) {
  int i = s ? find(s, id) : -1;
  return i < 0 || cw__terrain_build_finish(s->e.data[i].t, wait);
}
