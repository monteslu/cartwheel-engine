/* render2d global illumination (cw_r2d_gi): radiance cascades over a
 * distance field of the scene, as fx passes (fx.h) on GL and WebGPU.
 *
 *   source    at the field's size: what emits (emission x colour, as drawn,
 *             HDR included) and what reflects (occluders: colour x the direct
 *             light on them, plus last frame's bounce x bounce), and where
 *             something is (occluders and emitters)
 *   field     jump flood of that occupancy (the same flood as the shadows)
 *   cascades  cascade N has probes every 2^(N+1) field pixels, each with
 *             4^(N+1) directions, in one texture the field's size; each
 *             direction marches its interval [l(4^N - 1)/3, l(4^(N+1) - 1)/3]
 *             through the field, and what it does not hit takes the four
 *             matching directions of cascade N+1's four nearest probes
 *             (merged top down, bilinear between probes)
 *   gather    each field pixel averages cascade 0's directions of its four
 *             probes: the light arriving there from every direction
 *   apply     that irradiance x intensity is added to the light buffer, so
 *             it lights colour as a light does (and is shadowed by nothing
 *             but the occluders it was traced against)
 *
 * The result of each frame feeds the next one's bounce: light bounces off
 * occluders as many times as frames go by, fading by `bounce` each time. */
#include <math.h>
#include <string.h>

#include "fx.h"
#include "cartwheel/quality.h"

#define MAX_CASCADES 8

static struct {
  fx_rt src, seed[2], casc[2], gi[2];
  int cur; /* gi[cur] is this frame's */
  int w, h;
  bool valid_prev;
} G;

/* ── shaders ───────────────────────────────────────────────────────────── */

/* source: t0 occluder mask (canvas), t1 scene colour, t2 material, t3 direct
 * light; u_v[0] = light buffer scale back, footprint taps per axis */
static const char *GL_SRC =
    "void main() {\n"
    "  vec2 q = floor(v_uv * u_size);\n"
    "  int n = int(u_v[0].y);\n"
    "  vec3 rad = vec3(0.0); float occ = 0.0, cnt = 0.0;\n"
    "  for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {\n"
    "    if (i >= n || j >= n) continue;\n"
    "    vec2 uv = (q + (vec2(i, j) + 0.5) / float(n)) / u_size;\n"
    "    float m = texture(t0, uv).r;\n"
    "    vec4 s = texture(t1, uv), mt = texture(t2, uv);\n"
    "    float e = mt.a > 0.004 ? mt.r / mt.a : 0.0;\n"
    "    vec3 l = texture(t3, uv).rgb * u_v[0].x;\n"
    "    rad += s.rgb * e * step(0.1, e) + s.rgb * l * m * (1.0 - e);\n"
    "    occ = max(occ, max(m, step(0.1, e) * s.a));\n"
    "    cnt += 1.0;\n"
    "  }\n"
    "  o_col = vec4(rad / cnt, occ);\n"
    "}\n";
static const char *WG_SRC =
    "@fragment fn fs(i0: VO) -> @location(0) vec4f {\n"
    "  let q = floor(i0.uv * fx.size);\n"
    "  let n = i32(fx.v[0].y);\n"
    "  var rad = vec3f(0.0); var occ = 0.0; var cnt = 0.0;\n"
    "  for (var j = 0; j < 4; j++) { for (var i = 0; i < 4; i++) {\n"
    "    if (i >= n || j >= n) { continue; }\n"
    "    let uv = (q + (vec2f(f32(i), f32(j)) + 0.5) / f32(n)) / fx.size;\n"
    "    let m = textureSampleLevel(t0, s_lin, uv, 0.0).r;\n"
    "    let s = textureSampleLevel(t1, s_lin, uv, 0.0); let mt = textureSampleLevel(t2, s_lin, uv, 0.0);\n"
    "    var e = 0.0;\n"
    "    if (mt.a > 0.004) { e = mt.r / mt.a; }\n"
    "    let l = textureSampleLevel(t3, s_lin, uv, 0.0).rgb * fx.v[0].x;\n"
    "    rad += s.rgb * e * step(0.1, e) + s.rgb * l * m * (1.0 - e);\n"
    "    occ = max(occ, max(m, step(0.1, e) * s.a));\n"
    "    cnt += 1.0;\n"
    "  } }\n"
    "  return vec4f(rad / cnt, occ);\n"
    "}\n";

