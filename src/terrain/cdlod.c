/* CDLOD (Strugar, "Continuous Distance-Dependent Level of Detail for
 * Rendering Heightmaps", 2010; the algorithm, written here in C): the
 * quadtree's min / max pyramid and the per-frame patch selection.
 *
 * A node of level l covers G << l quads (G = grid_quads). Level l is used
 * within range[l] of the camera; a node whose box reaches into range[l - 1]
 * hands the children that reach it to the next level down and draws the
 * others itself as quarters (with a grid of G / 2, so their vertices stay
 * 2^l samples apart). The vertex stage morphs a level's odd vertices into
 * the next level's triangles over the end of its range (terrain_shading.mjs),
 * so where levels meet the finer one already has the coarser one's shape.
 * That needs neighbours to differ by one level at most, which the ranges
 * guarantee when each band is wider than a node's diagonal: cw__terrain_ranges
 * raises lod0_range to that.
 *
 * Threads: the top of the tree is walked on the calling thread until a
 * level has enough nodes, then those subtrees spread over the job pool,
 * each chunk into its own list. The lists are merged and sorted front to
 * back with a total order, so the result is the same at any worker count.
 * SIMD: a node's four children are tested against the frustum planes and
 * the range sphere together (GCC / clang vector extensions). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/jobs.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"
#include "terrain_internal.h"

/* ── the min / max pyramid ─────────────────────────────────────────── */

void cw__terrain_tree_init(cw_terrain *t) {
  int G = t->def.grid_quads;
  int maxl = 1;
  while (maxl < CW_TERRAIN_MAX_LODS && t->def.size_x % (G << maxl) == 0 && t->def.size_z % (G << maxl) == 0) maxl++;
  int L = t->def.lod_count > 0 ? CW_MIN(t->def.lod_count, maxl) : maxl;
  t->lods = L;
  t->def.lod_count = L;
  for (int l = 0; l < L; l++) {
    t->lnx[l] = t->def.size_x / (G << l);
    t->lnz[l] = t->def.size_z / (G << l);
    size_t n = (size_t)t->lnx[l] * t->lnz[l];
    t->lmin[l] = cw_alloc(n * sizeof(uint16_t), CW_MEM_RENDER);
    t->lmax[l] = cw_alloc(n * sizeof(uint16_t), CW_MEM_RENDER);
  }
  t->tree_dirty = true;
  cw__terrain_ranges(t);
}

void cw__terrain_tree_free(cw_terrain *t) {
  for (int l = 0; l < t->lods; l++) {
    cw_free(t->lmin[l]);
    cw_free(t->lmax[l]);
  }
  for (int i = 0; i < t->chunk_cap; i++) cw_array_free(&t->chunk_out[i]);
  cw_free(t->chunk_out);
  cw_free(t->chunk_counts);
  cw_array_free(&t->patches);
}

/* level 0: min / max over each node's (G + 1)^2 samples, eight at a time */
static void leaf_rows(int begin, int end, int worker, void *u) {
  (void)worker;
  cw_terrain *t = u;
  int G = t->def.grid_quads, nx0 = t->lnx[0];
  for (int nz = begin; nz < end; nz++)
    for (int nx = 0; nx < nx0; nx++) {
      tr_u16x8 vmn, vmx;
      for (int k = 0; k < 8; k++) vmn[k] = 0xFFFF, vmx[k] = 0;
      uint16_t smn = 0xFFFF, smx = 0;
      for (int z = nz * G; z <= nz * G + G; z++) {
        const uint16_t *row = &t->h[tr_idx(t, nx * G, z)];
        int i = 0;
        for (; i + 8 <= G + 1; i += 8) {
          tr_u16x8 v;
          memcpy(&v, row + i, sizeof v);
          vmn = tr_u16_min(v, vmn);
          vmx = tr_u16_max(v, vmx);
        }
        for (; i <= G; i++) {
          smn = row[i] < smn ? row[i] : smn;
          smx = row[i] > smx ? row[i] : smx;
        }
      }
      for (int k = 0; k < 8; k++) {
        smn = vmn[k] < smn ? vmn[k] : smn;
        smx = vmx[k] > smx ? vmx[k] : smx;
      }
      t->lmin[0][nz * nx0 + nx] = smn;
      t->lmax[0][nz * nx0 + nx] = smx;
    }
}

