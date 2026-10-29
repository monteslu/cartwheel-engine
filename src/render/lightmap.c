/* The lightmapper (cartwheel/lightmap.h): gathering, texel rasterization,
 * direct light, bounces, dilation, the SH probe grid, serialization, and
 * applying a bake to a tree. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/environment.h"
#include "cartwheel/jobs.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "environment_internal.h"
#include "bake_internal.h"
#include "lightmap_internal.h"
#if CW_WGPU
#include "fx/fx_wgpu.h"
#endif

#define MAGIC 0x4d4c5743u /* "CWLM" */
#define VERSION 3u /* 3: the reflection cube; 2 still loads */

cw_lightmap_settings cw_lightmap_settings_default(void) {
  return (cw_lightmap_settings){ .texel_density = 8, .min_size = 8, .max_size = 1024, .padding = 2, .samples = 64,
                                 .bounces = 2, .seed = 1, .probe_spacing = 2, .probe_samples = 128, .intensity = 1,
                                 .denoise = 2, .direct_samples = 1, .shadow_softness = 0, .reflection_size = 0,
                                 .reflection_origin = { 0, 1.5f, 0 } };
}

/* ── the data ───────────────────────────────────────────────────────── */

typedef struct lm_rec {
  char path[256];
  int tri_count;
  uint64_t sig;
  int w, h;
  float *uv2;               /* tri_count * 6 */
  uint16_t *texels;         /* w * h * 4 halves */
} lm_rec;

struct cw_lightmap_data {
  CW_ARRAY(lm_rec) recs;
  bool has_sh;
  int nx, ny, nz;
  float lo[3], hi[3];
  float *sh;                /* nx * ny * nz * 27 */
  int cube_size;            /* the reflection cube's face side (0 = none) */
  uint16_t *cube;           /* 6 faces x cube_size^2 x rgba halves, three.js's face order (+x -x +y -y +z -z) */
  t3_texture *cube_tex;     /* made on first use (cw_lightmap_reflection) */
  float cube_scale;
  /* cw_lightmap_irradiance_below: the applied meshes' upward-facing triangles, world space, in an XZ grid */
  struct below_tri { float p[9]; float uv[6]; int rec; } *btris;
  int nbtris;
  float glo[2], gcell;
  int gw, gh;
  int *gstart, *gidx;       /* gw * gh + 1 offsets, then triangle indices */
  uint8_t *bytes;
  size_t len;
  cw_lightmap_info info;
};

static void data_free_recs(cw_lightmap_data *d) {
  for (int i = 0; i < d->recs.count; i++) {
    cw_free(d->recs.data[i].uv2);
    cw_free(d->recs.data[i].texels);
  }
  cw_array_free(&d->recs);
}

void cw_lightmap_free(cw_lightmap_data *d) {
  if (!d) return;
  data_free_recs(d);
  cw_free(d->sh);
  cw_free(d->cube);
  if (d->cube_tex) t3_release(d->cube_tex);
  cw_free(d->btris);
  cw_free(d->gstart);
  cw_free(d->gidx);
  cw_free(d->bytes);
  cw_free(d);
}

const void *cw_lightmap_bytes(const cw_lightmap_data *d, size_t *len) {
  *len = d->len;
  return d->bytes;
}

