/* Waterline (cartwheel/waterline.h): the "waterline" shading part's objects,
 * materials and values, and its fill hook. The generator side is
 * src/waterline/extensions/waterline.mjs. */
#include <math.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/quality.h"
#include "cartwheel/shading.h"
#include "cartwheel/waterline.h"
#include "../water/water_internal.h" /* the caustic tile the water's floor uses (caustics.c), read only */

#define PART "waterline"
#define NONE (-1.0e4f) /* a height no object reaches: no water, no mark */

struct cw_waterline {
  cw_waterline_desc d;
  bool known;       /* a surface was given */
  float level;      /* the surface over origin (m) */
  float mark;       /* the high-water mark over origin (m); NONE until the object first met the water */
  float soak;       /* 0..1 */
  t3_vec3 origin, n; /* the last origin and the fitted surface's normal */
  float plane_d;    /* dot(n, a surface point) */
  double time;      /* the water's clock (s) */
  float caus[3];    /* strength (times the quality), 1 / tile (m), 1.6 / clarity */
  int mats;         /* materials on it */
};

typedef struct wl_rec {
  t3_material *m; /* retained */
  cw_waterline *w;
  cw_waterline_look look;
} wl_rec;

static CW_ARRAY(wl_rec) recs;
static cw_map index_of; /* material -> index + 1 */
static bool index_init;
static t3_vec3 g_sun = { 0.34f, 0.88f, 0.34f };
static t3_texture *g_tile; /* the caustic tile, RGBA8 (red: the sharp web, green: the soft copy, both / 4) */

cw_waterline_desc cw_waterline_default(void) {
  return (cw_waterline_desc){ .wick = 0.12f, .fall_rate = 0.02f, .dry_time = 9.0f, .meniscus = 0.008f, .meniscus_strength = 0.25f,
                              .sky = { 0.55f, 0.68f, 0.82f }, .wobble = 0.006f, .caustics = 1.0f };
}

cw_waterline *cw_waterline_new(const cw_waterline_desc *d) {
  cw_waterline *w = cw_alloc(sizeof *w, CW_MEM_RENDER);
  memset(w, 0, sizeof *w);
  w->d = d ? *d : cw_waterline_default();
  w->mark = NONE;
  w->level = NONE;
  w->n = (t3_vec3){ 0, 1, 0 };
  w->plane_d = NONE;
  return w;
}

void cw_waterline_set_desc(cw_waterline *w, const cw_waterline_desc *d) {
  if (w) w->d = d ? *d : cw_waterline_default();
}

static int find(const t3_material *m) {
  uint64_t v;
  if (!index_init || !m || !cw_map_get(&index_of, (uint64_t)(uintptr_t)m, &v)) return -1;
  return (int)v - 1;
}

bool cw_waterline_apply(cw_waterline *w, t3_material *m, const cw_waterline_look *look) {
  if (!w || !m) return false;
  if (m->type != T3_MESH_STANDARD_MATERIAL && m->type != T3_MESH_PHYSICAL_MATERIAL) {
    cw_warn("cw_waterline_apply: material '%s' is not a MeshStandardMaterial or MeshPhysicalMaterial", m->name ? m->name : "");
    return false;
  }
  if (!index_init) {
    cw_map_init(&index_of, CW_MEM_RENDER);
    index_init = true;
  }
  cw_waterline_look lk = look ? *look : (cw_waterline_look){ 0.5f, 0.0f };
  lk.porosity = cw_clampf(lk.porosity, 0, 1), lk.shine = cw_clampf(lk.shine, 0, 1);
  int i = find(m);
  if (i < 0) {
    if (!cw_shading_add(m, PART)) return false;
    cw_array_push(&recs, ((wl_rec){ t3_retain(m), w, lk }));
    cw_map_set(&index_of, (uint64_t)(uintptr_t)m, (uint64_t)recs.count);
    w->mats++;
  } else {
    recs.data[i].w->mats--;
    recs.data[i].w = w, recs.data[i].look = lk;
    w->mats++;
  }
  return true;
}

