/* terrain_detail: Terrain3D's near-ground shading on a flat 256 m field
 * (cartwheel/terrain.h), for tests/terrain-detail.mjs to measure: the uv
 * rotation blended across control texels, the macro variation, the height
 * blend's sharpness and parallax occlusion.
 *
 * Two generated layers, tileable, in Terrain3D's packing: 0 is flagstones
 * (long slabs, high, with low dark joints between them: a pattern whose
 * rotation shows, and relief for parallax), 1 is sand (pale, low, smooth).
 * Both repeat every 4 m.
 *
 * Test switches (cw_test_env, applied the first frame they can be read):
 *   TD_PAINT   plain (flagstones, rotation 0: the default), rot (flagstones,
 *              a quarter turn on every other 4 m block, a checkerboard), pair
 *              (flagstones for x < 0, sand from x = 8, a blend between)
 *   TD_VIEW    play (default: from 8 m up and 20 m back, ground from 10 to
 *              80 m away), top (straight down from 34.64 m: 40 m of ground
 *              top to bottom), graze (1.6 m up, looking 10 m ahead), graze2
 *              (graze from 3 m to the side)
 *   TD_QUALITY verylow | low | medium | high (default) | ultra
 *   TD_MACRO_TINT, TD_MACRO_LAYER   cw_terrain_def.macro_tint, .macro_layer
 *   TD_SHARP   cw_terrain_def.blend_sharpness (set through cw_terrain_set_shading)
 *   TD_SHARP5  layer 5's own sharpness (no layer 5 is painted: a control)
 *   TD_PARALLAX  layers 0 and 1's parallax depth, metres
 *
 * Debug fields: td.applied (1 once the switches are in), td.gpu_ms (the
 * pipeline's GPU ms averaged from frame 60, with CW_GPU_TIMERS=1), td.frames
 * (frames in that average). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cartwheel/debug.h"
#include "cartwheel/math.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/terrain.h"

#define TILE 512

static cw_tree *tree;
static cw_node cam, terrain_node;
static int32_t applied, frames;
static float gpu_ms, gpu_sum, ground_y;

/* ── tileable noise ──────────────────────────────────────────────────── */