static uint64_t fnv64(const void *p, size_t n) {
  const uint8_t *b = p;
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

uint64_t cw_lightmap_hash(const cw_lightmap_data *d) { return d ? fnv64(d->bytes, d->len) : 0; }
void cw_lightmap_get_info(const cw_lightmap_data *d, cw_lightmap_info *out) { *out = d->info; }

typedef struct wbuf { uint8_t *p; size_t n, cap; } wbuf;
static void put(wbuf *w, const void *src, size_t n) {
  if (w->n + n > w->cap) {
    w->cap = (w->n + n) * 2 + 1024;
    w->p = cw_realloc(w->p, w->cap, CW_MEM_RENDER);
  }
  memcpy(w->p + w->n, src, n);
  w->n += n;
}
static void put_u32(wbuf *w, uint32_t v) {
  uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
  put(w, b, 4);
}
static void put_u64(wbuf *w, uint64_t v) {
  put_u32(w, (uint32_t)v);
  put_u32(w, (uint32_t)(v >> 32));
}
static void put_f32(wbuf *w, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  put_u32(w, u);
}

/* the bytes: every host writes the same little-endian layout */
static void serialize(cw_lightmap_data *d) {
  wbuf w = { 0 };
  put_u32(&w, MAGIC);
  put_u32(&w, VERSION);
  put_u32(&w, (uint32_t)d->recs.count);
  for (int i = 0; i < d->recs.count; i++) {
    const lm_rec *r = &d->recs.data[i];
    uint32_t pl = (uint32_t)strlen(r->path);
    put_u32(&w, pl);
    put(&w, r->path, pl);
    put_u32(&w, (uint32_t)r->tri_count);
    put_u64(&w, r->sig);
    put_u32(&w, (uint32_t)r->w);
    put_u32(&w, (uint32_t)r->h);
    for (int k = 0; k < r->tri_count * 6; k++) put_f32(&w, r->uv2[k]);
    for (int k = 0; k < r->w * r->h * 4; k++) {
      uint8_t b[2] = { (uint8_t)r->texels[k], (uint8_t)(r->texels[k] >> 8) };
      put(&w, b, 2);
    }
  }
  put_u32(&w, d->has_sh ? 1 : 0);
  if (d->has_sh) {
    put_u32(&w, (uint32_t)d->nx);
    put_u32(&w, (uint32_t)d->ny);
    put_u32(&w, (uint32_t)d->nz);
    for (int a = 0; a < 3; a++) put_f32(&w, d->lo[a]);
    for (int a = 0; a < 3; a++) put_f32(&w, d->hi[a]);
    for (int k = 0; k < d->nx * d->ny * d->nz * 27; k++) put_f32(&w, d->sh[k]);
  }
  put_u32(&w, (uint32_t)d->cube_size);
  for (int k = 0; k < 6 * d->cube_size * d->cube_size * 4; k++) {
    uint8_t b[2] = { (uint8_t)d->cube[k], (uint8_t)(d->cube[k] >> 8) };
    put(&w, b, 2);
  }
  cw_free(d->bytes);
  d->bytes = w.p;
  d->len = w.n;
  d->info.hash = fnv64(d->bytes, d->len);
}

typedef struct rbuf { const uint8_t *p; size_t n, at; bool bad; } rbuf;
static bool get(rbuf *r, void *dst, size_t n) {
  if (r->bad || r->at + n > r->n) { r->bad = true; return false; }
  memcpy(dst, r->p + r->at, n);
  r->at += n;
  return true;
}
static uint32_t get_u32(rbuf *r) {
  uint8_t b[4] = { 0 };
  get(r, b, 4);
  return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static float get_f32(rbuf *r) {
  uint32_t u = get_u32(r);
  float f;
  memcpy(&f, &u, 4);
  return f;
}

cw_lightmap_data *cw_lightmap_parse(const void *bytes, size_t len) {
  rbuf r = { bytes, len, 0, false };
  uint32_t magic = get_u32(&r), version = get_u32(&r);
  if (magic != MAGIC || (version != VERSION && version != 2u)) {
    cw_error("lightmap: not a .cwlm file (or another version)");
    return NULL;
  }
  cw_lightmap_data *d = cw_calloc(1, sizeof *d, CW_MEM_RENDER);
  uint32_t n = get_u32(&r);
  for (uint32_t i = 0; i < n && !r.bad; i++) {
    lm_rec rec;
    memset(&rec, 0, sizeof rec);
    uint32_t pl = get_u32(&r);
    if (pl >= sizeof rec.path) { r.bad = true; break; }
    get(&r, rec.path, pl);
    rec.tri_count = (int)get_u32(&r);
    rec.sig = (uint64_t)get_u32(&r);
    rec.sig |= (uint64_t)get_u32(&r) << 32;
    rec.w = (int)get_u32(&r);
    rec.h = (int)get_u32(&r);
    if (rec.tri_count < 0 || rec.tri_count > (1 << 24) || rec.w <= 0 || rec.h <= 0 || rec.w > 16384 || rec.h > 16384) {
      r.bad = true;
      break;
    }
    rec.uv2 = cw_alloc((size_t)rec.tri_count * 6 * sizeof(float) + 4, CW_MEM_RENDER);
    for (int k = 0; k < rec.tri_count * 6; k++) rec.uv2[k] = get_f32(&r);
    rec.texels = cw_alloc((size_t)rec.w * rec.h * 4 * sizeof(uint16_t), CW_MEM_RENDER);
    for (int k = 0; k < rec.w * rec.h * 4; k++) {
      uint8_t b[2] = { 0 };
      get(&r, b, 2);
      rec.texels[k] = (uint16_t)(b[0] | b[1] << 8);
    }
    cw_array_push(&d->recs, rec);
  }
  if (!r.bad && get_u32(&r)) {
    d->has_sh = true;
    d->nx = (int)get_u32(&r);
    d->ny = (int)get_u32(&r);
    d->nz = (int)get_u32(&r);
    for (int a = 0; a < 3; a++) d->lo[a] = get_f32(&r);
    for (int a = 0; a < 3; a++) d->hi[a] = get_f32(&r);
    if (d->nx <= 0 || d->ny <= 0 || d->nz <= 0 || d->nx > 256 || d->ny > 256 || d->nz > 256) r.bad = true;
    else {
      size_t k = (size_t)d->nx * d->ny * d->nz * 27;
      d->sh = cw_alloc(k * sizeof(float), CW_MEM_RENDER);
      for (size_t i = 0; i < k; i++) d->sh[i] = get_f32(&r);
    }
  }
  if (!r.bad && version >= 3u) {
    int cs = (int)get_u32(&r);
    if (cs < 0 || cs > 1024) r.bad = true;
    else if (cs > 0) {
      d->cube_size = cs;
      size_t k = (size_t)6 * cs * cs * 4;
      d->cube = cw_alloc(k * sizeof(uint16_t), CW_MEM_RENDER);
      for (size_t i = 0; i < k; i++) {
        uint8_t b[2] = { 0 };
        get(&r, b, 2);
        d->cube[i] = (uint16_t)(b[0] | b[1] << 8);
      }
    }
  }
  if (r.bad) {
    cw_error("lightmap: truncated or corrupt .cwlm data");
    cw_lightmap_free(d);
    return NULL;
  }
  d->bytes = cw_alloc(len, CW_MEM_RENDER);
  memcpy(d->bytes, bytes, len);
  d->len = len;
  d->info.meshes = d->recs.count;
  for (int i = 0; i < d->recs.count; i++) {
    d->info.triangles += d->recs.data[i].tri_count;
    d->info.texels += d->recs.data[i].w * d->recs.data[i].h;
    d->info.width_total += d->recs.data[i].w;
  }
  d->info.probes = d->has_sh ? d->nx * d->ny * d->nz : 0;
  d->info.hash = fnv64(d->bytes, d->len);
  return d;
}

cw_lightmap_data *cw_lightmap_load(const char *asset_path) {
  size_t len = 0;
  void *b = cw_asset_load(asset_path, &len);
  if (!b) {
    cw_error("lightmap: asset '%s' not found", asset_path);
    return NULL;
  }
  cw_lightmap_data *d = cw_lightmap_parse(b, len);
  free(b);
  return d;
}

bool cw_lightmap_save(const cw_lightmap_data *d, const char *path) {
#if CW_CART
  (void)d;
  cw_error("cw_lightmap_save: carts cannot write files (%s); bake natively", path);
  return false;
#else
  FILE *f = fopen(path, "wb");
  if (!f) {
    cw_error("cw_lightmap_save: cannot write %s", path);
    return false;
  }
  bool ok = fwrite(d->bytes, 1, d->len, f) == d->len;
  ok = fclose(f) == 0 && ok;
  if (!ok) cw_error("cw_lightmap_save: writing %s failed", path);
  return ok;
#endif
}

/* ── baking: the scene ──────────────────────────────────────────────── */

typedef struct lm_mesh {
  cw_node node;
  char path[256];
  int tri_count;
  float *pos, *nrm;         /* world, 9 floats a triangle */
  uint64_t sig;
  lm_unwrap uw;
  t3_color albedo, emissive;
  int w, h;
  int *texel_tri;           /* covering triangle, -1 = gap */
  uint8_t *inside;          /* the texel centre sits inside other geometry (its rays mostly hit back faces) */
  float *tp, *tn;           /* texel world position, normal */
  float *ed, *ei, *b;       /* direct irradiance, indirect irradiance, outgoing radiance (rgb) */
  float *ebd;               /* the direct irradiance of the bake_direct lights (kept in the lightmap) */
  float *ei_next;
} lm_mesh;

typedef struct sky_table {
  bool procedural;
  t3_color ambient, sky, ground; /* WorldEnvironment (radiance) */
  float *table;             /* 64 x 32 x rgb, lat-long (procedural) */
} sky_table;

typedef struct baker {
  cw_tree *tree;
  const cw_lightmap_settings *s;
  CW_ARRAY(lm_mesh) meshes;
  CW_ARRAY(cw__light) lights;
  bool any_direct;          /* a light with bake_direct */
  /* instanced STATIC meshes: occluders (and bouncers) with no lightmap, one radiance per triangle */
  CW_ARRAY(float) occ_pos;  /* 9 floats a triangle, world */
  CW_ARRAY(float) occ_alb;  /* 3 a triangle: albedo (material x vertex x instance colour) */
  CW_ARRAY(float) occ_emi;  /* 3 a triangle */
  float *occ_rad;           /* 3 a triangle: outgoing radiance */
  int occ_count;
  lm_bvh bvh;
  sky_table sky;
  float eps;
  int iteration;
  long long rays;           /* summed per job item, reduced in order */
  long long *row_rays;
} baker;

static void mat_apply(const t3_mat4 *m, const float *p, float *o) {
  const float *e = m->e;
  float x = p[0], y = p[1], z = p[2];
  o[0] = e[0] * x + e[4] * y + e[8] * z + e[12];
  o[1] = e[1] * x + e[5] * y + e[9] * z + e[13];
  o[2] = e[2] * x + e[6] * y + e[10] * z + e[14];
}
static void nrm_apply(const t3_mat3 *m, const float *n, float *o) {
  const float *e = m->e;
  float x = n[0], y = n[1], z = n[2];
  o[0] = e[0] * x + e[3] * y + e[6] * z;
  o[1] = e[1] * x + e[4] * y + e[7] * z;
  o[2] = e[2] * x + e[5] * y + e[8] * z;
  float l = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
  if (l > 0) for (int i = 0; i < 3; i++) o[i] /= l;
}

/* the geometry's local positions, non-indexed: what a bake is matched by */
static uint64_t geometry_sig(const t3_geometry *g) {
  t3_geometry *ni = t3_geometry_to_non_indexed(g);
  const t3_attribute *a = ni->attributes[T3_ATTR_POSITION];
  uint64_t h = a ? fnv64(a->array, (size_t)a->count * 3 * sizeof(float)) ^ (uint64_t)a->count : 0;
  t3_release(ni);
  return h;
}

typedef struct gather_ctx { baker *b; cw_node root; } gather_ctx;

/* an instanced STATIC mesh: every instance's triangles occlude and bounce light in the bake (its instances share
 * one geometry, so it gets no lightmap of its own and stays lit in real time). Albedo per triangle: the
 * material's colour x the triangle's mean vertex colour (when the material uses them) x the instance's colour. */
static void gather_instanced(baker *b, const cw__mesh_bake *mb) {
  t3_instanced_mesh *im = (t3_instanced_mesh *)mb->mesh;
  t3_mesh *mesh = mb->mesh;
  if (!mesh->geometry || !im->instance_matrix || im->count <= 0) return;
  t3_geometry *ni = t3_geometry_to_non_indexed(mesh->geometry);
  const t3_attribute *pa = ni->attributes[T3_ATTR_POSITION], *ca = ni->attributes[T3_ATTR_COLOR];
  if (!pa || pa->type != T3_FLOAT32 || pa->count < 3) {
    t3_release(ni);
    return;
  }
  const t3_material *mat = mesh->material_count ? mesh->materials[0] : NULL;
  bool vcol = mat && mat->vertex_colors && ca && ca->type == T3_FLOAT32 && ca->item_size >= 3;
  t3_color mc = mat ? mat->color : (t3_color){ 1, 1, 1 };
  float ek = mat ? mat->emissive_intensity : 0;
  t3_color me = mat ? (t3_color){ mat->emissive.r * ek, mat->emissive.g * ek, mat->emissive.b * ek } : (t3_color){ 0, 0, 0 };
  const float *P = pa->array, *C = vcol ? ca->array : NULL;
  const float *IM = im->instance_matrix->array;
  const float *IC = im->instance_color ? im->instance_color->array : NULL;
  int tris = pa->count / 3, cs = vcol ? ca->item_size : 0, n = CW_MIN(im->count, im->instance_matrix->count);
  for (int i = 0; i < n; i++) {
    t3_mat4 inst, w;
    memcpy(inst.e, IM + (size_t)i * 16, sizeof inst.e);
    if (inst.e[15] == 0) continue; /* a hidden (zero) instance */
    t3_mat4_multiply(&w, &mesh->base.matrix_world, &inst);
    float ic[3] = { 1, 1, 1 };
    if (IC) for (int a = 0; a < 3; a++) ic[a] = IC[i * 3 + a];
    for (int t = 0; t < tris; t++) {
      for (int k = 0; k < 3; k++) {
        float p[3];
        mat_apply(&w, P + (t * 3 + k) * 3, p);
        for (int a = 0; a < 3; a++) cw_array_push(&b->occ_pos, p[a]);
      }
      float al[3] = { mc.r, mc.g, mc.b };
      if (C)
        for (int a = 0; a < 3; a++) al[a] *= (C[(t * 3) * cs + a] + C[(t * 3 + 1) * cs + a] + C[(t * 3 + 2) * cs + a]) / 3.0f;
      for (int a = 0; a < 3; a++) cw_array_push(&b->occ_alb, CW_CLAMP(al[a] * ic[a], 0.0f, 1.0f));
      cw_array_push(&b->occ_emi, me.r);
      cw_array_push(&b->occ_emi, me.g);
      cw_array_push(&b->occ_emi, me.b);
      b->occ_count++;
    }
  }
  t3_release(ni);
}

static bool gather_node(cw_node n, void *user) {
  gather_ctx *g = user;
  baker *b = g->b;
  cw__light l;
  if (cw__light3d_bake_info(n, &l)) {
    cw_array_push(&b->lights, l);
    b->any_direct |= l.direct;
  }
  cw__mesh_bake mb;
  if (!cw__mesh3d_bake_info(n, &mb) || !mb.is_static) return true;
  if (mb.instanced) {
    gather_instanced(b, &mb);
    return true;
  }
  t3_mesh *mesh = mb.mesh;
  t3_geometry *ni = t3_geometry_to_non_indexed(mesh->geometry);
  const t3_attribute *pa = ni->attributes[T3_ATTR_POSITION], *na = ni->attributes[T3_ATTR_NORMAL];
  if (!pa || pa->type != T3_FLOAT32 || pa->count < 3) {
    t3_release(ni);
    return true;
  }
  lm_mesh m;
  memset(&m, 0, sizeof m);
  m.node = n;
  cw_node_path(n, g->root, m.path, sizeof m.path);
  m.tri_count = pa->count / 3;
  m.sig = geometry_sig(mesh->geometry);
  m.albedo = mb.albedo;
  m.emissive = mb.emissive;
  m.pos = cw_alloc((size_t)m.tri_count * 9 * sizeof(float), CW_MEM_RENDER);
  m.nrm = cw_alloc((size_t)m.tri_count * 9 * sizeof(float), CW_MEM_RENDER);
  const t3_mat4 *w = &mesh->base.matrix_world;
  t3_mat3 nm;
  t3_mat3_normal_matrix(&nm, w);
  const float *P = pa->array, *N = na && na->type == T3_FLOAT32 ? na->array : NULL;
  for (int t = 0; t < m.tri_count; t++) {
    for (int k = 0; k < 3; k++) mat_apply(w, P + (t * 3 + k) * 3, m.pos + t * 9 + k * 3);
    float fnr[3];
    {
      const float *p = m.pos + t * 9;
      float e1[3] = { p[3] - p[0], p[4] - p[1], p[5] - p[2] }, e2[3] = { p[6] - p[0], p[7] - p[1], p[8] - p[2] };
      fnr[0] = e1[1] * e2[2] - e1[2] * e2[1];
      fnr[1] = e1[2] * e2[0] - e1[0] * e2[2];
      fnr[2] = e1[0] * e2[1] - e1[1] * e2[0];
      float l = sqrtf(fnr[0] * fnr[0] + fnr[1] * fnr[1] + fnr[2] * fnr[2]);
      if (l > 0) for (int a = 0; a < 3; a++) fnr[a] /= l;
    }
    for (int k = 0; k < 3; k++) {
      if (N) nrm_apply(&nm, N + (t * 3 + k) * 3, m.nrm + t * 9 + k * 3);
      else memcpy(m.nrm + t * 9 + k * 3, fnr, sizeof fnr);
    }
  }
  const t3_attribute *ua = ni->attributes[T3_ATTR_UV];
  if (mb.uv0 && ua && ua->type == T3_FLOAT32 && ua->count == pa->count) {
    lm_unwrap_from_uv(&m.uw, ua->array, m.pos, m.tri_count, b->s->texel_density * mb.scale, CW_MAX(b->s->min_size, 4),
                      CW_MAX(b->s->max_size, 8));
  } else {
    if (mb.uv0) cw_warn("lightmap: '%s' asks for lightmap_uv UV0 but has no uv0: generated charts instead", m.path);
    lm_unwrap_mesh(&m.uw, m.pos, m.nrm, m.tri_count, b->s->texel_density * mb.scale, CW_MAX(b->s->padding, 1),
                   CW_MAX(b->s->min_size, 4), CW_MAX(b->s->max_size, 8));
  }
  t3_release(ni);
  m.w = m.uw.width;
  m.h = m.uw.height;
  cw_array_push(&b->meshes, m);
  return true;
}

/* ── the sky ────────────────────────────────────────────────────────── */

static void sky_build(baker *b) {
  sky_table *s = &b->sky;
  t3_vec3 sun;
  float ray, mie, g, e0;
  t3_color night;
  if (cw__sky_params(b->tree, &sun, &ray, &mie, &g, &e0, &night)) {
    s->procedural = true;
    s->table = cw_alloc(64 * 32 * 3 * sizeof(float), CW_MEM_RENDER);
    for (int j = 0; j < 32; j++)
      for (int i = 0; i < 64; i++) {
        float y = -1 + 2 * ((float)j + 0.5f) / 32, r = sqrtf(CW_MAX(1 - y * y, 0.0f));
        float ph = CW_TAU * ((float)i + 0.5f) / 64, sp, cp;
        cw_sincos(ph, &sp, &cp);
        t3_vec3 d = { r * cp, CW_MAX(y, 0.0f), r * sp };
        d = t3_vec3_normalize(d);
        t3_color c = cw_sky_radiance(d, sun, ray, mie, g);
        float *o = s->table + (j * 64 + i) * 3;
        o[0] = c.r * e0 + night.r;
        o[1] = c.g * e0 + night.g;
        o[2] = c.b * e0 + night.b;
        if (y < 0) {
          /* below the horizon: the haze at the horizon dims to a quarter */
          float k = 1 + 3 * y / 4;
          o[0] *= k; o[1] *= k; o[2] *= k;
        }
      }
  } else {
    cw__world_env_bake(b->tree, &s->ambient, &s->sky, &s->ground);
  }
}

/* sky radiance in true irradiance units (E = PI x L for uniform L) */
static void sky_radiance(const baker *b, const float d[3], float out[3]) {
  const sky_table *s = &b->sky;
  if (s->procedural) {
    int j = (int)floorf((d[1] * 0.5f + 0.5f) * 32);
    float ph = cw_atan2(d[2], d[0]);
    if (ph < 0) ph += CW_TAU;
    int i = (int)floorf(ph / CW_TAU * 64);
    j = CW_CLAMP(j, 0, 31);
    i = CW_CLAMP(i, 0, 63);
    const float *t = s->table + (j * 64 + i) * 3;
    out[0] = t[0];
    out[1] = t[1];
    out[2] = t[2];
    return;
  }
  /* the ambient + hemisphere light as radiance: irradiance A from a
   * uniform sky is PI x L */
  const t3_color *h = d[1] >= 0 ? &s->sky : &s->ground;
  float k = 1.0f / CW_PI;
  out[0] = (s->ambient.r + h->r) * k;
  out[1] = (s->ambient.g + h->g) * k;
  out[2] = (s->ambient.b + h->b) * k;
}

/* ── rays ───────────────────────────────────────────────────────────── */

static void onb(const float n[3], float t[3], float bt[3]) {
  float sign = copysignf(1.0f, n[2]);
  float a = -1.0f / (sign + n[2]), c = n[0] * n[1] * a;
  t[0] = 1 + sign * n[0] * n[0] * a;
  t[1] = sign * c;
  t[2] = -sign * n[0];
  bt[0] = c;
  bt[1] = sign + n[1] * n[1] * a;
  bt[2] = -n[1];
}

/* three.js's getDistanceAttenuation: inverse power, windowed to 0 at the cutoff */
static float falloff(float d, float cutoff, float decay) {
  float f = 1.0f / CW_MAX(cw_pow(d, decay), 0.01f);
  if (cutoff > 0) {
    float q = d / cutoff, w = 1 - q * q * q * q;
    w = w < 0 ? 0 : w > 1 ? 1 : w;
    f *= w * w;
  }
  return f;
}

/* the share of `samples` shadow rays from o toward a light that reach it: one ray at the light's centre, or
 * (shadow_softness > 0) rays to points of a disc round it whose radius is softness x the distance (a
 * directional light: softness radians), jittered per texel by rng */
static float visibility(const baker *b, const float o[3], const float L[3], float dist, cw_rng *rng, long long *rays) {
  int S = rng ? CW_MAX(b->s->direct_samples, 1) : 1;
  float soft = b->s->shadow_softness;
  if (S == 1 || soft <= 0) {
    (*rays)++;
    return lm_bvh_any(&b->bvh, o, L, 0, dist - b->eps) ? 0.0f : 1.0f;
  }
  float t[3], bt[3];
  onb(L, t, bt);
  int lit = 0;
  for (int i = 0; i < S; i++) {
    /* a point of the unit disc (concentric: stratified by i) */
    float r = sqrtf(((float)i + cw_rng_float(rng)) / (float)S) * soft, a = CW_TAU * cw_rng_float(rng), sa, ca;
    cw_sincos(a, &sa, &ca);
    float d[3];
    for (int k = 0; k < 3; k++) d[k] = L[k] + (t[k] * ca + bt[k] * sa) * r;
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int k = 0; k < 3; k++) d[k] /= l;
    (*rays)++;
    lit += !lm_bvh_any(&b->bvh, o, d, 0, dist * l - b->eps);
  }
  return (float)lit / (float)S;
}

/* direct irradiance at p (normal n) from the bake lights, shadow tested: out from every one, out_direct (when
 * not NULL) from the bake_direct ones only */
static void direct_at(const baker *b, const float p[3], const float n[3], float out[3], float *out_direct, cw_rng *rng,
                      long long *rays) {
  out[0] = out[1] = out[2] = 0;
  if (out_direct) out_direct[0] = out_direct[1] = out_direct[2] = 0;
  float o[3] = { p[0] + n[0] * b->eps, p[1] + n[1] * b->eps, p[2] + n[2] * b->eps };
  for (int i = 0; i < b->lights.count; i++) {
    const cw__light *l = &b->lights.data[i];
    float L[3], dist = 1e30f, f = 1;
    if (l->kind == 0) {
      L[0] = -l->dir.x;
      L[1] = -l->dir.y;
      L[2] = -l->dir.z;
    } else {
      float v[3] = { l->pos.x - p[0], l->pos.y - p[1], l->pos.z - p[2] };
      dist = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      if (dist <= 1e-6f) continue;
      if (l->range > 0 && dist >= l->range) continue;
      for (int a = 0; a < 3; a++) L[a] = v[a] / dist;
      f = falloff(dist, l->range, l->decay);
      if (l->kind == 2) {
        float cone = cw_cos(l->angle), pen = cw_cos(l->angle * (1 - l->penumbra));
        float ac = -(L[0] * l->dir.x + L[1] * l->dir.y + L[2] * l->dir.z);
        if (ac <= cone) continue;
        f *= cw_smoothstep(cone, pen, ac);
      }
    }
    float ndl = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
    if (ndl <= 0 || f <= 0) continue;
    float vis = visibility(b, o, L, dist, rng, rays);
    if (vis <= 0) continue;
    float k = f * ndl * vis;
    out[0] += l->color.r * k;
    out[1] += l->color.g * k;
    out[2] += l->color.b * k;
    if (out_direct && l->direct) {
      out_direct[0] += l->color.r * k;
      out_direct[1] += l->color.g * k;
      out_direct[2] += l->color.b * k;
    }
  }
}

/* the radiance a ray brings back: the surface it hits (its outgoing
 * radiance this bounce), or the sky; *back = it hit a back face */
static void trace(const baker *b, const float o[3], const float d[3], float out[3], bool *back) {
  lm_hit h;
  *back = false;
  if (!lm_bvh_closest(&b->bvh, o, d, 0, 1e30f, &h)) {
    sky_radiance(b, d, out);
    return;
  }
  if (h.back) {
    *back = true;
    out[0] = out[1] = out[2] = 0;
    return;
  }
  const lm_tri *t = &b->bvh.tris[h.tri];
  if (t->mesh < 0) { /* an instanced occluder: its triangle's radiance */
    const float *r = b->occ_rad + (size_t)t->tri * 3;
    out[0] = r[0];
    out[1] = r[1];
    out[2] = r[2];
    return;
  }
  const lm_mesh *m = &b->meshes.data[t->mesh];
  const float *tc = m->uw.tex + t->tri * 6;
  float w0 = 1 - h.u - h.v;
  float x = w0 * tc[0] + h.u * tc[2] + h.v * tc[4], y = w0 * tc[1] + h.u * tc[3] + h.v * tc[5];
  int ix = CW_CLAMP((int)floorf(x), 0, m->w - 1), iy = CW_CLAMP((int)floorf(y), 0, m->h - 1);
  const float *bb = m->b + ((size_t)iy * m->w + ix) * 3;
  out[0] = bb[0];
  out[1] = bb[1];
  out[2] = bb[2];
}

static uint64_t seed_of(const baker *b, int mesh, int texel, int pass) {
  uint64_t k[4] = { b->s->seed, (uint64_t)mesh, (uint64_t)texel, (uint64_t)pass };
  return cw_hash_bytes(k, sizeof k, 0x9E3779B97F4A7C15ull);
}

/* ── per texel work (rows of one mesh) ──────────────────────────────── */

typedef struct row_job { baker *b; int mesh; int kind; } row_job; /* kind 0 direct, 1 gather */

static void rows_fn(int begin, int end, int worker, void *ctx) {
  (void)worker;
  row_job *j = ctx;
  baker *b = j->b;
  lm_mesh *m = &b->meshes.data[j->mesh];
  int S = CW_MAX(b->s->samples, 1);
  for (int y = begin; y < end; y++) {
    long long rays = 0;
    for (int x = 0; x < m->w; x++) {
      int t = y * m->w + x;
      if (m->texel_tri[t] < 0) continue;
      const float *p = m->tp + t * 3, *n = m->tn + t * 3;
      if (j->kind == 0) {
        cw_rng drng;
        cw_rng_seed(&drng, seed_of(b, j->mesh, t, 1000));
        direct_at(b, p, n, m->ed + t * 3, b->any_direct ? m->ebd + t * 3 : NULL, &drng, &rays);
        continue;
      }
      cw_rng rng;
      cw_rng_seed(&rng, seed_of(b, j->mesh, t, b->iteration));
      float tg[3], bt[3];
      onb(n, tg, bt);
      float o[3] = { p[0] + n[0] * b->eps, p[1] + n[1] * b->eps, p[2] + n[2] * b->eps };
      float acc[3] = { 0, 0, 0 };
      int backs = 0;
      for (int s = 0; s < S; s++) {
        float r1 = cw_rng_float(&rng), r2 = cw_rng_float(&rng);
        float sp, cp, r = sqrtf(r2);
        cw_sincos(CW_TAU * r1, &sp, &cp);
        float lx = r * cp, ly = r * sp, lz = sqrtf(CW_MAX(1 - r2, 0.0f));
        float d[3] = { tg[0] * lx + bt[0] * ly + n[0] * lz, tg[1] * lx + bt[1] * ly + n[1] * lz, tg[2] * lx + bt[2] * ly + n[2] * lz };
        float L[3];
        bool back;
        trace(b, o, d, L, &back);
        backs += back;
        acc[0] += L[0];
        acc[1] += L[1];
        acc[2] += L[2];
      }
      rays += S;
      /* a texel whose centre is buried in a neighbouring surface (a wall
       * meeting the floor) sees back faces: its value comes from dilation */
      if (b->iteration == 0) m->inside[t] = backs * 4 > S;
      /* cosine-weighted: E = PI / N x sum of radiance */
      float k = CW_PI / (float)S;
      m->ei_next[t * 3] = acc[0] * k;
      m->ei_next[t * 3 + 1] = acc[1] * k;
      m->ei_next[t * 3 + 2] = acc[2] * k;
    }
    b->row_rays[y] = rays;
  }
}

static long long run_rows(baker *b, int mesh, int kind) {
  lm_mesh *m = &b->meshes.data[mesh];
  b->row_rays = cw_calloc((size_t)m->h, sizeof(long long), CW_MEM_RENDER);
  row_job j = { b, mesh, kind };
  cw_parallel_for(m->h, 1, rows_fn, &j);
  long long sum = 0;
  for (int y = 0; y < m->h; y++) sum += b->row_rays[y];
  cw_free(b->row_rays);
  b->row_rays = NULL;
  return sum;
}

/* rasterize each triangle's texel centres: world position and normal */
static void rasterize(lm_mesh *m) {
  int n = m->w * m->h;
  m->texel_tri = cw_alloc((size_t)n * sizeof(int), CW_MEM_RENDER);
  for (int i = 0; i < n; i++) m->texel_tri[i] = -1;
  m->inside = cw_calloc((size_t)n, 1, CW_MEM_RENDER);
  m->tp = cw_calloc((size_t)n * 3, sizeof(float), CW_MEM_RENDER);
  m->tn = cw_calloc((size_t)n * 3, sizeof(float), CW_MEM_RENDER);
  for (int t = 0; t < m->tri_count; t++) {
    const float *c = m->uw.tex + t * 6;
    float x0 = c[0], y0 = c[1], x1 = c[2], y1 = c[3], x2 = c[4], y2 = c[5];
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0) continue;
    int ix0 = (int)floorf(CW_MIN(x0, CW_MIN(x1, x2))), ix1 = (int)ceilf(CW_MAX(x0, CW_MAX(x1, x2)));
    int iy0 = (int)floorf(CW_MIN(y0, CW_MIN(y1, y2))), iy1 = (int)ceilf(CW_MAX(y0, CW_MAX(y1, y2)));
    ix0 = CW_MAX(ix0, 0);
    iy0 = CW_MAX(iy0, 0);
    ix1 = CW_MIN(ix1, m->w - 1);
    iy1 = CW_MIN(iy1, m->h - 1);
    for (int y = iy0; y <= iy1; y++)
      for (int x = ix0; x <= ix1; x++) {
        float px = (float)x + 0.5f, py = (float)y + 0.5f;
        float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
        float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
        float w2 = 1 - w0 - w1;
        if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
        int ti = y * m->w + x;
        if (m->texel_tri[ti] >= 0) continue;
        m->texel_tri[ti] = t;
        const float *p = m->pos + t * 9, *nn = m->nrm + t * 9;
        float nv[3];
        for (int a = 0; a < 3; a++) {
          m->tp[ti * 3 + a] = w0 * p[a] + w1 * p[3 + a] + w2 * p[6 + a];
          nv[a] = w0 * nn[a] + w1 * nn[3 + a] + w2 * nn[6 + a];
        }
        float l = sqrtf(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
        for (int a = 0; a < 3; a++) m->tn[ti * 3 + a] = l > 0 ? nv[a] / l : 0;
      }
  }
}

/* edge-aware smoothing of the Monte Carlo noise: each covered texel takes
 * the mean of the covered texels within `radius` whose normals agree and
 * whose positions are near (texels of the same chart; other charts are far
 * apart in world space) */
typedef struct denoise_job { const lm_mesh *m; const float *src; float *dst; int radius; float maxd2; } denoise_job;
static void denoise_rows(int begin, int end, int worker, void *ctx) {
  (void)worker;
  denoise_job *j = ctx;
  const lm_mesh *m = j->m;
  for (int y = begin; y < end; y++)
    for (int x = 0; x < m->w; x++) {
      int i = y * m->w + x;
      if (m->texel_tri[i] < 0 || m->inside[i]) {
        for (int a = 0; a < 3; a++) j->dst[i * 3 + a] = j->src[i * 3 + a];
        continue;
      }
      const float *n0 = m->tn + i * 3, *p0 = m->tp + i * 3;
      float s[3] = { 0, 0, 0 }, wsum = 0;
      for (int dy = -j->radius; dy <= j->radius; dy++)
        for (int dx = -j->radius; dx <= j->radius; dx++) {
          int xx = x + dx, yy = y + dy;
          if (xx < 0 || yy < 0 || xx >= m->w || yy >= m->h) continue;
          int k = yy * m->w + xx;
          if (m->texel_tri[k] < 0 || m->inside[k]) continue;
          const float *n1 = m->tn + k * 3, *p1 = m->tp + k * 3;
          if (n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] < 0.9f) continue;
          float d0 = p1[0] - p0[0], d1 = p1[1] - p0[1], d2 = p1[2] - p0[2];
          if (d0 * d0 + d1 * d1 + d2 * d2 > j->maxd2) continue;
          for (int a = 0; a < 3; a++) s[a] += j->src[k * 3 + a];
          wsum += 1;
        }
      for (int a = 0; a < 3; a++) j->dst[i * 3 + a] = wsum > 0 ? s[a] / wsum : j->src[i * 3 + a];
    }
}

