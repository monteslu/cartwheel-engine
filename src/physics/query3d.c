/* Box3D queries: rays, shape casts, overlaps, batched rays on the job pool. */
#include <string.h>

#include "cartwheel/jobs.h"
#include "physics3d_internal.h"

static b3QueryFilter qfilter(const cw_phys_filter *f) {
  b3QueryFilter q = b3DefaultQueryFilter();
  q.categoryBits = cw__query_category(f);
  q.maskBits = cw__query_mask(f);
  return q;
}

/* the per-shape part of a cw_phys_filter the library cannot express */
static bool keep_shape(const cw_phys_filter *f, b3ShapeId s) {
  if (!f) return true;
  uint64_t sid = b3StoreShapeId(s);
  if (f->ignore_body && b3StoreBodyId(b3Shape_GetBody(s)) == f->ignore_body) return false;
  if (f->fn && !f->fn(sid, f->ctx)) return false;
  return true;
}
static bool needs_callback(const cw_phys_filter *f) { return f && (f->ignore_body || f->fn); }

static cw_ray_hit3d make_hit(b3ShapeId s, b3Pos p, b3Vec3 n, float fraction, uint64_t material, int triangle) {
  return (cw_ray_hit3d){ { b3StoreShapeId(s) }, { b3StoreBodyId(b3Shape_GetBody(s)) }, t3p(p), t3v(n), fraction,
                         material, triangle };
}

/* keep the cap smallest (fraction, shape id), sorted */
static void insert_hit(cw_ray_hit3d *hits, int *count, int cap, cw_ray_hit3d h) {
  int n = *count;
  if (n == cap && !cw__hit_less(h.fraction, h.shape.id, hits[n - 1].fraction, hits[n - 1].shape.id)) return;
  int i = n < cap ? n : cap - 1;
  while (i > 0 && cw__hit_less(h.fraction, h.shape.id, hits[i - 1].fraction, hits[i - 1].shape.id)) {
    hits[i] = hits[i - 1];
    i--;
  }
  hits[i] = h;
  if (n < cap) *count = n + 1;
}

typedef struct cast_ctx {
  const cw_phys_filter *filter;
  cw_ray_hit3d *hits;
  int count, cap;
  bool closest;
} cast_ctx;

static float cast_fn(b3ShapeId s, b3Pos p, b3Vec3 n, float fraction, uint64_t material, int triangle, int child,
                     void *ctx) {
  (void)child;
  cast_ctx *c = ctx;
  if (!keep_shape(c->filter, s)) return -1;
  insert_hit(c->hits, &c->count, c->cap, make_hit(s, p, n, fraction, material, triangle));
  if (c->closest) return fraction; /* clip: only closer hits from here on */
  return 1;
}

static bool ray_closest(b3WorldId id, t3_vec3 origin, t3_vec3 translation, const cw_phys_filter *f, cw_ray_hit3d *hit) {
  memset(hit, 0, sizeof *hit);
  hit->triangle = -1;
  if (!needs_callback(f)) {
    b3RayResult r = b3World_CastRayClosest(id, pos3(origin), v3(translation), qfilter(f));
    if (!r.hit) return false;
    *hit = make_hit(r.shapeId, r.point, r.normal, r.fraction, r.userMaterialId, r.triangleIndex);
    return true;
  }
  cast_ctx c = { f, hit, 0, 1, true };
  b3World_CastRay(id, pos3(origin), v3(translation), qfilter(f), cast_fn, &c);
  return c.count > 0;
}

bool cw_world3d_raycast(cw_world3d wh, t3_vec3 origin, t3_vec3 translation, const cw_phys_filter *filter,
                        cw_ray_hit3d *hit) {
  cw__world3 *w = cw__w3(wh);
  if (!w) {
    memset(hit, 0, sizeof *hit);
    return false;
  }
  return ray_closest(w->id, origin, translation, filter, hit);
}

int cw_world3d_raycast_all(cw_world3d wh, t3_vec3 origin, t3_vec3 translation, const cw_phys_filter *filter,
                           cw_ray_hit3d *hits, int cap) {
  cw__world3 *w = cw__w3(wh);
  if (!w || cap <= 0) return 0;
  cast_ctx c = { filter, hits, 0, cap, false };
  b3World_CastRay(w->id, pos3(origin), v3(translation), qfilter(filter), cast_fn, &c);
  return c.count;
}

typedef struct batch_ctx {
  b3WorldId id;
  const cw_ray3d *rays;
  const cw_phys_filter *filter;
  cw_ray_hit3d *hits;
} batch_ctx;

