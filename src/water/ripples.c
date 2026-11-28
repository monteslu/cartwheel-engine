/* Water3D's interactive ripple layer (cartwheel/water.h): a height field on a
 * grid over the water, stepped on the fixed clock, that splashes, wakes and
 * rain write into. The drawn water reads it as a half-float texture (red:
 * the ripple's height in metres, green: foam), so GL and WebGPU draw the same
 * thing and no compute is needed.
 *
 * A second, fine layer (collar_cells over a square collar_size wide, 4 cm cells by default) holds the collars,
 * the rings of bubbles where something stands through the surface (cw_water_collar): the grid's cells are far wider
 * than a leg. It is foam only (no wave equation), kept around the grid's follow point or cw_water_collar_follow's.
 *
 * The simulation is + - * and integer hashes only, in a fixed order, so every
 * host steps the same bits. The ripple height is for looks: bodies float on
 * the Gerstner waves alone (cw_water_height_at), so gameplay never depends on
 * the grid's resolution or the quality preset. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/math.h"
#include "cartwheel/particles.h"
#include "cartwheel/render3d.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/water.h"
#include "water_internal.h"
#include "render/quality_internal.h"

#define SPONGE 6

static uint16_t to_half_signed(float f) {
  uint32_t x;
  memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000u, mant = x & 0x7fffffu;
  int e = (int)((x >> 23) & 0xffu) - 127 + 15;
  if (e >= 31) return (uint16_t)(sign | 0x7bffu);
  if (e <= 0) {
    if (e < -10) return (uint16_t)sign;
    mant |= 0x800000u;
    return (uint16_t)(sign | (mant >> (14 - e)));
  }
  uint32_t h = ((uint32_t)e << 10) | (mant >> 13), rem = mant & 0x1fffu;
  if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) h++;
  return (uint16_t)(sign | h);
}

/* the grid's size: cells along the longer side of the ripple area, the quality
 * scaling them (Low a half) */
static bool dims(const water3d *d, int *nx, int *nz, float *ax, float *az) {
  int n = d->ripple_cells;
  if (n < 4) return false;
  int q = cw__quality_effects(); /* src/render/quality.c */
  if (q <= CW_QUALITY_LOW) n /= 2; /* (Very Low as Low: a coarser grid loses the wakes, the ripples' point there) */
  else if (q == CW_QUALITY_MEDIUM) n = n * 3 / 4;
  if (n < 8) n = 8;
  if (n > 512) n = 512;
  float sx = d->ripple_size[0] > 0.01f ? d->ripple_size[0] : d->size[0];
  float sz = d->ripple_size[1] > 0.01f ? d->ripple_size[1] : d->size[1];
  if (sx < 0.01f) sx = 0.01f;
  if (sz < 0.01f) sz = 0.01f;
  *nx = sx >= sz ? n : (int)ceilf((float)n * sx / sz);
  *nz = sz >= sx ? n : (int)ceilf((float)n * sz / sx);
  if (*nx < 4) *nx = 4;
  if (*nz < 4) *nz = 4;
  *ax = sx, *az = sz;
  return true;
}

static void fine_free(water3d *d) {
  struct water_fine *g = &d->fine;
  cw_free(g->c);
  if (g->tex) t3_release(g->tex);
  memset(g, 0, sizeof *g);
}

static void coarse_free(water3d *d) {
  cw_free(d->rip_h);
  cw_free(d->rip_v);
  cw_free(d->rip_f);
  if (d->rip_tex) t3_release(d->rip_tex);
  d->rip_h = d->rip_v = d->rip_f = NULL;
  d->rip_tex = NULL;
  d->rip_nx = d->rip_nz = 0;
}

void cw__ripples_free(water3d *d) {
  fine_free(d);
  coarse_free(d);
}

static void alloc_grid(water3d *d, int nx, int nz, float ax, float az) {
  coarse_free(d);
  size_t n = (size_t)nx * (size_t)nz;
  d->rip_h = cw_calloc(n, sizeof(float), CW_MEM_RENDER);
  d->rip_v = cw_calloc(n, sizeof(float), CW_MEM_RENDER);
  d->rip_f = cw_calloc(n, sizeof(float), CW_MEM_RENDER);
  d->rip_nx = nx, d->rip_nz = nz;
  d->rip_ax = ax, d->rip_az = az;
  d->rip_awake = true; /* the first upload writes the empty field */
  d->rip_upload = true;
  d->rip_cells_built = d->ripple_cells;
  d->rip_quality = cw__quality_effects();
  d->rip_tex = t3_data_texture_new_typed(nx, nz, NULL, T3_HALF_FLOAT_TYPE);
  d->rip_tex->mag_filter = T3_LINEAR;
  d->rip_tex->min_filter = T3_LINEAR;
  d->rip_tex->wrap_s = d->rip_tex->wrap_t = T3_CLAMP_TO_EDGE;
}

