#include "cartwheel/framing.h"

#include "cartwheel/math.h"

static void basis(const cw_frame_params *p, float f[3], float r[3], float u[3]) {
  float sy, cy, sp, cp;
  cw_sincos(p->yaw, &sy, &cy);
  cw_sincos(p->pitch, &sp, &cp);
  f[0] = -sy * cp; f[1] = sp; f[2] = -cy * cp;
  r[0] = cy; r[1] = 0; r[2] = -sy;
  u[0] = r[1] * f[2] - r[2] * f[1];
  u[1] = r[2] * f[0] - r[0] * f[2];
  u[2] = r[0] * f[1] - r[1] * f[0];
}
static inline float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

bool cw_frame_fit(const cw_frame_params *p, const cw_frame_point *pts, int n, cw_frame_result *out) {
  if (!p || !pts || n < 1 || !out || p->fov_y <= 0.01f || p->fov_y >= 3.0f || p->aspect <= 0.01f) return false;
  float f[3], r[3], u[3];
  basis(p, f, r, u);
  float s, c;
  cw_sincos(p->fov_y * 0.5f, &s, &c);
  float ty = s / c, tx = ty * p->aspect;
  float k = 1.0f - cw_clampf(p->margin, 0, 0.9f);
  tx *= k; ty *= k;
  float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f, minz = 1e30f, maxz = -1e30f;
  for (int i = 0; i < n; i++) {
    float q[3] = { pts[i].x, pts[i].y, pts[i].z };
    float x = dot3(q, r), y = dot3(q, u), z = dot3(q, f), rad = pts[i].radius;
    if (x - rad < minx) minx = x - rad;
    if (x + rad > maxx) maxx = x + rad;
    if (y - rad < miny) miny = y - rad;
    if (y + rad > maxy) maxy = y + rad;
    if (z < minz) minz = z;
    if (z > maxz) maxz = z;
  }
  float cx = (minx + maxx) * 0.5f, cy = (miny + maxy) * 0.5f, cz = (minz + maxz) * 0.5f;
  /* each point needs dist + (z - cz) >= |x - cx| / tx (radius folded into x/y above) */
  float need = 0;
  for (int i = 0; i < n; i++) {
    float q[3] = { pts[i].x, pts[i].y, pts[i].z };
    float x = dot3(q, r) - cx, y = dot3(q, u) - cy, z = dot3(q, f) - cz, rad = pts[i].radius;
    float ax = (x < 0 ? -x : x) + rad, ay = (y < 0 ? -y : y) + rad;
    float dx = ax / tx - z, dy = ay / ty - z;
    if (dx > need) need = dx;
    if (dy > need) need = dy;
  }
  float d = need, mn = p->min_dist, mx = p->max_dist;
  bool clamped = false;
  if (d < mn) d = mn;
  if (mx > 0 && d > mx) { d = mx; clamped = true; }
  for (int i = 0; i < 3; i++) out->target[i] = r[i] * cx + u[i] * cy + f[i] * cz;
  for (int i = 0; i < 3; i++) out->pos[i] = out->target[i] - f[i] * d;
  out->dist = d;
  out->clamped = clamped;
  return true;
}

void cw_frame_follow(cw_frame_state *st, const cw_frame_params *p, const cw_frame_result *fit, float dt, float speed,
                     cw_frame_result *out) {
  if (!st->init || speed <= 0) {
    for (int i = 0; i < 3; i++) { st->target[i] = fit->target[i]; st->vel[i] = 0; }
    st->dist = fit->dist;
    st->dist_vel = 0;
    st->init = true;
  } else {
    /* critically damped spring, semi-implicit: x'' = w^2 (goal - x) - 2 w x' */
    float w = speed, w2 = w * w;
    for (int i = 0; i < 3; i++) {
      st->vel[i] += (w2 * (fit->target[i] - st->target[i]) - 2 * w * st->vel[i]) * dt;
      st->target[i] += st->vel[i] * dt;
    }
    st->dist_vel += (w2 * (fit->dist - st->dist) - 2 * w * st->dist_vel) * dt;
    st->dist += st->dist_vel * dt;
  }
  float f[3], r[3], u[3];
  basis(p, f, r, u);
  for (int i = 0; i < 3; i++) { out->target[i] = st->target[i]; out->pos[i] = st->target[i] - f[i] * st->dist; }
  out->dist = st->dist;
  out->clamped = fit->clamped;
}