static void denoise(const lm_mesh *m, float *v, int radius, float texel_world) {
  if (radius <= 0) return;
  size_t n = (size_t)m->w * m->h * 3;
  float *out = cw_alloc(n * sizeof(float), CW_MEM_RENDER);
  float md = texel_world * (float)(radius + 1) * 1.5f;
  denoise_job j = { m, v, out, radius, md * md };
  cw_parallel_for(m->h, 4, denoise_rows, &j);
  memcpy(v, out, n * sizeof(float));
  cw_free(out);
}

/* fill gap texels from covered neighbours, `passes` rings deep */
static void dilate(const lm_mesh *m, float *v, int passes) {
  int n = m->w * m->h;
  uint8_t *ok = cw_alloc((size_t)n, CW_MEM_RENDER), *next = cw_alloc((size_t)n, CW_MEM_RENDER);
  for (int i = 0; i < n; i++) ok[i] = m->texel_tri[i] >= 0 && !m->inside[i];
  for (int p = 0; p < passes; p++) {
    memcpy(next, ok, (size_t)n);
    for (int y = 0; y < m->h; y++)
      for (int x = 0; x < m->w; x++) {
        int i = y * m->w + x;
        if (ok[i]) continue;
        float s[3] = { 0, 0, 0 };
        int c = 0;
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            int xx = x + dx, yy = y + dy;
            if ((!dx && !dy) || xx < 0 || yy < 0 || xx >= m->w || yy >= m->h) continue;
            int k = yy * m->w + xx;
            if (!ok[k]) continue;
            for (int a = 0; a < 3; a++) s[a] += v[k * 3 + a];
            c++;
          }
        if (!c) continue;
        for (int a = 0; a < 3; a++) v[i * 3 + a] = s[a] / (float)c;
        next[i] = 1;
      }
    memcpy(ok, next, (size_t)n);
  }
  cw_free(ok);
  cw_free(next);
}

