/* looks_onebit: a one-bit picture of the finished frame through cw_look (cartwheel/look.h): an ORDERED dither with blue
 * noise at the DISPLAY stage (after bloom, the tone curve, the vignette), one dither pixel per screen pixel, the tone
 * options on (saturated hues by their value, grey by its luminance with the highlights kept, an unsharp mask, depth
 * outlines, gamma before contrast). Every pixel on screen is black or white.
 *
 * Beside it, as the gates' reference, the pass the look was taken from: a game's own one-bit POST pass (tone, then a
 * threshold; its shaders below are that pass's line for line), and the same picture written as a CUSTOM look from the
 * accessors (cw_look_scene, cw_look_linear_depth, cw_look_blue_noise).
 *
 * Test switches (cw_test_env, read every frame):
 *   ONEBIT=look       the engine's look at DISPLAY (default)
 *          scene      the same look at the SCENE stage (bloom, the tone curve and the vignette come after it)
 *          trial      the reference pass, its own 64x64 blue noise
 *          trial_em   the reference pass with the engine's blue noise (the same thresholds, rows turned for its
 *                     bottom-left anchoring at 1080 lines): pixel for pixel the look's picture
 *          custom     the CUSTOM look version (DISPLAY)
 *          plain      no look
 *   ONEBIT_ONLY=x     one tone option alone (hue | grey | sharpen | outline | gamma | contrast | none), the others
 *                     neutral, in the look and in the reference alike
 *   ONEBIT_SHARPEN=k  the unsharp mask's gain (0.9)
 *   ONEBIT_PATTERN=bayer   the 8x8 Bayer matrix instead of blue noise (the look and the reference)
 *   ONEBIT_PIXEL=n, ONEBIT_OUTPX=0|1   the dither pixel's size, in output pixels (1, the default) or 1080 line ones (0)
 *   ONEBIT_KIND=diffuse   the engine's look as a DIFFUSE dither (the same tone options)
 *   ONEBIT_RAMP=N     a fade from the plain picture into the look over N frames from frame 60 (the flash check)
 *   ONEBIT_FLICKER=N  the weight jumping between 0 and 1 every N frames from frame 60 (its control)
 *   ONEBIT_ORBIT=1    the camera orbits
 *   ONEBIT_BLOOM=0    no bloom
 *   ONEBIT_PAIR=1     the pair (the engine's look, the CUSTOM look) at weight ONEBIT_T (0.5), blended by ONEBIT_HOW
 *                     (wipe | fade | dissolve; wipe): with ONEBIT=scene the first is a SCENE stage look, and the pair
 *                     is still drawn at DISPLAY (the stage rule)
 *   ONEBIT_XFADE=N    a fade over N frames from frame 60 from the look (DISPLAY) into half blocks (SCENE stage),
 *                     then the half blocks alone (ONEBIT_XREV=1: the other way); ONEBIT_HOW picks the blend
 *                     (dissolve, the default, fade or wipe); ONEBIT_XT=t holds the pair at weight t (0 and 1: the
 *                     end look alone)
 *   ONEBIT_SPLIT=1    two views side by side with the shared post chain (one pass unless CW_TEST_VIEWS_ARRAY=0);
 *                     2: each view with its own chain
 *
 * Debug state: onebit.mode, onebit.ran (the pass that drew ran last frame), onebit.gpu_ms (its GPU time, natively with
 * CW_GPU_TIMERS=1), and the engine's look.stage, look.ran, look.kind_a. ONEBIT_H builds render at another height
 * (looks_onebit_720). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/look.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

#ifndef ONEBIT_H
#define ONEBIT_H 1080
#endif

enum { M_LOOK, M_SCENE, M_TRIAL, M_TRIAL_EM, M_CUSTOM, M_PLAIN, M_COUNT };
static const char *const MODES[M_COUNT] = { "look", "scene", "trial", "trial_em", "custom", "plain" };
static int32_t tick, mode, ran, array_used;
static float gpu_ms = -1, weight;

static const char *env(const char *name) { return cw_test_env(name); }
static float envf(const char *name, float def) {
  const char *e = env(name);
  return e && *e ? (float)atof(e) : def;
}
static int envi(const char *name, int def) {
  const char *e = env(name);
  return e && *e ? atoi(e) : def;
}
static bool only_is(const char *x) {
  const char *o = env("ONEBIT_ONLY");
  return !o || !*o || !strcmp(o, x);
}

/* the tone constants of the reference pass, and the same as look settings: an option ONEBIT_ONLY leaves out is
 * neutral in both */
