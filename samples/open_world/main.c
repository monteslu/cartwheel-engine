/* open_world: a 16.4 km square world streamed in around the player
 * (cartwheel/stream.h), with no loading screen.
 *
 * The world is 32 x 32 cells of 512 m. Each cell is one zstd-compressed
 * file (world/c_<x>_<z>.cell.zst, made at build time by gen-world.mjs):
 * heights and a control map for its terrain, and the props on it. A file
 * decompresses and parses on the job pool (the "cell" resource type below);
 * on_load gives the cell its terrain (a cw_terrain_set terrain, layers and
 * scatter shared) and its prop meshes; on_activate adds what physics needs
 * (the height fields' bodies, the props' boxes). The car drives a road
 * that crosses the whole world, or the camera flies across it.
 *
 * Precision: the stream keeps the world origin in double precision and
 * moves the local origin by whole cells when the player gets 1 km from it
 * (on_rebase: cw_world3d_shift_origin), so physics and the camera always
 * work near zero.
 *
 * OPEN_WORLD_MODE picks the run:
 *   drive  (default) the car drives the road west to east on an autopilot
 *          (steering toward a point ahead on the road, holding 25 m/s);
 *          every input is a pure function of the car's state, so the run
 *          is the same on every host; the camera chases it
 *   fly    the camera flies across the world at 150 m/s, 80 to 200 m above
 *          the ground (the streaming stress run)
 * OPEN_WORLD_QUALITY=low|medium|high|ultra picks the graphics preset.
 *
 * Debug state (compared across hosts): world.tick, stream.active,
 * stream.loaded, stream.hash (the activation history), stream.rebases,
 * stream.loads, stream.unloads, car.hash, car.km, origin.x, origin.z.
 * Reported, never compared: stream.stalls, stream.stall_ms, perf.* (frame
 * times: max, p50, p99, frames over budget), mem.* (memory over time). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/math.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/render3d.h"
#include "cartwheel/resource.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/stream.h"
#include "cartwheel/terrain.h"
#include "cartwheel/vehicle3d.h"

#define CELLS 32
#define CELL_M 512.0
#define QUADS 256
#define TILE 512

static cw_tree *tree;
static cw_world3d world;
static cw_stream *stream;
static cw_terrain_set *tset;
static cw_terrain *tmpl;
static cw_node cam, sun, car_body, car_wheel[4];
static bool fly;
static int src;

/* ── the cell resource: parsed on a worker ─────────────────────────── */

typedef struct prop {
  uint32_t kind;
  float x, z, yaw, sx, sy, sz;
  uint32_t seed;
} prop;

typedef struct cell_cpu {
  int cx, cz, n;
  float spacing, min_h, max_h;
  uint16_t *q;
  uint32_t *ctl;
  int nprops;
  prop *props;
} cell_cpu;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p) {
  uint32_t u = rd32(p);
  float f;
  memcpy(&f, &u, 4);
  return f;
}

static void *cell_decode(const cw_res_decode_args *a, size_t *cost) {
  const uint8_t *b = a->bytes;
  if (a->len < 40 || memcmp(b, "CWCL", 4) || rd32(b + 4) != 1) {
    cw_error("%s: not an open_world cell (rebuild the world: gen-world.mjs)", a->path);
    return NULL;
  }
  int n = (int)rd32(b + 16), np = (int)rd32(b + 32);
  size_t need = 40 + (size_t)n * n * 3 + (size_t)np * 32;
  if (n != QUADS + 1 || a->len < need) {
    cw_error("%s: %zu bytes, %zu expected", a->path, a->len, need);
    return NULL;
  }
  cell_cpu *c = cw_calloc(1, sizeof *c, CW_MEM_GAME);
  c->cx = (int)rd32(b + 8), c->cz = (int)rd32(b + 12), c->n = n;
  c->spacing = rdf(b + 20), c->min_h = rdf(b + 24), c->max_h = rdf(b + 28);
  c->q = cw_alloc(sizeof(uint16_t) * (size_t)n * n, CW_MEM_GAME);
  c->ctl = cw_alloc(sizeof(uint32_t) * (size_t)n * n, CW_MEM_GAME);
  const uint8_t *h = b + 40;
  for (int z = 0; z < n; z++) {
    uint16_t prev = 0;
    for (int x = 0; x < n; x++) {
      uint16_t d = (uint16_t)(h[0] | h[1] << 8);
      h += 2;
      prev = x ? (uint16_t)(prev + d) : d;
      c->q[z * n + x] = prev;
    }
  }
  for (int i = 0; i < n * n; i++) {
    uint8_t v = h[i];
    c->ctl[i] = cw_terrain_control(v & 3, (v >> 2) & 3, ((v >> 4) & 15) * 17, 0, 0, false);
  }
  const uint8_t *p = h + (size_t)n * n;
  c->nprops = np;
  c->props = cw_alloc(sizeof(prop) * (size_t)(np ? np : 1), CW_MEM_GAME);
  for (int i = 0; i < np; i++, p += 32)
    c->props[i] = (prop){ rd32(p), rdf(p + 4), rdf(p + 8), rdf(p + 12), rdf(p + 16), rdf(p + 20), rdf(p + 24), rd32(p + 28) };
  *cost = (size_t)n * n * 6;
  return c;
}
static void *cell_finish(void *d, cw_res self, const cw_res *deps, int nd, void *u) { return d; }
static void cell_free(void *d, void *u) {
  cell_cpu *c = d;
  cw_free(c->q);
  cw_free(c->ctl);
  cw_free(c->props);
  cw_free(c);
}
static size_t cell_size(const void *d, void *u) {
  const cell_cpu *c = d;
  return (size_t)c->n * c->n * 6 + (size_t)c->nprops * sizeof(prop);
}

