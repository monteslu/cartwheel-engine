/* Water3D (cartwheel/water.h): Gerstner waves on the CPU, the surface's
 * three.c mesh with the "water" extension (src/water/extensions/water.mjs),
 * and the planar reflection and refraction, rendered as three.js r186's
 * Reflector / Refractor (and ReflectorNode) render them: a mirrored camera
 * or the view's own, each with an oblique near plane (Lengyel) at the rest
 * level, before the scene pass. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/environment.h"
#include "cartwheel/gpu.h"
#include "cartwheel/math.h"
#include "cartwheel/physics3d.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/render3d.h"
#include "cartwheel/resource.h"
#include "cartwheel/water.h"
#include "water_internal.h"
#if CW_WGPU
#include "platform/wgpu.h"
#endif

#define TWO_PI 6.283185307179586
#define GRAVITY 9.81

/* ── the waves ───────────────────────────────────────────────────────── */

/* per wave (wave_eval, water_internal.h): direction, wave number,
 * amplitude, shared-out steepness and the time phase reduced to [0, 2 pi),
 * all from (waves, t) alone */
void cw__waves_prepare(const cw_waves *w, double t, wave_eval *e) {
  memset(e, 0, sizeof *e);
  int n = 0;
  for (int i = 0; i < w->count && i < CW_WATER_MAX_WAVES; i++)
    if (w->wave[i].wavelength > 0 && w->wave[i].amplitude != 0) n++;
  e->n = CW_WATER_MAX_WAVES;
  for (int i = 0; i < CW_WATER_MAX_WAVES; i++) {
    const cw_water_wave *v = &w->wave[i];
    if (i >= w->count || !(v->wavelength > 0) || v->amplitude == 0) continue; /* a = 0: no effect */
    float ang = v->direction_deg * (CW_PI / 180.0f);
    cw_sincos(ang, &e->dz[i], &e->dx[i]);
    double k = TWO_PI / (double)v->wavelength;
    double om = sqrt(GRAVITY * k) * (double)(w->speed > 0 ? w->speed : 1);
    e->k[i] = (float)k;
    e->a[i] = v->amplitude;
    e->om[i] = (float)om;
    /* GPU Gems: Q_i = steepness / (k_i A_i N), so sum Q k A <= 1 (no loops) */
    float st = v->steepness < 0 ? 0 : v->steepness > 1 ? 1 : v->steepness;
    e->q[i] = st / (e->k[i] * fabsf(e->a[i]) * (float)n);
    /* fmod is exact; the product is one rounding, in double */
    e->ph[i] = (float)fmod(om * t, TWO_PI);
  }
}

static void displace(const wave_eval *e, float x0, float z0, float out[3], float *nrm, float *vel) {
  float x = x0, y = 0, z = z0;
  float nx = 0, ny = 1, nz = 0, vx = 0, vy = 0, vz = 0;
  for (int i = 0; i < e->n; i++) {
    if (e->a[i] == 0) continue;
    float th = e->k[i] * (e->dx[i] * x0 + e->dz[i] * z0) - e->ph[i];
    float s, c;
    cw_sincos(th, &s, &c);
    float qa = e->q[i] * e->a[i];
    x += qa * e->dx[i] * c;
    y += e->a[i] * s;
    z += qa * e->dz[i] * c;
    float wa = e->k[i] * e->a[i];
    nx -= e->dx[i] * wa * c;
    ny -= e->q[i] * wa * s;
    nz -= e->dz[i] * wa * c;
    /* d/dt of the displacement: th falls at omega */
    vx += qa * e->dx[i] * e->om[i] * s;
    vy -= e->a[i] * e->om[i] * c;
    vz += qa * e->dz[i] * e->om[i] * s;
  }
  out[0] = x, out[1] = y, out[2] = z;
  if (nrm) {
    float l = sqrtf(nx * nx + ny * ny + nz * nz);
    nrm[0] = nx / l, nrm[1] = ny / l, nrm[2] = nz / l;
  }
  if (vel) vel[0] = vx, vel[1] = vy, vel[2] = vz;
}

void cw_waves_displace(const cw_waves *w, float x0, float z0, double t, float out[3], float out_normal[3]) {
  wave_eval e;
  cw__waves_prepare(w, t, &e);
  displace(&e, x0, z0, out, out_normal, NULL);
}

/* the rest point whose displaced position lands on (x, z) */
static void rest_point(const wave_eval *e, float x, float z, float *rx, float *rz) {
  float px = x, pz = z, d[3];
  for (int it = 0; it < 4; it++) {
    displace(e, px, pz, d, NULL, NULL);
    px -= d[0] - x;
    pz -= d[2] - z;
  }
  *rx = px, *rz = pz;
}

float cw_waves_height(const cw_waves *w, float x, float z, double t) {
  wave_eval e;
  cw__waves_prepare(w, t, &e);
  float rx, rz, d[3];
  rest_point(&e, x, z, &rx, &rz);
  displace(&e, rx, rz, d, NULL, NULL);
  return d[1];
}

/* ── the node ────────────────────────────────────────────────────────── */

static const char *const debug_names[] = { "NONE", "HEIGHT", "REFRACTION_DEPTH", NULL };
static const cw_node_type *water_t;

const cw_node_type *cw__water_type(void) { return water_t; }

static void water_init(cw_node n, void *data) {
  (void)n;
  water3d *d = data;
  d->size[0] = d->size[1] = 40;
  d->subdivisions = 160;
  d->wave_count = 3;
  d->wave0[0] = 20, d->wave0[1] = 12, d->wave0[2] = 0.35f, d->wave0[3] = 0.5f;
  d->wave1[0] = 75, d->wave1[1] = 7, d->wave1[2] = 0.18f, d->wave1[3] = 0.5f;
  d->wave2[0] = -40, d->wave2[1] = 3.5f, d->wave2[2] = 0.06f, d->wave2[3] = 0.4f;
  d->wave3[0] = 140, d->wave3[1] = 2, d->wave3[2] = 0.03f, d->wave3[3] = 0.3f;
  d->wave_speed = 1;
  d->color[0] = 0.75f, d->color[1] = 0.9f, d->color[2] = 0.92f, d->color[3] = 1;
  d->deep_color[0] = 0.01f, d->deep_color[1] = 0.05f, d->deep_color[2] = 0.07f, d->deep_color[3] = 1;
  d->reflectivity = 0.02f;
  d->distortion = 0.03f;
  d->detail = 0.35f;
  d->roughness = 0.04f; /* a crisp sun path: broader reads as a smear on the far chop */
  d->murk = 0.7f;
  d->normal_scale = 0.12f;
  d->flow_direction[0] = 1, d->flow_direction[1] = 0;
  d->flow_speed = 0.03f;
  d->reflection = d->refraction = true;
  d->resolution_scale = 0.5f;
  d->debug_range[0] = -2, d->debug_range[1] = 2;
  d->floor_map_size = 128;
  d->floor_range = 40;
  d->clarity = 5;
  d->shore_foam = 0.7f;
  d->crest_foam = 0.35f;
  d->shallow_color[0] = 0.05f, d->shallow_color[1] = 0.34f, d->shallow_color[2] = 0.38f, d->shallow_color[3] = 1;
  d->scatter = 0.6f;
  d->glint = 1.0f;
  d->ripple_cells = 128;
  d->ripple_speed = 2.2f;
  d->ripple_damping = 1.5f;
  d->ripple_strength = 1.0f;
  d->foam_fade = 0.9f;
  d->rain_size = 0.5f;
  d->rain_strength = 0.03f;
  d->splash_fx_min = 0.15f;
  /* a valid inverse projection before the first refraction render (the surface reads it in every state) */
  for (int i = 0; i < 16; i++) d->refr_inv[i] = (i % 5) == 0 ? 1.0f : 0.0f;
  d->caustics = 1.0f;
  d->caustic_size = 5.0f;
  d->underwater = true;
}