/* last frame's bounce off the occluders (not off what glows: that would feed
 * itself): t0 occluder mask, t1 scene colour (its albedo, 1 at most), t2
 * last frame's irradiance, t3 material; u_v[0].x = bounce. Added. */
static const char *GL_BOUNCE =
    "void main() {\n"
    "  float m = texture(t0, v_uv).r;\n"
    "  vec4 s = texture(t1, v_uv), mt = texture(t3, v_uv);\n"
    "  float e = mt.a > 0.004 ? clamp(mt.r / mt.a, 0.0, 1.0) : 0.0;\n"
    "  vec3 g = texture(t2, v_uv).rgb;\n"
    "  o_col = vec4(min(s.rgb, vec3(1.0)) * g * u_v[0].x * m * (1.0 - e), 0.0);\n"
    "}\n";
static const char *WG_BOUNCE =
    "@fragment fn fs(i: VO) -> @location(0) vec4f {\n"
    "  let m = textureSampleLevel(t0, s_lin, i.uv, 0.0).r;\n"
    "  let s = textureSampleLevel(t1, s_lin, i.uv, 0.0); let mt = textureSampleLevel(t3, s_lin, i.uv, 0.0);\n"
    "  var e = 0.0;\n"
    "  if (mt.a > 0.004) { e = clamp(mt.r / mt.a, 0.0, 1.0); }\n"
    "  let g = textureSampleLevel(t2, s_lin, i.uv, 0.0).rgb;\n"
    "  return vec4f(min(s.rgb, vec3f(1.0)) * g * fx.v[0].x * m * (1.0 - e), 0.0);\n"
    "}\n";

/* seeds from the source's occupancy (alpha) */
static const char *GL_SEED =
    "void main() {\n"
    "  vec2 q = floor(v_uv * u_size);\n"
    "  if (texelFetch(t0, ivec2(q), 0).a < 0.5) { o_col = vec4(1.0); return; }\n"
    "  vec2 hi = floor(q / 256.0); vec2 lo = q - hi * 256.0;\n"
    "  o_col = vec4(lo.x, hi.x, lo.y, hi.y) / 255.0;\n"
    "}\n";
static const char *WG_SEED =
    "@fragment fn fs(i: VO) -> @location(0) vec4f {\n"
    "  let q = floor(i.uv * fx.size);\n"
    "  if (textureLoad(t0, vec2i(q), 0).a < 0.5) { return vec4f(1.0); }\n"
    "  let hi = floor(q / 256.0); let lo = q - hi * 256.0;\n"
    "  return vec4f(lo.x, hi.x, lo.y, hi.y) / 255.0;\n"
    "}\n";
static const char *GL_JFA = /* the same flood as the shadows' (light.c) */
    "void main() {\n"
    "  ivec2 q = ivec2(v_uv * u_size), sz = ivec2(u_size);\n"
    "  int st = int(u_v[0].x);\n"
    "  vec2 best = vec2(65535.0); float bd = 1e20;\n"
    "  for (int j = -1; j <= 1; j++)\n"
    "    for (int i = -1; i <= 1; i++) {\n"
    "      ivec2 n = clamp(q + ivec2(i, j) * st, ivec2(0), sz - 1);\n"
    "      vec4 s = texelFetch(t0, n, 0);\n"
    "      vec2 sp = vec2(s.r * 255.0 + s.g * 65280.0, s.b * 255.0 + s.a * 65280.0);\n"
    "      if (sp.x > 65534.0) continue;\n"
    "      vec2 dd = sp - vec2(q);\n"
    "      float d = dot(dd, dd);\n"
    "      if (d < bd) { bd = d; best = sp; }\n"
    "    }\n"
    "  if (best.x > 65534.0) { o_col = vec4(1.0); return; }\n"
    "  vec2 hi = floor(best / 256.0); vec2 lo = best - hi * 256.0;\n"
    "  o_col = vec4(lo.x, hi.x, lo.y, hi.y) / 255.0;\n"
    "}\n";