static void batch_fn(int begin, int end, int worker, void *ctx) {
  (void)worker;
  batch_ctx *b = ctx;
  for (int i = begin; i < end; i++) ray_closest(b->id, b->rays[i].origin, b->rays[i].translation, b->filter, &b->hits[i]);
}

int cw_world3d_raycast_batch(cw_world3d wh, const cw_ray3d *rays, int count, const cw_phys_filter *filter,
                             cw_ray_hit3d *hits) {
  cw__world3 *w = cw__w3(wh);
  if (!w || count <= 0) return 0;
  batch_ctx b = { .id = w->id, .rays = rays, .filter = filter, .hits = hits };
  cw_parallel_for(count, 16, batch_fn, &b);
  int n = 0;
  for (int i = 0; i < count; i++) n += hits[i].shape.id != 0;
  return n;
}

/* ── query shapes ────────────────────────────────────────────────────── */

cw_qshape3d cw_qshape3d_sphere(t3_vec3 c, float r) { return (cw_qshape3d){ .points = { c }, .count = 1, .radius = r }; }
cw_qshape3d cw_qshape3d_capsule(t3_vec3 a, t3_vec3 b, float r) {
  return (cw_qshape3d){ .points = { a, b }, .count = 2, .radius = r };
}
cw_qshape3d cw_qshape3d_box(t3_vec3 center, t3_vec3 half, t3_quat rotation) {
  cw_qshape3d s = { .count = 8 };
  b3Quat q = q3(rotation);
  for (int i = 0; i < 8; i++) {
    b3Vec3 l = { (i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z };
    s.points[i] = t3v(b3Add(v3(center), b3RotateVector(q, l)));
  }
  return s;
}
cw_qshape3d cw_qshape3d_points(const t3_vec3 *points, int count, float radius) {
  cw_qshape3d s = { .radius = radius };
  if (count > CW_QSHAPE3D_POINTS) {
    cw_warn("physics3d: query shape keeps %d of %d points", CW_QSHAPE3D_POINTS, count);
    count = CW_QSHAPE3D_POINTS;
  }
  memcpy(s.points, points, (size_t)count * sizeof *points);
  s.count = count;
  return s;
}

static b3ShapeProxy proxy(const cw_qshape3d *s) { return (b3ShapeProxy){ (const b3Vec3 *)s->points, s->count, s->radius }; }

int cw_world3d_shapecast(cw_world3d wh, const cw_qshape3d *shape, t3_vec3 translation, const cw_phys_filter *filter,
                         cw_ray_hit3d *hits, int cap) {
  cw__world3 *w = cw__w3(wh);
  if (!w || cap <= 0) return 0;
  b3ShapeProxy p = proxy(shape);
  cast_ctx c = { filter, hits, 0, cap, cap == 1 };
  b3World_CastShape(w->id, b3Pos_zero, &p, v3(translation), qfilter(filter), cast_fn, &c);
  return c.count;
}

typedef struct overlap_ctx {
  const cw_phys_filter *filter;
  cw_shape3d *out;
  int count, cap;
} overlap_ctx;

static bool overlap_fn(b3ShapeId s, void *ctx) {
  overlap_ctx *c = ctx;
  if (!keep_shape(c->filter, s)) return true;
  uint64_t id = b3StoreShapeId(s);
  int n = c->count;
  if (n == c->cap && id >= c->out[n - 1].id) return true;
  int i = n < c->cap ? n : c->cap - 1;
  while (i > 0 && id < c->out[i - 1].id) {
    c->out[i] = c->out[i - 1];
    i--;
  }
  c->out[i] = (cw_shape3d){ id };
  if (n < c->cap) c->count = n + 1;
  return true;
}

int cw_world3d_overlap(cw_world3d wh, const cw_qshape3d *shape, const cw_phys_filter *filter, cw_shape3d *out, int cap) {
  cw__world3 *w = cw__w3(wh);
  if (!w || cap <= 0) return 0;
  b3ShapeProxy p = proxy(shape);
  overlap_ctx c = { filter, out, 0, cap };
  b3World_OverlapShape(w->id, b3Pos_zero, &p, qfilter(filter), overlap_fn, &c);
  return c.count;
}

int cw_world3d_overlap_aabb(cw_world3d wh, t3_vec3 lo, t3_vec3 hi, const cw_phys_filter *filter, cw_shape3d *out,
                            int cap) {
  cw__world3 *w = cw__w3(wh);
  if (!w || cap <= 0) return 0;
  overlap_ctx c = { filter, out, 0, cap };
  b3World_OverlapAABB(w->id, (b3AABB){ v3(lo), v3(hi) }, qfilter(filter), overlap_fn, &c);
  return c.count;
}