water3d *cw__water_data(cw_node n) { return cw_node_data_t(n, water_t); }

cw_waves cw__water_waves(const water3d *d) {
  cw_waves w = { .count = d->wave_count < 0 ? 0 : d->wave_count > 4 ? 4 : d->wave_count, .speed = d->wave_speed };
  const float *v[4] = { d->wave0, d->wave1, d->wave2, d->wave3 };
  for (int i = 0; i < 4; i++) w.wave[i] = (cw_water_wave){ v[i][0], v[i][1], v[i][2], v[i][3] };
  return w;
}

/* the tree's waters, in entry order */
typedef struct water_tree {
  CW_ARRAY(cw_node) nodes;
  cw_water_stats stats;
  t3_renderer *bound;
} water_tree;

static void wtree_free(void *p) {
  water_tree *wt = p;
  cw_array_free(&wt->nodes);
  cw_free(wt);
}

cw_node cw__water_at(cw_tree *t, int i) {
  water_tree *wt = t ? cw_tree_slot(t, "water") : NULL;
  return wt && i >= 0 && i < wt->nodes.count ? wt->nodes.data[i] : (cw_node){ 0 };
}

cw_node cw_water_find(cw_tree *t) {
  water_tree *wt = t ? cw_tree_slot(t, "water") : NULL;
  return wt && wt->nodes.count ? wt->nodes.data[0] : (cw_node){ 0 };
}

double cw_water_time(cw_node water) {
  water3d *d = cw_node_data_t(water, water_t);
  cw_tree *t = cw_node_tree(water);
  if (!d || !t) return 0;
  return (double)cw_tree_tick(t) * cw_fixed_dt() + (double)d->time_offset;
}

bool cw_water_get_waves(cw_node water, cw_waves *out) {
  water3d *d = cw_node_data_t(water, water_t);
  if (!d) return false;
  *out = cw__water_waves(d);
  return true;
}

/* world <-> the node's local frame (rest plane y = 0) */
static void to_local(cw_node n, float x, float z, float *lx, float *lz, t3_mat4 *inv) {
  const t3_mat4 *m = cw_node3d_global_transform(n);
  t3_mat4_invert(inv, m);
  t3_vec3 p = t3_vec3_apply_mat4((t3_vec3){ x, 0, z }, inv);
  /* the world point's local x, z on the rest plane (a level water: no tilt) */
  *lx = p.x, *lz = p.z;
}

bool cw_water_sample_at(cw_node water, float x, float z, double t, cw_water_sample *out) {
  memset(out, 0, sizeof *out);
  out->normal.y = 1;
  water3d *d = cw_node_data_t(water, water_t);
  if (!d) return false;
  cw_waves w = cw__water_waves(d);
  wave_eval e;
  cw__waves_prepare(&w, t, &e);
  float lx, lz, rx, rz, p[3], nl[3], vl[3];
  t3_mat4 inv;
  to_local(water, x, z, &lx, &lz, &inv);
  rest_point(&e, lx, lz, &rx, &rz);
  displace(&e, rx, rz, p, nl, vl);
  const t3_mat4 *m = cw_node3d_global_transform(water);
  t3_vec3 wp = t3_vec3_apply_mat4((t3_vec3){ p[0], p[1], p[2] }, m);
  out->height = wp.y;
  /* directions: the rotation part (the node may be yawed) */
  const float *e4 = m->e;
  t3_vec3 nw = { e4[0] * nl[0] + e4[4] * nl[1] + e4[8] * nl[2], e4[1] * nl[0] + e4[5] * nl[1] + e4[9] * nl[2],
                 e4[2] * nl[0] + e4[6] * nl[1] + e4[10] * nl[2] };
  float l = sqrtf(nw.x * nw.x + nw.y * nw.y + nw.z * nw.z);
  out->normal = (t3_vec3){ nw.x / l, nw.y / l, nw.z / l };
  out->velocity = (t3_vec3){ e4[0] * vl[0] + e4[4] * vl[1] + e4[8] * vl[2], e4[1] * vl[0] + e4[5] * vl[1] + e4[9] * vl[2],
                             e4[2] * vl[0] + e4[6] * vl[1] + e4[10] * vl[2] };
  return true;
}

float cw_water_height_at(cw_node water, float x, float z, double t) {
  cw_water_sample s;
  if (!cw_water_sample_at(water, x, z, t, &s)) return cw_node_valid(water) ? cw_node3d_global_position(water).y : 0;
  return s.height;
}

void cw_water_get_stats(cw_tree *t, cw_water_stats *out) {
  memset(out, 0, sizeof *out);
  water_tree *wt = t ? cw_tree_slot(t, "water") : NULL;
  if (wt) *out = wt->stats;
  if (t) out->buoyant = cw__buoyancy_count(t);
  if (wt)
    for (int i = 0; i < wt->nodes.count; i++) {
      water3d *d = cw_node_data_t(wt->nodes.data[i], water_t);
      if (d && d->under_drawn) out->underwater = 1;
    }
}

/* ── the floor map ─────────────────────────────────────────────────────── */

static uint16_t to_half(float f) {
  uint32_t x;
  memcpy(&x, &f, 4);
  uint32_t mant = x & 0x7fffffu;
  int e = (int)((x >> 23) & 0xffu) - 127 + 15;
  if (f <= 0) return 0;
  if (e >= 31) return 0x7bffu;
  if (e <= 0) {
    if (e < -10) return 0;
    mant |= 0x800000u;
    return (uint16_t)(mant >> (14 - e));
  }
  uint32_t h = ((uint32_t)e << 10) | (mant >> 13), rem = mant & 0x1fffu;
  if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) h++;
  return (uint16_t)h;
}

static bool floor_dims(const water3d *d, int *nx, int *nz) {
  int n = d->floor_map_size;
  if (n < 2) return false;
  if (n > 512) n = 512;
  float sx = d->size[0] > 0.01f ? d->size[0] : 0.01f, sz = d->size[1] > 0.01f ? d->size[1] : 0.01f;
  *nx = sx >= sz ? n : (int)ceilf((float)n * sx / sz);
  *nz = sz >= sx ? n : (int)ceilf((float)n * sz / sx);
  if (*nx < 2) *nx = 2;
  if (*nz < 2) *nz = 2;
  return true;
}

/* Bake the water depth under every cell: a ray down from 20 m over the rest
 * level through the tree's static shapes. The highest static hit under the
 * rest level is the floor; only hits above it mean land; none means deep. */
