/* The visibility BVH: a 4-wide BVH over static mesh boxes (median splits on
 * the widest centroid axis, two per level), refitted when statics move, and
 * a frustum walk that tests a node's four child boxes against each plane at
 * once with SIMD (GCC/clang vector extensions: SSE natively, simd128 in
 * carts). The top of the tree is walked on the calling thread until there
 * are enough subtrees to spread over the job pool; each job marks the prims
 * it finds by record id (distinct ids: no races, no ordering). */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/jobs.h"
#include "vis_internal.h"

#define LEAF_MAX 4

typedef float v4f __attribute__((vector_size(16)));
typedef int32_t v4i __attribute__((vector_size(16)));

static inline v4f ld4(const float *p) {
  v4f r;
  memcpy(&r, p, sizeof r);
  return r;
}

/* ── frustum ────────────────────────────────────────────────────────── */

void cw__vis_frustum_from(vis_frustum *f, const t3_mat4 *vp) {
  t3_frustum t;
  t3_frustum_from_matrix(&t, vp);
  f->n = 6;
  for (int i = 0; i < 6; i++) {
    f->nx[i] = t.planes[i].normal.x;
    f->ny[i] = t.planes[i].normal.y;
    f->nz[i] = t.planes[i].normal.z;
    f->d[i] = t.planes[i].constant;
  }
}

int cw__vis_box_test(const vis_frustum *f, const float mn[3], const float mx[3], const float *sweep) {
  int inside = 1;
  for (int p = 0; p < f->n; p++) {
    float nx = f->nx[p], ny = f->ny[p], nz = f->nz[p];
    float s = sweep ? nx * sweep[0] + ny * sweep[1] + nz * sweep[2] : 0;
    float far = nx * (nx >= 0 ? mx[0] : mn[0]) + ny * (ny >= 0 ? mx[1] : mn[1]) + nz * (nz >= 0 ? mx[2] : mn[2]) + f->d[p];
    if (far + (s > 0 ? s : 0) < 0) return 0;
    float near = nx * (nx >= 0 ? mn[0] : mx[0]) + ny * (ny >= 0 ? mn[1] : mx[1]) + nz * (nz >= 0 ? mn[2] : mx[2]) + f->d[p];
    if (near + (s < 0 ? s : 0) < 0) inside = 0;
  }
  return inside ? 2 : 1;
}

/* ── build ──────────────────────────────────────────────────────────── */

typedef struct build_ctx {
  vis_bvh *b;
  const float *mins, *maxs;
  int32_t *order; /* box indices */
  int axis;
} build_ctx;

static build_ctx *sort_ctx;
static int cmp_centroid(const void *a, const void *b) {
  int ia = *(const int32_t *)a, ib = *(const int32_t *)b;
  int ax = sort_ctx->axis;
  float ca = sort_ctx->mins[3 * ia + ax] + sort_ctx->maxs[3 * ia + ax];
  float cb = sort_ctx->mins[3 * ib + ax] + sort_ctx->maxs[3 * ib + ax];
  if (ca != cb) return ca < cb ? -1 : 1;
  return ia < ib ? -1 : ia > ib;
}

static void range_bounds(build_ctx *c, int lo, int hi, float mn[3], float mx[3]) {
  for (int k = 0; k < 3; k++) {
    mn[k] = 1e30f;
    mx[k] = -1e30f;
  }
  for (int i = lo; i < hi; i++) {
    int j = c->order[i];
    for (int k = 0; k < 3; k++) {
      if (c->mins[3 * j + k] < mn[k]) mn[k] = c->mins[3 * j + k];
      if (c->maxs[3 * j + k] > mx[k]) mx[k] = c->maxs[3 * j + k];
    }
  }
}

/* sort [lo, hi) along the widest centroid axis; returns the median */
static int split(build_ctx *c, int lo, int hi) {
  float cmn[3] = { 1e30f, 1e30f, 1e30f }, cmx[3] = { -1e30f, -1e30f, -1e30f };
  for (int i = lo; i < hi; i++) {
    int j = c->order[i];
    for (int k = 0; k < 3; k++) {
      float v = c->mins[3 * j + k] + c->maxs[3 * j + k];
      if (v < cmn[k]) cmn[k] = v;
      if (v > cmx[k]) cmx[k] = v;
    }
  }
  int ax = 0;
  for (int k = 1; k < 3; k++)
    if (cmx[k] - cmn[k] > cmx[ax] - cmn[ax]) ax = k;
  c->axis = ax;
  sort_ctx = c;
  qsort(c->order + lo, (size_t)(hi - lo), sizeof(int32_t), cmp_centroid);
  return lo + (hi - lo) / 2;
}

