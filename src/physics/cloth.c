/* XPBD cloth and ropes beside a Box3D world (cartwheel/cloth.h).
 *
 * Algorithms: extended position-based dynamics with one iteration per
 * substep (Macklin, Mueller, Chentanez: "XPBD", MIG 2016; Macklin et al.:
 * "Small Steps in Physics Simulation", SCA 2019), long-range attachments
 * (Kim, Chentanez, Mueller: "Long Range Attachments", SCA 2012), distance
 * bending and the spatial hash for self-collision as in Ten Minute Physics
 * (Matthias Mueller, MIT), a partition of the constraints for a parallel solve in the spirit of
 * Jolt Physics' soft bodies (Jorrit Rouwe, MIT) and Box2D v3's graph.
 *
 * Determinism: simulation values use only + - * / sqrt and cartwheel/math.h.
 * Every parallel pass writes per particle (or per pair of particle blocks,
 * which share no particle), so results do not depend on the thread count.
 * Reductions (hash, stats, impulses on bodies) run serially in index order. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/cloth.h"
#include "cartwheel/profile.h"
#include "cartwheel/jobs.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "physics3d_internal.h"

/* ── small vector maths (simulation: plain + - * / sqrt) ──────────────── */

typedef t3_vec3 V;
static inline V vadd(V a, V b) { return (V){ a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline V vsub(V a, V b) { return (V){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline V vmul(V a, float s) { return (V){ a.x * s, a.y * s, a.z * s }; }
static inline float vdot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V vcross(V a, V b) { return (V){ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static inline float vlen(V a) { return sqrtf(vdot(a, a)); }
static inline V vlerp(V a, V b, float t) { return (V){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t }; }

/* rotate v by unit quaternion q (t3 layout x, y, z, w) */
static inline V qrot(t3_quat q, V v) {
  V u = { q.x, q.y, q.z };
  V t = vmul(vcross(u, v), 2);
  return vadd(vadd(v, vmul(t, q.w)), vcross(u, t));
}
static inline V qrot_inv(t3_quat q, V v) { return qrot((t3_quat){ -q.x, -q.y, -q.z, q.w }, v); }
/* normalized lerp (shortest arc), as cw_xform3_lerp; plain maths */
static cw_xform3 xlerp(cw_xform3 a, cw_xform3 b, float t) {
  cw_xform3 r;
  r.p = vlerp(a.p, b.p, t);
  float d = a.q.x * b.q.x + a.q.y * b.q.y + a.q.z * b.q.z + a.q.w * b.q.w;
  float s = d < 0 ? -1.0f : 1.0f;
  t3_quat q = { a.q.x + (s * b.q.x - a.q.x) * t, a.q.y + (s * b.q.y - a.q.y) * t, a.q.z + (s * b.q.z - a.q.z) * t,
                a.q.w + (s * b.q.w - a.q.w) * t };
  float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (l > 0) q = (t3_quat){ q.x / l, q.y / l, q.z / l, q.w / l };
  r.q = q;
  return r;
}
static inline V xpoint(cw_xform3 x, V local) { return vadd(x.p, qrot(x.q, local)); }
static inline V xlocal(cw_xform3 x, V world) { return qrot_inv(x.q, vsub(world, x.p)); }

/* ── records ──────────────────────────────────────────────────────────── */

typedef enum { SH_SPHERE, SH_CAPSULE, SH_HULL } shape_kind;

typedef struct cshape {
  shape_kind kind;
  b3BodyId body;
  bool dynamic;
  float friction;
  cw_xform3 from, to;    /* the body's pose before and after the world step */
  V a, b;                /* sphere centre / capsule centres (body local) */
  float radius;
  const b3Plane *planes; /* hull planes (body local), owned by Box3D */
  int plane_count;
  V bound_c;             /* bounding sphere (body local): centre and radius */
  float bound_r;
  cw_xform3 pose, prev;  /* this substep's pose and the last one's (set before the per-particle pass) */
  V world_c;             /* bound_c at pose */
} cshape;

typedef struct attach {
  int vertex;
  cw_body3d body;
  V local;
  float force; /* N: the last step's pull on a dynamic body */
} attach;

typedef struct cw__cloth {
  cw_world3d world;
  int n, ntri, nedge;    /* nedge: stretch constraints (edges) */
  /* particles */
  V *x, *p, *v, *xt;     /* position, substep start, velocity, position at the tick's start */
  float *w;              /* inverse mass now (0 = pinned or attached to a non-dynamic body) */
  float *w0;             /* inverse mass free */
  float *mass;           /* kg (0 for a pinned scale) */
  float *area;           /* m^2 per particle (wind) */
  uint8_t *pinned, *attached, *taut;
  V *pin_from, *pin_to;
  V *offset;             /* attached: where the solve moved it from the body point */
  float *maxd;           /* skin max distance (NULL = none) */
  V *skin_from, *skin_to;
  bool skin_set;
  V *nrm;                /* simulation normals (wind) */
  V *wv;                 /* the wind at each particle this tick */
  V *dx;                 /* scratch: self-collision corrections */
  int *coll;             /* scratch: the shape each particle last touched (two_way), -1 none */
  V *coll_d;             /* scratch: its push this step (two_way) */
  /* mesh */
  uint32_t *tri;
  t3_vec2 *uv;
  int *vt_start, *vt_tri; /* vertex -> triangles (CSR) */
  int *vv_start, *vv_n;  /* vertex -> edge neighbours (CSR, for tethers) */
  float *vv_len;
  /* distance constraints, sorted by group (partition below); edge_slot maps edge -> slot */
  int ncon, ncol;        /* ncol: groups (phase A pairs, phase B pairs, serial) */
  int bs, na, nb;        /* particles per block, phase A and phase B group counts */
  int *col_start;        /* ncol + 1: each group's constraints */
  V *xa;                 /* tether anchors' positions, snapshot before phase B */
  int32_t *anchors;      /* the distinct tether anchors */
  int nanchors;
  int32_t *ci0, *ci1;
  float *crest, *ccomp;
  float *cinv;           /* 1 / (w0 + w1 + compliance / h^2), set at each step's start */
  int32_t *edge_slot;    /* edge index -> constraint slot */
  /* tethers: free particle -> anchor (nearest by rest distance along the mesh) */
  int ntether;
  int32_t *tv, *ta;
  float *tlen;
  int *tether_of;        /* per particle, its tether, -1 */
  bool tethers_dirty;
  bool in_batch;           /* listed in the cw_cloth_step_many call under way */
  bool inline_run;         /* steps on one thread: its cloth is one job of a batch */
  /* anchor-to-anchor tethers between body attachments (solved serially) */
  int natether;
  int32_t *av, *aa;
  float *alen;
  CW_ARRAY(attach) attachments;
  /* settings */
  cw_cloth_def def;
  float stretch_c, bend_c;
  float gscale;
  V wind;
  float drag, lift, turbulence;
  /* per step */
  CW_ARRAY(cshape) shapes;
  uint32_t hash;
  int32_t steps;
  float time;
  float step_us;
  /* rendering */
  float tube_radius;
  int tube_sides;
} cw__cloth;

/* wall clock for the step timing (not simulation) */
static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static cw_pool g_cloths;
static bool g_init;

static cw__cloth *rec(cw_cloth c) {
  if (!g_init || !c.id) return NULL;
  return cw_pool_get(&g_cloths, c.id);
}

cw_cloth_def cw_cloth_default_def(void) {
  return (cw_cloth_def){ .mass = 1, .bend_compliance = 0.01f, .tether_slack = 1.02f, .damping = 0.1f,
                         .thickness = 0.02f, .friction = 0.4f, .substeps = 8, .collide_mask = CW_PHYS_ALL_LAYERS,
                         .max_speed = 50 };
}

cw_rope_def cw_rope_default_def(void) {
  return (cw_rope_def){ .end = { 0, -1, 0 }, .segments = 16, .mass = 1, .bend_compliance = 1, .damping = 0.1f,
                        .thickness = 0.03f, .friction = 0.4f, .substeps = 16, .pin_start = true,
                        .collide_mask = CW_PHYS_ALL_LAYERS };
}

/* ── creation ─────────────────────────────────────────────────────────── */

typedef struct epair {
  uint32_t a, b;
  int32_t tri;   /* the triangle it came from (-1: a def edge) */
  uint32_t opp;  /* the triangle's third vertex */
} epair;

static int epair_cmp(const void *pa, const void *pb) {
  const epair *a = pa, *b = pb;
  if (a->a != b->a) return a->a < b->a ? -1 : 1;
  if (a->b != b->b) return a->b < b->b ? -1 : 1;
  if (a->tri != b->tri) return a->tri < b->tri ? -1 : 1;
  return 0;
}

typedef struct upair {
  uint32_t a, b;
} upair;
static int upair_cmp(const void *pa, const void *pb) {
  const upair *a = pa, *b = pb;
  if (a->a != b->a) return a->a < b->a ? -1 : 1;
  if (a->b != b->b) return a->b < b->b ? -1 : 1;
  return 0;
}

static void *zalloc(size_t n, size_t sz) { return cw_calloc(n ? n : 1, sz, CW_MEM_PHYSICS); }

static void cloth_free(cw__cloth *c) {
  void *ptrs[] = { c->x, c->p, c->v, c->xt, c->w, c->w0, c->mass, c->area, c->pinned, c->attached, c->taut,
                   c->pin_from, c->pin_to, c->offset, c->maxd, c->skin_from, c->skin_to, c->nrm, c->dx, c->coll,
                   c->coll_d, c->wv, c->tri, c->uv, c->vt_start, c->vt_tri, c->vv_start, c->vv_n, c->vv_len, c->col_start,
                   c->ci0, c->ci1, c->crest, c->ccomp, c->cinv, c->edge_slot, c->tv, c->ta, c->tlen,
                   c->tether_of, c->av, c->aa, c->alen, c->xa, c->anchors };
  for (size_t i = 0; i < CW_ARRAY_LEN(ptrs); i++) cw_free(ptrs[i]);
  cw_array_free(&c->attachments);
  cw_array_free(&c->shapes);
}

/* The parallel layout. Particles are cut into blocks of `bs` consecutive indices (bs at least the
 * widest index span of any constraint, so a constraint touches at most two neighbouring blocks).
 * Phase A runs pairs of blocks {2p, 2p+1}: each takes the constraints inside its pair. Phase B
 * runs the pairs {2g-1, 2g}: the constraints across the odd boundaries. Pairs of one phase share
 * no particle, so each pair is one job solved Gauss-Seidel in creation order, and the result does
 * not depend on the thread count. Constraints spanning farther (unstructured meshes) run serially.
 * Groups: [0, na) phase A, [na, na + nb) phase B, then the serial leftovers. */
static void partition(cw__cloth *c, int32_t *i0, int32_t *i1, float *rest, float *comp, int count, int nedge) {
  int span = 1;
  for (int k = 0; k < count; k++) {
    int d = i0[k] > i1[k] ? i0[k] - i1[k] : i1[k] - i0[k];
    if (d > span) span = d;
  }
  int bs = span < 128 ? 128 : span;
  if (bs > 4096) bs = 4096; /* wider spans go serial */
  int nblocks = (c->n + bs - 1) / bs;
  int na = (nblocks + 1) / 2, nb = nblocks / 2 + 1;
  int ng = na + nb + 1;
  c->bs = bs, c->na = na, c->nb = nb, c->ncol = ng;
  int *grp = zalloc((size_t)count, sizeof(int));
  c->col_start = zalloc((size_t)ng + 1, sizeof(int));
  for (int k = 0; k < count; k++) {
    int ba = i0[k] / bs, bb = i1[k] / bs, lo = ba < bb ? ba : bb, hi = ba < bb ? bb : ba;
    int g;
    if (hi - lo > 1) g = na + nb;               /* serial */
    else if (lo == hi || !(lo & 1)) g = lo / 2; /* inside an A pair */
    else g = na + (lo + 1) / 2;                 /* across an odd boundary: B */
    grp[k] = g;
    c->col_start[g + 1]++;
  }
  for (int i = 0; i < ng; i++) c->col_start[i + 1] += c->col_start[i];
  int *fill = zalloc((size_t)ng, sizeof(int));
  c->ci0 = zalloc((size_t)count, sizeof(int32_t));
  c->ci1 = zalloc((size_t)count, sizeof(int32_t));
  c->crest = zalloc((size_t)count, sizeof(float));
  c->ccomp = zalloc((size_t)count, sizeof(float));
  c->cinv = zalloc((size_t)count, sizeof(float));
  c->edge_slot = zalloc((size_t)nedge, sizeof(int32_t));
  for (int k = 0; k < count; k++) {
    int s = c->col_start[grp[k]] + fill[grp[k]]++;
    c->ci0[s] = i0[k], c->ci1[s] = i1[k], c->crest[s] = rest[k], c->ccomp[s] = comp[k];
    if (k < nedge) c->edge_slot[k] = s;
  }
  /* inside each group: order the constraints by a greedy colouring (lowest colour neither particle
   * has yet, in creation order), so neighbouring constraints in the loop share no particle and the
   * CPU overlaps them instead of waiting on each one's result */
  {
    uint64_t *used = zalloc((size_t)c->n, sizeof(uint64_t));
    int maxg = 0;
    for (int g = 0; g < ng; g++)
      if (c->col_start[g + 1] - c->col_start[g] > maxg) maxg = c->col_start[g + 1] - c->col_start[g];
    int *colr = zalloc((size_t)maxg + 1, sizeof(int)), *order = zalloc((size_t)maxg + 1, sizeof(int));
    int32_t *t0 = zalloc((size_t)maxg + 1, sizeof(int32_t)), *t1 = zalloc((size_t)maxg + 1, sizeof(int32_t));
    float *tr = zalloc((size_t)maxg + 1, sizeof(float)), *tc = zalloc((size_t)maxg + 1, sizeof(float));
    int *slot_of = zalloc((size_t)count, sizeof(int)); /* old slot -> new slot */
    for (int g = 0; g < ng; g++) {
      int b = c->col_start[g], m = c->col_start[g + 1] - b, nc = 0;
      for (int k = 0; k < m; k++) used[c->ci0[b + k]] = used[c->ci1[b + k]] = 0;
      for (int k = 0; k < m; k++) {
        uint64_t u = used[c->ci0[b + k]] | used[c->ci1[b + k]];
        int cc = 0;
        while (cc < 63 && (u >> cc & 1)) cc++;
        colr[k] = cc;
        if (cc < 63) used[c->ci0[b + k]] |= 1ull << cc, used[c->ci1[b + k]] |= 1ull << cc;
        if (cc + 1 > nc) nc = cc + 1;
      }
      int o = 0;
      for (int cc = 0; cc < nc; cc++)
        for (int k = 0; k < m; k++)
          if (colr[k] == cc) order[o++] = k;
      for (int k = 0; k < m; k++) t0[k] = c->ci0[b + k], t1[k] = c->ci1[b + k], tr[k] = c->crest[b + k], tc[k] = c->ccomp[b + k];
      for (int k = 0; k < m; k++) {
        int src = order[k];
        c->ci0[b + k] = t0[src], c->ci1[b + k] = t1[src], c->crest[b + k] = tr[src], c->ccomp[b + k] = tc[src];
        slot_of[b + src] = b + k;
      }
    }
    for (int e = 0; e < nedge; e++) c->edge_slot[e] = slot_of[c->edge_slot[e]];
    cw_free(used), cw_free(colr), cw_free(order), cw_free(t0), cw_free(t1), cw_free(tr), cw_free(tc), cw_free(slot_of);
  }
  c->ncon = count;
  cw_free(grp), cw_free(fill);
}

/* tethers: Dijkstra over the edges from every anchor at once (ties by vertex index) */
typedef struct hnode {
  float d;
  int v;
} hnode;
static bool hless(hnode a, hnode b) { return a.d < b.d || (a.d == b.d && a.v < b.v); }
static void hpush(hnode *h, int *n, hnode x) {
  int i = (*n)++;
  while (i > 0) {
    int p = (i - 1) / 2;
    if (!hless(x, h[p])) break;
    h[i] = h[p], i = p;
  }
  h[i] = x;
}
static hnode hpop(hnode *h, int *n) {
  hnode top = h[0], x = h[--(*n)];
  int i = 0;
  for (;;) {
    int l = 2 * i + 1, r = l + 1, m = i;
    hnode best = x;
    if (l < *n && hless(h[l], best)) m = l, best = h[l];
    if (r < *n && hless(h[r], best)) m = r;
    if (m == i) break;
    h[i] = h[m], i = m;
  }
  if (*n) h[i] = x;
  return top;
}

/* distances from the anchors (is_src) to every vertex; src_of[v] = the anchor it came from */
static void dijkstra(const cw__cloth *c, const uint8_t *is_src, float *dist, int *src_of, hnode *heap) {
  int hn = 0;
  for (int v = 0; v < c->n; v++) {
    dist[v] = -1, src_of[v] = -1;
    if (is_src[v]) dist[v] = 0, src_of[v] = v, hpush(heap, &hn, (hnode){ 0, v });
  }
  while (hn) {
    hnode t = hpop(heap, &hn);
    if (t.d > dist[t.v]) continue;
    for (int k = c->vv_start[t.v]; k < c->vv_start[t.v + 1]; k++) {
      int u = c->vv_n[k];
      float nd = t.d + c->vv_len[k];
      if (dist[u] < 0 || nd < dist[u] || (nd == dist[u] && src_of[t.v] < src_of[u])) {
        dist[u] = nd, src_of[u] = src_of[t.v];
        hpush(heap, &hn, (hnode){ nd, u });
      }
    }
  }
}

static bool dynamic_anchor(const cw__cloth *c, int v) { return c->attached[v] && c->w[v] > 0; }

static void build_tethers(cw__cloth *c) {
  c->tethers_dirty = false;
  c->ntether = c->natether = c->nanchors = 0;
  for (int v = 0; v < c->n; v++) c->tether_of[v] = -1;
  if (c->def.tether_slack <= 0) return;
  uint8_t *src = zalloc((size_t)c->n, 1);
  float *dist = zalloc((size_t)c->n, sizeof(float));
  int *from = zalloc((size_t)c->n, sizeof(int));
  hnode *heap = zalloc((size_t)c->vv_start[c->n] + (size_t)c->n + 1, sizeof(hnode));
  int anchors = 0;
  for (int v = 0; v < c->n; v++) {
    src[v] = c->pinned[v] || c->attached[v];
    anchors += src[v];
  }
  if (anchors) {
    dijkstra(c, src, dist, from, heap);
    for (int v = 0; v < c->n; v++) {
      if (src[v] || from[v] < 0 || c->w[v] <= 0) continue;
      int k = c->ntether++;
      c->tv[k] = v, c->ta[k] = from[v], c->tlen[k] = dist[v] * c->def.tether_slack;
      c->tether_of[v] = k;
    }
    /* the distinct anchors, in index order (snapshotted each substep) */
    uint8_t *seen = zalloc((size_t)c->n, 1);
    for (int k = 0; k < c->ntether; k++) seen[c->ta[k]] = 1;
    for (int v = 0; v < c->n; v++)
      if (seen[v]) c->anchors[c->nanchors++] = v;
    cw_free(seen);
    /* each dynamic attachment: a tether to the nearest other anchor */
    for (int a = 0; a < c->n && anchors > 1; a++) {
      if (!dynamic_anchor(c, a)) continue;
      src[a] = 0;
      dijkstra(c, src, dist, from, heap);
      src[a] = 1;
      if (from[a] < 0) continue;
      int k = c->natether++;
      c->av = cw_realloc(c->av, sizeof(int32_t) * (size_t)c->natether, CW_MEM_PHYSICS);
      c->aa = cw_realloc(c->aa, sizeof(int32_t) * (size_t)c->natether, CW_MEM_PHYSICS);
      c->alen = cw_realloc(c->alen, sizeof(float) * (size_t)c->natether, CW_MEM_PHYSICS);
      c->av[k] = a, c->aa[k] = from[a], c->alen[k] = dist[a] * c->def.tether_slack;
    }
  }
  cw_free(src), cw_free(dist), cw_free(from), cw_free(heap);
}

static void compute_normals(const cw__cloth *c, const V *x, V *out, int begin, int end) {
  for (int v = begin; v < end; v++) {
    V s = { 0, 0, 0 };
    for (int k = c->vt_start[v]; k < c->vt_start[v + 1]; k++) {
      const uint32_t *t = &c->tri[3 * c->vt_tri[k]];
      s = vadd(s, vcross(vsub(x[t[1]], x[t[0]]), vsub(x[t[2]], x[t[0]])));
    }
    float l = vlen(s);
    out[v] = l > 1e-12f ? vmul(s, 1 / l) : (V){ 0, 1, 0 };
  }
}

/* rope "normals": the tangent along the chain */
static void compute_tangents(const cw__cloth *c, const V *x, V *out) {
  for (int v = 0; v < c->n; v++) {
    V s = { 0, 0, 0 };
    for (int k = c->vv_start[v]; k < c->vv_start[v + 1]; k++) {
      int u = c->vv_n[k];
      V d = vsub(x[u], x[v]);
      s = u > v ? vadd(s, d) : vsub(s, d);
    }
    float l = vlen(s);
    out[v] = l > 1e-12f ? vmul(s, 1 / l) : (V){ 0, 1, 0 };
  }
}

static cw_cloth create(cw_world3d w, const cw_cloth_def *d, const upair *bends_extra, int nbends_extra) {
  if (!cw_world3d_valid(w)) {
    cw_error("cloth: invalid world (create the cw_world3d first)");
    return (cw_cloth){ 0 };
  }
  if (!d || d->vertex_count < 2 || !d->positions || (d->triangle_count <= 0 && d->edge_count <= 0)) {
    cw_error("cloth: needs at least 2 positions and some triangles or edges");
    return (cw_cloth){ 0 };
  }
  int n = d->vertex_count;
  for (int i = 0; i < d->triangle_count * 3; i++)
    if (d->triangles[i] >= (uint32_t)n) {
      cw_error("cloth: triangle index %u out of range (%d vertices)", d->triangles[i], n);
      return (cw_cloth){ 0 };
    }
  for (int i = 0; i < d->edge_count * 2; i++)
    if (d->edges[i] >= (uint32_t)n) {
      cw_error("cloth: edge index %u out of range (%d vertices)", d->edges[i], n);
      return (cw_cloth){ 0 };
    }
  if (!g_init) {
    cw_pool_init(&g_cloths, sizeof(cw__cloth), CW_MEM_PHYSICS);
    g_init = true;
  }
  uint64_t id;
  cw__cloth *c = cw_pool_alloc(&g_cloths, &id);
  memset(c, 0, sizeof *c);
  c->world = w;
  c->def = *d;
  c->def.positions = NULL, c->def.triangles = NULL, c->def.edges = NULL, c->def.uvs = NULL;
  c->def.inv_mass = NULL, c->def.max_distance = NULL;
  if (c->def.substeps < 1) c->def.substeps = 1;
  if (c->def.max_speed <= 0) c->def.max_speed = 50;
  if (c->def.thickness < 0) c->def.thickness = 0;
  c->n = n, c->ntri = d->triangle_count;
  c->gscale = 1;
  c->drag = 1, c->lift = 0.3f;
  size_t N = (size_t)n;
  c->x = zalloc(N, sizeof(V)), c->p = zalloc(N, sizeof(V)), c->v = zalloc(N, sizeof(V)), c->xt = zalloc(N, sizeof(V));
  c->w = zalloc(N, sizeof(float)), c->w0 = zalloc(N, sizeof(float)), c->mass = zalloc(N, sizeof(float));
  c->area = zalloc(N, sizeof(float));
  c->pinned = zalloc(N, 1), c->attached = zalloc(N, 1), c->taut = zalloc(N, 1);
  c->pin_from = zalloc(N, sizeof(V)), c->pin_to = zalloc(N, sizeof(V)), c->offset = zalloc(N, sizeof(V));
  c->nrm = zalloc(N, sizeof(V)), c->wv = zalloc(N, sizeof(V)), c->dx = zalloc(N, sizeof(V)), c->coll = zalloc(N, sizeof(int));
  c->coll_d = zalloc(N, sizeof(V));
  c->tether_of = zalloc(N, sizeof(int));
  memcpy(c->x, d->positions, sizeof(V) * N);
  memcpy(c->p, d->positions, sizeof(V) * N);
  memcpy(c->xt, d->positions, sizeof(V) * N);
  memcpy(c->pin_from, d->positions, sizeof(V) * N);
  memcpy(c->pin_to, d->positions, sizeof(V) * N);
  if (d->max_distance) {
    c->maxd = zalloc(N, sizeof(float));
    memcpy(c->maxd, d->max_distance, sizeof(float) * N);
    c->skin_from = zalloc(N, sizeof(V)), c->skin_to = zalloc(N, sizeof(V));
    memcpy(c->skin_from, d->positions, sizeof(V) * N);
    memcpy(c->skin_to, d->positions, sizeof(V) * N);
  }
  if (c->ntri) {
    c->tri = zalloc((size_t)c->ntri * 3, sizeof(uint32_t));
    memcpy(c->tri, d->triangles, sizeof(uint32_t) * 3 * (size_t)c->ntri);
  }
  if (d->uvs) {
    c->uv = zalloc(N, sizeof(t3_vec2));
    memcpy(c->uv, d->uvs, sizeof(t3_vec2) * N);
  }

  /* areas (a third of each triangle per corner); vertex -> triangle CSR */
  c->vt_start = zalloc(N + 1, sizeof(int));
  float total = 0;
  for (int t = 0; t < c->ntri; t++) {
    const uint32_t *k = &c->tri[3 * t];
    float a = 0.5f * vlen(vcross(vsub(c->x[k[1]], c->x[k[0]]), vsub(c->x[k[2]], c->x[k[0]])));
    for (int j = 0; j < 3; j++) c->area[k[j]] += a / 3, c->vt_start[k[j] + 1]++;
    total += a;
  }
  for (int v = 0; v < n; v++) c->vt_start[v + 1] += c->vt_start[v];
  c->vt_tri = zalloc((size_t)c->ntri * 3, sizeof(int));
  {
    int *fill = zalloc(N, sizeof(int));
    for (int t = 0; t < c->ntri; t++)
      for (int j = 0; j < 3; j++) {
        uint32_t v = c->tri[3 * t + j];
        c->vt_tri[c->vt_start[v] + fill[v]++] = t;
      }
    cw_free(fill);
  }

  /* edges: every triangle's three plus the def's, unique, sorted */
  int ne_raw = c->ntri * 3 + d->edge_count;
  epair *ep = zalloc((size_t)ne_raw, sizeof(epair));
  int k = 0;
  for (int t = 0; t < c->ntri; t++)
    for (int j = 0; j < 3; j++) {
      uint32_t a = c->tri[3 * t + j], b = c->tri[3 * t + (j + 1) % 3], o = c->tri[3 * t + (j + 2) % 3];
      if (a == b) continue;
      ep[k++] = (epair){ a < b ? a : b, a < b ? b : a, t, o };
    }
  for (int e = 0; e < d->edge_count; e++) {
    uint32_t a = d->edges[2 * e], b = d->edges[2 * e + 1];
    if (a == b) continue;
    ep[k++] = (epair){ a < b ? a : b, a < b ? b : a, -1, 0 };
  }
  qsort(ep, (size_t)k, sizeof(epair), epair_cmp);
  /* unique edges, and bend pairs: the opposite corners of two triangles sharing an edge */
  upair *edges = zalloc((size_t)k, sizeof(upair));
  upair *bends = zalloc((size_t)k + (size_t)nbends_extra + 1, sizeof(upair));
  int ne = 0, nb = 0;
  for (int i = 0; i < k; i++) {
    if (i == 0 || ep[i].a != ep[i - 1].a || ep[i].b != ep[i - 1].b) edges[ne++] = (upair){ ep[i].a, ep[i].b };
    if (ep[i].tri >= 0)
      for (int j = i + 1; j < k && ep[j].a == ep[i].a && ep[j].b == ep[i].b; j++)
        if (ep[j].tri >= 0 && ep[j].opp != ep[i].opp) {
          uint32_t a = ep[i].opp, b = ep[j].opp;
          bends[nb++] = (upair){ a < b ? a : b, a < b ? b : a };
        }
  }
  for (int i = 0; i < nbends_extra; i++) bends[nb++] = bends_extra[i];
  cw_free(ep);
  c->nedge = ne;
  /* bends that duplicate an edge or each other go */
  qsort(bends, (size_t)nb, sizeof(upair), upair_cmp);
  {
    int m = 0;
    for (int i = 0; i < nb; i++) {
      if (m && bends[m - 1].a == bends[i].a && bends[m - 1].b == bends[i].b) continue;
      if (bsearch(&bends[i], edges, (size_t)ne, sizeof(upair), upair_cmp)) continue;
      bends[m++] = bends[i];
    }
    nb = m;
  }
  if (d->bend_compliance < 0) nb = 0;

  /* vertex -> edge neighbours (tethers, rope tangents) */
  c->vv_start = zalloc(N + 1, sizeof(int));
  for (int e = 0; e < ne; e++) c->vv_start[edges[e].a + 1]++, c->vv_start[edges[e].b + 1]++;
  for (int v = 0; v < n; v++) c->vv_start[v + 1] += c->vv_start[v];
  c->vv_n = zalloc((size_t)ne * 2, sizeof(int));
  c->vv_len = zalloc((size_t)ne * 2, sizeof(float));
  {
    int *fill = zalloc(N, sizeof(int));
    for (int e = 0; e < ne; e++) {
      uint32_t a = edges[e].a, b = edges[e].b;
      float l = vlen(vsub(c->x[a], c->x[b]));
      int sa = c->vv_start[a] + fill[a]++, sb = c->vv_start[b] + fill[b]++;
      c->vv_n[sa] = (int)b, c->vv_len[sa] = l;
      c->vv_n[sb] = (int)a, c->vv_len[sb] = l;
    }
    cw_free(fill);
  }

  /* masses: by area for cloth, evenly for line meshes */
  float mtot = d->mass > 0 ? d->mass : 1;
  for (int v = 0; v < n; v++) {
    float m = total > 0 && c->ntri ? mtot * c->area[v] / total : mtot / (float)n;
    if (m <= 0) m = mtot / (float)n;
    float scale = d->inv_mass ? d->inv_mass[v] : 1;
    c->mass[v] = m;
    c->w0[v] = 1 / m;
    c->w[v] = scale > 0 ? scale / m : 0;
    c->pinned[v] = scale <= 0;
    if (!c->ntri) c->area[v] = 0;
  }
  if (!c->ntri) /* rope: the side area of its segments */
    for (int e = 0; e < ne; e++) {
      float a = vlen(vsub(c->x[edges[e].a], c->x[edges[e].b])) * 2 * (c->def.thickness > 0 ? c->def.thickness : 0.01f);
      c->area[edges[e].a] += a / 2, c->area[edges[e].b] += a / 2;
    }

  /* constraints: stretch then bend, partitioned for the parallel solve */
  int nc = ne + nb;
  int32_t *i0 = zalloc((size_t)nc, sizeof(int32_t)), *i1 = zalloc((size_t)nc, sizeof(int32_t));
  float *rest = zalloc((size_t)nc, sizeof(float)), *comp = zalloc((size_t)nc, sizeof(float));
  for (int e = 0; e < ne; e++) {
    i0[e] = (int32_t)edges[e].a, i1[e] = (int32_t)edges[e].b;
    rest[e] = vlen(vsub(c->x[edges[e].a], c->x[edges[e].b]));
    comp[e] = d->stretch_compliance > 0 ? d->stretch_compliance : 0;
  }
  for (int b = 0; b < nb; b++) {
    i0[ne + b] = (int32_t)bends[b].a, i1[ne + b] = (int32_t)bends[b].b;
    rest[ne + b] = vlen(vsub(c->x[bends[b].a], c->x[bends[b].b]));
    comp[ne + b] = d->bend_compliance > 0 ? d->bend_compliance : 0;
  }
  partition(c, i0, i1, rest, comp, nc, ne);
  cw_free(i0), cw_free(i1), cw_free(rest), cw_free(comp), cw_free(edges), cw_free(bends);

  c->tv = zalloc(N, sizeof(int32_t)), c->ta = zalloc(N, sizeof(int32_t)), c->tlen = zalloc(N, sizeof(float));
  c->xa = zalloc(N, sizeof(V)), c->anchors = zalloc(N, sizeof(int32_t));
  build_tethers(c);
  if (c->ntri) compute_normals(c, c->x, c->nrm, 0, n);
  else compute_tangents(c, c->x, c->nrm);
  c->hash = CW_HASH32_INIT;
  return (cw_cloth){ id };
}

cw_cloth cw_cloth_create(cw_world3d w, const cw_cloth_def *def) { return create(w, def, NULL, 0); }

cw_cloth cw_cloth_create_grid(cw_world3d w, const cw_cloth_def *def, int nu, int nv, t3_vec3 origin, t3_vec3 u,
                              t3_vec3 v) {
  if (nu < 2 || nv < 2) {
    cw_error("cloth: a grid needs at least 2 x 2 vertices (got %d x %d)", nu, nv);
    return (cw_cloth){ 0 };
  }
  cw_cloth_def d = def ? *def : cw_cloth_default_def();
  int n = nu * nv, nt = (nu - 1) * (nv - 1) * 2;
  V *pos = zalloc((size_t)n, sizeof(V));
  t3_vec2 *uv = zalloc((size_t)n, sizeof(t3_vec2));
  uint32_t *tri = zalloc((size_t)nt * 3, sizeof(uint32_t));
  for (int j = 0; j < nv; j++)
    for (int i = 0; i < nu; i++) {
      float s = (float)i / (float)(nu - 1), t = (float)j / (float)(nv - 1);
      pos[i + j * nu] = vadd(origin, vadd(vmul(u, s), vmul(v, t)));
      uv[i + j * nu] = (t3_vec2){ s, t };
    }
  int k = 0;
  for (int j = 0; j + 1 < nv; j++)
    for (int i = 0; i + 1 < nu; i++) {
      uint32_t a = (uint32_t)(i + j * nu), b = a + 1, cc = a + (uint32_t)nu, dd = cc + 1;
      if ((i + j) & 1) {
        tri[k++] = a, tri[k++] = b, tri[k++] = dd;
        tri[k++] = a, tri[k++] = dd, tri[k++] = cc;
      } else {
        tri[k++] = a, tri[k++] = b, tri[k++] = cc;
        tri[k++] = b, tri[k++] = dd, tri[k++] = cc;
      }
    }
  d.positions = pos, d.vertex_count = n, d.triangles = tri, d.triangle_count = nt, d.uvs = uv;
  d.edges = NULL, d.edge_count = 0;
  cw_cloth c = create(w, &d, NULL, 0);
  cw_free(pos), cw_free(uv), cw_free(tri);
  return c;
}

cw_cloth cw_rope_create(cw_world3d w, const cw_rope_def *def) {
  cw_rope_def r = def ? *def : cw_rope_default_def();
  if (r.segments < 1) {
    cw_error("rope: needs at least 1 segment (got %d)", r.segments);
    return (cw_cloth){ 0 };
  }
  int n = r.segments + 1;
  V *pos = zalloc((size_t)n, sizeof(V));
  float *inv = zalloc((size_t)n, sizeof(float));
  uint32_t *edges = zalloc((size_t)r.segments * 2, sizeof(uint32_t));
  upair *bends = zalloc((size_t)n, sizeof(upair));
  V span = vsub(r.end, r.start);
  float straight = vlen(span), len = r.length > straight ? r.length : straight;
  /* sag: a parabola below the chord whose arc length is about `len` (s = L(1 + 8/3 (h/L)^2)) */
  float sag = straight > 0 && len > straight ? straight * sqrtf((len / straight - 1) * 3.0f / 8.0f) : 0;
  if (straight <= 0) span = (V){ 0, -len, 0 };
  for (int i = 0; i < n; i++) {
    float t = (float)i / (float)r.segments;
    pos[i] = vadd(r.start, vmul(span, t));
    pos[i].y -= sag * 4 * t * (1 - t);
    inv[i] = 1;
  }
  if (r.pin_start) inv[0] = 0;
  if (r.pin_end) inv[n - 1] = 0;
  for (int i = 0; i < r.segments; i++) edges[2 * i] = (uint32_t)i, edges[2 * i + 1] = (uint32_t)i + 1;
  int nb = 0;
  for (int i = 0; i + 2 < n; i++) bends[nb++] = (upair){ (uint32_t)i, (uint32_t)i + 2 };
  cw_cloth_def d = cw_cloth_default_def();
  d.positions = pos, d.vertex_count = n, d.edges = edges, d.edge_count = r.segments, d.inv_mass = inv;
  d.mass = r.mass, d.stretch_compliance = r.stretch_compliance, d.bend_compliance = r.bend_compliance;
  d.damping = r.damping, d.thickness = r.thickness, d.friction = r.friction, d.substeps = r.substeps;
  d.collide_mask = r.collide_mask, d.two_way = r.two_way, d.workers = r.workers;
  /* the rest length is the rope's, not the sagging curve's chords */
  cw_cloth c = create(w, &d, r.bend_compliance < 0 ? NULL : bends, r.bend_compliance < 0 ? 0 : nb);
  cw__cloth *cr = rec(c);
  if (cr) {
    float seg = len / (float)r.segments;
    for (int s = 0; s < cr->ncon; s++)
      cr->crest[s] = cr->ci1[s] - cr->ci0[s] == 1 || cr->ci0[s] - cr->ci1[s] == 1 ? seg : 2 * seg;
    for (int v = 0; v < n; v++)
      for (int kk = cr->vv_start[v]; kk < cr->vv_start[v + 1]; kk++) cr->vv_len[kk] = seg;
    build_tethers(cr);
  }
  cw_free(pos), cw_free(inv), cw_free(edges), cw_free(bends);
  return c;
}

void cw_cloth_destroy(cw_cloth c) {
  cw__cloth *r = rec(c);
  if (!r) return;
  cloth_free(r);
  cw_pool_release(&g_cloths, c.id);
}

bool cw_cloth_valid(cw_cloth c) {
  cw__cloth *r = rec(c);
  return r && cw_world3d_valid(r->world);
}

/* ── pins, attachments, skin ──────────────────────────────────────────── */

static bool vert_ok(const cw__cloth *c, int v) {
  if (v >= 0 && v < c->n) return true;
  cw_error("cloth: vertex %d out of range (%d vertices)", v, c->n);
  return false;
}

static void detach(cw__cloth *c, int v) {
  for (int i = 0; i < c->attachments.count; i++)
    if (c->attachments.data[i].vertex == v) {
      memmove(&c->attachments.data[i], &c->attachments.data[i + 1],
              sizeof(attach) * (size_t)(c->attachments.count - i - 1));
      c->attachments.count--;
      break;
    }
  c->attached[v] = 0;
}

void cw_cloth_pin(cw_cloth h, int vertex, bool pinned, t3_vec3 target) {
  cw__cloth *c = rec(h);
  if (!c || !vert_ok(c, vertex)) return;
  if (c->attached[vertex]) detach(c, vertex);
  c->pinned[vertex] = pinned;
  c->w[vertex] = pinned ? 0 : c->w0[vertex];
  c->pin_from[vertex] = c->x[vertex];
  c->pin_to[vertex] = pinned ? target : c->x[vertex];
  c->tethers_dirty = true;
}

void cw_cloth_set_pin_target(cw_cloth h, int vertex, t3_vec3 target) {
  cw__cloth *c = rec(h);
  if (!c || !vert_ok(c, vertex)) return;
  c->pin_to[vertex] = target;
}

void cw_cloth_attach_body(cw_cloth h, int vertex, cw_body3d b, t3_vec3 local) {
  cw__cloth *c = rec(h);
  if (!c || !vert_ok(c, vertex)) return;
  if (c->attached[vertex]) detach(c, vertex);
  c->tethers_dirty = true;
  if (!cw_body3d_valid(b)) {
    c->pinned[vertex] = 0;
    c->w[vertex] = c->w0[vertex];
    return;
  }
  cw_array_push(&c->attachments, (attach){ vertex, b, local });
  c->attached[vertex] = 1;
  c->pinned[vertex] = 0;
  c->offset[vertex] = (V){ 0, 0, 0 };
  /* a dynamic body lends the particle its own (translational) inverse mass */
  c->w[vertex] = cw_body3d_type(b) == CW_BODY_DYNAMIC ? b3Body_GetInverseMass(cw_body3d_raw(b)) : 0;
  V at = xpoint(cw_body3d_transform(b), local);
  c->x[vertex] = c->p[vertex] = c->xt[vertex] = at;
  c->v[vertex] = (V){ 0, 0, 0 };
}

void cw_cloth_set_skin_targets(cw_cloth h, const t3_vec3 *targets) {
  cw__cloth *c = rec(h);
  if (!c || !targets) return;
  if (!c->maxd) {
    cw_error("cloth: skin targets need cw_cloth_def.max_distance at creation");
    return;
  }
  if (!c->skin_set) memcpy(c->skin_from, targets, sizeof(V) * (size_t)c->n);
  memcpy(c->skin_to, targets, sizeof(V) * (size_t)c->n);
  c->skin_set = true;
}

void cw_cloth_teleport(cw_cloth h, cw_xform3 from, cw_xform3 to) {
  cw__cloth *c = rec(h);
  if (!c) return;
  for (int i = 0; i < c->n; i++) {
    V *arrs[] = { &c->x[i], &c->p[i], &c->xt[i], &c->pin_from[i], &c->pin_to[i] };
    for (size_t k = 0; k < CW_ARRAY_LEN(arrs); k++) *arrs[k] = xpoint(to, xlocal(from, *arrs[k]));
    c->v[i] = qrot(to.q, qrot_inv(from.q, c->v[i]));
    if (c->skin_from) {
      c->skin_from[i] = xpoint(to, xlocal(from, c->skin_from[i]));
      c->skin_to[i] = xpoint(to, xlocal(from, c->skin_to[i]));
    }
  }
}

void cw_cloth_set_wind(cw_cloth h, t3_vec3 wind, float drag, float lift, float turbulence) {
  cw__cloth *c = rec(h);
  if (!c) return;
  c->wind = wind, c->drag = drag, c->lift = lift, c->turbulence = cw_clampf(turbulence, 0, 1);
}

void cw_cloth_set_gravity_scale(cw_cloth h, float scale) {
  cw__cloth *c = rec(h);
  if (c) c->gscale = scale;
}

void cw_cloth_impulse(cw_cloth h, t3_vec3 center, float radius, t3_vec3 delta_v) {
  cw__cloth *c = rec(h);
  if (!c || radius <= 0) return;
  for (int i = 0; i < c->n; i++) {
    if (c->w[i] <= 0 || c->attached[i]) continue;
    float d = vlen(vsub(c->x[i], center));
    if (d < radius) c->v[i] = vadd(c->v[i], vmul(delta_v, 1 - d / radius));
  }
}

/* ── the step ─────────────────────────────────────────────────────────── */

typedef struct stepctx {
  cw__cloth *c;
  float h, inv_h, sub_t;     /* substep length, 1/h, the substep's end as a share of the tick */
  float prev_t;              /* the substep's start as a share of the tick */
  V g;
  float damp;
  bool fuse_post;            /* phase B runs post on its particles too (no self-collision) */
} stepctx;

/* small cloths (and workers == 1) run on the calling thread: fanning out costs more than it saves */
static void run(cw__cloth *c, int count, cw_range_fn fn, void *ctx) {
  if (count <= 0) return;
  if (c->def.workers == 1 || c->inline_run || c->n < 1024 || count == 1) fn(0, count, 0, ctx);
  else cw_parallel_for(count, 1, fn, ctx);
}

/* the wind at a particle: the set wind, varied smoothly in space and time by turbulence */
static V wind_at(const cw__cloth *c, V x) {
  if (c->turbulence <= 0) return c->wind;
  float t = c->time;
  float a = cw_sin(0.71f * x.x + 0.37f * x.z + 1.9f * t) + 0.5f * cw_sin(1.37f * x.y - 0.83f * x.x + 3.1f * t);
  float b = cw_sin(0.53f * x.z + 0.91f * x.y + 1.3f * t);
  float s = 1 + c->turbulence * 0.6f * a;
  V wv = vmul(c->wind, s);
  float speed = vlen(c->wind);
  wv.y += c->turbulence * 0.35f * speed * b;
  return wv;
}

/* the air's push on each particle for this tick, from its velocity and normal at the tick's start
 * (an acceleration, held over the substeps) */
static void wind_range(int begin, int end, int worker, void *u) {
  (void)worker;
  cw__cloth *c = ((stepctx *)u)->c;
  const float rho = 1.2f;
  for (int i = begin; i < end; i++) {
    V f = { 0, 0, 0 };
    if (c->w[i] > 0 && !c->attached[i] && c->area[i] > 0) {
      V rel = vsub(wind_at(c, c->x[i]), c->v[i]);
      if (c->ntri) {
        /* drag along the flow and lift across it, both by the share of the flow the surface meets
         * (|cos|: none edge-on), lift by sin too (none face-on) */
        float s2 = vdot(rel, rel);
        if (s2 > 1e-12f) {
          float sp = sqrtf(s2);
          V dir = vmul(rel, 1 / sp);
          V nn = c->nrm[i];
          float cs = vdot(nn, dir);
          if (cs < 0) nn = vmul(nn, -1), cs = -cs;
          V l = vsub(nn, vmul(dir, cs)); /* |l| = sin */
          float q = 0.5f * rho * s2 * cs;
          f = vadd(vmul(dir, q * c->drag), vmul(l, -q * c->lift));
        }
      } else { /* rope: drag across the segment */
        V tg = c->nrm[i];
        V vp = vsub(rel, vmul(tg, vdot(rel, tg)));
        f = vmul(vp, 0.5f * rho * c->drag * vlen(vp));
      }
      f = vmul(f, c->area[i] * c->w[i]);
    }
    c->wv[i] = f;
  }
}

static void predict_range(int begin, int end, int worker, void *u) {
  (void)worker;
  stepctx *s = u;
  cw__cloth *c = s->c;
  for (int i = begin; i < end; i++) {
    c->p[i] = c->x[i];
    if (c->attached[i]) continue; /* placed by place_attached */
    if (c->w[i] <= 0) {
      c->x[i] = vlerp(c->pin_from[i], c->pin_to[i], s->sub_t);
      continue;
    }
    V v = vmul(vadd(c->v[i], vmul(vadd(s->g, c->wv[i]), s->h)), s->damp);
    c->v[i] = v;
    c->x[i] = vadd(c->x[i], vmul(v, s->h));
  }
}

/* Gauss-Seidel over one group's constraints, in creation order (one iteration: lambda starts at 0) */
static void solve_group(cw__cloth *c, int g) {
  for (int k = c->col_start[g]; k < c->col_start[g + 1]; k++) {
    float inv = c->cinv[k];
    if (inv <= 0) continue;
    int a = c->ci0[k], b = c->ci1[k];
    V d = vsub(c->x[a], c->x[b]);
    float l = sqrtf(vdot(d, d));
    if (l < 1e-9f) continue;
    float f = (c->crest[k] - l) * inv / l; /* lambda / l */
    c->x[a] = vadd(c->x[a], vmul(d, f * c->w[a]));
    c->x[b] = vsub(c->x[b], vmul(d, f * c->w[b]));
  }
}

/* body pose at share t of the tick */
static cw_xform3 shape_pose(const cshape *sh, float t) { return xlerp(sh->from, sh->to, t); }

/* signed distance of local point q from the shape's surface and the outward normal (body local) */
static float shape_sdf(const cshape *sh, V q, V *n) {
  switch (sh->kind) {
  case SH_SPHERE: {
    V d = vsub(q, sh->a);
    float l = vlen(d);
    *n = l > 1e-9f ? vmul(d, 1 / l) : (V){ 0, 1, 0 };
    return l - sh->radius;
  }
  case SH_CAPSULE: {
    V ab = vsub(sh->b, sh->a);
    float ll = vdot(ab, ab);
    float t = ll > 0 ? vdot(vsub(q, sh->a), ab) / ll : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    V d = vsub(q, vadd(sh->a, vmul(ab, t)));
    float l = vlen(d);
    *n = l > 1e-9f ? vmul(d, 1 / l) : (V){ 0, 1, 0 };
    return l - sh->radius;
  }
  case SH_HULL: {
    float best = -1e30f;
    int bi = 0;
    for (int i = 0; i < sh->plane_count; i++) {
      const b3Plane *pl = &sh->planes[i];
      float s = pl->normal.x * q.x + pl->normal.y * q.y + pl->normal.z * q.z - pl->offset;
      if (s > best) best = s, bi = i;
    }
    *n = (V){ sh->planes[bi].normal.x, sh->planes[bi].normal.y, sh->planes[bi].normal.z };
    return best;
  }
  }
  return 1e30f;
}

/* tethers, skin, shapes, then the velocity update: everything per particle after the constraints */
static void post_range(int begin, int end, int worker, void *u) {
  (void)worker;
  stepctx *s = u;
  cw__cloth *c = s->c;
  float th = c->def.thickness;
  for (int i = begin; i < end; i++) {
    if (c->w[i] <= 0) goto velocity;
    /* long-range attachment: never farther from the anchor than the rest distance (x slack) */
    if (c->tether_of[i] >= 0) {
      int k = c->tether_of[i];
      V d = vsub(c->x[i], c->xa[c->ta[k]]);
      float l2 = vdot(d, d), tl = c->tlen[k];
      c->taut[i] = 0;
      if (l2 > tl * tl && l2 > 1e-18f) {
        float l = sqrtf(l2);
        c->x[i] = vsub(c->x[i], vmul(d, (l - c->tlen[k]) / l));
        c->taut[i] = 1;
      }
    }
    if (c->maxd && c->skin_set && c->maxd[i] >= 0 && !c->attached[i]) {
      V target = vlerp(c->skin_from[i], c->skin_to[i], s->sub_t);
      V d = vsub(c->x[i], target);
      float l = vlen(d);
      if (l > c->maxd[i]) c->x[i] = l > 1e-9f ? vadd(target, vmul(d, c->maxd[i] / l)) : target;
    }
    if (!c->attached[i])
      for (int k = 0; k < c->shapes.count; k++) {
        const cshape *sh = &c->shapes.data[k];
        V dc = vsub(c->x[i], sh->world_c);
        float reach = sh->bound_r + th;
        if (vdot(dc, dc) > reach * reach) continue; /* outside its bounding sphere */
        cw_xform3 pose = sh->pose;
        V q = xlocal(pose, c->x[i]);
        V n = { 0, 1, 0 };
        float dist = shape_sdf(sh, q, &n);
        if (dist >= th) continue;
        float depth = th - dist;
        V nw = qrot(pose.q, n);
        V push = vmul(nw, depth);
        V xn = vadd(c->x[i], push);
        /* friction against the surface's own motion over this substep */
        if (c->def.friction > 0 || sh->friction > 0) {
          float mu = c->def.friction < sh->friction ? c->def.friction : sh->friction;
          V surf_prev = xpoint(sh->prev, q);
          V surf_now = xpoint(pose, q);
          V rel = vsub(vsub(xn, c->p[i]), vsub(surf_now, surf_prev));
          V tan = vsub(rel, vmul(nw, vdot(rel, nw)));
          float tl = vlen(tan);
          if (tl > 1e-9f) {
            float cut = mu * depth >= tl ? 1 : mu * depth / tl;
            xn = vsub(xn, vmul(tan, cut));
          }
        }
        if (sh->dynamic && c->def.two_way) {
          c->coll[i] = k;
          c->coll_d[i] = vadd(c->coll_d[i], vsub(xn, c->x[i]));
        }
        c->x[i] = xn;
      }
  velocity:
    if (c->attached[i]) continue; /* its velocity is the body's; see finish_attached */
    {
      V v = vmul(vsub(c->x[i], c->p[i]), s->inv_h);
      float s2 = vdot(v, v);
      if (s2 > c->def.max_speed * c->def.max_speed) v = vmul(v, c->def.max_speed / sqrtf(s2));
      c->v[i] = v;
    }
  }
}

/* phase A: each pair of blocks predicts its particles, then solves the constraints inside it */
static void phase_a(int begin, int end, int worker, void *u) {
  stepctx *s = u;
  cw__cloth *c = s->c;
  for (int p = begin; p < end; p++) {
    int lo = 2 * p * c->bs, hi = (2 * p + 2) * c->bs;
    if (hi > c->n) hi = c->n;
    predict_range(lo, hi, worker, u);
    solve_group(c, p);
  }
}

/* phase B: the constraints across each odd block boundary, then (without self-collision) the
 * per-particle pass for the two blocks it joins, which nothing else touches any more */
static void phase_b(int begin, int end, int worker, void *u) {
  stepctx *s = u;
  cw__cloth *c = s->c;
  for (int g = begin; g < end; g++) {
    solve_group(c, c->na + g);
    if (!s->fuse_post) continue;
    int lo = (2 * g - 1) * c->bs, hi = (2 * g + 1) * c->bs;
    if (lo < 0) lo = 0;
    if (hi > c->n) hi = c->n;
    if (lo < hi) post_range(lo, hi, worker, u);
  }
}

/* self-collision: Jacobi corrections from the neighbours found at the tick's start */
typedef struct selfgrid {
  int *cell_start, *cell_items; /* hash table: counting sort of particles by cell */
  int table;
  float spacing;
  int *nb_start, *nb;           /* per particle: neighbours within reach (CSR) */
  int nb_cap;
} selfgrid;

static int hash_cell(int xi, int yi, int zi, int table) {
  uint32_t h = (uint32_t)xi * 92837111u ^ (uint32_t)yi * 689287499u ^ (uint32_t)zi * 283923481u; /* int-math-ok: uint32 */
  return (int)(h % (uint32_t)table);
}
static int cell_coord(float v, float sp) { return (int)floorf(v / sp); }

static void self_build(cw__cloth *c, selfgrid *g, float reach) {
  int n = c->n;
  g->spacing = reach;
  g->table = 2 * n;
  g->cell_start = zalloc((size_t)g->table + 1, sizeof(int));
  g->cell_items = zalloc((size_t)n, sizeof(int));
  for (int i = 0; i < n; i++) {
    V x = c->x[i];
    g->cell_start[hash_cell(cell_coord(x.x, reach), cell_coord(x.y, reach), cell_coord(x.z, reach), g->table) + 1]++;
  }
  for (int i = 0; i < g->table; i++) g->cell_start[i + 1] += g->cell_start[i];
  int *fill = zalloc((size_t)g->table, sizeof(int));
  for (int i = 0; i < n; i++) {
    V x = c->x[i];
    int h = hash_cell(cell_coord(x.x, reach), cell_coord(x.y, reach), cell_coord(x.z, reach), g->table);
    g->cell_items[g->cell_start[h] + fill[h]++] = i;
  }
  cw_free(fill);
  /* neighbours (serial: counts then lists; order: cells in a fixed 27-cell sweep, items by index) */
  g->nb_start = zalloc((size_t)n + 1, sizeof(int));
  CW_ARRAY(int) list = { 0 };
  float r2 = reach * reach;
  for (int i = 0; i < n; i++) {
    V x = c->x[i];
    int cx = cell_coord(x.x, reach), cy = cell_coord(x.y, reach), cz = cell_coord(x.z, reach);
    int seen[27], ns = 0;
    for (int dz = -1; dz <= 1; dz++)
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          int h = hash_cell(cx + dx, cy + dy, cz + dz, g->table);
          bool dup = false;
          for (int q = 0; q < ns; q++) dup |= seen[q] == h;
          if (dup) continue;
          seen[ns++] = h;
          for (int k = g->cell_start[h]; k < g->cell_start[h + 1]; k++) {
            int j = g->cell_items[k];
            if (j == i) continue;
            V d = vsub(c->x[j], x);
            if (vdot(d, d) < r2) cw_array_push(&list, j);
          }
        }
    g->nb_start[i + 1] = list.count;
  }
  g->nb = list.data;
  g->nb_cap = list.cap;
}

static void self_free(selfgrid *g) {
  cw_free(g->cell_start), cw_free(g->cell_items), cw_free(g->nb_start), cw_free(g->nb);
  memset(g, 0, sizeof *g);
}

typedef struct selfctx {
  cw__cloth *c;
  selfgrid *g;
} selfctx;

static void self_range(int begin, int end, int worker, void *u) {
  (void)worker;
  selfctx *s = u;
  cw__cloth *c = s->c;
  float th = c->def.thickness * 2;
  for (int i = begin; i < end; i++) {
    V acc = { 0, 0, 0 };
    if (c->w[i] > 0 && !c->attached[i])
      for (int k = s->g->nb_start[i]; k < s->g->nb_start[i + 1]; k++) {
        int j = s->g->nb[k];
        V d = vsub(c->x[i], c->x[j]);
        float l = vlen(d);
        /* particles closer than thickness at rest (neighbours) keep their rest distance as the minimum */
        float rest = vlen(vsub(c->xt[i], c->xt[j]));
        float minl = th < rest ? th : rest;
        if (l >= minl || l < 1e-9f) continue;
        float wj = c->w[j] > 0 && !c->attached[j] ? 0.5f : 1.0f;
        acc = vadd(acc, vmul(d, (minl - l) / l * wj));
      }
    c->dx[i] = acc;
  }
}
static void self_apply(int begin, int end, int worker, void *u) {
  (void)worker;
  cw__cloth *c = ((selfctx *)u)->c;
  for (int i = begin; i < end; i++) c->x[i] = vadd(c->x[i], c->dx[i]);
}

typedef struct bodyinfo {
  cw_xform3 from, to;
  bool dynamic;
  b3BodyId id;
} bodyinfo;

static bodyinfo body_info(cw_world3d w, cw_body3d b) {
  bodyinfo bi = { 0 };
  bi.id = cw_body3d_raw(b);
  bi.to = cw_body3d_transform(b);
  bi.from = bi.to;
  cw__body3 *r = cw__b3(b);
  cw__world3 *wr = cw__w3(w);
  if (r && wr && r->interpolate && r->moved_span == wr->span) bi.from = r->prev, bi.to = r->cur;
  bi.dynamic = cw_body3d_type(b) == CW_BODY_DYNAMIC;
  return bi;
}

static void place_attached(cw__cloth *c, float t) {
  for (int k = 0; k < c->attachments.count; k++) {
    const attach *a = &c->attachments.data[k];
    if (!cw_body3d_valid(a->body)) continue;
    bodyinfo bi = body_info(c->world, a->body);
    V at = xpoint(xlerp(bi.from, bi.to, t), a->local);
    c->x[a->vertex] = vadd(at, c->offset[a->vertex]);
  }
}

static void after_solve_attached(cw__cloth *c, float t) {
  for (int k = 0; k < c->attachments.count; k++) {
    const attach *a = &c->attachments.data[k];
    if (!cw_body3d_valid(a->body)) continue;
    bodyinfo bi = body_info(c->world, a->body);
    V at = xpoint(xlerp(bi.from, bi.to, t), a->local);
    c->offset[a->vertex] = c->w[a->vertex] > 0 ? vsub(c->x[a->vertex], at) : (V){ 0, 0, 0 };
  }
}

/* anchor-to-anchor tethers between body attachments (serial, few) */
static void solve_anchor_tethers(cw__cloth *c) {
  for (int k = 0; k < c->natether; k++) c->taut[c->av[k]] = 0;
  for (int k = 0; k < c->natether; k++) {
    int a = c->av[k], b = c->aa[k];
    float wa = c->w[a], wb = c->w[b], ws = wa + wb;
    if (ws <= 0) continue;
    V d = vsub(c->x[a], c->x[b]);
    float l = vlen(d);
    if (l <= c->alen[k] || l < 1e-9f) continue;
    V corr = vmul(d, (l - c->alen[k]) / l / ws);
    c->x[a] = vsub(c->x[a], vmul(corr, wa));
    c->x[b] = vadd(c->x[b], vmul(corr, wb));
    c->taut[a] = 1;
    if (c->attached[b] && wb > 0) c->taut[b] = 1;
  }
}

static V body_point_velocity(const attach *a) {
  if (!cw_body3d_valid(a->body)) return (V){ 0, 0, 0 };
  b3BodyId id = cw_body3d_raw(a->body);
  V at = xpoint(cw_body3d_transform(a->body), a->local);
  b3Vec3 lv = b3Body_GetLinearVelocity(id), av = b3Body_GetAngularVelocity(id);
  b3Pos com = b3Body_GetWorldCenterOfMass(id);
  V r = vsub(at, (V){ (float)com.x, (float)com.y, (float)com.z });
  return vadd((V){ lv.x, lv.y, lv.z }, vcross((V){ av.x, av.y, av.z }, r));
}

/* after the substeps: what the cloth did to each dynamic body becomes one impulse (acting in the
 * world's next step): a soft correction of where the solve put the attachment, plus, while the
 * cloth holds it taut, the body's velocity away from the cloth taken out */
static void finish_attached(cw__cloth *c, float dt) {
  const float beta = 0.3f;
  for (int k = 0; k < c->attachments.count; k++) {
    attach *a = &c->attachments.data[k];
    int v = a->vertex;
    a->force = 0;
    if (!cw_body3d_valid(a->body)) continue;
    bodyinfo bi = body_info(c->world, a->body);
    V at = xpoint(bi.to, a->local);
    V d = c->offset[v];
    c->offset[v] = (V){ 0, 0, 0 };
    c->x[v] = at;
    if (!bi.dynamic || c->w[v] <= 0) {
      c->v[v] = vmul(vsub(at, xpoint(bi.from, a->local)), 1 / dt);
      continue;
    }
    float dl = vlen(d);
    b3Vec3 lv = b3Body_GetLinearVelocity(bi.id), av = b3Body_GetAngularVelocity(bi.id);
    b3Pos com = b3Body_GetWorldCenterOfMass(bi.id);
    V r = vsub(at, (V){ (float)com.x, (float)com.y, (float)com.z });
    V vp = vadd((V){ lv.x, lv.y, lv.z }, vcross((V){ av.x, av.y, av.z }, r));
    c->v[v] = vp;
    if (dl < 1e-7f) continue;
    V u = vmul(d, 1 / dl);
    b3Matrix3 ii = b3Body_GetWorldInverseRotationalInertia(bi.id);
    V ru = vcross(r, u);
    b3Vec3 iru = b3MulMV(ii, (b3Vec3){ ru.x, ru.y, ru.z });
    float k_inv = b3Body_GetInverseMass(bi.id) + vdot(ru, (V){ iru.x, iru.y, iru.z });
    if (k_inv <= 0) continue;
    /* the partner it is tethered to: its velocity (a body point's, or a moving pin's) */
    V vpart = { 0, 0, 0 };
    for (int t = 0; t < c->natether; t++)
      if (c->av[t] == v) {
        int o = c->aa[t];
        if (c->pinned[o]) vpart = vmul(vsub(c->pin_to[o], c->pin_from[o]), 1 / dt);
        else
          for (int q = 0; q < c->attachments.count; q++)
            if (c->attachments.data[q].vertex == o) vpart = body_point_velocity(&c->attachments.data[q]);
        break;
      }
    float away = -vdot(vsub(vp, vpart), u);
    /* a velocity along u of beta * dl / dt, toward the cloth (soft position correction), from what the
     * body already has: all of it while taut, only what slows the correction while slack */
    float dv = beta * dl / dt + (c->taut[v] ? away : (away < 0 ? away : 0));
    if (dv <= 0) continue;
    V J = vmul(u, dv / k_inv);
    a->force = dv / k_inv / dt;
    cw_body3d_apply_impulse(a->body, J, &at);
  }
}

static void gather_shapes(cw__cloth *c, float dt) {
  cw_array_clear(&c->shapes);
  if (!c->def.collide_mask) return;
  V lo = c->x[0], hi = c->x[0];
  float vmax = 0;
  for (int i = 0; i < c->n; i++) {
    V x = c->x[i];
    lo = (V){ x.x < lo.x ? x.x : lo.x, x.y < lo.y ? x.y : lo.y, x.z < lo.z ? x.z : lo.z };
    hi = (V){ x.x > hi.x ? x.x : hi.x, x.y > hi.y ? x.y : hi.y, x.z > hi.z ? x.z : hi.z };
    float s = vdot(c->v[i], c->v[i]);
    if (s > vmax) vmax = s;
  }
  float m = c->def.thickness + sqrtf(vmax) * dt + 9.81f * dt * dt + 0.05f;
  lo = vsub(lo, (V){ m, m, m }), hi = vadd(hi, (V){ m, m, m });
  cw_shape3d found[256];
  cw_phys_filter f = cw_phys_filter_layers(c->def.collide_mask);
  int nf = cw_world3d_overlap_aabb(c->world, lo, hi, &f, found, (int)CW_ARRAY_LEN(found));
  for (int i = 0; i < nf; i++) {
    b3ShapeId sid = cw_shape3d_raw(found[i]);
    b3ShapeType ty = b3Shape_GetType(sid);
    cshape sh = { 0 };
    if (ty == b3_sphereShape) {
      b3Sphere s = b3Shape_GetSphere(sid);
      sh.kind = SH_SPHERE, sh.a = (V){ s.center.x, s.center.y, s.center.z }, sh.radius = s.radius;
      sh.bound_c = sh.a, sh.bound_r = s.radius;
    } else if (ty == b3_capsuleShape) {
      b3Capsule s = b3Shape_GetCapsule(sid);
      sh.kind = SH_CAPSULE, sh.a = (V){ s.center1.x, s.center1.y, s.center1.z };
      sh.b = (V){ s.center2.x, s.center2.y, s.center2.z }, sh.radius = s.radius;
      sh.bound_c = vmul(vadd(sh.a, sh.b), 0.5f), sh.bound_r = 0.5f * vlen(vsub(sh.b, sh.a)) + s.radius;
    } else if (ty == b3_hullShape) {
      const b3HullData *hd = b3Shape_GetHull(sid);
      if (!hd) continue;
      sh.kind = SH_HULL, sh.planes = b3GetHullPlanes(hd), sh.plane_count = hd->faceCount;
      V lo = { hd->aabb.lowerBound.x, hd->aabb.lowerBound.y, hd->aabb.lowerBound.z };
      V hi = { hd->aabb.upperBound.x, hd->aabb.upperBound.y, hd->aabb.upperBound.z };
      sh.bound_c = vmul(vadd(lo, hi), 0.5f), sh.bound_r = 0.5f * vlen(vsub(hi, lo));
    } else continue; /* meshes, height fields, compounds: not yet */
    cw_body3d b = cw_shape3d_body(found[i]);
    bodyinfo bi = body_info(c->world, b);
    sh.body = bi.id, sh.dynamic = bi.dynamic, sh.from = bi.from, sh.to = bi.to;
    sh.friction = b3Shape_GetFriction(sid);
    cw_array_push(&c->shapes, sh);
  }
}

static uint32_t fold(uint32_t h, const void *data, size_t words) {
  const uint32_t *w = data;
  for (size_t i = 0; i < words; i++) h = (h ^ w[i]) * 16777619u;
  return h;
}

typedef struct normctx {
  cw__cloth *c;
} normctx;
static void normals_range(int begin, int end, int worker, void *u) {
  (void)worker;
  cw__cloth *c = ((normctx *)u)->c;
  compute_normals(c, c->x, c->nrm, begin, end);
}

/* One step in four pieces, so a scene's cloths can share the pool (cw_cloth_step_many):
 *   prepare  serial: tethers, quality, the shapes near the cloth (a world query)
 *   body     touches only this cloth and reads body poses: any thread, any order
 *   finish   serial, in cloth order: impulses on dynamic bodies (they read and change velocities)
 *   tail     this cloth only: reached targets and the hash
 * Run in this order for each cloth in turn (cw_cloth_step), or each piece across all of them, the
 * bits are the same: no piece reads what another cloth's piece writes. */
typedef struct stepjob {
  cw__cloth *c;
  stepctx s;
  float dt;
  int S;
  bool self;
  double work; /* seconds this cloth's own pieces took (not the batch's wall time) */
} stepjob;

static bool step_prepare(cw__cloth *c, float dt, stepjob *j) {
  if (!c || dt <= 0) return false;
  if (!cw_world3d_valid(c->world)) return false;
  j->c = c, j->dt = dt;
  double t0 = now_s();
  if (c->tethers_dirty) build_tethers(c);
  int S = c->def.substeps;
  bool self = c->def.self_collision;
  if (c->def.quality_scaled && cw_quality_get() == CW_QUALITY_LOW) {
    S = S > 1 ? S / 2 : 1;
    self = false;
  }
  j->S = S, j->self = self;
  memcpy(c->xt, c->x, sizeof(V) * (size_t)c->n);
  /* the shapes near the cloth, sorted by id (the query sorts) */
  gather_shapes(c, dt);
  j->s = (stepctx){ .c = c, .h = dt / (float)S, .inv_h = (float)S / dt };
  t3_vec3 g = cw_world3d_gravity(c->world);
  j->s.g = vmul(g, c->gscale);
  j->s.damp = 1 - c->def.damping * j->s.h;
  if (j->s.damp < 0) j->s.damp = 0;
  j->work = now_s() - t0;
  return true;
}

static void step_body(stepjob *j) {
  double t0 = now_s();
  cw__cloth *c = j->c;
  stepctx s = j->s;
  float dt = j->dt;
  int S = j->S;
  if (c->ntri) {
    normctx nc = { c };
    run(c, c->n, normals_range, &nc);
  } else compute_tangents(c, c->x, c->nrm);
  if (c->def.two_way)
    for (int i = 0; i < c->n; i++) c->coll[i] = -1, c->coll_d[i] = (V){ 0, 0, 0 };
  selfgrid sg = { 0 };
  if (j->self && c->def.thickness > 0) {
    float vmax = 0;
    for (int i = 0; i < c->n; i++) {
      float s2 = vdot(c->v[i], c->v[i]);
      if (s2 > vmax) vmax = s2;
    }
    self_build(c, &sg, 2 * c->def.thickness + sqrtf(vmax) * dt);
  }
  if (c->drag != 0 || c->lift != 0) run(c, c->n, wind_range, &s);
  else memset(c->wv, 0, sizeof(V) * (size_t)c->n);
  {
    float ih2 = s.inv_h * s.inv_h;
    for (int k = 0; k < c->ncon; k++) {
      float ws = c->w[c->ci0[k]] + c->w[c->ci1[k]] + c->ccomp[k] * ih2;
      c->cinv[k] = ws > 0 ? 1 / ws : 0;
    }
  }
  for (int sub = 0; sub < S; sub++) {
    s.prev_t = (float)sub / (float)S;
    s.sub_t = (float)(sub + 1) / (float)S;
    s.fuse_post = sg.nb_start == NULL;
    place_attached(c, s.sub_t);
    run(c, c->na, phase_a, &s);
    /* the serial leftovers, then the tethers between body attachments */
    solve_group(c, c->na + c->nb);
    solve_anchor_tethers(c);
    for (int k = 0; k < c->nanchors; k++) c->xa[c->anchors[k]] = c->x[c->anchors[k]];
    for (int k = 0; k < c->shapes.count; k++) { /* the shapes' poses for this substep, once */
      cshape *sh = &c->shapes.data[k];
      sh->pose = shape_pose(sh, s.sub_t), sh->prev = shape_pose(sh, s.prev_t);
      sh->world_c = xpoint(sh->pose, sh->bound_c);
    }
    run(c, c->nb, phase_b, &s);
    if (!s.fuse_post) {
      selfctx sc = { c, &sg };
      run(c, c->n, self_range, &sc);
      run(c, c->n, self_apply, &sc);
      run(c, c->n, post_range, &s);
    }
    after_solve_attached(c, s.sub_t);
  }
  self_free(&sg);
  j->work += now_s() - t0;
}

static void step_finish(stepjob *j) {
  double t0 = now_s();
  cw__cloth *c = j->c;
  float dt = j->dt;
  finish_attached(c, dt);
  /* collisions pushed dynamic bodies: one impulse per particle, in particle order */
  if (c->def.two_way)
    for (int i = 0; i < c->n; i++) {
      if (c->coll[i] < 0) continue;
      const cshape *sh = &c->shapes.data[c->coll[i]];
      V J = vmul(c->coll_d[i], -c->mass[i] / dt);
      b3Vec3 jb = { J.x, J.y, J.z };
      b3Body_ApplyLinearImpulse(sh->body, jb, (b3Pos){ c->x[i].x, c->x[i].y, c->x[i].z }, true);
    }
  j->work += now_s() - t0;
}

static void step_tail(stepjob *j) {
  double t0 = now_s();
  cw__cloth *c = j->c;
  /* targets reached: the next step starts from them */
  memcpy(c->pin_from, c->pin_to, sizeof(V) * (size_t)c->n);
  if (c->skin_set) memcpy(c->skin_from, c->skin_to, sizeof(V) * (size_t)c->n);
  c->steps++;
  c->time = (float)c->steps * j->dt;
  uint32_t hh = c->hash;
  hh = fold(hh, c->x, (size_t)c->n * 3);
  hh = fold(hh, c->v, (size_t)c->n * 3);
  c->hash = hh;
  c->step_us = (float)((j->work + now_s() - t0) * 1e6);
}

void cw_cloth_step(cw_cloth h, float dt) {
  CW_PROFILE_SCOPE("cloth.step");
  stepjob j;
  if (!step_prepare(rec(h), dt, &j)) return;
  step_body(&j);
  step_finish(&j);
  step_tail(&j);
}

typedef struct batch {
  stepjob *jobs;
} batch;

static void batch_range(int begin, int end, int worker, void *u) {
  (void)worker;
  batch *b = u;
  for (int i = begin; i < end; i++) {
    stepjob *j = &b->jobs[i];
    CW_PROFILE_SCOPE("cloth.step");
    step_body(j);
  }
}

void cw_cloth_step_many(const cw_cloth *cloths, int count, float dt) {
  if (!cloths || count <= 0) return;
  if (count == 1) {
    cw_cloth_step(cloths[0], dt);
    return;
  }
  stepjob *jobs = cw_alloc(sizeof(stepjob) * (size_t)count, CW_MEM_PHYSICS);
  int n = 0, particles = 0;
  bool pool_ok = true; /* a cloth made with workers = 1 keeps the whole batch on this thread */
  bool dup = false;
  for (int i = 0; i < count && !dup; i++) {
    cw__cloth *c = rec(cloths[i]);
    if (!c) continue;
    if (c->in_batch) dup = true;
    c->in_batch = true;
  }
  for (int i = 0; i < count; i++) {
    cw__cloth *c = rec(cloths[i]);
    if (c) c->in_batch = false;
  }
  if (dup) { /* a cloth listed twice steps twice, in order: one at a time */
    cw_free(jobs);
    for (int i = 0; i < count; i++) cw_cloth_step(cloths[i], dt);
    return;
  }
  for (int i = 0; i < count; i++) {
    cw__cloth *c = rec(cloths[i]);
    if (!step_prepare(c, dt, &jobs[n])) continue;
    particles += c->n;
    if (c->def.workers == 1) pool_ok = false;
    n++;
  }
  /* one job per cloth: each runs its own substeps on one thread (a cloth's own fan-out is off
   * while the batch runs). Too little work to hand out runs here, in order. */
  bool fan = pool_ok && n > 1 && particles >= 256 && cw_jobs_thread_count() > 1;
  for (int i = 0; i < n; i++) jobs[i].c->inline_run = fan;
  batch b = { jobs };
  if (fan) cw_parallel_for(n, 1, batch_range, &b);
  else batch_range(0, n, 0, &b);
  for (int i = 0; i < n; i++) step_finish(&jobs[i]);
  for (int i = 0; i < n; i++) step_tail(&jobs[i]); /* a hash each: too little to hand out */
  for (int i = 0; i < n; i++) jobs[i].c->inline_run = false;
  cw_free(jobs);
}

/* ── reading ──────────────────────────────────────────────────────────── */

const t3_vec3 *cw_cloth_positions(cw_cloth h, int *count) {
  cw__cloth *c = rec(h);
  if (count) *count = c ? c->n : 0;
  return c ? c->x : NULL;
}
const t3_vec3 *cw_cloth_velocities(cw_cloth h, int *count) {
  cw__cloth *c = rec(h);
  if (count) *count = c ? c->n : 0;
  return c ? c->v : NULL;
}
void cw_cloth_interpolated(cw_cloth h, float alpha, t3_vec3 *out) {
  cw__cloth *c = rec(h);
  if (!c || !out) return;
  for (int i = 0; i < c->n; i++) out[i] = vlerp(c->xt[i], c->x[i], alpha);
}
int cw_cloth_vertex_count(cw_cloth h) {
  cw__cloth *c = rec(h);
  return c ? c->n : 0;
}
uint32_t cw_cloth_hash(cw_cloth h) {
  cw__cloth *c = rec(h);
  return c ? c->hash : 0;
}

cw_cloth_stats cw_cloth_stats_get(cw_cloth h) {
  cw_cloth_stats st = { 0 };
  cw__cloth *c = rec(h);
  if (!c) return st;
  st.vertices = c->n, st.triangles = c->ntri, st.constraints = c->ncon, st.groups = c->ncol;
  st.tethers = c->ntether + c->natether, st.shapes = c->shapes.count, st.steps = c->steps;
  st.step_us = c->step_us;
  float ms = 0, sum = 0;
  for (int e = 0; e < c->nedge; e++) {
    int k = c->edge_slot[e];
    float l = vlen(vsub(c->x[c->ci0[k]], c->x[c->ci1[k]]));
    float s = c->crest[k] > 0 ? l / c->crest[k] - 1 : 0;
    if (s > ms) ms = s;
    if (s > 0) sum += s;
  }
  st.max_stretch = ms;
  st.mean_stretch = c->nedge ? sum / (float)c->nedge : 0;
  float ke = 0;
  for (int i = 0; i < c->n; i++)
    if (c->w[i] > 0 && !c->attached[i]) ke += 0.5f * c->mass[i] * vdot(c->v[i], c->v[i]);
  st.kinetic_energy = ke;
  return st;
}

float cw_cloth_attachment_force(cw_cloth h, int vertex) {
  cw__cloth *c = rec(h);
  if (!c) return 0;
  for (int i = 0; i < c->attachments.count; i++)
    if (c->attachments.data[i].vertex == vertex) return c->attachments.data[i].force;
  return 0;
}

/* ── rollback ─────────────────────────────────────────────────────────── */

typedef struct state_hdr {
  uint32_t magic, n, hash;
  int32_t steps;
  float time;
} state_hdr;

static size_t state_size(const cw__cloth *c) {
  size_t n = (size_t)c->n;
  return sizeof(state_hdr) + n * (sizeof(V) * 6 + sizeof(float) + 2) + (c->skin_to ? n * sizeof(V) * 2 : 0);
}
size_t cw_cloth_state_size(cw_cloth h) {
  cw__cloth *c = rec(h);
  return c ? state_size(c) : 0;
}

#define PUT(ptr, bytes) (memcpy(o, (ptr), (bytes)), o += (bytes))
#define GET(ptr, bytes) (memcpy((ptr), i, (bytes)), i += (bytes))
bool cw_cloth_save(cw_cloth h, void *out) {
  cw__cloth *c = rec(h);
  if (!c || !out) return false;
  size_t n = (size_t)c->n;
  uint8_t *o = out;
  state_hdr hd = { 0x434c5448u, (uint32_t)c->n, c->hash, c->steps, c->time };
  PUT(&hd, sizeof hd);
  PUT(c->x, n * sizeof(V)), PUT(c->v, n * sizeof(V)), PUT(c->xt, n * sizeof(V));
  PUT(c->pin_from, n * sizeof(V)), PUT(c->pin_to, n * sizeof(V)), PUT(c->offset, n * sizeof(V));
  PUT(c->w, n * sizeof(float)), PUT(c->pinned, n), PUT(c->taut, n);
  if (c->skin_to) PUT(c->skin_from, n * sizeof(V)), PUT(c->skin_to, n * sizeof(V));
  return true;
}
bool cw_cloth_load(cw_cloth h, const void *state, size_t size) {
  cw__cloth *c = rec(h);
  if (!c || !state || size != state_size(c)) return false;
  const uint8_t *i = state;
  size_t n = (size_t)c->n;
  state_hdr hd;
  GET(&hd, sizeof hd);
  if (hd.magic != 0x434c5448u || hd.n != (uint32_t)c->n) return false;
  c->hash = hd.hash, c->steps = hd.steps, c->time = hd.time;
  GET(c->x, n * sizeof(V)), GET(c->v, n * sizeof(V)), GET(c->xt, n * sizeof(V));
  GET(c->pin_from, n * sizeof(V)), GET(c->pin_to, n * sizeof(V)), GET(c->offset, n * sizeof(V));
  GET(c->w, n * sizeof(float)), GET(c->pinned, n), GET(c->taut, n);
  if (c->skin_to) GET(c->skin_from, n * sizeof(V)), GET(c->skin_to, n * sizeof(V));
  c->tethers_dirty = true;
  return true;
}
#undef PUT
#undef GET

/* ── rendering ────────────────────────────────────────────────────────── */

t3_geometry *cw_cloth_geometry_new(cw_cloth h) {
  cw__cloth *c = rec(h);
  if (!c || !c->ntri) return NULL;
  t3_geometry *g = t3_geometry_new();
  t3_attribute *pos = t3_attribute_new(T3_FLOAT32, c->x, c->n, 3);
  t3_attribute *nrm = t3_attribute_new(T3_FLOAT32, c->nrm, c->n, 3);
  pos->usage = nrm->usage = T3_DYNAMIC_DRAW;
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, pos);
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, nrm);
  if (c->uv) t3_geometry_set_attribute(g, T3_ATTR_UV, t3_attribute_new(T3_FLOAT32, c->uv, c->n, 2));
  t3_geometry_set_index(g, t3_attribute_new(T3_UINT32, c->tri, c->ntri * 3, 1));
  cw_cloth_update_geometry(h, g, 1);
  return g;
}

t3_geometry *cw_rope_geometry_new(cw_cloth h, float radius, int sides) {
  cw__cloth *c = rec(h);
  if (!c) return NULL;
  if (sides < 3) sides = 3;
  c->tube_radius = radius > 0 ? radius : 0.02f;
  c->tube_sides = sides;
  int ring = sides + 1, nv = c->n * ring, ni = (c->n - 1) * sides * 6;
  float *zero = zalloc((size_t)nv * 3, sizeof(float));
  float *uv = zalloc((size_t)nv * 2, sizeof(float));
  uint32_t *idx = zalloc((size_t)ni, sizeof(uint32_t));
  for (int i = 0; i < c->n; i++)
    for (int s = 0; s < ring; s++) {
      uv[2 * (i * ring + s)] = (float)s / (float)sides;
      uv[2 * (i * ring + s) + 1] = (float)i / (float)(c->n - 1);
    }
  int k = 0;
  for (int i = 0; i + 1 < c->n; i++)
    for (int s = 0; s < sides; s++) {
      uint32_t a = (uint32_t)(i * ring + s), b = a + 1, cc = a + (uint32_t)ring, d = cc + 1;
      idx[k++] = a, idx[k++] = cc, idx[k++] = b;
      idx[k++] = b, idx[k++] = cc, idx[k++] = d;
    }
  t3_geometry *g = t3_geometry_new();
  t3_attribute *pos = t3_attribute_new(T3_FLOAT32, zero, nv, 3), *nrm = t3_attribute_new(T3_FLOAT32, zero, nv, 3);
  pos->usage = nrm->usage = T3_DYNAMIC_DRAW;
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, pos);
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, nrm);
  t3_geometry_set_attribute(g, T3_ATTR_UV, t3_attribute_new(T3_FLOAT32, uv, nv, 2));
  t3_geometry_set_index(g, t3_attribute_new(T3_UINT32, idx, ni, 1));
  cw_free(zero), cw_free(uv), cw_free(idx);
  cw_cloth_update_geometry(h, g, 1);
  return g;
}

static void finish_attr(t3_geometry *g) {
  g->attributes[T3_ATTR_POSITION]->version++;
  g->attributes[T3_ATTR_NORMAL]->version++;
  t3_attribute_clear_update_ranges(g->attributes[T3_ATTR_POSITION]);
  t3_attribute_clear_update_ranges(g->attributes[T3_ATTR_NORMAL]);
  t3_geometry_compute_bounding_sphere(g);
  t3_geometry_compute_bounding_box(g);
}

void cw_cloth_update_geometry(cw_cloth h, t3_geometry *g, float alpha) {
  cw__cloth *c = rec(h);
  if (!c || !g || !g->attributes[T3_ATTR_POSITION] || !g->attributes[T3_ATTR_NORMAL]) return;
  t3_attribute *pa = g->attributes[T3_ATTR_POSITION], *na = g->attributes[T3_ATTR_NORMAL];
  V *pos = (V *)pa->array, *nrm = (V *)na->array;
  if (pa->count == c->n && c->ntri) {
    for (int i = 0; i < c->n; i++) pos[i] = vlerp(c->xt[i], c->x[i], alpha);
    compute_normals(c, pos, nrm, 0, c->n);
    finish_attr(g);
    return;
  }
  int ring = pa->count / (c->n > 0 ? c->n : 1);
  int sides = ring - 1;
  if (sides < 3 || ring * c->n != pa->count) return;
  float r = c->tube_radius > 0 ? c->tube_radius : 0.02f;
  /* parallel-transport frames along the chain */
  V prev_n = { 0, 0, 0 };
  for (int i = 0; i < c->n; i++) {
    V xi = vlerp(c->xt[i], c->x[i], alpha);
    int a = i > 0 ? i - 1 : i, b = i + 1 < c->n ? i + 1 : i;
    V t = vsub(vlerp(c->xt[b], c->x[b], alpha), vlerp(c->xt[a], c->x[a], alpha));
    float tl = vlen(t);
    t = tl > 1e-9f ? vmul(t, 1 / tl) : (V){ 0, 1, 0 };
    V nn;
    if (i == 0) {
      V ref = fabsf(t.y) < 0.9f ? (V){ 0, 1, 0 } : (V){ 1, 0, 0 };
      nn = vcross(t, ref);
    } else nn = vsub(prev_n, vmul(t, vdot(prev_n, t)));
    float nl = vlen(nn);
    nn = nl > 1e-9f ? vmul(nn, 1 / nl) : (V){ 1, 0, 0 };
    prev_n = nn;
    V bn = vcross(t, nn);
    for (int s = 0; s <= sides; s++) {
      float ang = (float)s / (float)sides * 6.2831853f;
      V dir = vadd(vmul(nn, cosf(ang)), vmul(bn, sinf(ang)));
      pos[i * ring + s] = vadd(xi, vmul(dir, r));
      nrm[i * ring + s] = dir;
    }
  }
  finish_attr(g);
}

/* ── chains of rigid links ────────────────────────────────────────────── */

cw_chain3d_def cw_chain3d_default_def(void) {
  return (cw_chain3d_def){ .end = { 0, -1, 0 }, .links = 12, .radius = 0.05f, .density = 1000, .damping = 0.1f, .fix_start = true,
                           .mask = CW_PHYS_ALL_LAYERS, .group = -1 };
}

int cw_chain3d_create(cw_world3d w, const cw_chain3d_def *def, cw_body3d *links, cw_joint3d *joints) {
  cw_chain3d_def d = def ? *def : cw_chain3d_default_def();
  if (!cw_world3d_valid(w) || d.links < 1 || !links) {
    cw_error("chain3d: needs a valid world, at least one link and a links array");
    return 0;
  }
  V span = vsub(d.end, d.start);
  float len = vlen(span);
  if (len <= 0) {
    cw_error("chain3d: start and end are the same point");
    return 0;
  }
  V dir = vmul(span, 1 / len);
  float seg = len / (float)d.links;
  float half = seg * 0.5f - d.radius;
  if (half < 0) half = 0;
  b3Quat bq = cw__quat_between((b3Vec3){ 0, 1, 0 }, (b3Vec3){ dir.x, dir.y, dir.z });
  t3_quat q = t3q(bq);
  for (int i = 0; i < d.links; i++) {
    cw_body3d_def bd = cw_body3d_default_def();
    bd.position = vadd(d.start, vmul(dir, seg * ((float)i + 0.5f)));
    bd.rotation = q;
    bd.linear_damping = bd.angular_damping = d.damping;
    links[i] = cw_body3d_create(w, &bd);
    cw_shape3d_def sd = cw_shape3d_default_def();
    sd.density = d.density, sd.layer = d.layer, sd.mask = d.mask, sd.group = d.group;
    cw_shape3d_capsule(links[i], &sd, (V){ 0, -half, 0 }, (V){ 0, half, 0 }, d.radius);
  }
  for (int i = 0; i <= d.links; i++) {
    cw_body3d a = i > 0 ? links[i - 1] : d.attach_start, b = i < d.links ? links[i] : d.attach_end;
    cw_joint3d j = { 0 };
    bool has_a = i > 0 || cw_body3d_valid(d.attach_start) || d.fix_start;
    bool has_b = i < d.links || cw_body3d_valid(d.attach_end);
    if (has_a && has_b) {
      cw_joint3d_def jd = cw_joint3d_default_def(CW_JOINT_SPHERICAL);
      if (i == d.links) { /* end: links[last] to attach_end */
        jd.a = b, jd.b = a;
      } else {
        jd.a = a, jd.b = b;
      }
      cw_joint3d_def_anchor(&jd, vadd(d.start, vmul(dir, seg * (float)i)), dir);
      j = cw_joint3d_create(w, &jd);
    }
    if (joints) joints[i] = j;
  }
  return d.links;
}