void cw__terrain_tree_rebuild(cw_terrain *t) {
  cw_parallel_for(t->lnz[0], 4, leaf_rows, t);
  for (int l = 1; l < t->lods; l++)
    for (int z = 0; z < t->lnz[l]; z++)
      for (int x = 0; x < t->lnx[l]; x++) {
        int cw = t->lnx[l - 1];
        int c00 = (2 * z) * cw + 2 * x, c10 = c00 + 1, c01 = c00 + cw, c11 = c01 + 1;
        const uint16_t *mn = t->lmin[l - 1], *mx = t->lmax[l - 1];
        uint16_t a = CW_MIN(CW_MIN(mn[c00], mn[c10]), CW_MIN(mn[c01], mn[c11]));
        uint16_t b = CW_MAX(CW_MAX(mx[c00], mx[c10]), CW_MAX(mx[c01], mx[c11]));
        t->lmin[l][z * t->lnx[l] + x] = a;
        t->lmax[l][z * t->lnx[l] + x] = b;
      }
  t->tree_dirty = false;
}

/* ranges: geometric, lod0_range raised so each band is wider than a node's
 * diagonal over the morph's share of it (at most one level between neighbours) */
void cw__terrain_ranges(cw_terrain *t) {
  float leaf = (float)t->def.grid_quads * t->def.vertex_spacing;
  float ratio = t->def.lod_ratio < 2 ? 2 : t->def.lod_ratio;
  float ms = cw_clampf(t->def.morph_start, 0.3f, 0.9f);
  t->def.lod_ratio = ratio;
  t->def.morph_start = ms;
  float need = 1.05f * 2.0f * 1.41421356f * leaf / (ms * (ratio - 1));
  float r0 = t->def.lod0_range > need ? t->def.lod0_range : need;
  t->def.lod0_range = r0;
  /* (repeated products, not powf: the selection is compared across hosts, and libm's powf differs) */
  float r = r0 * t->range_scale;
  for (int l = 0; l < t->lods; l++, r *= ratio) t->range[l] = r;
}

/* ── selection ──────────────────────────────────────────────────────── */

typedef struct sel_ctx {
  cw_terrain *t;
  float cam[3];
  float planes[6][4];
  bool cull;
  float far, vis2;           /* far distance; visible_range squared (0 = none) */
  int split;                 /* the level the frontier is taken at */
  CW_ARRAY(int32_t) frontier; /* (level, ix, iz, inside) packed as 4 ints */
  int visited, culled;
} sel_ctx;

typedef struct sel_out {
  tr_patch_array *out;
  int visited, culled;
} sel_out;

static void node_box(const cw_terrain *t, int l, int ix, int iz, float mn[3], float mx[3]) {
  float S = (float)(t->def.grid_quads << l) * t->def.vertex_spacing;
  mn[0] = t->def.origin.x + (float)ix * S;
  mx[0] = mn[0] + S;
  mn[2] = t->def.origin.z + (float)iz * S;
  mx[2] = mn[2] + S;
  int i = iz * t->lnx[l] + ix;
  mn[1] = tr_height_m(t, t->lmin[l][i]);
  mx[1] = tr_height_m(t, t->lmax[l][i]);
}

static float box_dist2(const float c[3], const float mn[3], const float mx[3]) {
  float d = 0;
  for (int k = 0; k < 3; k++) {
    float v = c[k] < mn[k] ? mn[k] - c[k] : c[k] > mx[k] ? c[k] - mx[k] : 0;
    d += v * v;
  }
  return d;
}

/* four boxes (SoA) against the planes: per box 0 outside, 1 crossing, 2 inside; and their squared distance */
static void test4(const sel_ctx *s, const tr_v4f mn[3], const tr_v4f mx[3], tr_v4i *state, tr_v4f *d2) {
  tr_v4i out = { 0, 0, 0, 0 }, cross = { 0, 0, 0, 0 };
  if (s->cull)
    for (int p = 0; p < 6; p++) {
      float nx = s->planes[p][0], ny = s->planes[p][1], nz = s->planes[p][2], d = s->planes[p][3];
      /* the corner farthest along the normal decides "outside", the nearest "crossing" */
      tr_v4f fx = nx >= 0 ? mx[0] : mn[0], fy = ny >= 0 ? mx[1] : mn[1], fz = nz >= 0 ? mx[2] : mn[2];
      tr_v4f gx = nx >= 0 ? mn[0] : mx[0], gy = ny >= 0 ? mn[1] : mx[1], gz = nz >= 0 ? mn[2] : mx[2];
      tr_v4f far = fx * nx + fy * ny + fz * nz + d;
      tr_v4f near = gx * nx + gy * ny + gz * nz + d;
      out |= (tr_v4i)(far < 0);
      cross |= (tr_v4i)(near < 0);
    }
  tr_v4f zero = { 0, 0, 0, 0 }, dd = zero;
  for (int k = 0; k < 3; k++) {
    tr_v4f lo = mn[k] - s->cam[k], hi = s->cam[k] - mx[k];
    tr_v4f v = tr_v4_max(tr_v4_max(lo, zero), hi);
    dd += v * v;
  }
  /* (vector compares give -1 for true) */
  for (int k = 0; k < 4; k++) (*state)[k] = out[k] ? 0 : cross[k] ? 1 : 2;
  *d2 = dd;
}

