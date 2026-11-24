/* Terrain scatter (cartwheel/terrain.h): grass, rocks and trees placed
 * over the terrain in square cells (32 m or more: see cartwheel/terrain.h).
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

#include "cartwheel/debug.h"
#include "cartwheel/jobs.h"
#include "cartwheel/math.h"
#include "cartwheel/quality.h"
#include "terrain_internal.h"
#include "../render/view_layers.h"

#define INST_F 12 /* x y z scale qx qy qz qw rank r g b (the instance colour, when the layer has one) */
#define CELL_MAX 8192

/* a cell as one view draws it: its GPU copies posed for that view's camera (a one-pass split screen's later views
 * keep their own, on their view's layer: src/render/view_layers.h) */
typedef struct sc_view {
  bool live;
  t3_instanced_mesh *im[CW_SCATTER_LODS][CW_SCATTER_PARTS]; /* parts share part 0's instance matrices */
  int posed_state;                /* -2 never; -1 dynamic; else the LOD it was posed whole at */
  float posed_density;
  int drawn;
} sc_view;

typedef struct sc_cell {
  int cx, cz;
  bool generated;
  bool generating;                /* in the batch the job pool is placing */
  int n;                          /* instances */
  float *inst;                    /* n * INST_F */
  float sphere[4];
  float ymin, ymax;
  sc_view v[TR_VIEWS];
} sc_cell;

struct tr_scatter {
  cw_scatter_def def;
  uint8_t *dm;
  int nlod, nparts[CW_SCATTER_LODS];
  int cnx, cnz;
  sc_cell *cells;                 /* cnx * cnz, generated lazily */
  float radius;                   /* the finest mesh's bounding radius */
  float cell;                     /* cell size, metres: 32 x a power of two (TR_SCATTER_CELL) */
  t3_object *parent;              /* the terrain's group (cw__terrain_parent), NULL out of a scene */
  /* cells being placed on the job pool (one batch at a time; the main thread never waits for it) */
  cw_job *gen;
  struct gen_job *gen_ctx;
  bool view_used[TR_VIEWS];       /* a later view has GPU copies of some cells (freed when the views draw one at a time) */
  bool icol;                      /* instances carry a colour (tint_jitter, root_from_terrain) */
  bool foliage;                   /* the materials are the layer's own copies with the foliage parts */
  bool pixel_dither;              /* DITHER drawn as the screen-door control (CW_TEST_FOLIAGE_PIXEL_DITHER), read each
                                   * update (pixel_dither_tick): its materials and its posing change together */
  float foliage_top;              /* LOD 0's top (the foliage parts' height) */
  float tint[3];                  /* the terrain material's colour when cells were last placed (root_from_terrain) */
};

static bool gen_collect(tr_scatter *s, bool wait);

cw_scatter_def cw_scatter_default_def(void) {
  cw_wind w = cw_wind_default();
  w.strength = 0;
  return (cw_scatter_def){ .density = 0.5f, .scale_min = 0.8f, .scale_max = 1.2f, .visible_range = 60, .fade = 10,
                           .density_knob = "grass", .align_to_normal = true, .wind = w, .lod_blend = 6 };
}

