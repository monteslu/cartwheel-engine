/* Lightmap UVs: triangles flood-filled across shared edges into charts whose
 * normals stay within ~32 degrees of the chart's first triangle, each chart
 * projected onto the plane of its mean normal, charts shelf-packed tallest
 * first. Every ordering has a total tie-break, so the layout is the same on
 * every host. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lightmap_internal.h"

#define CHART_COS 0.85f

typedef struct edge {
  int64_t a[3], b[3]; /* quantized endpoints, a < b */
  int tri;
} edge;

static int cmp_i64x3(const int64_t *x, const int64_t *y) {
  for (int i = 0; i < 3; i++)
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}
static int edge_cmp(const void *pa, const void *pb) {
  const edge *x = pa, *y = pb;
  int c = cmp_i64x3(x->a, y->a);
  if (c) return c;
  c = cmp_i64x3(x->b, y->b);
  if (c) return c;
  return x->tri < y->tri ? -1 : x->tri > y->tri;
}

static void quant(const float *p, int64_t q[3]) {
  for (int i = 0; i < 3; i++) q[i] = (int64_t)llroundf(p[i] * 4096.0f);
}

typedef struct chart { int first, count; float w, h; int x, y; float minu, minv; int index; } chart;

static int chart_cmp(const void *pa, const void *pb) {
  const chart *x = pa, *y = pb;
  if (x->h != y->h) return x->h > y->h ? -1 : 1;
  if (x->w != y->w) return x->w > y->w ? -1 : 1;
  return x->index < y->index ? -1 : x->index > y->index;
}

static void cross3(const float *a, const float *b, float *o) {
  o[0] = a[1] * b[2] - a[2] * b[1];
  o[1] = a[2] * b[0] - a[0] * b[2];
  o[2] = a[0] * b[1] - a[1] * b[0];
}
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void norm3(float *a) {
  float l = sqrtf(dot3(a, a));
  if (l > 0) for (int i = 0; i < 3; i++) a[i] /= l;
}