/* ── the probe grid ─────────────────────────────────────────────────── */

typedef struct probe_job { baker *b; cw_lightmap_data *d; float *pos; uint8_t *valid; long long *rays; } probe_job;

static void probes_fn(int begin, int end, int worker, void *ctx) {
  (void)worker;
  probe_job *j = ctx;
  baker *b = j->b;
  int S = CW_MAX(b->s->probe_samples, 8);
  for (int i = begin; i < end; i++) {
    cw_rng rng;
    cw_rng_seed(&rng, seed_of(b, -1, i, 0));
    const float *p = j->pos + i * 3;
    float c[27];
    memset(c, 0, sizeof c);
    int backs = 0;
    for (int s = 0; s < S; s++) {
      float r1 = cw_rng_float(&rng), r2 = cw_rng_float(&rng);
      float z = 1 - 2 * r1, r = sqrtf(CW_MAX(1 - z * z, 0.0f)), sp, cp;
      cw_sincos(CW_TAU * r2, &sp, &cp);
      float d[3] = { r * cp, r * sp, z };
      float L[3];
      bool back;
      trace(b, p, d, L, &back);
      backs += back;
      /* three.js SphericalHarmonics3 basis */
      float x = d[0], y = d[1], zz = d[2];
      float Y[9] = { 0.282095f, 0.488603f * y, 0.488603f * zz, 0.488603f * x, 1.092548f * x * y, 1.092548f * y * zz,
                     0.315392f * (3 * zz * zz - 1), 1.092548f * x * zz, 0.546274f * (x * x - y * y) };
      for (int k = 0; k < 9; k++)
        for (int ch = 0; ch < 3; ch++) c[k * 3 + ch] += L[ch] * Y[k];
    }
    j->rays[i] = S;
    float w = 4 * CW_PI / (float)S;
    /* stored as radiance SH */
    float *o = j->d->sh + (size_t)i * 27;
    for (int k = 0; k < 27; k++) o[k] = c[k] * w;
    j->valid[i] = backs * 4 < S; /* mostly inside geometry: invalid */
  }
}

