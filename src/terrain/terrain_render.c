/* The terrain's GPU side (cartwheel/terrain.h): textures, the patch grids,
 * the instanced meshes the selection fills, and the fill hook of the
 * terrain extensions (src/terrain/extensions/terrain_shading.mjs names what
 * it sets).
 *
 * Four instanced meshes: {near, far} x {full grid, half grid}. Each patch is
 * one instance; its matrix scales the unit grid (vertices at y = 1) to the
 * patch and carries the patch's level in its Y column, which the vertex
 * stage reads back from positionLocal.y (the grid's y is 1, so the level is
 * exactly what lands there). The objects are never frustum culled by
 * three.c: the selection culled the patches.
 *
 * Textures (all RGBA8):
 *   heights   (size + 1)^2, linear + mipmaps: the uint16 step, high byte
 *             in R, low in G (read with textureLoad, exact); the normal's
 *             x, z in B, A (0.5 = 0; sampled filtered)
 *   control   (size + 1)^2, nearest: the control word's bytes, big end in R
 *   macro     at most 1024^2, linear + mipmaps, sRGB: the layers' mean
 *             colours blended by the control map (the distant colour)
 *   layers    6 x 3 cells of TR_CELL: layer i's albedo + height (sRGB data,
 *             decoded in the shader) in cell (i % 3, i / 3), its normal +
 *             roughness three cells right; each tile with a wrapped gutter
 *             (so mipmaps and filtering repeat it). One texture for both
 *             keeps the program within three.c's eight per group. */
#include <math.h>
#include <string.h>

#include "cartwheel/jobs.h"
#include "cartwheel/math.h"
#include "cartwheel/quality.h"
#include "terrain_internal.h"

typedef struct tr_gpu {
  t3_scene *scene;
  t3_geometry *grid[2];
  t3_material *mat_near, *mat_far;
  t3_instanced_mesh *im[2][2]; /* [far][half] */
  int cap[2][2];
  t3_texture *heights, *control, *macro, *layers;
  int mw, mh, mdiv;
  int near_q;                  /* the quality the near material's program is for */
} tr_gpu;

/* the instanced meshes of every terrain, for the fill hook to find its terrain by the drawn object */
#define TR_MAX_TERRAINS 16
static struct { const void *obj; cw_terrain *t; } g_objs[TR_MAX_TERRAINS * 4];

static void reg_obj(const void *obj, cw_terrain *t) {
  for (int i = 0; i < (int)CW_ARRAY_LEN(g_objs); i++)
    if (!g_objs[i].obj || g_objs[i].obj == obj) {
      g_objs[i].obj = obj;
      g_objs[i].t = t;
      return;
    }
  cw_error("terrain: more than %d terrains", TR_MAX_TERRAINS);
}
static void unreg_obj(const void *obj) {
  for (int i = 0; i < (int)CW_ARRAY_LEN(g_objs); i++)
    if (g_objs[i].obj == obj) g_objs[i].obj = NULL, g_objs[i].t = NULL;
}
static cw_terrain *terrain_of_obj(const void *obj) {
  for (int i = 0; i < (int)CW_ARRAY_LEN(g_objs); i++)
    if (g_objs[i].obj == obj) return g_objs[i].t;
  return NULL;
}

static tr_gpu *gpu(cw_terrain *t) {
  if (!t->gpu) t->gpu = cw_calloc(1, sizeof(tr_gpu), CW_MEM_RENDER);
  return t->gpu;
}

int cw__terrain_quality(const cw_terrain *t) { return t->quality >= 0 ? t->quality : (int)cw_quality_get(); }

float cw__terrain_far(const cw_terrain *t) {
  static const float mul[CW_QUALITY_COUNT] = { 0.5f, 0.75f, 1.0f, 1.5f };
  return t->def.far_distance * mul[cw__terrain_quality(t)];
}

static const char *near_ext(int q) { return q >= CW_QUALITY_HIGH ? "terrain" : "terrainlow"; }

/* ── textures ───────────────────────────────────────────────────────── */

