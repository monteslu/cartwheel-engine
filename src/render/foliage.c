/* Foliage (cartwheel/foliage.h): the "foliage" and "folalpha" shading
 * parts' materials and values, their fill hooks, and the foliage clock. The
 * generator side is src/render/extensions/foliage.mjs and folalpha.mjs. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/foliage.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"
#include "cartwheel/shading.h"

#define PART "foliage"
#define PART_ALPHA "folalpha"

typedef struct fol_rec {
  t3_material *m; /* retained */
  cw_foliage_desc d;
  bool a2c_was;   /* m->alpha_to_coverage before smooth_alpha set it */
  bool alpha;     /* the folalpha part is on m */
} fol_rec;

static CW_ARRAY(fol_rec) recs;
static cw_map index_of; /* material -> index + 1 */
static bool index_init;

/* the frame's state: the clock, and what the scene pass draws into (cw__foliage_frame, the render pipeline) */
static struct {
  uint64_t frame;  /* cw_frame_index() the clock last advanced at (+1: 0 = never) */
  double time;
  bool msaa, taa;
  uint32_t taa_frame;
  bool lockstep;   /* CW_TEST_FOLIAGE_LOCKSTEP=1 (a gate's control): every plant sways in the same phase */
} F;

cw_wind cw_wind_default(void) {
  return (cw_wind){ .strength = 0.08f, .frequency = 0.5f, .direction = { 1, 0 }, .gust = 0.5f, .gust_scale = 14 };
}

cw_foliage_desc cw_foliage_default(void) {
  cw_foliage_desc d = { 0 };
  d.wind = cw_wind_default();
  d.wind.strength = 0;
  return d;
}

static void clock_tick(void) {
  uint64_t f = cw_frame_index() + 1;
  if (F.frame == f) return;
  if (F.frame) F.time += cw_frame_dt();
  F.frame = f;
  const char *lock = cw_test_env("CW_TEST_FOLIAGE_LOCKSTEP");
  F.lockstep = lock && atoi(lock);
  if (F.time >= 3600.0) F.time -= 3600.0; /* (float precision in the shaders: a jump once an hour) */
}

float cw_foliage_time(void) {
  clock_tick();
  return (float)F.time;
}

void cw__foliage_frame(bool msaa, bool taa, uint64_t frame);
void cw__foliage_frame(bool msaa, bool taa, uint64_t frame) {
  clock_tick();
  F.msaa = msaa;
  F.taa = taa;
  F.taa_frame = taa ? (uint32_t)(frame % 64u) : 0;
}

/* ── the wind, as the shader computes it ────────────────────────────── */

static float fract_(float x) { return x - floorf(x); }
static float hash2(float i, float j) { return fract_(sinf(i * 127.1f + j * 311.7f) * 43758.5453f); }
/* smooth value noise in [0, 1) */
static float vnoise(float x, float z) {
  float ix = floorf(x), iz = floorf(z), fx = x - ix, fz = z - iz;
  float ux = fx * fx * (3 - 2 * fx), uz = fz * fz * (3 - 2 * fz);
  float a = hash2(ix, iz), b = hash2(ix + 1, iz), c = hash2(ix, iz + 1), d = hash2(ix + 1, iz + 1);
  return a + (b - a) * ux + (c - a) * uz + (a - b - c + d) * ux * uz;
}

t3_vec3 cw_wind_lean(const cw_wind *w, float x, float z, float share, float h, float s, float t) {
  if (!w || !(w->strength > 0)) return (t3_vec3){ 0, 0, 0 };
  float dx = w->direction[0], dz = w->direction[1], l = sqrtf(dx * dx + dz * dz);
  if (l < 1e-6f) dx = 1, dz = 0, l = 1;
  dx /= l, dz /= l;
  float hs = cw_clampf(share, 0, 1);
  float k = w->strength * hs * hs * s;
  float g = cw_clampf(w->gust, 0, 1), L = w->gust_scale > 0.5f ? w->gust_scale : 0.5f;
  float th = 6.2831853f * (w->frequency * t + 2 * vnoise(x * 0.9f, z * 0.9f));
  float s1 = sinf(th);
  float gw = 0.5f + 0.5f * sinf(6.2831853f * ((x * dx + z * dz) / L - 0.25f * w->frequency * t));
  gw *= gw;
  float a = k * ((1 - g) * (0.5f + 0.5f * s1) + g * gw * (0.75f + 0.25f * s1));
  float c = k * 0.125f * sinf(2.3f * th + 1.7f);
  float hh = (h > 0 ? h : 1) * s;
  float drop = (a * a + c * c) / (2 * (hh > 0.05f ? hh : 0.05f)) * hs;
  return (t3_vec3){ dx * a - dz * c, -drop, dz * a + dx * c };
}

