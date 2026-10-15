/* Terrain scatter (cartwheel/terrain.h): grass, rocks and trees placed
 * over the terrain in 32 m cells.
 *
 * Placement is a pure function of (layer seed, cell): a jittered grid of
 * candidates over the cell (one per stratum, so instances never clump),
 * each kept by an integer hash against the layer's density, its density
 * map, the control layer under it, holes and the slope. Heights come from
 * the triangles (+ - * / only) and rotations from cw_sincos, so the same
 * instances land on every host at any worker count. Each instance also gets
 * a rank in [0, 1): quality thins a layer by drawing the instances whose
 * rank is under the density scale, so thinning never reshuffles.
 *
 * Cells within visible_range are generated on the job pool (each into its
 * own array), get one InstancedMesh per mesh LOD with the cell's bounding
 * sphere, and are frustum culled as a whole. Posing runs on the job pool,
 * four instances at a time: each instance picks its LOD by distance and
 * shrinks to nothing over the last `fade` metres of visible_range. A cell
 * wholly inside one LOD's band and short of the fade keeps its last pose. */
#include <math.h>
#include <string.h>

#include "cartwheel/jobs.h"
#include "cartwheel/math.h"
#include "cartwheel/quality.h"
#include "terrain_internal.h"

#define INST_F 9 /* x y z scale qx qy qz qw rank */
#define CELL_MAX 8192

typedef struct sc_cell {
  int cx, cz;
  bool generated, live;
  int n;                          /* instances */
  float *inst;                    /* n * INST_F */
  t3_instanced_mesh *im[CW_SCATTER_LODS][CW_SCATTER_PARTS]; /* parts share part 0's instance matrices */
  float sphere[4];
  float ymin, ymax;
  int posed_state;                /* -2 never; -1 dynamic; else the LOD it was posed whole at */
  float posed_density;
  int drawn;
} sc_cell;

struct tr_scatter {
  cw_scatter_def def;
  uint8_t *dm;
  int nlod, nparts[CW_SCATTER_LODS];
  int cnx, cnz;
  sc_cell *cells;                 /* cnx * cnz, generated lazily */
  float radius;                   /* the finest mesh's bounding radius */
  t3_scene *scene;
};

cw_scatter_def cw_scatter_default_def(void) {
  return (cw_scatter_def){ .density = 0.5f, .scale_min = 0.8f, .scale_max = 1.2f, .visible_range = 60, .fade = 10,
                           .density_knob = "grass", .align_to_normal = true };
}

static uint32_t mix32(uint32_t h) {
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}
static uint32_t hash4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return mix32(mix32(mix32(mix32(a) ^ b) ^ c) ^ d);
}
static float unit(uint32_t h) { return (float)(h >> 8) * (1.0f / 16777216.0f); }

/* ── placement ──────────────────────────────────────────────────────── */

static float dm_at(const tr_scatter *s, const cw_terrain *t, float fx, float fz) {
  if (!s->dm) return 1;
  float u = fx / (float)(t->nx - 1), v = fz / (float)(t->nz - 1);
  int x = CW_CLAMP((int)(u * (float)s->def.dm_w), 0, s->def.dm_w - 1);
  int z = CW_CLAMP((int)(v * (float)s->def.dm_h), 0, s->def.dm_h - 1);
  return (float)s->dm[z * s->def.dm_w + x] / 255.0f;
}

