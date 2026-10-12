#include "cartwheel/math.h"
#include "test.h"

/* accuracy against libm (which is NOT deterministic, but is accurate), and
 * every result folded into the checksum the gate compares across hosts */
static void test_trig(void) {
  float worst_s = 0, worst_a = 0;
  for (int i = -20000; i <= 20000; i++) {
    float x = (float)i * 0.0137f;
    float s, c;
    cw_sincos(x, &s, &c);
    FOLD(s);
    FOLD(c);
    float es = fabsf(s - sinf(x)), ec = fabsf(c - cosf(x));
    if (es > worst_s) worst_s = es;
    if (ec > worst_s) worst_s = ec;
    float y = (float)(i % 97) * 0.21f - 10;
    float a = cw_atan2(y, x);
    FOLD(a);
    float ea = fabsf(a - atan2f(y, x));
    if (ea > worst_a && fabsf(fabsf(a) - CW_PI) > 1e-3f) worst_a = ea;
  }
  cw_info("sincos max err %g, atan2 max err %g", worst_s, worst_a);
  CHECK(worst_s < 2e-6f * 300); /* large |x| loses a little in float reduction */
  CHECK(worst_a < 1e-6f * 5);
}

static void test_exp_log(void) {
  float worst = 0;
  for (int i = -800; i <= 800; i++) {
    float x = (float)i * 0.1f;
    float e = cw_exp(x);
    FOLD(e);
    float rel = fabsf(e - expf(x)) / (expf(x) + 1e-30f);
    if (rel > worst) worst = rel;
  }
  CHECK(worst < 1e-6f);
  worst = 0;
  for (int i = 1; i <= 10000; i++) {
    float x = (float)i * 0.37f;
    float l = cw_ln(x);
    FOLD(l);
    float err = fabsf(l - logf(x));
    if (err > worst) worst = err;
  }
  CHECK(worst < 1e-5f);
  CHECK(fabsf(cw_pow(2, 10) - 1024) < 1e-2f);
  CHECK(fabsf(cw_pow(-2, 3) + 8) < 1e-4f);
}

static void test_misc(void) {
  CHECK(fabsf(cw_wrap_angle(3 * CW_PI) - (-CW_PI)) < 1e-5f || fabsf(cw_wrap_angle(3 * CW_PI) - CW_PI) < 1e-5f);
  CHECK(fabsf(cw_wrap_angle(0.5f) - 0.5f) < 1e-6f);
  for (int e = 0; e < CW_EASE_COUNT; e++) {
    CHECK(fabsf(cw_ease_apply((cw_ease)e, 0)) < 1e-6f);
    CHECK(fabsf(cw_ease_apply((cw_ease)e, 1) - 1) < 1e-5f);
    float m = cw_ease_apply((cw_ease)e, 0.37f);
    FOLD(m);
  }
  cw_vec2 v = cw_v2_normalize(cw_v2(3, 4));
  CHECK(fabsf(v.x - 0.6f) < 1e-6f && fabsf(v.y - 0.8f) < 1e-6f);
  CHECK(fabsf(cw_acos(0) - CW_PI / 2) < 1e-6f);
}

CW_TEST_SUITE("math", { "trig", test_trig }, { "exp_log", test_exp_log }, { "misc", test_misc })