static void bake_probes(baker *b, cw_lightmap_data *d) {
  float sp = b->s->probe_spacing;
  if (sp <= 0 || !b->meshes.count) return;
  float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
  for (int i = 0; i < b->meshes.count; i++) {
    const lm_mesh *m = &b->meshes.data[i];
    for (int k = 0; k < m->tri_count * 3; k++)
      for (int a = 0; a < 3; a++) {
        float v = m->pos[k * 3 + a];
        lo[a] = v < lo[a] ? v : lo[a];
        hi[a] = v > hi[a] ? v : hi[a];
      }
  }
  int dims[3];
  for (int a = 0; a < 3; a++) {
    lo[a] -= 0.25f * sp;
    hi[a] += 0.25f * sp;
    dims[a] = CW_CLAMP((int)ceilf((hi[a] - lo[a]) / sp) + 1, 2, 64);
  }
  d->has_sh = true;
  d->nx = dims[0];
  d->ny = dims[1];
  d->nz = dims[2];
  memcpy(d->lo, lo, sizeof lo);
  memcpy(d->hi, hi, sizeof hi);
  int n = dims[0] * dims[1] * dims[2];
  d->sh = cw_calloc((size_t)n * 27, sizeof(float), CW_MEM_RENDER);
  float *pos = cw_alloc((size_t)n * 3 * sizeof(float), CW_MEM_RENDER);
  for (int z = 0; z < dims[2]; z++)
    for (int y = 0; y < dims[1]; y++)
      for (int x = 0; x < dims[0]; x++) {
        int i = (z * dims[1] + y) * dims[0] + x;
        int c[3] = { x, y, z };
        for (int a = 0; a < 3; a++) pos[i * 3 + a] = lo[a] + (hi[a] - lo[a]) * (float)c[a] / (float)(dims[a] - 1);
      }
  uint8_t *valid = cw_alloc((size_t)n, CW_MEM_RENDER);
  long long *rays = cw_calloc((size_t)n, sizeof(long long), CW_MEM_RENDER);
  probe_job j = { b, d, pos, valid, rays };
  cw_parallel_for(n, 4, probes_fn, &j);
  for (int i = 0; i < n; i++) b->rays += rays[i];
  /* probes inside geometry take their valid neighbours' average */
  int invalid = 0;
  for (int i = 0; i < n; i++) invalid += !valid[i];
  d->info.invalid_probes = invalid;
  uint8_t *nv = cw_alloc((size_t)n, CW_MEM_RENDER);
  for (int pass = 0; pass < 8 && invalid; pass++) {
    memcpy(nv, valid, (size_t)n);
    for (int z = 0; z < dims[2]; z++)
      for (int y = 0; y < dims[1]; y++)
        for (int x = 0; x < dims[0]; x++) {
          int i = (z * dims[1] + y) * dims[0] + x;
          if (valid[i]) continue;
          static const int off[6][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
          float s[27];
          memset(s, 0, sizeof s);
          int c = 0;
          for (int k = 0; k < 6; k++) {
            int xx = x + off[k][0], yy = y + off[k][1], zz = z + off[k][2];
            if (xx < 0 || yy < 0 || zz < 0 || xx >= dims[0] || yy >= dims[1] || zz >= dims[2]) continue;
            int q = (zz * dims[1] + yy) * dims[0] + xx;
            if (!valid[q]) continue;
            for (int e = 0; e < 27; e++) s[e] += d->sh[(size_t)q * 27 + e];
            c++;
          }
          if (!c) continue;
          for (int e = 0; e < 27; e++) d->sh[(size_t)i * 27 + e] = s[e] / (float)c;
          nv[i] = 1;
        }
    memcpy(valid, nv, (size_t)n);
    invalid = 0;
    for (int i = 0; i < n; i++) invalid += !valid[i];
  }
  cw_free(nv);
  cw_free(rays);
  cw_free(valid);
  cw_free(pos);
}

/* ── instanced occluders: one radiance a triangle ───────────────────── */

/* what an occluder triangle sends back: albedo / PI x (the direct light at its centre + the sky it sees, 16
 * cosine rays) + its emission. Taken before the bounces (rays that hit lightmapped surfaces then see their
 * emission only): occluders are scenery round the baked surfaces, a single bounce off them is what shows. */
typedef struct occ_job { baker *b; long long *rays; } occ_job;
static void occ_fn(int begin, int end, int worker, void *ctx) {
  (void)worker;
  occ_job *j = ctx;
  baker *b = j->b;
  for (int i = begin; i < end; i++) {
    const float *p = b->occ_pos.data + (size_t)i * 9;
    float e1[3] = { p[3] - p[0], p[4] - p[1], p[5] - p[2] }, e2[3] = { p[6] - p[0], p[7] - p[1], p[8] - p[2] };
    float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    float *o = b->occ_rad + (size_t)i * 3;
    const float *al = b->occ_alb.data + (size_t)i * 3, *em = b->occ_emi.data + (size_t)i * 3;
    if (l <= 0) {
      for (int a = 0; a < 3; a++) o[a] = em[a];
      continue;
    }
    for (int a = 0; a < 3; a++) n[a] /= l;
    float c[3] = { (p[0] + p[3] + p[6]) / 3, (p[1] + p[4] + p[7]) / 3, (p[2] + p[5] + p[8]) / 3 };
    long long rays = 0;
    float E[3];
    direct_at(b, c, n, E, NULL, NULL, &rays);
    cw_rng rng;
    cw_rng_seed(&rng, seed_of(b, -2, i, 0));
    float tg[3], bt[3];
    onb(n, tg, bt);
    float org[3] = { c[0] + n[0] * b->eps, c[1] + n[1] * b->eps, c[2] + n[2] * b->eps }, acc[3] = { 0, 0, 0 };
    enum { R = 16 };
    for (int k = 0; k < R; k++) {
      float r1 = cw_rng_float(&rng), r2 = cw_rng_float(&rng), sp, cp, r = sqrtf(r2);
      cw_sincos(CW_TAU * r1, &sp, &cp);
      float lx = r * cp, ly = r * sp, lz = sqrtf(CW_MAX(1 - r2, 0.0f));
      float d[3] = { tg[0] * lx + bt[0] * ly + n[0] * lz, tg[1] * lx + bt[1] * ly + n[1] * lz, tg[2] * lx + bt[2] * ly + n[2] * lz };
      if (!lm_bvh_any(&b->bvh, org, d, 0, 1e30f)) {
        float L[3];
        sky_radiance(b, d, L);
        for (int a = 0; a < 3; a++) acc[a] += L[a];
      }
    }
    rays += R;
    for (int a = 0; a < 3; a++) o[a] = al[a] / CW_PI * (E[a] + acc[a] * CW_PI / R) + em[a];
    j->rays[i] = rays;
  }
}

/* ── the reflection cube ────────────────────────────────────────────── */

/* the direction of texel (x, y) of cube face f (+x -x +y -y +z -z, row 0 at the top), as three.js samples a
 * CubeTexture (it flips x: CubeTexture._needsFlipEnvMap) */
static void cube_dir(int f, float sc, float tc, float d[3]) {
  switch (f) {
  case 0: d[0] = 1, d[1] = -tc, d[2] = -sc; break;
  case 1: d[0] = -1, d[1] = -tc, d[2] = sc; break;
  case 2: d[0] = sc, d[1] = 1, d[2] = tc; break;
  case 3: d[0] = sc, d[1] = -1, d[2] = -tc; break;
  case 4: d[0] = sc, d[1] = -tc, d[2] = 1; break;
  default: d[0] = -sc, d[1] = -tc, d[2] = -1; break;
  }
  d[0] = -d[0];
  float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  for (int a = 0; a < 3; a++) d[a] /= l;
}

typedef struct cube_job { baker *b; float *out; int size; } cube_job;
static void cube_fn(int begin, int end, int worker, void *ctx) {
  (void)worker;
  cube_job *j = ctx;
  const baker *b = j->b;
  const float *o = b->s->reflection_origin;
  int S = j->size;
  for (int row = begin; row < end; row++) { /* row = face * S + y */
    int f = row / S, y = row % S;
    for (int x = 0; x < S; x++) {
      cw_rng rng;
      cw_rng_seed(&rng, seed_of(b, -3, row * S + x, 0));
      float acc[3] = { 0, 0, 0 };
      enum { SS = 4 };
      for (int k = 0; k < SS; k++) {
        float sc = 2 * ((float)x + cw_rng_float(&rng)) / (float)S - 1, tc = 2 * ((float)y + cw_rng_float(&rng)) / (float)S - 1;
        float d[3], L[3];
        bool back;
        cube_dir(f, sc, tc, d);
        trace(b, o, d, L, &back);
        for (int a = 0; a < 3; a++) acc[a] += L[a];
      }
      for (int a = 0; a < 3; a++) j->out[((size_t)row * S + x) * 3 + a] = acc[a] / SS;
    }
  }
}

/* ── the bake ───────────────────────────────────────────────────────── */

static void baker_free(baker *b) {
  for (int i = 0; i < b->meshes.count; i++) {
    lm_mesh *m = &b->meshes.data[i];
    cw_free(m->pos);
    cw_free(m->nrm);
    lm_unwrap_free(&m->uw);
    cw_free(m->texel_tri);
    cw_free(m->inside);
    cw_free(m->tp);
    cw_free(m->tn);
    cw_free(m->ed);
    cw_free(m->ei);
    cw_free(m->b);
    cw_free(m->ei_next);
    cw_free(m->ebd);
  }
  cw_array_free(&b->meshes);
  cw_array_free(&b->lights);
  cw_array_free(&b->occ_pos);
  cw_array_free(&b->occ_alb);
  cw_array_free(&b->occ_emi);
  cw_free(b->occ_rad);
  lm_bvh_free(&b->bvh);
  cw_free(b->sky.table);
}

static void update_radiance(lm_mesh *m) {
  int n = m->w * m->h;
  float ar = m->albedo.r / CW_PI, ag = m->albedo.g / CW_PI, ab = m->albedo.b / CW_PI;
  for (int i = 0; i < n; i++) {
    m->b[i * 3] = ar * (m->ed[i * 3] + m->ei[i * 3]) + m->emissive.r;
    m->b[i * 3 + 1] = ag * (m->ed[i * 3 + 1] + m->ei[i * 3 + 1]) + m->emissive.g;
    m->b[i * 3 + 2] = ab * (m->ed[i * 3 + 2] + m->ei[i * 3 + 2]) + m->emissive.b;
  }
  dilate(m, m->b, 3);
}

cw_lightmap_data *cw_lightmap_bake(cw_tree *t, cw_node root, const cw_lightmap_settings *settings) {
  cw_lightmap_settings s = settings ? *settings : cw_lightmap_settings_default();
  baker b;
  memset(&b, 0, sizeof b);
  b.tree = t;
  b.s = &s;
  /* world matrices as the nodes have them now */
  t3_object_update_matrix_world(cw_render3d_scene(t), false);
  gather_ctx g = { &b, root };
  cw_node_walk(root, gather_node, &g);
  if (!b.meshes.count) {
    cw_warn("cw_lightmap_bake: no MeshInstance3D with gi_mode STATIC under the root: nothing to bake");
    baker_free(&b);
    return NULL;
  }
  sky_build(&b);
  /* the BVH over every static triangle; rays start eps off the surface */
  int total = b.occ_count;
  float span = 0;
  for (int i = 0; i < b.meshes.count; i++) total += b.meshes.data[i].tri_count;
  lm_tri *tris = cw_alloc((size_t)total * sizeof *tris, CW_MEM_RENDER);
  int at = 0;
  for (int i = 0; i < b.meshes.count; i++) {
    const lm_mesh *m = &b.meshes.data[i];
    for (int k = 0; k < m->tri_count; k++, at++) {
      const float *p = m->pos + k * 9;
      lm_tri *tr = &tris[at];
      for (int a = 0; a < 3; a++) {
        tr->v0[a] = p[a];
        tr->e1[a] = p[3 + a] - p[a];
        tr->e2[a] = p[6 + a] - p[a];
        float ext = fabsf(p[a]);
        span = ext > span ? ext : span;
      }
      tr->mesh = i;
      tr->tri = k;
    }
  }
  for (int k = 0; k < b.occ_count; k++, at++) {
    const float *p = b.occ_pos.data + (size_t)k * 9;
    lm_tri *tr = &tris[at];
    for (int a = 0; a < 3; a++) {
      tr->v0[a] = p[a];
      tr->e1[a] = p[3 + a] - p[a];
      tr->e2[a] = p[6 + a] - p[a];
      float ext = fabsf(p[a]);
      span = ext > span ? ext : span;
    }
    tr->mesh = -1;
    tr->tri = k;
  }
  lm_bvh_build(&b.bvh, tris, total);
  b.eps = CW_MAX(span * 2e-5f, 1e-3f);
  if (b.occ_count) {
    b.occ_rad = cw_calloc((size_t)b.occ_count * 3, sizeof(float), CW_MEM_RENDER);
    long long *rays = cw_calloc((size_t)b.occ_count, sizeof(long long), CW_MEM_RENDER);
    occ_job oj = { &b, rays };
    cw_parallel_for(b.occ_count, 64, occ_fn, &oj);
    for (int k = 0; k < b.occ_count; k++) b.rays += rays[k];
    cw_free(rays);
  }
  /* texels, direct light */
  for (int i = 0; i < b.meshes.count; i++) {
    lm_mesh *m = &b.meshes.data[i];
    rasterize(m);
    int n = m->w * m->h;
    m->ed = cw_calloc((size_t)n * 3, sizeof(float), CW_MEM_RENDER);
    m->ei = cw_calloc((size_t)n * 3, sizeof(float), CW_MEM_RENDER);
    m->b = cw_calloc((size_t)n * 3, sizeof(float), CW_MEM_RENDER);
    m->ei_next = cw_calloc((size_t)n * 3, sizeof(float), CW_MEM_RENDER);
    if (b.any_direct) m->ebd = cw_calloc((size_t)n * 3, sizeof(float), CW_MEM_RENDER);
    b.rays += run_rows(&b, i, 0);
  }
  /* bounces: each gathers the radiance the last one left on the surfaces */
  int bounces = CW_CLAMP(s.bounces, 1, 16);
  for (int it = 0; it < bounces; it++) {
    for (int i = 0; i < b.meshes.count; i++) update_radiance(&b.meshes.data[i]);
    b.iteration = it;
    for (int i = 0; i < b.meshes.count; i++) b.rays += run_rows(&b, i, 1);
    for (int i = 0; i < b.meshes.count; i++) {
      lm_mesh *m = &b.meshes.data[i];
      memcpy(m->ei, m->ei_next, (size_t)m->w * m->h * 3 * sizeof(float));
    }
  }
  for (int i = 0; i < b.meshes.count; i++) update_radiance(&b.meshes.data[i]);
  /* the result */
  cw_lightmap_data *d = cw_calloc(1, sizeof *d, CW_MEM_RENDER);
  for (int i = 0; i < b.meshes.count; i++) {
    lm_mesh *m = &b.meshes.data[i];
    denoise(m, m->ei, s.denoise, 1.0f / CW_MAX(s.texel_density, 1e-3f));
    if (m->ebd) { /* the baked direct light: exact shadow rays (soft ones are already averaged), not smoothed */
      for (int k = 0; k < m->w * m->h * 3; k++) m->ei[k] += m->ebd[k];
    }
    dilate(m, m->ei, 4);
    lm_rec r;
    memset(&r, 0, sizeof r);
    snprintf(r.path, sizeof r.path, "%s", m->path);
    r.tri_count = m->tri_count;
    r.sig = m->sig;
    r.w = m->w;
    r.h = m->h;
    r.uv2 = cw_alloc((size_t)m->tri_count * 6 * sizeof(float) + 4, CW_MEM_RENDER);
    memcpy(r.uv2, m->uw.uv2, (size_t)m->tri_count * 6 * sizeof(float));
    int n = m->w * m->h;
    r.texels = cw_alloc((size_t)n * 4 * sizeof(uint16_t), CW_MEM_RENDER);
    for (int x = 0; x < n; x++) {
      for (int c = 0; c < 3; c++) r.texels[x * 4 + c] = lm_float_to_half(m->ei[x * 3 + c]);
      r.texels[x * 4 + 3] = lm_float_to_half(1.0f);
    }
    cw_array_push(&d->recs, r);
    d->info.triangles += m->tri_count;
    d->info.texels += n;
    d->info.width_total += m->w;
  }
  d->info.meshes = b.meshes.count;
  if (s.reflection_size > 0) {
    int S = CW_CLAMP(s.reflection_size, 4, 256);
    float *rad = cw_alloc((size_t)6 * S * S * 3 * sizeof(float), CW_MEM_RENDER);
    cube_job cj = { &b, rad, S };
    cw_parallel_for(6 * S, 1, cube_fn, &cj);
    b.rays += (long long)6 * S * S * 4;
    d->cube_size = S;
    d->cube = cw_alloc((size_t)6 * S * S * 4 * sizeof(uint16_t), CW_MEM_RENDER);
    for (int k = 0; k < 6 * S * S; k++) {
      for (int a = 0; a < 3; a++) d->cube[k * 4 + a] = lm_float_to_half(rad[k * 3 + a]);
      d->cube[k * 4 + 3] = lm_float_to_half(1.0f);
    }
    cw_free(rad);
  }
  bake_probes(&b, d);
  d->info.probes = d->has_sh ? d->nx * d->ny * d->nz : 0;
  d->info.rays = b.rays;
  serialize(d);
  baker_free(&b);
  return d;
}

/* ── applying a bake ────────────────────────────────────────────────── */

/* ── the irradiance below a point (cw_lightmap_irradiance_below) ─────── */

static void below_add(cw_lightmap_data *d, t3_geometry *ni, const t3_mat4 *w, const lm_rec *r, int rec) {
  const t3_attribute *pa = ni->attributes[T3_ATTR_POSITION];
  if (!pa || pa->type != T3_FLOAT32 || pa->count / 3 != r->tri_count) return;
  const float *P = pa->array;
  d->btris = cw_realloc(d->btris, sizeof *d->btris * (size_t)(d->nbtris + r->tri_count), CW_MEM_RENDER);
  for (int t = 0; t < r->tri_count; t++) {
    struct below_tri *bt = &d->btris[d->nbtris];
    for (int k = 0; k < 3; k++) mat_apply(w, P + (t * 3 + k) * 3, bt->p + k * 3);
    const float *p = bt->p;
    float e1[3] = { p[3] - p[0], p[4] - p[1], p[5] - p[2] }, e2[3] = { p[6] - p[0], p[7] - p[1], p[8] - p[2] };
    float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
    float l = sqrtf(nx * nx + ny * ny + nz * nz);
    if (l <= 0 || ny / l < 0.3f) continue; /* faces up: ground, a floor, a ramp */
    memcpy(bt->uv, r->uv2 + t * 6, sizeof bt->uv);
    bt->rec = rec;
    d->nbtris++;
  }
  cw_free(d->gstart), cw_free(d->gidx);
  d->gstart = d->gidx = NULL; /* the grid is (re)built on the next query */
}

static void below_grid(cw_lightmap_data *d) {
  float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f }, area = 0;
  for (int i = 0; i < d->nbtris; i++)
    for (int k = 0; k < 3; k++) {
      lo[0] = CW_MIN(lo[0], d->btris[i].p[k * 3]), hi[0] = CW_MAX(hi[0], d->btris[i].p[k * 3]);
      lo[1] = CW_MIN(lo[1], d->btris[i].p[k * 3 + 2]), hi[1] = CW_MAX(hi[1], d->btris[i].p[k * 3 + 2]);
    }
  area = (hi[0] - lo[0]) * (hi[1] - lo[1]);
  /* about four triangles a cell */
  float cell = sqrtf(CW_MAX(area, 1e-6f) / (float)CW_MAX(d->nbtris / 4, 1));
  d->gcell = CW_MAX(cell, 1e-3f);
  d->glo[0] = lo[0], d->glo[1] = lo[1];
  d->gw = CW_CLAMP((int)((hi[0] - lo[0]) / d->gcell) + 1, 1, 4096);
  d->gh = CW_CLAMP((int)((hi[1] - lo[1]) / d->gcell) + 1, 1, 4096);
  int cells = d->gw * d->gh;
  d->gstart = cw_calloc((size_t)cells + 1, sizeof(int), CW_MEM_RENDER);
  for (int pass = 0; pass < 2; pass++) {
    int *fill = pass ? cw_alloc(sizeof(int) * (size_t)cells, CW_MEM_RENDER) : NULL;
    if (pass) {
      for (int c = 0; c < cells; c++) d->gstart[c + 1] += d->gstart[c];
      memcpy(fill, d->gstart, sizeof(int) * (size_t)cells);
      d->gidx = cw_alloc(sizeof(int) * (size_t)CW_MAX(d->gstart[cells], 1), CW_MEM_RENDER);
    }
    for (int i = 0; i < d->nbtris; i++) {
      const float *p = d->btris[i].p;
      float x0 = CW_MIN(p[0], CW_MIN(p[3], p[6])), x1 = CW_MAX(p[0], CW_MAX(p[3], p[6]));
      float z0 = CW_MIN(p[2], CW_MIN(p[5], p[8])), z1 = CW_MAX(p[2], CW_MAX(p[5], p[8]));
      int cx0 = CW_CLAMP((int)((x0 - lo[0]) / d->gcell), 0, d->gw - 1), cx1 = CW_CLAMP((int)((x1 - lo[0]) / d->gcell), 0, d->gw - 1);
      int cz0 = CW_CLAMP((int)((z0 - lo[1]) / d->gcell), 0, d->gh - 1), cz1 = CW_CLAMP((int)((z1 - lo[1]) / d->gcell), 0, d->gh - 1);
      for (int z = cz0; z <= cz1; z++)
        for (int x = cx0; x <= cx1; x++) {
          if (pass) d->gidx[fill[z * d->gw + x]++] = i;
          else d->gstart[z * d->gw + x + 1]++;
        }
    }
    cw_free(fill);
  }
}