static const char *WG_JFA =
    "@fragment fn fs(i0: VO) -> @location(0) vec4f {\n"
    "  let q = vec2i(i0.uv * fx.size); let sz = vec2i(fx.size);\n"
    "  let st = i32(fx.v[0].x);\n"
    "  var best = vec2f(65535.0); var bd = 1e20;\n"
    "  for (var j = -1; j <= 1; j++) {\n"
    "    for (var i = -1; i <= 1; i++) {\n"
    "      let n = clamp(q + vec2i(i, j) * st, vec2i(0), sz - 1);\n"
    "      let s = textureLoad(t0, n, 0);\n"
    "      let sp = vec2f(s.r * 255.0 + s.g * 65280.0, s.b * 255.0 + s.a * 65280.0);\n"
    "      if (sp.x > 65534.0) { continue; }\n"
    "      let dd = sp - vec2f(q);\n"
    "      let d = dot(dd, dd);\n"
    "      if (d < bd) { bd = d; best = sp; }\n"
    "    }\n"
    "  }\n"
    "  if (best.x > 65534.0) { return vec4f(1.0); }\n"
    "  let hi = floor(best / 256.0); let lo = best - hi * 256.0;\n"
    "  return vec4f(lo.x, hi.x, lo.y, hi.y) / 255.0;\n"
    "}\n";

/* one cascade, merged with the one above. t0 seeds, t1 source, t2 cascade
 * N+1. u_v[0] = block (directions per side, = probe spacing), interval
 * start, end, steps; u_v[1] = has upper (1/0) */
static const char *GL_CASCADE =
    "vec2 seedp(ivec2 q) { vec4 s = texelFetch(t0, q, 0); return vec2(s.r * 255.0 + s.g * 65280.0, s.b * 255.0 + s.a * 65280.0); }\n"
    "float field(vec2 p) { ivec2 q = clamp(ivec2(p), ivec2(0), ivec2(u_size) - 1); return length(seedp(q) + 0.5 - p); }\n"
    "vec4 upper(ivec2 pr, int du, int bu) {\n"
    "  ivec2 np = ivec2(u_size) / bu;\n"
    "  pr = clamp(pr, ivec2(0), np - 1);\n"
    "  return texelFetch(t2, pr * bu + ivec2(du % bu, du / bu), 0);\n"
    "}\n"
    "void main() {\n"
    "  ivec2 x = ivec2(v_uv * u_size);\n"
    "  int b = int(u_v[0].x);\n"
    "  ivec2 probe = x / b;\n"
    "  int d = (x.x % b) + (x.y % b) * b, nd = b * b;\n"
    "  float ang = (float(d) + 0.5) / float(nd) * 6.2831853;\n"
    "  vec2 dir = vec2(cos(ang), sin(ang));\n"
    "  vec2 c = (vec2(probe) + 0.5) * float(b);\n"
    "  float t = u_v[0].y, tend = u_v[0].z;\n"
    "  vec4 r = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "  int n = int(u_v[0].w);\n"
    "  for (int i = 0; i < 32; i++) {\n"
    "    if (i >= n || t >= tend) break;\n"
    "    vec2 p = c + dir * t;\n"
    "    if (p.x < 0.0 || p.y < 0.0 || p.x >= u_size.x || p.y >= u_size.y) break;\n"
    "    float h = field(p);\n"
    "    if (h < 0.75) { r = vec4(texelFetch(t1, ivec2(p), 0).rgb, 0.0); break; }\n"
    "    t += max(h - 0.5, 0.75);\n"
    "  }\n"
    "  if (r.a > 0.0 && u_v[1].x > 0.5) {\n"
    "    int bu = b * 2;\n"
    "    vec2 f = c / float(bu) - 0.5;\n"
    "    ivec2 b0 = ivec2(floor(f));\n"
    "    vec2 w = f - vec2(b0);\n"
    "    vec4 acc = vec4(0.0);\n"
    "    for (int k = 0; k < 4; k++) {\n"
    "      int du = d * 4 + k;\n"
    "      vec4 a00 = upper(b0, du, bu), a10 = upper(b0 + ivec2(1, 0), du, bu);\n"
    "      vec4 a01 = upper(b0 + ivec2(0, 1), du, bu), a11 = upper(b0 + ivec2(1, 1), du, bu);\n"
    "      acc += mix(mix(a00, a10, w.x), mix(a01, a11, w.x), w.y);\n"
    "    }\n"
    "    acc *= 0.25;\n"
    "    r = vec4(r.rgb + acc.rgb, acc.a);\n"
    "  }\n"
    "  o_col = r;\n"
    "}\n";
