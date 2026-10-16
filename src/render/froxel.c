/* Froxel grid (cartwheel/froxel.h): the cells of a view, CPU binning of
 * spheres and cones on the job pool, the compacted lists and their textures.
 *
 * Binning (Filament's Froxelizer, the algorithm): per depth slice, a volume's
 * sphere is cut down to the part inside the slice (a sphere of radius
 * sqrt(r^2 - dz^2) around the centre moved into the slice, which holds that
 * part), and the columns and rows of tiles it reaches come from its signed
 * distances to the tile planes (planes through the tile edges, found by
 * binary search: the distances fall monotonically across the screen). Every
 * cell of that rectangle gets the volume; a cone also has to pass a
 * cone-vs-sphere test against the cell's bounding sphere. Slices are binned
 * in parallel, each writing only its own cells and taking the volumes in
 * input order, so the lists never depend on the worker count. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/core.h"
#include "cartwheel/froxel.h"
#include "cartwheel/jobs.h"
#include "cartwheel/math.h"
#include "cartwheel/profile.h"
#include "three.h"

#define RECORDS_W 1024 /* texels a row of the records texture */
#define ITEMS_W 1024   /* texels a row of the items texture (two items a texel) */

typedef struct plane { float x, y, z, d; } plane;

/* one volume as binned */
typedef struct vol {
  float cx, cy, cz, r;  /* the sphere binned (a cone's bounding sphere) */
  int k0, k1;           /* slices it reaches; k1 < k0: none */
  bool cone;
  float px, py, pz;     /* cone apex, axis, angle */
  float ax, ay, az, cosa, sina, len;
} vol;

struct cw_froxel_grid {
  cw_froxel_config cfg;
  int w, h, nx, ny, nz;
  float proj[16], near, far, zscale, zbias;
  bool ready;
  int cells;
  plane *colp, *rowp;   /* nx + 1 column planes (normals toward +x), ny + 1 row planes (toward +y) */
  float *slice;         /* nz + 1 slice depths */
  float *corner;        /* per tile corner: two points on its line of sight (6 floats) */
  float *sph;           /* per cell: bounding sphere (cone tests) */
  /* binning */
  uint16_t *scratch;    /* per cell: max_per_froxel indices */
  uint16_t *count;      /* per cell: volumes that touched it (may exceed max) */
  size_t scratch_cap;
  int count_cap;
  vol *vols;
  int vols_cap;
  int *slice_start, *slice_vols, slice_vols_cap; /* per slice: the volumes reaching it, in input order */
  int *sl;              /* per slice: references, overflowed cells, dropped, max in a cell, first item (5 ints) */
  uint32_t *records;
  uint16_t *items;
  int items_cap, item_count;
  cw_froxel_stats stats;
  t3_texture *rec_tex, *item_tex;
  bool tex_dirty;
};

cw_froxel_grid *cw_froxel_grid_new(void) {
  cw_froxel_grid *g = cw_calloc(1, sizeof *g, CW_MEM_RENDER);
  g->cfg = CW_FROXEL_CONFIG_DEFAULT;
  return g;
}

