/* terrain_demo: a 2 km x 2 km generated landscape (cartwheel/terrain.h)
 * from main.cwscene: a Terrain3D with four painted layers (grass, rock,
 * dirt, snow: generated here, tileable, Terrain3D's albedo + height and
 * normal + roughness packing), grass, rocks and trees scattered over it,
 * and the sky's sun casting shadows.
 *
 * The control map is painted here from the ground's slope and height, the
 * way a tool would: grass on gentle ground, rock on steep, dirt near the
 * water line, snow on high flat ground, each sample's two strongest layers
 * as base and overlay with their blend.
 *
 * TERRAIN_MODE picks the run:
 *   fly    (default) the camera flies a fixed path: low over the grass,
 *          up over a ridge, across to the far mountains
 * TERRAIN_QUALITY=low|medium|high|ultra picks the graphics preset.
 *   cracks a magenta clear colour, no sky, and the same flight: any crack
 *          between patches shows magenta below the horizon
 *          (terrain.magenta counts such pixels, natively)
 *
 * Debug state: terrain.tick, terrain.patches, terrain.far, terrain.hash
 * (the selection's hash), terrain.scatter (instances drawn), terrain.cells,
 * terrain.height (cw_terrain_height_at under the camera), and timings
 * (reported, never compared): terrain.select_ms, terrain.scatter_ms,
 * terrain.cpu_ms, terrain.gpu_ms, terrain.frame_ms. */
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
#include "cartwheel/terrain.h"

#define TILE 512

static cw_tree *tree;
static cw_node cam, terrain_node;
static int32_t tick, patches, far_patches, scatter, cells;
static uint32_t sel_hash;
static float height, select_ms, scatter_ms, cpu_ms, gpu_ms, frame_ms;
static bool cracks;

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
  cw_terrain_set_control(t, 0, 0, nx, nz, ctl);
  free(ctl);
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
  if (cracks) {
    cw_node_free(cw_node_get(root, "World/Sky"));
    cw_node_set(cw_node_get(root, "World/Env"), "background", cw_val_color(1, 0, 1, 1));
    cw_node_set(cw_node_get(root, "World/Env"), "use_background", cw_val_bool(true));
    cw_node_set(cw_node_get(root, "World/Terrain"), "far_distance", cw_val_float(100000));
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
  cw_debug_f32("terrain.frame_ms", &frame_ms);
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  cw_terrain *t = cw_terrain_of(terrain_node);
  if (!t) return;
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
  (void)alpha;
  cw_terrain *t = cw_terrain_of(terrain_node);
  if (!t) return;
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