static void emit(const sel_ctx *s, tr_patch_array *out, int l, int x, int z, int size, int lod, bool half, float d2) {
  float d = sqrtf(d2);
  tr_patch p = { x, z, size, (int16_t)lod, (uint8_t)half, (uint8_t)(d > s->far), d };
  cw_array_push(out, p);
  (void)l;
}

/* select node (l, ix, iz); inside: an ancestor was wholly inside the frustum; frontier: defer at s->split */
static void select_node(sel_ctx *s, sel_out *o, int l, int ix, int iz, bool inside, float d2, bool defer) {
  cw_terrain *t = s->t;
  int G = t->def.grid_quads;
  int size = G << l, x = ix * size, z = iz * size;
  o->visited++;
  if (l == 0 || d2 > t->range[l - 1] * t->range[l - 1]) {
    emit(s, o->out, l, x, z, size, l, false, d2);
    return;
  }
  /* the four children: their boxes together */
  tr_v4f mn[3], mx[3];
  int cx[4], cz[4];
  for (int k = 0; k < 4; k++) {
    cx[k] = 2 * ix + (k & 1);
    cz[k] = 2 * iz + (k >> 1);
    float a[3], b[3];
    node_box(t, l - 1, cx[k], cz[k], a, b);
    for (int c = 0; c < 3; c++) mn[c][k] = a[c], mx[c][k] = b[c];
  }
  tr_v4i st;
  tr_v4f cd2;
  sel_ctx s2 = *s;
  s2.cull = s->cull && !inside;
  test4(&s2, mn, mx, &st, &cd2);
  float r2 = t->range[l - 1] * t->range[l - 1];
  for (int k = 0; k < 4; k++) {
    if (st[k] == 0 || (s->vis2 > 0 && cd2[k] > s->vis2)) {
      o->culled++;
      continue;
    }
    bool in = inside || st[k] == 2;
    if (cd2[k] <= r2) {
      if (defer && l - 1 <= s->split) {
        int32_t e[5] = { l - 1, cx[k], cz[k], in, 0 };
        float dk = cd2[k];
        memcpy(&e[4], &dk, sizeof(float));
        for (int q = 0; q < 5; q++) cw_array_push(&((sel_ctx *)s)->frontier, e[q]);
      } else {
        select_node(s, o, l - 1, cx[k], cz[k], in, cd2[k], defer);
      }
    } else {
      /* out of the finer level's range: this level draws the quarter */
      emit(s, o->out, l, cx[k] * (size / 2), cz[k] * (size / 2), size / 2, l, true, cd2[k]);
    }
  }
}

static int chunk_size(int count, int min_batch) {
  int c = (count + CW_JOB_CHUNKS - 1) / CW_JOB_CHUNKS;
  return c > min_batch ? c : min_batch;
}

typedef struct frontier_job {
  sel_ctx *s;
  int chunk;
} frontier_job;

static void frontier_range(int begin, int end, int worker, void *u) {
  (void)worker;
  frontier_job *j = u;
  sel_ctx *s = j->s;
  int ci = begin / j->chunk;
  sel_out o = { &s->t->chunk_out[ci], 0, 0 };
  o.out->count = 0;
  for (int i = begin; i < end; i++) {
    const int32_t *e = &s->frontier.data[5 * i];
    float d2;
    memcpy(&d2, &e[4], sizeof d2);
    select_node(s, &o, e[0], e[1], e[2], e[3] != 0, d2, false);
  }
  /* counters per chunk, summed in chunk order below */
  s->t->chunk_counts[2 * ci] = o.visited;
  s->t->chunk_counts[2 * ci + 1] = o.culled;
}

static int cmp_patch(const void *a, const void *b) {
  const tr_patch *p = a, *q = b;
  if (p->dist != q->dist) return p->dist < q->dist ? -1 : 1;
  if (p->lod != q->lod) return p->lod < q->lod ? -1 : 1;
  if (p->z != q->z) return p->z < q->z ? -1 : 1;
  if (p->x != q->x) return p->x < q->x ? -1 : 1;
  return (int)p->half - (int)q->half;
}

