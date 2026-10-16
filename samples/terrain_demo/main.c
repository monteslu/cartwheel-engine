/* terrain_demo: a car on a 2 km x 2 km generated landscape
 * (cartwheel/terrain.h), from main.cwscene: a Terrain3D with four painted
 * layers (grass, rock, dirt, snow: generated here, tileable, Terrain3D's
 * albedo + height and normal + roughness packing), grass, rocks and trees
 * scattered over it, the sky's sun casting shadows, and a cw_vehicle3d car
 * on Box3D's height field driving a dirt track.
 *
 * The control map is painted here from slope and height, the way a tool
 * would: grass on gentle ground, rock on steep, dirt near the water line,
 * snow on high flat ground, each sample's two strongest layers as base and
 * overlay with their blend. The track is cut into the terrain the same way
 * an editor brush would: a closed loop whose ground is smoothed along it
 * and blended into the hills over its shoulders (cw_terrain_set_heights_m:
 * the physics regions it touches are rebuilt), painted dirt, and kept clear
 * of grass and trees (their scatter layers grow on grass only).
 *
 * TERRAIN_MODE picks the run:
 *   drive  (default) the car drives the track on an autopilot (steering
 *          toward a point ahead on the loop, holding about 15 m/s): every
 *          input is a pure function of the car's state, so the run is the
 *          same on every host; the camera chases it
 *   fly    the camera flies a fixed path, low over the grass, up over a
 *          ridge, across to the far mountains (the perf run)
 *   cracks the flight with a magenta clear colour and no sky: any crack
 *          between patches would show magenta below the horizon
 * TERRAIN_QUALITY=low|medium|high|ultra picks the graphics preset.
 *
 * Debug state: terrain.tick, terrain.patches, terrain.far, terrain.hash
 * (the selection's hash), terrain.scatter (instances drawn), terrain.cells,
 * terrain.height (cw_terrain_height_at under the camera), car.hash (FNV-1a
 * of the chassis transform's bits), car.contacts, car.speed, car.lap (track
 * points passed), and timings (reported, never compared):
 * terrain.select_ms, terrain.scatter_ms, terrain.cpu_ms, terrain.gpu_ms. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/terrain.h"
#include "cartwheel/vehicle3d.h"

#define TILE 512

static cw_tree *tree;
static cw_node cam, terrain_node;
static int32_t tick, patches, far_patches, scatter, cells;
static uint32_t sel_hash;
static float height, select_ms, scatter_ms, cpu_ms, gpu_ms;
static bool cracks, fly;

/* ── the track ─────────────────────────────────────────────────────── */

#define TRACK_N 256
#define ROAD_HALF 5.0f   /* metres either side, flat */
#define ROAD_EDGE 9.0f   /* then blended into the hills over this much */
static float track[TRACK_N][3]; /* x, road height, z */

static void make_track(cw_terrain *t) {
  /* a lumpy loop around the middle of the map (cw_sincos: the same on every host) */
  for (int i = 0; i < TRACK_N; i++) {
    float a = (float)i / TRACK_N * CW_TAU, s3, c3, s5, c5, sa, ca;
    cw_sincos(a, &sa, &ca);
    cw_sincos(3 * a + 0.7f, &s3, &c3);
    cw_sincos(5 * a + 2.1f, &s5, &c5);
    float r = 210 + 35 * s3 + 18 * c5;
    track[i][0] = 40 + ca * r * 1.15f;
    track[i][2] = -30 + sa * r;
    track[i][1] = cw_terrain_height_at(t, track[i][0], track[i][2]);
  }
  /* the road's height: the ground along it, smoothed twice over +-12 points */
  float tmp[TRACK_N];
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < TRACK_N; i++) {
      float sum = 0;
      for (int k = -16; k <= 16; k++) sum += track[(i + k + TRACK_N) % TRACK_N][1];
      tmp[i] = sum / 33;
    }
    for (int i = 0; i < TRACK_N; i++) track[i][1] = tmp[i];
  }
}