/* q = rotation taking +Y to n (shortest arc) times a yaw about +Y */
static void orient(float yaw, t3_vec3 n, bool align, float q[4]) {
  float sy, cy;
  cw_sincos(yaw * 0.5f, &sy, &cy);
  float a[4] = { 0, sy, 0, cy };
  if (!align || n.y > 0.9999f) {
    memcpy(q, a, sizeof a);
    return;
  }
  /* from (0,1,0) to n: axis (n.z, 0, -n.x), half-angle via the half vector */
  float w = 1 + n.y;
  float b[4] = { n.z, 0, -n.x, w };
  float l = sqrtf(b[0] * b[0] + b[2] * b[2] + b[3] * b[3]);
  for (int k = 0; k < 4; k++) b[k] /= l;
  /* q = b * a */
  q[0] = b[3] * a[0] + b[0] * a[3] + b[1] * a[2] - b[2] * a[1];
  q[1] = b[3] * a[1] - b[0] * a[2] + b[1] * a[3] + b[2] * a[0];
  q[2] = b[3] * a[2] + b[0] * a[1] - b[1] * a[0] + b[2] * a[3];
  q[3] = b[3] * a[3] - b[0] * a[0] - b[1] * a[1] - b[2] * a[2];
}

static int place(const cw_terrain *t, const tr_scatter *s, int cx, int cz, float *out, int cap) {
  const cw_scatter_def *d = &s->def;
  float sp = t->def.vertex_spacing;
  float area = TR_SCATTER_CELL * TR_SCATTER_CELL;
  float want = d->density * area;
  if (!(want > 0)) return 0;
  int k = (int)ceilf(sqrtf(want));
  if (k * k > CELL_MAX) k = (int)sqrtf((float)CELL_MAX);
  float p = want / (float)(k * k); /* chance a stratum holds one */
  float cell = TR_SCATTER_CELL / (float)k;
  float x0 = (float)cx * TR_SCATTER_CELL, z0 = (float)cz * TR_SCATTER_CELL; /* terrain-local metres */
  float wx = (float)t->def.size_x * sp, wz = (float)t->def.size_z * sp;
  int n = 0;
  for (int j = 0; j < k && n < cap; j++)
    for (int i = 0; i < k && n < cap; i++) {
      uint32_t h = hash4(d->seed, (uint32_t)cx * 73856093u ^ (uint32_t)cz * 19349663u, (uint32_t)i, (uint32_t)j);
      float lx = x0 + ((float)i + unit(h)) * cell;
      uint32_t h2 = mix32(h ^ 0x68E31DA4u);
      float lz = z0 + ((float)j + unit(h2)) * cell;
      if (lx >= wx || lz >= wz) continue;
      float fx = lx / sp, fz = lz / sp;
      uint32_t h3 = mix32(h2 ^ 0xB5297A4Du);
      if (unit(h3) >= p * dm_at(s, t, fx, fz)) continue;
      int ix = CW_CLAMP((int)(fx + 0.5f), 0, t->nx - 1), iz = CW_CLAMP((int)(fz + 0.5f), 0, t->nz - 1);
      uint32_t c = t->ctl[tr_idx(t, ix, iz)];
      if ((c >> 2) & 1) continue; /* a hole */
      if (d->layers) {
        int base = (int)(c >> 27), over = (int)((c >> 22) & 31), blend = (int)((c >> 14) & 255);
        int layer = blend > 127 ? over : base;
        if (!((d->layers >> layer) & 1)) continue;
      }
      t3_vec3 nrm = cw__terrain_normal_fast(t, fx, fz);
      if (nrm.y < d->min_slope_cos) continue;
      uint32_t h4 = mix32(h3 ^ 0x1B56C4E9u), h5 = mix32(h4 ^ 0x9E3779B9u);
      float scale = d->scale_min + (d->scale_max - d->scale_min) * unit(h4);
      float yaw = unit(h5) * CW_TAU;
      float *o = &out[INST_F * n++];
      o[0] = t->def.origin.x + lx;
      o[1] = cw__terrain_height_fast(t, fx, fz) - d->sink * scale;
      o[2] = t->def.origin.z + lz;
      o[3] = scale;
      orient(yaw, nrm, d->align_to_normal, &o[4]);
      o[8] = unit(mix32(h5 ^ 0x2545F491u)); /* rank */
    }
  return n;
}

