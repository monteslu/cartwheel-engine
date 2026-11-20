/* Toon shading (cartwheel/toon.h): three.js r187's MeshToonMaterial made
 * from other materials, the shared gradient ramps, and the table of the
 * materials cw_toon_apply_tree replaced. */
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/toon.h"

static t3_texture *ramps[CW_TOON_MAX_BANDS + 1];

t3_texture *cw_toon_ramp(int bands) {
  if (bands < 2) return NULL;
  if (bands > CW_TOON_MAX_BANDS) bands = CW_TOON_MAX_BANDS;
  if (!ramps[bands]) {
    uint8_t d[CW_TOON_MAX_BANDS * 4];
    for (int i = 0; i < bands; i++) {
      uint8_t v = (uint8_t)(((i + 1) * 255 + bands / 2) / bands);
      d[i * 4] = d[i * 4 + 1] = d[i * 4 + 2] = v;
      d[i * 4 + 3] = 255;
    }
    ramps[bands] = t3_data_texture_new(bands, 1, d); /* a DataTexture: nearest, no mipmaps, clamped */
  }
  return ramps[bands];
}

static bool lit(const t3_material *m) {
  return m->type == T3_MESH_LAMBERT_MATERIAL || m->type == T3_MESH_PHONG_MATERIAL || m->type == T3_MESH_STANDARD_MATERIAL ||
         m->type == T3_MESH_PHYSICAL_MATERIAL;
}

t3_material *cw_toon_material(const t3_material *src, int bands) {
  t3_material *m = t3_mesh_toon_material_new(0xffffff);
  if (src) {
    m->color = src->color;
    m->opacity = src->opacity;
    m->transparent = src->transparent;
    m->alpha_test = src->alpha_test;
    m->side = src->side;
    m->shadow_side = src->shadow_side;
    m->vertex_colors = src->vertex_colors;
    m->flat_shading = src->flat_shading;
    m->fog = src->fog;
    m->visible = src->visible;
    m->depth_test = src->depth_test;
    m->depth_write = src->depth_write;
    m->blending = src->blending;
    m->emissive = src->emissive;
    m->emissive_intensity = src->emissive_intensity;
    m->normal_scale = src->normal_scale;
    m->ao_map_intensity = src->ao_map_intensity;
    m->light_map_intensity = src->light_map_intensity;
    m->name = src->name;
    if (src->map) t3_material_set_texture(m, T3_MAP, src->map);
    if (src->emissive_map) t3_material_set_texture(m, T3_EMISSIVE_MAP, src->emissive_map);
    if (src->normal_map) t3_material_set_texture(m, T3_NORMAL_MAP, src->normal_map);
    if (src->ao_map) t3_material_set_texture(m, T3_AO_MAP, src->ao_map);
    if (src->light_map) t3_material_set_texture(m, T3_LIGHT_MAP, src->light_map);
    if (src->alpha_map) t3_material_set_texture(m, T3_ALPHA_MAP, src->alpha_map);
  }
  t3_texture *ramp = cw_toon_ramp(bands);
  if (ramp) t3_material_set_texture(m, T3_GRADIENT_MAP, ramp);
  return m;
}

/* the materials cw_toon_apply_tree replaced: both retained while a mesh draws the toon one */
typedef struct toon_rec {
  t3_material *src, *toon;
  uint32_t src_version;
  int bands;
} toon_rec;
static CW_ARRAY(toon_rec) recs;

/* records whose toon material nothing draws any more (only this table holds it) go */
static void sweep(void) {
  for (int i = 0; i < recs.count;) {
    if (recs.data[i].toon->_refs == 1) {
      t3_release(recs.data[i].toon);
      t3_release(recs.data[i].src);
      recs.data[i] = recs.data[--recs.count];
    } else {
      i++;
    }
  }
}

static int find_toon(const t3_material *m) {
  for (int i = 0; i < recs.count; i++)
    if (recs.data[i].toon == m) return i;
  return -1;
}

bool cw_toon_is_applied(const t3_material *m) { return m && find_toon(m) >= 0; }

typedef struct { int bands, n; } apply_ctx;
static void apply_visit(t3_object *o, void *u) {
  apply_ctx *c = u;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH && o->type != T3_INSTANCED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  for (int k = 0; k < m->material_count; k++) {
    t3_material *mt = m->materials[k];
    if (!mt || !lit(mt)) continue;
    t3_material *toon = NULL;
    for (int i = 0; i < recs.count && !toon; i++)
      if (recs.data[i].src == mt && recs.data[i].bands == c->bands && recs.data[i].src_version == mt->version) toon = recs.data[i].toon;
    if (!toon) {
      toon = cw_toon_material(mt, c->bands);
      cw_array_push(&recs, ((toon_rec){ t3_retain(mt), toon, mt->version, c->bands }));
    }
    t3_retain(toon);
    t3_release(mt);
    m->materials[k] = toon;
    c->n++;
  }
}

int cw_toon_apply_tree(t3_object *root, int bands) {
  if (!root) return 0;
  if (bands > CW_TOON_MAX_BANDS) bands = CW_TOON_MAX_BANDS;
  if (bands < 2) bands = 0;
  apply_ctx c = { bands, 0 };
  t3_object_traverse(root, apply_visit, &c);
  sweep();
  return c.n;
}

static void restore_visit(t3_object *o, void *u) {
  int *n = u;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH && o->type != T3_INSTANCED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  for (int k = 0; k < m->material_count; k++) {
    int i = m->materials[k] ? find_toon(m->materials[k]) : -1;
    if (i < 0) continue;
    t3_material *src = recs.data[i].src;
    t3_retain(src);
    t3_release(m->materials[k]);
    m->materials[k] = src;
    (*n)++;
  }
}

int cw_toon_restore_tree(t3_object *root) {
  if (!root) return 0;
  int n = 0;
  t3_object_traverse(root, restore_visit, &n);
  sweep();
  return n;
}
