/* cartwheel math: 2D vectors, scalar helpers, and DETERMINISTIC
 * transcendentals.
 *
 * 3D math is three.c's (t3_vec3, t3_quat, t3_mat4 and their functions).
 *
 * Determinism rule: libm's sinf/cosf/atan2f/expf/logf/powf differ between
 * glibc, musl (wasi-libc), Apple's libm and MSVC's CRT, so simulation code
 * (anything that feeds game state, physics, replays) uses the cw_ versions
 * below. They use only + - * / and sqrt, which IEEE 754 makes identical on
 * every target given -ffp-contract=off (set engine-wide). Rendering code may
 * use libm freely: pixels are compared with tolerance, state is not. */
#ifndef CW_MATH_H
#define CW_MATH_H

#include <math.h>

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_PI 3.14159265358979323846f
#define CW_TAU 6.28318530717958647692f

typedef struct { float x, y; } cw_vec2;

/* Inline helpers below use only + - * / and sqrtf: deterministic under -ffp-contract=off. */
static inline cw_vec2 cw_v2(float x, float y) { return (cw_vec2){ x, y }; }
/* a + b. */
static inline cw_vec2 cw_v2_add(cw_vec2 a, cw_vec2 b) { return (cw_vec2){ a.x + b.x, a.y + b.y }; }
/* a - b. */
static inline cw_vec2 cw_v2_sub(cw_vec2 a, cw_vec2 b) { return (cw_vec2){ a.x - b.x, a.y - b.y }; }
/* a * s, componentwise. */
static inline cw_vec2 cw_v2_scale(cw_vec2 a, float s) { return (cw_vec2){ a.x * s, a.y * s }; }
/* Dot product. */
static inline float cw_v2_dot(cw_vec2 a, cw_vec2 b) { return a.x * b.x + a.y * b.y; }
/* 2D cross product (z of the 3D cross): positive when b is counter-clockwise of a. */
static inline float cw_v2_cross(cw_vec2 a, cw_vec2 b) { return a.x * b.y - a.y * b.x; }
/* Euclidean length (sqrtf is IEEE-exact, so deterministic). */
static inline float cw_v2_length(cw_vec2 a) { return sqrtf(a.x * a.x + a.y * a.y); }
/* a + (b - a) * t; t is not clamped. */
static inline cw_vec2 cw_v2_lerp(cw_vec2 a, cw_vec2 b, float t) { return (cw_vec2){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t }; }
/* Unit vector along a; a zero vector comes back unchanged (no NaN). */
static inline cw_vec2 cw_v2_normalize(cw_vec2 a) {
  float l = cw_v2_length(a);
  return l > 0 ? cw_v2_scale(a, 1.0f / l) : a;
}
/* Rotates by the angle whose cos/sin are cs/sn (cw_sincos): counter-clockwise with y up, clockwise on screen. */
static inline cw_vec2 cw_v2_rotate(cw_vec2 a, float cs, float sn) { return (cw_vec2){ cs * a.x - sn * a.y, sn * a.x + cs * a.y }; }

/* a + (b - a) * t; t is not clamped. */
static inline float cw_lerp(float a, float b, float t) { return a + (b - a) * t; }
/* x limited to [lo, hi]; a NaN x passes through unchanged. */
static inline float cw_clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
/* Hermite 0..1 as x goes e0..e1, clamped outside; e0 == e1 divides by zero. */
static inline float cw_smoothstep(float e0, float e1, float x) {
  float t = cw_clampf((x - e0) / (e1 - e0), 0, 1);
  return t * t * (3 - 2 * t);
}
/* x stepped toward target by at most step (step >= 0), never overshooting. */
static inline float cw_move_toward(float x, float target, float step) {
  return x < target ? (x + step > target ? target : x + step) : (x - step < target ? target : x - step);
}

/* deterministic transcendentals (src/core/dmath.c) */
float cw_sin(float x);
float cw_cos(float x); /* deterministic, like cw_sin */
void cw_sincos(float x, float *s, float *c); /* both at the cost of one; deterministic */
float cw_atan2(float y, float x); /* deterministic; (-pi, pi], 0 for (0, 0) */
float cw_atan(float x); /* deterministic; (-pi/2, pi/2) */
float cw_acos(float x); /* deterministic; x clamped to [-1, 1], result [0, pi] */
float cw_asin(float x); /* deterministic; x clamped to [-1, 1], result [-pi/2, pi/2] */
float cw_exp(float x); /* deterministic; +inf above 88.72, 0 below -103.97 */
float cw_ln(float x); /* natural log (cw_log is logging) */
float cw_pow(float x, float y); /* deterministic exp(y*ln x); x < 0 needs integral y (else NaN) */
float cw_wrap_angle(float a); /* to [-pi, pi) */

/* easing (deterministic) */
typedef enum {
  CW_EASE_LINEAR, CW_EASE_IN_QUAD, CW_EASE_OUT_QUAD, CW_EASE_IN_OUT_QUAD, CW_EASE_IN_CUBIC, CW_EASE_OUT_CUBIC,
  CW_EASE_IN_OUT_CUBIC, CW_EASE_IN_SINE, CW_EASE_OUT_SINE, CW_EASE_IN_OUT_SINE, CW_EASE_OUT_BACK, CW_EASE_OUT_ELASTIC,
  CW_EASE_OUT_BOUNCE, CW_EASE_COUNT
} cw_ease;
float cw_ease_apply(cw_ease e, float t); /* t clamped to [0, 1]; deterministic; unknown e = linear */

#ifdef __cplusplus
}
#endif
#endif