typedef struct tone { float gamma, contrast, bias, sharpen, outline, out_from, out_to, world, block, hue_on; } tone;
static tone tone_now(void) {
  tone t = { 1.3f, 1.3f, 0, envf("ONEBIT_SHARPEN", 0.9f), 1.0f, 0.02f, 0.08f, 0.8f, 1.15f, 1 };
  if (!only_is("gamma")) t.gamma = 1;
  if (!only_is("contrast")) t.contrast = 1;
  if (!only_is("sharpen")) t.sharpen = 0;
  if (!only_is("outline")) t.outline = 0;
  if (!only_is("grey")) t.world = 1;
  if (!only_is("hue")) t.hue_on = 0;
  return t;
}

/* ── the reference pass: a game's one-bit POST pass, last in the chain ─────────────────────────────────────────
 * Its tone and threshold shaders are the game's as they were written, with one addition for the gates: u_key.z
 * scales the hue key (1: as written; 0: the hue option off, as the look's hue_value 0). */
static const char *TONE_GLSL =
    "uniform sampler2D u_src; uniform sampler2D u_depth; uniform vec4 u_tone; uniform vec4 u_edge; uniform vec4 u_lin; uniform vec4 u_key;\n"
    "out vec4 o;\n"
    "float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"
    "float L(ivec2 q) {\n"
    "  q = clamp(q, ivec2(0), textureSize(u_src, 0) - 1); vec3 c = clamp(texelFetch(u_src, q, 0).rgb, 0.0, 1.0);\n"
    "  float mx = max(max(c.r, c.g), c.b), mn = min(min(c.r, c.g), c.b);\n"
    "  float key = smoothstep(0.22, 0.5, (mx - mn) / max(mx, 1e-4)) * u_key.z;\n"
    "  float l = lum(c);\n"
    "  return clamp(mix(mix(l * u_key.x, l, smoothstep(0.6, 0.95, l)), mx * u_key.y, key), 0.0, 1.0);\n"
    "}\n"
    "float W(ivec2 q) {\n"
    "  q = clamp(q, ivec2(0), textureSize(u_depth, 0) - 1);\n"
    "  float d = texelFetch(u_depth, q, 0).r;\n"
    "  if (d >= 0.999999) return 0.0;\n"
    "  float z = u_lin.z > 0.5 ? u_lin.x + d * (u_lin.y - u_lin.x) : u_lin.x * u_lin.y / (u_lin.y - d * (u_lin.y - u_lin.x));\n"
    "  return 1.0 / max(z, 1e-4);\n"
    "}\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  float c = L(p);\n"
    "  float b = (L(p + ivec2(2, 0)) + L(p - ivec2(2, 0)) + L(p + ivec2(0, 2)) + L(p - ivec2(0, 2))) * 0.15\n"
    "          + (L(p + ivec2(1, 1)) + L(p - ivec2(1, 1)) + L(p + ivec2(1, -1)) + L(p - ivec2(1, -1))) * 0.1;\n"
    "  float v = clamp(c + u_tone.w * (c - b), 0.0, 1.0);\n"
    "  v = pow(v, u_tone.x);\n"
    "  v = clamp((v - 0.5) * u_tone.y + 0.5 + u_tone.z, 0.0, 1.0);\n"
    "  float w = W(p);\n"
    "  if (u_edge.x > 0.0 && w > 0.0) {\n"
    "    float lap = 4.0 * w - W(p + ivec2(1, 0)) - W(p - ivec2(1, 0)) - W(p + ivec2(0, 1)) - W(p - ivec2(0, 1));\n"
    "    float e = smoothstep(u_edge.y, u_edge.z, lap / w);\n"
    "    v = max(v, e * u_edge.x);\n"
    "  }\n"
    "  o = vec4(v, 0.0, 0.0, 1.0);\n"
    "}\n";
