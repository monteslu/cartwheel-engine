/* Four float lanes for the crowd and the audio mixer's HRTF filters: SSE2 on x86, wasm SIMD128 in carts, NEON
 * on 64-bit ARM, and plain C elsewhere. Only the operations IEEE 754 rounds
 * exactly (add, sub, mul, div, sqrt), compares and bitwise selects: every
 * path gives the same bits, lane for lane, as the plain C one (with
 * -ffp-contract=off, which the engine builds with: no fused multiply-add).
 * No min/max (their NaN rules differ per ISA) and no reciprocal estimates. */
#ifndef CW_NAV_SIMD4_H
#define CW_NAV_SIMD4_H

#include <stdint.h>
#include <string.h>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#define V4_SSE 1
typedef __m128 v4;
typedef __m128 v4m; /* a mask: all ones or all zeros per lane */
static inline v4 v4_set(float a, float b, float c, float d) { return _mm_setr_ps(a, b, c, d); }
static inline v4 v4_splat(float a) { return _mm_set1_ps(a); }
static inline v4 v4_add(v4 a, v4 b) { return _mm_add_ps(a, b); }
static inline v4 v4_sub(v4 a, v4 b) { return _mm_sub_ps(a, b); }
static inline v4 v4_mul(v4 a, v4 b) { return _mm_mul_ps(a, b); }
static inline v4 v4_div(v4 a, v4 b) { return _mm_div_ps(a, b); }
static inline v4 v4_sqrt(v4 a) { return _mm_sqrt_ps(a); }
static inline v4m v4_lt(v4 a, v4 b) { return _mm_cmplt_ps(a, b); }
static inline v4m v4_gt(v4 a, v4 b) { return _mm_cmpgt_ps(a, b); }
static inline v4m v4_and(v4m a, v4m b) { return _mm_and_ps(a, b); }
static inline v4m v4_or(v4m a, v4m b) { return _mm_or_ps(a, b); }
static inline v4m v4_not(v4m a) { return _mm_xor_ps(a, _mm_castsi128_ps(_mm_set1_epi32(-1))); }
static inline v4 v4_sel(v4m m, v4 a, v4 b) { return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b)); }
static inline int v4_bits(v4m m) { return _mm_movemask_ps(m); }
static inline void v4_store(float *p, v4 a) { _mm_storeu_ps(p, a); }
static inline v4 v4_load(const float *p) { return _mm_loadu_ps(p); }
#elif defined(__wasm_simd128__)
#include <wasm_simd128.h>
#define V4_WASM 1
typedef v128_t v4;
typedef v128_t v4m;
static inline v4 v4_set(float a, float b, float c, float d) { return wasm_f32x4_make(a, b, c, d); }
static inline v4 v4_splat(float a) { return wasm_f32x4_splat(a); }
static inline v4 v4_add(v4 a, v4 b) { return wasm_f32x4_add(a, b); }
static inline v4 v4_sub(v4 a, v4 b) { return wasm_f32x4_sub(a, b); }
static inline v4 v4_mul(v4 a, v4 b) { return wasm_f32x4_mul(a, b); }
static inline v4 v4_div(v4 a, v4 b) { return wasm_f32x4_div(a, b); }
static inline v4 v4_sqrt(v4 a) { return wasm_f32x4_sqrt(a); }
static inline v4m v4_lt(v4 a, v4 b) { return wasm_f32x4_lt(a, b); }
static inline v4m v4_gt(v4 a, v4 b) { return wasm_f32x4_gt(a, b); }
static inline v4m v4_and(v4m a, v4m b) { return wasm_v128_and(a, b); }
static inline v4m v4_or(v4m a, v4m b) { return wasm_v128_or(a, b); }
static inline v4m v4_not(v4m a) { return wasm_v128_not(a); }
static inline v4 v4_sel(v4m m, v4 a, v4 b) { return wasm_v128_bitselect(a, b, m); }
static inline int v4_bits(v4m m) { return (int)wasm_i32x4_bitmask(m); }
static inline void v4_store(float *p, v4 a) { wasm_v128_store(p, a); }
static inline v4 v4_load(const float *p) { return wasm_v128_load(p); }
#elif defined(__aarch64__)
#include <arm_neon.h>
#define V4_NEON 1
typedef float32x4_t v4;
typedef uint32x4_t v4m;
static inline v4 v4_set(float a, float b, float c, float d) {
  float t[4] = { a, b, c, d };
  return vld1q_f32(t);
}
static inline v4 v4_splat(float a) { return vdupq_n_f32(a); }
static inline v4 v4_add(v4 a, v4 b) { return vaddq_f32(a, b); }
static inline v4 v4_sub(v4 a, v4 b) { return vsubq_f32(a, b); }
static inline v4 v4_mul(v4 a, v4 b) { return vmulq_f32(a, b); }
static inline v4 v4_div(v4 a, v4 b) { return vdivq_f32(a, b); }
static inline v4 v4_sqrt(v4 a) { return vsqrtq_f32(a); }
static inline v4m v4_lt(v4 a, v4 b) { return vcltq_f32(a, b); }
static inline v4m v4_gt(v4 a, v4 b) { return vcgtq_f32(a, b); }
static inline v4m v4_and(v4m a, v4m b) { return vandq_u32(a, b); }
static inline v4m v4_or(v4m a, v4m b) { return vorrq_u32(a, b); }
static inline v4m v4_not(v4m a) { return vmvnq_u32(a); }
static inline v4 v4_sel(v4m m, v4 a, v4 b) { return vbslq_f32(m, a, b); }
static inline int v4_bits(v4m m) {
  static const int32_t sh[4] = { 0, 1, 2, 3 };
  return (int)vaddvq_u32(vshlq_u32(vshrq_n_u32(m, 31), vld1q_s32(sh)));
}
static inline void v4_store(float *p, v4 a) { vst1q_f32(p, a); }
static inline v4 v4_load(const float *p) { return vld1q_f32(p); }
#else
#include <math.h>
#define V4_SCALAR 1
typedef struct { float f[4]; } v4;
typedef struct { uint32_t u[4]; } v4m;
static inline v4 v4_set(float a, float b, float c, float d) { v4 r = { { a, b, c, d } }; return r; }
static inline v4 v4_splat(float a) { return v4_set(a, a, a, a); }
#define V4_OP(name, op) \
  static inline v4 name(v4 a, v4 b) { v4 r; for (int i = 0; i < 4; i++) r.f[i] = a.f[i] op b.f[i]; return r; }