static void bake_floor(cw_node n, water3d *d) {
  int nx, nz;
  cw_world3d w = cw_physics3d_world(cw_node_tree(n));
  if (!floor_dims(d, &nx, &nz) || !cw_world3d_valid(w)) return;
  const t3_mat4 *m = cw_node3d_global_transform(n);
  float sx = d->size[0] > 0.01f ? d->size[0] : 0.01f, sz = d->size[1] > 0.01f ? d->size[1] : 0.01f;
  float range = d->floor_range > 1 ? d->floor_range : 1;
  float *depth = cw_alloc((size_t)nx * (size_t)nz * sizeof(float), CW_MEM_RENDER);
  uint16_t *tex = cw_alloc((size_t)nx * (size_t)nz * 4 * sizeof(uint16_t), CW_MEM_RENDER);
  for (int j = 0; j < nz; j++)
    for (int i = 0; i < nx; i++) {
      float lx = ((float)i + 0.5f) / (float)nx * sx - sx * 0.5f, lz = ((float)j + 0.5f) / (float)nz * sz - sz * 0.5f;
      t3_vec3 p = t3_vec3_apply_mat4((t3_vec3){ lx, 0, lz }, m);
      float v = range;
      /* a static shape holding the point at the rest level: land (and a post's foot) */
      cw_qshape3d q = cw_qshape3d_sphere(p, 0.05f);
      cw_shape3d inside[4];
      int ni = cw_world3d_overlap(w, &q, NULL, inside, 4), land = 0;
      for (int k = 0; k < ni && !land; k++) land = cw_body3d_type(cw_shape3d_body(inside[k])) == CW_BODY_STATIC;
      if (land) v = 0;
      else {
        /* the highest static hit under the rest level is the floor */
        cw_ray_hit3d hits[6];
        int c = cw_world3d_raycast_all(w, p, (t3_vec3){ 0, -range, 0 }, NULL, hits, 6);
        for (int k = 0; k < c; k++)
          if (cw_body3d_type(hits[k].body) == CW_BODY_STATIC) {
            float under = p.y - hits[k].point.y;
            v = under < range ? under : range;
            break;
          }
      }
      depth[j * nx + i] = v;
      uint16_t h = to_half(v);
      tex[(j * nx + i) * 4] = h, tex[(j * nx + i) * 4 + 1] = h, tex[(j * nx + i) * 4 + 2] = h, tex[(j * nx + i) * 4 + 3] = 0x3c00;
    }
  if (d->floor_tex) t3_release(d->floor_tex);
  d->floor_tex = t3_data_texture_new_typed(nx, nz, tex, T3_HALF_FLOAT_TYPE);
  d->floor_tex->mag_filter = T3_LINEAR;
  d->floor_tex->min_filter = T3_LINEAR;
  d->floor_tex->wrap_s = d->floor_tex->wrap_t = T3_CLAMP_TO_EDGE;
  d->floor_tex->version++;
  cw_free(tex);
  cw_free(d->floor_depth);
  d->floor_depth = depth;
  d->floor_n = nx;
  d->floor_done = true;
  d->floor_dirty = false;
}

float cw_water_floor_depth_at(cw_node water, float x, float z) {
  water3d *d = cw_node_data_t(water, water_t);
  int nx, nz;
  if (!d || !d->floor_depth || !floor_dims(d, &nx, &nz)) return 1e6f;
  t3_mat4 inv;
  const t3_mat4 *m = cw_node3d_global_transform(water);
  t3_mat4_invert(&inv, m);
  t3_vec3 p = t3_vec3_apply_mat4((t3_vec3){ x, 0, z }, &inv);
  float u = p.x / (d->size[0] > 0.01f ? d->size[0] : 0.01f) + 0.5f, v = p.z / (d->size[1] > 0.01f ? d->size[1] : 0.01f) + 0.5f;
  if (u < 0 || u >= 1 || v < 0 || v >= 1) return 1e6f;
  return d->floor_depth[(int)(v * (float)nz) * nx + (int)(u * (float)nx)];
}

void cw_water_rebake_floor(cw_node water) {
  water3d *d = cw_node_data_t(water, water_t);
  if (d) d->floor_dirty = true;
}

/* ── the mesh ────────────────────────────────────────────────────────── */

/* the grid: nx x nz quads over the rectangle, in the node's XZ plane */
/* Preserve a uniform central grid; stretch only the outer fifth to the
 * horizon. One connected surface and material means no overlapping sea,
 * depth fighting or change of reflection at the edge of a local patch. */
static float ocean_coordinate(float t, float size, float horizon) {
  if (horizon <= size * 0.5f) return t * size * 0.5f;
  float a = fabsf(t), inner = size * 0.5f;
  float x = a <= 0.8f ? a * inner / 0.8f
    : inner * expf(logf(horizon / inner) * (a - 0.8f) / 0.2f);
  return copysignf(x, t);
}
static t3_geometry *grid(float sx, float sz, int nx, int nz, float horizon) {
  int vx = nx + 1, vz = nz + 1, nv = vx * vz;
  float *pos = cw_alloc((size_t)nv * 3 * sizeof(float), CW_MEM_RENDER);
  float *nrm = cw_alloc((size_t)nv * 3 * sizeof(float), CW_MEM_RENDER);
  float *uv = cw_alloc((size_t)nv * 2 * sizeof(float), CW_MEM_RENDER);
  for (int j = 0; j < vz; j++)
    for (int i = 0; i < vx; i++) {
      int k = j * vx + i;
      pos[k * 3] = ocean_coordinate(2.0f * (float)i / (float)nx - 1.0f, sx, horizon);
      pos[k * 3 + 1] = 0;
      pos[k * 3 + 2] = ocean_coordinate(2.0f * (float)j / (float)nz - 1.0f, sz, horizon);
      nrm[k * 3] = 0, nrm[k * 3 + 1] = 1, nrm[k * 3 + 2] = 0;
      uv[k * 2] = (float)i / (float)nx;
      uv[k * 2 + 1] = (float)j / (float)nz;
    }
  int ni = nx * nz * 6;
  uint32_t *idx = cw_alloc((size_t)ni * sizeof(uint32_t), CW_MEM_RENDER);
  int w = 0;
  for (int j = 0; j < nz; j++)
    for (int i = 0; i < nx; i++) {
      uint32_t a = (uint32_t)(j * vx + i), b = a + 1, c = a + (uint32_t)vx, d = c + 1;
      /* counter-clockwise seen from +Y */
      idx[w++] = a, idx[w++] = c, idx[w++] = b;
      idx[w++] = b, idx[w++] = c, idx[w++] = d;
    }
  t3_geometry *g = t3_geometry_new();
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, t3_attribute_new(T3_FLOAT32, pos, nv, 3));
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, t3_attribute_new(T3_FLOAT32, nrm, nv, 3));
  t3_geometry_set_attribute(g, T3_ATTR_UV, t3_attribute_new(T3_FLOAT32, uv, nv, 2));
  t3_geometry_set_index(g, t3_attribute_new(T3_UINT32, idx, ni, 1));
  cw_free(pos);
  cw_free(nrm);
  cw_free(uv);
  cw_free(idx);
  return g;
}

int cw__water_quality(void) { return cw_gfx_get(CW_GFX_EFFECTS); }