/* ── per-cell content ──────────────────────────────────────────────── */

#define KINDS 3
static t3_geometry *prop_geo[KINDS];
static t3_material *prop_mat[KINDS];

typedef struct cell_rt {
  cw_terrain *t;
  t3_object *group;
  t3_instanced_mesh *im[KINDS];
  cw_body3d bodies[16];
  int nbodies;
} cell_rt;

static void *on_load(cw_stream *s, const cw_stream_cell *c, void *u) {
  const cell_cpu *cpu = cw_res_get(c->res);
  cell_rt *r = cw_calloc(1, sizeof *r, CW_MEM_GAME);
  r->t = cw_terrain_set_add(tset, c->id, c->local, cpu->q, cpu->ctl);
  r->group = t3_group_new();
  t3_object_set_position(r->group, c->local.x, c->local.y, c->local.z);
  int count[KINDS] = { 0 };
  for (int i = 0; i < cpu->nprops; i++)
    if (cpu->props[i].kind < KINDS) count[cpu->props[i].kind]++;
  for (int k = 0; k < KINDS; k++) {
    if (!count[k]) continue;
    r->im[k] = t3_instanced_mesh_new(prop_geo[k], prop_mat[k], count[k]);
    r->im[k]->mesh.base.cast_shadow = r->im[k]->mesh.base.receive_shadow = true;
    r->im[k]->mesh.base.frustum_culled = false; /* a few draws a cell; the cell is near when loaded */
    int at = 0;
    for (int i = 0; i < cpu->nprops; i++) {
      const prop *p = &cpu->props[i];
      if (p->kind != (uint32_t)k) continue;
      float y = cw_terrain_height_at(r->t, c->local.x + p->x, c->local.z + p->z) - c->local.y;
      float sn, cs;
      cw_sincos(p->yaw, &sn, &cs);
      float *m = t3_attribute_f32(r->im[k]->instance_matrix) + 16 * at++;
      /* column major: rotation about Y, scaled, standing on the ground (the unit meshes sit on y = 0) */
      m[0] = cs * p->sx, m[1] = 0, m[2] = -sn * p->sx, m[3] = 0;
      m[4] = 0, m[5] = p->sy, m[6] = 0, m[7] = 0;
      m[8] = sn * p->sz, m[9] = 0, m[10] = cs * p->sz, m[11] = 0;
      m[12] = p->x, m[13] = y - (k == 2 ? p->sy * 0.3f : 0.2f), m[14] = p->z, m[15] = 1;
    }
    r->im[k]->instance_matrix->version++;
    t3_object_add(r->group, r->im[k]);
  }
  t3_object_add(&cw_render3d_scene(tree)->base, r->group);
  return r;
}

static void on_unload(cw_stream *s, const cw_stream_cell *c, void *u) {
  cell_rt *r = c->data;
  if (!r) return;
  t3_object_remove(&cw_render3d_scene(tree)->base, r->group);
  for (int k = 0; k < KINDS; k++)
    if (r->im[k]) t3_release(r->im[k]);
  t3_release(r->group);
  cw_terrain_set_remove(tset, c->id);
  cw_free(r);
}

static void on_activate(cw_stream *s, const cw_stream_cell *c, void *u) {
  cell_rt *r = c->data;
  if (!r) return;
  cw_terrain_set_activate(tset, c->id, true);
  const cell_cpu *cpu = cw_res_get(c->res);
  cw_shape3d_def sd = cw_shape3d_default_def();
  for (int i = 0; i < cpu->nprops && r->nbodies < (int)CW_ARRAY_LEN(r->bodies); i++) {
    const prop *p = &cpu->props[i];
    float y = cw_terrain_height_at(r->t, c->local.x + p->x, c->local.z + p->z);
    float sn, cs;
    cw_sincos(p->yaw * 0.5f, &sn, &cs);
    cw_body3d_def bd = cw_body3d_default_def();
    bd.type = CW_BODY_STATIC;
    bd.position = (t3_vec3){ c->local.x + p->x, y + p->sy * 0.5f - 0.2f, c->local.z + p->z };
    bd.rotation = (t3_quat){ 0, sn, 0, cs };
    bd.name = "prop";
    cw_body3d b = cw_body3d_create(world, &bd);
    float rx = p->kind == 1 ? p->sx * 0.5f : p->sx * 0.5f, rz = p->kind == 1 ? p->sz * 0.5f : p->sz * 0.5f;
    cw_shape3d_box(b, &sd, (t3_vec3){ rx, p->sy * 0.5f, rz }, cw_xform3_identity());
    r->bodies[r->nbodies++] = b;
  }
}

static void on_deactivate(cw_stream *s, const cw_stream_cell *c, void *u) {
  cell_rt *r = c->data;
  if (!r) return;
  cw_terrain_set_activate(tset, c->id, false);
  for (int i = 0; i < r->nbodies; i++) cw_body3d_destroy(r->bodies[i]);
  r->nbodies = 0;
}

/* ── the car and the road ──────────────────────────────────────────── */

static double *road; /* x, y, z world metres, 8 m apart */
static int road_n;
static cw_vehicle3d car;
static cw_body3d chassis;
static int car_near;
static uint32_t car_hash;
static float car_speed, car_km;
static t3_vec3 chase;

static t3_vec3 road_local(int i) {
  i = CW_CLAMP(i, 0, road_n - 1);
  return cw_stream_to_local(stream, (cw_dvec3){ road[3 * i], road[3 * i + 1], road[3 * i + 2] });
}