static bool foliage_wanted(const cw_scatter_def *d) {
  return d->wind.strength > 0 || d->smooth_alpha || d->two_sided_lighting || d->fade_mode == CW_SCATTER_FADE_DITHER ||
         d->root_from_terrain || d->bend > 0 || d->shadow_at_root;
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

/* DITHER as a screen-door on every pixel (0.76.1 and before): CW_TEST_FOLIAGE_PIXEL_DITHER=1, the grain gate's control.
 * A screen-door is a fixed pattern of dropped pixels each frame; TAA resolves it only where the picture holds still
 * (a moving pixel keeps a quarter of its history), so in a run or a pan the dots of the LOD crossfade and the fade
 * stayed on the grass, worst where lit blades cross a dark shadow. */
static bool pixel_dither(void) {
  const char *e = cw_test_env("CW_TEST_FOLIAGE_PIXEL_DITHER");
  return e && *e && *e != '0';
}

/* a layer's foliage description, without the fades */
static cw_foliage_desc layer_desc(const tr_scatter *s) {
  const cw_scatter_def *d = &s->def;
  cw_foliage_desc fd = cw_foliage_default();
  fd.wind = d->wind;
  fd.height = s->foliage_top;
  fd.smooth_alpha = d->smooth_alpha;
  fd.two_sided_lighting = d->two_sided_lighting;
  fd.root_color = d->root_from_terrain;
  fd.root_height = d->root_height;
  fd.tip_jitter = d->root_from_terrain ? d->tint_jitter : 0;
  fd.bend = d->bend;
  fd.shadow_at_root = d->shadow_at_root;
  fd.shadow_soften = d->shadow_soften;
  return fd;
}

/* LOD l's fades in its material: the screen-door control's alone (per instance, the posing drops instances) */
static void layer_fades(const tr_scatter *s, int l, cw_foliage_desc *fd) {
  const cw_scatter_def *d = &s->def;
  if (d->fade_mode != CW_SCATTER_FADE_DITHER || !s->pixel_dither) return;
  float vr = d->visible_range, hw = d->lod_blend * 0.5f, end0 = vr - d->fade;
  if (l > 0) fd->fade_in_from = d->lod_distance[l - 1] - hw, fd->fade_in_to = d->lod_distance[l - 1] + hw;
  if (l + 1 < s->nlod && d->lod_distance[l] - hw < end0) fd->fade_out_from = d->lod_distance[l] - hw, fd->fade_out_to = d->lod_distance[l] + hw;
  else fd->fade_out_from = end0, fd->fade_out_to = vr;
}

/* the control switch, read each update rather than once when the layer is made: a layer a scene file holds is made
 * on frame 0, where cw_test_env answers nothing (no cart host has delivered the env yet), so it never saw it. A
 * change sets the layer's materials' fades again and poses every cell again. */
static void pixel_dither_tick(tr_scatter *s) {
  bool pd = pixel_dither();
  if (pd == s->pixel_dither) return;
  s->pixel_dither = pd;
  if (s->foliage)
    for (int l = 0; l < s->nlod; l++) {
      cw_foliage_desc fd = layer_desc(s);
      layer_fades(s, l, &fd);
      for (int p = 0; p < s->nparts[l]; p++) cw_foliage_apply(s->def.material[l][p], &fd);
    }
  for (int i = 0; i < s->cnx * s->cnz; i++)
    for (int v = 0; v < TR_VIEWS; v++) s->cells[i].v[v].posed_state = -2;
}

/* an instance's own number in [0, 1), from its rank's bits (independent of the rank itself, which the density
 * thins by): where in a band the instance crosses */
static float inst_hash(const float *in) {
  uint32_t h;
  memcpy(&h, &in[8], 4);
  h ^= h >> 16, h *= 0x7FEB352Du, h ^= h >> 15, h *= 0x846CA68Bu, h ^= h >> 16;
  return (float)(h >> 8) * (1.0f / 16777216.0f);
}

/* DITHER per instance: the share of a band (a LOD crossfade, the fade) each instance takes to cross it (each starts at its
 * own point) */
#define FADE_SHARE 0.3f

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
  float area = s->cell * s->cell;
  float want = d->density * area;
  if (!(want > 0)) return 0;
  int k = (int)ceilf(sqrtf(want));
  if (k * k > CELL_MAX) k = (int)sqrtf((float)CELL_MAX);
  float p = want / (float)(k * k); /* chance a stratum holds one */
  float cell = s->cell / (float)k;
  float x0 = (float)cx * s->cell, z0 = (float)cz * s->cell; /* terrain-local metres */
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
      o[0] = lx; /* x, z in the terrain's own frame (posing adds the origin): a moved terrain places nothing again */
      o[1] = cw__terrain_height_fast(t, fx, fz) - d->sink * scale;
      o[2] = lz;
      o[3] = scale;
      orient(yaw, nrm, d->align_to_normal, &o[4]);
      o[8] = unit(mix32(h5 ^ 0x2545F491u)); /* rank */
      o[9] = o[10] = o[11] = 1;
      if (s->icol) {
        /* (hashes of their own: the placement above is what it was) */
        uint32_t h6 = mix32(h5 ^ 0x7F4A7C15u), h7 = mix32(h6 ^ 0x94D049BBu);
        float j = cw_clampf(d->tint_jitter, 0, 1);
        float b = 1 + j * (unit(h6) - 0.5f) * 1.2f, y = j * (unit(h7) - 0.5f) * 2; /* brightness; + a dry yellow, - a deep green */
        float col[3] = { b * (1 + 0.45f * y), b * (1 + 0.1f * y), b * (1 - 0.6f * y) };
        if (d->root_from_terrain) {
          t3_vec3 a = cw__terrain_albedo_fast(t, fx, fz);
          col[0] *= a.x * s->tint[0], col[1] *= a.y * s->tint[1], col[2] *= a.z * s->tint[2];
        }
        o[9] = col[0], o[10] = col[1], o[11] = col[2];
      }
    }
  return n;
}