/* world (x, z) to the grid's continuous cell coordinates (false: off the grid) */
static bool to_cell(cw_node n, const water3d *d, float x, float z, float *cx, float *cz) {
  t3_mat4 inv;
  t3_mat4_invert(&inv, cw_node3d_global_transform(n));
  t3_vec3 p = t3_vec3_apply_mat4((t3_vec3){ x, 0, z }, &inv);
  float u = (p.x - d->ripple_center[0]) / d->rip_ax + 0.5f, v = (p.z - d->ripple_center[1]) / d->rip_az + 0.5f;
  *cx = u * (float)d->rip_nx - 0.5f;
  *cz = v * (float)d->rip_nz - 0.5f;
  return true;
}

/* a smooth round dent in a field of nx x nz cells of cwx x cwz metres: depth metres at the centre, radius metres,
 * foam 0..1 (false: nothing of it on the field) */
static bool stamp_field(float *h, float *fo, int nx, int nz, float cwx, float cwz, float cx, float cz, float radius, float depth, float foam) {
  float rx = radius / cwx, rz = radius / cwz;
  int i0 = (int)floorf(cx - rx), i1 = (int)ceilf(cx + rx), j0 = (int)floorf(cz - rz), j1 = (int)ceilf(cz + rz);
  if (i1 < 0 || j1 < 0 || i0 >= nx || j0 >= nz) return false;
  if (i0 < 1) i0 = 1;
  if (j0 < 1) j0 = 1;
  if (i1 > nx - 2) i1 = nx - 2;
  if (j1 > nz - 2) j1 = nz - 2;
  for (int j = j0; j <= j1; j++)
    for (int i = i0; i <= i1; i++) {
      float dx = ((float)i - cx) / rx, dz = ((float)j - cz) / rz;
      float q = dx * dx + dz * dz;
      if (q >= 1.0f) continue;
      float w = (1.0f - q) * (1.0f - q);
      size_t k = (size_t)j * (size_t)nx + (size_t)i;
      h[k] -= depth * w;
      float f = foam * w;
      if (f > fo[k]) fo[k] = f;
    }
  return true;
}
static void stamp(water3d *d, float cx, float cz, float radius, float depth, float foam) {
  if (!d->rip_h) return;
  if (stamp_field(d->rip_h, d->rip_f, d->rip_nx, d->rip_nz, d->rip_ax / (float)d->rip_nx, d->rip_az / (float)d->rip_nz, cx, cz, radius, depth, foam))
    d->rip_awake = true;
}

/* world (x, z) to water-local (x, z) */
static void to_local(cw_node n, float x, float z, float *lx, float *lz) {
  t3_mat4 inv;
  t3_mat4_invert(&inv, cw_node3d_global_transform(n));
  t3_vec3 p = t3_vec3_apply_mat4((t3_vec3){ x, 0, z }, &inv);
  *lx = p.x, *lz = p.z;
}
/* the fine layer's cell coordinates of water-local (x, z) */
static void fine_cell(const struct water_fine *g, float lx, float lz, float *cx, float *cz) {
  *cx = (lx - g->cx) / g->cell + 0.5f * (float)g->n - 0.5f;
  *cz = (lz - g->cz) / g->cell + 0.5f * (float)g->n - 0.5f;
}

/* a pcg-style integer hash, then a float in [0, 1) */
static uint32_t hash(uint32_t v) {
  v = v * 747796405u + 2891336453u;
  uint32_t w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
  return (w >> 22u) ^ w;
}
static float unit(uint32_t h) { return (float)(h >> 8) * (1.0f / 16777216.0f); }

/* the pool of splash particle emitters (the children of splash_fx): one is
 * moved to the splash and restarted */