static t3_texture *data_tex(int w, int h, bool linear, bool srgb) {
  t3_texture *x = t3_data_texture_new(w, h, NULL);
  if (!x->pixels) x->pixels = t3_malloc((size_t)w * h * 4);
  memset(x->pixels, 0, (size_t)w * h * 4);
  x->wrap_s = x->wrap_t = T3_CLAMP_TO_EDGE;
  if (linear) {
    x->mag_filter = T3_LINEAR;
    x->min_filter = T3_LINEAR_MIPMAP_LINEAR;
    x->generate_mipmaps = true;
  } else {
    x->mag_filter = x->min_filter = T3_NEAREST;
    x->generate_mipmaps = false;
  }
  if (srgb) x->color_space = T3_SRGB_COLOR_SPACE;
  return x;
}

typedef struct rect_job {
  cw_terrain *t;
  int x0, z0, x1, z1;
} rect_job;

static void heights_rows(int begin, int end, int worker, void *u) {
  (void)worker;
  rect_job *j = u;
  cw_terrain *t = j->t;
  tr_gpu *g = t->gpu;
  for (int z = j->z0 + begin; z < j->z0 + end; z++)
    for (int x = j->x0; x <= j->x1; x++) {
      uint16_t q = t->h[tr_idx(t, x, z)];
      uint8_t *p = &g->heights->pixels[4 * (size_t)tr_idx(t, x, z)];
      p[0] = (uint8_t)(q >> 8), p[1] = (uint8_t)q;
    }
}

static void normal_rows(int begin, int end, int worker, void *u) {
  (void)worker;
  rect_job *j = u;
  cw_terrain *t = j->t;
  tr_gpu *g = t->gpu;
  float inv2 = t->sy / (2.0f * t->def.vertex_spacing);
  for (int z = j->z0 + begin; z < j->z0 + end; z++)
    for (int x = j->x0; x <= j->x1; x++) {
      int xa = CW_MAX(x - 1, 0), xb = CW_MIN(x + 1, t->nx - 1), za = CW_MAX(z - 1, 0), zb = CW_MIN(z + 1, t->nz - 1);
      float dx = ((float)t->h[tr_idx(t, xb, z)] - (float)t->h[tr_idx(t, xa, z)]) * inv2 * 2.0f / (float)(xb - xa);
      float dz = ((float)t->h[tr_idx(t, x, zb)] - (float)t->h[tr_idx(t, x, za)]) * inv2 * 2.0f / (float)(zb - za);
      float l = sqrtf(dx * dx + 1 + dz * dz);
      uint8_t *p = &g->heights->pixels[4 * (size_t)tr_idx(t, x, z)];
      p[2] = (uint8_t)cw_clampf((-dx / l) * 127.5f + 127.5f + 0.5f, 0, 255);
      p[3] = (uint8_t)cw_clampf((-dz / l) * 127.5f + 127.5f + 0.5f, 0, 255);
    }
}

static void control_rows(int begin, int end, int worker, void *u) {
  (void)worker;
  rect_job *j = u;
  cw_terrain *t = j->t;
  tr_gpu *g = t->gpu;
  for (int z = j->z0 + begin; z < j->z0 + end; z++)
    for (int x = j->x0; x <= j->x1; x++) {
      uint32_t c = t->ctl[tr_idx(t, x, z)];
      uint8_t *p = &g->control->pixels[4 * (size_t)tr_idx(t, x, z)];
      p[0] = (uint8_t)(c >> 24), p[1] = (uint8_t)(c >> 16), p[2] = (uint8_t)(c >> 8), p[3] = (uint8_t)c;
    }
}

static float linear_to_srgb(float c) {
  c = cw_clampf(c, 0, 1);
  return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f;
}

static void macro_rows(int begin, int end, int worker, void *u) {
  (void)worker;
  cw_terrain *t = u;
  tr_gpu *g = t->gpu;
  int d = g->mdiv;
  for (int mz = begin; mz < end; mz++)
    for (int mx = 0; mx < g->mw; mx++) {
      float acc[3] = { 0, 0, 0 };
      int n = 0;
      /* the block of samples this texel stands for, every other one */
      for (int z = mz * d; z <= CW_MIN(mz * d + d - 1, t->nz - 1); z += (d > 2 ? 2 : 1))
        for (int x = mx * d; x <= CW_MIN(mx * d + d - 1, t->nx - 1); x += (d > 2 ? 2 : 1)) {
          uint32_t c = t->ctl[tr_idx(t, x, z)];
          int a = CW_MIN((int)(c >> 27), CW_TERRAIN_LAYERS - 1), b = CW_MIN((int)((c >> 22) & 31), CW_TERRAIN_LAYERS - 1);
          float w = (float)((c >> 14) & 255) / 255.0f;
          for (int k = 0; k < 3; k++) acc[k] += t->layer[a].mean[k] + (t->layer[b].mean[k] - t->layer[a].mean[k]) * w;
          n++;
        }
      uint8_t *p = &g->macro->pixels[4 * ((size_t)mz * g->mw + mx)];
      for (int k = 0; k < 3; k++) p[k] = (uint8_t)(linear_to_srgb(acc[k] / (float)CW_MAX(n, 1)) * 255.0f + 0.5f);
      p[3] = 255;
    }
}