int cw_terrain_scatter_cell(cw_terrain *t, int layer, int cx, int cz, float *out, int cap) {
  if (!t || layer < 0 || layer >= TR_MAX_SCATTER || !t->scatter[layer]) return 0;
  float *tmp = cw_alloc(sizeof(float) * INST_F * CELL_MAX, CW_MEM_RENDER);
  int n = place(t, t->scatter[layer], cx, cz, tmp, CELL_MAX);
  int m = CW_MIN(n, cap);
  for (int i = 0; i < m; i++) {
    out[5 * i] = tmp[INST_F * i];
    out[5 * i + 1] = tmp[INST_F * i + 1];
    out[5 * i + 2] = tmp[INST_F * i + 2];
    out[5 * i + 3] = tmp[INST_F * i + 3];
    /* yaw back from the quaternion's y / w (render use only: atan2f) */
    out[5 * i + 4] = 2 * atan2f(tmp[INST_F * i + 5], tmp[INST_F * i + 7]);
  }
  cw_free(tmp);
  return m;
}

/* ── layers ─────────────────────────────────────────────────────────── */

static void cell_free_gpu(tr_scatter *s, sc_cell *c) {
  for (int l = 0; l < CW_SCATTER_LODS; l++)
    for (int p = 0; p < CW_SCATTER_PARTS; p++)
      if (c->im[l][p]) {
        if (s->scene) t3_object_remove(&s->scene->base, c->im[l][p]);
        t3_release(c->im[l][p]);
        c->im[l][p] = NULL;
      }
}

static void cell_reset(tr_scatter *s, sc_cell *c) {
  cell_free_gpu(s, c);
  cw_free(c->inst);
  int cx = c->cx, cz = c->cz;
  memset(c, 0, sizeof *c);
  c->cx = cx, c->cz = cz;
  c->posed_state = -2;
}

void cw__scatter_free(tr_scatter *s) {
  if (!s) return;
  for (int i = 0; i < s->cnx * s->cnz; i++) cell_reset(s, &s->cells[i]);
  for (int l = 0; l < CW_SCATTER_LODS; l++)
    for (int p = 0; p < CW_SCATTER_PARTS; p++) {
      if (s->def.geometry[l][p]) t3_release(s->def.geometry[l][p]);
      if (s->def.material[l][p]) t3_release(s->def.material[l][p]);
    }
  cw_free(s->cells);
  cw_free(s->dm);
  cw_free(s);
}

void cw__scatter_attach(cw_terrain *t, tr_scatter *s) {
  t3_scene *scene = cw__terrain_scene(t);
  if (s->scene == scene) return;
  for (int i = 0; i < s->cnx * s->cnz; i++) cell_free_gpu(s, &s->cells[i]), s->cells[i].posed_state = -2;
  s->scene = scene;
}

