/* Deterministic quaternion interpolation (cartwheel/anim.h). On its own so tools that link
 * the pose and retarget code without the scene tree (cwpack) get it too. */
#include <math.h>
#include <string.h>

#include "cartwheel/anim.h"
#include "cartwheel/math.h"

void cw_quat_slerp_det(float out[4], const float a[4], const float b[4], float t) {
  /* three.js Quaternion.slerpFlat with cw_atan2 / cw_sin (deterministic) */
  float x0 = a[0], y0 = a[1], z0 = a[2], w0 = a[3];
  const float x1 = b[0], y1 = b[1], z1 = b[2], w1 = b[3];
  if (t == 0) { out[0] = x0; out[1] = y0; out[2] = z0; out[3] = w0; return; }
  if (t == 1) { out[0] = x1; out[1] = y1; out[2] = z1; out[3] = w1; return; }
  if (w0 != w1 || x0 != x1 || y0 != y1 || z0 != z1) {
    float s = 1 - t;
    const float cs = x0 * x1 + y0 * y1 + z0 * z1 + w0 * w1, dir = cs >= 0 ? 1.0f : -1.0f, sqr_sin = 1 - cs * cs;
    if (sqr_sin > 2.220446049250313e-16f) {
      const float sn = sqrtf(sqr_sin), len = cw_atan2(sn, cs * dir);
      s = cw_sin(s * len) / sn;
      t = cw_sin(t * len) / sn;
    }
    const float tdir = t * dir;
    x0 = x0 * s + x1 * tdir;
    y0 = y0 * s + y1 * tdir;
    z0 = z0 * s + z1 * tdir;
    w0 = w0 * s + w1 * tdir;
    if (s == 1 - t) {
      const float f = 1 / sqrtf(x0 * x0 + y0 * y0 + z0 * z0 + w0 * w0);
      x0 *= f; y0 *= f; z0 *= f; w0 *= f;
    }
  }
  out[0] = x0; out[1] = y0; out[2] = z0; out[3] = w0;
}

void cw_quat_nlerp(float out[4], const float a[4], const float b[4], float t) {
  float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], sb = d < 0 ? -1.0f : 1.0f, r[4];
  for (int i = 0; i < 4; i++) r[i] = a[i] + (b[i] * sb - a[i]) * t;
  float l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
  if (l == 0) { r[0] = r[1] = r[2] = 0; r[3] = 1; }
  else {
    l = 1 / l;
    for (int i = 0; i < 4; i++) r[i] *= l;
  }
  memcpy(out, r, sizeof r);
}