V4_OP(v4_add, +)
V4_OP(v4_sub, -)
V4_OP(v4_mul, *)
V4_OP(v4_div, /)
#undef V4_OP
static inline v4 v4_sqrt(v4 a) { v4 r; for (int i = 0; i < 4; i++) r.f[i] = sqrtf(a.f[i]); return r; }
static inline v4m v4_lt(v4 a, v4 b) { v4m r; for (int i = 0; i < 4; i++) r.u[i] = a.f[i] < b.f[i] ? ~0u : 0; return r; }
static inline v4m v4_gt(v4 a, v4 b) { v4m r; for (int i = 0; i < 4; i++) r.u[i] = a.f[i] > b.f[i] ? ~0u : 0; return r; }
static inline v4m v4_and(v4m a, v4m b) { v4m r; for (int i = 0; i < 4; i++) r.u[i] = a.u[i] & b.u[i]; return r; }
static inline v4m v4_or(v4m a, v4m b) { v4m r; for (int i = 0; i < 4; i++) r.u[i] = a.u[i] | b.u[i]; return r; }
static inline v4m v4_not(v4m a) { v4m r; for (int i = 0; i < 4; i++) r.u[i] = ~a.u[i]; return r; }
static inline v4 v4_sel(v4m m, v4 a, v4 b) {
  v4 r;
  for (int i = 0; i < 4; i++) {
    uint32_t x, y, z;
    memcpy(&x, &a.f[i], 4);
    memcpy(&y, &b.f[i], 4);
    z = (m.u[i] & x) | (~m.u[i] & y);
    memcpy(&r.f[i], &z, 4);
  }
  return r;
}
static inline int v4_bits(v4m m) { return (int)((m.u[0] >> 31) | (m.u[1] >> 31 << 1) | (m.u[2] >> 31 << 2) | (m.u[3] >> 31 << 3)); }
static inline void v4_store(float *p, v4 a) { memcpy(p, a.f, 16); }
static inline v4 v4_load(const float *p) { v4 r; memcpy(r.f, p, 16); return r; }
#endif

#endif