/* cut the road in: heights near the loop move to the road's, dirt painted on it */
static void cut_track(cw_terrain *t, uint32_t *ctl, int nx) {
  const cw_terrain_def *d = cw_terrain_get_def(t);
  float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
  for (int i = 0; i < TRACK_N; i++) {
    lo[0] = CW_MIN(lo[0], track[i][0]), hi[0] = CW_MAX(hi[0], track[i][0]);
    lo[1] = CW_MIN(lo[1], track[i][2]), hi[1] = CW_MAX(hi[1], track[i][2]);
  }
  int x0 = (int)((lo[0] - ROAD_EDGE - d->origin.x) / d->vertex_spacing), x1 = (int)((hi[0] + ROAD_EDGE - d->origin.x) / d->vertex_spacing) + 1;
  int z0 = (int)((lo[1] - ROAD_EDGE - d->origin.z) / d->vertex_spacing), z1 = (int)((hi[1] + ROAD_EDGE - d->origin.z) / d->vertex_spacing) + 1;
  int w = x1 - x0 + 1, h = z1 - z0 + 1;
  float *m = malloc(sizeof(float) * (size_t)w * h);
  for (int z = 0; z < h; z++)
    for (int x = 0; x < w; x++) {
      float px = d->origin.x + (float)(x0 + x) * d->vertex_spacing, pz = d->origin.z + (float)(z0 + z) * d->vertex_spacing;
      float best = 1e30f, by = 0;
      for (int i = 0; i < TRACK_N; i++) {
        const float *a = track[i], *b = track[(i + 1) % TRACK_N];
        float ex = b[0] - a[0], ez = b[2] - a[2], l2 = ex * ex + ez * ez;
        float u = l2 > 0 ? ((px - a[0]) * ex + (pz - a[2]) * ez) / l2 : 0;
        u = u < 0 ? 0 : u > 1 ? 1 : u;
        float qx = a[0] + ex * u - px, qz = a[2] + ez * u - pz, d2 = qx * qx + qz * qz;
        if (d2 < best) best = d2, by = a[1] + (b[1] - a[1]) * u;
      }
      float dist = sqrtf(best), orig = cw_terrain_height_at(t, px, pz) - d->origin.y;
      float k = dist <= ROAD_HALF ? 1 : dist >= ROAD_HALF + ROAD_EDGE ? 0 : 1 - cw_smoothstep(ROAD_HALF, ROAD_HALF + ROAD_EDGE, dist);
      m[z * w + x] = orig + ((by - d->origin.y) - orig) * k;
      if (dist < ROAD_HALF + 1.5f) {
        int b = dist < ROAD_HALF - 0.5f ? 255 : (int)((ROAD_HALF + 1.5f - dist) / 2 * 255);
        uint32_t *c = &ctl[(z0 + z) * nx + x0 + x];
        *c = cw_terrain_control((int)(*c >> 27), 2, b, 0, 0, false);
      }
    }
  cw_terrain_set_heights_m(t, x0, z0, w, h, m);
  free(m);
}

/* ── the car ───────────────────────────────────────────────────────── */

static cw_vehicle3d car;
static cw_body3d chassis;
static cw_node car_body, car_wheel[4], sun;
static int32_t car_contacts, car_lap, car_near;
static uint32_t car_hash;
static float car_speed;
static t3_vec3 chase;

static int nearest_track(t3_vec3 p, int hint) {
  /* search a window around the last index (the car moves a few points a second at most) */
  int best = hint;
  float bd = 1e30f;
  for (int k = -20; k <= 20; k++) {
    int i = (hint + k + TRACK_N) % TRACK_N;
    float dx = track[i][0] - p.x, dz = track[i][2] - p.z, d2 = dx * dx + dz * dz;
    if (d2 < bd) bd = d2, best = i;
  }
  return best;
}

static void make_car(cw_terrain *t) {
  cw_world3d w = cw_physics3d_world(tree);
  float x = track[0][0], z = track[0][2];
  float dx = track[2][0] - x, dz = track[2][2] - z, yaw = cw_atan2(dx, dz), s, c;
  cw_sincos(yaw * 0.5f, &s, &c);
  cw_body3d_def bd = cw_body3d_default_def();
  bd.position = (t3_vec3){ x, cw_terrain_height_at(t, x, z) + 1.2f, z };
  bd.rotation = (t3_quat){ 0, s, 0, c };
  bd.sleep = false;
  bd.name = "car";
  chassis = cw_body3d_create(w, &bd);
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
    vd.wheels[k].grip = 1.2f;
  }
  vd.roll_influence = 0.4f;
  cw_vehicle3d_init(&car, chassis, &vd);
  chase = (t3_vec3){ x - dx * 5, bd.position.y + 4, z - dz * 5 };
}