static const char *WG_CASCADE =
    "fn seedp(q: vec2i) -> vec2f { let s = textureLoad(t0, q, 0); return vec2f(s.r * 255.0 + s.g * 65280.0, s.b * 255.0 + s.a * 65280.0); }\n"
    "fn field(p: vec2f) -> f32 { let q = clamp(vec2i(p), vec2i(0), vec2i(fx.size) - 1); return length(seedp(q) + 0.5 - p); }\n"
    "fn upper(pr0: vec2i, du: i32, bu: i32) -> vec4f {\n"
    "  let np = vec2i(fx.size) / bu;\n"
    "  let pr = clamp(pr0, vec2i(0), np - 1);\n"
    "  return textureLoad(t2, pr * bu + vec2i(du % bu, du / bu), 0);\n"
    "}\n"
    "@fragment fn fs(i0: VO) -> @location(0) vec4f {\n"
    "  let x = vec2i(i0.uv * fx.size);\n"
    "  let b = i32(fx.v[0].x);\n"
    "  let probe = x / b;\n"
    "  let d = (x.x % b) + (x.y % b) * b; let nd = b * b;\n"
    "  let ang = (f32(d) + 0.5) / f32(nd) * 6.2831853;\n"
    "  let dir = vec2f(cos(ang), sin(ang));\n"
    "  let c = (vec2f(probe) + 0.5) * f32(b);\n"
    "  var t = fx.v[0].y; let tend = fx.v[0].z;\n"
    "  var r = vec4f(0.0, 0.0, 0.0, 1.0);\n"
    "  let n = i32(fx.v[0].w);\n"
    "  for (var i = 0; i < 32; i++) {\n"
    "    if (i >= n || t >= tend) { break; }\n"
    "    let p = c + dir * t;\n"
    "    if (p.x < 0.0 || p.y < 0.0 || p.x >= fx.size.x || p.y >= fx.size.y) { break; }\n"
    "    let h = field(p);\n"
    "    if (h < 0.75) { r = vec4f(textureLoad(t1, vec2i(p), 0).rgb, 0.0); break; }\n"
    "    t += max(h - 0.5, 0.75);\n"
    "  }\n"
    "  if (r.a > 0.0 && fx.v[1].x > 0.5) {\n"
    "    let bu = b * 2;\n"
    "    let f = c / f32(bu) - 0.5;\n"
    "    let b0 = vec2i(floor(f));\n"
    "    let w = f - vec2f(b0);\n"
    "    var acc = vec4f(0.0);\n"
    "    for (var k = 0; k < 4; k++) {\n"
    "      let du = d * 4 + k;\n"
    "      let a00 = upper(b0, du, bu); let a10 = upper(b0 + vec2i(1, 0), du, bu);\n"
    "      let a01 = upper(b0 + vec2i(0, 1), du, bu); let a11 = upper(b0 + vec2i(1, 1), du, bu);\n"
    "      acc += mix(mix(a00, a10, w.x), mix(a01, a11, w.x), w.y);\n"
    "    }\n"
    "    acc *= 0.25;\n"
    "    r = vec4f(r.rgb + acc.rgb, acc.a);\n"
    "  }\n"
    "  return r;\n"
    "}\n";