static void on_rebase(cw_stream *s, t3_vec3 shift, void *u) {
  cw_world3d_shift_origin(world, shift);
  chase = t3_vec3_sub(chase, shift);
}

static bool cell_ready(cw_stream *s, const cw_stream_cell *c, bool wait, void *u) {
  return cw_terrain_set_ready(tset, c->id, wait);
}

static void on_cell_rebase(cw_stream *s, const cw_stream_cell *c, void *u) {
  cell_rt *r = c->data;
  if (!r) return;
  cw_terrain_set_move(tset, c->id, c->local);
  t3_object_set_position(r->group, c->local.x, c->local.y, c->local.z);
}

static void make_car(void) {
  t3_vec3 a = road_local(10), b = road_local(12);
  float dx = b.x - a.x, dz = b.z - a.z, yaw = cw_atan2(dx, dz), s, c;
  cw_sincos(yaw * 0.5f, &s, &c);
  cw_body3d_def bd = cw_body3d_default_def();
  bd.position = (t3_vec3){ a.x, a.y + 1.2f, a.z };
  bd.rotation = (t3_quat){ 0, s, 0, c };
  bd.sleep = false;
  bd.name = "car";
  chassis = cw_body3d_create(world, &bd);
  cw_shape3d_def sd = cw_shape3d_default_def();
  sd.density = 1200.0f / (1.8f * 0.7f * 4.4f);
  sd.material.friction = 0.4f;
  cw_shape3d_box(chassis, &sd, (t3_vec3){ 0.9f, 0.35f, 2.2f }, cw_xform3_identity());
  cw_vehicle3d_def vd = cw_vehicle3d_default_def();
  static const float wx[4] = { -0.85f, 0.85f, -0.85f, 0.85f }, wz[4] = { 1.4f, 1.4f, -1.4f, -1.4f };
  for (int i = 0; i < 4; i++) {
    int k = cw_vehicle3d_def_add_wheel(&vd, (t3_vec3){ wx[i], -0.2f, wz[i] }, i < 2 ? 0 : 1);
    vd.wheels[k].radius = 0.38f;
    vd.wheels[k].travel = 0.3f;
    vd.wheels[k].hertz = 1.8f;
    vd.wheels[k].grip = 1.4f;
  }
  vd.roll_influence = 0.3f;
  cw_vehicle3d_init(&car, chassis, &vd);
  car_near = 10;
  chase = (t3_vec3){ a.x - dx * 4, a.y + 4, a.z - dz * 4 };
}

static void drive(void) {
  cw_xform3 x = cw_body3d_transform(chassis);
  /* the nearest road point in a window ahead of the last (the car moves a few points a tick at most) */
  float bd = 1e30f;
  int best = car_near;
  for (int k = -4; k <= 12; k++) {
    t3_vec3 p = road_local(car_near + k);
    float dx = p.x - x.p.x, dz = p.z - x.p.z, d2 = dx * dx + dz * dz;
    if (d2 < bd) bd = d2, best = car_near + k;
  }
  car_near = CW_CLAMP(best, 0, road_n - 1);
  /* look further ahead the faster it goes */
  int ahead = 3 + (int)(car.speed * 0.35f);
  t3_vec3 target = road_local(car_near + ahead);
  t3_vec3 f = car.forward_world;
  float heading = cw_atan2(f.x, f.z), bearing = cw_atan2(target.x - x.p.x, target.z - x.p.z);
  float err = cw_wrap_angle(bearing - heading);
  float steer = cw_clampf(err * 1.4f, -0.45f, 0.45f);
  /* slow for bends: the road's turn over the next 120 m */
  t3_vec3 a = road_local(car_near), b = road_local(car_near + 7), c = road_local(car_near + 15);
  float t1 = cw_atan2(b.x - a.x, b.z - a.z), t2 = cw_atan2(c.x - b.x, c.z - b.z);
  float bend = fabsf(cw_wrap_angle(t2 - t1));
  float want = car_near >= road_n - 20 ? 0 : cw_clampf(25.0f - bend * 40.0f, 9.0f, 25.0f);
  float drive_n = car.speed < want ? 6500 : 0, brake = car.speed > want + 3 ? 4000 : 0;
  cw_vehicle3d_set_axle(&car, 0, steer, 0, brake);
  cw_vehicle3d_set_axle(&car, 1, 0, drive_n, brake);
  cw_vehicle3d_update(&car, (float)cw_fixed_dt());
  car_speed = car.speed;
  car_km = (float)car_near * 8.0f / 1000.0f;
  uint32_t h = 2166136261u;
  const uint8_t *bb = (const uint8_t *)&x;
  for (size_t i = 0; i < sizeof x; i++) h = (h ^ bb[i]) * 16777619u;
  car_hash = h;
}