static void texel_bilinear(const lm_rec *r, float u, float v, float out[3]) {
  float x = u * (float)r->w - 0.5f, y = v * (float)r->h - 0.5f;
  int x0 = (int)floorf(x), y0 = (int)floorf(y);
  float fx = x - (float)x0, fy = y - (float)y0;
  out[0] = out[1] = out[2] = 0;
  for (int k = 0; k < 4; k++) {
    int xx = CW_CLAMP(x0 + (k & 1), 0, r->w - 1), yy = CW_CLAMP(y0 + (k >> 1), 0, r->h - 1);
    float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
    const uint16_t *t = r->texels + ((size_t)yy * r->w + xx) * 4;
    for (int a = 0; a < 3; a++) out[a] += wgt * lm_half_to_float(t[a]);
  }
}

bool cw_lightmap_irradiance_below(const cw_lightmap_data *dc, t3_vec3 pos, t3_color *out) {
  cw_lightmap_data *d = (cw_lightmap_data *)dc;
  if (out) *out = (t3_color){ 0, 0, 0 };
  if (!d || !d->nbtris) return false;
  if (!d->gstart) below_grid(d);
  int cx = (int)floorf((pos.x - d->glo[0]) / d->gcell), cz = (int)floorf((pos.z - d->glo[1]) / d->gcell);
  if (cx < 0 || cz < 0 || cx >= d->gw || cz >= d->gh) return false;
  int c = cz * d->gw + cx, best = -1;
  float best_y = -1e30f, bw[3] = { 0, 0, 0 };
  for (int k = d->gstart[c]; k < d->gstart[c + 1]; k++) {
    const struct below_tri *t = &d->btris[d->gidx[k]];
    const float *p = t->p;
    /* barycentrics in XZ */
    float ax = p[0], az = p[2], bx = p[3], bz = p[5], qx = p[6], qz = p[8];
    float den = (bz - qz) * (ax - qx) + (qx - bx) * (az - qz);
    if (fabsf(den) < 1e-12f) continue;
    float w0 = ((bz - qz) * (pos.x - qx) + (qx - bx) * (pos.z - qz)) / den;
    float w1 = ((qz - az) * (pos.x - qx) + (ax - qx) * (pos.z - qz)) / den;
    float w2 = 1 - w0 - w1;
    if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
    float y = w0 * p[1] + w1 * p[4] + w2 * p[7];
    if (y > pos.y + 1e-3f || y <= best_y) continue;
    best_y = y, best = d->gidx[k];
    bw[0] = w0, bw[1] = w1, bw[2] = w2;
  }
  if (best < 0) return false;
  const struct below_tri *t = &d->btris[best];
  float u = bw[0] * t->uv[0] + bw[1] * t->uv[2] + bw[2] * t->uv[4], v = bw[0] * t->uv[1] + bw[1] * t->uv[3] + bw[2] * t->uv[5];
  float e[3];
  texel_bilinear(&d->recs.data[t->rec], u, v, e);
  if (out) *out = (t3_color){ e[0], e[1], e[2] };
  return true;
}