static uint32_t hash3(int x, int y, uint32_t seed) {
  uint32_t h = seed * 0x9E3779B1u ^ (uint32_t)x * 0x85EBCA77u ^ (uint32_t)y * 0xC2B2AE3Du;
  h ^= h >> 15, h *= 0x2C1B3C6Du, h ^= h >> 12, h *= 0x297A2D39u, h ^= h >> 15;
  return h;
}
static float lat(int x, int y, int period, uint32_t seed) {
  x = ((x % period) + period) % period;
  y = ((y % period) + period) % period;
  return (float)(hash3(x, y, seed) >> 8) / 16777216.0f;
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

/* ── the layers ──────────────────────────────────────────────────────── */

/* the normal (from the height, wrapped) and roughness into nrm */
static void normals_from(const float *h, float bump, float rough, uint8_t *nrm) {
  for (int y = 0; y < TILE; y++)
    for (int x = 0; x < TILE; x++) {
      float dx = (h[y * TILE + (x + 1) % TILE] - h[y * TILE + (x + TILE - 1) % TILE]) * bump;
      float dy = (h[((y + 1) % TILE) * TILE + x] - h[((y + TILE - 1) % TILE) * TILE + x]) * bump;
      float l = sqrtf(dx * dx + dy * dy + 1);
      uint8_t *n = &nrm[4 * (y * TILE + x)];
      n[0] = (uint8_t)((-dx / l * 0.5f + 0.5f) * 255);
      n[1] = (uint8_t)((dy / l * 0.5f + 0.5f) * 255);
      n[2] = (uint8_t)((1 / l * 0.5f + 0.5f) * 255);
      n[3] = (uint8_t)(rough * 255);
    }
}

/* flagstones: a 4 x 8 grid of jittered sites (slabs twice as long in x as in y), joints where the nearest two
 * are about as near; each slab its own tone and a little height */
static void flagstones(uint8_t *alb, uint8_t *nrm) {
  enum { GX = 4, GY = 8 };
  float sx[GX][GY], sy[GX][GY], tone[GX][GY];
  for (int i = 0; i < GX; i++)
    for (int j = 0; j < GY; j++) {
      sx[i][j] = ((float)i + 0.2f + 0.6f * (float)(hash3(i, j, 11) >> 8) / 16777216.0f) * TILE / GX;
      sy[i][j] = ((float)j + 0.2f + 0.6f * (float)(hash3(i, j, 12) >> 8) / 16777216.0f) * TILE / GY;
      tone[i][j] = (float)(hash3(i, j, 13) >> 8) / 16777216.0f;
    }
  float *h = malloc(sizeof(float) * TILE * TILE);
  for (int y = 0; y < TILE; y++)
    for (int x = 0; x < TILE; x++) {
      float d1 = 1e9f, d2 = 1e9f, t1 = 0;
      int ci = (int)((float)x * GX / TILE), cj = (int)((float)y * GY / TILE);
      for (int di = -1; di <= 1; di++)
        for (int dj = -1; dj <= 1; dj++) {
          int i = (ci + di + GX) % GX, j = (cj + dj + GY) % GY;
          /* the site in the neighbouring cell, wrapped to stay near (the tile repeats) */
          float px = sx[i][j] + (float)(ci + di - ((ci + di + GX) % GX)) * TILE / GX;
          float py = sy[i][j] + (float)(cj + dj - ((cj + dj + GY) % GY)) * TILE / GY;
          /* distances stretched along y: slabs long in x */
          float ex = ((float)x - px) * 0.5f, ey = (float)y - py;
          float d = sqrtf(ex * ex + ey * ey);
          if (d < d1) d2 = d1, d1 = d, t1 = tone[i][j];
          else if (d < d2) d2 = d;
        }
      float edge = d2 - d1; /* pixels from the joint (roughly) */
      float joint = edge < 3 ? 1 : edge < 9 ? 1 - (edge - 3) / 6 : 0;
      float grain = fbm(x, y, 8, 5, 21);
      float hv = (0.62f + 0.25f * t1 + 0.13f * grain) * (1 - joint) + 0.04f * joint;
      h[y * TILE + x] = hv;
      float c = (0.30f + 0.28f * t1 + 0.22f * grain) * (1 - joint * 0.75f);
      uint8_t *a = &alb[4 * (y * TILE + x)];
      a[0] = (uint8_t)(cw_clampf(c * 1.05f, 0, 1) * 255);
      a[1] = (uint8_t)(cw_clampf(c * 0.98f, 0, 1) * 255);
      a[2] = (uint8_t)(cw_clampf(c * 0.88f, 0, 1) * 255);
      a[3] = (uint8_t)(cw_clampf(hv, 0, 1) * 255);
    }
  normals_from(h, 6, 0.8f, nrm);
  free(h);
}

/* sand: pale, low and smooth */
static void sand(uint8_t *alb, uint8_t *nrm) {
  float *h = malloc(sizeof(float) * TILE * TILE);
  for (int y = 0; y < TILE; y++)
    for (int x = 0; x < TILE; x++) {
      float v = fbm(x, y, 6, 5, 31);
      h[y * TILE + x] = 0.25f + 0.3f * v;
      uint8_t *a = &alb[4 * (y * TILE + x)];
      a[0] = (uint8_t)((0.74f + 0.12f * v) * 255);
      a[1] = (uint8_t)((0.64f + 0.12f * v) * 255);
      a[2] = (uint8_t)((0.46f + 0.10f * v) * 255);
      a[3] = (uint8_t)((0.25f + 0.3f * v) * 255);
    }
  normals_from(h, 2, 0.95f, nrm);
  free(h);
}

static void paint(cw_terrain *t, const char *mode) {
  const cw_terrain_def *d = cw_terrain_get_def(t);
  int nx = d->size_x + 1, nz = d->size_z + 1;
  uint32_t *ctl = malloc(sizeof(uint32_t) * (size_t)nx * nz);
  bool rot = mode && !strcmp(mode, "rot"), pair = mode && !strcmp(mode, "pair");
  for (int z = 0; z < nz; z++)
    for (int x = 0; x < nx; x++) {
      int angle = rot && (((x >> 2) + (z >> 2)) & 1) ? 4 : 0;
      int blend = 0;
      if (pair) {
        /* sample x at world x - 128: flagstones to world x 0, sand from 8 */
        float wx = (float)x - 128;
        blend = (int)(cw_clampf(wx / 8.0f, 0, 1) * 255);
      }
      ctl[z * nx + x] = cw_terrain_control(0, pair ? 1 : 0, blend, angle, 0, false);
    }
  cw_terrain_set_control(t, 0, 0, nx, nz, ctl);
  free(ctl);
}

static float envf(const char *name, float dflt) {
  const char *s = cw_test_env(name);
  return s && *s ? (float)atof(s) : dflt;
}

/* the switches, the first frame they can be read */
static void apply(void) {
  cw_terrain *t = cw_terrain_of(terrain_node);
  if (!t) return;
  static const char *const qn[CW_QUALITY_COUNT] = { "verylow", "low", "medium", "high", "ultra" };
  const char *qs = cw_test_env("TD_QUALITY");
  cw_quality_set(CW_QUALITY_HIGH, false);
  for (int q = 0; qs && q < CW_QUALITY_COUNT; q++)
    if (!strcasecmp(qs, qn[q])) cw_quality_set((cw_quality)q, false);
  paint(t, cw_test_env("TD_PAINT"));
  cw_terrain_def sd = *cw_terrain_get_def(t);
  bool shading = false;
  if (cw_test_env("TD_MACRO_TINT")) sd.macro_tint = envf("TD_MACRO_TINT", 0), shading = true;
  if (cw_test_env("TD_MACRO_LAYER")) sd.macro_layer = envf("TD_MACRO_LAYER", 0), shading = true;
  if (cw_test_env("TD_SHARP")) sd.blend_sharpness = envf("TD_SHARP", 0.15f), shading = true;
  if (shading) cw_terrain_set_shading(t, &sd);
  if (cw_test_env("TD_SHARP5")) cw_terrain_set_layer_blend_sharpness(t, 5, envf("TD_SHARP5", 0));
  float pd = envf("TD_PARALLAX", 0);
  cw_terrain_set_layer_parallax(t, 0, pd);
  cw_terrain_set_layer_parallax(t, 1, pd);
  const char *v = cw_test_env("TD_VIEW");
  v = v ? v : "play";
  t3_vec3 pos = { 0, 8, 20 }, at = { 0, 0, -10 }, up = { 0, 1, 0 };
  if (!strcmp(v, "top")) pos = (t3_vec3){ 0, 34.641f, 0 }, at = (t3_vec3){ 0, 0, 0 }, up = (t3_vec3){ 0, 0, -1 };
  else if (!strcmp(v, "graze")) pos = (t3_vec3){ 0, 1.6f, 6 }, at = (t3_vec3){ 0, 0, -4 };
  else if (!strcmp(v, "graze2")) pos = (t3_vec3){ 3, 1.6f, 6 }, at = (t3_vec3){ 0, 0, -4 };
  cw_node3d_set_position(cam, pos);
  cw_node3d_look_at(cam, at, up);
  ground_y = cw_terrain_height_at(t, 0, 0);
  applied = 1;
}

static void init(void *u) {
  (void)u;
  tree = cw_scene_app_tree();
  cw_node root = cw_tree_root(tree);
  cam = cw_node_get(root, "World/Camera");
  terrain_node = cw_node_get(root, "World/Terrain");
  cw_terrain *t = cw_terrain_of(terrain_node);
  if (t) {
    uint8_t *a = malloc(TILE * TILE * 4), *n = malloc(TILE * TILE * 4);
    flagstones(a, n);
    cw_terrain_set_layer_pixels(t, 0, a, n, TILE, TILE, 0.25f);
    sand(a, n);
    cw_terrain_set_layer_pixels(t, 1, a, n, TILE, TILE, 0.25f);
    free(a), free(n);
    paint(t, NULL);
  }
  cw_node3d_set_position(cam, (t3_vec3){ 0, 8, 20 });
  cw_node3d_look_at(cam, (t3_vec3){ 0, 0, -10 }, (t3_vec3){ 0, 1, 0 });
  cw_debug_i32("td.applied", &applied);
  cw_debug_i32("td.frames", &frames);
  cw_debug_f32("td.gpu_ms", &gpu_ms);
  cw_debug_f32("td.ground", &ground_y);
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  if (!applied && cw_frame_index() >= 1) apply();
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  if (cw_frame_index() < 60) return;
  float g = cw_pipeline_gpu_ms(cw_pipeline_of(tree));
  if (g > 0) gpu_sum += g, frames++, gpu_ms = gpu_sum / (float)frames;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "terrain_detail", .width = 1920, .height = 1080, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
