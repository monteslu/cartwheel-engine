/* Static batches (cartwheel/static_batch.h): the tree's three.c BatchedMeshes,
 * one a material and shadow setting, and the record of each MeshInstance3D
 * that draws through them (src/scene/render3d.c calls in from its
 * pre-render). */
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/static_batch.h"
#include "cartwheel/render3d.h"
#include "static_batch_internal.h"

typedef struct sb_batch {
  t3_batched_mesh *bm; /* in the scene; retained */
  t3_material *mat;
  bool cast, receive;
  uint32_t layout; /* which attributes its geometries carry */
  cw_map geos;     /* geometry pointer -> its id in the batch + 1 */
  int meshes;
} sb_batch;

typedef struct sb_item {
  t3_mesh *src; /* retained: hidden while batched */
  t3_geometry *geo;
  t3_material *mat;
  sb_batch *batch;
  int inst;
  bool shown;
  t3_mat4 world;
} sb_item;

struct cw__sbatch_rec {
  struct cw__sbatch *owner;
  sb_item *items;
  int n;
  bool cast, receive;
  const t3_object *root; /* the node's drawn object the items were collected under */
  uint32_t graph;        /* three.c's graph version then (it moves when an object is added or removed anywhere) */
};

typedef struct cw__sbatch {
  t3_scene *scene;
  CW_ARRAY(sb_batch *) batches;
  int meshes, geometries;
  int recs;  /* node records that point here */
  bool dead; /* the tree is gone: freed with the last record */
} cw__sbatch;

static void sbatch_destroy(cw__sbatch *S) {
  for (int i = 0; i < S->batches.count; i++) {
    sb_batch *b = S->batches.data[i];
    t3_release(b->bm);
    t3_release(b->mat);
    cw_map_free(&b->geos);
    cw_free(b);
  }
  cw_array_free(&S->batches);
  cw_free(S);
}
/* the tree's slot goes: the batches leave the scene now; the storage waits for the nodes' records */
static void sbatch_free(void *p) {
  cw__sbatch *S = p;
  for (int i = 0; i < S->batches.count; i++) {
    t3_batched_mesh *bm = S->batches.data[i]->bm;
    if (bm->mesh.base.parent) t3_object_remove(bm->mesh.base.parent, bm);
  }
  S->dead = true;
  if (!S->recs) sbatch_destroy(S);
}

static cw__sbatch *sbatch_of(cw_tree *t, bool make) {
  cw__sbatch *S = cw_tree_slot(t, "static_batch");
  if (S || !make) return S;
  S = cw_calloc(1, sizeof *S, CW_MEM_RENDER);
  S->scene = cw_render3d_scene(t);
  cw_tree_set_slot(t, "static_batch", S, sbatch_free);
  return S;
}

static uint32_t layout_of(const t3_geometry *g) {
  static const t3_attr_slot slots[] = { T3_ATTR_NORMAL, T3_ATTR_UV, T3_ATTR_COLOR, T3_ATTR_TANGENT, T3_ATTR_UV1 };
  uint32_t l = 0;
  for (int i = 0; i < 5; i++)
    if (g->attributes[slots[i]]) l |= (uint32_t)g->attributes[slots[i]]->item_size << (i * 4);
  return l;
}

/* a mesh a batch can hold: one material, plain float attributes, no morph targets */
static bool batchable(const t3_object *o) {
  if (o->type != T3_MESH) return false;
  const t3_mesh *m = (const t3_mesh *)o;
  const t3_geometry *g = m->geometry;
  if (m->material_count != 1 || !m->materials[0] || !g || !g->attributes[T3_ATTR_POSITION] || g->morph_count) return false;
  if (g->draw_start != 0 || g->draw_count >= 0) return false; /* (groups do not matter to one material) */
  static const t3_attr_slot slots[] = { T3_ATTR_POSITION, T3_ATTR_NORMAL, T3_ATTR_UV, T3_ATTR_COLOR, T3_ATTR_TANGENT, T3_ATTR_UV1 };
  for (int i = 0; i < 6; i++)
    if (g->attributes[slots[i]] && g->attributes[slots[i]]->type != T3_FLOAT32) return false;
  return !m->materials[0]->transparent; /* (blended props are sorted one by one: nothing to gain) */
}

typedef struct { t3_mesh *m[64]; int n; bool over; } collect;
static void collect_visit(t3_object *o, void *u) {
  collect *c = u;
  if (!batchable(o)) return;
  if (c->n < 64) c->m[c->n++] = (t3_mesh *)o;
  else c->over = true;
}

static sb_batch *batch_for(cw__sbatch *S, t3_material *mat, const t3_geometry *g, bool cast, bool receive) {
  uint32_t layout = layout_of(g);
  for (int i = 0; i < S->batches.count; i++) {
    sb_batch *b = S->batches.data[i];
    if (b->mat == mat && b->cast == cast && b->receive == receive && b->layout == layout) return b;
  }
  sb_batch *b = cw_calloc(1, sizeof *b, CW_MEM_RENDER);
  b->mat = t3_retain(mat);
  b->cast = cast, b->receive = receive, b->layout = layout;
  b->bm = t3_batched_mesh_new(256, 0, 0, mat);
  b->bm->mesh.base.cast_shadow = cast;
  b->bm->mesh.base.receive_shadow = receive;
  /* runs join over a few props that are out of view between them: those are drawn and clipped, a draw call less */
  b->bm->bridge_indices = 6000;
  cw_map_init(&b->geos, CW_MEM_RENDER);
  t3_object_add(&S->scene->base, b->bm);
  cw_array_push(&S->batches, b);
  return b;
}

