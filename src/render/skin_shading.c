/* Skin shading (cartwheel/skin_shading.h): the "skinshade" shading part's
 * materials and their values, its fill hook, and the quality switch.
 * The generator side is src/render/extensions/skinshade.mjs. */
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/quality.h"
#include "cartwheel/shading.h"
#include "cartwheel/skin_shading.h"

#define PART "skinshade"

typedef struct skin_rec {
  t3_material *m; /* retained */
  cw_skin_shading_desc d;
} skin_rec;

static CW_ARRAY(skin_rec) recs;
static cw_map index_of; /* material -> index + 1 */
static bool index_init;
static int enabled = -1; /* the quality setting's choice the parts follow (-1: not looked yet) */

cw_skin_shading_desc cw_skin_shading_default(void) {
  return (cw_skin_shading_desc){ .scatter = { 1.0f, 0.35f, 0.22f }, .wrap = 0.55f, .translucency = 0.35f,
                                 .distortion = 0.25f, .power = 3.0f, .ambient = 0.0f };
}

static int find(const t3_material *m) {
  uint64_t v;
  if (!index_init || !m || !cw_map_get(&index_of, (uint64_t)(uintptr_t)m, &v)) return -1;
  return (int)v - 1;
}

bool cw_skin_shading_enabled(void) {
  return cw_quality_scale("skin") > 0;
}

/* the part on or off on m, as the quality setting says */
static bool place(t3_material *m, bool on) {
  if (on) return cw_shading_add(m, PART);
  cw_shading_remove(m, PART);
  return true;
}

bool cw_skin_shading_apply(t3_material *m, const cw_skin_shading_desc *d) {
  if (!m) return false;
  if (m->type != T3_MESH_STANDARD_MATERIAL && m->type != T3_MESH_PHYSICAL_MATERIAL) {
    cw_warn("cw_skin_shading_apply: material '%s' is not a MeshStandardMaterial or MeshPhysicalMaterial", m->name ? m->name : "");
    return false;
  }
  if (!index_init) {
    cw_map_init(&index_of, CW_MEM_RENDER);
    index_init = true;
  }
  cw_skin_shading_desc v = d ? *d : cw_skin_shading_default();
  int i = find(m);
  if (i < 0) {
    if (!place(m, cw_skin_shading_enabled())) return false;
    cw_array_push(&recs, ((skin_rec){ t3_retain(m), v }));
    cw_map_set(&index_of, (uint64_t)(uintptr_t)m, (uint64_t)recs.count);
  } else {
    recs.data[i].d = v;
  }
  return true;
}

void cw_skin_shading_remove(t3_material *m) {
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

bool cw_skin_shading_get(const t3_material *m, cw_skin_shading_desc *out) {
  int i = find(m);
  if (i < 0) return false;
  if (out) *out = recs.data[i].d;
  return true;
}

int cw_skin_shading_count(void) { return recs.count; }

typedef struct { const char *name; const cw_skin_shading_desc *d; int n; CW_ARRAY(t3_material *) seen; } named_ctx;
static void named_visit(t3_object *o, void *u) {
  named_ctx *c = u;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  for (int k = 0; k < m->material_count; k++) {
    t3_material *mt = m->materials[k];
    if (!mt || !mt->name || strcmp(mt->name, c->name)) continue;
    bool dup = false;
    for (int j = 0; j < c->seen.count; j++) dup |= c->seen.data[j] == mt;
    if (dup) continue;
    cw_array_push(&c->seen, mt);
    if (cw_skin_shading_apply(mt, c->d)) c->n++;
  }
}
int cw_skin_shading_apply_named(t3_object *root, const char *material_name, const cw_skin_shading_desc *d) {
  if (!root || !material_name) return 0;
  named_ctx c = { material_name, d, 0, { 0 } };
  t3_object_traverse(root, named_visit, &c);
  cw_array_free(&c.seen);
  return c.n;
}

/* ── the part's hook: the drawn material's values ─────────────────────── */
static void fill(t3_extension_ctx *c, void *u) {
  (void)u;
  int i = find(t3_extension_material(c));
  if (i < 0) return; /* a material copied with the part on it: keeps the last values drawn */
  const cw_skin_shading_desc *d = &recs.data[i].d;
  float s[4] = { d->scatter.r, d->scatter.g, d->scatter.b, d->wrap < 0 ? 0 : d->wrap };
  float t[4] = { d->translucency, d->distortion, d->power > 0.01f ? d->power : 0.01f, d->ambient };
  t3_extension_set_uniform(c, "skinScatter", s, 4);
  t3_extension_set_uniform(c, "skinTranslucency", t, 4);
}

void cw__skin_shading_renderer_init(t3_renderer *r);
void cw__skin_shading_renderer_init(t3_renderer *r) {
  if (!r) return;
  cw_shading_part(r, PART, fill, NULL);
  /* the hook reads only the material, which an instanced batch shares */
  cw_shading_part_batching(r, PART, true);
}

/* once a frame (before the scene draws): follow the quality setting */
void cw__skin_shading_frame(void);
void cw__skin_shading_frame(void) {
  int on = cw_skin_shading_enabled();
  if (on == enabled) return;
  enabled = on;
  for (int i = 0; i < recs.count; i++) place(recs.data[i].m, on);
}

/* a fresh engine (tests): forget every material */
void cw__skin_shading_shutdown(void);
void cw__skin_shading_shutdown(void) {
  for (int i = 0; i < recs.count; i++) t3_release(recs.data[i].m);
  cw_array_free(&recs);
  if (index_init) cw_map_free(&index_of);
  index_init = false;
  enabled = -1;
}