static void splash_fx(cw_node n, water3d *d, float x, float y, float z) {
  if (!d->splash_fx || !*d->splash_fx) return;
  cw_node root = cw_node_get(n, d->splash_fx);
  if (!cw_node_valid(root)) return;
  int kids = cw_node_child_count(root);
  cw_node e = root;
  if (kids > 0) e = cw_node_child(root, (int)(d->fx_next++ % (uint32_t)kids));
  if (!cw_node_is(e, "GPUParticles3D")) return;
  cw_node3d_set_position(e, (t3_vec3){ x, y, z });
  cw_particles_restart(e);
  cw_node_set(e, "emitting", cw_val_bool(true));
}

void cw_water_splash(cw_node water, float x, float z, float radius, float depth, float foam) {
  water3d *d = cw__water_data(water);
  if (!d || !d->rip_h) return;
  float cx, cz;
  to_cell(water, d, x, z, &cx, &cz);
  stamp(d, cx, cz, radius > 0.05f ? radius : 0.05f, depth, foam);
  /* the particles: only a splash that is a splash */
  if (depth >= d->splash_fx_min) {
    cw_water_sample s;
    double t = cw_water_time(water);
    cw_water_sample_at(water, x, z, t, &s);
    splash_fx(water, d, x, s.height, z);
  }
}

void cw_water_wake(cw_node water, float x, float z, float vx, float vz, float radius, float depth, float foam) {
  water3d *d = cw__water_data(water);
  if (!d || !d->rip_h) return;
  float dt = (float)cw_fixed_dt();
  float cx, cz;
  /* two dents along the path the body swept this tick, so a fast body leaves a continuous trough */
  for (int k = 0; k < 2; k++) {
    float back = 0.5f * (float)k * dt;
    to_cell(water, d, x - vx * back, z - vz * back, &cx, &cz);
    stamp(d, cx, cz, radius > 0.05f ? radius : 0.05f, depth, foam);
  }
}

void cw_water_collar(cw_node water, float x, float z, float radius, float vx, float vz, float foam) {
  water3d *d = cw__water_data(water);
  if (!d || !d->fine.c) return;
  struct water_fine *g = &d->fine;
  if (radius < 0.005f) radius = 0.005f;
  float lx, lz, cx, cz;
  to_local(water, x, z, &lx, &lz);
  fine_cell(g, lx, lz, &cx, &cz);
  /* the ring: from the object's side out to a width that grows with its size (a leg 7 cm, a hull 13 to 20 cm) and with
   * its speed; its bubbles thin round a still object (a meniscus, a few per cent brighter) and thick round a moving one,
   * thickest ahead of it. Foam only: the ring adds no water (held at a height each tick it would pour water in, and the
   * swell round a held leg would rise without end); a moving body's bow wave and trough are its wake's */
  float sp = sqrtf(vx * vx + vz * vz), ux = sp > 0.01f ? vx / sp : 0, uz = sp > 0.01f ? vz / sp : 0;
  float width = 0.06f + 0.12f * radius + 0.05f * (sp < 4.0f ? sp : 4.0f);
  float fo = foam * (0.45f + 0.55f * (sp < 2.0f ? sp * 0.5f : 1.0f));
  float r1 = (radius + width) / g->cell;
  int i0 = (int)floorf(cx - r1), i1 = (int)ceilf(cx + r1), j0 = (int)floorf(cz - r1), j1 = (int)ceilf(cz + r1);
  if (i1 < 1 || j1 < 1 || i0 > g->n - 2 || j0 > g->n - 2) return;
  if (i0 < 1) i0 = 1;
  if (j0 < 1) j0 = 1;
  if (i1 > g->n - 2) i1 = g->n - 2;
  if (j1 > g->n - 2) j1 = g->n - 2;
  for (int j = j0; j <= j1; j++)
    for (int i = i0; i <= i1; i++) {
      float dx = ((float)i - cx) * g->cell, dz = ((float)j - cz) * g->cell;
      float r = sqrtf(dx * dx + dz * dz);
      if (r >= radius + width) continue;
      /* across the ring: 0 at the object's side, 1 at its outer edge; the bump peaks a third of the way out */
      float t = r < radius ? 0.0f : (r - radius) / width;
      float w = t < 0.33f ? 0.4f + 0.6f * (t / 0.33f) : 1.0f - (t - 0.33f) / 0.67f;
      w = w * w * (3.0f - 2.0f * w);
      float ahead = r > 0.001f ? (dx * ux + dz * uz) / r : 0; /* 1 ahead, -1 behind */
      size_t k = (size_t)j * (size_t)g->n + (size_t)i;
      float ff = fo * w * (1.0f + 0.3f * ahead * (sp < 2.0f ? sp * 0.5f : 1.0f));
      if (ff > g->c[k]) g->c[k] = ff;
    }
  g->awake = true;
}