static void draw_car(float alpha) {
  cw_xform3 x = cw_body3d_interpolated(chassis, alpha);
  cw_node3d_set_position(car_body, x.p);
  cw_node3d_set_quaternion(car_body, x.q);
  float s = 0.70710678f;
  t3_quat to_x = { 0, 0, s, s };
  for (int i = 0; i < 4; i++) {
    cw_xform3 wx = cw_vehicle3d_wheel_world(&car, i, alpha);
    cw_node3d_set_position(car_wheel[i], wx.p);
    cw_node3d_set_quaternion(car_wheel[i], t3_quat_multiply(wx.q, to_x));
  }
  t3_vec3 f = car.forward_world;
  t3_vec3 want = { x.p.x - f.x * 9, x.p.y + 3.4f, x.p.z - f.z * 9 };
  float e = 0.08f;
  chase.x += (want.x - chase.x) * e, chase.y += (want.y - chase.y) * e, chase.z += (want.z - chase.z) * e;
  cw_terrain *t = cw_terrain_set_at(tset, chase.x, chase.z);
  float gy = t ? cw_terrain_height_at(t, chase.x, chase.z) + 1.5f : chase.y;
  if (chase.y < gy) chase.y = gy;
  cw_node3d_set_position(cam, chase);
  cw_node3d_look_at(cam, (t3_vec3){ x.p.x + f.x * 5, x.p.y + 1, x.p.z + f.z * 5 }, (t3_vec3){ 0, 1, 0 });
  cw_node3d_set_position(sun, x.p); /* the shadow map covers shadow_extent metres around it */
}

/* ── the flight: west to east across the world, 150 m/s ────────────── */

static int32_t tick;
static double fly_alt = 120;

static bool loop, hover;
static int hover_tick; /* "ow hover <tick>": hold where the flight is at that tick */
static cw_dvec3 fly_at(int t) {
  double s = (double)(hover ? hover_tick : t) * (150.0 / 60.0); /* metres along (hover: none, the no-streaming baseline) */
  if (loop) { /* back and forth over the same 6 km: every cell loads and unloads again and again */
    double u = fmod(s, 12000.0);
    return (cw_dvec3){ u < 6000 ? -3000 + u : 9000 - u, 0, 500 };
  }
  double x = -7600 + s * 0.94, z = -6800 + s * 0.70;
  float sn, cs;
  cw_sincos((float)(s / 2500.0), &sn, &cs);
  z += 1600.0 * sn;
  return (cw_dvec3){ x, 0, z };
}

static void flight(void) {
  cw_dvec3 w = fly_at(tick), ahead = fly_at(tick + 90);
  t3_vec3 p = cw_stream_to_local(stream, w), q = cw_stream_to_local(stream, ahead);
  cw_terrain *t = cw_terrain_set_at(tset, p.x, p.z);
  float gy = t ? cw_terrain_height_at(t, p.x, p.z) : 0;
  float s, c;
  cw_sincos((float)tick / 700.0f, &s, &c);
  double want = gy + 140 + 60 * s;
  fly_alt += (want - fly_alt) * 0.02;
  if (fly_alt < gy + 40) fly_alt = gy + 40;
  p.y = (float)fly_alt;
  cw_node3d_set_position(cam, p);
  cw_node3d_look_at(cam, (t3_vec3){ q.x, (float)fly_alt - 60, q.z }, (t3_vec3){ 0, 1, 0 });
  cw_node3d_set_position(sun, p);
  cw_stream_source_move(stream, src, w);
}

/* ── the layers: tileable value noise (as terrain_demo) ────────────── */

static float lat(int x, int y, int period, uint32_t seed) {
  x = ((x % period) + period) % period;
  y = ((y % period) + period) % period;
  uint32_t h = seed * 0x9E3779B1u ^ (uint32_t)x * 0x85EBCA77u ^ (uint32_t)y * 0xC2B2AE3Du;
  h ^= h >> 15, h *= 0x2C1B3C6Du, h ^= h >> 12, h *= 0x297A2D39u, h ^= h >> 15;
  return (float)(h >> 8) / 16777216.0f;
}
static float vnoise(float x, float y, int period, uint32_t seed) {
  int ix = (int)floorf(x), iy = (int)floorf(y);
  float u = x - (float)ix, v = y - (float)iy;
  u = u * u * (3 - 2 * u), v = v * v * (3 - 2 * v);
  float a = lat(ix, iy, period, seed), b = lat(ix + 1, iy, period, seed), c = lat(ix, iy + 1, period, seed),
        d = lat(ix + 1, iy + 1, period, seed);
  return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}
static float fbm(int x, int y, int period, int oct, uint32_t seed) {
  float s = 0, a = 0.5f, n = 0;
  for (int o = 0; o < oct; o++) {
    int p = period << o;
    s += vnoise((float)x * (float)p / TILE, (float)y * (float)p / TILE, p, seed + (uint32_t)o) * a;
    n += a;
    a *= 0.5f;
  }
  return s / n;
}
typedef struct look { float c0[3], c1[3]; int period, oct; float rough, bump, crack; } look;
static void make_layer(const look *lk, uint32_t seed, uint8_t *alb, uint8_t *nrm) {
  float *h = malloc(sizeof(float) * TILE * TILE);
  for (int y = 0; y < TILE; y++)
    for (int x = 0; x < TILE; x++) {
      float v = fbm(x, y, lk->period, lk->oct, seed);
      if (lk->crack > 0) {
        float c = fbm(x, y, lk->period * 2, 3, seed ^ 77u), r = 1 - fabsf(c - 0.5f) * 2;
        v -= lk->crack * powf(r, 8);
      }
      h[y * TILE + x] = v;
    }
  for (int y = 0; y < TILE; y++)
    for (int x = 0; x < TILE; x++) {
      float v = h[y * TILE + x], t = v < 0 ? 0 : v > 1 ? 1 : v;
      uint8_t *a = &alb[4 * (y * TILE + x)];
      for (int k = 0; k < 3; k++) a[k] = (uint8_t)((lk->c0[k] + (lk->c1[k] - lk->c0[k]) * t) * 255);
      a[3] = (uint8_t)(t * 255);
      float dx = (h[y * TILE + (x + 1) % TILE] - h[y * TILE + (x + TILE - 1) % TILE]) * lk->bump;
      float dy = (h[((y + 1) % TILE) * TILE + x] - h[((y + TILE - 1) % TILE) * TILE + x]) * lk->bump;
      float l = sqrtf(dx * dx + dy * dy + 1);
      uint8_t *n = &nrm[4 * (y * TILE + x)];
      n[0] = (uint8_t)((-dx / l * 0.5f + 0.5f) * 255);
      n[1] = (uint8_t)((dy / l * 0.5f + 0.5f) * 255);
      n[2] = (uint8_t)((1 / l * 0.5f + 0.5f) * 255);
      n[3] = (uint8_t)(lk->rough * 255);
    }
  free(h);
}