static const char *TONE_WGSL =
    "struct U { u_tone: vec4f, u_edge: vec4f, u_lin: vec4f, u_key: vec4f };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@group(0) @binding(1) var u_src: texture_2d<f32>;\n"
    "@group(0) @binding(2) var u_depth: texture_2d<f32>;\n"
    "fn lum(c: vec3f) -> f32 { return dot(c, vec3f(0.2126, 0.7152, 0.0722)); }\n"
    "fn L(q0: vec2i) -> f32 {\n"
    "  let q = clamp(q0, vec2i(0), vec2i(textureDimensions(u_src)) - 1); let c = clamp(textureLoad(u_src, q, 0).rgb, vec3f(0.0), vec3f(1.0));\n"
    "  let mx = max(max(c.r, c.g), c.b); let mn = min(min(c.r, c.g), c.b);\n"
    "  let key = smoothstep(0.22, 0.5, (mx - mn) / max(mx, 1e-4)) * u.u_key.z;\n"
    "  let l = lum(c);\n"
    "  return clamp(mix(mix(l * u.u_key.x, l, smoothstep(0.6, 0.95, l)), mx * u.u_key.y, key), 0.0, 1.0);\n"
    "}\n"
    "fn W(q0: vec2i) -> f32 {\n"
    "  let q = clamp(q0, vec2i(0), vec2i(textureDimensions(u_depth)) - 1);\n"
    "  let z = textureLoad(u_depth, q, 0).r;\n"
    "  if (z > 1e30) { return 0.0; }\n"
    "  return 1.0 / max(z, 1e-4);\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  let p = vec2i(cw_frag_coord(pos));\n"
    "  let c = L(p);\n"
    "  let b = (L(p + vec2i(2, 0)) + L(p - vec2i(2, 0)) + L(p + vec2i(0, 2)) + L(p - vec2i(0, 2))) * 0.15\n"
    "        + (L(p + vec2i(1, 1)) + L(p - vec2i(1, 1)) + L(p + vec2i(1, -1)) + L(p - vec2i(1, -1))) * 0.1;\n"
    "  var v = clamp(c + u.u_tone.w * (c - b), 0.0, 1.0);\n"
    "  v = pow(v, u.u_tone.x);\n"
    "  v = clamp((v - 0.5) * u.u_tone.y + 0.5 + u.u_tone.z, 0.0, 1.0);\n"
    "  let w = W(p);\n"
    "  if (u.u_edge.x > 0.0 && w > 0.0) {\n"
    "    let lap = 4.0 * w - W(p + vec2i(1, 0)) - W(p - vec2i(1, 0)) - W(p + vec2i(0, 1)) - W(p - vec2i(0, 1));\n"
    "    let e = smoothstep(u.u_edge.y, u.u_edge.z, lap / w);\n"
    "    v = max(v, e * u.u_edge.x);\n"
    "  }\n"
    "  return vec4f(v, 0.0, 0.0, 1.0);\n"
    "}\n";
static const char *QUANT_GLSL =
    "uniform sampler2D u_tonetex; uniform sampler2D u_thr; uniform vec4 u_q; uniform vec4 u_ink; uniform vec4 u_paper;\n"
    "out vec4 o;\n"
    "float T(ivec2 q) { int n = int(u_q.x); vec2 t = texelFetch(u_thr, ivec2(q.x - (q.x / n) * n, q.y - (q.y / n) * n), 0).rg; return (t.r * 65280.0 + t.g * 255.0) / 65536.0; }\n"
    "float V(ivec2 q) { q = clamp(q, ivec2(0), textureSize(u_tonetex, 0) - 1); return texelFetch(u_tonetex, q, 0).r; }\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  float v = V(p);\n"
    "  float b = v > T(p) ? 1.0 : 0.0;\n"
    "  o = vec4(mix(u_paper.rgb, u_ink.rgb, b), 1.0);\n"
    "}\n";
static const char *QUANT_WGSL =
    "struct U { u_q: vec4f, u_ink: vec4f, u_paper: vec4f };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@group(0) @binding(1) var u_tonetex: texture_2d<f32>;\n"
    "@group(0) @binding(2) var u_thr: texture_2d<f32>;\n"
    "fn T(q: vec2i) -> f32 { let n = i32(u.u_q.x); let t = textureLoad(u_thr, vec2i(q.x - (q.x / n) * n, q.y - (q.y / n) * n), 0).rg; return (t.r * 65280.0 + t.g * 255.0) / 65536.0; }\n"
    "fn V(q0: vec2i) -> f32 { let q = clamp(q0, vec2i(0), vec2i(textureDimensions(u_tonetex)) - 1); return textureLoad(u_tonetex, q, 0).r; }\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  let p = vec2i(cw_frag_coord(pos));\n"
    "  let v = V(p);\n"
    "  let b = select(0.0, 1.0, v > T(p));\n"
    "  return vec4f(mix(u.u_paper.rgb, u.u_ink.rgb, b), 1.0);\n"
    "}\n";

#define BN 64
static void put16(unsigned char *t, int i, unsigned v) { t[i * 4] = (unsigned char)(v >> 8), t[i * 4 + 1] = (unsigned char)v, t[i * 4 + 2] = 0, t[i * 4 + 3] = 255; }

