/* The lightmapper's BVH: median splits on the longest axis of the centroid
 * bounds (ties broken by triangle order, so every host builds the same
 * tree), at most 4 triangles a leaf; stack traversal, Moller-Trumbore. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lightmap_internal.h"

#define LEAF 4

typedef struct build_item { float c[3]; int idx; } build_item;

static void tri_bounds(const lm_tri *t, float lo[3], float hi[3]) {
  for (int a = 0; a < 3; a++) {
    float p0 = t->v0[a], p1 = t->v0[a] + t->e1[a], p2 = t->v0[a] + t->e2[a];
    float mn = p0 < p1 ? p0 : p1, mx = p0 > p1 ? p0 : p1;
    lo[a] = mn < p2 ? mn : p2;
    hi[a] = mx > p2 ? mx : p2;
  }
}

/* select the k-th element by (axis value, index): a total order, so the
 * partition is the same whatever the algorithm's path */
static int item_less(const build_item *a, const build_item *b, int axis) {
  if (a->c[axis] != b->c[axis]) return a->c[axis] < b->c[axis];
  return a->idx < b->idx;
}
static void nth_element(build_item *it, int lo, int hi, int k, int axis) {
  /* deterministic quickselect: median of three pivots by position */
  while (hi - lo > 1) {
    int mid = lo + (hi - lo) / 2;
    build_item a = it[lo], b = it[mid], c = it[hi - 1], pv;
    if (item_less(&a, &b, axis)) pv = item_less(&b, &c, axis) ? b : item_less(&a, &c, axis) ? c : a;
    else pv = item_less(&a, &c, axis) ? a : item_less(&b, &c, axis) ? c : b;
    int i = lo, j = hi - 1;
    while (i <= j) {
      while (item_less(&it[i], &pv, axis)) i++;
      while (item_less(&pv, &it[j], axis)) j--;
      if (i <= j) {
        build_item t = it[i];
        it[i] = it[j];
        it[j] = t;
        i++;
        j--;
      }
    }
    if (k <= j) hi = j + 1;
    else if (k >= i) lo = i;
    else return;
  }
}

typedef struct builder {
  lm_bvh *b;
  build_item *items;
  const lm_tri *src;
  int cap;
} builder;

static int new_node(builder *bd) {
  lm_bvh *b = bd->b;
  if (b->node_count == bd->cap) {
    bd->cap = bd->cap ? bd->cap * 2 : 256;
    b->nodes = cw_realloc(b->nodes, (size_t)bd->cap * sizeof *b->nodes, CW_MEM_RENDER);
  }
  return b->node_count++;
}

static void build(builder *bd, int ni, int lo, int hi) {
  lm_node *n = &bd->b->nodes[ni];
  float blo[3] = { 1e30f, 1e30f, 1e30f }, bhi[3] = { -1e30f, -1e30f, -1e30f };
  float clo[3] = { 1e30f, 1e30f, 1e30f }, chi[3] = { -1e30f, -1e30f, -1e30f };
  for (int i = lo; i < hi; i++) {
    float tl[3], th[3];
    tri_bounds(&bd->src[bd->items[i].idx], tl, th);
    for (int a = 0; a < 3; a++) {
      blo[a] = tl[a] < blo[a] ? tl[a] : blo[a];
      bhi[a] = th[a] > bhi[a] ? th[a] : bhi[a];
      float c = bd->items[i].c[a];
      clo[a] = c < clo[a] ? c : clo[a];
      chi[a] = c > chi[a] ? c : chi[a];
    }
  }
  memcpy(n->lo, blo, sizeof blo);
  memcpy(n->hi, bhi, sizeof bhi);
  if (hi - lo <= LEAF) {
    n->left = -1;
    n->first = lo;
    n->count = hi - lo;
    return;
  }
  int axis = 0;
  float ext = chi[0] - clo[0];
  for (int a = 1; a < 3; a++)
    if (chi[a] - clo[a] > ext) {
      ext = chi[a] - clo[a];
      axis = a;
    }
  int mid = lo + (hi - lo) / 2;
  nth_element(bd->items, lo, hi, mid, axis);
  int l = new_node(bd);
  new_node(bd);
  n = &bd->b->nodes[ni]; /* the array may have moved */
  n->left = l;
  n->first = n->count = 0;
  build(bd, l, lo, mid);
  build(bd, l + 1, mid, hi);
}

void lm_bvh_build(lm_bvh *b, lm_tri *tris, int n) {
  memset(b, 0, sizeof *b);
  build_item *items = cw_alloc((size_t)CW_MAX(n, 1) * sizeof *items, CW_MEM_RENDER);
  for (int i = 0; i < n; i++) {
    const lm_tri *t = &tris[i];
    for (int a = 0; a < 3; a++) items[i].c[a] = t->v0[a] + (t->e1[a] + t->e2[a]) * (1.0f / 3.0f);
    items[i].idx = i;
  }
  builder bd = { b, items, tris, 0 };
  int root = new_node(&bd);
  if (n) build(&bd, root, 0, n);
  else {
    b->nodes[root].left = -1;
    b->nodes[root].count = 0;
  }
  /* triangles in leaf order */
  b->tris = cw_alloc((size_t)CW_MAX(n, 1) * sizeof *b->tris, CW_MEM_RENDER);
  for (int i = 0; i < n; i++) b->tris[i] = tris[items[i].idx];
  b->tri_count = n;
  cw_free(items);
  cw_free(tris);
}

void lm_bvh_free(lm_bvh *b) {
  cw_free(b->tris);
  cw_free(b->nodes);
  memset(b, 0, sizeof *b);
}

