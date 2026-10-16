/* Water3D (cartwheel/water.h): Gerstner waves on the CPU, the surface's
 * three.c mesh with the "water" extension (src/water/extensions/water.mjs),
 * and the planar reflection and refraction, rendered as three.js r186's
 * Reflector / Refractor (and ReflectorNode) render them: a mirrored camera
 * or the view's own, each with an oblique near plane (Lengyel) at the rest
 * level, before the scene pass. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cartwheel/gpu.h"
#include "cartwheel/math.h"
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

static const char *const debug_names[] = { "NONE", "HEIGHT", NULL };
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
  d->roughness = 0.08f;
  d->murk = 0.7f;
  d->normal_scale = 0.12f;
  d->flow_direction[0] = 1, d->flow_direction[1] = 0;
  d->flow_speed = 0.03f;
  d->reflection = d->refraction = true;
  d->resolution_scale = 0.5f;
  d->debug_range[0] = -2, d->debug_range[1] = 2;
}

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
}

/* ── the mesh ────────────────────────────────────────────────────────── */

/* the grid: nx x nz quads over the rectangle, in the node's XZ plane */
static t3_geometry *grid(float sx, float sz, int nx, int nz) {
  int vx = nx + 1, vz = nz + 1, nv = vx * vz;
  float *pos = cw_alloc((size_t)nv * 3 * sizeof(float), CW_MEM_RENDER);
  float *nrm = cw_alloc((size_t)nv * 3 * sizeof(float), CW_MEM_RENDER);
  float *uv = cw_alloc((size_t)nv * 2 * sizeof(float), CW_MEM_RENDER);
  for (int j = 0; j < vz; j++)
    for (int i = 0; i < vx; i++) {
      int k = j * vx + i;
      pos[k * 3] = -sx * 0.5f + sx * (float)i / (float)nx;
      pos[k * 3 + 1] = 0;
      pos[k * 3 + 2] = -sz * 0.5f + sz * (float)j / (float)nz;
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
  if (d->mesh && d->built_sub == sub && d->built_size[0] == sx && d->built_size[1] == sz && d->built_amp == amp) return;
  t3_object *group = cw_node3d_object(n);
  if (d->mesh) {
    t3_object_remove(group, d->mesh);
    t3_release(d->mesh);
    d->mesh = NULL;
  }
  int nx = sx >= sz ? sub : (int)ceilf((float)sub * sx / sz), nz = sz >= sx ? sub : (int)ceilf((float)sub * sz / sx);
  if (nx < 1) nx = 1;
  if (nz < 1) nz = 1;
  t3_geometry *g = grid(sx, sz, nx, nz);
  /* the waves lift and move the surface: bounds that hold every crest */
  t3_geometry_compute_bounding_sphere(g);
  g->bounding_sphere.radius += amp;
  if (!d->mat) {
    d->mat = t3_mesh_standard_material_new(0x000000);
    d->mat->metalness = 0;
    d->mat->emissive = (t3_color){ 1, 1, 1 }; /* the extension multiplies its light by it (white: as computed) */
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
  d->refr = target(d->refr, d->refr_on ? w : 1, d->refr_on ? h : 1);
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
    render_into(ctx, wt, d->refr, v);
    v->base.matrix_world_external = false;
  } else if (first || d->refr_on) {
    render_into(ctx, wt, d->refr, NULL);
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
  t3_texture *nm = cw_res_texture(d->normal_res);
  t3_extension_set_texture(c, "waterNormal", nm);
  t3_extension_set_texture(c, "waterFlowMap", flow ? flow : nm);
  if (d->refl) t3_extension_set_texture(c, "waterReflection", d->refl->texture);
  if (d->refr) t3_extension_set_texture(c, "waterRefraction", d->refr->texture);
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
    build(n, d);
    textures(d);
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
    d->flow_off[0] = (float)o0, d->flow_off[1] = (float)o1, d->flow_off[2] = (float)half;
    const float *cw = ctx->camera->base.matrix_world.e;
    d->cam[0] = cw[12], d->cam[1] = cw[13], d->cam[2] = cw[14];
    /* the WebGPU scene pass draws rows bottom-up (GL's order, pipeline_wgpu.h), three.c's own
     * targets top-down: the planar targets are read flipped there */
    d->cam[3] = cw_gpu_backend() == CW_GPU_WEBGPU ? 1.0f : 0.0f;
    d->mat->roughness = d->roughness;
    /* the environment's specular is the reflection when no planar one is drawn */
    planar(ctx, wt, n, d);
    d->mat->env_map_intensity = d->refl_on ? 0.0f : 1.0f;
    tri += d->triangles;
    wt->stats.surfaces++;
  }
  wt->stats.triangles = tri;
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
  CW_PROP_FIELD(water3d, debug_range, CW_T_VEC2, CW_PROP_DEFAULT, "debug_view HEIGHT: heights mapped to 0 .. 1."),
  CW_PROP_END,
};

static cw_node_type water_type = {
  .name = "Water3D", .parent = "Node3D", .data_size = sizeof(water3d), .props = water_props,
  .doc = "Gerstner-wave water with planar reflection, refraction and flow (water.h).",
  .init = water_init, .destroy = water_destroy, .enter_tree = water_enter, .exit_tree = water_exit,
};

void cw__register_water_types(void) {
  cw_node_type_register(&water_type);
  water_t = &water_type;
  cw__register_buoyancy_type();
}
