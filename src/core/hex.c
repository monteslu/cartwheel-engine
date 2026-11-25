/* Hex grids (cartwheel/hex.h): axial coordinates, shapes, layouts in 2D and 3D, and the hex map. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/hex.h"

static const cw_axial DIRS[6] = { { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, 0 }, { -1, 1 }, { 0, 1 } };
static const cw_axial DIAGS[6] = { { 2, -1 }, { 1, -2 }, { -1, -1 }, { -2, 1 }, { -1, 2 }, { 1, 1 } };

static int mod6(int d) { return ((d % 6) + 6) % 6; }

cw_axial cw_hex_direction(int dir) { return DIRS[mod6(dir)]; }
cw_axial cw_hex_neighbor(cw_axial h, int dir) { return cw_hex_add(h, DIRS[mod6(dir)]); }
cw_axial cw_hex_diagonal(int d) { return DIAGS[mod6(d)]; }
cw_axial cw_hex_diagonal_neighbor(cw_axial h, int d) { return cw_hex_add(h, DIAGS[mod6(d)]); }

int cw_hex_length(cw_axial h) { return (abs(h.q) + abs(h.r) + abs(cw_hex_s(h))) / 2; }
int cw_hex_distance(cw_axial a, cw_axial b) { return cw_hex_length(cw_hex_sub(a, b)); }

int cw_hex_direction_to(cw_axial from, cw_axial to) {
  cw_axial d = cw_hex_sub(to, from);
  if (!d.q && !d.r) return -1;
  int ds = cw_hex_s(d), best = 0;
  long long best_dot = 0;
  for (int i = 0; i < 6; i++) { /* the largest cube dot product: the closest direction (integers, so exact) */
    long long dot = (long long)d.q * DIRS[i].q + (long long)d.r * DIRS[i].r + (long long)ds * cw_hex_s(DIRS[i]);
    if (i == 0 || dot > best_dot) best_dot = dot, best = i;
  }
  return best;
}

cw_axial cw_hex_rotate(cw_axial h, cw_axial center, int steps) {
  cw_axial v = cw_hex_sub(h, center);
  int n = mod6(steps);
  for (int i = 0; i < n; i++) { /* 60 degrees counterclockwise: cube (q, r, s) -> (-s, -q, -r) */
    int q = v.q, r = v.r, s = cw_hex_s(v);
    (void)r;
    v = (cw_axial){ -s, -q };
  }
  return cw_hex_add(center, v);
}

cw_axial cw_hex_reflect(cw_axial h, cw_axial center, cw_hex_axis axis) {
  cw_axial v = cw_hex_sub(h, center);
  int q = v.q, r = v.r, s = cw_hex_s(v);
  switch (axis) {
  case CW_HEX_AXIS_Q: v = (cw_axial){ q, s }; break;  /* (q, s, r) */
  case CW_HEX_AXIS_R: v = (cw_axial){ s, r }; break;  /* (s, r, q) */
  case CW_HEX_AXIS_S: v = (cw_axial){ r, q }; break;  /* (r, q, s) */
  default: break;
  }
  return cw_hex_add(center, v);
}

cw_axial cw_hex_round(cw_axialf f) {
  float fs = -f.q - f.r;
  float q = roundf(f.q), r = roundf(f.r), s = roundf(fs);
  float dq = fabsf(q - f.q), dr = fabsf(r - f.r), ds = fabsf(s - fs);
  if (dq > dr && dq > ds) q = -r - s;
  else if (dr > ds) r = -q - s;
  return (cw_axial){ (int)q, (int)r };
}

cw_axialf cw_hex_lerp(cw_axial a, cw_axial b, float t) {
  return (cw_axialf){ (float)a.q + (float)(b.q - a.q) * t, (float)a.r + (float)(b.r - a.r) * t };
}

/* ── shapes ─────────────────────────────────────────────────────────── */

static void put(cw_axial *out, int cap, int *n, cw_axial h) {
  if (out && *n < cap) out[*n] = h;
  (*n)++;
}

int cw_hex_line(cw_axial a, cw_axial b, cw_axial *out, int cap) {
  int dist = cw_hex_distance(a, b), n = 0;
  /* nudged off the edges so a line along a boundary goes the same way every time */
  float aq = (float)a.q + 1e-6f, ar = (float)a.r + 2e-6f, bq = (float)b.q + 1e-6f, br = (float)b.r + 2e-6f;
  for (int i = 0; i <= dist; i++) {
    float t = dist ? (float)i / (float)dist : 0.0f;
    put(out, cap, &n, cw_hex_round((cw_axialf){ aq + (bq - aq) * t, ar + (br - ar) * t }));
  }
  return n;
}

