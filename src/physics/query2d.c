/* Box2D queries: mirrors query3d.c. */
#include <string.h>

#include "cartwheel/jobs.h"
#include "physics2d_internal.h"

static b2QueryFilter qfilter(const cw_phys_filter *f) {
  b2QueryFilter q = b2DefaultQueryFilter();
  q.categoryBits = cw__query_category(f);
  q.maskBits = cw__query_mask(f);
  return q;
}

static bool keep_shape(const cw_phys_filter *f, b2ShapeId s) {
  if (!f) return true;
  if (f->ignore_body && b2StoreBodyId(b2Shape_GetBody(s)) == f->ignore_body) return false;
  if (f->fn && !f->fn(b2StoreShapeId(s), f->ctx)) return false;
  return true;
}
static bool needs_callback(const cw_phys_filter *f) { return f && (f->ignore_body || f->fn); }

static cw_ray_hit2d make_hit(b2ShapeId s, b2Pos p, b2Vec2 n, float fraction) {
  return (cw_ray_hit2d){ { b2StoreShapeId(s) }, { b2StoreBodyId(b2Shape_GetBody(s)) }, cwp(p), cwv(n), fraction,
                         b2Shape_GetSurfaceMaterial(s).userMaterialId, -1 };
}

static void insert_hit(cw_ray_hit2d *hits, int *count, int cap, cw_ray_hit2d h) {
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
  cw_ray_hit2d *hits;
  int count, cap;
  bool closest;
} cast_ctx;

static float cast_fn(b2ShapeId s, b2Pos p, b2Vec2 n, float fraction, void *ctx) {
  cast_ctx *c = ctx;
  if (!keep_shape(c->filter, s)) return -1;
  insert_hit(c->hits, &c->count, c->cap, make_hit(s, p, n, fraction));
  return c->closest ? fraction : 1;
}

static bool ray_closest(b2WorldId id, cw_pos2 origin, cw_vec2 translation, const cw_phys_filter *f, cw_ray_hit2d *hit) {
  memset(hit, 0, sizeof *hit);
  hit->triangle = -1;
  if (!needs_callback(f)) {
    b2RayResult r = b2World_CastRayClosest(id, bp(origin), v2(translation), qfilter(f));
    if (!r.hit) return false;
    *hit = make_hit(r.shapeId, r.point, r.normal, r.fraction);
    return true;
  }
  cast_ctx c = { f, hit, 0, 1, true };
  b2World_CastRay(id, bp(origin), v2(translation), qfilter(f), cast_fn, &c);
  return c.count > 0;
}

bool cw_world2d_raycast(cw_world2d wh, cw_pos2 origin, cw_vec2 translation, const cw_phys_filter *filter,
                        cw_ray_hit2d *hit) {
  cw__world2 *w = cw__w2(wh);
  if (!w) {
    memset(hit, 0, sizeof *hit);
    return false;
  }
  return ray_closest(w->id, origin, translation, filter, hit);
}

int cw_world2d_raycast_all(cw_world2d wh, cw_pos2 origin, cw_vec2 translation, const cw_phys_filter *filter,
                           cw_ray_hit2d *hits, int cap) {
  cw__world2 *w = cw__w2(wh);
  if (!w || cap <= 0) return 0;
  cast_ctx c = { filter, hits, 0, cap, false };
  b2World_CastRay(w->id, bp(origin), v2(translation), qfilter(filter), cast_fn, &c);
  return c.count;
}

typedef struct batch_ctx {
  b2WorldId id;
  const cw_ray2d *rays;
  const cw_phys_filter *filter;
  cw_ray_hit2d *hits;
} batch_ctx;

static void batch_fn(int begin, int end, int worker, void *ctx) {
  (void)worker;
  batch_ctx *b = ctx;
  for (int i = begin; i < end; i++) ray_closest(b->id, b->rays[i].origin, b->rays[i].translation, b->filter, &b->hits[i]);
}

int cw_world2d_raycast_batch(cw_world2d wh, const cw_ray2d *rays, int count, const cw_phys_filter *filter,
                             cw_ray_hit2d *hits) {
  cw__world2 *w = cw__w2(wh);
  if (!w || count <= 0) return 0;
  batch_ctx b = { w->id, rays, filter, hits };
  cw_parallel_for(count, 16, batch_fn, &b);
  int n = 0;
  for (int i = 0; i < count; i++) n += hits[i].shape.id != 0;
  return n;
}