void lm_unwrap_mesh(lm_unwrap *u, const float *pos, const float *nrm, int n, float density, int padding,
                    int min_size, int max_size) {
  (void)nrm;
  memset(u, 0, sizeof *u);
  u->tri_count = n;
  u->uv2 = cw_calloc((size_t)CW_MAX(n, 1) * 6, sizeof(float), CW_MEM_RENDER);
  u->tex = cw_calloc((size_t)CW_MAX(n, 1) * 6, sizeof(float), CW_MEM_RENDER);
  if (!n) return;
  /* face normals and areas */
  float *fn = cw_alloc((size_t)n * 3 * sizeof(float), CW_MEM_RENDER);
  float *area = cw_alloc((size_t)n * sizeof(float), CW_MEM_RENDER);
  for (int t = 0; t < n; t++) {
    const float *p = pos + t * 9;
    float e1[3] = { p[3] - p[0], p[4] - p[1], p[5] - p[2] }, e2[3] = { p[6] - p[0], p[7] - p[1], p[8] - p[2] };
    cross3(e1, e2, fn + t * 3);
    area[t] = 0.5f * sqrtf(dot3(fn + t * 3, fn + t * 3));
    norm3(fn + t * 3);
  }
  /* shared edges */
  edge *edges = cw_alloc((size_t)n * 3 * sizeof *edges, CW_MEM_RENDER);
  for (int t = 0; t < n; t++)
    for (int k = 0; k < 3; k++) {
      edge *e = &edges[t * 3 + k];
      int64_t qa[3], qb[3];
      quant(pos + t * 9 + k * 3, qa);
      quant(pos + t * 9 + ((k + 1) % 3) * 3, qb);
      if (cmp_i64x3(qa, qb) <= 0) {
        memcpy(e->a, qa, sizeof qa);
        memcpy(e->b, qb, sizeof qb);
      } else {
        memcpy(e->a, qb, sizeof qb);
        memcpy(e->b, qa, sizeof qa);
      }
      e->tri = t;
    }
  qsort(edges, (size_t)n * 3, sizeof *edges, edge_cmp);
  /* neighbours per triangle (up to 3 x a few), from runs of equal edges */
  int *nb_start = cw_calloc((size_t)n + 1, sizeof(int), CW_MEM_RENDER);
  int ne = n * 3;
  for (int i = 0; i < ne;) {
    int j = i;
    while (j < ne && !cmp_i64x3(edges[j].a, edges[i].a) && !cmp_i64x3(edges[j].b, edges[i].b)) j++;
    for (int x = i; x < j; x++) nb_start[edges[x].tri + 1] += j - i - 1;
    i = j;
  }
  for (int t = 0; t < n; t++) nb_start[t + 1] += nb_start[t];
  int *nb = cw_alloc((size_t)CW_MAX(nb_start[n], 1) * sizeof(int), CW_MEM_RENDER);
  int *fill = cw_alloc((size_t)n * sizeof(int), CW_MEM_RENDER);
  for (int t = 0; t < n; t++) fill[t] = nb_start[t];
  for (int i = 0; i < ne;) {
    int j = i;
    while (j < ne && !cmp_i64x3(edges[j].a, edges[i].a) && !cmp_i64x3(edges[j].b, edges[i].b)) j++;
    for (int x = i; x < j; x++)
      for (int y = i; y < j; y++)
        if (x != y) nb[fill[edges[x].tri]++] = edges[y].tri;
    i = j;
  }
  cw_free(edges);
  /* flood fill */
  int *chart_of = cw_alloc((size_t)n * sizeof(int), CW_MEM_RENDER);
  for (int t = 0; t < n; t++) chart_of[t] = -1;
  int *order = cw_alloc((size_t)n * sizeof(int), CW_MEM_RENDER); /* triangles grouped by chart */
  CW_ARRAY(chart) charts = { 0 };
  int at = 0;
  for (int s = 0; s < n; s++) {
    if (chart_of[s] >= 0) continue;
    chart c = { at, 0, 0, 0, 0, 0, 0, 0, charts.count };
    const float *n0 = fn + s * 3;
    int head = at;
    order[at++] = s;
    chart_of[s] = charts.count;
    while (head < at) {
      int t = order[head++];
      for (int k = nb_start[t]; k < nb_start[t + 1]; k++) {
        int o = nb[k];
        if (chart_of[o] >= 0 || dot3(fn + o * 3, n0) < CHART_COS) continue;
        chart_of[o] = charts.count;
        order[at++] = o;
      }
    }
    c.count = at - c.first;
    cw_array_push(&charts, c);
  }
  /* each chart on its plane (in world units; the density scales below) */
  float *loc = cw_alloc((size_t)n * 6 * sizeof(float), CW_MEM_RENDER);
  for (int ci = 0; ci < charts.count; ci++) {
    chart *c = &charts.data[ci];
    float N[3] = { 0, 0, 0 };
    for (int i = c->first; i < c->first + c->count; i++)
      for (int a = 0; a < 3; a++) N[a] += fn[order[i] * 3 + a] * area[order[i]];
    norm3(N);
    if (dot3(N, N) < 0.5f) memcpy(N, fn + order[c->first] * 3, sizeof N);
    if (dot3(N, N) < 0.5f) N[1] = 1;
    float ref[3] = { 0, 1, 0 };
    if (fabsf(N[1]) > 0.9f) { ref[1] = 0; ref[0] = 1; }
    float U[3], V[3];
    cross3(ref, N, U);
    norm3(U);
    cross3(N, U, V);
    float mu = 1e30f, mv = 1e30f, xu = -1e30f, xv = -1e30f;
    for (int i = c->first; i < c->first + c->count; i++) {
      int t = order[i];
      for (int k = 0; k < 3; k++) {
        const float *p = pos + t * 9 + k * 3;
        float pu = dot3(p, U), pv = dot3(p, V);
        loc[t * 6 + k * 2] = pu;
        loc[t * 6 + k * 2 + 1] = pv;
        mu = pu < mu ? pu : mu;
        mv = pv < mv ? pv : mv;
        xu = pu > xu ? pu : xu;
        xv = pv > xv ? pv : xv;
      }
    }
    c->minu = mu;
    c->minv = mv;
    c->w = xu - mu; /* world units for now */
    c->h = xv - mv;
  }
  /* pack, lowering the density until the charts fit */
  float d = density;
  int W = 0, H = 0;
  chart *sorted = cw_alloc((size_t)charts.count * sizeof *sorted, CW_MEM_RENDER);
  for (int attempt = 0; attempt < 40; attempt++) {
    double total = 0;
    float widest = 0;
    for (int ci = 0; ci < charts.count; ci++) {
      chart *c = &charts.data[ci];
      sorted[ci] = *c;
      sorted[ci].w = ceilf(c->w * d) + 1 + 2 * (float)padding;
      sorted[ci].h = ceilf(c->h * d) + 1 + 2 * (float)padding;
      total += (double)sorted[ci].w * sorted[ci].h;
      widest = sorted[ci].w > widest ? sorted[ci].w : widest;
    }
    qsort(sorted, (size_t)charts.count, sizeof *sorted, chart_cmp);
    W = (int)ceil(sqrt(total * 1.25));
    if (W < (int)widest) W = (int)widest;
    W = (W + 3) & ~3;
    if (W < min_size) W = min_size;
    int x = 0, y = 0, shelf = 0;
    for (int ci = 0; ci < charts.count; ci++) {
      chart *c = &sorted[ci];
      if (x + (int)c->w > W) {
        y += shelf;
        x = 0;
        shelf = 0;
      }
      c->x = x;
      c->y = y;
      x += (int)c->w;
      shelf = (int)c->h > shelf ? (int)c->h : shelf;
    }
    H = y + shelf;
    H = (H + 3) & ~3;
    if (H < min_size) H = min_size;
    if (W <= max_size && H <= max_size) break;
    d *= 0.8f;
  }
  if (d < density) cw_warn("lightmap: a mesh's charts did not fit %d texels at density %.3g; baked at %.3g", max_size, density, d);
  u->width = W;
  u->height = H;
  u->charts = charts.count;
  for (int ci = 0; ci < charts.count; ci++) {
    const chart *c = &sorted[ci];
    const chart *src = &charts.data[c->index];
    for (int i = src->first; i < src->first + src->count; i++) {
      int t = order[i];
      for (int k = 0; k < 3; k++) {
        float tx = (float)c->x + (float)padding + 0.5f + (loc[t * 6 + k * 2] - src->minu) * d;
        float ty = (float)c->y + (float)padding + 0.5f + (loc[t * 6 + k * 2 + 1] - src->minv) * d;
        u->tex[t * 6 + k * 2] = tx;
        u->tex[t * 6 + k * 2 + 1] = ty;
        u->uv2[t * 6 + k * 2] = tx / (float)W;
        u->uv2[t * 6 + k * 2 + 1] = ty / (float)H;
      }
    }
  }
  cw_free(sorted);
  cw_free(loc);
  cw_array_free(&charts);
  cw_free(order);
  cw_free(chart_of);
  cw_free(fill);
  cw_free(nb);
  cw_free(nb_start);
  cw_free(area);
  cw_free(fn);
}

void lm_unwrap_free(lm_unwrap *u) {
  cw_free(u->uv2);
  cw_free(u->tex);
  memset(u, 0, sizeof *u);
}