int cw_hex_ring(cw_axial center, int radius, cw_axial *out, int cap) {
  int n = 0;
  if (radius < 0) return 0;
  if (radius == 0) {
    put(out, cap, &n, center);
    return n;
  }
  cw_axial h = cw_hex_add(center, cw_hex_scale(DIRS[4], radius));
  for (int i = 0; i < 6; i++)
    for (int j = 0; j < radius; j++) {
      put(out, cap, &n, h);
      h = cw_hex_neighbor(h, i);
    }
  return n;
}

int cw_hex_spiral(cw_axial center, int radius, cw_axial *out, int cap) {
  int n = 0;
  for (int k = 0; k <= radius; k++) {
    int at = n < cap ? n : cap;
    n += cw_hex_ring(center, k, out ? out + at : NULL, cap - at);
  }
  return n;
}

int cw_hex_reachable(cw_axial start, int steps, bool (*passable)(cw_axial h, void *user), void *user, cw_axial *out, int cap) {
  cw_hex_map *seen = cw_hex_map_new(0);
  /* the frontier is kept whole (not only what fits in out) so the count is right with a small cap */
  CW_ARRAY(cw_axial) q = { 0 };
  cw_array_push(&q, start);
  cw_hex_map_set(seen, start, NULL);
  int head = 0, n = 0;
  for (int d = 0; d <= steps && head < q.count; d++) {
    int end = q.count;
    for (; head < end; head++) {
      cw_axial h = q.data[head];
      put(out, cap, &n, h);
      if (d == steps) continue;
      for (int i = 0; i < 6; i++) {
        cw_axial nb = cw_hex_neighbor(h, i);
        if (cw_hex_map_has(seen, nb)) continue;
        cw_hex_map_set(seen, nb, NULL);
        if (passable && !passable(nb, user)) continue;
        cw_array_push(&q, nb);
      }
    }
  }
  cw_array_free(&q);
  cw_hex_map_free(seen);
  return n;
}

/* ── offset coordinates ─────────────────────────────────────────────── */

void cw_hex_to_offset(cw_axial h, cw_hex_offset_kind kind, int *col, int *row) {
  int c = 0, r = 0;
  switch (kind) {
  case CW_HEX_ODD_R: c = h.q + (h.r - (h.r & 1)) / 2, r = h.r; break;
  case CW_HEX_EVEN_R: c = h.q + (h.r + (h.r & 1)) / 2, r = h.r; break;
  case CW_HEX_ODD_Q: c = h.q, r = h.r + (h.q - (h.q & 1)) / 2; break;
  case CW_HEX_EVEN_Q: c = h.q, r = h.r + (h.q + (h.q & 1)) / 2; break;
  }
  if (col) *col = c;
  if (row) *row = r;
}

cw_axial cw_hex_from_offset(int col, int row, cw_hex_offset_kind kind) {
  switch (kind) {
  case CW_HEX_ODD_R: return (cw_axial){ col - (row - (row & 1)) / 2, row };
  case CW_HEX_EVEN_R: return (cw_axial){ col - (row + (row & 1)) / 2, row };
  case CW_HEX_ODD_Q: return (cw_axial){ col, row - (col - (col & 1)) / 2 };
  case CW_HEX_EVEN_Q: return (cw_axial){ col, row - (col + (col & 1)) / 2 };
  }
  return (cw_axial){ col, row };
}

/* ── positions ──────────────────────────────────────────────────────── */

#define SQRT3 1.7320508075688772f
/* forward (hex to point) and back (point to hex) matrices, and the corners' unit offsets, per orientation */
static const float FWD[2][4] = { { SQRT3, SQRT3 / 2.0f, 0.0f, 1.5f }, { 1.5f, 0.0f, SQRT3 / 2.0f, SQRT3 } };
static const float BACK[2][4] = { { SQRT3 / 3.0f, -1.0f / 3.0f, 0.0f, 2.0f / 3.0f }, { 2.0f / 3.0f, 0.0f, -1.0f / 3.0f, SQRT3 / 3.0f } };
static const float CORNER[2][6][2] = {
  { { SQRT3 / 2, 0.5f }, { 0, 1 }, { -SQRT3 / 2, 0.5f }, { -SQRT3 / 2, -0.5f }, { 0, -1 }, { SQRT3 / 2, -0.5f } }, /* 30 + 60 i */
  { { 1, 0 }, { 0.5f, SQRT3 / 2 }, { -0.5f, SQRT3 / 2 }, { -1, 0 }, { -0.5f, -SQRT3 / 2 }, { 0.5f, -SQRT3 / 2 } },  /* 60 i */
};