static void build(cw_node n, water3d *d) {
  int q = cw__water_quality();
  int sub = d->subdivisions < 1 ? 1 : d->subdivisions;
  if (q == CW_QUALITY_LOW) sub = sub / 4 < 1 ? 1 : sub / 4;
  else if (q == CW_QUALITY_MEDIUM) sub = sub / 2 < 1 ? 1 : sub / 2;
  float sx = d->size[0] > 0.01f ? d->size[0] : 0.01f, sz = d->size[1] > 0.01f ? d->size[1] : 0.01f;
  float amp = 0;
  cw_waves w = cw__water_waves(d);
  for (int i = 0; i < w.count; i++) amp += fabsf(w.wave[i].amplitude) * 2;
  if (d->mesh && d->built_sub == sub && d->built_size[0] == sx && d->built_size[1] == sz && d->built_amp == amp && d->built_horizon == d->horizon_distance) return;
  t3_object *group = cw_node3d_object(n);
  if (d->mesh) {
    t3_object_remove(group, d->mesh);
    t3_release(d->mesh);
    d->mesh = NULL;
  }
  int nx = sx >= sz ? sub : (int)ceilf((float)sub * sx / sz), nz = sz >= sx ? sub : (int)ceilf((float)sub * sz / sx);
  if (nx < 1) nx = 1;
  if (nz < 1) nz = 1;
  t3_geometry *g = grid(sx, sz, nx, nz, d->horizon_distance);
  /* the waves lift and move the surface: bounds that hold every crest */
  t3_geometry_compute_bounding_sphere(g);
  g->bounding_sphere.radius += amp;
  if (!d->mat) {
    d->mat = t3_mesh_standard_material_new(0x000000);
    d->mat->metalness = 0;
    d->mat->emissive = (t3_color){ 1, 1, 1 }; /* the extension multiplies its light by it (white: as computed) */
    d->mat->side = T3_DOUBLE_SIDE;           /* seen from under the water too */
    t3_material_set_extension(d->mat, "water");
  }
  d->mat->roughness = d->roughness;
  d->mesh = t3_mesh_new(g, d->mat);
  t3_release(g);
  d->mesh->base.user_data = d;
  d->mesh->base.receive_shadow = true;
  d->mesh->base.cast_shadow = false;
  t3_object_add(group, d->mesh);
  d->built_sub = sub;
  d->built_size[0] = sx, d->built_size[1] = sz;
  d->built_amp = amp;
  d->built_horizon = d->horizon_distance;
  d->triangles = nx * nz * 2;
}

static void water_destroy(cw_node n, void *data) {
  (void)n;
  water3d *d = data;
  if (d->mesh) t3_release(d->mesh);
  if (d->mat) t3_release(d->mat);
  if (d->refl) t3_release(d->refl);
  if (d->refr) t3_release(d->refr);
  if (d->vcam) t3_release(d->vcam);
  cw_res_release(d->normal_res);
  cw_res_release(d->flow_res);
  if (d->floor_tex) t3_release(d->floor_tex);
  if (d->caus_tex) t3_release(d->caus_tex);
  cw__water_refr_wgpu_free(d);
  cw__ripples_free(d);
  cw_free(d->floor_depth);
  memset(&d->mesh, 0, sizeof *d - offsetof(water3d, mesh));
}

/* the textures the properties name (loaded when they change) */
static void textures(water3d *d) {
  const char *nm = d->normal_map && *d->normal_map ? d->normal_map
                   : "gen://noise?size=256&type=perlin&fractal=fbm&octaves=4&freq=0.035&out=normal&strength=3&seamless=1";
  if (nm != d->loaded_normal) {
    cw_res_release(d->normal_res);
    d->normal_res = cw_res_load(nm, CW_RES_TEXTURE);
    d->loaded_normal = nm;
  }
  if (!d->caus_tex) {
    /* the caustic tile (caustics.c) as RGBA8, repeating, mipmapped */
    const uint8_t *c = cw__water_caustics();
    const int n = CW_WATER_CAUSTIC_N;
    uint8_t *px = cw_alloc((size_t)n * (size_t)n * 4, CW_MEM_RENDER);
    const uint8_t *soft = cw__water_caustics_soft();
    /* red: the sharp web, green: the soft copy */
    for (int k = 0; k < n * n; k++) px[k * 4] = c[k], px[k * 4 + 1] = soft[k], px[k * 4 + 2] = 0, px[k * 4 + 3] = 255;
    d->caus_tex = t3_data_texture_new(n, n, px);
    cw_free(px);
    d->caus_tex->wrap_s = d->caus_tex->wrap_t = T3_REPEAT;
    d->caus_tex->mag_filter = T3_LINEAR;
    d->caus_tex->min_filter = T3_LINEAR_MIPMAP_LINEAR;
    d->caus_tex->generate_mipmaps = true;
    d->caus_tex->version++;
  }
  const char *fm = d->flow_map && *d->flow_map ? d->flow_map : NULL;
  if (fm != d->loaded_flow) {
    cw_res_release(d->flow_res);
    d->flow_res = fm ? cw_res_load(fm, CW_RES_TEXTURE) : (cw_res){ 0 };
    d->loaded_flow = fm;
  }
}

/* ── planar reflection and refraction ─────────────────────────────────── */

static t3_render_target *target(t3_render_target *rt, int w, int h) {
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (!rt) {
    t3_render_target_options o = t3_render_target_options_default();
    o.type = T3_HALF_FLOAT_TYPE;
    rt = t3_render_target_new(w, h, &o);
  } else if (rt->width != w || rt->height != h) {
    t3_render_target_set_size(rt, w, h);
  }
  return rt;
}

/* the refraction target: with a depth texture, so the surface can measure the water over what lies under it */
static t3_render_target *refr_target(t3_render_target *rt, int w, int h, bool depth) {
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (rt && (rt->width != w || rt->height != h || (depth && !rt->depth_texture))) {
    t3_release(rt);
    rt = NULL;
  }
  if (!rt) {
    t3_render_target_options o = t3_render_target_options_default();
    o.type = T3_HALF_FLOAT_TYPE;
    t3_texture *dt = depth ? t3_depth_texture_new(w, h, 0) : NULL;
    o.depth_texture = dt;
    rt = t3_render_target_new(w, h, &o);
    if (dt) t3_release(dt);
  }
  return rt;
}