void cw_waterline_remove(t3_material *m) {
  int i = find(m);
  if (i < 0) return;
  cw_shading_remove(m, PART);
  cw_map_remove(&index_of, (uint64_t)(uintptr_t)m);
  recs.data[i].w->mats--;
  int last = recs.count - 1;
  if (i != last) {
    recs.data[i] = recs.data[last];
    cw_map_set(&index_of, (uint64_t)(uintptr_t)recs.data[i].m, (uint64_t)i + 1);
  }
  recs.count--;
  t3_release(m);
}

void cw_waterline_free(cw_waterline *w) {
  if (!w) return;
  for (int i = recs.count - 1; i >= 0; i--)
    if (recs.data[i].w == w) cw_waterline_remove(recs.data[i].m);
  cw_free(w);
}

bool cw_waterline_get(const t3_material *m, cw_waterline **w, cw_waterline_look *look) {
  int i = find(m);
  if (i < 0) return false;
  if (w) *w = recs.data[i].w;
  if (look) *look = recs.data[i].look;
  return true;
}

int cw_waterline_count(void) { return recs.count; }

typedef struct { cw_waterline *w; const cw_waterline_look *look; int n; bool add; CW_ARRAY(t3_material *) seen; } tree_ctx;
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
    if (c->add ? cw_waterline_apply(c->w, mt, c->look) : find(mt) >= 0) {
      if (!c->add) cw_waterline_remove(mt);
      c->n++;
    }
  }
}
int cw_waterline_apply_tree(cw_waterline *w, t3_object *root, const cw_waterline_look *look) {
  if (!root || !w) return 0;
  tree_ctx c = { w, look, 0, true, { 0 } };
  t3_object_traverse(root, tree_visit, &c);
  cw_array_free(&c.seen);
  return c.n;
}
int cw_waterline_remove_tree(t3_object *root) {
  if (!root) return 0;
  tree_ctx c = { NULL, NULL, 0, false, { 0 } };
  t3_object_traverse(root, tree_visit, &c);
  cw_array_free(&c.seen);
  return c.n;
}

/* ── the water at the object ──────────────────────────────────────────── */
static void track(cw_waterline *w, t3_vec3 origin, float height, t3_vec3 n, float dt) {
  float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
  n = len > 1e-6f ? (t3_vec3){ n.x / len, n.y / len, n.z / len } : (t3_vec3){ 0, 1, 0 };
  if (n.y < 0.2f) n = (t3_vec3){ 0, 1, 0 }; /* (a surface steeper than that is a breaking wave the fit cannot see) */
  w->known = true;
  w->origin = origin;
  w->n = n;
  w->plane_d = n.x * origin.x + n.y * height + n.z * origin.z;
  w->level = height - origin.y;
  if (dt < 0) dt = 0;
  /* the mark rises with the water at once and falls back at fall_rate once it drops */
  float fall = w->d.fall_rate > 0 ? w->d.fall_rate * dt : 0;
  w->mark = w->mark <= NONE * 0.5f ? w->level : fmaxf(w->mark - fall, w->level);
  float dry = w->d.dry_time > 0.01f ? dt / w->d.dry_time : 1.0f;
  w->soak = cw_clampf(w->soak - dry, 0, 1);
}

static float prop_f(cw_node n, const char *name, float def) {
  cw_value v;
  if (!cw_node_get_prop(n, name, &v)) return def;
  return v.type == CW_T_FLOAT ? (float)v.f : v.type == CW_T_INT ? (float)v.i : def;
}