static int orient(const cw_hex_layout *l) { return l->orientation == CW_HEX_FLAT ? 1 : 0; }
static float sz(float v) { return v != 0.0f ? v : 1.0f; }

cw_vec2 cw_hex_to_point(const cw_hex_layout *l, cw_axial h) {
  const float *f = FWD[orient(l)];
  float x = (f[0] * (float)h.q + f[1] * (float)h.r) * sz(l->size.x);
  float y = (f[2] * (float)h.q + f[3] * (float)h.r) * sz(l->size.y);
  return (cw_vec2){ x + l->origin.x, y + l->origin.y };
}

cw_axialf cw_hex_from_point_frac(const cw_hex_layout *l, cw_vec2 p) {
  const float *b = BACK[orient(l)];
  float x = (p.x - l->origin.x) / sz(l->size.x), y = (p.y - l->origin.y) / sz(l->size.y);
  return (cw_axialf){ b[0] * x + b[1] * y, b[2] * x + b[3] * y };
}

cw_axial cw_hex_from_point(const cw_hex_layout *l, cw_vec2 p) { return cw_hex_round(cw_hex_from_point_frac(l, p)); }

cw_vec2 cw_hex_corner(const cw_hex_layout *l, cw_axial h, int corner) {
  cw_vec2 c = cw_hex_to_point(l, h);
  const float *o = CORNER[orient(l)][mod6(corner)];
  return (cw_vec2){ c.x + o[0] * sz(l->size.x), c.y + o[1] * sz(l->size.y) };
}

void cw_hex_corners(const cw_hex_layout *l, cw_axial h, cw_vec2 out[6]) {
  for (int i = 0; i < 6; i++) out[i] = cw_hex_corner(l, h, i);
}

t3_vec3 cw_hex_to_world(const cw_hex_layout *l, cw_axial h, float y) {
  cw_vec2 p = cw_hex_to_point(l, h);
  return (t3_vec3){ p.x, y, p.y };
}
cw_axialf cw_hex_from_world_frac(const cw_hex_layout *l, t3_vec3 p) { return cw_hex_from_point_frac(l, (cw_vec2){ p.x, p.z }); }
cw_axial cw_hex_from_world(const cw_hex_layout *l, t3_vec3 p) { return cw_hex_round(cw_hex_from_world_frac(l, p)); }
t3_vec3 cw_hex_world_corner(const cw_hex_layout *l, cw_axial h, int corner, float y) {
  cw_vec2 p = cw_hex_corner(l, h, corner);
  return (t3_vec3){ p.x, y, p.y };
}

/* ── the map: open addressing, linear probing, backward-shift removal ── */

struct cw_hex_map {
  size_t vsize;
  int count, cap; /* cap a power of two, or 0 */
  cw_axial *keys;
  uint8_t *used;
  uint8_t *vals;
};

static uint32_t slot_of(cw_axial h, int cap) {
  uint32_t x = (uint32_t)h.q * 0x9E3779B1u ^ ((uint32_t)h.r * 0x85EBCA77u + 0x165667B1u);
  x ^= x >> 15, x *= 0x2C1B3C6Du, x ^= x >> 12;
  return x & (uint32_t)(cap - 1);
}

cw_hex_map *cw_hex_map_new(size_t value_size) {
  cw_hex_map *m = cw_calloc(1, sizeof *m, CW_MEM_GAME);
  m->vsize = value_size;
  return m;
}

void cw_hex_map_free(cw_hex_map *m) {
  if (!m) return;
  cw_free(m->keys);
  cw_free(m->used);
  cw_free(m->vals);
  cw_free(m);
}

static int find(const cw_hex_map *m, cw_axial h) {
  if (!m || !m->cap) return -1;
  for (uint32_t i = slot_of(h, m->cap);; i = (i + 1) & (uint32_t)(m->cap - 1)) {
    if (!m->used[i]) return -1;
    if (cw_hex_eq(m->keys[i], h)) return (int)i;
  }
}

static void grow(cw_hex_map *m) {
  int old_cap = m->cap;
  cw_axial *ok = m->keys;
  uint8_t *ou = m->used, *ov = m->vals;
  m->cap = old_cap ? old_cap * 2 : 16;
  m->keys = cw_alloc(sizeof(cw_axial) * (size_t)m->cap, CW_MEM_GAME);
  m->used = cw_calloc((size_t)m->cap, 1, CW_MEM_GAME);
  m->vals = m->vsize ? cw_alloc(m->vsize * (size_t)m->cap, CW_MEM_GAME) : NULL;
  for (int j = 0; j < old_cap; j++) {
    if (!ou[j]) continue;
    uint32_t i = slot_of(ok[j], m->cap);
    while (m->used[i]) i = (i + 1) & (uint32_t)(m->cap - 1);
    m->used[i] = 1;
    m->keys[i] = ok[j];
    if (m->vsize) memcpy(m->vals + m->vsize * i, ov + m->vsize * (size_t)j, m->vsize);
  }
  cw_free(ok);
  cw_free(ou);
  cw_free(ov);
}