/* the reference's own matrices: an 8x8 Bayer and a 64x64 void and cluster blue noise, ranked from an empty matrix */
static unsigned make_bayer_ref(void) {
  unsigned char t[64 * 4];
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      int a = x ^ y, r = ((a & 1) << 5) | ((y & 1) << 4) | ((a & 2) << 2) | ((y & 2) << 1) | ((a & 4) >> 1) | ((y & 4) >> 2);
      put16(t, y * 8 + x, (unsigned)((r * 2 + 1) * 65536 / 128));
    }
  return cw_pass_texture_new(8, 8, CW_RT_RGBA8, t);
}
static unsigned make_blue_ref(void) {
  enum { N = BN * BN };
  float *lut = malloc(sizeof(float) * N), *e = calloc(N, sizeof(float));
  unsigned char *on = calloc(N, 1), *t = malloc((size_t)N * 4);
  for (int y = 0; y < BN; y++)
    for (int x = 0; x < BN; x++) {
      int dx = x > BN / 2 ? BN - x : x, dy = y > BN / 2 ? BN - y : y;
      lut[y * BN + x] = expf(-(float)(dx * dx + dy * dy) / (2.0f * 1.5f * 1.5f));
    }
  for (int i = 0; i < N; i++) {
    uint32_t h = (uint32_t)i * 2654435761u;
    h ^= h >> 15, h *= 2246822519u, h ^= h >> 13;
    e[i] = (float)(h & 0xffff) * 1e-9f;
  }
  for (int rank = 0; rank < N; rank++) {
    int best = -1;
    float be = 1e30f;
    for (int i = 0; i < N; i++)
      if (!on[i] && e[i] < be) be = e[i], best = i;
    on[best] = 1;
    put16(t, best, (unsigned)(((uint64_t)rank * 2 + 1) * 65536 / (2 * N)));
    int bx = best % BN, by = best / BN;
    for (int y = 0; y < BN; y++) {
      int dy = (y - by + BN) % BN;
      for (int x = 0; x < BN; x++) e[y * BN + x] += lut[dy * BN + (x - bx + BN) % BN];
    }
  }
  unsigned tex = cw_pass_texture_new(BN, BN, CW_RT_RGBA8, t);
  free(lut), free(e), free(on), free(t);
  return tex;
}

/* the engine's matrices for the reference (trial_em): src/render/looks.c's blue noise generator, copied, so the
 * reference thresholds every pixel as the look does. The look anchors its patterns to the view's top left; the
 * reference indexes its matrix from the bottom left, so row r of its matrix is the engine's row (H - 1 - r) mod n. */
static void bn_add(float *e, const float *g, int pos, float sgn) {
  int px = pos % BN, py = pos / BN;
  for (int y = 0; y < BN; y++)
    for (int x = 0; x < BN; x++) e[y * BN + x] += sgn * g[((y - py + BN) % BN) * BN + ((x - px + BN) % BN)];
}
static void engine_blue_ranks(uint8_t *tx) {
  enum { N = BN * BN };
  float *g = malloc(sizeof(float) * N), *e = calloc(N, sizeof(float)), *e0 = malloc(sizeof(float) * N);
  uint8_t *on = calloc(N, 1), *proto = calloc(N, 1);
  int *rank = malloc(sizeof(int) * N);
  for (int y = 0; y < BN; y++)
    for (int x = 0; x < BN; x++) {
      int dx = x > BN / 2 ? x - BN : x, dy = y > BN / 2 ? y - BN : y;
      g[y * BN + x] = expf(-(float)(dx * dx + dy * dy) / (2.0f * 1.5f * 1.5f));
    }
  uint32_t r = 12345u;
  int ones = 0;
  while (ones < N / 10) {
    r = r * 1664525u + 1013904223u;
    int pos = (int)((r >> 8) % N);
    if (!on[pos]) on[pos] = 1, ones++, bn_add(e, g, pos, 1.0f);
  }
  for (int guard = 0; guard < 4 * N; guard++) {
    int tc = -1, lv = -1;
    for (int i = 0; i < N; i++)
      if (on[i] && (tc < 0 || e[i] > e[tc])) tc = i;
    on[tc] = 0, bn_add(e, g, tc, -1.0f);
    for (int i = 0; i < N; i++)
      if (!on[i] && (lv < 0 || e[i] < e[lv])) lv = i;
    on[lv] = 1, bn_add(e, g, lv, 1.0f);
    if (lv == tc) break;
  }
  memcpy(proto, on, N);
  memcpy(e0, e, sizeof(float) * N);
  for (int k = ones - 1; k >= 0; k--) {
    int tc = -1;
    for (int i = 0; i < N; i++)
      if (on[i] && (tc < 0 || e[i] > e[tc])) tc = i;
    on[tc] = 0, bn_add(e, g, tc, -1.0f), rank[tc] = k;
  }
  memcpy(on, proto, N);
  memcpy(e, e0, sizeof(float) * N);
  for (int k = ones; k < N; k++) {
    int lv = -1;
    for (int i = 0; i < N; i++)
      if (!on[i] && (lv < 0 || e[i] < e[lv])) lv = i;
    on[lv] = 1, bn_add(e, g, lv, 1.0f), rank[lv] = k;
  }
  for (int i = 0; i < N; i++) tx[i] = (uint8_t)(rank[i] * 256 / N);
  free(g), free(e), free(e0), free(on), free(proto), free(rank);
}
/* the engine's thresholds: blue noise (v + 0.5) / 256 from its byte v, Bayer (rank + 0.5) / 64, as 16 bit values */
static unsigned make_engine_ref(bool bayer, int out_h) {
  int n = bayer ? 8 : BN;
  unsigned char *t = malloc((size_t)n * n * 4);
  uint8_t ranks[BN * BN];
  if (!bayer) engine_blue_ranks(ranks);
  for (int y = 0; y < n; y++) {
    int ey = (((out_h - 1 - y) % n) + n) % n;
    for (int x = 0; x < n; x++) {
      unsigned v16;
      if (bayer) {
        int a = x ^ ey, r = ((a & 1) << 5) | ((ey & 1) << 4) | ((a & 2) << 2) | ((ey & 2) << 1) | ((a & 4) >> 1) | ((ey & 4) >> 2);
        v16 = (unsigned)(r * 1024 + 512);
      } else v16 = (unsigned)ranks[ey * BN + x] * 256u + 128u;
      put16(t, y * n + x, v16);
    }
  }
  unsigned tex = cw_pass_texture_new(n, n, CW_RT_RGBA8, t);
  free(t);
  return tex;
}