float cw_water_ripple_at(cw_node water, float x, float z) {
  water3d *d = cw__water_data(water);
  if (!d || !d->rip_h) return 0;
  float cx, cz;
  to_cell(water, d, x, z, &cx, &cz);
  int i = (int)floorf(cx), j = (int)floorf(cz);
  float fx = cx - (float)i, fz = cz - (float)j;
  if (i < 0 || j < 0 || i >= d->rip_nx - 1 || j >= d->rip_nz - 1) return 0;
  const float *h = d->rip_h + (size_t)j * (size_t)d->rip_nx + (size_t)i;
  int w = d->rip_nx;
  float a = h[0] + (h[1] - h[0]) * fx, b = h[w] + (h[w + 1] - h[w]) * fx;
  return a + (b - a) * fz;
}

uint32_t cw_water_ripple_hash(cw_node water) {
  water3d *d = cw__water_data(water);
  if (!d || !d->rip_h) return 0;
  size_t n = (size_t)d->rip_nx * (size_t)d->rip_nz;
  uint32_t h = CW_HASH32_INIT;
  h = cw_hash32(d->rip_h, n * sizeof(float), h);
  h = cw_hash32(d->rip_f, n * sizeof(float), h);
  if (d->fine.c) {
    size_t m = (size_t)d->fine.n * (size_t)d->fine.n;
    h = cw_hash32(d->fine.c, m * sizeof(float), h);
  }
  return h;
}

/* shift a field by (sx, sz) whole cells: the cell that was at (i + sx, j + sz) is now at (i, j), cells coming in flat */
static void shift_field(float *f, int nx, int nz, int sx, int sz) {
  if (abs(sx) >= nx || abs(sz) >= nz) {
    memset(f, 0, (size_t)nx * (size_t)nz * sizeof(float));
    return;
  }
  int cols = nx - abs(sx);
  /* rows walked so nothing is read after it is written */
  for (int jj = 0; jj < nz; jj++) {
    int j = sz >= 0 ? jj : nz - 1 - jj;
    float *dst = f + (size_t)j * (size_t)nx;
    int src_j = j + sz;
    if (src_j < 0 || src_j >= nz) {
      memset(dst, 0, (size_t)nx * sizeof(float));
      continue;
    }
    const float *src = f + (size_t)src_j * (size_t)nx;
    if (sx >= 0) {
      memmove(dst, src + sx, (size_t)cols * sizeof(float));
      memset(dst + cols, 0, (size_t)(nx - cols) * sizeof(float));
    } else {
      memmove(dst - sx, src, (size_t)cols * sizeof(float));
      memset(dst, 0, (size_t)(nx - cols) * sizeof(float));
    }
  }
}

static void fine_follow(water3d *d, float lx, float lz) {
  struct water_fine *g = &d->fine;
  if (!g->c) return;
  int sx = (int)floorf((lx - g->cx) / g->cell + 0.5f), sz = (int)floorf((lz - g->cz) / g->cell + 0.5f);
  if (!sx && !sz) return;
  shift_field(g->c, g->n, g->n, sx, sz);
  g->cx += (float)sx * g->cell, g->cz += (float)sz * g->cell;
  g->upload = true;
}

void cw_water_collar_follow(cw_node water, float x, float z) {
  water3d *d = cw__water_data(water);
  if (!d || !d->fine.c) return;
  float lx, lz;
  to_local(water, x, z, &lx, &lz);
  fine_follow(d, lx, lz);
  d->fine.own = true;
}

void cw_water_ripple_follow(cw_node water, float x, float z) {
  water3d *d = cw__water_data(water);
  if (!d) return;
  if (d->fine.c && !d->fine.own) {
    float lx, lz;
    to_local(water, x, z, &lx, &lz);
    fine_follow(d, lx, lz);
  }
  if (!d->rip_h) return;
  t3_mat4 inv;
  t3_mat4_invert(&inv, cw_node3d_global_transform(water));
  t3_vec3 p = t3_vec3_apply_mat4((t3_vec3){ x, 0, z }, &inv);
  float cell_x = d->rip_ax / (float)d->rip_nx, cell_z = d->rip_az / (float)d->rip_nz;
  /* whole cells only, so the field stays in step with the world */
  int sx = (int)floorf((p.x - d->ripple_center[0]) / cell_x + 0.5f), sz = (int)floorf((p.z - d->ripple_center[1]) / cell_z + 0.5f);
  if (!sx && !sz) return;
  shift_field(d->rip_h, d->rip_nx, d->rip_nz, sx, sz);
  shift_field(d->rip_v, d->rip_nx, d->rip_nz, sx, sz);
  shift_field(d->rip_f, d->rip_nx, d->rip_nz, sx, sz);
  d->ripple_center[0] += (float)sx * cell_x;
  d->ripple_center[1] += (float)sz * cell_z;
  d->rip_upload = true;
}