int cw_terrain_scatter_add(cw_terrain *t, const cw_scatter_def *def) {
  if (!t || !def || !def->geometry[0][0] || !def->material[0][0]) {
    cw_error("terrain: a scatter layer needs a geometry and a material");
    return -1;
  }
  int slot = -1;
  for (int i = 0; i < TR_MAX_SCATTER; i++)
    if (!t->scatter[i]) {
      slot = i;
      break;
    }
  if (slot < 0) {
    cw_error("terrain: at most %d scatter layers", TR_MAX_SCATTER);
    return -1;
  }
  tr_scatter *s = cw_calloc(1, sizeof *s, CW_MEM_RENDER);
  s->def = *def;
  if (!(s->def.visible_range > 0)) s->def.visible_range = 60;
  s->def.fade = cw_clampf(s->def.fade, 0, s->def.visible_range);
  if (!s->def.density_knob) s->def.density_knob = "grass";
  if (def->density_map && def->dm_w > 0 && def->dm_h > 0) {
    s->dm = cw_alloc((size_t)def->dm_w * def->dm_h, CW_MEM_RENDER);
    memcpy(s->dm, def->density_map, (size_t)def->dm_w * def->dm_h);
  }
  s->def.density_map = NULL;
  for (int l = 0; l < CW_SCATTER_LODS; l++) {
    if (!s->def.geometry[l][0]) break;
    int np = 0;
    for (int p = 0; p < CW_SCATTER_PARTS && s->def.geometry[l][p]; p++) {
      if (!s->def.material[l][p]) s->def.material[l][p] = l ? s->def.material[l - 1][p] : NULL;
      if (!s->def.material[l][p]) break;
      t3_retain(s->def.geometry[l][p]);
      t3_retain(s->def.material[l][p]);
      np = p + 1;
    }
    for (int p = np; p < CW_SCATTER_PARTS; p++) s->def.geometry[l][p] = NULL, s->def.material[l][p] = NULL;
    s->nparts[l] = np;
    s->nlod = l + 1;
  }
  for (int l = s->nlod; l < CW_SCATTER_LODS; l++)
    for (int p = 0; p < CW_SCATTER_PARTS; p++) s->def.geometry[l][p] = NULL, s->def.material[l][p] = NULL;
  /* the bounding radius over LOD 0's parts */
  for (int p = 0; p < s->nparts[0]; p++) {
    t3_geometry *g = s->def.geometry[0][p];
    if (!g->has_bounding_sphere) t3_geometry_compute_bounding_sphere(g);
    float r = g->bounding_sphere.radius + sqrtf(g->bounding_sphere.center.x * g->bounding_sphere.center.x +
                                                g->bounding_sphere.center.y * g->bounding_sphere.center.y +
                                                g->bounding_sphere.center.z * g->bounding_sphere.center.z);
    s->radius = r > s->radius ? r : s->radius;
  }
  float sp = t->def.vertex_spacing;
  s->cnx = (int)ceilf((float)t->def.size_x * sp / TR_SCATTER_CELL);
  s->cnz = (int)ceilf((float)t->def.size_z * sp / TR_SCATTER_CELL);
  s->cells = cw_calloc((size_t)s->cnx * s->cnz, sizeof(sc_cell), CW_MEM_RENDER);
  for (int z = 0; z < s->cnz; z++)
    for (int x = 0; x < s->cnx; x++) {
      sc_cell *c = &s->cells[z * s->cnx + x];
      c->cx = x, c->cz = z, c->posed_state = -2;
    }
  t->scatter[slot] = s;
  cw__scatter_attach(t, s);
  return slot;
}

void cw_terrain_scatter_remove(cw_terrain *t, int layer) {
  if (!t || layer < 0 || layer >= TR_MAX_SCATTER || !t->scatter[layer]) return;
  cw__scatter_free(t->scatter[layer]);
  t->scatter[layer] = NULL;
}

void cw_terrain_scatter_invalidate(cw_terrain *t) {
  if (!t) return;
  for (int i = 0; i < TR_MAX_SCATTER; i++) {
    tr_scatter *s = t->scatter[i];
    if (!s) continue;
    for (int k = 0; k < s->cnx * s->cnz; k++) cell_reset(s, &s->cells[k]);
  }
}

/* ── per frame ──────────────────────────────────────────────────────── */

typedef struct gen_job {
  cw_terrain *t;
  tr_scatter *s;
  sc_cell **list;
} gen_job;

static void gen_cells(int begin, int end, int worker, void *u) {
  (void)worker;
  gen_job *j = u;
  float *tmp = cw_alloc(sizeof(float) * INST_F * CELL_MAX, CW_MEM_RENDER);
  for (int i = begin; i < end; i++) {
    sc_cell *c = j->list[i];
    int n = place(j->t, j->s, c->cx, c->cz, tmp, CELL_MAX);
    c->n = n;
    c->inst = n ? cw_alloc(sizeof(float) * INST_F * (size_t)n, CW_MEM_RENDER) : NULL;
    if (n) memcpy(c->inst, tmp, sizeof(float) * INST_F * (size_t)n);
    float lo = 1e30f, hi = -1e30f;
    for (int k = 0; k < n; k++) {
      lo = CW_MIN(lo, c->inst[INST_F * k + 1]);
      hi = CW_MAX(hi, c->inst[INST_F * k + 1]);
    }
    c->ymin = n ? lo : 0;
    c->ymax = n ? hi : 0;
    c->generated = true;
  }
  cw_free(tmp);
}