static t3_vec3 v3sub(t3_vec3 a, t3_vec3 b) { return (t3_vec3){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static t3_vec3 v3add(t3_vec3 a, t3_vec3 b) { return (t3_vec3){ a.x + b.x, a.y + b.y, a.z + b.z }; }
static float v3dot(t3_vec3 a, t3_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
/* three.js Vector3.reflect(normal): v - 2 (v . n) n */
static t3_vec3 v3reflect(t3_vec3 v, t3_vec3 n) {
  float k = 2 * v3dot(v, n);
  return (t3_vec3){ v.x - n.x * k, v.y - n.y * k, v.z - n.z * k };
}
static t3_vec3 v3neg(t3_vec3 a) { return (t3_vec3){ -a.x, -a.y, -a.z }; }
static t3_vec3 rot3(const t3_mat4 *m, t3_vec3 v) {
  const float *e = m->e;
  t3_vec3 r = { e[0] * v.x + e[4] * v.y + e[8] * v.z, e[1] * v.x + e[5] * v.y + e[9] * v.z, e[2] * v.x + e[6] * v.y + e[10] * v.z };
  float l = sqrtf(v3dot(r, r));
  return l > 0 ? (t3_vec3){ r.x / l, r.y / l, r.z / l } : r;
}
static float signf(float x) { return x > 0 ? 1.0f : x < 0 ? -1.0f : 0.0f; }

/* Lengyel's oblique near plane (r186 Reflector / Refractor, GL clip space):
 * the plane (normal n, through p, world) in the camera's view space becomes
 * the near plane of proj */
static void oblique(t3_mat4 *proj, const t3_mat4 *view, t3_vec3 n, t3_vec3 p) {
  t3_vec3 nv = rot3(view, n);
  t3_vec3 pv = t3_vec3_apply_mat4(p, view);
  float c[4] = { nv.x, nv.y, nv.z, -v3dot(nv, pv) };
  float *e = proj->e;
  float q[4] = { (signf(c[0]) + e[8]) / e[0], (signf(c[1]) + e[9]) / e[5], -1.0f, (1.0f + e[10]) / e[14] };
  float s = 2.0f / (c[0] * q[0] + c[1] * q[1] + c[2] * q[2] + c[3] * q[3]);
  e[2] = c[0] * s;
  e[6] = c[1] * s;
  e[10] = c[2] * s + 1.0f;
  e[14] = c[3] * s;
}

/* render the scene into rt from cam (the water hidden), as the pipeline's
 * passes that render with three.c must: bindings reset, the renderer's
 * target put back */
static void render_into(cw_pass_ctx *ctx, water_tree *wt, t3_render_target *rt, t3_camera *cam) {
  t3_renderer *r = ctx->renderer;
  for (int i = 0; i < wt->nodes.count; i++) {
    water3d *o = cw_node_data_t(wt->nodes.data[i], water_t);
    if (o && o->mesh) o->mesh->base.visible = false;
  }
#if CW_WGPU
  /* WebGPU: three.c submits this render itself, queued before the frame's
   * work (as the sky's PMREM does) */
  if (cw__wgpu_on()) t3_renderer_set_external_encoder(r, 0);
#endif
  t3_renderer_reset_bindings(r);
  t3_render_target *prev = t3_renderer_get_render_target(r);
  bool auto_clear = t3_renderer_get_auto_clear(r);
  t3_renderer_set_auto_clear(r, true);
  t3_renderer_set_render_target(r, rt);
  if (cam) t3_renderer_render(r, ctx->scene3d, cam);
  else t3_renderer_clear(r, true, true, false);
  t3_renderer_set_render_target(r, prev);
  t3_renderer_set_auto_clear(r, auto_clear);
  t3_renderer_reset_bindings(r);
  for (int i = 0; i < wt->nodes.count; i++) {
    water3d *o = cw_node_data_t(wt->nodes.data[i], water_t);
    if (o && o->mesh) o->mesh->base.visible = true;
  }
}

/* the refraction: three.c's render target on GL; on WebGPU a target of the engine's own whose depth is copied for
 * the surface to read (refraction_wgpu.c). cam NULL: cleared, 1 x 1 */
static void render_refr(cw_pass_ctx *ctx, water_tree *wt, water3d *d, t3_camera *cam, int w, int h) {
  if (cw__wgpu_on()) {
    for (int i = 0; i < wt->nodes.count; i++) {
      water3d *o = cw_node_data_t(wt->nodes.data[i], water_t);
      if (o && o->mesh) o->mesh->base.visible = false;
    }
    cw__water_refr_wgpu(ctx, d, cam, cam ? w : 1, cam ? h : 1);
    for (int i = 0; i < wt->nodes.count; i++) {
      water3d *o = cw_node_data_t(wt->nodes.data[i], water_t);
      if (o && o->mesh) o->mesh->base.visible = true;
    }
    return;
  }
  render_into(ctx, wt, d->refr, cam);
}

static void planar(cw_pass_ctx *ctx, water_tree *wt, cw_node n, water3d *d) {
  int q = cw__water_quality();
  /* Low: neither (the environment and deep_color stand in); Medium: the
   * reflection at half the scale; High: both as asked; Ultra: both at 1.5x */
  float sc = d->resolution_scale * (q == CW_QUALITY_MEDIUM ? 0.5f : q == CW_QUALITY_ULTRA ? 1.5f : 1.0f);
  sc = sc < 0.05f ? 0.05f : sc > 1 ? 1 : sc;
  d->refl_on = d->reflection && q >= CW_QUALITY_MEDIUM;
  d->refr_on = d->refraction && q >= CW_QUALITY_HIGH;
  int w = (int)lroundf((float)ctx->width * sc), h = (int)lroundf((float)ctx->height * sc);
  t3_camera *cam = ctx->camera;
  const t3_mat4 *m = cw_node3d_global_transform(n);
  t3_vec3 wpos = { m->e[12], m->e[13], m->e[14] };
  t3_vec3 up = rot3(m, (t3_vec3){ 0, 1, 0 });
  t3_vec3 cpos = { cam->base.matrix_world.e[12], cam->base.matrix_world.e[13], cam->base.matrix_world.e[14] };
  /* the camera under the water sees no reflection from above (r186 skips the render) */
  bool below = v3dot(v3sub(wpos, cpos), up) > 0;
  bool first = !d->refl;
  d->refl = target(d->refl, d->refl_on ? w : 1, d->refl_on ? h : 1);
  /* always with a depth texture (1 x 1 when off): the surface's program samples one in every state */
  if (!cw__wgpu_on()) d->refr = refr_target(d->refr, d->refr_on ? w : 1, d->refr_on ? h : 1, true);
  d->refr_depth_ok = false;
  d->refr_above = false;
  if (!d->vcam) d->vcam = t3_perspective_camera_new(cam->fov, cam->aspect, cam->near, cam->far);
  t3_camera *v = d->vcam;
  v->fov = cam->fov, v->aspect = cam->aspect, v->near = cam->near, v->far = cam->far, v->zoom = cam->zoom;
  if (d->refl_on && !below) {
    /* r186 ReflectorNode.updateBefore, with the water's up as the normal */
    t3_vec3 view = v3add(v3neg(v3reflect(v3sub(wpos, cpos), up)), wpos);
    t3_vec3 look = v3add(rot3(&cam->base.matrix_world, (t3_vec3){ 0, 0, -1 }), cpos);
    t3_vec3 tgt = v3add(v3neg(v3reflect(v3sub(wpos, look), up)), wpos);
    v->base.matrix_world_external = false;
    v->base.position = view;
    v->base.up = v3reflect(rot3(&cam->base.matrix_world, (t3_vec3){ 0, 1, 0 }), up);
    t3_object_look_at(v, tgt.x, tgt.y, tgt.z);
    t3_object_update_matrix_world(v, true);
    v->projection_matrix = ctx->proj;
    oblique(&v->projection_matrix, &v->matrix_world_inverse, up, wpos);
    t3_mat4_invert(&v->projection_matrix_inverse, &v->projection_matrix);
    render_into(ctx, wt, d->refl, v);
  } else if (first || d->refl_on) {
    render_into(ctx, wt, d->refl, NULL); /* cleared: never sampled garbage */
  }
  if (d->refr_on && !below) {
    /* r186 Refractor: the view's own camera, clipped to below the rest level */
    t3_object_set_matrix_world(v, &cam->base.matrix_world);
    v->projection_matrix = ctx->proj;
    t3_mat4 vi;
    t3_mat4_invert(&vi, &cam->base.matrix_world);
    oblique(&v->projection_matrix, &vi, v3neg(up), wpos);
    t3_mat4_invert(&v->projection_matrix_inverse, &v->projection_matrix);
    render_refr(ctx, wt, d, v, w, h);
    memcpy(d->refr_inv, v->projection_matrix_inverse.e, sizeof d->refr_inv);
    d->refr_depth_ok = cw__wgpu_on() ? d->wg_dep_tex != NULL : d->refr->depth_texture != NULL;
    v->base.matrix_world_external = false;
  } else if (below && (d->refr_on || d->refl_on)) {
    /* under the water: the refraction target holds the view up through the surface (the scene above the rest
     * level), which the surface shows inside its window of light */
    /* (three.c's own target on both backends: it draws the sky behind the scene, and no depth is read from below) */
    d->refr = refr_target(d->refr, w, h, true);
    t3_object_set_matrix_world(v, &cam->base.matrix_world);
    v->projection_matrix = ctx->proj;
    t3_mat4 vi;
    t3_mat4_invert(&vi, &cam->base.matrix_world);
    oblique(&v->projection_matrix, &vi, up, wpos);
    t3_mat4_invert(&v->projection_matrix_inverse, &v->projection_matrix);
    render_into(ctx, wt, d->refr, v);
    memcpy(d->refr_inv, v->projection_matrix_inverse.e, sizeof d->refr_inv);
    d->refr_above = true;
    v->base.matrix_world_external = false;
  } else if (first || d->refr_on) {
    render_refr(ctx, wt, d, NULL, 1, 1);
  }
  wt->stats.reflection_w = d->refl_on ? w : 0;
  wt->stats.reflection_h = d->refl_on ? h : 0;
  wt->stats.refraction_w = d->refr_on ? w : 0;
  wt->stats.refraction_h = d->refr_on ? h : 0;
}

/* ── the extension's fill hook ──────────────────────────────────────── */

static void fill(t3_extension_ctx *c, void *user) {
  (void)user;
  const t3_object *o = t3_extension_object(c);
  water3d *d = o ? o->user_data : NULL;
  if (!d) return;
  float v[4];
  static const char *const wn[4] = { "waterWave0", "waterWave1", "waterWave2", "waterWave3" };
  for (int i = 0; i < 4; i++) {
    v[0] = d->ev.dx[i], v[1] = d->ev.dz[i], v[2] = d->ev.k[i], v[3] = d->ev.a[i];
    t3_extension_set_uniform(c, wn[i], v, 4);
  }
  t3_extension_set_uniform(c, "waterSteep", d->ev.q, 4);
  t3_extension_set_uniform(c, "waterPhase", d->ev.ph, 4);
  v[0] = d->reflectivity, v[1] = d->distortion, v[2] = d->detail, v[3] = d->normal_scale;
  t3_extension_set_uniform(c, "waterLook", v, 4);
  v[0] = d->color[0], v[1] = d->color[1], v[2] = d->color[2], v[3] = d->refl_on ? 1.0f : 0.0f;
  t3_extension_set_uniform(c, "waterTint", v, 4);
  v[0] = d->deep_color[0], v[1] = d->deep_color[1], v[2] = d->deep_color[2], v[3] = d->refr_on ? 1.0f : 0.0f;
  t3_extension_set_uniform(c, "waterDeep", v, 4);
  t3_texture *flow = cw_res_texture(d->flow_res);
  v[0] = d->flow_off[0], v[1] = d->flow_off[1], v[2] = d->flow_off[2], v[3] = flow ? 1.0f : 0.0f;
  t3_extension_set_uniform(c, "waterFlow", v, 4);
  v[0] = d->flow_direction[0], v[1] = d->flow_direction[1], v[2] = d->size[0], v[3] = d->size[1];
  t3_extension_set_uniform(c, "waterFlowDir", v, 4);
  v[0] = (float)d->debug_view, v[1] = d->debug_range[0], v[2] = d->debug_range[1], v[3] = d->murk;
  t3_extension_set_uniform(c, "waterDebug", v, 4);
  t3_extension_set_uniform(c, "waterCam", d->cam, 4);
  /* shore: floor map on, foam width, 1 / clarity, crest foam; shallow colour; the map's rectangle; foam clock */
  v[0] = d->floor_tex ? 1.0f : 0.0f, v[1] = d->shore_foam, v[2] = 1.0f / (d->clarity > 0.01f ? d->clarity : 0.01f), v[3] = d->crest_foam;
  t3_extension_set_uniform(c, "waterShore", v, 4);
  v[0] = d->shallow_color[0], v[1] = d->shallow_color[1], v[2] = d->shallow_color[2], v[3] = d->foam_time;
  t3_extension_set_uniform(c, "waterShallow", v, 4);
  v[0] = d->size[0], v[1] = d->size[1], v[2] = d->cam_under ? 1.0f : 0.0f, v[3] = 0;
  t3_extension_set_uniform(c, "waterFloorRect", v, 4);
  /* every texture the program declares is bound in every state (a draw with one unset is skipped): the
   * normal map stands in for maps a feature that is off never reads */
  t3_texture *nm0 = cw_res_texture(d->normal_res);
  t3_extension_set_texture(c, "waterFloorMap", d->floor_tex ? d->floor_tex : nm0);
  t3_extension_set_uniform(c, "waterSun", d->sun, 4);
  t3_extension_set_uniform(c, "waterSunCol", d->sun_col, 4);
  {
    bool on = d->rip_tex && d->rip_nx > 0;
    v[0] = on ? d->ripple_strength : 0.0f, v[1] = d->rip_ax, v[2] = d->rip_az, v[3] = d->glint;
    t3_extension_set_uniform(c, "waterRipple", v, 4);
    v[0] = d->ripple_center[0], v[1] = d->ripple_center[1], v[2] = on ? 1.0f / (float)d->rip_nx : 1.0f, v[3] = on ? 1.0f / (float)d->rip_nz : 1.0f;
    t3_extension_set_uniform(c, "waterRippleRect", v, 4);
    t3_extension_set_texture(c, "waterRippleMap", on ? d->rip_tex : nm0);
  }
  t3_texture *nm = cw_res_texture(d->normal_res);
  t3_extension_set_texture(c, "waterNormal", nm);
  t3_extension_set_texture(c, "waterFlowMap", flow ? flow : nm);
  if (d->refl) t3_extension_set_texture(c, "waterReflection", d->refl->texture);
  if (cw__wgpu_on() && !d->refr_above) {
    if (d->wg_col_tex) t3_extension_set_texture(c, "waterRefraction", d->wg_col_tex);
  } else if (d->refr) {
    t3_extension_set_texture(c, "waterRefraction", d->refr->texture);
  }
  {
    /* x: the refraction depth is valid; y: caustic strength (Medium and up); z: 1 / the caustic tile (m); w: the
     * camera is under the water (the refraction target holds the view up through the surface) */
    float cs = d->caustic_size > 0.05f ? d->caustic_size : 0.05f;
    v[0] = d->refr_depth_ok ? 1.0f : 0.0f, v[1] = cw__water_quality() >= CW_QUALITY_MEDIUM ? d->caustics : 0.0f;
    v[2] = 1.0f / cs, v[3] = d->refr_above ? 1.0f : 0.0f;
    t3_extension_set_uniform(c, "waterRefr", v, 4);
    t3_extension_set_uniform(c, "waterRefrInv", d->refr_inv, 16);
    {
      int tw = 1, th = 1;
      if (cw__wgpu_on() && d->wg_dep) tw = d->wg_dep->w, th = d->wg_dep->h;
      else if (!cw__wgpu_on() && d->refr) tw = d->refr->width, th = d->refr->height;
      float sz[4] = { (float)tw, (float)th, 0, 0 };
      t3_extension_set_uniform(c, "waterRefrSize", sz, 4);
    }
    /* the window depth under each pixel: GL's depth texture itself, WebGPU's R32Float copy (refraction_wgpu.c) */
    if (cw__wgpu_on()) {
      if (d->wg_dep_tex) t3_extension_set_texture(c, "waterRefractionDepth", d->wg_dep_tex);
    } else if (d->refr && d->refr->depth_texture) {
      t3_extension_set_texture(c, "waterRefractionDepth", d->refr->depth_texture);
    }
    t3_extension_set_texture(c, "waterCaustic", d->caus_tex ? d->caus_tex : nm0);
  }
}

/* ── the pass (SHADOW phase: before the scene) ─────────────────────── */

static void water_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  water_tree *wt = cw_tree_slot(ctx->tree, "water");
  if (!wt || !ctx->camera || !ctx->renderer) return;
  if (wt->bound != ctx->renderer) {
    t3_renderer_set_extension(ctx->renderer, "water", fill, NULL);
    wt->bound = ctx->renderer;
  }
  int tri = 0;
  wt->stats.surfaces = 0;
  t3_object_update_matrix_world(ctx->camera, true);
  for (int i = 0; i < wt->nodes.count; i++) {
    cw_node n = wt->nodes.data[i];
    water3d *d = cw_node_data_t(n, water_t);
    if (!d) continue;
    d->under_drawn = false; /* the full-screen pass (POST) sets it again when it draws this frame from under the water */
    build(n, d);
    textures(d);
    if (d->floor_dirty || (!d->floor_done && cw_tree_tick(ctx->tree) >= 1 && d->floor_map_size >= 2)) bake_floor(n, d);
    if (d->floor_dirty && d->floor_map_size < 2) d->floor_dirty = false;
    if (!d->mesh) continue;
    /* the waves drawn as the bodies are: between the last two ticks, at alpha */
    double dt = cw_fixed_dt();
    double t = ((double)cw_tree_tick(ctx->tree) - 1.0 + ctx->alpha) * dt + (double)d->time_offset;
    if (t < 0) t = 0;
    cw_waves w = cw__water_waves(d);
    cw__waves_prepare(&w, t, &d->ev);
    /* Water2's two-phase flow, from the time (no state carried) */
    const double cycle = 0.15, half = cycle * 0.5;
    double o0 = fmod((double)d->flow_speed * t, cycle);
    double o1 = fmod(o0 + half, cycle);
    d->foam_time = (float)fmod(t, 1000.0);
    d->flow_off[0] = (float)o0, d->flow_off[1] = (float)o1, d->flow_off[2] = (float)half;
    const float *cw = ctx->camera->base.matrix_world.e;
    d->cam[0] = cw[12], d->cam[1] = cw[13], d->cam[2] = cw[14];
    /* the WebGPU scene pass draws rows bottom-up (GL's order, pipeline_wgpu.h), three.c's own
     * targets top-down: the planar targets are read flipped there */
    d->cam[3] = cw_gpu_backend() == CW_GPU_WEBGPU ? 1.0f : 0.0f;
    {
      /* the camera under the surface (the drawn waves at its x, z): the surface is then seen from below */
      float ch = cw_water_height_at(n, cw[12], cw[14], t);
      d->cam_under = cw[13] < ch;
    }
    cw__ripples_upload(d);
    {
      cw_sky_state sk;
      cw_sky_get_state(ctx->tree, &sk);
      float amp = 0;
      for (int k = 0; k < 4; k++) amp += fabsf(d->ev.a[k]);
      d->sun[0] = sk.sun_direction.x, d->sun[1] = sk.sun_direction.y, d->sun[2] = sk.sun_direction.z, d->sun[3] = amp;
      float on = sk.active ? 1.0f : 0.0f;
      d->sun_col[0] = sk.sun_color.r * sk.sun_energy * on, d->sun_col[1] = sk.sun_color.g * sk.sun_energy * on;
      d->sun_col[2] = sk.sun_color.b * sk.sun_energy * on, d->sun_col[3] = d->scatter;
    }
    d->mat->roughness = d->roughness;
    /* the environment's specular is the reflection when no planar one is drawn */
    planar(ctx, wt, n, d);
    /* (and none from under the surface: what the surface mirrors there is the water itself, which the shader draws;
     * the environment's lower half is not that) */
    d->mat->env_map_intensity = d->refl_on || d->cam_under ? 0.0f : 1.0f;
    tri += d->triangles;
    wt->stats.surfaces++;
  }
  wt->stats.triangles = tri;
}

static void water_fixed(cw_node n, void *data, double dt) {
  (void)dt;
  water3d *d = data;
  cw__ripples_step(n, d, cw_tree_tick(cw_node_tree(n)));
}

static void water_enter(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  water_tree *wt = cw_tree_slot(t, "water");
  if (!wt) {
    wt = cw_calloc(1, sizeof *wt, CW_MEM_RENDER);
    cw_tree_set_slot(t, "water", wt, wtree_free);
  }
  cw_array_push(&wt->nodes, n);
  cw__water_add_under_pass(t);
  cw_pipeline *p = cw_pipeline_of(t);
  if (!cw_pipeline_has_pass(p, "water"))
    cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "water", .phase = CW_PHASE_SHADOW, .order = 1600,
                                             .execute = water_exec });
}