/* ── materials ──────────────────────────────────────────────────────── */

static int find(const t3_material *m) {
  uint64_t v;
  if (!index_init || !m || !cw_map_get(&index_of, (uint64_t)(uintptr_t)m, &v)) return -1;
  return (int)v - 1;
}

static bool lit_type(const t3_material *m) {
  return m->type == T3_MESH_STANDARD_MATERIAL || m->type == T3_MESH_PHYSICAL_MATERIAL || m->type == T3_MESH_LAMBERT_MATERIAL ||
         m->type == T3_MESH_PHONG_MATERIAL;
}

bool cw_foliage_apply(t3_material *m, const cw_foliage_desc *d) {
  if (!m) return false;
  if (!lit_type(m)) {
    cw_warn("cw_foliage_apply: material '%s' is not a MeshStandardMaterial, MeshPhysicalMaterial, MeshLambertMaterial or "
            "MeshPhongMaterial", m->name ? m->name : "");
    return false;
  }
  if (!index_init) {
    cw_map_init(&index_of, CW_MEM_RENDER);
    index_init = true;
  }
  cw_foliage_desc v = d ? *d : cw_foliage_default();
  bool want_alpha = v.smooth_alpha && m->alpha_test > 0;
  int i = find(m);
  if (i < 0) {
    if (!cw_shading_add(m, PART)) return false;
    if (want_alpha && !cw_shading_add(m, PART_ALPHA)) {
      cw_shading_remove(m, PART);
      return false;
    }
    cw_array_push(&recs, ((fol_rec){ t3_retain(m), v, m->alpha_to_coverage, want_alpha }));
    cw_map_set(&index_of, (uint64_t)(uintptr_t)m, (uint64_t)recs.count);
    i = recs.count - 1;
  } else {
    fol_rec *r = &recs.data[i];
    if (want_alpha && !r->alpha) {
      if (!cw_shading_add(m, PART_ALPHA)) return false;
      r->alpha = true;
    } else if (!want_alpha && r->alpha) {
      cw_shading_remove(m, PART_ALPHA);
      r->alpha = false;
    }
    r->d = v;
  }
  bool a2c = recs.data[i].alpha ? true : recs.data[i].a2c_was;
  if (m->alpha_to_coverage != a2c) {
    m->alpha_to_coverage = a2c;
    m->version++;
  }
  return true;
}

static float geometry_top(t3_geometry *g) {
  if (!g) return 1;
  if (!g->has_bounding_box) t3_geometry_compute_bounding_box(g);
  float top = g->bounding_box.max.y;
  return top > 0.05f ? top : 0.05f;
}

bool cw_foliage_apply_mesh(t3_material *m, t3_geometry *g, const cw_foliage_desc *d) {
  cw_foliage_desc v = d ? *d : cw_foliage_default();
  if (!(v.height > 0)) v.height = geometry_top(g);
  return cw_foliage_apply(m, &v);
}

void cw_foliage_remove(t3_material *m) {
  int i = find(m);
  if (i < 0) return;
  fol_rec r = recs.data[i];
  cw_shading_remove(m, PART);
  if (r.alpha) cw_shading_remove(m, PART_ALPHA);
  if (m->alpha_to_coverage != r.a2c_was) {
    m->alpha_to_coverage = r.a2c_was;
    m->version++;
  }
  cw_map_remove(&index_of, (uint64_t)(uintptr_t)m);
  int last = recs.count - 1;
  if (i != last) {
    recs.data[i] = recs.data[last];
    cw_map_set(&index_of, (uint64_t)(uintptr_t)recs.data[i].m, (uint64_t)i + 1);
  }
  recs.count--;
  t3_release(m);
}

bool cw_foliage_get(const t3_material *m, cw_foliage_desc *out) {
  int i = find(m);
  if (i < 0) return false;
  if (out) *out = recs.data[i].d;
  return true;
}

int cw_foliage_count(void) { return recs.count; }

typedef struct { const cw_foliage_desc *d; int n; CW_ARRAY(t3_material *) seen; } tree_ctx;
static void tree_visit(t3_object *o, void *u) {
  tree_ctx *c = u;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH && o->type != T3_INSTANCED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  for (int k = 0; k < m->material_count; k++) {
    t3_material *mt = m->materials[k];
    if (!mt) continue;
    bool dup = false;
    for (int j = 0; j < c->seen.count; j++) dup |= c->seen.data[j] == mt;
    if (dup) continue;
    cw_array_push(&c->seen, mt);
    if (cw_foliage_apply_mesh(mt, m->geometry, c->d)) c->n++;
  }
}
int cw_foliage_apply_tree(t3_object *root, const cw_foliage_desc *d) {
  if (!root) return 0;
  tree_ctx c = { d, 0, { 0 } };
  t3_object_traverse(root, tree_visit, &c);
  cw_array_free(&c.seen);
  return c.n;
}

