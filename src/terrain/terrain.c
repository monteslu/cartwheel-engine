/* Terrain data (cartwheel/terrain.h): samples, control words, imports,
 * layers, the deterministic queries and the Box3D height-field regions.
 *
 * Every region always has its Box3D height field, physics or not: the
 * queries (cw_terrain_height_at, cw_terrain_raycast) run Box3D's own ray
 * cast on it, in the order a world ray cast does its arithmetic, so a query
 * and a physics ray agree to the bit. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/jobs.h"
#include "cartwheel/math.h"
#include "cartwheel/quality.h"
#include "cartwheel/vfs.h"
#include "physics/physics3d_internal.h"
#include "terrain_internal.h"

/* three.c's stb_image (src/gltf.c) */
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req_comp);
unsigned short *stbi_load_16_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req_comp);
void stbi_image_free(void *p);

cw_terrain_def cw_terrain_default_def(void) {
  return (cw_terrain_def){ .size_x = 512, .size_z = 512, .region_size = 256, .vertex_spacing = 1, .min_height = 0,
                           .max_height = 100, .grid_quads = 32, .lod_ratio = 2, .morph_start = 0.66f,
                           .far_distance = 400 };
}

static bool pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

cw_terrain *cw_terrain_create(const cw_terrain_def *def_in) {
  cw_terrain_def d = def_in ? *def_in : cw_terrain_default_def();
  if (!d.grid_quads) d.grid_quads = 32;
  if (!d.region_size) d.region_size = 256;
  if (!(d.vertex_spacing > 0)) d.vertex_spacing = 1;
  if (!(d.lod_ratio > 1)) d.lod_ratio = 2;
  if (!(d.morph_start >= 0 && d.morph_start < 1)) d.morph_start = 0.66f;
  if (!(d.far_distance > 0)) d.far_distance = 400;
  if (!(d.far_fade > 0)) d.far_fade = d.far_distance * 0.25f;
  if (!pow2(d.grid_quads) || d.grid_quads < 8 || d.grid_quads > 64) {
    cw_error("terrain: grid_quads %d must be a power of two in 8..64", d.grid_quads);
    return NULL;
  }
  if (!pow2(d.region_size)) {
    cw_error("terrain: region_size %d must be a power of two", d.region_size);
    return NULL;
  }
  if (d.size_x <= 0 || d.size_z <= 0 || d.size_x % d.grid_quads || d.size_z % d.grid_quads || d.size_x > 8192 ||
      d.size_z > 8192) {
    cw_error("terrain: size %d x %d must be positive multiples of grid_quads (%d), at most 8192", d.size_x, d.size_z,
             d.grid_quads);
    return NULL;
  }
  if (!(d.max_height > d.min_height)) {
    cw_error("terrain: max_height (%g) must exceed min_height (%g)", d.max_height, d.min_height);
    return NULL;
  }
  cw_terrain *t = cw_calloc(1, sizeof *t, CW_MEM_RENDER);
  t->def = d;
  t->nx = d.size_x + 1;
  t->nz = d.size_z + 1;
  t->sy = (d.max_height - d.min_height) / 65535.0f;
  t->h = cw_calloc((size_t)t->nx * t->nz, sizeof *t->h, CW_MEM_RENDER);
  t->ctl = cw_calloc((size_t)t->nx * t->nz, sizeof *t->ctl, CW_MEM_RENDER);
  t->rnx = (d.size_x + d.region_size - 1) / d.region_size;
  t->rnz = (d.size_z + d.region_size - 1) / d.region_size;
  t->regions = cw_calloc((size_t)t->rnx * t->rnz, sizeof *t->regions, CW_MEM_RENDER);
  t->quality = -1;
  t->range_scale = 1;
  for (int i = 0; i < CW_TERRAIN_LAYERS; i++) t->layer[i].uv_scale = 0.25f;
  cw__terrain_tree_init(t);
  cw__terrain_mark(t, 0, 0, t->nx - 1, t->nz - 1, true, true);
  t->layers_dirty = true;
  return t;
}

void cw_terrain_destroy(cw_terrain *t) {
  if (!t) return;
  cw__terrain_build_finish(t, true);
  for (int i = 0; i < TR_MAX_SCATTER; i++)
    if (t->scatter[i]) cw__scatter_free(t->scatter[i]);
  cw_terrain_detach_physics(t);
  for (int i = 0; i < t->rnx * t->rnz; i++) cw_heightfield3d_destroy(t->regions[i].field);
  cw__terrain_gpu_free(t);
  cw__terrain_tree_free(t);
  for (int i = 0; i < CW_TERRAIN_LAYERS; i++) {
    cw_free(t->layer[i].albedo);
    cw_free(t->layer[i].normal);
  }
  cw_free(t->regions);
  cw_free(t->h);
  cw_free(t->ctl);
  cw_free(t);
}

const cw_terrain_def *cw_terrain_get_def(const cw_terrain *t) { return t ? &t->def : NULL; }

static t3_vec3 region_pos(const cw_terrain *t, int rx, int rz) {
  float span = (float)t->def.region_size * t->def.vertex_spacing;
  return (t3_vec3){ t->def.origin.x + (float)rx * span, t->def.origin.y + t->def.min_height,
                    t->def.origin.z + (float)rz * span };
}