static void build_atlas(cw_terrain *t, t3_texture *tex, bool normal) {
  static const uint8_t def_albedo[4] = { 128, 128, 128, 128 }, def_normal[4] = { 128, 128, 255, 230 };
  for (int i = 0; i < 9; i++) {
    int cx = (i % TR_ATLAS_COLS + (normal ? TR_ATLAS_COLS : 0)) * TR_CELL, cy = (i / TR_ATLAS_COLS) * TR_CELL;
    const uint8_t *src = i < CW_TERRAIN_LAYERS ? (normal ? t->layer[i].normal : t->layer[i].albedo) : NULL;
    for (int y = 0; y < TR_CELL; y++) {
      int sy = ((y - TR_GUTTER) % TR_TILE + TR_TILE) % TR_TILE;
      uint8_t *row = &tex->pixels[4 * ((size_t)(cy + y) * TR_ATLAS_W + cx)];
      for (int x = 0; x < TR_CELL; x++) {
        int sx = ((x - TR_GUTTER) % TR_TILE + TR_TILE) % TR_TILE;
        memcpy(row + 4 * x, src ? &src[4 * (sy * TR_TILE + sx)] : (normal ? def_normal : def_albedo), 4);
      }
    }
  }
  tex->version++;
}

/* ── meshes ─────────────────────────────────────────────────────────── */

static t3_geometry *grid_geometry(int n) {
  int nv = (n + 1) * (n + 1);
  float *pos = cw_alloc(sizeof(float) * 3 * (size_t)nv, CW_MEM_RENDER);
  float *nrm = cw_alloc(sizeof(float) * 3 * (size_t)nv, CW_MEM_RENDER);
  float *uv = cw_alloc(sizeof(float) * 2 * (size_t)nv, CW_MEM_RENDER);
  for (int j = 0; j <= n; j++)
    for (int i = 0; i <= n; i++) {
      int v = j * (n + 1) + i;
      pos[3 * v] = (float)i / (float)n, pos[3 * v + 1] = 1, pos[3 * v + 2] = (float)j / (float)n;
      nrm[3 * v] = 0, nrm[3 * v + 1] = 1, nrm[3 * v + 2] = 0;
      uv[2 * v] = (float)i / (float)n, uv[2 * v + 1] = (float)j / (float)n;
    }
  uint16_t *idx = cw_alloc(sizeof(uint16_t) * 6 * (size_t)n * n, CW_MEM_RENDER);
  int k = 0;
  for (int j = 0; j < n; j++)
    for (int i = 0; i < n; i++) {
      uint16_t a = (uint16_t)(j * (n + 1) + i), b = (uint16_t)(a + 1), c = (uint16_t)(a + n + 1), d = (uint16_t)(c + 1);
      /* Box3D's split: the diagonal from (x, z + 1) to (x + 1, z); counter-clockwise from above */
      idx[k++] = a, idx[k++] = c, idx[k++] = b;
      idx[k++] = c, idx[k++] = d, idx[k++] = b;
    }
  t3_geometry *g = t3_geometry_new();
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, t3_attribute_new(T3_FLOAT32, pos, nv, 3));
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, t3_attribute_new(T3_FLOAT32, nrm, nv, 3));
  t3_geometry_set_attribute(g, T3_ATTR_UV, t3_attribute_new(T3_FLOAT32, uv, nv, 2));
  t3_geometry_set_index(g, t3_attribute_new(T3_UINT16, idx, 6 * n * n, 1));
  cw_free(pos);
  cw_free(nrm);
  cw_free(uv);
  cw_free(idx);
  return g;
}

