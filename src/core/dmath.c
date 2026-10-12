/* Deterministic transcendentals: only + - * / and sqrt (IEEE-exact on every
 * target with -ffp-contract=off). Accuracy is a few ulp, plenty for games;
 * identical results on every host is the point. */
#include "cartwheel/math.h"

/* sin/cos on [-pi/4, pi/4] with minimax polynomials (Cephes coefficients),
 * after a two-constant Cody-Waite reduction by pi/2. */
static float sin_core(float x) {
  float z = x * x;
  return x + x * z * (-1.6666654611e-1f + z * (8.3321608736e-3f + z * -1.9515295891e-4f));
}
static float cos_core(float x) {
  float z = x * x;
  return 1.0f - 0.5f * z + z * z * (4.166664568298827e-2f + z * (-1.388731625493765e-3f + z * 2.443315711809948e-5f));
}

void cw_sincos(float x, float *s, float *c) {
  /* quadrant = round(x * 2/pi), done in double for range (still IEEE exact) */
  double q = (double)x * 0.63661977236758134308;
  double qr = q >= 0 ? (double)(long long)(q + 0.5) : (double)(long long)(q - 0.5);
  long long qi = (long long)qr;
  double r = ((double)x - qr * 1.57079632673412561417) - qr * 6.07710050650619224932e-11;
  float rf = (float)r;
  float sv = sin_core(rf), cv = cos_core(rf);
  switch ((int)(qi & 3)) {
  case 0: *s = sv; *c = cv; break;
  case 1: *s = cv; *c = -sv; break;
  case 2: *s = -sv; *c = -cv; break;
  default: *s = -cv; *c = sv; break;
  }
}

float cw_sin(float x) { float s, c; cw_sincos(x, &s, &c); return s; }
float cw_cos(float x) { float s, c; cw_sincos(x, &s, &c); return c; }

float cw_wrap_angle(float a) {
  double t = ((double)a + 3.14159265358979323846) / 6.28318530717958647692;
  double f = t - (double)(long long)t;
  if (f < 0) f += 1;
  return (float)(f * 6.28318530717958647692 - 3.14159265358979323846);
}

/* atan on [0, 1] via range reduction to |x| <= tan(pi/12) (Cephes atanf) */
float cw_atan(float x) {
  float sign = x < 0 ? -1.0f : 1.0f;
  x = x < 0 ? -x : x;
  float y = 0;
  if (x > 2.414213562373095f) {
    y = CW_PI * 0.5f;
    x = -1.0f / x;
  } else if (x > 0.4142135623730950f) {
    y = CW_PI * 0.25f;
    x = (x - 1.0f) / (x + 1.0f);
  }
  float z = x * x;
  y += ((((8.05374449538e-2f * z - 1.38776856032e-1f) * z + 1.99777106478e-1f) * z - 3.33329491539e-1f) * z * x + x);
  return sign * y;
}

float cw_atan2(float y, float x) {
  if (x == 0) {
    if (y > 0) return CW_PI * 0.5f;
    if (y < 0) return -CW_PI * 0.5f;
    return 0;
  }
  float a = cw_atan(y / x);
  if (x < 0) a += y >= 0 ? CW_PI : -CW_PI;
  return a;
}

float cw_asin(float x) {
  if (x >= 1) return CW_PI * 0.5f;
  if (x <= -1) return -CW_PI * 0.5f;
  return cw_atan2(x, sqrtf((1 - x) * (1 + x)));
}
float cw_acos(float x) {
  if (x >= 1) return 0;
  if (x <= -1) return CW_PI;
  return cw_atan2(sqrtf((1 - x) * (1 + x)), x);
}

/* exp: x = k ln2 + r, |r| <= ln2/2; e^r by a degree-6 Taylor/minimax poly;
 * 2^k built from exponent bits */
float cw_exp(float x) {
  if (x > 88.72f) return INFINITY;
  if (x < -103.97f) return 0;
  double kd = (double)x * 1.44269504088896340736;
  long long k = (long long)(kd >= 0 ? kd + 0.5 : kd - 0.5);
  double r = (double)x - (double)k * 0.69314718055994530942;
  double p = 1 + r * (1 + r * (0.5 + r * (1.0 / 6 + r * (1.0 / 24 + r * (1.0 / 120 + r * (1.0 / 720))))));
  union { double d; uint64_t u; } two = { .u = (uint64_t)(k + 1023) << 52 };
  return (float)(p * two.d);
}

/* log: x = m 2^e, m in [sqrt(1/2), sqrt(2)); log(m) via atanh series */
float cw_ln(float x) {
  if (!(x > 0)) return x == 0 ? -INFINITY : NAN;
  if (x == INFINITY) return x;
  union { double d; uint64_t u; } v = { .d = (double)x };
  int e = (int)((v.u >> 52) & 0x7ff) - 1023;
  v.u = (v.u & 0x000fffffffffffffull) | 0x3ff0000000000000ull; /* m in [1, 2) */
  double m = v.d;
  if (m > 1.41421356237309504880) { m *= 0.5; e++; }
  double s = (m - 1) / (m + 1), s2 = s * s;
  double l = 2 * s * (1 + s2 * (1.0 / 3 + s2 * (1.0 / 5 + s2 * (1.0 / 7 + s2 * (1.0 / 9 + s2 * (1.0 / 11))))));
  return (float)(l + e * 0.69314718055994530942);
}

float cw_pow(float x, float y) {
  if (y == 0) return 1;
  if (x == 0) return y > 0 ? 0 : INFINITY;
  if (x < 0) {
    float yi = (float)(long long)y;
    if (yi != y) return NAN;
    float r = cw_exp(y * cw_ln(-x));
    return ((long long)yi & 1) ? -r : r;
  }
  return cw_exp(y * cw_ln(x));
}

float cw_ease_apply(cw_ease e, float t) {
  t = cw_clampf(t, 0, 1);
  switch (e) {
  case CW_EASE_LINEAR: return t;
  case CW_EASE_IN_QUAD: return t * t;
  case CW_EASE_OUT_QUAD: return t * (2 - t);
  case CW_EASE_IN_OUT_QUAD: return t < 0.5f ? 2 * t * t : -1 + (4 - 2 * t) * t;
  case CW_EASE_IN_CUBIC: return t * t * t;
  case CW_EASE_OUT_CUBIC: { float u = t - 1; return u * u * u + 1; }
  case CW_EASE_IN_OUT_CUBIC: return t < 0.5f ? 4 * t * t * t : (t - 1) * (2 * t - 2) * (2 * t - 2) + 1;
  case CW_EASE_IN_SINE: return 1 - cw_cos(t * CW_PI * 0.5f);
  case CW_EASE_OUT_SINE: return cw_sin(t * CW_PI * 0.5f);
  case CW_EASE_IN_OUT_SINE: return 0.5f * (1 - cw_cos(CW_PI * t));
  case CW_EASE_OUT_BACK: { const float c1 = 1.70158f, c3 = c1 + 1; float u = t - 1; return 1 + c3 * u * u * u + c1 * u * u; }
  case CW_EASE_OUT_ELASTIC:
    if (t == 0 || t == 1) return t;
    return cw_pow(2, -10 * t) * cw_sin((t * 10 - 0.75f) * (CW_TAU / 3)) + 1;
  case CW_EASE_OUT_BOUNCE: {
    const float n1 = 7.5625f, d1 = 2.75f;
    if (t < 1 / d1) return n1 * t * t;
    if (t < 2 / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
    if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
    t -= 2.625f / d1;
    return n1 * t * t + 0.984375f;
  }
  default: return t;
  }
}
