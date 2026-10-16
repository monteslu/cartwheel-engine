/* cw_noise (cartwheel/noise.h): FastNoiseLite's patterns, the job-pool
 * fill, domain warp and the gen://noise texture generator. Every result is
 * folded into unit.checksum, which the gate compares across hosts (native,
 * romdev, Chromium, wasmcart-native) and pins to a value: the noise is the
 * same bits everywhere, and a change of the library shows. */
#include <math.h>
#include <string.h>
#include <time.h>

#include "cartwheel/jobs.h"
#include "cartwheel/noise.h"
#include "cartwheel/resource.h"
#include "test.h"

static double wall_s(void) { /* a real clock (timings are reported, never compared) */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#define G 48

static void fold_grid(const float *v, int n) {
  uint64_t h = cw_noise_hash(v, (size_t)n);
  FOLD(h);
}

static void test_defaults(void) {
  cw_noise n = cw_noise_default();
  CHECK_EQ_INT(n.seed, 1337);
  CHECK(n.frequency == 0.01f);
  CHECK_EQ_INT(n.type, CW_NOISE_OPENSIMPLEX2);
  CHECK_EQ_INT(n.octaves, 3);
  CHECK_EQ_INT(n.cellular_return, CW_NOISE_CELL_DISTANCE);
  CHECK_EQ_INT(n.warp, CW_NOISE_WARP_OPENSIMPLEX2);
  CHECK_EQ_INT(cw_noise_type_from_name("cellular"), CW_NOISE_CELLULAR);
  CHECK_EQ_INT(cw_noise_type_from_name("nope"), -1);
  CHECK(!strcmp(cw_noise_type_name(CW_NOISE_VALUE_CUBIC), "value_cubic"));
}

/* every pattern x fractal, 2D and 3D: hashed grids, plus range and variety */
static void test_patterns(void) {
  static float v[G * G];
  for (int t = 0; t < CW_NOISE_TYPE_COUNT; t++)
    for (int f = 0; f < 4; f++)
      for (int d = 2; d <= 3; d++) {
        cw_noise n = cw_noise_default();
        n.type = (cw_noise_type)t;
        n.fractal = (cw_noise_fractal)f;
        n.octaves = 4;
        n.frequency = 0.05f;
        n.seed = 7 + t;
        float lo = 1e9f, hi = -1e9f;
        int k = 0;
        for (int y = 0; y < G; y++)
          for (int x = 0; x < G; x++) {
            /* negative coordinates too: FastNoiseLite's own floor differs from floorf there */
            float px = (float)x * 1.37f - 30.0f, py = (float)y * 1.11f - 20.0f;
            float s = d == 2 ? cw_noise_2d(&n, px, py) : cw_noise_3d(&n, px, py, 3.3f);
            v[k++] = s;
            lo = s < lo ? s : lo;
            hi = s > hi ? s : hi;
          }
        fold_grid(v, G * G);
        if (hi - lo < 0.05f) cw_error("pattern %s fractal %d %dD: flat (%g .. %g)", cw_noise_type_name(n.type), f, d, lo, hi);
        CHECK(hi - lo > 0.05f);
        if (t != CW_NOISE_CELLULAR) CHECK(lo >= -1.5f && hi <= 1.5f);
      }
}

static void test_cellular_modes(void) {
  static float v[G * G];
  for (int dist = 0; dist < CW_NOISE_DIST_COUNT; dist++)
    for (int ret = 0; ret < CW_NOISE_CELL_RETURN_COUNT; ret++) {
      cw_noise n = cw_noise_default();
      n.type = CW_NOISE_CELLULAR;
      n.cellular_distance = (cw_noise_cellular_distance)dist;
      n.cellular_return = (cw_noise_cellular_return)ret;
      n.frequency = 0.08f;
      cw_noise_fill_2d(&n, NULL, v, G, G, -10, -10, 1);
      fold_grid(v, G * G);
    }
}

/* the job-pool fill is the single-sample call, value for value */
static void test_fill(void) {
  static float a[G * G], b[G * G];
  cw_noise n = cw_noise_default();
  n.fractal = CW_NOISE_FRACTAL_FBM;
  n.octaves = 5;
  cw_noise_fill_2d(&n, NULL, a, G, G, -100, 50, 2.5f);
  int same = 0;
  for (int y = 0; y < G; y++)
    for (int x = 0; x < G; x++) same += a[y * G + x] == cw_noise_2d(&n, -100 + (float)x * 2.5f, 50 + (float)y * 2.5f);
  CHECK_EQ_INT(same, G * G);
  cw_noise_fill_3d_slice(&n, NULL, b, G, G, 0, 0, 12.5f, 1);
  same = 0;
  for (int y = 0; y < G; y++)
    for (int x = 0; x < G; x++) same += b[y * G + x] == cw_noise_3d(&n, (float)x, (float)y, 12.5f);
  CHECK_EQ_INT(same, G * G);
  fold_grid(a, G * G);
  fold_grid(b, G * G);
}

static void test_warp(void) {
  cw_noise w = cw_noise_default();
  float x = 10, y = 20, z = 30;
  w.warp = CW_NOISE_WARP_NONE;
  cw_noise_warp_2d(&w, &x, &y);
  CHECK(x == 10 && y == 20);
  for (int k = CW_NOISE_WARP_OPENSIMPLEX2; k < CW_NOISE_WARP_COUNT; k++)
    for (int f = 0; f < 3; f++) {
      w.warp = (cw_noise_warp)k;
      w.fractal = f == 0 ? CW_NOISE_FRACTAL_NONE : f == 1 ? CW_NOISE_FRACTAL_WARP_PROGRESSIVE : CW_NOISE_FRACTAL_WARP_INDEPENDENT;
      x = 10, y = 20, z = 30;
      cw_noise_warp_2d(&w, &x, &y);
      CHECK(x != 10 || y != 20);
      FOLD(x);
      FOLD(y);
      x = 10, y = 20;
      cw_noise_warp_3d(&w, &x, &y, &z);
      FOLD(x);
      FOLD(y);
      FOLD(z);
    }
  /* a warped fill */
  static float v[G * G];
  cw_noise n = cw_noise_default();
  w.warp = CW_NOISE_WARP_OPENSIMPLEX2;
  w.fractal = CW_NOISE_FRACTAL_WARP_PROGRESSIVE;
  cw_noise_fill_2d(&n, &w, v, G, G, 0, 0, 1);
  fold_grid(v, G * G);
}

static uint64_t image_hash(const char *q, int *w, int *h) {
  uint8_t *px = NULL;
  if (!cw_noise_image(q, &px, w, h)) return 0;
  uint64_t r = cw_hash_bytes(px, (size_t)*w * (size_t)*h * 4, 0);
  cw_free(px);
  return r;
}

static void test_image(void) {
  int w = 0, h = 0;
  static const char *const qs[] = {
    "gen://noise?size=64",
    "gen://noise?size=64x32&fractal=fbm&octaves=6&freq=0.04&seamless=1",
    "gen://noise?size=32&type=perlin&fractal=fbm&octaves=4&freq=0.1&out=normal&seamless=1",
    "gen://noise?size=32&type=cellular&cell_return=distance2_sub&freq=0.1",
    "gen://noise?size=32&out=rgba&fractal=ridged&freq=0.08",
    "gen://noise?size=32&warp=opensimplex2&warp_amp=8&warp_fractal=progressive&freq=0.05",
    "gen://noise?size=32&type=value_cubic&z=4.5&rotation=xz&min=-0.5&max=0.5",
  };
  for (size_t i = 0; i < CW_ARRAY_LEN(qs); i++) {
    uint64_t hh = image_hash(qs[i], &w, &h);
    CHECK(hh != 0);
    FOLD(hh);
  }
  image_hash(qs[1], &w, &h);
  CHECK_EQ_INT(w, 64);
  CHECK_EQ_INT(h, 32);
  /* bad queries fail (and log the key) */
  CHECK(!image_hash("gen://noise?size=0", &w, &h));
  CHECK(!image_hash("gen://noise?type=fuzzy", &w, &h));
  CHECK(!image_hash("gen://noise?colour=red", &w, &h));
  CHECK(!image_hash("gen://noise?freq=abc", &w, &h));
  /* seamless: the step across the wrap is like a step inside the image */
  uint8_t *px;
  CHECK(cw_noise_image("gen://noise?size=128&fractal=fbm&octaves=5&freq=0.03&seamless=1", &px, &w, &h));
  double inside = 0, wrap = 0;
  for (int y = 0; y < h; y++) {
    inside += fabs((double)px[(y * w + 64) * 4] - px[(y * w + 63) * 4]);
    wrap += fabs((double)px[(y * w + 0) * 4] - px[(y * w + w - 1) * 4]);
  }
  for (int x = 0; x < w; x++) wrap += fabs((double)px[x * 4] - px[((h - 1) * w + x) * 4]);
  wrap *= 0.5;
  cw_info("noise seamless: mean step inside %.2f, across the wrap %.2f", inside / h, wrap / h);
  CHECK(wrap < inside * 2 + h);
  /* contrast survives the blend */
  int lo = 255, hi = 0;
  for (int i = 0; i < w * h; i++) { lo = px[i * 4] < lo ? px[i * 4] : lo; hi = px[i * 4] > hi ? px[i * 4] : hi; }
  CHECK(hi - lo > 100);
  cw_free(px);
  /* a normal map points up on average */
  CHECK(cw_noise_image("gen://noise?size=32&type=perlin&freq=0.1&out=normal", &px, &w, &h));
  long bsum = 0;
  for (int i = 0; i < w * h; i++) bsum += px[i * 4 + 2];
  CHECK(bsum / (w * h) > 200);
  cw_free(px);
}

/* gen://noise through the resource cache: a texture, loaded once */
static void test_resource(void) {
  cw_res r = cw_res_load("gen://noise?size=64x16&fractal=fbm", CW_RES_TEXTURE);
  CHECK_EQ_INT(cw_res_get_state(r), CW_RES_STATE_READY);
  cw_texture_info ti;
  CHECK(cw_res_texture_info(r, &ti));
  CHECK_EQ_INT(ti.width, 64);
  CHECK_EQ_INT(ti.height, 16);
  CHECK(cw_res_texture(r) != NULL);
  cw_res again = cw_res_load("gen://noise?size=64x16&fractal=fbm", CW_RES_TYPE_AUTO); /* the type follows from the path */
  CHECK(again.id == r.id);
  cw_res bad = cw_res_load("gen://noise?size=99999", CW_RES_TEXTURE);
  CHECK_EQ_INT(cw_res_get_state(bad), CW_RES_STATE_FAILED);
  cw_res_release(again);
  cw_res_release(r);
  cw_res_release(bad);
}

/* cost per sample (reported, never compared) */
static void test_cost(void) {
  static float v[256 * 256];
  cw_noise n = cw_noise_default();
  double t0 = wall_s();
  cw_noise_fill_2d(&n, NULL, v, 256, 256, 0, 0, 1);
  double t1 = wall_s();
  n.fractal = CW_NOISE_FRACTAL_FBM;
  n.octaves = 5;
  cw_noise_fill_2d(&n, NULL, v, 256, 256, 0, 0, 1);
  double t2 = wall_s();
  cw_info("noise cost: 256x256 OpenSimplex2 %.2f ms, 5-octave FBM %.2f ms (%d threads)", (t1 - t0) * 1000,
          (t2 - t1) * 1000, cw_jobs_thread_count());
  /* one thread, ns a sample */
  static const cw_noise_type ts[] = { CW_NOISE_OPENSIMPLEX2, CW_NOISE_PERLIN, CW_NOISE_CELLULAR, CW_NOISE_VALUE };
  volatile float sink = 0;
  for (size_t i = 0; i < CW_ARRAY_LEN(ts); i++) {
    cw_noise m = cw_noise_default();
    m.type = ts[i];
    double a = wall_s();
    for (int k = 0; k < 200000; k++) sink += cw_noise_2d(&m, (float)k * 0.37f, (float)k * 0.11f);
    double b = wall_s();
    for (int k = 0; k < 200000; k++) sink += cw_noise_3d(&m, (float)k * 0.37f, (float)k * 0.11f, 4.0f);
    double c = wall_s();
    cw_info("noise cost: %s 2D %.1f ns, 3D %.1f ns a sample (one octave, one thread)", cw_noise_type_name(m.type),
            (b - a) * 1e9 / 200000, (c - b) * 1e9 / 200000);
  }
  (void)sink;
  CHECK(1);
}

CW_TEST_SUITE("noise", { "defaults", test_defaults }, { "patterns", test_patterns }, { "cellular", test_cellular_modes },
              { "fill", test_fill }, { "warp", test_warp }, { "image", test_image }, { "resource", test_resource },
              { "cost", test_cost })
