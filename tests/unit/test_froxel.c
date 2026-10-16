/* Froxel grid (cartwheel/froxel.h): binning against a brute-force
 * reference, coverage (a point inside a light finds the light in its cell),
 * cone culling, overflow order, and a hash of the binned lists folded into
 * unit.checksum, which the gate requires to be equal on every host (and
 * tests/froxel-determinism.mjs at several worker counts). Also times a
 * 1080p bin of 256 lights (logged, not checked). */
#include <math.h>
#include <string.h>
#include <time.h>

#include "cartwheel/froxel.h"
#include "cartwheel/math.h"
#include "test.h"

#define W 1920
#define H 1080

static float proj[16];
static float near_ = 0.1f, far_ = 400;

/* three.js PerspectiveCamera(60, 16 / 9, near, far).projectionMatrix, written out (no libm tan: the
 * same bits on every host) */
static void make_proj(void) {
  const float f = 1.7320508f, aspect = (float)W / (float)H;
  memset(proj, 0, sizeof proj);
  proj[0] = f / aspect;
  proj[5] = f;
  proj[10] = (far_ + near_) / (near_ - far_);
  proj[11] = -1;
  proj[14] = 2 * far_ * near_ / (near_ - far_);
}

/* random view-space lights in front of the camera: n spheres, every third a cone */
static void lights(cw_froxel_volume *v, int n, uint64_t seed) {
  cw_rng r;
  cw_rng_seed(&r, seed);
  for (int i = 0; i < n; i++) {
    float z = -cw_rng_range(&r, 0.5f, 150), half = -z * 0.6f;
    v[i] = (cw_froxel_volume){ cw_rng_range(&r, -half * 1.8f, half * 1.8f), cw_rng_range(&r, -half, half), z,
                               cw_rng_range(&r, 1, 14), { 0, 0, 0 }, -2, 0 };
    if (i % 3 == 2) {
      float dx = cw_rng_range(&r, -1, 1), dy = cw_rng_range(&r, -1, 0.2f), dz = cw_rng_range(&r, -1, 1);
      float l = sqrtf(dx * dx + dy * dy + dz * dz);
      if (l < 1e-3f) dx = 0, dy = -1, dz = 0, l = 1;
      v[i].dir[0] = dx / l, v[i].dir[1] = dy / l, v[i].dir[2] = dz / l;
      float a = cw_rng_range(&r, 0.15f, 1.2f);
      v[i].cos_angle = cw_cos(a);
      v[i].sin_angle = cw_sin(a);
    }
  }
}

static bool sphere_box(const cw_froxel_volume *v, const float mn[3], const float mx[3]) {
  float c[3] = { v->x, v->y, v->z }, d2 = 0;
  for (int a = 0; a < 3; a++) {
    float d = c[a] < mn[a] ? mn[a] - c[a] : c[a] > mx[a] ? c[a] - mx[a] : 0;
    d2 += d * d;
  }
  return d2 <= v->radius * v->radius;
}

static bool in_list(const cw_froxel_grid *g, int cell, int v) {
  const uint32_t *rec = cw_froxel_records(g, NULL);
  const uint16_t *items = cw_froxel_items(g, NULL);
  uint32_t first = rec[cell] >> 8, n = rec[cell] & 255;
  for (uint32_t k = 0; k < n; k++)
    if (items[first + k] == v) return true;
  return false;
}

static void test_grid(void) {
  make_proj();
  cw_froxel_grid *g = cw_froxel_grid_new();
  cw_froxel_config c = CW_FROXEL_CONFIG_DEFAULT;
  cw_froxel_grid_setup(g, &c, W, H, proj, near_, far_);
  int nx, ny, nz;
  float zs, zb;
  cw_froxel_grid_dims(g, &nx, &ny, &nz, &zs, &zb);
  CHECK_EQ_INT(nx, 60);
  CHECK_EQ_INT(ny, 34);
  CHECK_EQ_INT(nz, 24);
  /* slices: exponential from near to far */
  CHECK(fabsf(cw_froxel_slice_depth(g, 0) - near_) < 1e-6f);
  CHECK(fabsf(cw_froxel_slice_depth(g, 24) - far_) < 1e-3f);
  CHECK(fabsf(cw_froxel_slice_depth(g, 12) - sqrtf(near_ * far_)) < 0.01f);
  /* a point on the axis at depth 10 lands in the middle tiles and its slice */
  int cell = cw_froxel_cell_of(g, 0.001f, 0.001f, -10);
  int k = (int)floorf(cw_ln(10) * zs + zb);
  CHECK_EQ_INT(cell, 30 + 60 * (17 + 34 * k));
  float mn[3], mx[3];
  cw_froxel_cell_box(g, cell, mn, mx);
  CHECK(mn[2] <= -10 && mx[2] >= -10 && mn[0] <= 0.001f && mx[0] >= 0.001f);
  CHECK_EQ_INT(cw_froxel_cell_of(g, 0, 0, -0.01f), -1); /* nearer than near */
  cw_froxel_grid_free(g);
}