/* gather: cascade 0 (2x2 directions per probe, probes every 2 pixels),
 * bilinear between the four nearest probes, the four directions averaged */
static const char *GL_GATHER =
    "vec3 probe(ivec2 pr) {\n"
    "  ivec2 np = ivec2(u_size) / 2;\n"
    "  pr = clamp(pr, ivec2(0), np - 1);\n"
    "  ivec2 o = pr * 2;\n"
    "  return (texelFetch(t0, o, 0).rgb + texelFetch(t0, o + ivec2(1, 0), 0).rgb + texelFetch(t0, o + ivec2(0, 1), 0).rgb +\n"
    "          texelFetch(t0, o + ivec2(1, 1), 0).rgb) * 0.25;\n"
    "}\n"
    "void main() {\n"
    "  vec2 f = (v_uv * u_size) / 2.0 - 0.5;\n"
    "  ivec2 b0 = ivec2(floor(f));\n"
    "  vec2 w = f - vec2(b0);\n"
    "  vec3 g = mix(mix(probe(b0), probe(b0 + ivec2(1, 0)), w.x), mix(probe(b0 + ivec2(0, 1)), probe(b0 + ivec2(1, 1)), w.x), w.y);\n"
    "  o_col = vec4(g, 1.0);\n"
    "}\n";
static const char *WG_GATHER =
    "fn probe(pr0: vec2i) -> vec3f {\n"
    "  let np = vec2i(fx.size) / 2;\n"
    "  let pr = clamp(pr0, vec2i(0), np - 1);\n"
    "  let o = pr * 2;\n"
    "  return (textureLoad(t0, o, 0).rgb + textureLoad(t0, o + vec2i(1, 0), 0).rgb + textureLoad(t0, o + vec2i(0, 1), 0).rgb +\n"
    "          textureLoad(t0, o + vec2i(1, 1), 0).rgb) * 0.25;\n"
    "}\n"
    "@fragment fn fs(i: VO) -> @location(0) vec4f {\n"
    "  let f = (i.uv * fx.size) / 2.0 - 0.5;\n"
    "  let b0 = vec2i(floor(f));\n"
    "  let w = f - vec2f(b0);\n"
    "  let g = mix(mix(probe(b0), probe(b0 + vec2i(1, 0)), w.x), mix(probe(b0 + vec2i(0, 1)), probe(b0 + vec2i(1, 1)), w.x), w.y);\n"
    "  return vec4f(g, 1.0);\n"
    "}\n";

/* apply: the irradiance (bilinear) x intensity x light buffer scale, added */
static const char *GL_APPLY = "void main() { o_col = vec4(texture(t0, v_uv).rgb * u_v[0].x, 0.0); }\n";
static const char *WG_APPLY =
    "@fragment fn fs(i: VO) -> @location(0) vec4f { return vec4f(textureSampleLevel(t0, s_lin, i.uv, 0.0).rgb * fx.v[0].x, 0.0); }\n";

static fx_prog p_src = { "r2d gi source", NULL, NULL, 0, 0, 0, NULL, false };
static fx_prog p_bounce = { "r2d gi bounce", NULL, NULL, 0, 0, 0, NULL, false };
static fx_prog p_seed = { "r2d gi seed", NULL, NULL, 0, 0, 0, NULL, false };
static fx_prog p_jfa = { "r2d gi flood", NULL, NULL, 0, 0, 0, NULL, false };
static fx_prog p_casc = { "r2d gi cascade", NULL, NULL, 0, 0, 0, NULL, false };
static fx_prog p_gather = { "r2d gi gather", NULL, NULL, 0, 0, 0, NULL, false };
static fx_prog p_apply = { "r2d gi apply", NULL, NULL, 0, 0, 0, NULL, false };