static struct { unsigned prog_tone, prog_quant, bayer, blue, em_blue, em_bayer; bool ready; } R;

static bool ref_active(const cw_pass_ctx *ctx, void *u) {
  (void)ctx, (void)u;
  return R.ready && (mode == M_TRIAL || mode == M_TRIAL_EM);
}
static void ref_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  bool wg = cw_gpu_backend() == CW_GPU_WEBGPU;
  if (!R.prog_tone) {
    R.prog_tone = wg ? cw_pass_program_wgsl("onebit_ref_tone", TONE_WGSL) : cw_pass_program("onebit_ref_tone", TONE_GLSL);
    R.prog_quant = wg ? cw_pass_program_wgsl("onebit_ref_quant", QUANT_WGSL) : cw_pass_program("onebit_ref_quant", QUANT_GLSL);
  }
  if (!R.prog_tone || !R.prog_quant) return;
  tone t = tone_now();
  const cw_rt *tn = cw_pass_rt(ctx, ctx->width, ctx->height, CW_RT_R16F, false);
  float lin[3];
  cw_pass_linear_depth_params(ctx, lin);
  cw_pass_bind_target(ctx, tn);
  cw_pass_use(R.prog_tone);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, ctx->depth_tex);
  if (!wg) cw_pass_uniform1i(R.prog_tone, "u_src", 0), cw_pass_uniform1i(R.prog_tone, "u_depth", 1);
  cw_pass_uniform4f(R.prog_tone, "u_tone", t.gamma, t.contrast, t.bias, t.sharpen);
  cw_pass_uniform4f(R.prog_tone, "u_edge", t.outline, t.out_from, t.out_to, 0);
  cw_pass_uniform4f(R.prog_tone, "u_lin", lin[0], lin[1], lin[2], 0);
  cw_pass_uniform4f(R.prog_tone, "u_key", t.world, t.block, t.hue_on, 0);
  cw_pass_fullscreen();
  cw_pass_bind_target(ctx, ctx->dst);
  cw_pass_use(R.prog_quant);
  cw_pass_bind_texture(0, tn->tex);
  bool bay = env("ONEBIT_PATTERN") && !strcmp(env("ONEBIT_PATTERN"), "bayer");
  cw_pass_bind_texture(1, mode == M_TRIAL_EM ? (bay ? R.em_bayer : R.em_blue) : (bay ? R.bayer : R.blue));
  if (!wg) cw_pass_uniform1i(R.prog_quant, "u_tonetex", 0), cw_pass_uniform1i(R.prog_quant, "u_thr", 1);
  cw_pass_uniform4f(R.prog_quant, "u_q", bay ? 8.0f : (float)BN, 2, 0, 0);
  cw_pass_uniform4f(R.prog_quant, "u_ink", 1, 1, 1, 1);
  cw_pass_uniform4f(R.prog_quant, "u_paper", 0, 0, 0, 1);
  cw_pass_fullscreen();
  cw_pass_rt_release(ctx, tn);
}