static void test_bin(void) {
  make_proj();
  enum { N = 256 };
  static cw_froxel_volume v[N];
  lights(v, N, 7);
  cw_froxel_grid *g = cw_froxel_grid_new();
  cw_froxel_config c = CW_FROXEL_CONFIG_DEFAULT;
  c.max_per_froxel = 255; /* no overflow: every list is complete */
  cw_froxel_grid_setup(g, &c, W, H, proj, near_, far_);
  cw_froxel_grid_bin(g, v, N);
  cw_froxel_stats st;
  cw_froxel_grid_stats(g, &st);
  CHECK_EQ_INT(st.volumes, N);
  CHECK_EQ_INT(st.dropped, 0);
  CHECK(st.references > N);
  int cells;
  const uint32_t *rec = cw_froxel_records(g, &cells);
  const uint16_t *items = cw_froxel_items(g, NULL);
  /* every list: increasing light order */
  int bad_order = 0, total = 0;
  for (int i = 0; i < cells; i++) {
    uint32_t first = rec[i] >> 8, n = rec[i] & 255;
    CHECK(first == (uint32_t)total || n == 0);
    total += (int)n;
    for (uint32_t k = 1; k < n; k++)
      if (items[first + k] <= items[first + k - 1]) bad_order++;
  }
  CHECK_EQ_INT(bad_order, 0);
  CHECK_EQ_INT(total, st.references);
  /* coverage: points inside each light (and inside its cone) find it in their cell */
  cw_rng r;
  cw_rng_seed(&r, 99);
  int missed = 0, tested = 0;
  for (int i = 0; i < N; i++)
    for (int s = 0; s < 64; s++) {
      float dx = cw_rng_range(&r, -1, 1), dy = cw_rng_range(&r, -1, 1), dz = cw_rng_range(&r, -1, 1);
      float l = sqrtf(dx * dx + dy * dy + dz * dz);
      if (l > 1 || l < 1e-3f) continue;
      float d = cw_rng_range(&r, 0, 0.999f) * v[i].radius;
      float px = v[i].x + dx / l * d, py = v[i].y + dy / l * d, pz = v[i].z + dz / l * d;
      if (v[i].cos_angle > -1) {
        float ca = (dx * v[i].dir[0] + dy * v[i].dir[1] + dz * v[i].dir[2]) / l;
        if (ca < v[i].cos_angle) continue; /* outside the cone */
      }
      int cell = cw_froxel_cell_of(g, px, py, pz);
      if (cell < 0) continue;
      tested++;
      if (!in_list(g, cell, i)) missed++;
    }
  CHECK(tested > 1000);
  CHECK_EQ_INT(missed, 0);
  /* cones cull: the same lights as spheres reference more cells */
  static cw_froxel_volume sph[N];
  memcpy(sph, v, sizeof sph);
  for (int i = 0; i < N; i++) sph[i].cos_angle = -2;
  cw_froxel_grid_bin(g, sph, N);
  cw_froxel_stats st2;
  cw_froxel_grid_stats(g, &st2);
  CHECK(st2.references > st.references);
  /* tight: no more references than a brute-force sphere vs cell-box test gives */
  int brute = 0;
  for (int i = 0; i < cells; i++) {
    float mn[3], mx[3];
    cw_froxel_cell_box(g, i, mn, mx);
    for (int k = 0; k < N; k++) brute += sphere_box(&sph[k], mn, mx);
  }
  cw_info("froxel: spheres: %d references, brute-force cell boxes %d", st2.references, brute);
  CHECK(st2.references <= brute);
  /* the result's fingerprint must agree on every host and worker count */
  cw_froxel_grid_bin(g, v, N);
  uint64_t h = cw_froxel_grid_hash(g);
  FOLD(h);
  cw_info("froxel: %d lights, %d references, max %d in a cell, hash %016llx", N, st.references, st.max_in_froxel,
          (unsigned long long)h);
  cw_froxel_grid_free(g);
}

static void test_overflow(void) {
  make_proj();
  enum { N = 100 };
  static cw_froxel_volume v[N];
  for (int i = 0; i < N; i++) v[i] = (cw_froxel_volume){ 0, 0, -20, 1 + (float)i * 0.01f, { 0, 0, 0 }, -2, 0 };
  cw_froxel_grid *g = cw_froxel_grid_new();
  cw_froxel_config c = CW_FROXEL_CONFIG_DEFAULT; /* 64 a cell */
  cw_froxel_grid_setup(g, &c, W, H, proj, near_, far_);
  cw_froxel_grid_bin(g, v, N);
  cw_froxel_stats st;
  cw_froxel_grid_stats(g, &st);
  CHECK_EQ_INT(st.max_in_froxel, 64);
  CHECK(st.overflowed > 0);
  CHECK(st.dropped >= st.overflowed);
  /* the first 64 in input order are kept */
  int cell = cw_froxel_cell_of(g, 0.001f, 0.001f, -20);
  CHECK(in_list(g, cell, 0) && in_list(g, cell, 63) && !in_list(g, cell, 64));
  /* nothing in view: empty lists */
  v[0] = (cw_froxel_volume){ 0, 0, 50, 2, { 0, 0, 0 }, -2, 0 }; /* behind the camera */
  cw_froxel_grid_bin(g, v, 1);
  cw_froxel_grid_stats(g, &st);
  CHECK_EQ_INT(st.references, 0);
  cw_froxel_grid_free(g);
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

static void test_bench(void) {
  make_proj();
  enum { N = 256 };
  static cw_froxel_volume v[N];
  lights(v, N, 3);
  cw_froxel_grid *g = cw_froxel_grid_new();
  cw_froxel_config c = CW_FROXEL_CONFIG_DEFAULT;
  cw_froxel_grid_setup(g, &c, W, H, proj, near_, far_);
  cw_froxel_grid_bin(g, v, N);
  double t0 = now_ms();
  for (int i = 0; i < 20; i++) cw_froxel_grid_bin(g, v, N);
  double ms = (now_ms() - t0) / 20;
  cw_froxel_stats st;
  cw_froxel_grid_stats(g, &st);
  cw_info("froxel bench: %d lights at %dx%d, %d references: %.3f ms a bin", N, W, H, st.references, ms);
  CHECK(st.references > 0);
  cw_froxel_grid_free(g);
}

CW_TEST_SUITE("froxel", { "grid", test_grid }, { "bin", test_bin }, { "overflow", test_overflow },
              { "bench", test_bench })