static void progs_init(void) {
  if (p_src.glsl) return;
  p_src.glsl = GL_SRC, p_src.wgsl = WG_SRC;
  p_bounce.glsl = GL_BOUNCE, p_bounce.wgsl = WG_BOUNCE;
  p_seed.glsl = GL_SEED, p_seed.wgsl = WG_SEED;
  p_jfa.glsl = GL_JFA, p_jfa.wgsl = WG_JFA;
  p_casc.glsl = GL_CASCADE, p_casc.wgsl = WG_CASCADE;
  p_gather.glsl = GL_GATHER, p_gather.wgsl = WG_GATHER;
  p_apply.glsl = GL_APPLY, p_apply.wgsl = WG_APPLY;
}

/* the field's share of the canvas and the march steps, by effects detail;
 * 0 share: off */
static void gi_quality(const cw_r2d_gi *g, float *share, int *steps) {
  int e = CW_CLAMP(cw_gfx_effective(CW_GFX_EFFECTS), 0, 4);
  static const float sh[5] = { 0, 0.125f, 0.25f, 0.25f, 0.5f };
  static const int st[5] = { 0, 8, 12, 16, 24 };
  *share = sh[e];
  *steps = st[e];
  if (g->resolution > 0 && e >= CW_QUALITY_HIGH) *share = CW_CLAMP(g->resolution, 0.0625f, 1.0f);
}

bool r2d_gi_enabled(void) {
  if (!R.has_gi) return false;
  float s;
  int st;
  gi_quality(&R.gi, &s, &st);
  return s > 0;
}

