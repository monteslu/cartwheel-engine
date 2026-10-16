/* Water's CPU side (cartwheel/water.h): Gerstner displacement, the
 * height query that inverts the sideways motion, and the phase reduction.
 * Every result folds into unit.checksum, compared across hosts (the waves
 * feed buoyancy, so they must be the same bits everywhere). */
#include <math.h>
#include <time.h>

#include "cartwheel/water.h"
#include "test.h"

static cw_waves sea(void) {
  return (cw_waves){ .count = 3, .speed = 1,
                     .wave = { { 20, 12, 0.35f, 0.5f }, { 75, 7, 0.18f, 0.5f }, { -40, 3.5f, 0.06f, 0.4f } } };
}

/* the height over a displaced point is that point's height */
static void test_inverse(void) {
  cw_waves w = sea();
  float worst = 0;
  for (int i = 0; i < 40; i++) {
    float x0 = -30 + (float)i * 1.53f, z0 = 12 - (float)i * 0.71f;
    double t = 3.25 + i * 0.4;
    float p[3], n[3];
    cw_waves_displace(&w, x0, z0, t, p, n);
    float h = cw_waves_height(&w, p[0], p[2], t);
    float e = fabsf(h - p[1]);
    worst = e > worst ? e : worst;
    FOLD(h);
    FOLD(p);
    CHECK(n[1] > 0.5f && fabsf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] - 1) < 1e-4f);
  }
  cw_info("water: height query vs displaced point, worst %.6f m", worst);
  CHECK(worst < 2e-4f);
}

/* still water, one wave: the analytic form */
static void test_single(void) {
  cw_waves w = { .count = 1, .speed = 1, .wave = { { 0, 10, 0.5f, 0 } } };
  float p[3];
  /* steepness 0: a sine swell, no sideways motion */
  cw_waves_displace(&w, 2.5f, 0, 0, p, NULL);
  CHECK(fabsf(p[0] - 2.5f) < 1e-6f && fabsf(p[1] - 0.5f) < 1e-5f); /* sin(2 pi / 10 * 2.5) = 1 */
  /* the crest moves at sqrt(g / k) */
  double c = sqrt(9.81 / (6.283185307179586 / 10));
  float h0 = cw_waves_height(&w, 2.5f, 0, 0), h1 = cw_waves_height(&w, 2.5f + (float)c, 0, 1);
  CHECK(fabsf(h0 - h1) < 1e-4f);
  /* no waves: flat */
  cw_waves none = { .count = 0, .speed = 1 };
  CHECK(cw_waves_height(&none, 3, 4, 10) == 0);
  /* far into the run the phases stay exact: an hour in, the same as the time a period later */
  double period = 10 / c;
  float a = cw_waves_height(&w, 1, 0, 3600), b = cw_waves_height(&w, 1, 0, 3600 + period * 100);
  CHECK(fabsf(a - b) < 1e-3f);
  FOLD(a);
}

/* a grid of heights over a minute of time, hashed */
static void test_grid(void) {
  cw_waves w = sea();
  w.count = 4;
  w.wave[3] = (cw_water_wave){ 140, 2, 0.03f, 0.3f };
  for (int k = 0; k < 6; k++)
    for (int j = 0; j < 16; j++)
      for (int i = 0; i < 16; i++) {
        float h = cw_waves_height(&w, (float)i * 2.1f - 15, (float)j * 1.7f - 12, k * 10.25);
        FOLD(h);
      }
  CHECK(1);
}

static double wall_s(void) { /* a real clock (reported, never compared) */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void test_cost(void) {
  cw_waves w = sea();
  w.count = 4;
  w.wave[3] = (cw_water_wave){ 140, 2, 0.03f, 0.3f };
  volatile float sink = 0;
  double t0 = wall_s();
  for (int i = 0; i < 100000; i++) sink += cw_waves_height(&w, (float)(i % 300) * 0.3f, (float)(i / 300) * 0.2f, 12.5);
  double t1 = wall_s();
  cw_info("water cost: cw_waves_height with 4 waves %.2f us a query (one thread)", (t1 - t0) * 1e6 / 100000);
  (void)sink;
  CHECK(1);
}

CW_TEST_SUITE("water", { "inverse", test_inverse }, { "single", test_single }, { "grid", test_grid }, { "cost", test_cost })