static void make_mesh(cw_terrain *t, int far, int half, int cap) {
  tr_gpu *g = t->gpu;
  t3_instanced_mesh *old = g->im[far][half];
  t3_instanced_mesh *im = t3_instanced_mesh_new(g->grid[half], far ? g->mat_far : g->mat_near, cap);
  im->count = 0;
  im->instance_matrix->usage = T3_DYNAMIC_DRAW;
  im->mesh.base.frustum_culled = false;
  im->mesh.base.cast_shadow = false;
  im->mesh.base.receive_shadow = true;
  im->mesh.base.visible = false;
  im->mesh.base.render_order = far ? 1 : 0;
  if (old) {
    unreg_obj(old);
    if (g->scene) t3_object_remove(&g->scene->base, old);
    t3_release(old);
  }
  g->im[far][half] = im;
  g->cap[far][half] = cap;
  reg_obj(im, t);
  if (g->scene) t3_object_add(&g->scene->base, im);
}

static void ensure_gpu(cw_terrain *t) {
  tr_gpu *g = gpu(t);
  if (g->grid[0]) return;
  int G = t->def.grid_quads;
  g->grid[0] = grid_geometry(G);
  g->grid[1] = grid_geometry(G / 2);
  g->mat_near = t3_mesh_standard_material_new(0xffffff);
  g->mat_near->roughness = 1;
  g->mat_near->metalness = 0;
  g->near_q = cw__terrain_quality(t);
  t3_material_set_extension(g->mat_near, near_ext(g->near_q));
  g->mat_far = t3_mesh_standard_material_new(0xffffff);
  g->mat_far->roughness = 1;
  g->mat_far->metalness = 0;
  t3_material_set_extension(g->mat_far, "terrainfar");
  g->heights = data_tex(t->nx, t->nz, true, false);
  g->control = data_tex(t->nx, t->nz, false, false);
  g->mdiv = 1;
  while ((t->nx - 1) / g->mdiv + 1 > 1025 || (t->nz - 1) / g->mdiv + 1 > 1025) g->mdiv *= 2;
  g->mw = (t->nx - 1) / g->mdiv + 1;
  g->mh = (t->nz - 1) / g->mdiv + 1;
  g->macro = data_tex(g->mw, g->mh, true, true);
  g->layers = data_tex(TR_ATLAS_W, TR_ATLAS_H, true, false);
  for (int f = 0; f < 2; f++)
    for (int h = 0; h < 2; h++) make_mesh(t, f, h, 256);
}

void cw__terrain_gpu_free(cw_terrain *t) {
  tr_gpu *g = t->gpu;
  if (!g) return;
  for (int f = 0; f < 2; f++)
    for (int h = 0; h < 2; h++)
      if (g->im[f][h]) {
        unreg_obj(g->im[f][h]);
        if (g->scene) t3_object_remove(&g->scene->base, g->im[f][h]);
        t3_release(g->im[f][h]);
      }
  t3_texture *tx[] = { g->heights, g->control, g->macro, g->layers };
  for (int i = 0; i < (int)CW_ARRAY_LEN(tx); i++)
    if (tx[i]) t3_release(tx[i]);
  if (g->grid[0]) t3_release(g->grid[0]);
  if (g->grid[1]) t3_release(g->grid[1]);
  if (g->mat_near) t3_release(g->mat_near);
  if (g->mat_far) t3_release(g->mat_far);
  cw_free(g);
  t->gpu = NULL;
}

void cw_terrain_add_to_scene(cw_terrain *t, t3_scene *scene) {
  if (!t) return;
  ensure_gpu(t);
  tr_gpu *g = t->gpu;
  if (g->scene == scene) return;
  for (int f = 0; f < 2; f++)
    for (int h = 0; h < 2; h++) {
      if (g->scene) t3_object_remove(&g->scene->base, g->im[f][h]);
      if (scene) t3_object_add(&scene->base, g->im[f][h]);
    }
  g->scene = scene;
  for (int i = 0; i < TR_MAX_SCATTER; i++)
    if (t->scatter[i]) cw__scatter_attach(t, t->scatter[i]);
}

t3_scene *cw__terrain_scene(cw_terrain *t) { return t->gpu ? ((tr_gpu *)t->gpu)->scene : NULL; }

