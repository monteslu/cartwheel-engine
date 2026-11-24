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
  bool pixel_dither; /* CW_TEST_FOLIAGE_PIXEL_DITHER=1: an instanced mesh's fades dither every pixel, as before (a control) */
  uint32_t taa_frame;
  float slots[CW_FOLIAGE_MOVER_SLOTS][4]; /* this frame's mover discs: x, z, radius, weight */
  int nslots;
  uint64_t slots_frame;  /* the clock frame the slots were packed at (+1) */
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
  const char *pd = cw_test_env("CW_TEST_FOLIAGE_PIXEL_DITHER");
  F.pixel_dither = pd && atoi(pd);
  if (F.time >= 3600.0) F.time -= 3600.0; /* (float precision in the shaders: a jump once an hour) */
}

float cw_foliage_time(void) {
  clock_tick();
  return (float)F.time;
}

/* ── movers ─────────────────────────────────────────────────────────── */

#define MOVERS 64
#define TRAIL 128
static struct {
  bool live;
  cw_foliage_mover m;
  float tx, tz;  /* where its trail last took a point */
  bool placed;   /* it has a position */
} mv[MOVERS];
static struct { float x, z, radius, strength, recover, t; } trail[TRAIL]; /* t: foliage time it was left */
static int trail_next;

cw_foliage_mover cw_foliage_mover_default(void) {
  return (cw_foliage_mover){ .radius = 0.5f, .strength = 1, .recover = 1.5f };
}

static void trail_drop(int id, float x, float z) {
  const cw_foliage_mover *m = &mv[id].m;
  if (!(m->recover > 0) || !(m->strength > 0)) return;
  trail[trail_next].x = x, trail[trail_next].z = z, trail[trail_next].radius = m->radius;
  trail[trail_next].strength = m->strength, trail[trail_next].recover = m->recover, trail[trail_next].t = cw_foliage_time();
  trail_next = (trail_next + 1) % TRAIL;
  F.slots_frame = 0;
}

int cw_foliage_mover_add(const cw_foliage_mover *m) {
  for (int i = 0; i < MOVERS; i++)
    if (!mv[i].live) {
      mv[i].live = true;
      mv[i].m = m ? *m : cw_foliage_mover_default();
      mv[i].placed = true;
      mv[i].tx = mv[i].m.x, mv[i].tz = mv[i].m.z;
      F.slots_frame = 0;
      return i;
    }
  cw_warn("cw_foliage_mover_add: %d movers in use", MOVERS);
  return -1;
}

void cw_foliage_mover_update(int id, float x, float z) {
  if (id < 0 || id >= MOVERS || !mv[id].live) return;
  mv[id].m.x = x, mv[id].m.z = z;
  /* a trail point every three quarters of its radius it moves: the plants it left stand up again over recover seconds */
  float dx = x - mv[id].tx, dz = z - mv[id].tz, step = CW_MAX(mv[id].m.radius * 0.75f, 0.05f);
  if (dx * dx + dz * dz >= step * step) {
    trail_drop(id, mv[id].tx, mv[id].tz);
    mv[id].tx = x, mv[id].tz = z;
  }
  F.slots_frame = 0;
}

void cw_foliage_mover_set(int id, const cw_foliage_mover *m) {
  if (id < 0 || id >= MOVERS || !mv[id].live || !m) return;
  mv[id].m = *m;
  mv[id].tx = m->x, mv[id].tz = m->z;
  F.slots_frame = 0;
}

bool cw_foliage_mover_get(int id, cw_foliage_mover *out) {
  if (id < 0 || id >= MOVERS || !mv[id].live) return false;
  if (out) *out = mv[id].m;
  return true;
}

void cw_foliage_mover_remove(int id) {
  if (id < 0 || id >= MOVERS || !mv[id].live) return;
  trail_drop(id, mv[id].m.x, mv[id].m.z); /* where it stood fades like its trail */
  if (mv[id].tx != mv[id].m.x || mv[id].tz != mv[id].m.z) trail_drop(id, mv[id].tx, mv[id].tz);
  mv[id].live = false;
  F.slots_frame = 0;
}

int cw_foliage_mover_count(void) {
  int n = 0;
  for (int i = 0; i < MOVERS; i++) n += mv[i].live;
  return n;
}

void cw_foliage_movers_clear(void) {
  memset(mv, 0, sizeof mv);
  memset(trail, 0, sizeof trail);
  trail_next = 0;
  F.slots_frame = 0;
}

/* the weight of a trail point now: its strength easing to 0 over recover seconds */
static float trail_weight(int i, float now) {
  if (!(trail[i].recover > 0) || !(trail[i].strength > 0)) return 0;
  float left = 1 - (now - trail[i].t) / trail[i].recover;
  if (left <= 0) return 0;
  left = cw_clampf(left, 0, 1);
  return trail[i].strength * left * left * (3 - 2 * left);
}