static int build_node(build_ctx *c, int lo, int hi) {
  int ni = c->b->nodes.count;
  cw_array_push(&c->b->nodes, (vis_bvh_node){ 0 });
  int r[5];
  r[0] = lo;
  r[4] = hi;
  r[2] = split(c, lo, hi);
  r[1] = r[2] - lo > 1 ? split(c, lo, r[2]) : r[2];
  r[3] = hi - r[2] > 1 ? split(c, r[2], hi) : hi;
  vis_bvh_node tmp = { 0 };
  tmp.lo = lo;
  tmp.hi = hi;
  for (int k = 0; k < 4; k++) {
    int a = r[k], z = r[k + 1];
    if (z <= a) {
      tmp.minx[k] = tmp.miny[k] = tmp.minz[k] = 1e30f;
      tmp.maxx[k] = tmp.maxy[k] = tmp.maxz[k] = -1e30f;
      continue;
    }
    float mn[3], mx[3];
    range_bounds(c, a, z, mn, mx);
    tmp.minx[k] = mn[0];
    tmp.miny[k] = mn[1];
    tmp.minz[k] = mn[2];
    tmp.maxx[k] = mx[0];
    tmp.maxy[k] = mx[1];
    tmp.maxz[k] = mx[2];
    if (z - a <= LEAF_MAX) {
      tmp.child[k] = -(a + 1);
      tmp.count[k] = z - a;
    } else {
      tmp.count[k] = 1;
      tmp.child[k] = build_node(c, a, z); /* may realloc nodes: tmp is a copy */
    }
  }
  c->b->nodes.data[ni] = tmp;
  return ni;
}

void cw__vis_bvh_free(vis_bvh *b) {
  cw_array_free(&b->nodes);
  cw_array_free(&b->prims);
  b->root = -1;
}

void cw__vis_bvh_build(vis_bvh *b, int n, const float *mins, const float *maxs, const int32_t *ids) {
  cw_array_clear(&b->nodes);
  cw_array_clear(&b->prims);
  b->root = -1;
  if (n <= 0) return;
  int32_t *order = cw_alloc(sizeof(int32_t) * (size_t)n, CW_MEM_RENDER);
  for (int i = 0; i < n; i++) order[i] = i;
  build_ctx c = { b, mins, maxs, order, 0 };
  b->root = build_node(&c, 0, n);
  range_bounds(&c, 0, n, b->rmin, b->rmax);
  cw_array_reserve(&b->prims, n);
  for (int i = 0; i < n; i++) b->prims.data[i] = ids[order[i]];
  b->prims.count = n;
  cw_free(order);
}

/* refit: children before parents (children always have larger indices) */
void cw__vis_bvh_refit(vis_bvh *b, const float *rmin, const float *rmax, int stride) {
  for (int ni = b->nodes.count - 1; ni >= 0; ni--) {
    vis_bvh_node *nd = &b->nodes.data[ni];
    for (int k = 0; k < 4; k++) {
      if (!nd->count[k]) continue;
      float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
      if (nd->child[k] >= 0) {
        const vis_bvh_node *ch = &b->nodes.data[nd->child[k]];
        for (int q = 0; q < 4; q++) {
          if (!ch->count[q]) continue;
          mn[0] = CW_MIN(mn[0], ch->minx[q]);
          mn[1] = CW_MIN(mn[1], ch->miny[q]);
          mn[2] = CW_MIN(mn[2], ch->minz[q]);
          mx[0] = CW_MAX(mx[0], ch->maxx[q]);
          mx[1] = CW_MAX(mx[1], ch->maxy[q]);
          mx[2] = CW_MAX(mx[2], ch->maxz[q]);
        }
      } else {
        int first = -nd->child[k] - 1;
        for (int i = first; i < first + nd->count[k]; i++) {
          int id = b->prims.data[i];
          for (int a = 0; a < 3; a++) {
            mn[a] = CW_MIN(mn[a], rmin[(size_t)id * stride + a]);
            mx[a] = CW_MAX(mx[a], rmax[(size_t)id * stride + a]);
          }
        }
      }
      nd->minx[k] = mn[0];
      nd->miny[k] = mn[1];
      nd->minz[k] = mn[2];
      nd->maxx[k] = mx[0];
      nd->maxy[k] = mx[1];
      nd->maxz[k] = mx[2];
    }
  }
  if (b->root >= 0) {
    const vis_bvh_node *r = &b->nodes.data[b->root];
    for (int a = 0; a < 3; a++) {
      b->rmin[a] = 1e30f;
      b->rmax[a] = -1e30f;
    }
    for (int q = 0; q < 4; q++) {
      if (!r->count[q]) continue;
      b->rmin[0] = CW_MIN(b->rmin[0], r->minx[q]);
      b->rmin[1] = CW_MIN(b->rmin[1], r->miny[q]);
      b->rmin[2] = CW_MIN(b->rmin[2], r->minz[q]);
      b->rmax[0] = CW_MAX(b->rmax[0], r->maxx[q]);
      b->rmax[1] = CW_MAX(b->rmax[1], r->maxy[q]);
      b->rmax[2] = CW_MAX(b->rmax[2], r->maxz[q]);
    }
  }
}

/* ── cull ───────────────────────────────────────────────────────────── */