t3_material *cw_terrain_material(cw_terrain *t) {
  if (!t) return NULL;
  ensure_gpu(t);
  return ((tr_gpu *)t->gpu)->mat_near;
}

void cw__terrain_gpu_set_origin(cw_terrain *t) { (void)t; /* the uniforms carry the origin */ }

/* ── per frame ──────────────────────────────────────────────────────── */

static void rect_parallel(cw_terrain *t, const int *r, cw_range_fn fn) {
  rect_job j = { t, r[0], r[1], r[2], r[3] };
  cw_parallel_for(r[3] - r[1] + 1, 16, fn, &j);
}

void cw__terrain_gpu_update(cw_terrain *t) {
  double t0 = tr_now_ms();
  ensure_gpu(t);
  tr_gpu *g = t->gpu;
  if (t->dirty_h[0] <= t->dirty_h[2]) {
    rect_parallel(t, t->dirty_h, heights_rows);
    /* normals one sample beyond the edit (central differences) */
    int r[4] = { CW_MAX(t->dirty_h[0] - 1, 0), CW_MAX(t->dirty_h[1] - 1, 0), CW_MIN(t->dirty_h[2] + 1, t->nx - 1),
                 CW_MIN(t->dirty_h[3] + 1, t->nz - 1) };
    rect_parallel(t, r, normal_rows);
    g->heights->version++;
    t->dirty_h[0] = 1, t->dirty_h[2] = 0;
    t->normals_dirty = false;
  }
  if (t->dirty_c[0] <= t->dirty_c[2]) {
    rect_parallel(t, t->dirty_c, control_rows);
    g->control->version++;
    t->dirty_c[0] = 1, t->dirty_c[2] = 0;
  }
  if (t->macro_dirty) {
    cw_parallel_for(g->mh, 8, macro_rows, t);
    g->macro->version++;
    t->macro_dirty = false;
  }
  if (t->layers_dirty) {
    build_atlas(t, g->layers, false);
    build_atlas(t, g->layers, true);
    t->layers_dirty = false;
  }
  int q = cw__terrain_quality(t);
  if (q != g->near_q) {
    g->near_q = q;
    t3_material_set_extension(g->mat_near, near_ext(q));
  }
  t->stats.upload_ms = (float)(tr_now_ms() - t0);
}

/* the selected patches into the four meshes' instances */
void cw__terrain_gpu_draw_setup(cw_terrain *t) {
  tr_gpu *g = t->gpu;
  int need[2][2] = { { 0, 0 }, { 0, 0 } };
  for (int i = 0; i < t->patches.count; i++) need[t->patches.data[i].far][t->patches.data[i].half]++;
  for (int f = 0; f < 2; f++)
    for (int h = 0; h < 2; h++)
      if (need[f][h] > g->cap[f][h]) {
        int c = g->cap[f][h];
        while (c < need[f][h]) c *= 2;
        make_mesh(t, f, h, c);
      }
  int fill[2][2] = { { 0, 0 }, { 0, 0 } };
  float sp = t->def.vertex_spacing;
  for (int i = 0; i < t->patches.count; i++) {
    const tr_patch *p = &t->patches.data[i];
    t3_instanced_mesh *im = g->im[p->far][p->half];
    float *m = t3_attribute_f32(im->instance_matrix) + 16 * fill[p->far][p->half]++;
    float s = (float)p->size * sp;
    /* column major: x scaled to the patch, Y column = the level, z scaled, translation to its corner */
    m[0] = s, m[1] = 0, m[2] = 0, m[3] = 0;
    m[4] = 0, m[5] = (float)p->lod, m[6] = 0, m[7] = 0;
    m[8] = 0, m[9] = 0, m[10] = s, m[11] = 0;
    m[12] = t->def.origin.x + (float)p->x * sp, m[13] = 0, m[14] = t->def.origin.z + (float)p->z * sp, m[15] = 1;
  }
  for (int f = 0; f < 2; f++)
    for (int h = 0; h < 2; h++) {
      t3_instanced_mesh *im = g->im[f][h];
      im->count = fill[f][h];
      im->mesh.base.visible = fill[f][h] > 0;
      if (fill[f][h]) {
        im->instance_matrix->version++;
        t3_attribute_add_update_range(im->instance_matrix, 0, fill[f][h]);
      }
    }
}