/* one fixed tick of a field's wave equation, foam spread and fade (the field's cell size cell): its energy left */
static float step_field(const water3d *d, float *h, float *v, float *f, int nx, int nz, float cell) {
  float dt = (float)cw_fixed_dt();
  /* wave equation: v += K lap(h); damp; h += v. K = (c dt / dx)^2, kept under the stability limit */
  float c = d->ripple_speed;
  float kk = (c * dt / cell) * (c * dt / cell);
  if (kk > 0.4f) kk = 0.4f;
  float damp = 1.0f - d->ripple_damping * dt;
  if (damp < 0) damp = 0;
  float fdecay = 1.0f - d->foam_fade * dt;
  if (fdecay < 0) fdecay = 0;
  float energy = 0;
  if (d->foam_spread > 0) {
    /* foam widens as it fades: a wake's trail opens behind a boat. Diffusion, one sweep in a fixed order. */
    float sp = d->foam_spread * dt / cell;
    float kf = sp * sp + sp * 0.25f;
    if (kf > 0.2f) kf = 0.2f;
    for (int j = 1; j < nz - 1; j++)
      for (int i = 1; i < nx - 1; i++) {
        size_t k = (size_t)j * (size_t)nx + (size_t)i;
        f[k] += kf * (f[k - 1] + f[k + 1] + f[k - (size_t)nx] + f[k + (size_t)nx] - 4.0f * f[k]);
      }
  }
  for (int j = 1; j < nz - 1; j++)
    for (int i = 1; i < nx - 1; i++) {
      size_t k = (size_t)j * (size_t)nx + (size_t)i;
      float lap = h[k - 1] + h[k + 1] + h[k - (size_t)nx] + h[k + (size_t)nx] - 4.0f * h[k];
      v[k] = (v[k] + kk * lap) * damp;
    }
  for (int j = 0; j < nz; j++)
    for (int i = 0; i < nx; i++) {
      size_t k = (size_t)j * (size_t)nx + (size_t)i;
      int m = i < j ? i : j;
      int m2 = (nx - 1 - i) < (nz - 1 - j) ? (nx - 1 - i) : (nz - 1 - j);
      if (m2 < m) m = m2;
      float s = 1.0f;
      if (m < SPONGE) s = m == 0 ? 0.0f : 0.55f + 0.45f * ((float)m / (float)SPONGE);
      h[k] = (h[k] + v[k]) * s;
      v[k] *= s;
      f[k] *= fdecay;
      float a = h[k] < 0 ? -h[k] : h[k];
      energy += a + (v[k] < 0 ? -v[k] : v[k]) + f[k];
    }
  return energy;
}

/* the fine layer: made, remade or freed as its properties and the quality ask (off at effects Low and under) */
static void fine_dims(water3d *d) {
  struct water_fine *g = &d->fine;
  int q = cw__quality_effects();
  int n = q <= CW_QUALITY_LOW ? 0 : d->collar_cells;
  if (n > 512) n = 512;
  if (n < 16 || d->collar_size < 0.1f) {
    if (g->c) fine_free(d);
    return;
  }
  if (g->c && g->built == d->collar_cells && g->quality == q && g->cell == d->collar_size / (float)n) return;
  float cx = g->c ? g->cx : d->ripple_center[0], cz = g->c ? g->cz : d->ripple_center[1];
  bool own = g->own;
  fine_free(d);
  g->own = own;
  size_t m = (size_t)n * (size_t)n;
  g->c = cw_calloc(m, sizeof(float), CW_MEM_RENDER);
  g->n = n, g->built = d->collar_cells, g->quality = q;
  g->cell = d->collar_size / (float)n;
  g->cx = cx, g->cz = cz;
  g->awake = g->upload = true;
  g->tex = t3_data_texture_new_typed(n, n, NULL, T3_HALF_FLOAT_TYPE);
  g->tex->mag_filter = T3_LINEAR;
  g->tex->min_filter = T3_LINEAR;
  g->tex->wrap_s = g->tex->wrap_t = T3_CLAMP_TO_EDGE;
}