static void water_exit(cw_node n, void *data) {
  water3d *d = data;
  cw_tree *t = cw_node_tree(n);
  water_tree *wt = t ? cw_tree_slot(t, "water") : NULL;
  if (wt)
    for (int i = 0; i < wt->nodes.count; i++)
      if (wt->nodes.data[i].id == n.id) {
        memmove(&wt->nodes.data[i], &wt->nodes.data[i + 1], (size_t)(wt->nodes.count - i - 1) * sizeof(cw_node));
        wt->nodes.count--;
        break;
      }
  if (d->mesh) {
    t3_object_remove(cw_node3d_object(n), d->mesh);
    t3_release(d->mesh);
    d->mesh = NULL;
  }
}

#define WAVE_DOC "Gerstner wave: direction (degrees, 0 = +X, 90 = +Z), wavelength (m), amplitude (m), steepness (0..1)."
static const cw_prop water_props[] = {
  CW_PROP_FIELD(water3d, size, CW_T_VEC2, CW_PROP_DEFAULT, "Rectangle, x by z metres, centred on the node."),
  CW_PROP_FIELD_RANGE(water3d, horizon_distance, CW_T_FLOAT, 0, 1e7, "Optional outer grid extent; keeps the central size densely sampled."),
  CW_PROP_FIELD_RANGE(water3d, subdivisions, CW_T_INT, 1, 2048, "Grid quads along the longer side (quality: Low 1/4, Medium 1/2)."),
  CW_PROP_FIELD_RANGE(water3d, wave_count, CW_T_INT, 0, 4, "Waves in use (wave0 ..)."),
  CW_PROP_FIELD(water3d, wave0, CW_T_VEC4, CW_PROP_DEFAULT, WAVE_DOC),
  CW_PROP_FIELD(water3d, wave1, CW_T_VEC4, CW_PROP_DEFAULT, WAVE_DOC),
  CW_PROP_FIELD(water3d, wave2, CW_T_VEC4, CW_PROP_DEFAULT, WAVE_DOC),
  CW_PROP_FIELD(water3d, wave3, CW_T_VEC4, CW_PROP_DEFAULT, WAVE_DOC),
  CW_PROP_FIELD_RANGE(water3d, wave_speed, CW_T_FLOAT, 0, 10, "Multiplies the deep-water wave speed."),
  CW_PROP_FIELD(water3d, time_offset, CW_T_FLOAT, CW_PROP_DEFAULT, "Seconds added to the waves' clock."),
  CW_PROP_FIELD(water3d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Tint of what is seen through the water (linear)."),
  CW_PROP_FIELD(water3d, deep_color, CW_T_COLOR, CW_PROP_DEFAULT, "The water body's colour when no refraction is drawn (linear)."),
  CW_PROP_FIELD_RANGE(water3d, murk, CW_T_FLOAT, 0, 1, "Share of the refraction lost into deep_color (0: clear as glass)."),
  CW_PROP_FIELD_RANGE(water3d, reflectivity, CW_T_FLOAT, 0, 1, "Fresnel F0 (0.02: water)."),
  CW_PROP_FIELD_RANGE(water3d, distortion, CW_T_FLOAT, 0, 1, "How far the surface bends reflection and refraction."),
  CW_PROP_FIELD_RANGE(water3d, detail, CW_T_FLOAT, 0, 4, "Strength of the normal map ripples on the waves."),
  CW_PROP_FIELD_RANGE(water3d, roughness, CW_T_FLOAT, 0, 1, "Surface roughness for the lights' glints."),
  CW_PROP_FIELD(water3d, normal_map, CW_T_STRING, CW_PROP_DEFAULT, "Normal map image (empty: a seamless gen://noise normal map)."),
  CW_PROP_FIELD_RANGE(water3d, normal_scale, CW_T_FLOAT, 0.0001, 1000, "Normal map repeats per metre."),
  CW_PROP_FIELD(water3d, flow_direction, CW_T_VEC2, CW_PROP_DEFAULT, "Flow (x, z) without a flow map."),
  CW_PROP_FIELD_RANGE(water3d, flow_speed, CW_T_FLOAT, 0, 100, "How fast the normal maps scroll along the flow."),
  CW_PROP_FIELD(water3d, flow_map, CW_T_STRING, CW_PROP_DEFAULT, "Optional image: RG = flow direction (0.5 = still) over the rectangle."),
  CW_PROP_FIELD(water3d, reflection, CW_T_BOOL, CW_PROP_DEFAULT, "Planar reflection (Medium and up)."),
  CW_PROP_FIELD(water3d, refraction, CW_T_BOOL, CW_PROP_DEFAULT, "Planar refraction (High and up)."),
  CW_PROP_FIELD_RANGE(water3d, resolution_scale, CW_T_FLOAT, 0.05, 1, "Reflection / refraction targets' share of the view."),
  { .name = "debug_view", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(water3d, debug_view),
    .size = sizeof(int), .enum_names = debug_names, .doc = "HEIGHT: the surface height as colour (tests)." },
  CW_PROP_FIELD_RANGE(water3d, floor_map_size, CW_T_INT, 0, 512, "Cells along the longer side of the baked floor depth map (0: no shore shading)."),
  CW_PROP_FIELD_RANGE(water3d, floor_range, CW_T_FLOAT, 1, 1000, "Metres under the rest level the floor rays probe."),
  CW_PROP_FIELD(water3d, shallow_color, CW_T_COLOR, CW_PROP_DEFAULT, "Colour of the water over a shallow floor (linear)."),
  CW_PROP_FIELD_RANGE(water3d, clarity, CW_T_FLOAT, 0.01, 1000, "Metres of water over which the shallow colour has become deep_color."),
  CW_PROP_FIELD_RANGE(water3d, shore_foam, CW_T_FLOAT, 0, 20, "Width in metres of the foam where the water meets the floor (0: none)."),
  CW_PROP_FIELD_RANGE(water3d, crest_foam, CW_T_FLOAT, 0, 1, "Foam on crests where the surface bunches up (0: none)."),
  CW_PROP_FIELD_RANGE(water3d, scatter, CW_T_FLOAT, 0, 4, "Light the sun sends through wave crests toward the eye (the turquoise glow; needs a ProceduralSky)."),
  CW_PROP_FIELD_RANGE(water3d, glint, CW_T_FLOAT, 0, 8, "Strength of the fine sparkle the sun makes on the small ripples."),
  CW_PROP_FIELD_RANGE(water3d, ripple_cells, CW_T_INT, 0, 512, "Cells along the longer side of the interactive ripple grid (0: no ripples; quality scales it)."),
  CW_PROP_FIELD(water3d, ripple_size, CW_T_VEC2, CW_PROP_DEFAULT, "The rectangle the ripple grid covers, metres (0: the whole water)."),
  CW_PROP_FIELD(water3d, ripple_center, CW_T_VEC2, CW_PROP_DEFAULT, "Where that rectangle is centred, in the water node's x, z."),
  CW_PROP_FIELD_RANGE(water3d, ripple_speed, CW_T_FLOAT, 0, 50, "How fast ripples travel, m/s."),
  CW_PROP_FIELD_RANGE(water3d, ripple_damping, CW_T_FLOAT, 0, 60, "How fast ripples die, per second."),
  CW_PROP_FIELD_RANGE(water3d, ripple_strength, CW_T_FLOAT, 0, 8, "How much the ripples bend the surface normal."),
  CW_PROP_FIELD_RANGE(water3d, foam_fade, CW_T_FLOAT, 0, 60, "How fast the foam of splashes and wakes fades, per second."),
  CW_PROP_FIELD_RANGE(water3d, rain, CW_T_FLOAT, 0, 20000, "Raindrops per second over the ripple area (0: dry)."),
  CW_PROP_FIELD_RANGE(water3d, rain_size, CW_T_FLOAT, 0.01, 20, "Radius of a raindrop ring, metres."),
  CW_PROP_FIELD_RANGE(water3d, rain_strength, CW_T_FLOAT, 0, 5, "Depth of a raindrop dent, metres."),
  CW_PROP_FIELD(water3d, splash_fx, CW_T_STRING, CW_PROP_DEFAULT, "Path of a node whose GPUParticles3D children are restarted at each big splash (a pool, used in turn)."),
  CW_PROP_FIELD_RANGE(water3d, splash_fx_min, CW_T_FLOAT, 0, 100, "The splash depth (m) from which splash_fx fires."),
  CW_PROP_FIELD_RANGE(water3d, caustics, CW_T_FLOAT, 0, 4, "Strength of the moving light patterns the sun makes through the waves on what lies under the water (0: none; Medium and up)."),
  CW_PROP_FIELD_RANGE(water3d, caustic_size, CW_T_FLOAT, 0.05, 1000, "Size in metres of one tile of the caustic pattern."),
  CW_PROP_FIELD(water3d, underwater, CW_T_BOOL, CW_PROP_DEFAULT, "A camera under the surface sees through the water: colour and murk over distance, the surface from below, the waterline."),
  CW_PROP_FIELD(water3d, debug_range, CW_T_VEC2, CW_PROP_DEFAULT, "debug_view HEIGHT: heights mapped to 0 .. 1."),
  CW_PROP_END,
};

static cw_node_type water_type = {
  .name = "Water3D", .parent = "Node3D", .data_size = sizeof(water3d), .props = water_props,
  .doc = "Gerstner-wave water with planar reflection, refraction and flow (water.h).",
  .init = water_init, .fixed_update = water_fixed, .destroy = water_destroy, .enter_tree = water_enter, .exit_tree = water_exit,
};

void cw__register_water_types(void) {
  cw_node_type_register(&water_type);
  water_t = &water_type;
  cw__register_buoyancy_type();
}