void r2d_gi_run(const fx_rt *mask, const fx_rt *scene, const fx_rt *mat, fx_rt *light, float light_scale) {
  progs_init();
  float share;
  int steps;
  gi_quality(&R.gi, &share, &steps);
  if (share <= 0) return;
  /* even sizes: cascade 0 holds 2x2 directions per probe */
  int w = CW_MAX(16, ((int)lroundf((float)R.canvas_w * share) + 1) & ~1);
  int h = CW_MAX(16, ((int)lroundf((float)R.canvas_h * share) + 1) & ~1);
  if (w != G.w || h != G.h) G.valid_prev = false;
  G.w = w, G.h = h;
  fx_fmt hf = fx_hdr_format();
  fx_rt_ensure(&G.src, w, h, hf);
  fx_rt_ensure(&G.seed[0], w, h, FX_RGBA8);
  fx_rt_ensure(&G.seed[1], w, h, FX_RGBA8);
  fx_rt_ensure(&G.casc[0], w, h, hf);
  fx_rt_ensure(&G.casc[1], w, h, hf);
  fx_rt_ensure(&G.gi[0], w, h, hf);
  fx_rt_ensure(&G.gi[1], w, h, hf);
  float bounce = R.gi.bounce < 0 ? 0 : R.gi.bounce > 0 ? CW_MIN(R.gi.bounce, 0.95f) : 0.5f;
  /* source */
  {
    float v[8][4] = { { 1.0f / light_scale, (float)CW_CLAMP((int)lroundf(1.0f / share / 2.0f), 1, 4), 0, 0 } };
    fx_dst d = fx_dst_of(&G.src);
    const fx_rt *in[4] = { mask, scene, mat, light };
    fx_pass(&p_src, &d, false, NULL, in, v, FX_BLEND_NONE);
    if (G.valid_prev && bounce > 0) {
      float bv[8][4] = { { bounce, 0, 0, 0 } };
      const fx_rt *bin[4] = { mask, scene, &G.gi[G.cur], mat };
      fx_dst td = fx_dst_of(&G.casc[0]);
      fx_pass(&p_bounce, &td, true, NULL, bin, bv, FX_BLEND_NONE);
      const fx_rt *ain[4] = { &G.casc[0], NULL, NULL, NULL };
      fx_pass(&p_apply, &d, false, NULL, ain, (float[8][4]){ { 1, 0, 0, 0 } }, FX_BLEND_ADD);
    }
  }
  /* field */
  {
    fx_dst d0 = fx_dst_of(&G.seed[0]);
    const fx_rt *in[4] = { &G.src, NULL, NULL, NULL };
    fx_pass(&p_seed, &d0, false, NULL, in, NULL, FX_BLEND_NONE);
    int cur = 0, st = 1;
    while (st * 2 < CW_MAX(w, h)) st *= 2;
    for (;; st /= 2) {
      float v[8][4] = { { (float)(st < 1 ? 1 : st), 0, 0, 0 } };
      fx_dst d = fx_dst_of(&G.seed[cur ^ 1]);
      const fx_rt *src[4] = { &G.seed[cur], NULL, NULL, NULL };
      fx_pass(&p_jfa, &d, false, NULL, src, v, FX_BLEND_NONE);
      cur ^= 1;
      if (st < 1) break;
    }
    if (cur != 0) { fx_rt t = G.seed[0]; G.seed[0] = G.seed[1]; G.seed[1] = t; }
  }
  /* cascades, top down: enough that the last interval reaches across the field */
  const float l0 = 2.0f;
  float diag = sqrtf((float)w * (float)w + (float)h * (float)h);
  int nc = 1;
  while (nc < MAX_CASCADES && l0 * (powf(4.0f, (float)nc) - 1) / 3.0f < diag) nc++;
  /* the top cascade's block must still divide into whole probes */
  while (nc > 1 && (1 << nc) > CW_MIN(w, h) / 2) nc--;
  int out = 0;
  for (int c = nc - 1; c >= 0; c--) {
    int b = 1 << (c + 1);
    float t0 = l0 * (powf(4.0f, (float)c) - 1) / 3.0f, t1 = l0 * (powf(4.0f, (float)(c + 1)) - 1) / 3.0f;
    if (c == nc - 1) t1 = diag;
    int dst = out ^ 1;
    float v[8][4] = { { (float)b, t0, t1, (float)steps }, { c < nc - 1 ? 1.0f : 0.0f, 0, 0, 0 } };
    fx_dst d = fx_dst_of(&G.casc[dst]);
    const fx_rt *in[4] = { &G.seed[0], &G.src, c < nc - 1 ? &G.casc[out] : NULL, NULL };
    fx_pass(&p_casc, &d, false, NULL, in, v, FX_BLEND_NONE);
    out = dst;
  }
  /* gather into this frame's irradiance, then add it to the light buffer */
  int next = G.cur ^ 1;
  fx_dst gd = fx_dst_of(&G.gi[next]);
  const fx_rt *gin[4] = { &G.casc[out], NULL, NULL, NULL };
  fx_pass(&p_gather, &gd, false, NULL, gin, NULL, FX_BLEND_NONE);
  G.cur = next;
  G.valid_prev = true;
  float k = (R.gi.intensity != 0 ? R.gi.intensity : 1.0f) * light_scale;
  fx_dst ld = fx_dst_of(light);
  const fx_rt *ain[4] = { &G.gi[G.cur], NULL, NULL, NULL };
  fx_pass(&p_apply, &ld, false, NULL, ain, (float[8][4]){ { k, 0, 0, 0 } }, FX_BLEND_ADD);
  R.stats.gi_cascades = (uint32_t)nc;
}

void r2d_gi_shutdown(void) {
  fx_rt *rts[] = { &G.src, &G.seed[0], &G.seed[1], &G.casc[0], &G.casc[1], &G.gi[0], &G.gi[1] };
  for (size_t i = 0; i < CW_ARRAY_LEN(rts); i++) fx_rt_free(rts[i]);
  memset(&G, 0, sizeof G);
}