int cw_terrain_scatter_cell(cw_terrain *t, int layer, int cx, int cz, float *out, int cap) {
  if (!t || layer < 0 || layer >= TR_MAX_SCATTER || !t->scatter[layer]) return 0;
  float *tmp = cw_alloc(sizeof(float) * INST_F * CELL_MAX, CW_MEM_RENDER);
  int n = place(t, t->scatter[layer], cx, cz, tmp, CELL_MAX);
  int m = CW_MIN(n, cap);
  for (int i = 0; i < m; i++) {
    out[5 * i] = t->def.origin.x + tmp[INST_F * i];
    out[5 * i + 1] = tmp[INST_F * i + 1];
    out[5 * i + 2] = t->def.origin.z + tmp[INST_F * i + 2];
    out[5 * i + 3] = tmp[INST_F * i + 3];
    /* the yaw back from the quaternion's y / w (cw_atan2: the same bits on every host) */
    out[5 * i + 4] = 2 * cw_atan2(tmp[INST_F * i + 5], tmp[INST_F * i + 7]);
  }
  cw_free(tmp);
  return m;
}

int cw_terrain_scatter_cell_colors(cw_terrain *t, int layer, int cx, int cz, float *out, int cap) {
  if (!t || layer < 0 || layer >= TR_MAX_SCATTER || !t->scatter[layer]) return 0;
  tr_scatter *s = t->scatter[layer];
  if (s->def.root_from_terrain) {
    t3_material *tm = cw_terrain_material(t); /* (the tint the next placement takes) */
    if (tm) s->tint[0] = tm->color.r, s->tint[1] = tm->color.g, s->tint[2] = tm->color.b;
  }
  float *tmp = cw_alloc(sizeof(float) * INST_F * CELL_MAX, CW_MEM_RENDER);
  int n = place(t, s, cx, cz, tmp, CELL_MAX);
  int m = CW_MIN(n, cap);
  for (int i = 0; i < m; i++) memcpy(&out[3 * i], &tmp[INST_F * i + 9], sizeof(float) * 3);
  cw_free(tmp);
  return m;
}

/* ── layers ─────────────────────────────────────────────────────────── */

static void cell_free_view(tr_scatter *s, sc_cell *c, int view) {
  sc_view *cv = &c->v[view];
  for (int l = 0; l < CW_SCATTER_LODS; l++)
    for (int p = 0; p < CW_SCATTER_PARTS; p++)
      if (cv->im[l][p]) {
        if (s->parent) t3_object_remove(s->parent, cv->im[l][p]);
        t3_release(cv->im[l][p]);
        cv->im[l][p] = NULL;
      }
  cv->live = false;
  cv->posed_state = -2;
  cv->drawn = 0;
}
static void cell_free_gpu(tr_scatter *s, sc_cell *c) {
  for (int v = 0; v < TR_VIEWS; v++) cell_free_view(s, c, v);
}

static void cell_reset(tr_scatter *s, sc_cell *c) {
  cell_free_gpu(s, c);
  cw_free(c->inst);
  int cx = c->cx, cz = c->cz;
  memset(c, 0, sizeof *c);
  c->cx = cx, c->cz = cz;
  for (int v = 0; v < TR_VIEWS; v++) c->v[v].posed_state = -2;
}

void cw__scatter_free(tr_scatter *s) {
  if (!s) return;
  gen_collect(s, true);
  for (int i = 0; i < s->cnx * s->cnz; i++) cell_reset(s, &s->cells[i]);
  for (int l = 0; l < CW_SCATTER_LODS; l++)
    for (int p = 0; p < CW_SCATTER_PARTS; p++) {
      if (s->def.geometry[l][p]) t3_release(s->def.geometry[l][p]);
      if (s->def.material[l][p]) {
        if (s->foliage) cw_foliage_remove(s->def.material[l][p]); /* (the layer's own copy) */
        t3_release(s->def.material[l][p]);
      }
    }
  cw_free(s->cells);
  cw_free(s->dm);
  cw_free(s);
}