static void drive(void) {
  cw_xform3 x = cw_body3d_transform(chassis);
  car_near = nearest_track(x.p, car_near);
  const float *target = track[(car_near + 6) % TRACK_N];
  /* the target's bearing in the car's frame: forward (0, 0, 1), left (1, 0, 0) */
  t3_vec3 f = car.forward_world;
  float tx = target[0] - x.p.x, tz = target[2] - x.p.z;
  float heading = cw_atan2(f.x, f.z), bearing = cw_atan2(tx, tz);
  float err = cw_wrap_angle(bearing - heading);
  float steer = cw_clampf(err * 1.6f, -0.5f, 0.5f);
  float drive_n = car.speed < 15 ? 5200 : 0, brake = car.speed > 18 ? 3000 : 0;
  cw_vehicle3d_set_axle(&car, 0, steer, 0, brake);
  cw_vehicle3d_set_axle(&car, 1, 0, drive_n, brake);
  cw_vehicle3d_update(&car, 1.0f / 60);
  car_contacts = car.contacts;
  car_speed = car.speed;
  static int last = 0;
  int adv = (car_near - last + TRACK_N) % TRACK_N;
  if (adv > 0 && adv < TRACK_N / 2) car_lap += adv;
  last = car_near;
  uint32_t h = 2166136261u;
  const uint8_t *b = (const uint8_t *)&x;
  for (size_t i = 0; i < sizeof x; i++) h = (h ^ b[i]) * 16777619u;
  car_hash = h;
}

static void draw_car(float alpha) {
  cw_xform3 x = cw_body3d_interpolated(chassis, alpha);
  cw_node3d_set_position(car_body, x.p);
  cw_node3d_set_quaternion(car_body, x.q);
  cw_node3d_set_position(sun, x.p); /* the sun's shadow map covers shadow_extent metres around it */
  /* the cylinders' axis is Y; wheels turn about the chassis X */
  float s = 0.70710678f;
  t3_quat to_x = { 0, 0, s, s };
  for (int i = 0; i < 4; i++) {
    cw_xform3 wx = cw_vehicle3d_wheel_world(&car, i, alpha);
    cw_node3d_set_position(car_wheel[i], wx.p);
    cw_node3d_set_quaternion(car_wheel[i], t3_quat_multiply(wx.q, to_x));
  }
  /* the camera chases: behind and above, eased */
  t3_vec3 f = car.forward_world;
  t3_vec3 want = { x.p.x - f.x * 8, x.p.y + 3.2f, x.p.z - f.z * 8 };
  float e = 0.08f;
  chase.x += (want.x - chase.x) * e, chase.y += (want.y - chase.y) * e, chase.z += (want.z - chase.z) * e;
  cw_terrain *t = cw_terrain_of(terrain_node);
  float gy = t ? cw_terrain_height_at(t, chase.x, chase.z) + 1.5f : chase.y;
  if (chase.y < gy) chase.y = gy;
  cw_node3d_set_position(cam, chase);
  cw_node3d_look_at(cam, (t3_vec3){ x.p.x + f.x * 4, x.p.y + 1, x.p.z + f.z * 4 }, (t3_vec3){ 0, 1, 0 });
}

/* ── tileable value noise for the layer textures ─────────────────────── */

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
/* fBm over a tile: base period cells across, octaves doubling */
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

typedef struct look {
  float c0[3], c1[3]; /* dark and light colour (sRGB 0..1) */
  int period, oct;
  float rough, bump, crack;
} look;

