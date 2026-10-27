/* Water3D's interactive ripple layer (cartwheel/water.h): a height field on a
 * grid over the water, stepped on the fixed clock, that splashes, wakes and
 * rain write into. The drawn water reads it as a half-float texture (red:
 * the ripple's height in metres, green: foam), so GL and WebGPU draw the same
 * thing and no compute is needed.
 *
 * The simulation is + - * and integer hashes only, in a fixed order, so every
 * host steps the same bits. The ripple height is for looks: bodies float on
 * the Gerstner waves alone (cw_water_height_at), so gameplay never depends on
 * the grid's resolution or the quality preset. */
#include <math.h>
#include <string.h>

#include "cartwheel/math.h"
#include "cartwheel/particles.h"
#include "cartwheel/render3d.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/water.h"
#include "water_internal.h"

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
  int q = cw_gfx_get(CW_GFX_EFFECTS);
  if (q == CW_QUALITY_LOW) n /= 2;
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

void cw__ripples_free(water3d *d) {
  cw_free(d->rip_h);
  cw_free(d->rip_v);
  cw_free(d->rip_f);
  if (d->rip_tex) t3_release(d->rip_tex);
  d->rip_h = d->rip_v = d->rip_f = NULL;
  d->rip_tex = NULL;
  d->rip_nx = d->rip_nz = 0;
}

static void alloc_grid(water3d *d, int nx, int nz, float ax, float az) {
  cw__ripples_free(d);
  size_t n = (size_t)nx * (size_t)nz;
  d->rip_h = cw_calloc(n, sizeof(float), CW_MEM_RENDER);
  d->rip_v = cw_calloc(n, sizeof(float), CW_MEM_RENDER);
  d->rip_f = cw_calloc(n, sizeof(float), CW_MEM_RENDER);
  d->rip_nx = nx, d->rip_nz = nz;
  d->rip_ax = ax, d->rip_az = az;
  d->rip_awake = true; /* the first upload writes the empty field */
  d->rip_upload = true;
  d->rip_cells_built = d->ripple_cells;
  d->rip_quality = cw_gfx_get(CW_GFX_EFFECTS);
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

/* a smooth round dent: depth metres at the centre, radius metres, foam 0..1 */
static void stamp(water3d *d, float cx, float cz, float radius, float depth, float foam) {
  if (!d->rip_h) return;
  float cw_x = d->rip_ax / (float)d->rip_nx, cw_z = d->rip_az / (float)d->rip_nz; /* cell size, m */
  float rx = radius / cw_x, rz = radius / cw_z;
  int i0 = (int)floorf(cx - rx), i1 = (int)ceilf(cx + rx), j0 = (int)floorf(cz - rz), j1 = (int)ceilf(cz + rz);
  if (i1 < 0 || j1 < 0 || i0 >= d->rip_nx || j0 >= d->rip_nz) return;
  if (i0 < 1) i0 = 1;
  if (j0 < 1) j0 = 1;
  if (i1 > d->rip_nx - 2) i1 = d->rip_nx - 2;
  if (j1 > d->rip_nz - 2) j1 = d->rip_nz - 2;
  for (int j = j0; j <= j1; j++)
    for (int i = i0; i <= i1; i++) {
      float dx = ((float)i - cx) / rx, dz = ((float)j - cz) / rz;
      float q = dx * dx + dz * dz;
      if (q >= 1.0f) continue;
      float w = (1.0f - q) * (1.0f - q);
      size_t k = (size_t)j * (size_t)d->rip_nx + (size_t)i;
      d->rip_h[k] -= depth * w;
      float f = foam * w;
      if (f > d->rip_f[k]) d->rip_f[k] = f;
    }
  d->rip_awake = true;
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
  return h;
}

/* one fixed tick: rain, then the wave equation */
void cw__ripples_step(cw_node n, water3d *d, uint32_t tick) {
  int nx, nz;
  float ax, az;
  if (!dims(d, &nx, &nz, &ax, &az)) {
    if (d->rip_h) cw__ripples_free(d);
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
  /* wave equation: v += K lap(h); damp; h += v. K = (c dt / dx)^2, kept under the stability limit */
  float cell = ax / (float)nx;
  float c = d->ripple_speed;
  float kk = (c * dt / cell) * (c * dt / cell);
  if (kk > 0.4f) kk = 0.4f;
  float damp = 1.0f - d->ripple_damping * dt;
  if (damp < 0) damp = 0;
  float fdecay = 1.0f - d->foam_fade * dt;
  if (fdecay < 0) fdecay = 0;
  if (!d->rip_awake) return;
  float *h = d->rip_h, *v = d->rip_v, *f = d->rip_f;
  float energy = 0;
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

/* the field as the texture (red: height, green: foam), when it changed */
void cw__ripples_upload(water3d *d) {
  if (!d->rip_tex || !d->rip_h) return;
  if (!d->rip_upload) return;
  d->rip_upload = false;
  size_t n = (size_t)d->rip_nx * (size_t)d->rip_nz;
  uint16_t *p = (uint16_t *)d->rip_tex->pixels;
  for (size_t k = 0; k < n; k++) {
    p[k * 4] = to_half_signed(d->rip_h[k]);
    p[k * 4 + 1] = to_half_signed(d->rip_f[k] > 1.0f ? 1.0f : d->rip_f[k]);
    p[k * 4 + 2] = 0;
    p[k * 4 + 3] = 0x3c00;
  }
  d->rip_tex->version++;
}