void cw_froxel_grid_free(cw_froxel_grid *g) {
  if (!g) return;
  cw_free(g->colp);
  cw_free(g->rowp);
  cw_free(g->slice);
  cw_free(g->corner);
  cw_free(g->sph);
  cw_free(g->scratch);
  cw_free(g->count);
  cw_free(g->vols);
  cw_free(g->slice_start);
  cw_free(g->slice_vols);
  cw_free(g->sl);
  cw_free(g->records);
  cw_free(g->items);
  if (g->rec_tex) t3_release(g->rec_tex);
  if (g->item_tex) t3_release(g->item_tex);
  cw_free(g);
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

/* view-space point of NDC (x, y, z) through the inverse projection */
static void unproject(const t3_mat4 *ip, float x, float y, float z, float out[3]) {
  const float *m = ip->e;
  float v[4];
  for (int i = 0; i < 4; i++) v[i] = m[i] * x + m[4 + i] * y + m[8 + i] * z + m[12 + i];
  out[0] = v[0] / v[3];
  out[1] = v[1] / v[3];
  out[2] = v[2] / v[3];
}

/* the plane through three points, its normal toward `toward` (normalized: distances are metric) */
static plane plane_of(const float a[3], const float b[3], const float c[3], const float toward[3]) {
  float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
  float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
  float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (l <= 0) l = 1;
  plane p = { n[0] / l, n[1] / l, n[2] / l, 0 };
  p.d = -(p.x * a[0] + p.y * a[1] + p.z * a[2]);
  if (p.x * toward[0] + p.y * toward[1] + p.z * toward[2] + p.d < 0) p = (plane){ -p.x, -p.y, -p.z, -p.d };
  return p;
}

static inline float pdist(const plane *p, float x, float y, float z) { return p->x * x + p->y * y + p->z * z + p->d; }

float cw_froxel_slice_depth(const cw_froxel_grid *g, int k) {
  if (k <= 0) return g->near;
  if (k >= g->nz) return g->far;
  return g->slice[k];
}

/* a cell's 8 corners: tile corner lines cut at its slice's depths */
static void cell_corners(const cw_froxel_grid *g, int i, int j, int k, float out[8][3]) {
  float d[2] = { cw_froxel_slice_depth(g, k), cw_froxel_slice_depth(g, k + 1) };
  for (int q = 0; q < 8; q++) {
    const float *c = g->corner + 6 * (size_t)((j + ((q >> 1) & 1)) * (g->nx + 1) + i + (q & 1));
    float t = (-d[q >> 2] - c[2]) / (c[5] - c[2]);
    out[q][0] = c[0] + (c[3] - c[0]) * t;
    out[q][1] = c[1] + (c[4] - c[1]) * t;
    out[q][2] = -d[q >> 2];
  }
}

void cw_froxel_grid_setup(cw_froxel_grid *g, const cw_froxel_config *c, int w, int h, const float proj[16], float near,
                          float far) {
  cw_froxel_config cfg = c ? *c : CW_FROXEL_CONFIG_DEFAULT;
  cfg.tile_px = CW_CLAMP(cfg.tile_px, 4, 1024);
  cfg.z_slices = CW_CLAMP(cfg.z_slices, 1, 256);
  cfg.max_per_froxel = CW_CLAMP(cfg.max_per_froxel, 1, 255);
  near = CW_MAX(near, 1e-3f);
  if (far <= near * 1.01f) far = near * 2;
  w = CW_MAX(w, 1);
  h = CW_MAX(h, 1);
  if (g->ready && !memcmp(&cfg, &g->cfg, sizeof cfg) && w == g->w && h == g->h && near == g->near && far == g->far &&
      !memcmp(proj, g->proj, sizeof g->proj))
    return;
  g->cfg = cfg;
  g->w = w;
  g->h = h;
  g->near = near;
  g->far = far;
  memcpy(g->proj, proj, sizeof g->proj);
  int nx = (w + cfg.tile_px - 1) / cfg.tile_px, ny = (h + cfg.tile_px - 1) / cfg.tile_px, nz = cfg.z_slices;
  int cells = nx * ny * nz;
  if (nx != g->nx || ny != g->ny || nz != g->nz) {
    g->nx = nx, g->ny = ny, g->nz = nz;
    g->cells = cells;
    cw_free(g->colp);
    cw_free(g->rowp);
    cw_free(g->slice);
    cw_free(g->corner);
    cw_free(g->sph);
    cw_free(g->records);
    cw_free(g->slice_start);
    cw_free(g->sl);
    g->sl = cw_alloc((size_t)nz * 5 * sizeof(int), CW_MEM_RENDER);
    g->colp = cw_alloc((size_t)(nx + 1) * sizeof(plane), CW_MEM_RENDER);
    g->rowp = cw_alloc((size_t)(ny + 1) * sizeof(plane), CW_MEM_RENDER);
    g->slice = cw_alloc((size_t)(nz + 1) * sizeof(float), CW_MEM_RENDER);
    g->corner = cw_alloc((size_t)(nx + 1) * (ny + 1) * 6 * sizeof(float), CW_MEM_RENDER);
    g->sph = cw_alloc((size_t)cells * 4 * sizeof(float), CW_MEM_RENDER);
    g->records = cw_calloc((size_t)cells, sizeof(uint32_t), CW_MEM_RENDER);
    g->slice_start = cw_alloc((size_t)(nz + 1) * sizeof(int), CW_MEM_RENDER);
    g->item_count = 0;
    g->tex_dirty = true;
  }
  g->zscale = (float)nz / cw_ln(far / near);
  g->zbias = -(float)nz * cw_ln(near) / cw_ln(far / near);
  for (int k = 0; k <= nz; k++) g->slice[k] = k == 0 ? near : k == nz ? far : near * cw_pow(far / near, (float)k / (float)nz);
  t3_mat4 P, ip;
  memcpy(P.e, proj, sizeof P.e);
  t3_mat4_invert(&ip, &P);
  /* NDC z 0 and 0.5 lie inside the frustum under both clip-z conventions;
   * this works for perspective and orthographic projections alike */
  for (int j = 0; j <= ny; j++)
    for (int i = 0; i <= nx; i++) {
      float x = -1 + 2.0f * (float)i / (float)nx, y = -1 + 2.0f * (float)j / (float)ny;
      float *cc = g->corner + 6 * (size_t)(j * (nx + 1) + i);
      unproject(&ip, x, y, 0.0f, cc);
      unproject(&ip, x, y, 0.5f, cc + 3);
    }
  for (int i = 0; i <= nx; i++) {
    float x = -1 + 2.0f * (float)i / (float)nx, a[3], b[3], cc[3], t[3];
    unproject(&ip, x, -1, 0.0f, a);
    unproject(&ip, x, 1, 0.0f, b);
    unproject(&ip, x, -1, 0.5f, cc);
    unproject(&ip, x + 0.5f, 0, 0.25f, t);
    g->colp[i] = plane_of(a, b, cc, t);
  }
  for (int j = 0; j <= ny; j++) {
    float y = -1 + 2.0f * (float)j / (float)ny, a[3], b[3], cc[3], t[3];
    unproject(&ip, -1, y, 0.0f, a);
    unproject(&ip, 1, y, 0.0f, b);
    unproject(&ip, -1, y, 0.5f, cc);
    unproject(&ip, 0, y + 0.5f, 0.25f, t);
    g->rowp[j] = plane_of(a, b, cc, t);
  }
  /* each cell's bounding sphere (its corners' box) for the cone test */
  for (int k = 0; k < nz; k++)
    for (int j = 0; j < ny; j++)
      for (int i = 0; i < nx; i++) {
        float p[8][3], mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        cell_corners(g, i, j, k, p);
        for (int q = 0; q < 8; q++)
          for (int a = 0; a < 3; a++) {
            mn[a] = p[q][a] < mn[a] ? p[q][a] : mn[a];
            mx[a] = p[q][a] > mx[a] ? p[q][a] : mx[a];
          }
        float *s = g->sph + 4 * ((size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * k));
        float hx = (mx[0] - mn[0]) * 0.5f, hy = (mx[1] - mn[1]) * 0.5f, hz = (mx[2] - mn[2]) * 0.5f;
        s[0] = mn[0] + hx, s[1] = mn[1] + hy, s[2] = mn[2] + hz;
        s[3] = sqrtf(hx * hx + hy * hy + hz * hz);
      }
  size_t need = (size_t)cells * (size_t)cfg.max_per_froxel;
  if (need > g->scratch_cap) {
    cw_free(g->scratch);
    g->scratch = cw_alloc(need * sizeof(uint16_t), CW_MEM_RENDER);
    g->scratch_cap = need;
  }
  if (cells > g->count_cap) {
    cw_free(g->count);
    g->count = cw_alloc((size_t)cells * sizeof(uint16_t), CW_MEM_RENDER);
    g->count_cap = cells;
  }
  g->ready = true;
}

void cw_froxel_grid_dims(const cw_froxel_grid *g, int *nx, int *ny, int *nz, float *zscale, float *zbias) {
  if (nx) *nx = g->nx;
  if (ny) *ny = g->ny;
  if (nz) *nz = g->nz;
  if (zscale) *zscale = g->zscale;
  if (zbias) *zbias = g->zbias;
}

int cw_froxel_cell_of(const cw_froxel_grid *g, float x, float y, float z) {
  if (!g->ready) return -1;
  const float *P = g->proj;
  float cx = P[0] * x + P[4] * y + P[8] * z + P[12], cy = P[1] * x + P[5] * y + P[9] * z + P[13];
  float cw_ = P[3] * x + P[7] * y + P[11] * z + P[15];
  if (cw_ <= 0) return -1;
  float u = cx / cw_, v = cy / cw_, d = -z;
  if (u < -1 || u > 1 || v < -1 || v > 1 || d < g->near || d > g->far) return -1;
  int i = CW_CLAMP((int)floorf((u * 0.5f + 0.5f) * (float)g->nx), 0, g->nx - 1);
  int j = CW_CLAMP((int)floorf((v * 0.5f + 0.5f) * (float)g->ny), 0, g->ny - 1);
  int k = CW_CLAMP((int)floorf(cw_ln(d) * g->zscale + g->zbias), 0, g->nz - 1);
  return i + g->nx * (j + g->ny * k);
}

void cw_froxel_cell_box(const cw_froxel_grid *g, int cell, float mn[3], float mx[3]) {
  int i = cell % g->nx, j = (cell / g->nx) % g->ny, k = cell / (g->nx * g->ny);
  float p[8][3];
  cell_corners(g, i, j, k, p);
  for (int a = 0; a < 3; a++) mn[a] = 1e30f, mx[a] = -1e30f;
  for (int q = 0; q < 8; q++)
    for (int a = 0; a < 3; a++) {
      mn[a] = p[q][a] < mn[a] ? p[q][a] : mn[a];
      mx[a] = p[q][a] > mx[a] ? p[q][a] : mx[a];
    }
}

/* ── binning ────────────────────────────────────────────────────────── */

/* the tiles [lo, hi] along one axis whose slab (between planes t and t + 1)
 * the sphere (x, y, z, r) reaches; distances fall as t rises. false: none */
static bool axis_range(const plane *p, int n, float x, float y, float z, float r, int *lo, int *hi) {
  if (pdist(&p[0], x, y, z) < -r || pdist(&p[n], x, y, z) > r) return false;
  /* hi: the last t with dist(t) >= -r */
  int a = 0, b = n - 1;
  while (a < b) {
    int m = (a + b + 1) >> 1;
    if (pdist(&p[m], x, y, z) >= -r) a = m;
    else b = m - 1;
  }
  *hi = a;
  /* lo: the first t with dist(t + 1) <= r */
  a = 0, b = n - 1;
  while (a < b) {
    int m = (a + b) >> 1;
    if (pdist(&p[m + 1], x, y, z) <= r) b = m;
    else a = m + 1;
  }
  *lo = a;
  return *lo <= *hi;
}

/* cone vs sphere (a cell's bounding sphere): outside when the sphere's
 * centre is farther than its radius from the cone's surface, behind the
 * apex, or past the cone's length (Bart Wronski's "cull that cone", the test
 * Filament's Froxelizer uses) */
static bool cone_hits(const vol *v, const float *s) {
  float vx = s[0] - v->px, vy = s[1] - v->py, vz = s[2] - v->pz;
  float v2 = vx * vx + vy * vy + vz * vz;
  float v1 = vx * v->ax + vy * v->ay + vz * v->az;
  float perp = sqrtf(CW_MAX(v2 - v1 * v1, 0.0f));
  if (v->cosa * perp - v1 * v->sina > s[3]) return false;
  if (v1 < -s[3] || v1 > v->len + s[3]) return false;
  return true;
}

static void bin_slices(int begin, int end, int worker, void *ctx) {
  (void)worker;
  cw_froxel_grid *g = ctx;
  int nx = g->nx, ny = g->ny, mpc = g->cfg.max_per_froxel;
  for (int k = begin; k < end; k++) {
    size_t base = (size_t)k * nx * ny;
    memset(g->count + base, 0, (size_t)nx * ny * sizeof *g->count);
    float z_near = -g->slice[k], z_far = -g->slice[k + 1];
    for (int q = g->slice_start[k]; q < g->slice_start[k + 1]; q++) {
      int vi = g->slice_vols[q];
      const vol *v = &g->vols[vi];
      /* the part of the sphere inside this slice */
      float zc = v->cz > z_near ? z_near : v->cz < z_far ? z_far : v->cz;
      float dz = v->cz - zc, r2 = v->r * v->r - dz * dz;
      if (r2 < 0) continue;
      float r = sqrtf(r2);
      int i0, i1, j0, j1;
      if (!axis_range(g->rowp, ny, v->cx, v->cy, zc, r, &j0, &j1)) continue;
      for (int j = j0; j <= j1; j++) {
        /* the part inside this row: the sphere cut by the row's nearer plane
         * when the centre lies outside the row (a half-space cut, as the slice's) */
        float x = v->cx, y = v->cy, z = zc, rr = r;
        const plane *lo = &g->rowp[j], *hi = &g->rowp[j + 1];
        float dl = pdist(lo, x, y, z), dh = pdist(hi, x, y, z);
        const plane *cut = dl < 0 ? lo : dh > 0 ? hi : NULL;
        if (cut) {
          float d = cut == lo ? dl : dh, q = rr * rr - d * d;
          if (q < 0) continue;
          x -= cut->x * d, y -= cut->y * d, z -= cut->z * d;
          rr = sqrtf(q);
        }
        if (!axis_range(g->colp, nx, x, y, z, rr, &i0, &i1)) continue;
        for (int i = i0; i <= i1; i++) {
          size_t cell = base + (size_t)j * nx + i;
          if (v->cone && !cone_hits(v, g->sph + 4 * cell)) continue;
          uint16_t *n = &g->count[cell];
          if (*n < mpc) g->scratch[cell * mpc + *n] = (uint16_t)vi;
          if (*n < 0xffff) ++*n;
        }
      }
    }
    /* the slice's totals, for the compaction */
    int *t = g->sl + 5 * k;
    t[0] = t[1] = t[2] = t[3] = 0;
    for (size_t c = base; c < base + (size_t)nx * ny; c++) {
      int cnt = g->count[c];
      if (cnt > mpc) t[1]++, t[2] += cnt - mpc, cnt = mpc;
      if (cnt > t[3]) t[3] = cnt;
      t[0] += cnt;
    }
  }
}

/* each slice's records and items, from its first item on */
static void compact_slices(int begin, int end, int worker, void *ctx) {
  (void)worker;
  cw_froxel_grid *g = ctx;
  int mpc = g->cfg.max_per_froxel;
  size_t per = (size_t)g->nx * g->ny;
  for (int k = begin; k < end; k++) {
    int at = g->sl[5 * k + 4];
    for (size_t c = (size_t)k * per; c < (size_t)(k + 1) * per; c++) {
      int cnt = CW_MIN((int)g->count[c], mpc);
      g->records[c] = (uint32_t)at << 8 | (uint32_t)cnt;
      memcpy(g->items + at, g->scratch + c * mpc, (size_t)cnt * sizeof(uint16_t));
      at += cnt;
    }
  }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void cw_froxel_grid_bin(cw_froxel_grid *g, const cw_froxel_volume *in, int n) {
  CW_PROFILE_SCOPE("froxel.bin");
  double t0 = now_ms();
  memset(&g->stats, 0, sizeof g->stats);
  if (!g->ready) return;
  n = CW_CLAMP(n, 0, 65535);
  int nz = g->nz;
  g->stats.nx = g->nx, g->stats.ny = g->ny, g->stats.nz = nz, g->stats.volumes = n;
  if (g->vols_cap < n) {
    g->vols_cap = n + 64;
    g->vols = cw_realloc(g->vols, (size_t)g->vols_cap * sizeof *g->vols, CW_MEM_RENDER);
  }
  /* each volume's binning sphere and slices */
  memset(g->slice_start, 0, (size_t)(nz + 1) * sizeof(int));
  int refs = 0;
  for (int i = 0; i < n; i++) {
    const cw_froxel_volume *s = &in[i];
    vol *v = &g->vols[i];
    v->cx = s->x, v->cy = s->y, v->cz = s->z, v->r = s->radius;
    v->cone = s->cos_angle > -1;
    if (v->cone) {
      v->px = s->x, v->py = s->y, v->pz = s->z;
      v->ax = s->dir[0], v->ay = s->dir[1], v->az = s->dir[2];
      v->cosa = s->cos_angle, v->sina = s->sin_angle, v->len = s->radius;
      /* the tighter of two bounding spheres of the cone (Filament's) */
      if (s->cos_angle >= 0.70710678f) {
        float h = s->radius / (2 * s->cos_angle);
        v->cx += s->dir[0] * h, v->cy += s->dir[1] * h, v->cz += s->dir[2] * h;
        v->r = h;
      } else if (s->cos_angle > 0) {
        v->cx += s->dir[0] * s->cos_angle * s->radius;
        v->cy += s->dir[1] * s->cos_angle * s->radius;
        v->cz += s->dir[2] * s->cos_angle * s->radius;
        v->r = s->sin_angle * s->radius;
      }
    }
    float dmin = -v->cz - v->r, dmax = -v->cz + v->r;
    if (!(v->r > 0) || dmax < g->near || dmin > g->far) {
      v->k0 = 1, v->k1 = 0;
      continue;
    }
    v->k0 = dmin <= g->near ? 0 : clampi((int)floorf(cw_ln(dmin) * g->zscale + g->zbias), 0, nz - 1);
    v->k1 = clampi((int)floorf(cw_ln(CW_MIN(dmax, g->far)) * g->zscale + g->zbias), 0, nz - 1);
    /* (a slice boundary within rounding of a depth: the neighbours too; the
     * per-slice cut drops them when the sphere misses) */
    v->k0 = CW_MAX(v->k0 - 1, 0);
    v->k1 = CW_MIN(v->k1 + 1, nz - 1);
    for (int k = v->k0; k <= v->k1; k++) g->slice_start[k + 1]++;
    refs += v->k1 - v->k0 + 1;
  }
  for (int k = 0; k < nz; k++) g->slice_start[k + 1] += g->slice_start[k];
  if (g->slice_vols_cap < refs) {
    g->slice_vols_cap = refs + 1024;
    cw_free(g->slice_vols);
    g->slice_vols = cw_alloc((size_t)g->slice_vols_cap * sizeof(int), CW_MEM_RENDER);
  }
  int *fill = cw_frame_alloc((size_t)nz * sizeof(int));
  memcpy(fill, g->slice_start, (size_t)nz * sizeof(int));
  for (int i = 0; i < n; i++)
    for (int k = g->vols[i].k0; k <= g->vols[i].k1; k++) g->slice_vols[fill[k]++] = i;
  /* a pool wake-up costs tens of microseconds a thread: a few volumes bin on
   * this thread (the lists are the same either way: slices are independent) */
  double t1 = now_ms();
  if (refs < 256 || cw_jobs_thread_count() < 2) bin_slices(0, nz, 0, g);
  else cw_parallel_for(nz, 1, bin_slices, g);
  double t2 = now_ms();
  /* compaction in cell order: each slice from its offset, in parallel */
  int total = 0;
  for (int k = 0; k < nz; k++) {
    int *t = g->sl + 5 * k;
    t[4] = total;
    total += t[0];
    g->stats.overflowed += t[1];
    g->stats.dropped += t[2];
    if (t[3] > g->stats.max_in_froxel) g->stats.max_in_froxel = t[3];
  }
  if (g->items_cap < total) {
    g->items_cap = total + 4096;
    g->items = cw_realloc(g->items, (size_t)g->items_cap * sizeof(uint16_t), CW_MEM_RENDER);
  }
  if (refs < 256 || cw_jobs_thread_count() < 2) compact_slices(0, nz, 0, g);
  else cw_parallel_for(nz, 1, compact_slices, g);
  g->item_count = total;
  g->stats.references = total;
  g->tex_dirty = true;
  g->stats.bin_ms = (float)(now_ms() - t0);
  if (getenv("CW_FROXEL_TRACE")) cw_info("froxel: prep %.3f slices %.3f compact %.3f ms", t1 - t0, t2 - t1, now_ms() - t2);
}

const uint32_t *cw_froxel_records(const cw_froxel_grid *g, int *cells) {
  if (cells) *cells = g->cells;
  return g->records;
}

const uint16_t *cw_froxel_items(const cw_froxel_grid *g, int *count) {
  if (count) *count = g->item_count;
  return g->items;
}

void cw_froxel_grid_stats(const cw_froxel_grid *g, cw_froxel_stats *out) { *out = g->stats; }

uint64_t cw_froxel_grid_hash(const cw_froxel_grid *g) {
  uint64_t h = 1469598103934665603ull;
  const uint8_t *p = (const uint8_t *)g->records;
  for (size_t i = 0; i < (size_t)g->cells * 4; i++) h = (h ^ p[i]) * 1099511628211ull;
  p = (const uint8_t *)g->items;
  for (size_t i = 0; i < (size_t)g->item_count * 2; i++) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

/* ── textures ───────────────────────────────────────────────────────── */

static int pow2_at_least(int v) {
  int p = 1;
  while (p < v) p <<= 1;
  return p;
}

static t3_texture *uint_tex(t3_texture *old, int w, int h) {
  if (old && old->width == w && old->height == h) return old;
  if (old) t3_release(old);
  return t3_data_texture_new_format(w, h, NULL, T3_RED_INTEGER_FORMAT, T3_UNSIGNED_INT_TYPE);
}

void cw_froxel_grid_textures(cw_froxel_grid *g, t3_texture **records, int *records_width, t3_texture **items,
                             int *items_width) {
  if (g->tex_dirty || !g->rec_tex || !g->item_tex) {
    int cells = CW_MAX(g->cells, 1);
    int rw = CW_MIN(cells, RECORDS_W), rh = (cells + rw - 1) / rw;
    g->rec_tex = uint_tex(g->rec_tex, rw, rh);
    memset(g->rec_tex->pixels, 0, (size_t)rw * rh * 4);
    if (g->cells) memcpy(g->rec_tex->pixels, g->records, (size_t)g->cells * 4);
    g->rec_tex->version++;
    int texels = CW_MAX((g->item_count + 1) / 2, 1);
    int ih = pow2_at_least((texels + ITEMS_W - 1) / ITEMS_W);
    g->item_tex = uint_tex(g->item_tex, ITEMS_W, ih);
    if (g->item_count) memcpy(g->item_tex->pixels, g->items, (size_t)g->item_count * 2);
    g->item_tex->version++;
    g->tex_dirty = false;
  }
  if (records) *records = g->rec_tex;
  if (records_width) *records_width = g->rec_tex->width;
  if (items) *items = g->item_tex;
  if (items_width) *items_width = g->item_tex->width;
}