void *cw_hex_map_set(cw_hex_map *m, cw_axial h, const void *value) {
  if (!m) return NULL;
  int at = find(m, h);
  if (at < 0) {
    if ((m->count + 1) * 4 > m->cap * 3) grow(m); /* at most three quarters full */
    uint32_t i = slot_of(h, m->cap);
    while (m->used[i]) i = (i + 1) & (uint32_t)(m->cap - 1);
    m->used[i] = 1;
    m->keys[i] = h;
    m->count++;
    at = (int)i;
  }
  if (!m->vsize) return m->keys + at; /* a set: a non-NULL pointer that says it is there */
  uint8_t *v = m->vals + m->vsize * (size_t)at;
  if (value) memmove(v, value, m->vsize);
  else memset(v, 0, m->vsize);
  return v;
}

void *cw_hex_map_get(const cw_hex_map *m, cw_axial h) {
  int at = find(m, h);
  if (at < 0) return NULL;
  return m->vsize ? (void *)(m->vals + m->vsize * (size_t)at) : (void *)(m->keys + at);
}

bool cw_hex_map_has(const cw_hex_map *m, cw_axial h) { return find(m, h) >= 0; }

bool cw_hex_map_remove(cw_hex_map *m, cw_axial h) {
  int at = find(m, h);
  if (at < 0) return false;
  uint32_t mask = (uint32_t)(m->cap - 1), i = (uint32_t)at, j = i;
  m->used[i] = 0;
  for (;;) { /* shift back the entries after the hole that probed past it */
    j = (j + 1) & mask;
    if (!m->used[j]) break;
    uint32_t home = slot_of(m->keys[j], m->cap);
    bool between = i <= j ? (home > i && home <= j) : (home > i || home <= j);
    if (between) continue;
    m->keys[i] = m->keys[j];
    if (m->vsize) memcpy(m->vals + m->vsize * i, m->vals + m->vsize * j, m->vsize);
    m->used[i] = 1;
    m->used[j] = 0;
    i = j;
  }
  m->count--;
  return true;
}

int cw_hex_map_count(const cw_hex_map *m) { return m ? m->count : 0; }
size_t cw_hex_map_value_size(const cw_hex_map *m) { return m ? m->vsize : 0; }

void cw_hex_map_clear(cw_hex_map *m) {
  if (!m) return;
  if (m->cap) memset(m->used, 0, (size_t)m->cap);
  m->count = 0;
}

static int key_cmp(const void *a, const void *b) {
  const cw_axial *x = a, *y = b;
  if (x->r != y->r) return x->r < y->r ? -1 : 1;
  return x->q < y->q ? -1 : x->q > y->q;
}

/* every key, sorted (cw_alloc'd; *n the count) */
static cw_axial *sorted_keys(const cw_hex_map *m, int *n) {
  *n = 0;
  if (!m || !m->count) return NULL;
  cw_axial *k = cw_alloc(sizeof(cw_axial) * (size_t)m->count, CW_MEM_GAME);
  for (int i = 0; i < m->cap; i++)
    if (m->used[i]) k[(*n)++] = m->keys[i];
  qsort(k, (size_t)*n, sizeof *k, key_cmp);
  return k;
}

int cw_hex_map_keys(const cw_hex_map *m, cw_axial *out, int cap) {
  int n;
  cw_axial *k = sorted_keys(m, &n);
  if (out && cap > 0) memcpy(out, k, sizeof(cw_axial) * (size_t)(n < cap ? n : cap));
  cw_free(k);
  return n;
}

uint32_t cw_hex_map_hash(const cw_hex_map *m, uint32_t seed) {
  int n;
  cw_axial *k = sorted_keys(m, &n);
  uint32_t h = seed;
  for (int i = 0; i < n; i++) {
    int32_t qr[2] = { k[i].q, k[i].r };
    uint8_t b[8];
    for (int j = 0; j < 8; j++) b[j] = (uint8_t)((uint32_t)qr[j / 4] >> (8 * (j % 4))); /* little endian on every host */
    h = cw_hash32(b, 8, h);
    if (m->vsize) h = cw_hash32(cw_hex_map_get(m, k[i]), m->vsize, h);
  }
  cw_free(k);
  return h;
}