void cw_terrain_set_origin(cw_terrain *t, t3_vec3 origin) {
  if (!t) return;
  if (t->def.origin.x == origin.x && t->def.origin.y == origin.y && t->def.origin.z == origin.z) return;
  cw__scatter_sync(t);
  t3_vec3 delta = { origin.x - t->def.origin.x, origin.y - t->def.origin.y, origin.z - t->def.origin.z };
  t->def.origin = origin;
  if (t->physics)
    for (int rz = 0; rz < t->rnz; rz++)
      for (int rx = 0; rx < t->rnx; rx++) {
        tr_region *r = &t->regions[rz * t->rnx + rx];
        if (cw_body3d_valid(r->body)) {
          t3_vec3 p = region_pos(t, rx, rz);
          cw_body3d_set_transform(r->body, cw_xform3_at(p.x, p.y, p.z));
        }
      }
  cw__terrain_gpu_set_origin(t);
  cw__scatter_shift(t, delta); /* placement moves with the terrain: nothing is placed again */
}

/* ── edits ──────────────────────────────────────────────────────────── */

static void grow(int *r, int x0, int z0, int x1, int z1) {
  if (r[0] > r[2]) {
    r[0] = x0, r[1] = z0, r[2] = x1, r[3] = z1;
    return;
  }
  r[0] = CW_MIN(r[0], x0), r[1] = CW_MIN(r[1], z0), r[2] = CW_MAX(r[2], x1), r[3] = CW_MAX(r[3], z1);
}

void cw__terrain_mark(cw_terrain *t, int x0, int z0, int x1, int z1, bool heights, bool control) {
  cw__scatter_sync(t);
  if (heights) {
    grow(t->dirty_h, x0, z0, x1, z1);
    t->tree_dirty = t->normals_dirty = true;
  }
  if (control) grow(t->dirty_c, x0, z0, x1, z1);
  t->macro_dirty = true;
  /* the regions whose samples (edges included) the rectangle touches */
  int R = t->def.region_size;
  int rx0 = CW_MAX(0, (x0 - 1) / R), rz0 = CW_MAX(0, (z0 - 1) / R);
  int rx1 = CW_MIN(t->rnx - 1, x1 / R), rz1 = CW_MIN(t->rnz - 1, z1 / R);
  if (heights || control)
    for (int rz = rz0; rz <= rz1; rz++)
      for (int rx = rx0; rx <= rx1; rx++) t->regions[rz * t->rnx + rx].dirty = true;
}

static bool clip_rect(const cw_terrain *t, int *x0, int *z0, int *w, int *h, int *sx, int *sz) {
  *sx = *x0 < 0 ? -*x0 : 0;
  *sz = *z0 < 0 ? -*z0 : 0;
  int ax = *x0 + *sx, az = *z0 + *sz;
  int bx = CW_MIN(*x0 + *w, t->nx), bz = CW_MIN(*z0 + *h, t->nz);
  if (bx <= ax || bz <= az) return false;
  *x0 = ax, *z0 = az;
  *w = bx - ax, *h = bz - az;
  return true;
}

void cw_terrain_set_heights(cw_terrain *t, int x0, int z0, int w, int h, const uint16_t *q) {
  if (!t || !q) return;
  int stride = w, sx, sz;
  if (!clip_rect(t, &x0, &z0, &w, &h, &sx, &sz)) return;
  for (int j = 0; j < h; j++) memcpy(&t->h[tr_idx(t, x0, z0 + j)], &q[(sz + j) * stride + sx], sizeof(uint16_t) * (size_t)w);
  cw__terrain_mark(t, x0, z0, x0 + w - 1, z0 + h - 1, true, false);
}

/* Box3D's quantization of metres: (clamped - min) / step, truncated */
static uint16_t quantize(const cw_terrain *t, float m) {
  float lo = t->def.min_height, hi = t->def.max_height;
  float c = m < lo ? lo : m > hi ? hi : m;
  float s = (c - lo) / t->sy;
  return (uint16_t)(s > 65535.0f ? 65535.0f : s);
}

void cw_terrain_set_heights_m(cw_terrain *t, int x0, int z0, int w, int h, const float *m) {
  if (!t || !m) return;
  int stride = w, sx, sz;
  if (!clip_rect(t, &x0, &z0, &w, &h, &sx, &sz)) return;
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) t->h[tr_idx(t, x0 + i, z0 + j)] = quantize(t, m[(sz + j) * stride + sx + i]);
  cw__terrain_mark(t, x0, z0, x0 + w - 1, z0 + h - 1, true, false);
}

const uint16_t *cw_terrain_heights(const cw_terrain *t) { return t ? t->h : NULL; }
const uint32_t *cw_terrain_controls(const cw_terrain *t) { return t ? t->ctl : NULL; }