static bool box_hit(const lm_node *n, const float o[3], const float inv[3], float tmin, float tmax) {
  for (int a = 0; a < 3; a++) {
    float t0 = (n->lo[a] - o[a]) * inv[a], t1 = (n->hi[a] - o[a]) * inv[a];
    if (t0 > t1) {
      float t = t0;
      t0 = t1;
      t1 = t;
    }
    tmin = t0 > tmin ? t0 : tmin;
    tmax = t1 < tmax ? t1 : tmax;
    if (tmax < tmin) return false;
  }
  return true;
}

/* Moller-Trumbore, both faces */
static bool tri_hit(const lm_tri *t, const float o[3], const float d[3], float tmin, float tmax, float *ot, float *ou,
                    float *ov, bool *back) {
  float p[3] = { d[1] * t->e2[2] - d[2] * t->e2[1], d[2] * t->e2[0] - d[0] * t->e2[2], d[0] * t->e2[1] - d[1] * t->e2[0] };
  float det = t->e1[0] * p[0] + t->e1[1] * p[1] + t->e1[2] * p[2];
  if (det > -1e-12f && det < 1e-12f) return false;
  float inv = 1.0f / det;
  float s[3] = { o[0] - t->v0[0], o[1] - t->v0[1], o[2] - t->v0[2] };
  float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
  if (u < 0 || u > 1) return false;
  float q[3] = { s[1] * t->e1[2] - s[2] * t->e1[1], s[2] * t->e1[0] - s[0] * t->e1[2], s[0] * t->e1[1] - s[1] * t->e1[0] };
  float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
  if (v < 0 || u + v > 1) return false;
  float tt = (t->e2[0] * q[0] + t->e2[1] * q[1] + t->e2[2] * q[2]) * inv;
  if (tt <= tmin || tt >= tmax) return false;
  *ot = tt;
  *ou = u;
  *ov = v;
  *back = det < 0;
  return true;
}

static void inv_dir(const float d[3], float inv[3]) {
  for (int a = 0; a < 3; a++) inv[a] = d[a] != 0 ? 1.0f / d[a] : (d[a] >= 0 ? 1e30f : -1e30f);
}

bool lm_bvh_closest(const lm_bvh *b, const float o[3], const float d[3], float tmin, float tmax, lm_hit *hit) {
  hit->tri = -1;
  if (!b->tri_count) return false;
  float inv[3];
  inv_dir(d, inv);
  int stack[64], sp = 0;
  stack[sp++] = 0;
  while (sp) {
    const lm_node *n = &b->nodes[stack[--sp]];
    if (!box_hit(n, o, inv, tmin, tmax)) continue;
    if (n->left < 0) {
      for (int i = n->first; i < n->first + n->count; i++) {
        float t, u, v;
        bool back;
        if (tri_hit(&b->tris[i], o, d, tmin, tmax, &t, &u, &v, &back)) {
          tmax = t;
          hit->t = t;
          hit->u = u;
          hit->v = v;
          hit->tri = i;
          hit->back = back;
        }
      }
    } else if (sp < 62) {
      stack[sp++] = n->left + 1;
      stack[sp++] = n->left;
    }
  }
  return hit->tri >= 0;
}

bool lm_bvh_any(const lm_bvh *b, const float o[3], const float d[3], float tmin, float tmax) {
  if (!b->tri_count) return false;
  float inv[3];
  inv_dir(d, inv);
  int stack[64], sp = 0;
  stack[sp++] = 0;
  while (sp) {
    const lm_node *n = &b->nodes[stack[--sp]];
    if (!box_hit(n, o, inv, tmin, tmax)) continue;
    if (n->left < 0) {
      for (int i = n->first; i < n->first + n->count; i++) {
        float t, u, v;
        bool back;
        if (tri_hit(&b->tris[i], o, d, tmin, tmax, &t, &u, &v, &back)) return true;
      }
    } else if (sp < 62) {
      stack[sp++] = n->left + 1;
      stack[sp++] = n->left;
    }
  }
  return false;
}

/* ── half floats (round to nearest even) ──────────────────────────────── */

uint16_t lm_float_to_half(float f) {
  uint32_t x;
  memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000u, mant = x & 0x7fffffu;
  int exp = (int)((x >> 23) & 0xffu);
  if (exp == 255) return (uint16_t)(sign | 0x7c00u | (mant ? 0x200u : 0));
  int e = exp - 127 + 15;
  if (e >= 31) return (uint16_t)(sign | 0x7c00u);
  if (e <= 0) {
    if (e < -10) return (uint16_t)sign;
    mant |= 0x800000u;
    int shift = 14 - e;
    uint32_t h = mant >> shift, rem = mant & ((1u << shift) - 1), half = 1u << (shift - 1);
    if (rem > half || (rem == half && (h & 1))) h++;
    return (uint16_t)(sign | h);
  }
  uint32_t h = ((uint32_t)e << 10) | (mant >> 13), rem = mant & 0x1fffu;
  if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) h++;
  return (uint16_t)(sign | h);
}

float lm_half_to_float(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 0x1fu, m = h & 0x3ffu, x;
  if (e == 0) {
    if (!m) x = sign;
    else {
      e = 1;
      while (!(m & 0x400u)) {
        m <<= 1;
        e--;
      }
      m &= 0x3ffu;
      x = sign | ((e + 127 - 15) << 23) | (m << 13);
    }
  } else if (e == 31) x = sign | 0x7f800000u | (m << 13);
  else x = sign | ((e + 127 - 15) << 23) | (m << 13);
  float f;
  memcpy(&f, &x, 4);
  return f;
}