void cw_lightmap_apply(cw_tree *t, cw_node root, const cw_lightmap_data *d, float intensity) {
  cw_lightmap_apply_shaded(t, root, d, intensity, CW_LIGHTMAP_LIT);
}

void cw_lightmap_apply_shaded(cw_tree *t, cw_node root, const cw_lightmap_data *d, float intensity, cw_lightmap_shading shading) {
  (void)t;
  for (int i = 0; i < d->recs.count; i++) {
    const lm_rec *r = &d->recs.data[i];
    cw_node n = cw_node_get(root, r->path);
    cw__mesh_bake mb;
    if (!cw__mesh3d_bake_info(n, &mb)) {
      cw_warn("lightmap: no MeshInstance3D at '%s' (baked); skipped", r->path);
      continue;
    }
    if (geometry_sig(mb.mesh->geometry) != r->sig) {
      cw_warn("lightmap: '%s' changed since the bake (re-bake); skipped", r->path);
      continue;
    }
    t3_geometry *ni = t3_geometry_to_non_indexed(mb.mesh->geometry);
    below_add((cw_lightmap_data *)d, ni, &mb.mesh->base.matrix_world, r, i);
    t3_attribute *uv2 = t3_attribute_new(T3_FLOAT32, r->uv2, r->tri_count * 3, 2);
    t3_geometry_set_attribute(ni, T3_ATTR_UV1, uv2);
    t3_release(uv2);
    t3_texture *tex = t3_data_texture_new_typed(r->w, r->h, r->texels, T3_HALF_FLOAT_TYPE);
    tex->mag_filter = T3_LINEAR;
    tex->min_filter = T3_LINEAR;
    tex->wrap_s = tex->wrap_t = T3_CLAMP_TO_EDGE;
    cw__mesh3d_set_lightmap(n, ni, tex, intensity, shading == CW_LIGHTMAP_BAKED);
    t3_release(tex);
    t3_release(ni);
  }
}

t3_texture *cw_lightmap_reflection(const cw_lightmap_data *dc, float *scale) {
  cw_lightmap_data *d = (cw_lightmap_data *)dc; /* the texture is made once and kept with the data */
  if (scale) *scale = 0;
  if (!d || !d->cube_size) return NULL;
  if (!d->cube_tex) {
    int S = d->cube_size;
    /* an 8-bit sRGB cube scaled by the brightest texel (at most 64): the colours keep their ratios, the scale
     * goes back in through the material's reflectivity */
    float mx = 0;
    for (int k = 0; k < 6 * S * S * 4; k++)
      if ((k & 3) != 3) mx = CW_MAX(mx, lm_half_to_float(d->cube[k]));
    d->cube_scale = CW_CLAMP(mx, 1e-4f, 64.0f);
    uint8_t *faces[6];
    for (int f = 0; f < 6; f++) {
      faces[f] = cw_alloc((size_t)S * S * 4, CW_MEM_RENDER);
      for (int i = 0; i < S * S; i++)
        for (int a = 0; a < 4; a++) {
          float v = a == 3 ? 1.0f : CW_CLAMP(lm_half_to_float(d->cube[((size_t)f * S * S + i) * 4 + a]) / d->cube_scale, 0.0f, 1.0f);
          float e = a == 3 ? v : v <= 0.0031308f ? v * 12.92f : 1.055f * cw_pow(v, 1.0f / 2.4f) - 0.055f;
          faces[f][i * 4 + a] = (uint8_t)CW_CLAMP((int)(e * 255.0f + 0.5f), 0, 255);
        }
    }
    d->cube_tex = t3_cube_texture_new(S, (const uint8_t *const *)faces);
    for (int f = 0; f < 6; f++) cw_free(faces[f]);
    d->cube_tex->color_space = T3_SRGB_COLOR_SPACE;
    d->cube_tex->generate_mipmaps = false;
    d->cube_tex->min_filter = T3_LINEAR;
    d->cube_tex->mag_filter = T3_LINEAR;
  }
  if (scale) *scale = d->cube_scale;
  return d->cube_tex;
}