void cw_terrain_set_control(cw_terrain *t, int x0, int z0, int w, int h, const uint32_t *ctl) {
  if (!t || !ctl) return;
  int stride = w, sx, sz;
  if (!clip_rect(t, &x0, &z0, &w, &h, &sx, &sz)) return;
  for (int j = 0; j < h; j++) memcpy(&t->ctl[tr_idx(t, x0, z0 + j)], &ctl[(sz + j) * stride + sx], sizeof(uint32_t) * (size_t)w);
  /* holes are physics too: cw__terrain_mark marks the regions for a rebuild */
  cw__terrain_mark(t, x0, z0, x0 + w - 1, z0 + h - 1, false, true);
}

uint32_t cw_terrain_control(int base, int overlay, int blend, int uv_angle, int uv_scale, bool hole) {
  return ((uint32_t)(base & 31) << 27) | ((uint32_t)(overlay & 31) << 22) | ((uint32_t)(blend & 255) << 14) |
         ((uint32_t)(uv_angle & 15) << 10) | ((uint32_t)(uv_scale & 7) << 7) | ((uint32_t)hole << 2);
}

/* ── generation: fBm value noise from integer hashing (+ - * / only) ── */

static uint32_t hash2(int32_t x, int32_t z, uint32_t seed) {
  uint32_t h = seed * 0x9E3779B1u ^ (uint32_t)x * 0x85EBCA77u ^ (uint32_t)z * 0xC2B2AE3Du;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return h;
}
static float lattice(int32_t x, int32_t z, uint32_t seed) { return (float)(hash2(x, z, seed) >> 8) * (1.0f / 16777216.0f); }
static float value_noise(float x, float z, uint32_t seed) {
  float fx = floorf(x), fz = floorf(z);
  int32_t ix = (int32_t)fx, iz = (int32_t)fz;
  float u = x - fx, v = z - fz;
  u = u * u * (3 - 2 * u);
  v = v * v * (3 - 2 * v);
  float a = lattice(ix, iz, seed), b = lattice(ix + 1, iz, seed), c = lattice(ix, iz + 1, seed), d = lattice(ix + 1, iz + 1, seed);
  return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

typedef struct gen_ctx {
  cw_terrain *t;
  uint32_t seed;
  float inv_feature, amplitude;
  int octaves;
} gen_ctx;

static void gen_rows(int begin, int end, int worker, void *u) {
  (void)worker;
  gen_ctx *g = u;
  cw_terrain *t = g->t;
  float sp = t->def.vertex_spacing;
  for (int z = begin; z < end; z++)
    for (int x = 0; x < t->nx; x++) {
      float px = (float)x * sp * g->inv_feature, pz = (float)z * sp * g->inv_feature;
      /* a domain warp: valleys wander instead of lining up with the lattice */
      float wx = value_noise(px * 0.5f + 11.3f, pz * 0.5f, g->seed ^ 0xA5A5u) - 0.5f;
      float wz = value_noise(px * 0.5f, pz * 0.5f + 7.1f, g->seed ^ 0x5A5Au) - 0.5f;
      px += wx * 1.5f;
      pz += wz * 1.5f;
      float sum = 0, amp = 0.5f, norm = 0, f = 1;
      for (int o = 0; o < g->octaves; o++) {
        float n = value_noise(px * f, pz * f, g->seed + (uint32_t)o * 1013u);
        /* ridged from the third octave on: sharper crests */
        if (o >= 2) {
          float r = 1 - 2 * (n > 0.5f ? n - 0.5f : 0.5f - n);
          n = r * r;
        }
        sum += n * amp;
        norm += amp;
        amp *= 0.5f;
        f *= 2;
      }
      float v = sum / norm;
      v = v * v * (3 - 2 * v); /* flatter lowlands, steeper slopes */
      float s = v * g->amplitude * 65535.0f;
      t->h[tr_idx(t, x, z)] = (uint16_t)(s < 0 ? 0 : s > 65535.0f ? 65535.0f : s);
    }
}

void cw_terrain_generate(cw_terrain *t, uint32_t seed, float feature_m, int octaves, float amplitude) {
  if (!t) return;
  gen_ctx g = { t, seed, 1.0f / (feature_m > 0 ? feature_m : 200.0f), amplitude > 0 ? amplitude : 1,
                CW_CLAMP(octaves, 1, 12) };
  cw_parallel_for(t->nz, 8, gen_rows, &g);
  cw__terrain_mark(t, 0, 0, t->nx - 1, t->nz - 1, true, false);
}

/* ── import ─────────────────────────────────────────────────────────── */

static void resample16(const uint16_t *src, int sw, int sh, uint16_t *dst, int dw, int dh) {
  for (int z = 0; z < dh; z++) {
    float fz = dh > 1 ? (float)z * (float)(sh - 1) / (float)(dh - 1) : 0;
    int z0 = (int)fz, z1 = CW_MIN(z0 + 1, sh - 1);
    float v = fz - (float)z0;
    for (int x = 0; x < dw; x++) {
      float fx = dw > 1 ? (float)x * (float)(sw - 1) / (float)(dw - 1) : 0;
      int x0 = (int)fx, x1 = CW_MIN(x0 + 1, sw - 1);
      float u = fx - (float)x0;
      float a = src[z0 * sw + x0], b = src[z0 * sw + x1], c = src[z1 * sw + x0], d = src[z1 * sw + x1];
      float s = (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
      dst[z * dw + x] = (uint16_t)(s + 0.5f);
    }
  }
}

bool cw_terrain_import_heightmap(cw_terrain *t, const char *path) {
  if (!t || !path) return false;
  size_t len = 0;
  uint8_t *bytes = cw_vfs_read(path, &len);
  if (!bytes) {
    cw_error("terrain: cannot read heightmap %s", path);
    return false;
  }
  uint16_t *src = NULL;
  int w = 0, h = 0;
  bool own = false;
  size_t pl = strlen(path);
  if (pl > 4 && (!strcmp(path + pl - 4, ".r16") || !strcmp(path + pl - 4, ".raw"))) {
    int n = (int)(len / 2), s = (int)sqrt((double)n);
    while (s * s > n) s--;
    while ((s + 1) * (s + 1) <= n) s++;
    if (s * s != n || s < 2) {
      cw_error("terrain: %s is not a square r16 image (%zu bytes)", path, len);
      free(bytes);
      return false;
    }
    src = cw_alloc(sizeof(uint16_t) * (size_t)n, CW_MEM_RENDER);
    for (int i = 0; i < n; i++) src[i] = (uint16_t)(bytes[2 * i] | bytes[2 * i + 1] << 8);
    w = h = s;
    own = true;
  } else {
    int c;
    src = stbi_load_16_from_memory(bytes, (int)len, &w, &h, &c, 1);
    if (!src) {
      cw_error("terrain: %s is not a PNG heightmap", path);
      free(bytes);
      return false;
    }
  }
  free(bytes);
  if (w == t->nx && h == t->nz) memcpy(t->h, src, sizeof(uint16_t) * (size_t)w * h);
  else resample16(src, w, h, t->h, t->nx, t->nz);
  if (own) cw_free(src);
  else stbi_image_free(src);
  cw__terrain_mark(t, 0, 0, t->nx - 1, t->nz - 1, true, false);
  return true;
}

static uint8_t *load_rgba(const char *path, int *w, int *h) {
  size_t len = 0;
  uint8_t *bytes = cw_vfs_read(path, &len);
  if (!bytes) {
    cw_error("terrain: cannot read %s", path);
    return NULL;
  }
  int c;
  uint8_t *px = stbi_load_from_memory(bytes, (int)len, w, h, &c, 4);
  free(bytes);
  if (!px) cw_error("terrain: %s is not a PNG or JPEG image", path);
  return px;
}

bool cw_terrain_import_control(cw_terrain *t, const char *path) {
  if (!t || !path) return false;
  int w, h;
  uint8_t *px = load_rgba(path, &w, &h);
  if (!px) return false;
  for (int z = 0; z < t->nz; z++)
    for (int x = 0; x < t->nx; x++) {
      int sx = (int)((int64_t)x * w / t->nx), sz = (int)((int64_t)z * h / t->nz);
      const uint8_t *p = &px[4 * (sz * w + sx)];
      t->ctl[tr_idx(t, x, z)] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
  stbi_image_free(px);
  cw__terrain_mark(t, 0, 0, t->nx - 1, t->nz - 1, false, true);
  return true;
}

bool cw_terrain_import_splat(cw_terrain *t, const char *path, int first_layer) {
  if (!t || !path) return false;
  int w, h;
  uint8_t *px = load_rgba(path, &w, &h);
  if (!px) return false;
  for (int z = 0; z < t->nz; z++)
    for (int x = 0; x < t->nx; x++) {
      int sx = (int)((int64_t)x * w / t->nx), sz = (int)((int64_t)z * h / t->nz);
      const uint8_t *p = &px[4 * (sz * w + sx)];
      int a = 0, b = -1;
      for (int k = 1; k < 4; k++)
        if (p[k] > p[a]) a = k;
      for (int k = 0; k < 4; k++)
        if (k != a && (b < 0 || p[k] > p[b])) b = k;
      int wa = p[a], wb = p[b];
      int blend = wa + wb > 0 ? wb * 255 / (wa + wb) : 0;
      uint32_t *c = &t->ctl[tr_idx(t, x, z)];
      *c = (*c & 0x3FFFu) | cw_terrain_control(first_layer + a, first_layer + b, blend, 0, 0, false);
    }
  stbi_image_free(px);
  cw__terrain_mark(t, 0, 0, t->nx - 1, t->nz - 1, false, true);
  return true;
}

/* ── layers ─────────────────────────────────────────────────────────── */

static uint8_t *resize_tile(const uint8_t *src, int w, int h) {
  uint8_t *dst = cw_alloc((size_t)TR_TILE * TR_TILE * 4, CW_MEM_RENDER);
  for (int y = 0; y < TR_TILE; y++)
    for (int x = 0; x < TR_TILE; x++) {
      /* bilinear, wrapping (the layers repeat) */
      float fx = ((float)x + 0.5f) * (float)w / TR_TILE - 0.5f, fy = ((float)y + 0.5f) * (float)h / TR_TILE - 0.5f;
      int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
      float u = fx - (float)x0, v = fy - (float)y0;
      int xa = ((x0 % w) + w) % w, xb = (xa + 1) % w, ya = ((y0 % h) + h) % h, yb = (ya + 1) % h;
      for (int k = 0; k < 4; k++) {
        float a = src[4 * (ya * w + xa) + k], b = src[4 * (ya * w + xb) + k], c = src[4 * (yb * w + xa) + k],
              d = src[4 * (yb * w + xb) + k];
        float s = (a + (b - a) * u) * (1 - v) + (c + (d - c) * u) * v;
        dst[4 * (y * TR_TILE + x) + k] = (uint8_t)(s + 0.5f);
      }
    }
  return dst;
}

static float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

void cw_terrain_set_layer_pixels(cw_terrain *t, int i, const uint8_t *albedo, const uint8_t *normal, int w, int h,
                                 float uv_scale) {
  if (!t || i < 0 || i >= CW_TERRAIN_LAYERS) return;
  if (t->layers_shared) {
    cw_error("terrain: cw_terrain_set_layer on a terrain that shares another's layers: set them on that one");
    return;
  }
  cw_free(t->layer[i].albedo);
  cw_free(t->layer[i].normal);
  t->layer[i].albedo = albedo ? resize_tile(albedo, w, h) : NULL;
  t->layer[i].normal = normal ? resize_tile(normal, w, h) : NULL;
  t->layer[i].uv_scale = uv_scale > 0 ? uv_scale : 0.25f;
  t->layer[i].set = true;
  double s[3] = { 0, 0, 0 };
  if (t->layer[i].albedo) {
    for (int p = 0; p < TR_TILE * TR_TILE; p++)
      for (int k = 0; k < 3; k++) s[k] += srgb_to_linear(t->layer[i].albedo[4 * p + k] / 255.0f);
    for (int k = 0; k < 3; k++) t->layer[i].mean[k] = (float)(s[k] / (TR_TILE * TR_TILE));
  } else {
    for (int k = 0; k < 3; k++) t->layer[i].mean[k] = srgb_to_linear(0.5f);
  }
  t->layers_dirty = t->macro_dirty = true;
}

void cw_terrain_share_layers(cw_terrain *t, cw_terrain *src) {
  if (!t || !src || t == src) return;
  for (int i = 0; i < CW_TERRAIN_LAYERS; i++) {
    cw_free(t->layer[i].albedo);
    cw_free(t->layer[i].normal);
    t->layer[i].albedo = t->layer[i].normal = NULL; /* the pixels stay with src */
    t->layer[i].uv_scale = src->layer[i].uv_scale;
    t->layer[i].set = src->layer[i].set;
    memcpy(t->layer[i].mean, src->layer[i].mean, sizeof t->layer[i].mean);
  }
  cw__terrain_gpu_share_layers(t, src);
  t->layers_shared = true;
  t->layers_dirty = false;
  t->macro_dirty = true;
}

bool cw_terrain_set_layer(cw_terrain *t, int i, const char *albedo_height, const char *normal_rough, float uv_scale) {
  if (!t || i < 0 || i >= CW_TERRAIN_LAYERS) return false;
  int aw = 0, ah = 0, nw = 0, nh = 0;
  uint8_t *a = albedo_height && *albedo_height ? load_rgba(albedo_height, &aw, &ah) : NULL;
  uint8_t *n = normal_rough && *normal_rough ? load_rgba(normal_rough, &nw, &nh) : NULL;
  bool ok = (!albedo_height || !*albedo_height || a) && (!normal_rough || !*normal_rough || n);
  /* the two images may differ in size: each is resized to the tile on its own */
  cw_free(t->layer[i].albedo);
  cw_free(t->layer[i].normal);
  t->layer[i].albedo = t->layer[i].normal = NULL;
  cw_terrain_set_layer_pixels(t, i, a, NULL, aw, ah, uv_scale);
  if (n) t->layer[i].normal = resize_tile(n, nw, nh);
  if (a) stbi_image_free(a);
  if (n) stbi_image_free(n);
  return ok;
}

/* ── queries ────────────────────────────────────────────────────────── */

/* the height of the triangles at sample-space (fx, fz), from the samples (Box3D's split) */
float cw__terrain_height_fast(const cw_terrain *t, float fx, float fz) {
  fx = cw_clampf(fx, 0, (float)(t->nx - 1));
  fz = cw_clampf(fz, 0, (float)(t->nz - 1));
  int x = CW_MIN((int)fx, t->nx - 2), z = CW_MIN((int)fz, t->nz - 2);
  float u = fx - (float)x, v = fz - (float)z;
  float h00 = t->h[tr_idx(t, x, z)], h10 = t->h[tr_idx(t, x + 1, z)], h01 = t->h[tr_idx(t, x, z + 1)],
        h11 = t->h[tr_idx(t, x + 1, z + 1)];
  float q = u + v <= 1 ? h00 + u * (h10 - h00) + v * (h01 - h00) : h11 + (1 - u) * (h01 - h11) + (1 - v) * (h10 - h11);
  return t->def.origin.y + t->def.min_height + q * t->sy;
}

t3_vec3 cw__terrain_normal_fast(const cw_terrain *t, float fx, float fz) {
  fx = cw_clampf(fx, 0, (float)(t->nx - 1));
  fz = cw_clampf(fz, 0, (float)(t->nz - 1));
  int x = CW_MIN((int)fx, t->nx - 2), z = CW_MIN((int)fz, t->nz - 2);
  float u = fx - (float)x, v = fz - (float)z;
  float sp = t->def.vertex_spacing;
  float h00 = t->h[tr_idx(t, x, z)] * t->sy, h10 = t->h[tr_idx(t, x + 1, z)] * t->sy,
        h01 = t->h[tr_idx(t, x, z + 1)] * t->sy, h11 = t->h[tr_idx(t, x + 1, z + 1)] * t->sy;
  float dx, dz;
  if (u + v <= 1) dx = h10 - h00, dz = h01 - h00;
  else dx = h11 - h01, dz = h11 - h10;
  float nx = -dx, ny = sp, nz = -dz;
  float l = sqrtf(nx * nx + ny * ny + nz * nz);
  return (t3_vec3){ nx / l, ny / l, nz / l };
}

/* rebuild dirty region fields on the job pool; the main thread adopts them */
typedef struct region_job {
  cw_terrain *t;
  int *list;
  b3HeightFieldData **out;
} region_job;

static void build_region(int begin, int end, int worker, void *u) {
  (void)worker;
  region_job *j = u;
  cw_terrain *t = j->t;
  int R = t->def.region_size;
  for (int k = begin; k < end; k++) {
    int ri = j->list[k], rx = ri % t->rnx, rz = ri / t->rnx;
    int x0 = rx * R, z0 = rz * R;
    int cx = CW_MIN(R, t->nx - 1 - x0), cz = CW_MIN(R, t->nz - 1 - z0); /* cells */
    int sx = cx + 1, sz = cz + 1;
    uint16_t *q = cw_alloc(sizeof(uint16_t) * (size_t)sx * sz, CW_MEM_PHYSICS);
    uint8_t *mat = cw_alloc((size_t)cx * cz, CW_MEM_PHYSICS);
    bool holes = false;
    for (int z = 0; z < sz; z++) memcpy(&q[z * sx], &t->h[tr_idx(t, x0, z0 + z)], sizeof(uint16_t) * (size_t)sx);
    for (int z = 0; z < cz; z++)
      for (int x = 0; x < cx; x++) {
        bool hole = (t->ctl[tr_idx(t, x0 + x, z0 + z)] >> 2) & 1;
        mat[z * cx + x] = hole ? 0xFF : 0;
        holes |= hole;
      }
    float sp = t->def.vertex_spacing;
    j->out[k] = cw__heightfield3d_build(q, sx, sz, (t3_vec3){ sp, t->sy, sp }, holes ? mat : NULL);
    cw_free(q);
    cw_free(mat);
  }
}

static void make_body(cw_terrain *t, int ri) {
  tr_region *r = &t->regions[ri];
  if (!t->physics || !cw_world3d_valid(t->world)) return;
  if (!cw_body3d_valid(r->body)) {
    cw_body3d_def bd = cw_body3d_default_def();
    bd.type = CW_BODY_STATIC;
    bd.position = region_pos(t, ri % t->rnx, ri / t->rnx);
    bd.name = "terrain";
    bd.interpolate = false;
    r->body = cw_body3d_create(t->world, &bd);
  }
  if (cw_shape3d_valid(r->shape)) cw_shape3d_destroy(r->shape);
  r->shape = cw_shape3d_heightfield(r->body, &t->shape_def, r->field);
}

void cw_terrain_sync_physics(cw_terrain *t) {
  if (!t) return;
  int n = t->rnx * t->rnz, cnt = 0;
  int *list = cw_alloc(sizeof(int) * (size_t)n, CW_MEM_RENDER);
  for (int i = 0; i < n; i++)
    if (t->regions[i].dirty || !t->regions[i].field.id) list[cnt++] = i;
  if (cnt) {
    b3HeightFieldData **out = cw_calloc((size_t)cnt, sizeof *out, CW_MEM_RENDER);
    region_job j = { t, list, out };
    cw_parallel_for(cnt, 1, build_region, &j);
    for (int k = 0; k < cnt; k++) {
      tr_region *r = &t->regions[list[k]];
      /* the shape goes before the field it points at */
      if (cw_shape3d_valid(r->shape)) cw_shape3d_destroy(r->shape);
      r->shape = (cw_shape3d){ 0 };
      cw_heightfield3d_destroy(r->field);
      r->field = cw__heightfield3d_adopt(out[k]);
      r->dirty = false;
      make_body(t, list[k]);
    }
    cw_free(out);
  }
  cw_free(list);
}

static void sync_if_dirty(const cw_terrain *t) {
  /* a background build owns the fields until it is adopted: queries use the samples meanwhile */
  if (t->build) return;
  for (int i = 0; i < t->rnx * t->rnz; i++)
    if (t->regions[i].dirty || !t->regions[i].field.id) {
      cw_terrain_sync_physics((cw_terrain *)t);
      return;
    }
}

void cw_terrain_attach_physics(cw_terrain *t, cw_world3d w, const cw_shape3d_def *def) {
  if (!t) return;
  cw__terrain_build_finish(t, true);
  cw_terrain_detach_physics(t);
  t->world = w;
  t->shape_def = def ? *def : cw_shape3d_default_def();
  t->physics = true;
  sync_if_dirty(t);
  for (int i = 0; i < t->rnx * t->rnz; i++) make_body(t, i);
}

void cw_terrain_detach_physics(cw_terrain *t) {
  if (!t || !t->physics) return;
  for (int i = 0; i < t->rnx * t->rnz; i++) {
    tr_region *r = &t->regions[i];
    if (cw_body3d_valid(r->body)) cw_body3d_destroy(r->body);
    r->body = (cw_body3d){ 0 };
    r->shape = (cw_shape3d){ 0 };
  }
  t->physics = false;
  t->world = (cw_world3d){ 0 };
}

cw_shape3d cw_terrain_region_shape(const cw_terrain *t, int rx, int rz) {
  if (!t || rx < 0 || rz < 0 || rx >= t->rnx || rz >= t->rnz) return (cw_shape3d){ 0 };
  return t->regions[rz * t->rnx + rx].shape;
}

static void clamp_xz(const cw_terrain *t, float *x, float *z) {
  float x1 = t->def.origin.x + (float)t->def.size_x * t->def.vertex_spacing;
  float z1 = t->def.origin.z + (float)t->def.size_z * t->def.vertex_spacing;
  *x = cw_clampf(*x, t->def.origin.x, x1);
  *z = cw_clampf(*z, t->def.origin.z, z1);
}

static int region_of(const cw_terrain *t, float x, float z) {
  float span = (float)t->def.region_size * t->def.vertex_spacing;
  int rx = (int)floorf((x - t->def.origin.x) / span), rz = (int)floorf((z - t->def.origin.z) / span);
  rx = CW_CLAMP(rx, 0, t->rnx - 1);
  rz = CW_CLAMP(rz, 0, t->rnz - 1);
  return rz * t->rnx + rx;
}

/* a world ray against one region's field, with the arithmetic of Box3D's
 * world ray cast (b3World_CastRay: shape transform relative to the ray's
 * origin, the hit offset back by the origin) */
static bool region_cast(const cw_terrain *t, int ri, t3_vec3 o, t3_vec3 tr, float *fraction, t3_vec3 *point,
                        t3_vec3 *normal) {
  const b3HeightFieldData *f = cw_heightfield3d_raw(t->regions[ri].field);
  if (!f) return false;
  t3_vec3 bp = region_pos(t, ri % t->rnx, ri / t->rnx);
  b3Vec3 rel = { bp.x - o.x, bp.y - o.y, bp.z - o.z };
  b3RayCastInput in = { 0 };
  in.origin = (b3Vec3){ -rel.x, -rel.y, -rel.z };
  in.translation = (b3Vec3){ tr.x, tr.y, tr.z };
  in.maxFraction = 1;
  b3CastOutput out = b3RayCastHeightField(f, &in);
  if (!out.hit) return false;
  *fraction = out.fraction;
  *point = (t3_vec3){ o.x + (out.point.x + rel.x), o.y + (out.point.y + rel.y), o.z + (out.point.z + rel.z) };
  *normal = (t3_vec3){ out.normal.x, out.normal.y, out.normal.z };
  return true;
}

float cw_terrain_height_at(const cw_terrain *t, float x, float z) {
  if (!t) return 0;
  sync_if_dirty(t);
  clamp_xz(t, &x, &z);
  /* straight down from above the highest possible sample to below the lowest */
  float top = t->def.origin.y + t->def.max_height + 1.0f;
  float len = t->def.max_height - t->def.min_height + 2.0f;
  t3_vec3 o = { x, top, z }, p, n;
  float f;
  if (region_cast(t, region_of(t, x, z), o, (t3_vec3){ 0, -len, 0 }, &f, &p, &n)) return p.y;
  /* a hole (or an edge the ray grazes): the triangles' height from the samples */
  float sp = t->def.vertex_spacing;
  return cw__terrain_height_fast(t, (x - t->def.origin.x) / sp, (z - t->def.origin.z) / sp);
}

t3_vec3 cw_terrain_normal_at(const cw_terrain *t, float x, float z) {
  if (!t) return (t3_vec3){ 0, 1, 0 };
  clamp_xz(t, &x, &z);
  float sp = t->def.vertex_spacing;
  return cw__terrain_normal_fast(t, (x - t->def.origin.x) / sp, (z - t->def.origin.z) / sp);
}

bool cw_terrain_is_hole(const cw_terrain *t, float x, float z) {
  if (!t) return false;
  clamp_xz(t, &x, &z);
  float sp = t->def.vertex_spacing;
  int ix = (int)floorf((x - t->def.origin.x) / sp + 0.5f), iz = (int)floorf((z - t->def.origin.z) / sp + 0.5f);
  ix = CW_CLAMP(ix, 0, t->nx - 1);
  iz = CW_CLAMP(iz, 0, t->nz - 1);
  return (t->ctl[tr_idx(t, ix, iz)] >> 2) & 1;
}

bool cw_terrain_raycast(const cw_terrain *t, t3_vec3 o, t3_vec3 d, float maxd, cw_terrain_hit *out) {
  if (!t || !(maxd > 0)) return false;
  sync_if_dirty(t);
  t3_vec3 tr = { d.x * maxd, d.y * maxd, d.z * maxd };
  /* the regions whose box the ray's box overlaps, in index order; the nearest hit wins */
  float span = (float)t->def.region_size * t->def.vertex_spacing;
  float ax = CW_MIN(o.x, o.x + tr.x), bx = CW_MAX(o.x, o.x + tr.x);
  float az = CW_MIN(o.z, o.z + tr.z), bz = CW_MAX(o.z, o.z + tr.z);
  int rx0 = CW_MAX(0, (int)floorf((ax - t->def.origin.x) / span)), rx1 = CW_MIN(t->rnx - 1, (int)floorf((bx - t->def.origin.x) / span));
  int rz0 = CW_MAX(0, (int)floorf((az - t->def.origin.z) / span)), rz1 = CW_MIN(t->rnz - 1, (int)floorf((bz - t->def.origin.z) / span));
  float best = 2;
  t3_vec3 bp = { 0 }, bn = { 0 };
  for (int rz = rz0; rz <= rz1; rz++)
    for (int rx = rx0; rx <= rx1; rx++) {
      float f;
      t3_vec3 p, n;
      if (region_cast(t, rz * t->rnx + rx, o, tr, &f, &p, &n) && f < best) best = f, bp = p, bn = n;
    }
  if (best > 1) return false;
  if (out) {
    out->point = bp;
    out->normal = bn;
    out->distance = best * maxd;
    float sp = t->def.vertex_spacing;
    int ix = CW_CLAMP((int)floorf((bp.x - t->def.origin.x) / sp + 0.5f), 0, t->nx - 1);
    int iz = CW_CLAMP((int)floorf((bp.z - t->def.origin.z) / sp + 0.5f), 0, t->nz - 1);
    out->control = t->ctl[tr_idx(t, ix, iz)];
  }
  return true;
}

void cw_terrain_set_quality(cw_terrain *t, int quality) {
  if (t) t->quality = quality < 0 ? -1 : CW_MIN(quality, CW_QUALITY_COUNT - 1);
}

void cw_terrain_get_stats(const cw_terrain *t, cw_terrain_stats *out) {
  if (!out) return;
  if (!t) {
    memset(out, 0, sizeof *out);
    return;
  }
  *out = t->stats;
}

/* ── a background build (terrain sets: a streamed cell's terrain) ─────────
 * Everything a new terrain computes before its first draw, on one job: the
 * height fields, the GPU textures' texels (heights, normals, control, macro)
 * and the quadtree's min / max pyramid. The main thread must leave the
 * terrain alone (no update, no edits) until cw__terrain_build_finish says it
 * is done; reading its heights meanwhile is fine. */
typedef struct tr_build {
  cw_job *job;
  int n;
  int *list;
  b3HeightFieldData **out;
  double ms;
} tr_build;

static void build_job(void *u) {
  cw_terrain *t = u;
  tr_build *b = t->build;
  double t0 = tr_now_ms();
  region_job j = { t, b->list, b->out };
  build_region(0, b->n, 0, &j);
  cw__terrain_gpu_fill(t);
  cw__terrain_tree_rebuild(t);
  b->ms = tr_now_ms() - t0;
}

void cw__terrain_build_start(cw_terrain *t) {
  if (t->build) return;
  cw__terrain_gpu_prepare(t); /* the textures and their texel arrays (main thread) */
  tr_build *b = cw_calloc(1, sizeof *b, CW_MEM_RENDER);
  int n = t->rnx * t->rnz;
  b->list = cw_alloc(sizeof(int) * (size_t)n, CW_MEM_RENDER);
  b->out = cw_calloc((size_t)n, sizeof *b->out, CW_MEM_RENDER);
  for (int i = 0; i < n; i++) b->list[b->n++] = i;
  t->build = b;
  b->job = cw_job_background(build_job, t);
}

bool cw__terrain_build_finish(cw_terrain *t, bool wait) {
  tr_build *b = t->build;
  if (!b) return true;
  if (!wait && !cw_job_done(b->job)) return false;
  cw_job_wait(b->job);
  for (int k = 0; k < b->n; k++) {
    tr_region *r = &t->regions[b->list[k]];
    if (cw_shape3d_valid(r->shape)) cw_shape3d_destroy(r->shape);
    r->shape = (cw_shape3d){ 0 };
    cw_heightfield3d_destroy(r->field);
    r->field = cw__heightfield3d_adopt(b->out[k]);
    r->dirty = false;
    make_body(t, b->list[k]);
  }
  cw__terrain_gpu_filled(t); /* texture versions up, the edits marked done */
  t->build_ms = (float)b->ms;
  cw_free(b->list);
  cw_free(b->out);
  cw_free(b);
  t->build = NULL;
  return true;
}