void cw__terrain_select(cw_terrain *t, t3_vec3 cam, const t3_mat4 *vp) {
  double t0 = tr_now_ms();
  if (t->tree_dirty) cw__terrain_tree_rebuild(t);
  sel_ctx s = { .t = t, .cam = { cam.x, cam.y, cam.z }, .cull = vp != NULL, .far = cw__terrain_far(t) };
  s.vis2 = t->def.visible_range > 0 ? t->def.visible_range * t->def.visible_range : 0;
  if (vp) {
    t3_frustum f;
    t3_frustum_from_matrix(&f, vp);
    for (int p = 0; p < 6; p++) {
      s.planes[p][0] = f.planes[p].normal.x;
      s.planes[p][1] = f.planes[p].normal.y;
      s.planes[p][2] = f.planes[p].normal.z;
      s.planes[p][3] = f.planes[p].constant;
    }
  }
  /* the frontier: the highest level with enough nodes to spread (or the leaves) */
  s.split = 0;
  for (int l = t->lods - 1; l >= 0; l--)
    if (t->lnx[l] * t->lnz[l] >= 64) {
      s.split = l;
      break;
    }
  t->patches.count = 0;
  sel_out top = { &t->patches, 0, 0 };
  int L = t->lods - 1;
  for (int iz = 0; iz < t->lnz[L]; iz++)
    for (int ix = 0; ix < t->lnx[L]; ix++) {
      float mn[3], mx[3];
      node_box(t, L, ix, iz, mn, mx);
      int st = 2;
      if (vp) {
        float (*pl)[4] = s.planes;
        for (int p = 0; p < 6; p++) {
          float far = pl[p][0] * (pl[p][0] >= 0 ? mx[0] : mn[0]) + pl[p][1] * (pl[p][1] >= 0 ? mx[1] : mn[1]) +
                      pl[p][2] * (pl[p][2] >= 0 ? mx[2] : mn[2]) + pl[p][3];
          float near = pl[p][0] * (pl[p][0] >= 0 ? mn[0] : mx[0]) + pl[p][1] * (pl[p][1] >= 0 ? mn[1] : mx[1]) +
                       pl[p][2] * (pl[p][2] >= 0 ? mn[2] : mx[2]) + pl[p][3];
          if (far < 0) st = 0;
          else if (near < 0 && st) st = 1;
        }
      }
      float d2 = box_dist2(s.cam, mn, mx);
      if (st == 0 || (s.vis2 > 0 && d2 > s.vis2)) {
        top.culled++;
        continue;
      }
      if (L <= s.split && L > 0) {
        int32_t e[5] = { L, ix, iz, st == 2, 0 };
        memcpy(&e[4], &d2, sizeof(float));
        for (int q = 0; q < 5; q++) cw_array_push(&s.frontier, e[q]);
      } else {
        select_node(&s, &top, L, ix, iz, st == 2, d2, true);
      }
    }
  int n = s.frontier.count / 5;
  int visited = top.visited, culled = top.culled;
  if (n) {
    int chunk = chunk_size(n, 4), chunks = (n + chunk - 1) / chunk;
    if (chunks > t->chunk_cap) {
      t->chunk_out = cw_realloc(t->chunk_out, sizeof(tr_patch_array) * (size_t)chunks, CW_MEM_RENDER);
      t->chunk_counts = cw_realloc(t->chunk_counts, sizeof(int) * 2 * (size_t)chunks, CW_MEM_RENDER);
      for (int i = t->chunk_cap; i < chunks; i++) memset(&t->chunk_out[i], 0, sizeof(tr_patch_array));
      t->chunk_cap = chunks;
    }
    frontier_job j = { &s, chunk };
    cw_parallel_for(n, chunk, frontier_range, &j);
    for (int c = 0; c < chunks; c++) {
      tr_patch_array *a = &t->chunk_out[c];
      visited += t->chunk_counts[2 * c];
      culled += t->chunk_counts[2 * c + 1];
      for (int i = 0; i < a->count; i++) cw_array_push(&t->patches, a->data[i]);
    }
  }
  cw_array_free(&s.frontier);
  if (t->patches.count > 1) qsort(t->patches.data, (size_t)t->patches.count, sizeof(tr_patch), cmp_patch);

  cw_terrain_stats *st = &t->stats;
  st->patches = st->patches_far = 0;
  memset(st->levels, 0, sizeof st->levels);
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < t->patches.count; i++) {
    const tr_patch *p = &t->patches.data[i];
    if (p->far) st->patches_far++;
    else st->patches++;
    st->levels[p->lod]++;
    int32_t key[4] = { p->x, p->z, p->size, p->lod * 2 + p->half };
    const uint8_t *b = (const uint8_t *)key;
    for (size_t k = 0; k < sizeof key; k++) h = (h ^ b[k]) * 1099511628211ull;
  }
  st->selection_hash = h;
  st->patches_culled = culled;
  st->nodes_visited = visited;
  st->select_ms = (float)(tr_now_ms() - t0);
}