/* ── the CUSTOM look version: the same tone and threshold from the accessors (n = 1, blue noise) ── */
static const char *CUSTOM_GLSL =
    "float ob_L(vec2 px) {\n"
    "  vec3 c = cw_look_to_display(cw_look_scene(px));\n"
    "  float mx = max(max(c.r, c.g), c.b), mn = min(min(c.r, c.g), c.b);\n"
    "  vec4 k = cw_look_param(0);\n"
    "  float key = smoothstep(0.22, 0.5, (mx - mn) / max(mx, 1e-4)) * k.z;\n"
    "  float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "  return clamp(mix(mix(l * k.x, l, smoothstep(0.6, 0.95, l)), mx * k.y, key), 0.0, 1.0);\n"
    "}\n"
    "float ob_W(vec2 px) { float z = cw_look_linear_depth(px); return z > 1e30 ? 0.0 : 1.0 / max(z, 1e-4); }\n"
    "vec3 look(vec2 px) {\n"
    "  vec4 t = cw_look_param(1); vec4 e = cw_look_param(2);\n"
    "  float c = ob_L(px);\n"
    "  float b = (ob_L(px + vec2(2.0, 0.0)) + ob_L(px - vec2(2.0, 0.0)) + ob_L(px - vec2(0.0, 2.0)) + ob_L(px + vec2(0.0, 2.0))) * 0.15\n"
    "          + (ob_L(px + vec2(1.0, -1.0)) + ob_L(px - vec2(1.0, -1.0)) + ob_L(px + vec2(1.0, 1.0)) + ob_L(px - vec2(1.0, 1.0))) * 0.1;\n"
    "  float v = clamp(c + t.w * (c - b), 0.0, 1.0);\n"
    "  v = pow(v, t.x);\n"
    "  v = clamp((v - 0.5) * t.y + 0.5 + t.z, 0.0, 1.0);\n"
    "  float w = ob_W(px);\n"
    "  if (e.x > 0.0 && w > 0.0) {\n"
    "    float lap = 4.0 * w - ob_W(px + vec2(1.0, 0.0)) - ob_W(px - vec2(1.0, 0.0)) - ob_W(px - vec2(0.0, 1.0)) - ob_W(px + vec2(0.0, 1.0));\n"
    "    v = max(v, smoothstep(e.y, e.z, lap / w) * e.x);\n"
    "  }\n"
    "  return vec3(v > cw_look_blue_noise(px) ? 1.0 : 0.0);\n"
    "}\n";
static const char *CUSTOM_WGSL =
    "fn ob_L(px: vec2f) -> f32 {\n"
    "  let c = cw_look_to_display(cw_look_scene(px));\n"
    "  let mx = max(max(c.r, c.g), c.b); let mn = min(min(c.r, c.g), c.b);\n"
    "  let k = cw_look_param(0);\n"
    "  let key = smoothstep(0.22, 0.5, (mx - mn) / max(mx, 1e-4)) * k.z;\n"
    "  let l = dot(c, vec3f(0.2126, 0.7152, 0.0722));\n"
    "  return clamp(mix(mix(l * k.x, l, smoothstep(0.6, 0.95, l)), mx * k.y, key), 0.0, 1.0);\n"
    "}\n"
    "fn ob_W(px: vec2f) -> f32 { let z = cw_look_linear_depth(px); if (z > 1e30) { return 0.0; } return 1.0 / max(z, 1e-4); }\n"
    "fn look(px: vec2f) -> vec3f {\n"
    "  let t = cw_look_param(1); let e = cw_look_param(2);\n"
    "  let c = ob_L(px);\n"
    "  let b = (ob_L(px + vec2f(2.0, 0.0)) + ob_L(px - vec2f(2.0, 0.0)) + ob_L(px - vec2f(0.0, 2.0)) + ob_L(px + vec2f(0.0, 2.0))) * 0.15\n"
    "        + (ob_L(px + vec2f(1.0, -1.0)) + ob_L(px - vec2f(1.0, -1.0)) + ob_L(px + vec2f(1.0, 1.0)) + ob_L(px - vec2f(1.0, 1.0))) * 0.1;\n"
    "  var v = clamp(c + t.w * (c - b), 0.0, 1.0);\n"
    "  v = pow(v, t.x);\n"
    "  v = clamp((v - 0.5) * t.y + 0.5 + t.z, 0.0, 1.0);\n"
    "  let w = ob_W(px);\n"
    "  if (e.x > 0.0 && w > 0.0) {\n"
    "    let lap = 4.0 * w - ob_W(px + vec2f(1.0, 0.0)) - ob_W(px - vec2f(1.0, 0.0)) - ob_W(px - vec2f(0.0, 1.0)) - ob_W(px + vec2f(0.0, 1.0));\n"
    "    v = max(v, smoothstep(e.y, e.z, lap / w) * e.x);\n"
    "  }\n"
    "  return vec3f(select(0.0, 1.0, v > cw_look_blue_noise(px)));\n"
    "}\n";

static cw_look look_eng, look_custom, look_hb;
static cw_node cams[2];
static int split_applied;
static char last_key[160];