void cw_waterline_update(cw_waterline *w, cw_node water, t3_vec3 origin, float radius, float dt) {
  if (!w) return;
  cw_water_sample s;
  if (!cw_node_valid(water) || !cw_water_sample_at(water, origin.x, origin.z, cw_water_time(water), &s)) {
    /* out of the water: what is wet dries */
    w->known = false;
    w->plane_d = NONE;
    w->level = NONE;
    float dry = w->d.dry_time > 0.01f && dt > 0 ? dt / w->d.dry_time : 0;
    w->soak = cw_clampf(w->soak - dry, 0, 1);
    if (w->mark > NONE * 0.5f && dt > 0) w->mark -= w->d.fall_rate * dt;
    return;
  }
  double t = cw_water_time(water);
  float h0 = s.height + cw_water_ripple_at(water, origin.x, origin.z);
  t3_vec3 n = s.normal;
  if (radius > 0.01f) {
    float hx1 = cw_water_height_at(water, origin.x + radius, origin.z, t), hx0 = cw_water_height_at(water, origin.x - radius, origin.z, t);
    float hz1 = cw_water_height_at(water, origin.x, origin.z + radius, t), hz0 = cw_water_height_at(water, origin.x, origin.z - radius, t);
    /* a plane through the five heights: the mean height, the slopes across */
    float mean = (s.height + hx1 + hx0 + hz1 + hz0) * 0.2f;
    h0 = h0 - s.height + mean;
    n = (t3_vec3){ -(hx1 - hx0) / (2 * radius), 1, -(hz1 - hz0) / (2 * radius) };
  }
  w->time = t;
  /* the caustics the water's floor draws (Water3D caustics, caustic_size, clarity; Medium and up) */
  float cs = prop_f(water, "caustic_size", 5.0f), cl = prop_f(water, "clarity", 5.0f);
  w->caus[0] = cw_quality_get() >= CW_QUALITY_MEDIUM ? prop_f(water, "caustics", 1.0f) * w->d.caustics : 0.0f;
  w->caus[1] = 1.0f / (cs > 0.05f ? cs : 0.05f);
  w->caus[2] = 1.6f / (cl > 0.05f ? cl : 0.05f);
  track(w, origin, h0, n, dt);
}

void cw_waterline_update_plane(cw_waterline *w, t3_vec3 origin, float height, t3_vec3 normal, float dt) {
  if (!w) return;
  w->time += dt > 0 ? dt : 0;
  track(w, origin, height, normal, dt);
}

void cw_waterline_soak(cw_waterline *w, float amount) {
  if (w && amount > 0) w->soak = cw_clampf(w->soak + amount, 0, 1);
}

void cw_waterline_splash(cw_waterline *w, float height) {
  if (!w || !w->known || height <= 0) return;
  w->mark = fmaxf(w->mark, w->level + height);
}

void cw_waterline_state_of(const cw_waterline *w, cw_waterline_state *out) {
  if (!out) return;
  memset(out, 0, sizeof *out);
  out->normal = (t3_vec3){ 0, 1, 0 };
  if (!w) return;
  out->in_water = w->known;
  out->level = w->known ? w->level : 0;
  out->mark = w->mark > NONE * 0.5f ? w->mark : 0;
  out->band = w->known && w->mark > NONE * 0.5f ? fmaxf(0, w->mark - w->level) : 0;
  out->soak = w->soak;
  out->normal = w->n;
}

void cw_waterline_set_sun(t3_vec3 to_sun) {
  float l = sqrtf(to_sun.x * to_sun.x + to_sun.y * to_sun.y + to_sun.z * to_sun.z);
  if (l > 1e-6f) g_sun = (t3_vec3){ to_sun.x / l, to_sun.y / l, to_sun.z / l };
}