/* one fixed tick: rain, then the wave equation */
void cw__ripples_step(cw_node n, water3d *d, uint32_t tick) {
  fine_dims(d);
  if (d->fine.c && d->fine.awake) {
    /* a collar's bubbles fade as foam does when they are no longer stamped (no spread: they hug what made them) */
    struct water_fine *g = &d->fine;
    float fdecay = 1.0f - d->foam_fade * (float)cw_fixed_dt(), e = 0;
    if (fdecay < 0) fdecay = 0;
    for (int k = 0; k < g->n * g->n; k++) g->c[k] *= fdecay, e += g->c[k];
    g->upload = true;
    if (e < 1e-4f) {
      memset(g->c, 0, (size_t)g->n * (size_t)g->n * sizeof(float));
      g->awake = false;
    }
  }
  int nx, nz;
  float ax, az;
  if (!dims(d, &nx, &nz, &ax, &az)) {
    if (d->rip_h) coarse_free(d);
    return;
  }
  if (!d->rip_h || d->rip_nx != nx || d->rip_nz != nz || d->rip_ax != ax || d->rip_az != az) alloc_grid(d, nx, nz, ax, az);
  float dt = (float)cw_fixed_dt();
  /* rain: drops per second over the whole area, positions from a hash of the tick */
  if (d->rain > 0) {
    d->rain_acc += d->rain * dt;
    uint32_t k = 0;
    while (d->rain_acc >= 1.0f && k < 64) {
      d->rain_acc -= 1.0f;
      uint32_t h = hash(tick * 2654435761u + k * 40503u + 12345u);
      float cx = unit(h) * (float)nx, cz = unit(hash(h ^ 0x9e3779b9u)) * (float)nz;
      float amt = 0.5f + unit(hash(h + 77u));
      stamp(d, cx, cz, d->rain_size * amt, d->rain_strength * amt, 0.0f);
      k++;
    }
  }
  if (!d->rip_awake) return;
  float *h = d->rip_h, *v = d->rip_v, *f = d->rip_f;
  float energy = step_field(d, h, v, f, nx, nz, ax / (float)nx);
  /* asleep when the field is flat: the next upload writes the last (empty) frame, then no more */
  if (energy < 1e-6f * (float)(nx * nz)) {
    memset(h, 0, (size_t)nx * (size_t)nz * sizeof(float));
    memset(v, 0, (size_t)nx * (size_t)nz * sizeof(float));
    memset(f, 0, (size_t)nx * (size_t)nz * sizeof(float));
    d->rip_awake = false;
    d->rip_upload = true;
  } else {
    d->rip_upload = true;
  }
  (void)n;
}

static void upload_field(t3_texture *t, const float *h, const float *f, size_t n) {
  uint16_t *p = (uint16_t *)t->pixels;
  for (size_t k = 0; k < n; k++) {
    p[k * 4] = to_half_signed(h[k]);
    p[k * 4 + 1] = to_half_signed(f[k] > 1.0f ? 1.0f : f[k]);
    p[k * 4 + 2] = 0;
    p[k * 4 + 3] = 0x3c00;
  }
  t->version++;
}

/* the field as the texture (red: height, green: foam), when it changed */
void cw__ripples_upload(water3d *d) {
  if (d->fine.tex && d->fine.upload) {
    d->fine.upload = false;
    /* (red: the collars' bubbles) */
    size_t m = (size_t)d->fine.n * (size_t)d->fine.n;
    uint16_t *p = (uint16_t *)d->fine.tex->pixels;
    for (size_t k = 0; k < m; k++) p[k * 4] = to_half_signed(d->fine.c[k] > 1.0f ? 1.0f : d->fine.c[k]), p[k * 4 + 1] = p[k * 4 + 2] = 0, p[k * 4 + 3] = 0x3c00;
    d->fine.tex->version++;
  }
  if (!d->rip_tex || !d->rip_h) return;
  if (!d->rip_upload) return;
  d->rip_upload = false;
  upload_field(d->rip_tex, d->rip_h, d->rip_f, (size_t)d->rip_nx * (size_t)d->rip_nz);
}