/* the engine look's desc for the switches now */
static cw_look_desc onebit_desc(int stage) {
  tone t = tone_now();
  const char *kind = env("ONEBIT_KIND");
  cw_look_desc d = cw_look_default(kind && !strcmp(kind, "diffuse") ? CW_LOOK_DITHER_DIFFUSE : CW_LOOK_DITHER_ORDERED);
  d.stage = stage ? CW_LOOK_STAGE_DISPLAY : CW_LOOK_STAGE_SCENE;
  const char *pat = env("ONEBIT_PATTERN");
  d.dither.pattern = pat && !strcmp(pat, "bayer") ? CW_DITHER_BAYER : CW_DITHER_BLUE_NOISE;
  d.dither.pixel = envf("ONEBIT_PIXEL", 1);
  d.dither.output_pixels = envi("ONEBIT_OUTPX", 1) != 0;
  d.dither.hue_value = t.hue_on ? t.block : 0;
  d.dither.grey_gain = t.world;
  d.dither.sharpen = t.sharpen;
  d.dither.outline = t.outline;
  d.dither.outline_from = t.out_from, d.dither.outline_to = t.out_to;
  d.dither.gamma = t.gamma;
  d.dither.contrast = t.contrast;
  d.dither.bias = t.bias;
  return d;
}

static void apply(void) {
  cw_tree *tree = cw_scene_app_tree();
  cw_pipeline *p = cw_pipeline_of(tree);
  const char *m = env("ONEBIT");
  mode = M_LOOK;
  for (int i = 0; m && i < M_COUNT; i++)
    if (!strcmp(m, MODES[i])) mode = i;
  /* the looks again when a switch changed */
  char key[160];
#define E_(n) (env(n) ? env(n) : "")
  snprintf(key, sizeof key, "%d|%s|%s|%s|%s|%s|%s", mode, E_("ONEBIT_ONLY"), E_("ONEBIT_PATTERN"), E_("ONEBIT_PIXEL"), E_("ONEBIT_OUTPX"), E_("ONEBIT_SHARPEN"), E_("ONEBIT_KIND"));
#undef E_
  if (strcmp(key, last_key)) {
    snprintf(last_key, sizeof last_key, "%s", key);
    cw_look_desc d = onebit_desc(mode != M_SCENE);
    if (CW_HANDLE_IS_NULL(look_eng)) look_eng = cw_look_new(&d);
    else cw_look_set(look_eng, &d);
    tone t = tone_now();
    cw_look_desc c = cw_look_default(CW_LOOK_CUSTOM);
    c.stage = CW_LOOK_STAGE_DISPLAY;
    c.custom.glsl = CUSTOM_GLSL, c.custom.wgsl = CUSTOM_WGSL;
    float pr[12] = { t.world, t.block, t.hue_on, 0, t.gamma, t.contrast, t.bias, t.sharpen, t.outline, t.out_from, t.out_to, 0 };
    memcpy(c.custom.params, pr, sizeof pr);
    if (CW_HANDLE_IS_NULL(look_custom)) look_custom = cw_look_new(&c);
    else cw_look_set(look_custom, &c);
  }
  cw_look a = (cw_look){ 0 }, b = (cw_look){ 0 };
  if (mode == M_LOOK || mode == M_SCENE) b = look_eng;
  if (mode == M_CUSTOM) b = look_custom;
  int split = envi("ONEBIT_SPLIT", 0);
  if (split != split_applied) {
    split_applied = split;
    if (split) {
      cw_pipeline_split(p, cams, 2);
      cw_pipeline_set_views_shared_post(p, split == 1);
    } else cw_pipeline_set_views(p, NULL, 0);
  }
  /* a fade between the look (DISPLAY) and half blocks (SCENE), then the end look alone, as a game changes looks */
  int xf = envi("ONEBIT_XFADE", 0);
  if (xf > 0) {
    if (CW_HANDLE_IS_NULL(look_hb)) {
      cw_look_desc h = cw_look_default(CW_LOOK_HALFBLOCK);
      h.halfblock.rows = 120, h.halfblock.levels = 0;
      look_hb = cw_look_new(&h);
    }
    bool rev = envi("ONEBIT_XREV", 0) != 0;
    cw_look from = rev ? look_hb : look_eng, to = rev ? look_eng : look_hb;
    const char *hw = env("ONEBIT_HOW");
    cw_look_mix how = { .blend = hw && !strcmp(hw, "fade") ? CW_LOOK_FADE : hw && !strcmp(hw, "wipe") ? CW_LOOK_WIPE : CW_LOOK_DISSOLVE,
                        .softness = 0.35f, .scale = 28 };
    float t = env("ONEBIT_XT") ? envf("ONEBIT_XT", 0) : CW_CLAMP((float)(tick - 60) / (float)xf, 0.0f, 1.0f);
    weight = t;
    if (t >= 1) cw_pipeline_set_looks(p, to, (cw_look){ 0 }, 0, NULL);
    else if (t <= 0) cw_pipeline_set_looks(p, from, (cw_look){ 0 }, 0, NULL);
    else cw_pipeline_set_looks(p, from, to, t, &how);
    return;
  }
  if (envi("ONEBIT_PAIR", 0)) {
    const char *hw = env("ONEBIT_HOW");
    cw_look_mix how = { .blend = hw && !strcmp(hw, "fade") ? CW_LOOK_FADE : hw && !strcmp(hw, "dissolve") ? CW_LOOK_DISSOLVE : CW_LOOK_WIPE };
    weight = envf("ONEBIT_T", 0.5f);
    cw_pipeline_set_looks(p, look_eng, look_custom, weight, &how);
    return;
  }
  float w = 1;
  int ramp = envi("ONEBIT_RAMP", 0), flick = envi("ONEBIT_FLICKER", 0), at = 60;
  if (ramp > 0) w = CW_CLAMP((float)(tick - at) / (float)ramp, 0.0f, 1.0f);
  if (flick > 0) w = tick >= at ? (float)(((tick - at) / flick) & 1) : 0.0f;
  weight = w;
  cw_pipeline_set_looks(p, a, b, w, NULL);
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("onebit.tick", &tick);
  cw_debug_i32("onebit.mode", &mode);
  cw_debug_i32("onebit.ran", &ran);
  cw_debug_f32("onebit.gpu_ms", &gpu_ms);
  cw_debug_f32("onebit.weight", &weight);
  cw_debug_i32("onebit.array", &array_used);
  cw_tree *tree = cw_scene_app_tree();
  cw_node world = cw_node_get(cw_tree_root(tree), "World");
  cw_node cam = cams[0] = cw_node_get(world, "Camera");
  cw_node3d_look_at(cam, (t3_vec3){ 0, 1.2f, -1 }, (t3_vec3){ 0, 1, 0 });
  cams[1] = cw_node_new("Camera3D", "Camera2");
  cw_node_add_child(world, cams[1]);
  cw_node_set(cams[1], "fov", cw_val_float(55));
  cw_node_set(cams[1], "near", cw_val_float(0.1f));
  cw_node_set(cams[1], "far", cw_val_float(300));
  cw_node3d_set_position(cams[1], (t3_vec3){ -9, 3.5f, 6 });
  cw_node3d_look_at(cams[1], (t3_vec3){ 0, 1.2f, -2 }, (t3_vec3){ 0, 1, 0 });
  cw_node post = cw_node_get(world, "Post");
  cw_node_set(post, "vignette", cw_val_bool(true));
  cw_node_set(post, "vignette_intensity", cw_val_float(0.35f));
  if (!envi("ONEBIT_BLOOM", 1)) cw_node_set(post, "bloom", cw_val_bool(false));
  /* the reference pass and its matrices */
  R.bayer = make_bayer_ref();
  R.blue = make_blue_ref();
  R.em_blue = make_engine_ref(false, ONEBIT_H);
  R.em_bayer = make_engine_ref(true, ONEBIT_H);
  cw_pipeline_add_pass(cw_pipeline_of(tree), &(cw_pass_desc){ .name = "onebit_ref", .phase = CW_PHASE_POST, .order = 9000,
                                                             .reads = CW_RES_COLOR | CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                                             .active = ref_active, .execute = ref_exec, .webgpu = true });
  R.ready = true;
}