void cw__scatter_attach(cw_terrain *t, tr_scatter *s) {
  t3_object *parent = cw__terrain_parent(t);
  if (s->parent == parent) return;
  for (int i = 0; i < s->cnx * s->cnz; i++) cell_free_gpu(s, &s->cells[i]);
  s->parent = parent;
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
    /* a part with fewer LODs than part 0 keeps drawing its coarsest one: a tree's leaf cards rarely simplify
     * (cwpack gives them one level) while its trunk gets three, and the crown vanished at the first switch */
    for (int p = 1; l > 0 && p < CW_SCATTER_PARTS && s->def.geometry[l - 1][p]; p++)
      if (!s->def.geometry[l][p]) s->def.geometry[l][p] = s->def.geometry[l - 1][p], s->def.material[l][p] = s->def.material[l - 1][p];
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
  s->icol = s->def.tint_jitter > 0 || s->def.root_from_terrain;
  s->tint[0] = s->tint[1] = s->tint[2] = 1;
  if (!(s->def.lod_blend >= 0)) s->def.lod_blend = 0;
  s->pixel_dither = pixel_dither();
  if (foliage_wanted(&s->def)) {
    /* the layer's own copy of each LOD's part materials, with the foliage parts: the wind and the fades are this
     * layer's (a glTF's materials are shared with every other user of the file) */
    float top = 0.05f;
    for (int p = 0; p < s->nparts[0]; p++) {
      t3_geometry *g = s->def.geometry[0][p];
      if (!g->has_bounding_box) t3_geometry_compute_bounding_box(g);
      top = CW_MAX(top, g->bounding_box.max.y);
    }
    s->foliage_top = top;
    for (int l = 0; l < s->nlod; l++) {
      cw_foliage_desc fd = layer_desc(s);
      layer_fades(s, l, &fd);
      for (int p = 0; p < s->nparts[l]; p++) {
        t3_material *c = t3_material_clone(s->def.material[l][p]);
        if (!cw_foliage_apply(c, &fd)) cw_warn("terrain: a scatter layer's material takes no foliage (wind, fades): drawn without");
        t3_release(s->def.material[l][p]);
        s->def.material[l][p] = c;
      }
    }
    s->foliage = true;
  }
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
  /* cells grow with the range (a quarter of it), so a long-range layer (trees) stays a few dozen
   * draws, while a cell holds at most CELL_MAX candidates */
  s->cell = TR_SCATTER_CELL;
  while (s->cell * 2 <= s->def.visible_range * 0.25f && s->def.density * (s->cell * 2) * (s->cell * 2) <= CELL_MAX) s->cell *= 2;
  s->cnx = (int)ceilf((float)t->def.size_x * sp / s->cell);
  s->cnz = (int)ceilf((float)t->def.size_z * sp / s->cell);
  s->cells = cw_calloc((size_t)s->cnx * s->cnz, sizeof(sc_cell), CW_MEM_RENDER);
  for (int z = 0; z < s->cnz; z++)
    for (int x = 0; x < s->cnx; x++) {
      sc_cell *c = &s->cells[z * s->cnx + x];
      c->cx = x, c->cz = z;
      for (int v = 0; v < TR_VIEWS; v++) c->v[v].posed_state = -2;
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
    gen_collect(s, true);
    for (int k = 0; k < s->cnx * s->cnz; k++) cell_reset(s, &s->cells[k]);
  }
}

/* ── per frame ──────────────────────────────────────────────────────── */

typedef struct gen_job {
  cw_terrain *t;
  tr_scatter *s;
  sc_cell **list;
  int n;
} gen_job;

/* the batch in flight: adopt it once placed (wait: now) */
static bool gen_collect(tr_scatter *s, bool wait) {
  if (!s->gen) return true;
  if (!wait && !cw_job_done(s->gen)) return false;
  cw_job_wait(s->gen);
  s->gen = NULL;
  for (int i = 0; i < s->gen_ctx->n; i++) {
    sc_cell *c = s->gen_ctx->list[i];
    c->generating = false;
    c->generated = true;
  }
  cw_free(s->gen_ctx->list);
  cw_free(s->gen_ctx);
  s->gen_ctx = NULL;
  return true;
}

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
  }
  cw_free(tmp);
}

typedef struct pose_job {
  const cw_terrain *t;
  tr_scatter *s;
  sc_cell **list;
  float cam[3];
  float origin[2]; /* the terrain's x, z: instances are stored relative to it */
  float density;
  int view;
} pose_job;

/* instance i's matrix (column major) at scale k */
static void write_matrix(float *m, const float *in, float k, float ox, float oz) {
  float x = in[4], y = in[5], z = in[6], w = in[7], s = in[3] * k;
  float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
  m[0] = (1 - 2 * (yy + zz)) * s, m[1] = 2 * (xy + wz) * s, m[2] = 2 * (xz - wy) * s, m[3] = 0;
  m[4] = 2 * (xy - wz) * s, m[5] = (1 - 2 * (xx + zz)) * s, m[6] = 2 * (yz + wx) * s, m[7] = 0;
  m[8] = 2 * (xz + wy) * s, m[9] = 2 * (yz - wx) * s, m[10] = (1 - 2 * (xx + yy)) * s, m[11] = 0;
  m[12] = in[0] + ox, m[13] = in[1], m[14] = in[2] + oz, m[15] = 1;
}