/* test a node's 4 children: out lane mask bits (1 = outside), in bits (1 = fully inside) */
static inline void test4(const vis_bvh_node *nd, const vis_frustum *f, const float *sw, int *out_mask, int *in_mask) {
  v4f mnx = ld4(nd->minx), mny = ld4(nd->miny), mnz = ld4(nd->minz);
  v4f mxx = ld4(nd->maxx), mxy = ld4(nd->maxy), mxz = ld4(nd->maxz);
  v4i out = { 0, 0, 0, 0 }, in = { -1, -1, -1, -1 };
  for (int p = 0; p < f->n; p++) {
    float nx = f->nx[p], ny = f->ny[p], nz = f->nz[p], d = f->d[p];
    float s = sw ? nx * sw[0] + ny * sw[1] + nz * sw[2] : 0;
    v4f fx = nx >= 0 ? mxx : mnx, fy = ny >= 0 ? mxy : mny, fz = nz >= 0 ? mxz : mnz;
    v4f qx = nx >= 0 ? mnx : mxx, qy = ny >= 0 ? mny : mxy, qz = nz >= 0 ? mnz : mxz;
    v4f far = fx * nx + fy * ny + fz * nz + (d + (s > 0 ? s : 0));
    v4f near = qx * nx + qy * ny + qz * nz + (d + (s < 0 ? s : 0));
    v4f zero = { 0, 0, 0, 0 };
    out |= (v4i)(far < zero);
    in &= (v4i)(near >= zero);
  }
  int om = 0, im = 0;
  for (int k = 0; k < 4; k++) {
    if (out[k] || !nd->count[k]) om |= 1 << k;
    if (in[k]) im |= 1 << k;
  }
  *out_mask = om;
  *in_mask = im;
}

static void mark_range(const vis_bvh *b, int lo, int hi, uint8_t *out) {
  for (int i = lo; i < hi; i++) out[b->prims.data[i]] = 2;
}

typedef struct cull_job {
  const vis_bvh *b;
  const vis_frustum *f;
  const float *sweep;
  uint8_t *out;
  const int32_t *frontier;
  int *visited;
} cull_job;

static int walk(const vis_bvh *b, const vis_frustum *f, const float *sw, uint8_t *out, int start) {
  int stack[256], sp = 0, visited = 0;
  stack[sp++] = start;
  while (sp) {
    const vis_bvh_node *nd = &b->nodes.data[stack[--sp]];
    visited++;
    int om, im;
    test4(nd, f, sw, &om, &im);
    for (int k = 0; k < 4; k++) {
      if (om & (1 << k)) continue;
      bool inside = (im >> k) & 1;
      if (nd->child[k] >= 0) {
        const vis_bvh_node *ch = &b->nodes.data[nd->child[k]];
        if (inside) mark_range(b, ch->lo, ch->hi, out);
        else if (sp < 256) stack[sp++] = nd->child[k];
        else mark_range(b, ch->lo, ch->hi, out); /* never with a sane depth: be conservative */
      } else {
        int first = -nd->child[k] - 1;
        for (int i = first; i < first + nd->count[k]; i++) out[b->prims.data[i]] = inside ? 2 : 1;
      }
    }
  }
  return visited;
}

static void cull_range(int begin, int end, int worker, void *ctx) {
  (void)worker;
  cull_job *j = ctx;
  for (int i = begin; i < end; i++) j->visited[i] = walk(j->b, j->f, j->sweep, j->out, j->frontier[i]);
}

int cw__vis_bvh_cull(const vis_bvh *b, const vis_frustum *f, const float *sweep, uint8_t *out) {
  if (b->root < 0) return 0;
  int rt = cw__vis_box_test(f, b->rmin, b->rmax, sweep);
  if (rt == 0) return 1;
  if (rt == 2) {
    mark_range(b, 0, b->prims.count, out);
    return 1;
  }
  /* breadth first until there are enough subtrees for the pool */
  enum { WANT = 64 };
  int32_t *front = cw_frame_alloc(sizeof(int32_t) * WANT * 4), nf = 0;
  int32_t *next = cw_frame_alloc(sizeof(int32_t) * WANT * 4);
  front[nf++] = b->root;
  int visited = 0;
  while (nf > 0 && nf < WANT) {
    int nn = 0;
    for (int i = 0; i < nf; i++) {
      const vis_bvh_node *nd = &b->nodes.data[front[i]];
      visited++;
      int om, im;
      test4(nd, f, sweep, &om, &im);
      for (int k = 0; k < 4; k++) {
        if (om & (1 << k)) continue;
        bool inside = (im >> k) & 1;
        if (nd->child[k] >= 0) {
          const vis_bvh_node *ch = &b->nodes.data[nd->child[k]];
          if (inside) mark_range(b, ch->lo, ch->hi, out);
          else next[nn++] = nd->child[k];
        } else {
          int first = -nd->child[k] - 1;
          for (int q = first; q < first + nd->count[k]; q++) out[b->prims.data[q]] = inside ? 2 : 1;
        }
      }
    }
    int32_t *t = front;
    front = next;
    next = t;
    nf = nn;
  }
  if (nf > 0) {
    int *vis = cw_frame_alloc(sizeof(int) * (size_t)nf);
    cull_job j = { b, f, sweep, out, front, vis };
    cw_parallel_for(nf, 4, cull_range, &j);
    for (int i = 0; i < nf; i++) visited += vis[i];
  }
  return visited;
}