static void fixed_update(void *u, double dt) {
  (void)u, (void)dt;
  tick++;
  if (envi("ONEBIT_ORBIT", 0)) {
    cw_node cam = cw_node_get(cw_node_get(cw_tree_root(cw_scene_app_tree()), "World"), "Camera");
    float a = (float)tick * 0.01f;
    cw_node3d_set_position(cam, (t3_vec3){ sinf(a) * 11, 4.5f, cosf(a) * 11 });
    cw_node3d_look_at(cam, (t3_vec3){ 0, 1.2f, -1 }, (t3_vec3){ 0, 1, 0 });
  }
  apply();
}

static void render(void *u, double alpha) {
  (void)u, (void)alpha;
  const cw_pass_stat *st;
  int n = cw_pipeline_pass_stats(cw_pipeline_of(cw_scene_app_tree()), &st);
  const char *want = mode == M_TRIAL || mode == M_TRIAL_EM ? "onebit_ref" : mode == M_SCENE ? "looks" : "looks_display";
  ran = 0;
  array_used = cw_pipeline_views_array_used(cw_pipeline_of(cw_scene_app_tree()), NULL) ? 1 : 0;
  for (int i = 0; i < n; i++)
    if (!strcmp(st[i].name, want)) gpu_ms = st[i].gpu_ms_avg, ran = st[i].ran;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  static cw_app_desc base = { .title = "looks_onebit", .width = ONEBIT_H * 16 / 9, .height = ONEBIT_H, .render_scale = 1,
                              .debug = true, .init = init, .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