bool cw_lightmap_use_reflection(const cw_lightmap_data *d, t3_material *m, float strength) {
  float k;
  t3_texture *cube = cw_lightmap_reflection(d, &k);
  if (!cube || !m) return false;
  t3_material_set_texture(m, T3_ENV_MAP, cube);
  if (m->type == T3_MESH_STANDARD_MATERIAL || m->type == T3_MESH_PHYSICAL_MATERIAL) m->env_map_intensity = strength * k;
  else {
    m->combine = T3_ADD_OPERATION;
    m->reflectivity = strength * k;
  }
  return true;
}

/* ── LightmapGI ─────────────────────────────────────────────────────── */

typedef struct gi_node {
  cw_lightmap_settings s;
  int seed;
  const char *data;         /* .cwlm asset (empty: bake on the first frame) */
  bool bake_twice;          /* bake a second time and compare (determinism check, tests) */
  int shading;              /* cw_lightmap_shading */
  /* read only */
  const char *hash;
  int texels, probes;
  float bake_ms;
  bool same;
  /* runtime */
  bool done;
  cw_node self;
  char pass_name[48];
  cw_lightmap_data *result;
} gi_node;

static double wall(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void gi_bake_now(cw_node n, gi_node *g, cw_tree *t) {
  g->done = true;
  cw_node root = cw_node_parent(n);
  if (CW_HANDLE_IS_NULL(root)) root = n;
  cw_lightmap_data *d = NULL;
  if (g->data && g->data[0]) d = cw_lightmap_load(g->data);
  if (!d) {
    g->s.seed = (uint64_t)(uint32_t)g->seed;
    double t0 = wall();
    d = cw_lightmap_bake(t, root, &g->s);
    g->bake_ms = (float)((wall() - t0) * 1000);
    if (d && g->bake_twice) {
      cw_lightmap_data *again = cw_lightmap_bake(t, root, &g->s);
      g->same = again && again->len == d->len && !memcmp(again->bytes, d->bytes, d->len);
      cw_lightmap_free(again);
    }
  }
  if (!d) return;
  g->result = d;
  char hex[24];
  snprintf(hex, sizeof hex, "%016llx", (unsigned long long)cw_lightmap_hash(d));
  g->hash = cw_intern(hex);
  g->texels = d->info.texels;
  g->probes = d->info.probes;
  cw_lightmap_apply_shaded(t, root, d, g->s.intensity, (cw_lightmap_shading)g->shading);
}

static void gi_node_init(cw_node n, void *data) {
  (void)n;
  gi_node *g = data;
  g->s = cw_lightmap_settings_default();
  g->seed = 1;
  g->data = cw_intern("");
  g->hash = cw_intern("");
}

static void gi_node_destroy(cw_node n, void *data) {
  (void)n;
  gi_node *g = data;
  cw_lightmap_free(g->result);
  g->result = NULL;
}

static void gi_node_exit(cw_node n, void *data) {
  gi_node *g = data;
  cw_tree *t = cw_node_tree(n);
  if (t && g->pass_name[0] && cw_tree_slot(t, "pipeline")) cw_pipeline_remove_pass(cw_pipeline_of(t), g->pass_name);
  g->pass_name[0] = 0;
}

/* the bake runs in the SHADOW phase of the first frame: every node has
 * written its transform, the scene's world matrices are current */
static void gi_bake_pass(cw_pass_ctx *ctx, void *user) {
  gi_node *g = user; /* the pass is removed when the node leaves the tree */
  if (g->done) return;
  gi_bake_now(g->self, g, ctx->tree);
}

static void gi_node_pre_render(cw_node n, void *data, double alpha) {
  (void)alpha;
  gi_node *g = data;
  cw_tree *t = cw_node_tree(n);
  if (!t) return;
  g->self = n;
  cw_pipeline *p = cw_pipeline_of(t);
  if (!g->done && !g->pass_name[0]) {
    snprintf(g->pass_name, sizeof g->pass_name, "lightmap_bake.%llx", (unsigned long long)n.id);
    cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = g->pass_name, .phase = CW_PHASE_SHADOW, .order = -100,
                                             .execute = gi_bake_pass, .user = g });
  }
}

#define GF(field, lo, hi, docs) \
  { .name = #field, .type = CW_T_FLOAT, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(gi_node, s.field), \
    .size = (int)sizeof(float), .min = lo, .max = hi, .doc = docs }
#define GI(field, lo, hi, docs) \
  { .name = #field, .type = CW_T_INT, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(gi_node, s.field), \
    .size = (int)sizeof(int), .min = lo, .max = hi, .doc = docs }

static void ro_hash(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_string(((gi_node *)d)->hash); }
static void ro_texels(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_int(((gi_node *)d)->texels); }
static void ro_probes(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_int(((gi_node *)d)->probes); }
static void ro_ms(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_float(((gi_node *)d)->bake_ms); }
static void ro_same(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_bool(((gi_node *)d)->same); }

static const char *const shading_names[] = { "LIT", "BAKED", NULL };
static const cw_prop gi_props[] = {
  GF(texel_density, 0.01, 1000, "Lightmap texels per world unit (times each mesh's lightmap_scale)."),
  GI(min_size, 4, 4096, "Smallest lightmap side, texels."),
  GI(max_size, 8, 8192, "Largest lightmap side, texels (the density drops for a mesh that would not fit)."),
  GI(padding, 1, 16, "Texels around each chart."),
  GI(samples, 1, 65536, "Hemisphere rays per texel per bounce."),
  GI(bounces, 1, 16, "Light bounces."),
  { .name = "seed", .type = CW_T_INT, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(gi_node, seed), .size = (int)sizeof(int),
    .doc = "Random sequence seed (the same seed and scene give the same bytes)." },
  GF(probe_spacing, 0, 1000, "World units between ambient SH probes (0 = no probe grid)."),
  GI(probe_samples, 8, 65536, "Rays per SH probe."),
  GF(intensity, 0, 100, "Multiplier of the baked light."),
  GI(denoise, 0, 8, "Edge-aware smoothing radius in texels (0 = the raw Monte Carlo result)."),
  CW_PROP_FIELD(gi_node, data, CW_T_STRING, CW_PROP_DEFAULT, "A .cwlm asset to load (empty or missing: bake on the first frame)."),
  GI(direct_samples, 1, 1024, "Shadow rays a texel per light for the baked direct light (soft shadows need several)."),
  GF(shadow_softness, 0, 1, "Baked shadows' softness: the light's radius over its distance (0 = hard)."),
  GI(reflection_size, 0, 256, "Face side of the reflection cube traced through the bake (0 = none; cw_lightmap_use_reflection)."),
  { .name = "reflection_origin", .type = CW_T_VEC3, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(gi_node, s.reflection_origin),
    .size = (int)sizeof(float[3]), .doc = "Where the reflection cube is traced from (world)." },
  { .name = "shading", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(gi_node, shading), .size = sizeof(int),
    .enum_names = shading_names,
    .doc = "LIT: lightmapped meshes keep their material (real-time lights add the direct light). BAKED: they draw unlit "
           "from the lightmap alone (lights with bake_direct put their direct light in it)." },
  CW_PROP_FIELD(gi_node, bake_twice, CW_T_BOOL, CW_PROP_DEFAULT, "Bake a second time and compare the bytes (`reproducible`)."),
  CW_PROP_ACCESSOR("hash", CW_T_STRING, CW_PROP_EDITOR | CW_PROP_READONLY, ro_hash, NULL, "The bake's fingerprint (FNV-1a 64 of its bytes)."),
  CW_PROP_ACCESSOR("texels", CW_T_INT, CW_PROP_EDITOR | CW_PROP_READONLY, ro_texels, NULL, "Lightmap texels in the bake."),
  CW_PROP_ACCESSOR("probes", CW_T_INT, CW_PROP_EDITOR | CW_PROP_READONLY, ro_probes, NULL, "SH probes in the bake."),
  CW_PROP_ACCESSOR("bake_ms", CW_T_FLOAT, CW_PROP_EDITOR | CW_PROP_READONLY, ro_ms, NULL, "Wall time of the bake (report only)."),
  CW_PROP_ACCESSOR("reproducible", CW_T_BOOL, CW_PROP_EDITOR | CW_PROP_READONLY, ro_same, NULL,
                   "With bake_twice: the second bake's bytes equal the first's."),
  CW_PROP_END,
};

const cw_lightmap_data *cw_lightmap_gi_data(cw_node gi) {
  gi_node *g = cw_node_is(gi, "LightmapGI") ? cw_node_data(gi, "LightmapGI") : NULL;
  return g ? g->result : NULL;
}

static cw_node_type gi_type = {
  .name = "LightmapGI", .parent = "Node", .data_size = sizeof(gi_node), .props = gi_props,
  .doc = "Baked lighting for its parent's subtree: lightmaps (generated uv2) for STATIC meshes, an SH probe grid for "
         "the rest; bakes on the first frame or loads a .cwlm (cartwheel/lightmap.h).",
  .init = gi_node_init, .destroy = gi_node_destroy, .exit_tree = gi_node_exit, .pre_render = gi_node_pre_render,
};

void cw__register_lightmap_types(void) { cw_node_type_register(&gi_type); }