static void make_template(void) {
  cw_terrain_def d = cw_terrain_default_def();
  d.size_x = d.size_z = 32;
  tmpl = cw_terrain_create(&d);
  static const look looks[4] = {
    { { 0.20f, 0.30f, 0.10f }, { 0.34f, 0.46f, 0.17f }, 6, 5, 0.9f, 3, 0 },      /* grass */
    { { 0.36f, 0.34f, 0.31f }, { 0.55f, 0.52f, 0.47f }, 3, 5, 0.85f, 6, 0.35f }, /* rock */
    { { 0.30f, 0.23f, 0.15f }, { 0.48f, 0.38f, 0.27f }, 5, 5, 0.9f, 4, 0 },      /* dirt */
    { { 0.80f, 0.82f, 0.86f }, { 0.94f, 0.95f, 0.97f }, 3, 4, 0.6f, 2, 0 },      /* snow */
  };
  static const float uv[4] = { 0.2f, 0.08f, 0.15f, 0.1f };
  uint8_t *a = malloc(TILE * TILE * 4), *n = malloc(TILE * TILE * 4);
  for (int i = 0; i < 4; i++) {
    make_layer(&looks[i], 100 + (uint32_t)i, a, n);
    cw_terrain_set_layer_pixels(tmpl, i, a, n, TILE, TILE, uv[i]);
  }
  free(a), free(n);
}

/* ── frame times and memory over time (reported, never compared) ──── */