/* ── the extensions' fill hook ─────────────────────────────────────── */

static void fill(t3_extension_ctx *c, void *user) {
  (void)user;
  cw_terrain *t = terrain_of_obj(t3_extension_object(c));
  if (!t) t = user; /* (a renderer bound to one terrain) */
  if (!t || !t->gpu) return;
  tr_gpu *g = t->gpu;
  const cw_terrain_def *d = &t->def;
  int q = cw__terrain_quality(t);
  float far = cw__terrain_far(t), fade = CW_MIN(d->far_fade, far * 0.5f);
  float v[4];
  v[0] = t->camera.x, v[1] = t->camera.y, v[2] = t->camera.z, v[3] = 0;
  t3_extension_set_uniform(c, "terrainCam", v, 4);
  v[0] = d->origin.x, v[1] = d->origin.z, v[2] = 1.0f / d->vertex_spacing, v[3] = d->vertex_spacing;
  t3_extension_set_uniform(c, "terrainGrid", v, 4);
  v[0] = d->origin.y + d->min_height, v[1] = t->sy, v[2] = (float)(t->nx - 1), v[3] = (float)(t->nz - 1);
  t3_extension_set_uniform(c, "terrainHeight", v, 4);
  /* morphing ends a little inside each range: the selection's ranges and the shader's exp2 agree to rounding */
  v[0] = t->range[0], v[1] = log2f(d->lod_ratio), v[2] = d->morph_start, v[3] = 0.985f;
  t3_extension_set_uniform(c, "terrainLod", v, 4);
  v[0] = far - fade, v[1] = 1.0f / fade, v[2] = 0.12f, v[3] = q >= CW_QUALITY_HIGH ? 2.0f : q == CW_QUALITY_MEDIUM ? 1.0f : 0.0f;
  t3_extension_set_uniform(c, "terrainFar", v, 4);
  v[0] = TR_ATLAS_COLS, v[1] = TR_ATLAS_COLS, v[2] = (float)TR_TILE / TR_CELL, v[3] = (float)TR_GUTTER / TR_CELL;
  t3_extension_set_uniform(c, "terrainAtlas", v, 4);
  for (int k = 0; k < 4; k++) v[k] = t->layer[k].uv_scale;
  t3_extension_set_uniform(c, "terrainUv0", v, 4);
  for (int k = 0; k < 4; k++) v[k] = t->layer[4 + k].uv_scale;
  t3_extension_set_uniform(c, "terrainUv1", v, 4);
  /* triplanar from normal.y 0.75 down over 0.2; gradients kept within the gutter's mip (cell units) */
  v[0] = 0.75f, v[1] = 0.2f, v[2] = (float)TR_GUTTER / TR_CELL, v[3] = 0.15f;
  t3_extension_set_uniform(c, "terrainSlope", v, 4);
  t3_extension_set_texture(c, "terrainHeights", g->heights);
  t3_extension_set_texture(c, "terrainMacro", g->macro);
  t3_extension_set_texture(c, "terrainControl", g->control);
  t3_extension_set_texture(c, "terrainLayers", g->layers);
}

void cw_terrain_bind_renderer(cw_terrain *t, t3_renderer *r) {
  if (!r) return;
  static const char *names[] = { "terrain", "terrainlow", "terrainfar" };
  for (int i = 0; i < 3; i++) t3_renderer_set_extension(r, names[i], fill, t);
}

void cw_terrain_update(cw_terrain *t, t3_vec3 cam, const t3_mat4 *vp) {
  if (!t) return;
  t->camera = cam;
  for (int i = 0; i < t->rnx * t->rnz; i++)
    if (t->regions[i].dirty) {
      cw_terrain_sync_physics(t);
      break;
    }
  /* quality: Ultra reaches farther with each level */
  float rs = cw__terrain_quality(t) == CW_QUALITY_ULTRA ? 1.25f : 1.0f;
  if (rs != t->range_scale) {
    t->range_scale = rs;
    cw__terrain_ranges(t);
  }
  cw__terrain_gpu_update(t);
  cw__terrain_select(t, cam, vp);
  cw__terrain_gpu_draw_setup(t);
  cw__scatter_update(t, cam, vp);
}