static t3_texture *tile(void) {
  if (g_tile) return g_tile;
  const uint8_t *c = cw__water_caustics(), *soft = cw__water_caustics_soft();
  const int n = CW_WATER_CAUSTIC_N;
  uint8_t *px = cw_alloc((size_t)n * (size_t)n * 4, CW_MEM_RENDER);
  for (int k = 0; k < n * n; k++) px[k * 4] = c[k], px[k * 4 + 1] = soft[k], px[k * 4 + 2] = 0, px[k * 4 + 3] = 255;
  g_tile = t3_data_texture_new(n, n, px);
  cw_free(px);
  g_tile->wrap_s = g_tile->wrap_t = T3_REPEAT;
  g_tile->mag_filter = T3_LINEAR;
  g_tile->min_filter = T3_LINEAR_MIPMAP_LINEAR;
  g_tile->generate_mipmaps = true;
  g_tile->version++;
  return g_tile;
}

/* ── the part's hook: the drawn material's values ─────────────────────── */
static void fill(t3_extension_ctx *c, void *u) {
  (void)u;
  int i = find(t3_extension_material(c));
  /* (a material copied with the part on it draws as a dry one, out of the water) */
  float plane[4] = { 0, 1, 0, NONE }, mark[4] = { NONE, 0, 0, 0 }, look[4] = { 0.5f, 0, 0.008f, 0 };
  float sky[4] = { 0, 0, 0, 0 }, caus[4] = { g_sun.x, g_sun.y, g_sun.z, 0.2f }, extra[4] = { 0, 0.32f, 0, 0 };
  if (i >= 0) {
    const cw_waterline *w = recs.data[i].w;
    const cw_waterline_look *lk = &recs.data[i].look;
    if (w->known) plane[0] = w->n.x, plane[1] = w->n.y, plane[2] = w->n.z, plane[3] = w->plane_d;
    mark[0] = w->mark > NONE * 0.5f ? w->origin.y + w->mark : NONE;
    mark[1] = w->soak;
    mark[2] = w->d.wobble > 0 ? w->d.wobble : 0;
    mark[3] = (float)fmod(w->time, 1000.0);
    look[0] = lk->porosity, look[1] = lk->shine;
    look[2] = w->d.meniscus > 0 ? w->d.meniscus : 0;
    look[3] = w->d.meniscus_strength > 0 ? w->d.meniscus_strength : 0;
    sky[0] = w->d.sky.r, sky[1] = w->d.sky.g, sky[2] = w->d.sky.b, sky[3] = w->caus[0];
    caus[3] = w->caus[1] > 0 ? w->caus[1] : 0.2f;
    extra[0] = (float)fmod(w->time, 1000.0);
    extra[1] = w->caus[2] > 0 ? w->caus[2] : 0.32f;
    extra[2] = w->d.wick > 0 ? w->d.wick : 0;
  }
  t3_extension_set_uniform(c, "wlPlane", plane, 4);
  t3_extension_set_uniform(c, "wlMark", mark, 4);
  t3_extension_set_uniform(c, "wlLook", look, 4);
  t3_extension_set_uniform(c, "wlSky", sky, 4);
  t3_extension_set_uniform(c, "wlCaus", caus, 4);
  t3_extension_set_uniform(c, "wlExtra", extra, 4);
  t3_extension_set_texture(c, "wlCaustic", tile());
}

void cw__waterline_renderer_init(t3_renderer *r);
void cw__waterline_renderer_init(t3_renderer *r) {
  if (!r) return;
  cw_shading_part(r, PART, fill, NULL);
  /* the hook reads only the material, which an instanced batch shares */
  cw_shading_part_batching(r, PART, true);
  /* it decides what the lighting shades (colour, roughness, emissive) and leaves the lighting to the engine: probes, the
   * G-buffer and mesh decals compose with it as with a game's surface */
  cw_shading_part_traits(PART, CW_PART_SURFACE);
}

/* a fresh engine (tests): forget every material */
void cw__waterline_shutdown(void);
void cw__waterline_shutdown(void) {
  for (int i = 0; i < recs.count; i++) t3_release(recs.data[i].m);
  cw_array_free(&recs);
  if (index_init) cw_map_free(&index_of);
  index_init = false;
  if (g_tile) t3_release(g_tile);
  g_tile = NULL;
}