#define HIST 2000 /* 0.05 ms bins up to 100 ms */
static uint32_t hist[HIST];
static int32_t frames_timed, over_budget;
static float perf_max, perf_p50, perf_p99, perf_budget = 1000.0f / 60.0f + 1.0f, perf_max_at;
static double last_t, last_cpu;
static uint32_t hist_cpu[HIST];
static float cpu_max, cpu_p99, cpu_p50;
static int32_t cpu_over;
static uint32_t hist_gpu[HIST];
static float gpu_max, gpu_p50, gpu_p99, draw_ms_max;
#define MEM_SAMPLES 64
static float mem_mb[MEM_SAMPLES], res_mb[MEM_SAMPLES], km_at[MEM_SAMPLES];
static int32_t mem_n;
static int32_t st_active, st_loaded, st_rebases, st_stalls, st_loads, st_unloads, st_start;
static float st_stall_ms, st_prepare_max, origin_x, origin_z, set_ms;
static uint32_t st_hash;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static double thread_cpu_ms(void) {
  struct timespec ts;
#ifdef CLOCK_THREAD_CPUTIME_ID
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
#else
  clock_gettime(CLOCK_MONOTONIC, &ts); /* a cart has no thread clock: wall time */
#endif
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static float process_mb(void) {
#if CW_CART
  return (float)(__builtin_wasm_memory_size(0) * 65536.0 / 1048576.0); /* the cart's linear memory */
#else
  FILE *f = fopen("/proc/self/statm", "r");
  long pages = 0, rss = 0;
  if (f) {
    if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0;
    fclose(f);
  }
  return (float)rss * 4096.0f / 1048576.0f;
#endif
}

static void pct(const uint32_t *h, float *p50, float *p99) {
  uint32_t total = 0, acc = 0;
  for (int i = 0; i < HIST; i++) total += h[i];
  *p50 = *p99 = 0;
  for (int i = 0; i < HIST && total; i++) {
    acc += h[i];
    if (!*p50 && acc >= total / 2) *p50 = (float)(i + 1) * 0.05f;
    if (!*p99 && acc >= total - total / 100) *p99 = (float)(i + 1) * 0.05f;
  }
}
static void percentile(void) {
  pct(hist, &perf_p50, &perf_p99);
  pct(hist_cpu, &cpu_p50, &cpu_p99);
  pct(hist_gpu, &gpu_p50, &gpu_p99);
}

/* ── the app ───────────────────────────────────────────────────────── */

/* ── the world starts at tick 3: after a test host's start requests (frame 2) chose the run ── */

static cw_res_type world_cell_type;
static float load_radius_set; /* "ow radius": overrides the preset's */
static bool profile; /* "ow profile" (or OW_PROFILE natively): log the main thread's slow frames */
#define START_TICK 3

static void ow_command(int argc, const char **argv, cw_reply *r) {
  if (argc >= 2 && (!strcmp(argv[1], "fly") || !strcmp(argv[1], "drive") || !strcmp(argv[1], "loop") || !strcmp(argv[1], "hover"))) {
    loop = !strcmp(argv[1], "loop");
    hover = !strcmp(argv[1], "hover");
    if (hover && argc >= 3) hover_tick = atoi(argv[2]);
    fly = loop || hover || !strcmp(argv[1], "fly");
  } else if (argc >= 3 && !strcmp(argv[1], "cache")) {
    cw_res_set_residency_budget((size_t)atof(argv[2]) * 1048576); /* the memory control: a cache that never evicts */
  } else if (argc >= 2 && !strcmp(argv[1], "profile")) {
    profile = true;
  } else if (argc >= 3 && !strcmp(argv[1], "radius")) {
    load_radius_set = (float)atof(argv[2]);
  } else {
    cw_reply_error(r, "usage: ow fly|drive|loop, or ow radius <metres>");
    return;
  }
  cw_reply_printf(r, "{\"mode\":\"%s\",\"radius\":%g}", loop ? "loop" : fly ? "fly" : "drive", (double)load_radius_set);
}

static void world_start(void) {
  cw_node root = cw_tree_root(tree);
  /* #region stream */
  cw_stream_desc sd = cw_stream_default_desc();
  sd.cells_x = sd.cells_z = CELLS;
  sd.cell_size = CELL_M;
  sd.origin = (cw_dvec3){ -CELLS * CELL_M / 2, 0, -CELLS * CELL_M / 2 };
  sd.path_format = "world/c_%d_%d.cell.zst";
  sd.cell_type = world_cell_type;
  /* what is drawn, by graphics preset (the fog closes in before it): visual only, so hosts on other
   * presets still activate the same cells on the same ticks */
  static const float load_r[CW_QUALITY_COUNT] = { 1200, 1500, 1800, 2200 };
  sd.load_radius = load_radius_set > 0 ? load_radius_set : load_r[cw_quality_get()];
  sd.active_radius = 600; /* what physics has */
  sd.hysteresis = 128;
  sd.max_requests_per_tick = 4;
  sd.prepare_per_frame = 1;
  sd.rebase_distance = 1024;
  sd.on_load = on_load;
  sd.on_unload = on_unload;
  sd.on_activate = on_activate;
  sd.on_deactivate = on_deactivate;
  sd.on_rebase = on_rebase;
  sd.on_cell_rebase = on_cell_rebase;
  sd.cell_ready = cell_ready;
  stream = cw_stream_create(&sd);
  /* #endregion */
  cw_node_set(cw_node_get(root, "World/Fog"), "density", cw_val_float(0.0011f * 1800 / CW_MAX(sd.load_radius, 900.0f)));
  /* the start position: its cells load (the first tick waits for them) and the local origin moves there */
  cw_dvec3 start = fly ? fly_at(0) : (cw_dvec3){ road[30], road[31], road[32] };
  src = cw_stream_source_add(stream, start, 0);
  cw_stream_tick(stream);
  if (!fly) make_car();
  else {
    cw_node_set(car_body, "visible", cw_val_bool(false));
    for (int i = 0; i < 4; i++) cw_node_set(car_wheel[i], "visible", cw_val_bool(false));
  }
}

static void init(void *u) {
  (void)u;
  const char *m = getenv("OPEN_WORLD_MODE");
  loop = m && !strcmp(m, "loop");
  fly = loop || (m && !strcmp(m, "fly"));
  profile = getenv("OW_PROFILE") != NULL;
  if (getenv("OPEN_WORLD_LOAD_RADIUS")) load_radius_set = (float)atof(getenv("OPEN_WORLD_LOAD_RADIUS"));
  const char *qs = getenv("OPEN_WORLD_QUALITY");
  for (int q = 0; qs && q < CW_QUALITY_COUNT; q++)
    if (!strcasecmp(qs, cw_quality_name((cw_quality)q))) cw_quality_set((cw_quality)q, false);
  tree = cw_scene_app_tree();
  world = cw_physics3d_world(tree);
  cw_node root = cw_tree_root(tree);
  cam = cw_node_get(root, "World/Camera");
  sun = cw_node_get(root, "World/Sun");
  car_body = cw_node_get(root, "World/Car");
  static const char *wn[4] = { "World/WheelFL", "World/WheelFR", "World/WheelRL", "World/WheelRR" };
  for (int i = 0; i < 4; i++) car_wheel[i] = cw_node_get(root, wn[i]);

  /* #region cell_type */
  cw_res_type cell = cw_res_register_type(&(cw_res_type_desc){
    .name = "owcell", .extensions = ".cell", .decode = cell_decode, .finish = cell_finish, .unload = cell_free,
    .size = cell_size });
  /* the cells that just left stay cached (CPU only: their parsed heights), so turning back costs nothing */
  cw_res_set_residency_budget(16u << 20);
  /* #endregion */

  make_template();
  /* #region terrain_set */
  cw_terrain_set_desc td = { 0 };
  td.def = cw_terrain_default_def();
  td.def.size_x = td.def.size_z = QUADS;
  td.def.region_size = QUADS;
  td.def.vertex_spacing = (float)(CELL_M / QUADS);
  td.def.min_height = -40, td.def.max_height = 560; /* gen-world.mjs's range: the same steps in every cell */
  td.def.far_distance = 500;
  td.layers = tmpl;
  td.world = world;
  td.shape = cw_shape3d_default_def();
  static const struct { int shape; float color[3], density, range, fade, lod; uint32_t layers; float slope; bool shadow, align; const char *knob; } sc[3] = {
    { CW_SCATTER_GRASS, { 0.55f, 0.7f, 0.35f }, 2.5f, 55, 15, 20, 1, 30, false, true, "grass" },
    { CW_SCATTER_ROCK, { 0.6f, 0.58f, 0.55f }, 0.008f, 220, 30, 60, 2, 60, true, true, "scatter" },
    { CW_SCATTER_TREE, { 1, 1, 1 }, 0.004f, 700, 100, 140, 1, 25, true, false, "scatter" },
  };
  for (int i = 0; i < 3; i++) {
    cw_scatter_def *d = &td.scatter[i];
    *d = cw_scatter_default_def();
    cw_scatter_def_builtin(d, sc[i].shape, sc[i].color[0], sc[i].color[1], sc[i].color[2]);
    d->density = sc[i].density;
    d->visible_range = sc[i].range;
    d->fade = sc[i].fade;
    for (int l = 0; l < CW_SCATTER_LODS; l++) d->lod_distance[l] = sc[i].lod * (float)(l + 1);
    d->layers = sc[i].layers;
    d->min_slope_cos = cosf(sc[i].slope * 3.14159265f / 180);
    d->cast_shadow = sc[i].shadow;
    d->align_to_normal = sc[i].align;
    d->density_knob = sc[i].knob;
    d->scale_min = 0.7f, d->scale_max = 1.3f;
    d->sink = 0.15f;
    d->seed = 1 + (uint32_t)i;
  }
  td.scatter_count = 3;
  tset = cw_terrain_set_create(tree, &td);
  for (int i = 0; i < 3; i++) cw_scatter_def_release(&td.scatter[i]); /* the set holds its own references */
  /* #endregion */

  /* props: a house (box), a stone tower (8-sided prism), a boulder (squat box) on y = 0 */
  static const uint32_t pc[KINDS] = { 0xb8a48a, 0x8c8a86, 0x6f6a62 };
  for (int k = 0; k < KINDS; k++) {
    prop_geo[k] = k == 1 ? t3_cylinder_geometry_new(0.45f, 0.5f, 1, 8, 1, false) : t3_box_geometry_new(1, 1, 1, 1, 1, 1);
    t3_attribute *pa = prop_geo[k]->attributes[T3_ATTR_POSITION];
    float *pos = t3_attribute_f32(pa);
    int nv = pa->count;
    for (int i = 0; i < nv; i++) pos[3 * i + 1] += 0.5f;
    prop_mat[k] = t3_mesh_standard_material_new(pc[k]);
    prop_mat[k]->roughness = 0.85f;
  }

  cw_debug_command("ow", "ow fly|drive|loop|hover, ow radius <metres>, ow cache <MB>, ow profile: the run (before tick 3, when the world starts)", ow_command);
  world_cell_type = cell;


  /* the road (world coordinates) */
  cw_res rr = cw_res_load("world/road.bin", CW_RES_BINARY);
  const cw_blob *rb = cw_res_blob(rr);
  road_n = rb && rb->len >= 4 ? (int)rd32(rb->data) : 0;
  road = malloc(sizeof(double) * 3 * (size_t)(road_n ? road_n : 1));
  if (rb) memcpy(road, rb->data + 4, sizeof(double) * 3 * (size_t)road_n);
  cw_res_release(rr);


  cw_debug_i32("world.tick", &tick);
  cw_debug_i32("stream.active", &st_active);
  cw_debug_i32("stream.loaded", &st_loaded);
  cw_debug_u32("stream.hash", &st_hash);
  cw_debug_i32("stream.rebases", &st_rebases);
  cw_debug_i32("stream.loads", &st_loads);
  cw_debug_i32("stream.unloads", &st_unloads);
  cw_debug_i32("stream.start_cells", &st_start);
  cw_debug_u32("car.hash", &car_hash);
  cw_debug_f32("car.km", &car_km);
  cw_debug_f32("car.speed", &car_speed);
  cw_debug_f32("origin.x", &origin_x);
  cw_debug_f32("origin.z", &origin_z);
  cw_debug_i32("stream.stalls", &st_stalls);
  cw_debug_f32("stream.stall_ms", &st_stall_ms);
  cw_debug_f32("stream.prepare_max_ms", &st_prepare_max);
  cw_debug_f32("perf.max_ms", &perf_max);
  cw_debug_f32("perf.max_at", &perf_max_at);
  cw_debug_f32("perf.p50_ms", &perf_p50);
  cw_debug_f32("perf.p99_ms", &perf_p99);
  cw_debug_f32("perf.budget_ms", &perf_budget);
  cw_debug_i32("perf.over_budget", &over_budget);
  cw_debug_i32("perf.frames", &frames_timed);
  cw_debug_f32("perf.cpu_max_ms", &cpu_max);
  cw_debug_f32("perf.gpu_max_ms", &gpu_max);
  cw_debug_f32("perf.gpu_p50_ms", &gpu_p50);
  cw_debug_f32("perf.gpu_p99_ms", &gpu_p99);
  cw_debug_f32("perf.draw_cpu_max_ms", &draw_ms_max);
  cw_debug_f32("perf.cpu_p50_ms", &cpu_p50);
  cw_debug_f32("perf.cpu_p99_ms", &cpu_p99);
  cw_debug_i32("perf.cpu_over_budget", &cpu_over);
  cw_debug_f32("perf.terrain_ms", &set_ms);
  cw_debug_f32_array("mem.process_mb", mem_mb, MEM_SAMPLES);
  cw_debug_f32_array("mem.resource_mb", res_mb, MEM_SAMPLES);
  cw_debug_f32_array("mem.km", km_at, MEM_SAMPLES);
  cw_debug_i32("mem.samples", &mem_n);
}

static double prof_tick, prof_load;
static void fixed_update(void *u, double dt) {
  (void)u, (void)dt;
  double p0 = thread_cpu_ms();
  tick++;
  if (!stream) {
    if (tick >= START_TICK) world_start();
    return;
  }
  if (fly) flight();
  else {
    drive();
    cw_stream_source_move_local(stream, src, cw_body3d_transform(chassis).p);
  }
  cw_stream_tick(stream);
  cw_stream_stats st;
  cw_stream_get_stats(stream, &st);
  st_active = st.active, st_loaded = st.loaded, st_rebases = st.rebases, st_stalls = st.stalls, st_start = st.start_cells;
  st_loads = (int32_t)st.loads, st_unloads = (int32_t)st.unloads;
  st_hash = (uint32_t)(st.activation_hash ^ (st.activation_hash >> 32));
  st_stall_ms = (float)st.stall_ms;
  st_prepare_max = (float)st.prepare_ms_max;
  cw_dvec3 o = cw_stream_origin(stream);
  origin_x = (float)o.x, origin_z = (float)o.z;
  prof_tick += thread_cpu_ms() - p0;
}

static void update(void *u, double dt) {
  (void)u, (void)dt;
  double p0 = thread_cpu_ms();
  cw_stream_frame(stream); /* on_load for cells whose file arrived (one a frame) */
  prof_load = thread_cpu_ms() - p0;
  /* frame to frame wall time, after the first second (the start position's load and first uploads) */
  double t = now_ms(), tc = thread_cpu_ms();
  uint64_t f = cw_frame_index();
  if (last_t > 0 && f > 60) {
    float cms = (float)(tc - last_cpu);
    int cb = (int)(cms / 0.05f);
    hist_cpu[cb < HIST ? cb : HIST - 1]++;
    if (cms > cpu_max) cpu_max = cms;
    if (cms > perf_budget) cpu_over++;
    float ms = (float)(t - last_t);
    if ((cms > 6 || ms > perf_budget) && profile) {
      cw_render3d_stats rs;
      cw_render3d_get_stats(tree, &rs);
      cw_info("slow frame %llu: wall %.2f, main CPU %.2f ms: ticks %.2f, on_load %.2f, terrain pass %.2f, draw %.2f", (unsigned long long)f, (double)ms, (double)cms, prof_tick, prof_load, (double)set_ms, (double)rs.cpu_ms);
    }
    int b = (int)(ms / 0.05f);
    hist[b < HIST ? b : HIST - 1]++;
    if (ms > perf_max) perf_max = ms, perf_max_at = (float)f;
    if (ms > perf_budget) over_budget++;
    frames_timed++;
    if ((frames_timed & 255) == 0) percentile();
  }
  last_t = t;
  last_cpu = tc;
  prof_tick = 0;
  /* memory over time: a sample every 600 frames */
  if (f % 600 == 0 && mem_n < MEM_SAMPLES) {
    cw_res_stats rs;
    cw_res_get_stats(&rs);
    mem_mb[mem_n] = process_mb();
    res_mb[mem_n] = (float)(rs.cpu_bytes / 1048576.0);
    km_at[mem_n] = fly ? (float)((double)tick * 150.0 / 60.0 / 1000.0) : car_km;
    cw_info("open_world: frame %llu, %.2f km: %d cells loaded, %d active; process %.1f MB, resources %.1f MB (%d cached); "
            "frame max %.2f ms, p99 %.2f, over %.2f ms: %d (main thread CPU max %.2f p50 %.2f p99 %.2f; GPU max %.2f p50 %.2f p99 %.2f; draw CPU max %.2f); stalls %d",
            (unsigned long long)f, (double)km_at[mem_n], st_loaded, st_active, (double)mem_mb[mem_n],
            (double)res_mb[mem_n], rs.cached, (double)perf_max, (double)perf_p99, (double)perf_budget, over_budget, (double)cpu_max, (double)cpu_p50, (double)cpu_p99, (double)gpu_max, (double)gpu_p50, (double)gpu_p99, (double)draw_ms_max,
            st_stalls);
    cw_mem_stats ms;
    cw_mem_get_stats(&ms);
    const t3_render_info *ri = cw_render3d_renderer(tree) ? t3_renderer_info(cw_render3d_renderer(tree)) : NULL;
    cw_info("open_world mem: total %.1f MB (game %.1f render %.1f physics %.1f resource %.1f general %.1f); three.c %u geometries %u textures %u programs",
            ms.total_bytes / 1048576.0, ms.bytes[CW_MEM_GAME] / 1048576.0, ms.bytes[CW_MEM_RENDER] / 1048576.0,
            ms.bytes[CW_MEM_PHYSICS] / 1048576.0, ms.bytes[CW_MEM_RESOURCE] / 1048576.0, ms.bytes[CW_MEM_GENERAL] / 1048576.0,
            ri ? ri->geometries : 0, ri ? ri->textures : 0, ri ? ri->programs : 0);
    mem_n++;
  }
}

static void render(void *u, double alpha) {
  (void)u;
  if (!stream) return;
  if (!fly) draw_car((float)alpha);
  cw_terrain_set_stats ts;
  cw_terrain_set_get_stats(tset, &ts);
  set_ms = ts.update_ms;
  float g = cw_pipeline_gpu_ms(cw_pipeline_of(tree));
  if (g > 0 && cw_frame_index() > 60) {
    int b = (int)(g / 0.05f);
    hist_gpu[b < HIST ? b : HIST - 1]++;
    if (g > gpu_max) gpu_max = g;
  }
  cw_render3d_stats rs;
  cw_render3d_get_stats(tree, &rs);
  if (cw_frame_index() > 60 && rs.cpu_ms > draw_ms_max) draw_ms_max = rs.cpu_ms;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  static cw_app_desc base = { .title = "open_world", .width = 1280, .height = 720, .debug = true, .init = init,
                              .fixed_update = fixed_update, .update = update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
