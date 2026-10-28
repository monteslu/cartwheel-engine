/* Rim light (cartwheel/rim_light.h): the "rim" shading part's materials and
 * their values, and its fill hook. The generator side is
 * src/render/extensions/rim.mjs. */
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/rim_light.h"
#include "cartwheel/shading.h"

#define PART "rim"

typedef struct rim_rec {
  t3_material *m; /* retained */
  cw_rim_light_desc d;
} rim_rec;

static CW_ARRAY(rim_rec) recs;
static cw_map index_of; /* material -> index + 1 */
static bool index_init;

cw_rim_light_desc cw_rim_light_default(void) {
  return (cw_rim_light_desc){ .color = { 1.0f, 0.3f, 0.2f }, .strength = 2.0f, .power = 3.0f, .near = 6.0f, .far = 20.0f, .close = 0.15f };
}

static int find(const t3_material *m) {
  uint64_t v;
  if (!index_init || !m || !cw_map_get(&index_of, (uint64_t)(uintptr_t)m, &v)) return -1;
  return (int)v - 1;
}

bool cw_rim_light_apply(t3_material *m, const cw_rim_light_desc *d) {
  if (!m) return false;
  if (m->type != T3_MESH_STANDARD_MATERIAL && m->type != T3_MESH_PHYSICAL_MATERIAL) {
    cw_warn("cw_rim_light_apply: material '%s' is not a MeshStandardMaterial or MeshPhysicalMaterial", m->name ? m->name : "");
    return false;
  }
  if (!index_init) {
    cw_map_init(&index_of, CW_MEM_RENDER);
    index_init = true;
  }
  cw_rim_light_desc v = d ? *d : cw_rim_light_default();
  int i = find(m);
  if (i < 0) {
    if (!cw_shading_add(m, PART)) return false;
    cw_array_push(&recs, ((rim_rec){ t3_retain(m), v }));
    cw_map_set(&index_of, (uint64_t)(uintptr_t)m, (uint64_t)recs.count);
  } else {
    recs.data[i].d = v;
  }
  return true;
}

void cw_rim_light_remove(t3_material *m) {
  int i = find(m);
  if (i < 0) return;
  cw_shading_remove(m, PART);
  cw_map_remove(&index_of, (uint64_t)(uintptr_t)m);
  int last = recs.count - 1;
  if (i != last) {
    recs.data[i] = recs.data[last];
    cw_map_set(&index_of, (uint64_t)(uintptr_t)recs.data[i].m, (uint64_t)i + 1);
  }
  recs.count--;
  t3_release(m);
}

bool cw_rim_light_get(const t3_material *m, cw_rim_light_desc *out) {
  int i = find(m);
  if (i < 0) return false;
  if (out) *out = recs.data[i].d;
  return true;
}

int cw_rim_light_count(void) { return recs.count; }

typedef struct { const cw_rim_light_desc *d; int n; CW_ARRAY(t3_material *) seen; } tree_ctx;
static void tree_visit(t3_object *o, void *u) {
  tree_ctx *c = u;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  for (int k = 0; k < m->material_count; k++) {
    t3_material *mt = m->materials[k];
    if (!mt) continue;
    bool dup = false;
    for (int j = 0; j < c->seen.count; j++) dup |= c->seen.data[j] == mt;
    if (dup) continue;
    cw_array_push(&c->seen, mt);
    if (cw_rim_light_apply(mt, c->d)) c->n++;
  }
}
int cw_rim_light_apply_tree(t3_object *root, const cw_rim_light_desc *d) {
  if (!root) return 0;
  tree_ctx c = { d, 0, { 0 } };
  t3_object_traverse(root, tree_visit, &c);
  cw_array_free(&c.seen);
  return c.n;
}

typedef struct { int n; } rm_ctx;
static void rm_visit(t3_object *o, void *u) {
  rm_ctx *c = u;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  for (int k = 0; k < m->material_count; k++)
    if (m->materials[k] && find(m->materials[k]) >= 0) {
      cw_rim_light_remove(m->materials[k]);
      c->n++;
    }
}
int cw_rim_light_remove_tree(t3_object *root) {
  if (!root) return 0;
  rm_ctx c = { 0 };
  t3_object_traverse(root, rm_visit, &c);
  return c.n;
}

/* ── the part's hook: the drawn material's values ─────────────────────── */
static void fill(t3_extension_ctx *c, void *u) {
  (void)u;
  int i = find(t3_extension_material(c));
  float col[4] = { 0, 0, 0, 0 }, shape[4] = { 3, 6, 20, 0.15f };
  if (i >= 0) { /* (a material copied with the part on it draws no rim) */
    const cw_rim_light_desc *d = &recs.data[i].d;
    col[0] = d->color.r, col[1] = d->color.g, col[2] = d->color.b, col[3] = d->strength > 0 ? d->strength : 0;
    shape[0] = d->power > 0.05f ? d->power : 0.05f;
    shape[1] = d->near;
    shape[2] = d->far > d->near + 0.01f ? d->far : d->near + 0.01f;
    shape[3] = d->close < 0 ? 0 : d->close > 1 ? 1 : d->close;
  }
  t3_extension_set_uniform(c, "rimColor", col, 4);
  t3_extension_set_uniform(c, "rimShape", shape, 4);
}

void cw__rim_light_renderer_init(t3_renderer *r);
void cw__rim_light_renderer_init(t3_renderer *r) {
  if (!r) return;
  cw_shading_part(r, PART, fill, NULL);
  /* the hook reads only the material, which an instanced batch shares */
  cw_shading_part_batching(r, PART, true);
}

/* a fresh engine (tests): forget every material */
void cw__rim_light_shutdown(void);
void cw__rim_light_shutdown(void) {
  for (int i = 0; i < recs.count; i++) t3_release(recs.data[i].m);
  cw_array_free(&recs);
  if (index_init) cw_map_free(&index_of);
  index_init = false;
}