/* this frame's discs: every live mover where it is (at most the slots), then the trail points that still weigh most */
static void pack_slots(void) {
  uint64_t f = cw_frame_index() + 1;
  if (F.slots_frame == f) return;
  F.slots_frame = f;
  float now = cw_foliage_time();
  int n = 0;
  for (int i = 0; i < MOVERS && n < CW_FOLIAGE_MOVER_SLOTS; i++) {
    if (!mv[i].live || !(mv[i].m.strength > 0) || !(mv[i].m.radius > 0)) continue;
    F.slots[n][0] = mv[i].m.x, F.slots[n][1] = mv[i].m.z, F.slots[n][2] = mv[i].m.radius;
    F.slots[n][3] = cw_clampf(mv[i].m.strength, 0, 1);
    n++;
  }
  int first_trail = n;
  static bool used[TRAIL];
  memset(used, 0, sizeof used);
  while (n < CW_FOLIAGE_MOVER_SLOTS) {
    int best = -1;
    float bw = 0;
    for (int i = 0; i < TRAIL; i++) {
      float w = used[i] ? 0 : trail_weight(i, now);
      if (w > bw) best = i, bw = w;
    }
    if (best < 0) break;
    used[best] = true;
    F.slots[n][0] = trail[best].x, F.slots[n][1] = trail[best].z, F.slots[n][2] = trail[best].radius;
    F.slots[n][3] = cw_clampf(bw, 0, 1);
    n++;
  }
  /* more trail than slots: every trail disc less the heaviest one left out, so a point leaving the slots (or taking
   * one) does so at weight 0: the plants never jump */
  float cut = 0;
  for (int i = 0; i < TRAIL; i++)
    if (!used[i]) cut = CW_MAX(cut, trail_weight(i, now));
  if (cut > 0)
    for (int k = first_trail; k < n; k++) F.slots[k][3] = CW_MAX(F.slots[k][3] - cut, 0);
  F.nslots = n;
}

int cw_foliage_mover_slots(float *out, int cap) {
  pack_slots();
  int n = CW_MIN(F.nslots, cap);
  if (out) memcpy(out, F.slots, sizeof(float) * 4 * (size_t)n);
  return n;
}

float cw_foliage_push_at(float x, float z, float *dir_x, float *dir_z) {
  pack_slots();
  float px = 0, pz = 0;
  for (int i = 0; i < F.nslots; i++) {
    const float *m = F.slots[i];
    float ox = x - m[0], oz = z - m[1], d = sqrtf(ox * ox + oz * oz);
    float a = m[2] * 0.5f, t = cw_clampf((d - a) / CW_MAX(m[2] - a, 1e-6f), 0, 1);
    float f = m[3] * (1 - t * t * (3 - 2 * t));
    if (d > 1e-3f) px += ox / d * f, pz += oz / d * f;
    else px += f; /* (the shaders push a root on the centre along the wind: the material's; here +x) */
  }
  float l = sqrtf(px * px + pz * pz);
  if (dir_x) *dir_x = l > 1e-6f ? px / l : 0;
  if (dir_z) *dir_z = l > 1e-6f ? pz / l : 0;
  return l < 1 ? l : 1;
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
  float frame[4] = { cw_foliage_time(), (float)F.taa_frame * 5.588238f, F.pixel_dither ? 1.0f : 0.0f, F.lockstep ? 0.0f : 2.0f };
  float wind[4] = { 0, 0.5f, 1, 0 }, wind2[4] = { 0, 14, 1, 0 }, mode[4] = { 0, 0, 0, 0 }, fade[4] = { 0, 0, 0, 0 },
        root[4] = { 0.36f, 0, 0, 0 }, bend[4] = { 0, 0, 0, 0 };
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
    if (d->bend > 0) {
      pack_slots();
      bend[0] = cw_clampf(d->bend, 0, 1.5f);
      bend[1] = (float)F.nslots;
    }
    bend[2] = d->shadow_at_root ? 1.0f : 0.0f;
    bend[3] = cw_clampf(d->shadow_soften, 0, 1);
  }
  t3_extension_set_uniform(c, "folBend", bend, 4);
  static const char *const slot_names[CW_FOLIAGE_MOVER_SLOTS] = { "folMover0", "folMover1", "folMover2", "folMover3", "folMover4", "folMover5",
                                                                  "folMover6", "folMover7", "folMover8", "folMover9", "folMover10", "folMover11",
                                                                  "folMover12", "folMover13", "folMover14", "folMover15" };
  for (int k = 0; k < CW_FOLIAGE_MOVER_SLOTS; k++) {
    static const float none[4] = { 0, 0, 1, 0 };
    t3_extension_set_uniform(c, slot_names[k], k < (int)bend[1] ? F.slots[k] : none, 4);
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
  cw_foliage_movers_clear();
  index_init = false;
  memset(&F, 0, sizeof F);
}
