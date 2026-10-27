/* cartwheel/framing.h: the fit puts the extreme points on the margin, the
 * limits clamp, the follow converges without overshoot. */
#include <string.h>

#include "cartwheel/framing.h"
#include "cartwheel/math.h"
#include "test.h"

/* project a point through the result's camera: returns ndc x,y (|v| <= 1 is in view) */
static void project(const cw_frame_params *p, const cw_frame_result *r, float x, float y, float z, float *nx, float *ny) {
  float sy, cy, sp, cp;
  cw_sincos(p->yaw, &sy, &cy);
  cw_sincos(p->pitch, &sp, &cp);
  float f[3] = { -sy * cp, sp, -cy * cp }, rr[3] = { cy, 0, -sy };
  float u[3] = { rr[1] * f[2] - rr[2] * f[1], rr[2] * f[0] - rr[0] * f[2], rr[0] * f[1] - rr[1] * f[0] };
  float d[3] = { x - r->pos[0], y - r->pos[1], z - r->pos[2] };
  float dz = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
  float dx = d[0] * rr[0] + d[1] * rr[1] + d[2] * rr[2], dy = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
  float s, c;
  cw_sincos(p->fov_y * 0.5f, &s, &c);
  float ty = s / c;
  *nx = dx / dz / (ty * p->aspect);
  *ny = dy / dz / ty;
}
static float absf(float x) { return x < 0 ? -x : x; }

static void test_fit_margin(void) {
  cw_frame_params p = { .yaw = 0.3f, .pitch = -0.2f, .fov_y = 0.7f, .aspect = 16.f / 9, .margin = 0.1f, .min_dist = 1 };
  cw_frame_point pts[3] = { { -3, 0, 0, 0 }, { 4, 1, 1, 0 }, { 0, 2, -1, 0 } };
  cw_frame_result r;
  CHECK(cw_frame_fit(&p, pts, 3, &r));
  float maxx = 0, maxy = 0;
  for (int i = 0; i < 3; i++) {
    float nx, ny;
    project(&p, &r, pts[i].x, pts[i].y, pts[i].z, &nx, &ny);
    if (absf(nx) > maxx) maxx = absf(nx);
    if (absf(ny) > maxy) maxy = absf(ny);
  }
  CHECK(maxx < 0.9f + 0.02f && maxx > 0.9f - 0.1f);
  CHECK(maxx <= 0.9f + 0.001f && maxy <= 0.9f + 0.001f); /* everyone is in view with the margin */
  CHECK(!r.clamped);
  uint32_t v = (uint32_t)(r.dist * 1000); FOLD(v);
}

static void test_limits(void) {
  cw_frame_params p = { .yaw = 0, .pitch = -0.1f, .fov_y = 0.7f, .aspect = 1.78f, .margin = 0.1f, .min_dist = 6, .max_dist = 9 };
  cw_frame_point close[2] = { { -0.5f, 0, 0, 0 }, { 0.5f, 0, 0, 0 } };
  cw_frame_result r;
  cw_frame_fit(&p, close, 2, &r);
  CHECK(r.dist >= 5.999f && r.dist <= 6.001f); /* min distance holds */
  cw_frame_point far2[2] = { { -30, 0, 0, 0 }, { 30, 0, 0, 0 } };
  cw_frame_fit(&p, far2, 2, &r);
  CHECK(r.clamped && r.dist <= 9.001f); /* max distance: reported, not hidden */
  float nx, ny;
  project(&p, &r, 30, 0, 0, &nx, &ny);
  CHECK(absf(nx) > 1.0f); /* and the point really is outside, which clamped says */
  CHECK(!cw_frame_fit(&p, far2, 0, &r));
}

static void test_follow(void) {
  cw_frame_params p = { .yaw = 0, .pitch = -0.1f, .fov_y = 0.7f, .aspect = 1.78f, .margin = 0.1f, .min_dist = 4 };
  cw_frame_point a[2] = { { -2, 0, 0, 0 }, { 2, 0, 0, 0 } }, b[2] = { { 6, 0, 0, 0 }, { 10, 0, 0, 0 } };
  cw_frame_state st; memset(&st, 0, sizeof st);
  cw_frame_result fa, fb, out;
  cw_frame_fit(&p, a, 2, &fa);
  cw_frame_follow(&st, &p, &fa, 1.0f / 60, 6, &out);
  CHECK(absf(out.target[0] - fa.target[0]) < 1e-5f); /* first call snaps */
  cw_frame_fit(&p, b, 2, &fb);
  float last = out.target[0], maxx = last;
  bool mono = true;
  for (int i = 0; i < 300; i++) {
    cw_frame_follow(&st, &p, &fb, 1.0f / 60, 6, &out);
    if (out.target[0] < last - 1e-4f) mono = false;
    last = out.target[0];
    if (last > maxx) maxx = last;
  }
  CHECK(mono);
  CHECK(maxx <= fb.target[0] + 0.05f); /* critically damped: no real overshoot */
  CHECK(absf(out.target[0] - fb.target[0]) < 0.01f); /* and it arrives */
  uint32_t v = (uint32_t)(out.target[0] * 1000); FOLD(v);
}

CW_TEST_SUITE("framing", { "fit_margin", test_fit_margin }, { "limits", test_limits }, { "follow", test_follow })