/* ── the parts' hooks ───────────────────────────────────────────────── */

static float band_lo(float a) { return a > 0 ? a : 0; }

static void fill(t3_extension_ctx *c, void *u) {
  (void)u;
  int i = find(t3_extension_material(c));
  float frame[4] = { cw_foliage_time(), (float)F.taa_frame * 5.588238f, F.msaa ? 1.0f : 2.0f, F.lockstep ? 0.0f : 2.0f };
  float wind[4] = { 0, 0.5f, 1, 0 }, wind2[4] = { 0, 14, 1, 0 }, mode[4] = { 0, 0, 0, 0 }, fade[4] = { 0, 0, 0, 0 },
        root[4] = { 0.36f, 0, 0, 0 };
  if (i >= 0) { /* (a material copied with the part on it draws as with none of it) */
    const cw_foliage_desc *d = &recs.data[i].d;
    const cw_wind *w = &d->wind;
    float dx = w->direction[0], dz = w->direction[1], l = sqrtf(dx * dx + dz * dz);
    if (l < 1e-6f) dx = 1, dz = 0, l = 1;
    float h = d->height > 0 ? d->height : 1;
    wind[0] = w->strength > 0 ? w->strength : 0;
    wind[1] = w->frequency;
    wind[2] = dx / l, wind[3] = dz / l;
    wind2[0] = cw_clampf(w->gust, 0, 1);
    wind2[1] = w->gust_scale > 0.5f ? w->gust_scale : 0.5f;
    wind2[2] = 1 / h;
    wind2[3] = h;
    mode[0] = d->two_sided_lighting ? 1.0f : 0.0f;
    mode[1] = d->root_color ? 1.0f : 0.0f;
    mode[2] = cw_clampf(d->tip_jitter, 0, 1);
    float in0 = band_lo(d->fade_in_from), in1 = d->fade_in_to, out0 = band_lo(d->fade_out_from), out1 = d->fade_out_to;
    fade[0] = in0, fade[1] = in1 > in0 ? in1 : in0;
    fade[2] = out0, fade[3] = out1 > out0 ? out1 : out0;
    mode[3] = (fade[1] > fade[0] || fade[3] > fade[2]) ? 1.0f : 0.0f;
    root[0] = d->root_height > 0 ? d->root_height : 0.6f * h;
  }
  t3_extension_set_uniform(c, "folFrame", frame, 4);
  t3_extension_set_uniform(c, "folWind", wind, 4);
  t3_extension_set_uniform(c, "folWind2", wind2, 4);
  t3_extension_set_uniform(c, "folMode", mode, 4);
  t3_extension_set_uniform(c, "folFade", fade, 4);
  /* (only a program with an instance colour has it) */
  const t3_object *o = t3_extension_object(c);
  if (o && o->type == T3_INSTANCED_MESH && ((const t3_instanced_mesh *)o)->instance_color) t3_extension_set_uniform(c, "folRoot", root, 4);
}

static void fill_alpha(t3_extension_ctx *c, void *u) {
  (void)u;
  float frame[4] = { cw_foliage_time(), (float)F.taa_frame * 5.588238f, F.msaa ? 1.0f : 2.0f, 0 };
  t3_extension_set_uniform(c, "folAlphaFrame", frame, 4);
}

void cw__foliage_renderer_init(t3_renderer *r);
void cw__foliage_renderer_init(t3_renderer *r) {
  if (!r) return;
  cw_shading_part_traits(PART, CW_PART_SURFACE | CW_PART_MOVES_VERTICES);
  cw_shading_part_traits(PART_ALPHA, CW_PART_SURFACE);
  cw_shading_part(r, PART, fill, NULL);
  cw_shading_part(r, PART_ALPHA, fill_alpha, NULL);
  /* the hooks read the material, and whether the object has an instance colour: both the same across a batch */
  cw_shading_part_batching(r, PART, true);
  cw_shading_part_batching(r, PART_ALPHA, true);
}

/* a fresh engine (tests): forget every material */
void cw__foliage_shutdown(void);
void cw__foliage_shutdown(void) {
  for (int i = 0; i < recs.count; i++) t3_release(recs.data[i].m);
  cw_array_free(&recs);
  if (index_init) cw_map_free(&index_of);
  index_init = false;
  memset(&F, 0, sizeof F);
}