typedef struct pose_job {
  const cw_terrain *t;
  tr_scatter *s;
  sc_cell **list;
  float cam[3];
  float density;
} pose_job;

/* instance i's matrix (column major) at scale k */
static void write_matrix(float *m, const float *in, float k) {
  float x = in[4], y = in[5], z = in[6], w = in[7], s = in[3] * k;
  float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
  m[0] = (1 - 2 * (yy + zz)) * s, m[1] = 2 * (xy + wz) * s, m[2] = 2 * (xz - wy) * s, m[3] = 0;
  m[4] = 2 * (xy - wz) * s, m[5] = (1 - 2 * (xx + zz)) * s, m[6] = 2 * (yz + wx) * s, m[7] = 0;
  m[8] = 2 * (xz + wy) * s, m[9] = 2 * (yz - wx) * s, m[10] = (1 - 2 * (xx + yy)) * s, m[11] = 0;
  m[12] = in[0], m[13] = in[1], m[14] = in[2], m[15] = 1;
}

static void pose_cells(int begin, int end, int worker, void *u) {
  (void)worker;
  pose_job *j = u;
  const cw_scatter_def *d = &j->s->def;
  int nl = j->s->nlod;
  float vr = d->visible_range, fs = vr - d->fade;
  for (int ci = begin; ci < end; ci++) {
    sc_cell *c = j->list[ci];
    float *dst[CW_SCATTER_LODS] = { 0 };
    int cnt[CW_SCATTER_LODS] = { 0 };
    for (int l = 0; l < nl; l++) dst[l] = t3_attribute_f32(c->im[l][0]->instance_matrix);
    tr_v4f cx = { j->cam[0], j->cam[0], j->cam[0], j->cam[0] }, cy = { j->cam[1], j->cam[1], j->cam[1], j->cam[1] },
           cz = { j->cam[2], j->cam[2], j->cam[2], j->cam[2] };
    for (int i = 0; i < c->n; i += 4) {
      /* four distances at once */
      tr_v4f px, py, pz;
      for (int k = 0; k < 4; k++) {
        const float *in = &c->inst[INST_F * CW_MIN(i + k, c->n - 1)];
        px[k] = in[0], py[k] = in[1], pz[k] = in[2];
      }
      tr_v4f dx = px - cx, dy = py - cy, dz = pz - cz;
      tr_v4f d2 = dx * dx + dy * dy + dz * dz;
      for (int k = 0; k < 4 && i + k < c->n; k++) {
        const float *in = &c->inst[INST_F * (i + k)];
        if (in[8] >= j->density) continue;
        float dist = sqrtf(d2[k]);
        if (dist >= vr) continue;
        float f = dist > fs ? 1 - cw_smoothstep(fs, vr, dist) : 1;
        int l = 0;
        while (l + 1 < nl && dist >= d->lod_distance[l]) l++;
        write_matrix(dst[l] + 16 * cnt[l]++, in, f);
      }
    }
    int drawn = 0;
    for (int l = 0; l < nl; l++) {
      for (int p = 0; p < j->s->nparts[l]; p++) c->im[l][p]->count = cnt[l];
      drawn += cnt[l];
    }
    c->drawn = drawn;
  }
}

static float cell_dist2(const float cam[3], const float mn[3], const float mx[3]) {
  float d = 0;
  for (int k = 0; k < 3; k++) {
    float v = cam[k] < mn[k] ? mn[k] - cam[k] : cam[k] > mx[k] ? cam[k] - mx[k] : 0;
    d += v * v;
  }
  return d;
}