cw_qshape2d cw_qshape2d_circle(cw_vec2 c, float r) { return (cw_qshape2d){ .points = { c }, .count = 1, .radius = r }; }
cw_qshape2d cw_qshape2d_capsule(cw_vec2 a, cw_vec2 b, float r) {
  return (cw_qshape2d){ .points = { a, b }, .count = 2, .radius = r };
}
cw_qshape2d cw_qshape2d_box(cw_vec2 center, cw_vec2 half, cw_rot2 q) {
  cw_qshape2d s = { .count = 4 };
  const float sx[4] = { -1, 1, 1, -1 }, sy[4] = { -1, -1, 1, 1 };
  for (int i = 0; i < 4; i++) {
    cw_vec2 l = { sx[i] * half.x, sy[i] * half.y };
    s.points[i] = (cw_vec2){ center.x + q.c * l.x - q.s * l.y, center.y + q.s * l.x + q.c * l.y };
  }
  return s;
}
cw_qshape2d cw_qshape2d_points(const cw_vec2 *points, int count, float radius) {
  cw_qshape2d s = { .radius = radius };
  if (count > CW_QSHAPE2D_POINTS) {
    cw_warn("physics2d: query shape keeps %d of %d points", CW_QSHAPE2D_POINTS, count);
    count = CW_QSHAPE2D_POINTS;
  }
  memcpy(s.points, points, (size_t)count * sizeof *points);
  s.count = count;
  return s;
}

static b2ShapeProxy proxy(const cw_qshape2d *s) { return b2MakeProxy((const b2Vec2 *)s->points, s->count, s->radius); }

int cw_world2d_shapecast(cw_world2d wh, const cw_qshape2d *shape, cw_vec2 translation, const cw_phys_filter *filter,
                         cw_ray_hit2d *hits, int cap) {
  cw__world2 *w = cw__w2(wh);
  if (!w || cap <= 0) return 0;
  b2ShapeProxy p = proxy(shape);
  cast_ctx c = { filter, hits, 0, cap, cap == 1 };
  b2World_CastShape(w->id, bp(shape->origin), &p, v2(translation), qfilter(filter), cast_fn, &c);
  return c.count;
}

typedef struct overlap_ctx {
  const cw_phys_filter *filter;
  cw_shape2d *out;
  int count, cap;
} overlap_ctx;

static bool overlap_fn(b2ShapeId s, void *ctx) {
  overlap_ctx *c = ctx;
  if (!keep_shape(c->filter, s)) return true;
  uint64_t id = b2StoreShapeId(s);
  int n = c->count;
  if (n == c->cap && id >= c->out[n - 1].id) return true;
  int i = n < c->cap ? n : c->cap - 1;
  while (i > 0 && id < c->out[i - 1].id) {
    c->out[i] = c->out[i - 1];
    i--;
  }
  c->out[i] = (cw_shape2d){ id };
  if (n < c->cap) c->count = n + 1;
  return true;
}

int cw_world2d_overlap(cw_world2d wh, const cw_qshape2d *shape, const cw_phys_filter *filter, cw_shape2d *out, int cap) {
  cw__world2 *w = cw__w2(wh);
  if (!w || cap <= 0) return 0;
  b2ShapeProxy p = proxy(shape);
  overlap_ctx c = { filter, out, 0, cap };
  b2World_OverlapShape(w->id, bp(shape->origin), &p, qfilter(filter), overlap_fn, &c);
  return c.count;
}

int cw_world2d_overlap_aabb(cw_world2d wh, cw_pos2 lo, cw_pos2 hi, const cw_phys_filter *filter, cw_shape2d *out,
                            int cap) {
  cw__world2 *w = cw__w2(wh);
  if (!w || cap <= 0) return 0;
  overlap_ctx c = { filter, out, 0, cap };
  /* relative to lo, so the box keeps its precision far from the origin */
  b2World_OverlapAABB(w->id, bp(lo), (b2AABB){ { 0, 0 }, v2(cw_pos2_sub(hi, lo)) }, qfilter(filter), overlap_fn, &c);
  return c.count;
}