static void pose_cells(int begin, int end, int worker, void *u) {
  (void)worker;
  pose_job *j = u;
  const cw_scatter_def *d = &j->s->def;
  int nl = j->s->nlod;
  float vr = d->visible_range, fs = vr - d->fade;
  bool dither = d->fade_mode == CW_SCATTER_FADE_DITHER, per_pixel = dither && j->s->pixel_dither;
  float hw = d->lod_blend * 0.5f, rr = j->s->radius * d->scale_max + d->wind.strength;
  for (int ci = begin; ci < end; ci++) {
    sc_cell *c = j->list[ci];
    float *dst[CW_SCATTER_LODS] = { 0 }, *col[CW_SCATTER_LODS] = { 0 };
    int cnt[CW_SCATTER_LODS] = { 0 };
    sc_view *cv = &c->v[j->view];
    for (int l = 0; l < nl; l++) {
      dst[l] = t3_attribute_f32(cv->im[l][0]->instance_matrix);
      if (j->s->icol) col[l] = t3_attribute_f32(cv->im[l][0]->instance_color);
    }
    tr_v4f cx = { j->cam[0], j->cam[0], j->cam[0], j->cam[0] }, cy = { j->cam[1], j->cam[1], j->cam[1], j->cam[1] },
           cz = { j->cam[2], j->cam[2], j->cam[2], j->cam[2] };
    for (int i = 0; i < c->n; i += 4) {
      /* four distances at once */
      tr_v4f px, py, pz;
      for (int k = 0; k < 4; k++) {
        const float *in = &c->inst[INST_F * CW_MIN(i + k, c->n - 1)];
        px[k] = in[0] + j->origin[0], py[k] = in[1], pz[k] = in[2] + j->origin[1];
      }
      tr_v4f dx = px - cx, dy = py - cy, dz = pz - cz;
      tr_v4f d2 = dx * dx + dy * dy + dz * dz;
      for (int k = 0; k < 4 && i + k < c->n; k++) {
        const float *in = &c->inst[INST_F * (i + k)];
        if (in[8] >= j->density) continue;
        float dist = sqrtf(d2[k]);
        if (dither && !per_pixel) {
          /* each instance crosses each band at its own point (inst_hash), over its own share of the band: at a LOD
           * crossfade the next LOD grows in inside it over the first half of that share and then it shrinks away
           * over the second, and over the fade it shrinks away. Whole instances that change size smoothly: no
           * pattern of dropped pixels for TAA to resolve (no dots in motion), no pop */
          if (dist >= vr) continue;
          float h = inst_hash(in), st = h * (1 - FADE_SHARE);
          float k = 1;
          if (dist > fs && vr > fs) {
            k = 1 - cw_smoothstep(st, st + FADE_SHARE, (dist - fs) / (vr - fs));
            if (k < 0.02f) continue;
          }
          float grow = 1; /* the share this LOD has grown in (from the band before it) */
          for (int l = 0; l < nl; l++) {
            float shrink = 1, next_grow = 0;
            if (l + 1 < nl && d->lod_distance[l] - hw < fs) {
              float a = d->lod_distance[l];
              float u = hw > 0 ? cw_clampf((dist - (a - hw)) / (2 * hw), 0, 1) : dist >= a ? 1.0f : 0.0f;
              float w = hw > 0 ? cw_clampf((u - st) / FADE_SHARE, 0, 1) : u;
              next_grow = cw_smoothstep(0, 0.5f, w);
              shrink = 1 - cw_smoothstep(0.5f, 1, w);
            }
            float kl = k * grow * shrink;
            if (kl >= 0.02f) {
              if (col[l]) memcpy(col[l] + 3 * cnt[l], &in[9], sizeof(float) * 3);
              write_matrix(dst[l] + 16 * cnt[l]++, in, kl, j->origin[0], j->origin[1]);
            }
            if (next_grow <= 0) break;
            grow = next_grow;
          }
          continue;
        }
        if (dither) {
          /* whole instances; the shaders dissolve them over the fade and crossfade each LOD band (foliage.h): an
           * instance within its reach (rr) of a band is drawn by both LODs */
          if (dist >= vr + rr) continue;
          for (int l = 0; l < nl; l++) {
            if (l > 0 && dist < d->lod_distance[l - 1] - hw - rr) continue;
            if (l + 1 < nl && dist >= d->lod_distance[l] + hw + rr) continue;
            if (col[l]) memcpy(col[l] + 3 * cnt[l], &in[9], sizeof(float) * 3);
            write_matrix(dst[l] + 16 * cnt[l]++, in, 1, j->origin[0], j->origin[1]);
          }
          continue;
        }
        if (dist >= vr) continue;
        float f = dist > fs ? 1 - cw_smoothstep(fs, vr, dist) : 1;
        int l = 0;
        while (l + 1 < nl && dist >= d->lod_distance[l]) l++;
        if (col[l]) memcpy(col[l] + 3 * cnt[l], &in[9], sizeof(float) * 3);
        write_matrix(dst[l] + 16 * cnt[l]++, in, f, j->origin[0], j->origin[1]);
      }
    }
    int drawn = 0;
    for (int l = 0; l < nl; l++) {
      for (int p = 0; p < j->s->nparts[l]; p++) {
        cv->im[l][p]->count = cnt[l];
        cv->im[l][p]->mesh.base.visible = cnt[l] > 0; /* no draw for an empty LOD */
      }
      drawn += cnt[l];
    }
    cv->drawn = drawn;
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

/* DITHER: 2 l inside LOD l alone, 2 l + 1 in the band where LODs l and l + 1 both draw */
static int dither_region(const tr_scatter *s, float dist) {
  float hw = s->def.lod_blend * 0.5f, rr = s->radius * s->def.scale_max + s->def.wind.strength;
  int r = 0;
  for (int l = 0; l + 1 < s->nlod; l++) {
    if (dist < s->def.lod_distance[l] - hw - rr) return r;
    if (dist < s->def.lod_distance[l] + hw + rr) return r + 1;
    r += 2;
  }
  return r;
}

static void scatter_one(cw_terrain *t, tr_scatter *s, t3_vec3 cam, const t3_frustum *fr, int view, int views) {
  /* the layer this view's copies are on: layer 0 while the views draw one at a time */
  uint32_t layer = views ? cw__view_layer(view) : 1;
  if (view) s->view_used[view] = true;
  const cw_scatter_def *d = &s->def;
  float sp = t->def.vertex_spacing;
  float vr = d->visible_range;
  float camv[3] = { cam.x, cam.y, cam.z };
  float lx = cam.x - t->def.origin.x, lz = cam.z - t->def.origin.z;
  int x0 = CW_MAX(0, (int)floorf((lx - vr) / s->cell)), x1 = CW_MIN(s->cnx - 1, (int)floorf((lx + vr) / s->cell));
  int z0 = CW_MAX(0, (int)floorf((lz - vr) / s->cell)), z1 = CW_MIN(s->cnz - 1, (int)floorf((lz + vr) / s->cell));
  float density = cw_quality_scale(d->density_knob);
  /* cells that left the range drop their GPU copies (their placement stays: it is cheap to keep) */
  for (int i = 0; i < s->cnx * s->cnz; i++) {
    sc_cell *c = &s->cells[i];
    if (c->v[view].live && (c->cx < x0 - 1 || c->cx > x1 + 1 || c->cz < z0 - 1 || c->cz > z1 + 1)) cell_free_view(s, c, view);
    if (c->v[view].live)
      for (int l = 0; l < s->nlod; l++)
        for (int p = 0; p < s->nparts[l]; p++) c->v[view].im[l][p]->mesh.base.layers = layer;
  }
  if (x1 < x0 || z1 < z0) return;
  int span = (x1 - x0 + 1) * (z1 - z0 + 1);
  sc_cell **gen = cw_alloc(sizeof(sc_cell *) * (size_t)span, CW_MEM_RENDER);
  sc_cell **pose = cw_alloc(sizeof(sc_cell *) * (size_t)span, CW_MEM_RENDER);
  int ng = 0, np = 0;
  for (int z = z0; z <= z1; z++)
    for (int x = x0; x <= x1; x++) {
      sc_cell *c = &s->cells[z * s->cnx + x];
      float mn[3] = { t->def.origin.x + (float)x * s->cell, -1e30f, t->def.origin.z + (float)z * s->cell };
      float mx[3] = { mn[0] + s->cell, 1e30f, mn[2] + s->cell };
      mn[1] = camv[1], mx[1] = camv[1]; /* height unknown before generation: horizontal distance */
      if (cell_dist2(camv, mn, mx) < vr * vr && !c->generated && !c->generating) gen[ng++] = c;
    }
  /* placement runs on the job pool without the frame waiting for it: a cell is placed a frame or two
   * after it comes into range, where it is still faded to nothing (the last `fade` metres) */
  gen_collect(s, false);
  if (ng && !s->gen) {
    if (d->root_from_terrain) {
      /* the terrain's tint, as cw_terrain_albedo_at gives it (read here, on the main thread) */
      t3_material *tm = cw_terrain_material(t);
      if (tm) s->tint[0] = tm->color.r, s->tint[1] = tm->color.g, s->tint[2] = tm->color.b;
    }
    gen_job *gj = cw_alloc(sizeof *gj, CW_MEM_RENDER);
    gj->t = t, gj->s = s, gj->n = ng;
    gj->list = cw_alloc(sizeof(sc_cell *) * (size_t)ng, CW_MEM_RENDER);
    memcpy(gj->list, gen, sizeof(sc_cell *) * (size_t)ng);
    for (int i = 0; i < ng; i++) gen[i]->generating = true;
    s->gen_ctx = gj;
    s->gen = cw_job_range(ng, 1, gen_cells, gj);
  }
  for (int z = z0; z <= z1; z++)
    for (int x = x0; x <= x1; x++) {
      sc_cell *c = &s->cells[z * s->cnx + x];
      if (!c->generated || !c->n) continue;
      float r = s->radius * d->scale_max + d->wind.strength; /* (the wind leans the tops out by up to its strength) */
      float mn[3] = { t->def.origin.x + (float)x * s->cell - r, c->ymin - r, t->def.origin.z + (float)z * s->cell - r };
      float mx[3] = { mn[0] + s->cell + 2 * r, c->ymax + r, mn[2] + s->cell + 2 * r };
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
      if (!c->v[view].live && vis) {
        /* GPU copies for a cell coming into view */
        for (int l = 0; l < s->nlod; l++)
          for (int p = 0; p < s->nparts[l]; p++) {
            t3_instanced_mesh *im = t3_instanced_mesh_new(d->geometry[l][p], d->material[l][p], c->n);
            im->count = 0;
            if (p) {
              /* the parts draw part 0's instances: one pose, one upload */
              t3_release(im->instance_matrix);
              im->instance_matrix = t3_retain(c->v[view].im[l][0]->instance_matrix);
            }
            im->instance_matrix->usage = T3_DYNAMIC_DRAW;
            if (s->icol) {
              if (p) im->instance_color = t3_retain(c->v[view].im[l][0]->instance_color);
              else t3_instanced_mesh_set_color_at(im, 0, (t3_color){ 1, 1, 1 });
              im->instance_color->usage = T3_DYNAMIC_DRAW;
            }
            im->mesh.base.cast_shadow = l == 0 && d->cast_shadow && view == 0; /* (the first view's draw the shadow maps) */
            im->mesh.base.receive_shadow = true;
            t3_sphere sph = { { (mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f }, 0 };
            float ex = (mx[0] - mn[0]) * 0.5f, ey = (mx[1] - mn[1]) * 0.5f, ez = (mx[2] - mn[2]) * 0.5f;
            sph.radius = sqrtf(ex * ex + ey * ey + ez * ez);
            t3_instanced_mesh_set_bounding_sphere(im, &sph);
            im->mesh.base.layers = layer;
            c->v[view].im[l][p] = im;
            if (s->parent) t3_object_add(s->parent, im);
          }
        c->v[view].live = true;
        c->v[view].posed_state = -2;
      }
      if (!c->v[view].live) continue;
      if (!vis) {
        for (int l = 0; l < s->nlod; l++)
          for (int p = 0; p < s->nparts[l]; p++) c->v[view].im[l][p]->mesh.base.visible = false;
        c->v[view].drawn = 0;
        c->v[view].posed_state = -2; /* posed again when it comes back into view */
        continue;
      }
      /* a cell wholly in one LOD band, short of the fade, keeps its pose */
      float dmin = sqrtf(d2), dmax;
      {
        /* the farthest corner */
        float fx = camv[0] - mn[0] > mx[0] - camv[0] ? camv[0] - mn[0] : mx[0] - camv[0];
        float fy = camv[1] - mn[1] > mx[1] - camv[1] ? camv[1] - mn[1] : mx[1] - camv[1];
        float fz = camv[2] - mn[2] > mx[2] - camv[2] ? camv[2] - mn[2] : mx[2] - camv[2];
        dmax = sqrtf(fx * fx + fy * fy + fz * fz);
      }
      int la = 0, lb = 0, state;
      if (d->fade_mode == CW_SCATTER_FADE_DITHER) {
        /* which LODs draw an instance changes only at a band's edges, pushed out by an instance's reach */
        la = dither_region(s, dmin), lb = dither_region(s, dmax);
        state = la == lb && !(la & 1) && dmax < vr - d->fade ? la / 2 : -1;
      } else {
        while (la + 1 < s->nlod && dmin >= d->lod_distance[la]) la++;
        while (lb + 1 < s->nlod && dmax >= d->lod_distance[lb]) lb++;
        state = la == lb && dmax < vr - d->fade ? la : -1;
      }
      if (state >= 0 && state == c->v[view].posed_state && c->v[view].posed_density == density) continue;
      c->v[view].posed_state = state;
      c->v[view].posed_density = density;
      pose[np++] = c;
    }
  if (np) {
    pose_job pj = { t, s, pose, { cam.x, cam.y, cam.z }, { t->def.origin.x, t->def.origin.z }, density, view };
    cw_parallel_for(np, 1, pose_cells, &pj);
    for (int i = 0; i < np; i++)
      for (int l = 0; l < s->nlod; l++) {
        t3_instanced_mesh *im = pose[i]->v[view].im[l][0];
        if (im->count) {
          im->instance_matrix->version++;
          t3_attribute_add_update_range(im->instance_matrix, 0, im->count);
          if (s->icol) {
            im->instance_color->version++;
            t3_attribute_add_update_range(im->instance_color, 0, im->count);
          }
        }
      }
  }
  for (int z = z0; z <= z1; z++)
    for (int x = x0; x <= x1; x++) {
      sc_cell *c = &s->cells[z * s->cnx + x];
      if (!c->v[view].live) continue;
      t->stats.scatter_cells++;
      if (c->v[view].drawn) {
        t->stats.scatter_cells_visible++;
        t->stats.scatter_instances += c->v[view].drawn;
        for (int l = 0; l < s->nlod; l++) t->stats.scatter_draws += (c->v[view].im[l][0]->count > 0) * s->nparts[l];
      }
    }
  cw_free(gen);
  cw_free(pose);
  (void)sp;
}

void cw__scatter_update(cw_terrain *t, t3_vec3 cam, const t3_mat4 *vp, int view, int views) {
  double t0 = tr_now_ms();
  /* the views draw one at a time: later views' copies go (the first view's are the ones drawn) */
  if (view == 0 && views == 0)
    for (int i = 0; i < TR_MAX_SCATTER; i++) {
      tr_scatter *s = t->scatter[i];
      if (!s) continue;
      for (int v = 1; v < TR_VIEWS; v++)
        if (s->view_used[v]) {
          for (int k = 0; k < s->cnx * s->cnz; k++) cell_free_view(s, &s->cells[k], v);
          s->view_used[v] = false;
        }
    }
  t->stats.scatter_cells = t->stats.scatter_cells_visible = t->stats.scatter_instances = t->stats.scatter_draws = 0;
  t3_frustum fr;
  if (vp) t3_frustum_from_matrix(&fr, vp);
  for (int i = 0; i < TR_MAX_SCATTER; i++)
    if (t->scatter[i]) {
      cw__scatter_attach(t, t->scatter[i]);
      if (view == 0) pixel_dither_tick(t->scatter[i]);
      scatter_one(t, t->scatter[i], cam, vp ? &fr : NULL, view, views);
    }
  t->stats.scatter_ms = (float)(tr_now_ms() - t0);
}

/* The terrain moved by d (cw_terrain_set_origin): placement is the same in
 * the terrain's own frame, so the generated cells move with it instead of
 * being placed again; live cells keep their GPU copies (bounds moved) and
 * are posed again at the next update. */
void cw__scatter_shift(cw_terrain *t, t3_vec3 d) {
  for (int i = 0; i < TR_MAX_SCATTER; i++) {
    tr_scatter *s = t->scatter[i];
    if (!s) continue;
    gen_collect(s, true); /* (cw__scatter_sync ran before the origin moved) */
    for (int k = 0; k < s->cnx * s->cnz; k++) {
      sc_cell *c = &s->cells[k];
      if (!c->generated) continue;
      if (d.y != 0)
        for (int n = 0; n < c->n; n++) c->inst[INST_F * n + 1] += d.y; /* x, z are relative to the terrain already */
      c->ymin += d.y, c->ymax += d.y;
      for (int v = 0; v < TR_VIEWS; v++) {
        for (int l = 0; l < CW_SCATTER_LODS; l++)
          for (int p = 0; p < CW_SCATTER_PARTS; p++) {
            t3_instanced_mesh *im = c->v[v].im[l][p];
            if (!im) continue;
            t3_sphere sph = { { im->_isph[0] + d.x, im->_isph[1] + d.y, im->_isph[2] + d.z }, im->_isph[3] };
            t3_instanced_mesh_set_bounding_sphere(im, &sph);
          }
        c->v[v].posed_state = -2;
      }
    }
  }
}

/* Wait for placement in flight: before the terrain's heights, control or
 * origin change (the jobs read them). */
void cw__scatter_sync(cw_terrain *t) {
  for (int i = 0; i < TR_MAX_SCATTER; i++)
    if (t->scatter[i]) gen_collect(t->scatter[i], true);
}

/* The layers' colours changed (cw_terrain_set_layer): the layers whose roots take the terrain's colour place
 * their cells again (their colours are made with the placement). */
void cw__scatter_colors_dirty(cw_terrain *t) {
  for (int i = 0; i < TR_MAX_SCATTER; i++) {
    tr_scatter *s = t->scatter[i];
    if (!s || !s->def.root_from_terrain) continue;
    gen_collect(s, true);
    for (int k = 0; k < s->cnx * s->cnz; k++) cell_reset(s, &s->cells[k]);
  }
}