static void scatter_one(cw_terrain *t, tr_scatter *s, t3_vec3 cam, const t3_frustum *fr) {
  const cw_scatter_def *d = &s->def;
  float sp = t->def.vertex_spacing;
  float vr = d->visible_range;
  float camv[3] = { cam.x, cam.y, cam.z };
  float lx = cam.x - t->def.origin.x, lz = cam.z - t->def.origin.z;
  int x0 = CW_MAX(0, (int)floorf((lx - vr) / TR_SCATTER_CELL)), x1 = CW_MIN(s->cnx - 1, (int)floorf((lx + vr) / TR_SCATTER_CELL));
  int z0 = CW_MAX(0, (int)floorf((lz - vr) / TR_SCATTER_CELL)), z1 = CW_MIN(s->cnz - 1, (int)floorf((lz + vr) / TR_SCATTER_CELL));
  float density = cw_quality_scale(d->density_knob);
  /* cells that left the range drop their GPU copies (their placement stays: it is cheap to keep) */
  for (int i = 0; i < s->cnx * s->cnz; i++) {
    sc_cell *c = &s->cells[i];
    if (c->live && (c->cx < x0 - 1 || c->cx > x1 + 1 || c->cz < z0 - 1 || c->cz > z1 + 1)) {
      cell_free_gpu(s, c);
      c->live = false;
      c->posed_state = -2;
    }
  }
  if (x1 < x0 || z1 < z0) return;
  int span = (x1 - x0 + 1) * (z1 - z0 + 1);
  sc_cell **gen = cw_alloc(sizeof(sc_cell *) * (size_t)span, CW_MEM_RENDER);
  sc_cell **pose = cw_alloc(sizeof(sc_cell *) * (size_t)span, CW_MEM_RENDER);
  int ng = 0, np = 0;
  for (int z = z0; z <= z1; z++)
    for (int x = x0; x <= x1; x++) {
      sc_cell *c = &s->cells[z * s->cnx + x];
      float mn[3] = { t->def.origin.x + (float)x * TR_SCATTER_CELL, -1e30f, t->def.origin.z + (float)z * TR_SCATTER_CELL };
      float mx[3] = { mn[0] + TR_SCATTER_CELL, 1e30f, mn[2] + TR_SCATTER_CELL };
      mn[1] = camv[1], mx[1] = camv[1]; /* height unknown before generation: horizontal distance */
      if (cell_dist2(camv, mn, mx) < vr * vr && !c->generated) gen[ng++] = c;
    }
  if (ng) {
    gen_job gj = { t, s, gen };
    cw_parallel_for(ng, 1, gen_cells, &gj);
  }
  for (int z = z0; z <= z1; z++)
    for (int x = x0; x <= x1; x++) {
      sc_cell *c = &s->cells[z * s->cnx + x];
      if (!c->generated || !c->n) continue;
      float r = s->radius * d->scale_max;
      float mn[3] = { t->def.origin.x + (float)x * TR_SCATTER_CELL - r, c->ymin - r, t->def.origin.z + (float)z * TR_SCATTER_CELL - r };
      float mx[3] = { mn[0] + TR_SCATTER_CELL + 2 * r, c->ymax + r, mn[2] + TR_SCATTER_CELL + 2 * r };
      float d2 = cell_dist2(camv, mn, mx);
      bool vis = d2 < vr * vr;
      if (vis && fr) {
        for (int p = 0; p < 6 && vis; p++) {
          const t3_plane *pl = &fr->planes[p];
          float far = pl->normal.x * (pl->normal.x >= 0 ? mx[0] : mn[0]) + pl->normal.y * (pl->normal.y >= 0 ? mx[1] : mn[1]) +
                      pl->normal.z * (pl->normal.z >= 0 ? mx[2] : mn[2]) + pl->constant;
          if (far < 0) vis = false;
        }
      }
      if (!c->live && vis) {
        /* GPU copies for a cell coming into view */
        for (int l = 0; l < s->nlod; l++)
          for (int p = 0; p < s->nparts[l]; p++) {
            t3_instanced_mesh *im = t3_instanced_mesh_new(d->geometry[l][p], d->material[l][p], c->n);
            im->count = 0;
            if (p) {
              /* the parts draw part 0's instances: one pose, one upload */
              t3_release(im->instance_matrix);
              im->instance_matrix = t3_retain(c->im[l][0]->instance_matrix);
            }
            im->instance_matrix->usage = T3_DYNAMIC_DRAW;
            im->mesh.base.cast_shadow = l == 0 && d->cast_shadow;
            im->mesh.base.receive_shadow = true;
            t3_sphere sph = { { (mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f }, 0 };
            float ex = (mx[0] - mn[0]) * 0.5f, ey = (mx[1] - mn[1]) * 0.5f, ez = (mx[2] - mn[2]) * 0.5f;
            sph.radius = sqrtf(ex * ex + ey * ey + ez * ez);
            t3_instanced_mesh_set_bounding_sphere(im, &sph);
            c->im[l][p] = im;
            if (s->scene) t3_object_add(&s->scene->base, im);
          }
        c->live = true;
        c->posed_state = -2;
      }
      if (!c->live) continue;
      for (int l = 0; l < s->nlod; l++)
        for (int p = 0; p < s->nparts[l]; p++) c->im[l][p]->mesh.base.visible = vis;
      if (!vis) continue;
      /* a cell wholly in one LOD band, short of the fade, keeps its pose */
      float dmin = sqrtf(d2), dmax;
      {
        /* the farthest corner */
        float fx = camv[0] - mn[0] > mx[0] - camv[0] ? camv[0] - mn[0] : mx[0] - camv[0];
        float fy = camv[1] - mn[1] > mx[1] - camv[1] ? camv[1] - mn[1] : mx[1] - camv[1];
        float fz = camv[2] - mn[2] > mx[2] - camv[2] ? camv[2] - mn[2] : mx[2] - camv[2];
        dmax = sqrtf(fx * fx + fy * fy + fz * fz);
      }
      int la = 0, lb = 0;
      while (la + 1 < s->nlod && dmin >= d->lod_distance[la]) la++;
      while (lb + 1 < s->nlod && dmax >= d->lod_distance[lb]) lb++;
      int state = la == lb && dmax < vr - d->fade ? la : -1;
      if (state >= 0 && state == c->posed_state && c->posed_density == density) continue;
      c->posed_state = state;
      c->posed_density = density;
      pose[np++] = c;
    }
  if (np) {
    pose_job pj = { t, s, pose, { cam.x, cam.y, cam.z }, density };
    cw_parallel_for(np, 1, pose_cells, &pj);
    for (int i = 0; i < np; i++)
      for (int l = 0; l < s->nlod; l++) {
        t3_instanced_mesh *im = pose[i]->im[l][0];
        if (im->count) {
          im->instance_matrix->version++;
          t3_attribute_add_update_range(im->instance_matrix, 0, im->count);
        }
      }
  }
  for (int z = z0; z <= z1; z++)
    for (int x = x0; x <= x1; x++) {
      sc_cell *c = &s->cells[z * s->cnx + x];
      if (!c->live) continue;
      t->stats.scatter_cells++;
      if (c->im[0][0] && c->im[0][0]->mesh.base.visible) {
        t->stats.scatter_cells_visible++;
        t->stats.scatter_instances += c->drawn;
        for (int l = 0; l < s->nlod; l++) t->stats.scatter_draws += (c->im[l][0]->count > 0) * s->nparts[l];
      }
    }
  cw_free(gen);
  cw_free(pose);
  (void)sp;
}

void cw__scatter_update(cw_terrain *t, t3_vec3 cam, const t3_mat4 *vp) {
  double t0 = tr_now_ms();
  t->stats.scatter_cells = t->stats.scatter_cells_visible = t->stats.scatter_instances = t->stats.scatter_draws = 0;
  t3_frustum fr;
  if (vp) t3_frustum_from_matrix(&fr, vp);
  for (int i = 0; i < TR_MAX_SCATTER; i++)
    if (t->scatter[i]) {
      cw__scatter_attach(t, t->scatter[i]);
      scatter_one(t, t->scatter[i], cam, vp ? &fr : NULL);
    }
  t->stats.scatter_ms = (float)(tr_now_ms() - t0);
}