/* shown: the mesh's ancestors are visible (the mesh itself is hidden while batched) */
static bool shown(const t3_object *o) {
  for (o = o->parent; o; o = o->parent)
    if (!o->visible) return false;
  return true;
}

static void rec_clear(cw__sbatch_rec *R) {
  cw__sbatch *S = R->owner;
  for (int i = 0; i < R->n; i++) {
    sb_item *it = &R->items[i];
    t3_batched_mesh_delete_instance(it->batch->bm, it->inst);
    it->batch->meshes--;
    S->meshes--;
    it->src->base.visible = true;
    t3_release(it->src);
  }
  cw_free(R->items);
  R->items = NULL;
  R->n = 0;
}

void cw__sbatch_remove(cw__sbatch_rec **slot) {
  cw__sbatch_rec *R = *slot;
  if (!R) return;
  cw__sbatch *S = R->owner;
  rec_clear(R);
  cw_free(R);
  *slot = NULL;
  if (--S->recs == 0 && S->dead) sbatch_destroy(S);
}

void cw__sbatch_sync(cw_tree *t, cw__sbatch_rec **slot, t3_object *root, bool cast, bool receive) {
  if (!t || !root) { cw__sbatch_remove(slot); return; }
  cw__sbatch *S = sbatch_of(t, true);
  cw__sbatch_rec *R = *slot;
  if (R && R->owner != S) { cw__sbatch_remove(slot); R = NULL; } /* the node moved to another tree */
  collect c = { { 0 }, 0, false };
  /* nothing under the node can have changed unless the scene graph did, a mesh swapped its geometry or material, or
   * the shadow flags moved: then the meshes are collected again (5,000 props cost a few compares each a frame) */
  bool same = R && R->owner == S && R->root == root && R->cast == cast && R->receive == receive;
  for (int i = 0; same && i < R->n; i++)
    same = R->items[i].geo == R->items[i].src->geometry && R->items[i].src->material_count == 1 && R->items[i].mat == R->items[i].src->materials[0];
  if (same && R->graph != t3_graph_version()) {
    /* (the meshes already batched are hidden, not gone: batchable() does not read visible) */
    t3_object_traverse(root, collect_visit, &c);
    same = R->n == c.n;
    for (int i = 0; same && i < c.n; i++) same = R->items[i].src == c.m[i];
    if (same) R->graph = t3_graph_version();
  } else if (!same) {
    t3_object_traverse(root, collect_visit, &c);
  }
  if (!same) {
    if (R) rec_clear(R);
    else { R = *slot = cw_calloc(1, sizeof *R, CW_MEM_RENDER); S->recs++; }
    R->owner = S;
    R->root = root;
    R->cast = cast, R->receive = receive;
    R->items = c.n ? cw_calloc((size_t)c.n, sizeof *R->items, CW_MEM_RENDER) : NULL;
    for (int i = 0; i < c.n; i++) {
      t3_mesh *m = c.m[i];
      sb_batch *b = batch_for(S, m->materials[0], m->geometry, cast, receive);
      uint64_t gid;
      if (!cw_map_get(&b->geos, (uint64_t)(uintptr_t)m->geometry, &gid)) {
        int id = t3_batched_mesh_add_geometry(b->bm, m->geometry);
        if (id < 0) continue; /* (its attributes do not match: the mesh keeps drawing on its own) */
        gid = (uint64_t)id + 1;
        cw_map_set(&b->geos, (uint64_t)(uintptr_t)m->geometry, gid);
        S->geometries++;
      }
      t3_object_update_world_matrix(m, true, false); /* a mesh added this frame has no world matrix yet */
      sb_item *it = &R->items[R->n++];
      it->src = t3_retain(m);
      it->geo = m->geometry;
      it->mat = m->materials[0];
      it->batch = b;
      it->inst = t3_batched_mesh_add_instance(b->bm, (int)gid - 1);
      it->world = m->base.matrix_world;
      it->shown = true;
      t3_batched_mesh_set_matrix_at(b->bm, it->inst, &it->world);
      m->base.visible = false; /* the batch draws it */
      b->meshes++;
      S->meshes++;
    }
    R->graph = t3_graph_version();
    if (c.over) cw_warn("static batch: a MeshInstance3D with batch = true shows more than 64 meshes; the rest draw on their own");
  }
  for (int i = 0; i < R->n; i++) {
    sb_item *it = &R->items[i];
    it->src->base.visible = false;
    bool sh = shown(&it->src->base);
    if (sh != it->shown) {
      t3_batched_mesh_set_visible_at(it->batch->bm, it->inst, sh);
      it->shown = sh;
    }
    if (memcmp(&it->world, &it->src->base.matrix_world, sizeof it->world)) {
      it->world = it->src->base.matrix_world;
      t3_batched_mesh_set_matrix_at(it->batch->bm, it->inst, &it->world);
    }
  }
}

void cw_static_batch_get_stats(cw_tree *t, cw_static_batch_stats *out) {
  memset(out, 0, sizeof *out);
  cw__sbatch *S = t ? sbatch_of(t, false) : NULL;
  if (!S) return;
  out->meshes = S->meshes;
  out->geometries = S->geometries;
  for (int i = 0; i < S->batches.count; i++) {
    const sb_batch *b = S->batches.data[i];
    if (!b->meshes) continue;
    int runs = 0, inst = 0;
    t3_batched_mesh_last_draws(b->bm, &runs, &inst);
    out->batches++;
    out->draws += runs;
    out->drawn += inst;
  }
}