/* albedo + height and normal + roughness, Terrain3D's packing */
static void make_layer(const look *lk, uint32_t seed, uint8_t *alb, uint8_t *nrm) {
  float *h = malloc(sizeof(float) * TILE * TILE);
  for (int y = 0; y < TILE; y++)
    for (int x = 0; x < TILE; x++) {
      float v = fbm(x, y, lk->period, lk->oct, seed);
      if (lk->crack > 0) {
        float c = fbm(x, y, lk->period * 2, 3, seed ^ 77u);
        float r = 1 - fabsf(c - 0.5f) * 2;
        v -= lk->crack * powf(r, 8);
      }
      h[y * TILE + x] = v;
    }
  for (int y = 0; y < TILE; y++)
    for (int x = 0; x < TILE; x++) {
      float v = h[y * TILE + x];
      float t = v < 0 ? 0 : v > 1 ? 1 : v;
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

static void paint(cw_terrain *t) {
  static const look looks[4] = {
    { { 0.20f, 0.30f, 0.10f }, { 0.34f, 0.46f, 0.17f }, 6, 5, 0.9f, 3, 0 },      /* grass */
    { { 0.36f, 0.34f, 0.31f }, { 0.55f, 0.52f, 0.47f }, 3, 5, 0.85f, 6, 0.35f }, /* rock */
    { { 0.30f, 0.23f, 0.15f }, { 0.48f, 0.38f, 0.27f }, 5, 5, 0.9f, 4, 0 },      /* dirt */
    { { 0.80f, 0.82f, 0.86f }, { 0.94f, 0.95f, 0.97f }, 3, 4, 0.6f, 2, 0 },      /* snow */
  };
  uint8_t *a = malloc(TILE * TILE * 4), *n = malloc(TILE * TILE * 4);
  const cw_terrain_def *d = cw_terrain_get_def(t);
  for (int i = 0; i < 4; i++) {
    make_layer(&looks[i], 100 + (uint32_t)i, a, n);
    static const float uv[4] = { 0.2f, 0.08f, 0.15f, 0.1f };
    cw_terrain_set_layer_pixels(t, i, a, n, TILE, TILE, uv[i]);
  }
  free(a), free(n);
  /* the control map from slope and height: two strongest layers per sample */
  int nx = d->size_x + 1, nz = d->size_z + 1;
  uint32_t *ctl = malloc(sizeof(uint32_t) * (size_t)nx * nz);
  for (int z = 0; z < nz; z++)
    for (int x = 0; x < nx; x++) {
      float wx = d->origin.x + (float)x * d->vertex_spacing, wz = d->origin.z + (float)z * d->vertex_spacing;
      float y = cw_terrain_height_at(t, wx, wz) - d->origin.y;
      float ny = cw_terrain_normal_at(t, wx, wz).y;
      float w[4];
      float steep = (0.88f - ny) / 0.12f;
      w[1] = steep < 0 ? 0 : steep > 1 ? 1 : steep;
      float low = (8 - y) / 6;
      w[2] = (low < 0 ? 0 : low > 1 ? 1 : low) * (1 - w[1]);
      float high = (y - 150) / 25;
      w[3] = (high < 0 ? 0 : high > 1 ? 1 : high) * (ny > 0.75f ? 1 : 0.2f);
      w[0] = 1 - w[1] - w[2];
      if (w[0] < 0) w[0] = 0;
      w[0] *= 1 - w[3];
      int b0 = 0, b1 = 1;
      for (int k = 0; k < 4; k++)
        if (w[k] > w[b0]) b0 = k;
      b1 = b0 == 0 ? 1 : 0;
      for (int k = 0; k < 4; k++)
        if (k != b0 && w[k] > w[b1]) b1 = k;
      float s = w[b0] + w[b1];
      int blend = s > 0 ? (int)(w[b1] / s * 255) : 0;
      ctl[z * nx + x] = cw_terrain_control(b0, b1, blend, 0, 0, false);
    }
  make_track(t);
  cut_track(t, ctl, nx);
  cw_terrain_set_control(t, 0, 0, nx, nz, ctl);
  free(ctl);
  cw_terrain_sync_physics(t);
  cw_terrain_scatter_invalidate(t);
}

/* ── the flight ──────────────────────────────────────────────────────── */

/* a closed path of control points (x, z, height above ground, look-ahead) */
static const float path[][3] = { { -400, -380, 2.5f }, { -250, -300, 3 },   { -120, -150, 12 }, { 0, 0, 60 },
                                 { 150, 120, 140 },    { 380, 260, 220 },   { 600, 300, 60 },   { 520, 520, 8 } };
#define NPATH ((int)(sizeof path / sizeof path[0]))
#define SEG_TICKS 120

static void pos_at(float tt, float out[3]) {
  int seg = (int)tt;
  float u = tt - (float)seg;
  const float *a = path[seg % NPATH], *b = path[(seg + 1) % NPATH];
  float s = u * u * (3 - 2 * u);
  out[0] = a[0] + (b[0] - a[0]) * s;
  out[1] = a[2] + (b[2] - a[2]) * s;
  out[2] = a[1] + (b[1] - a[1]) * s;
}

static void init(void *u) {
  (void)u;
  const char *m = getenv("TERRAIN_MODE");
  cracks = m && !strcmp(m, "cracks");
  fly = cracks || (m && !strcmp(m, "fly"));
  /* TERRAIN_QUALITY=low|medium|high|ultra: the graphics preset (terrain shading, scatter density) */
  const char *qs = getenv("TERRAIN_QUALITY");
  for (int q = 0; qs && q < CW_QUALITY_COUNT; q++)
    if (!strcasecmp(qs, cw_quality_name((cw_quality)q))) cw_quality_set((cw_quality)q, false);
  tree = cw_scene_app_tree();
  cw_node root = cw_tree_root(tree);
  cam = cw_node_get(root, "World/Camera");
  terrain_node = cw_node_get(root, "World/Terrain");
  cw_terrain *t = cw_terrain_of(terrain_node);
  if (t) paint(t);
  car_body = cw_node_get(root, "World/Car");
  sun = cw_node_get(root, "World/Sun");
  static const char *wn[4] = { "World/WheelFL", "World/WheelFR", "World/WheelRL", "World/WheelRR" };
  for (int i = 0; i < 4; i++) car_wheel[i] = cw_node_get(root, wn[i]);
  if (t && !fly) make_car(t);
  if (fly) {
    cw_node_set(car_body, "visible", cw_val_bool(false));
    for (int i = 0; i < 4; i++) cw_node_set(car_wheel[i], "visible", cw_val_bool(false));
  }
  cw_debug_u32("car.hash", &car_hash);
  cw_debug_i32("car.contacts", &car_contacts);
  cw_debug_f32("car.speed", &car_speed);
  cw_debug_i32("car.lap", &car_lap);
  if (cracks) {
    cw_node_free(cw_node_get(root, "World/Sky"));
    /* (no scatter: its silhouettes against the sky are not the terrain's) */
    cw_node_free(cw_node_get(root, "World/Terrain/Grass"));
    cw_node_free(cw_node_get(root, "World/Terrain/Rocks"));
    cw_node_free(cw_node_get(root, "World/Terrain/Trees"));
    cw_node_set(cw_node_get(root, "World/Env"), "background", cw_val_color(1, 0, 1, 1));
    cw_node_set(cw_node_get(root, "World/Env"), "use_background", cw_val_bool(true));
  }
  cw_debug_i32("terrain.tick", &tick);
  cw_debug_i32("terrain.patches", &patches);
  cw_debug_i32("terrain.far", &far_patches);
  cw_debug_u32("terrain.hash", &sel_hash);
  cw_debug_i32("terrain.scatter", &scatter);
  cw_debug_i32("terrain.cells", &cells);
  cw_debug_f32("terrain.height", &height);
  cw_debug_f32("terrain.select_ms", &select_ms);
  cw_debug_f32("terrain.scatter_ms", &scatter_ms);
  cw_debug_f32("terrain.cpu_ms", &cpu_ms);
  cw_debug_f32("terrain.gpu_ms", &gpu_ms);
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  cw_terrain *t = cw_terrain_of(terrain_node);
  if (!t) return;
  if (!fly) {
    drive();
    return;
  }
  float tt = (float)tick / SEG_TICKS, p[3], q[3];
  pos_at(tt, p);
  pos_at(tt + 0.35f, q);
  float gy = cw_terrain_height_at(t, p[0], p[2]);
  float qy = cw_terrain_height_at(t, q[0], q[2]);
  height = gy;
  cw_node_set(cam, "position", cw_val_vec3(p[0], gy + p[1], p[2]));
  cw_node3d_look_at(cam, (t3_vec3){ q[0], qy + q[1] * 0.6f, q[2] }, (t3_vec3){ 0, 1, 0 });
}

static void render(void *u, double alpha) {
  (void)u;
  cw_terrain *t = cw_terrain_of(terrain_node);
  if (!t) return;
  if (!fly) draw_car((float)alpha);
  cw_terrain_stats st;
  cw_terrain_get_stats(t, &st);
  patches = st.patches;
  far_patches = st.patches_far;
  sel_hash = (uint32_t)(st.selection_hash ^ (st.selection_hash >> 32));
  scatter = st.scatter_instances;
  cells = st.scatter_cells_visible;
  select_ms = st.select_ms;
  scatter_ms = st.scatter_ms;
  cw_render3d_stats rs;
  cw_render3d_get_stats(tree, &rs);
  cpu_ms = rs.cpu_ms;
  gpu_ms = cw_pipeline_gpu_ms(cw_pipeline_of(tree));
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "terrain_demo", .width = 1280, .height = 720, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
