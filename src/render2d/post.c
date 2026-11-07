/* render2d HDR canvas and post chain (cw_r2d_post): bloom, exposure, the
 * tone curve, the grade, the vignette and the dither, as fx passes (fx.h),
 * the same on GL and WebGPU. */
#include <math.h>
#include <string.h>

#include "fx.h"
#include "cartwheel/quality.h"

#define MAX_LEVELS 8

static struct {
  fx_rt hdr;
  fx_rt dist;      /* distortion layers' offsets (canvas pixels, RG) */
  bool dist_used;  /* this pass drew into it */
  fx_rt mips[MAX_LEVELS + 1]; /* mips[1] = half size ... */
  uint32_t frame;
} P;

/* ── shaders ───────────────────────────────────────────────────────────── */

/* 13-tap downsample (Jimenez 2014); the first level weighs each group by
 * 1 / (1 + luma) (no fireflies from single hot pixels) and keeps what passes
 * the soft-knee threshold. u_v[0] = texel size of the source, first, 0;
 * u_v[1] = threshold, knee */
static const char *GL_DOWN =
    "vec3 tp(vec2 o) { return texture(t0, v_uv + o * u_v[0].xy).rgb; }\n"
    "float kw(vec3 c) { return 1.0 / (1.0 + max(max(c.r, c.g), c.b)); }\n"
    "void main() {\n"
    "  vec3 a = tp(vec2(-2.0, 2.0)), b = tp(vec2(0.0, 2.0)), c = tp(vec2(2.0, 2.0));\n"
    "  vec3 d = tp(vec2(-2.0, 0.0)), e = tp(vec2(0.0)), f = tp(vec2(2.0, 0.0));\n"
    "  vec3 g = tp(vec2(-2.0, -2.0)), h = tp(vec2(0.0, -2.0)), i = tp(vec2(2.0, -2.0));\n"
    "  vec3 j = tp(vec2(-1.0, 1.0)), k = tp(vec2(1.0, 1.0)), l = tp(vec2(-1.0, -1.0)), m = tp(vec2(1.0, -1.0));\n"
    "  vec3 r;\n"
    "  if (u_v[0].z > 0.5) {\n"
    "    vec3 g0 = (j + k + l + m) * 0.25, g1 = (a + b + d + e) * 0.25, g2 = (b + c + e + f) * 0.25;\n"
    "    vec3 g3 = (d + e + g + h) * 0.25, g4 = (e + f + h + i) * 0.25;\n"
    "    float w0 = 0.5 * kw(g0), w1 = 0.125 * kw(g1), w2 = 0.125 * kw(g2), w3 = 0.125 * kw(g3), w4 = 0.125 * kw(g4);\n"
    "    r = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);\n"
    "    float br = max(max(r.r, r.g), r.b), t = u_v[1].x, kn = t * u_v[1].y + 1e-5;\n"
    "    float q = clamp(br - t + kn, 0.0, 2.0 * kn);\n"
    "    q = q * q / (4.0 * kn);\n"
    "    r *= max(q, br - t) / max(br, 1e-5);\n"
    "  } else {\n"
    "    r = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;\n"
    "  }\n"
    "  o_col = vec4(max(r, vec3(0.0)), 1.0);\n"
    "}\n";
static const char *WG_DOWN =
    "fn tp(uv: vec2f, o: vec2f) -> vec3f { return textureSampleLevel(t0, s_lin, uv + o * fx.v[0].xy, 0.0).rgb; }\n"
    "fn kw(c: vec3f) -> f32 { return 1.0 / (1.0 + max(max(c.r, c.g), c.b)); }\n"
    "@fragment fn fs(i0: VO) -> @location(0) vec4f {\n"
    "  let u = i0.uv;\n"
    "  let a = tp(u, vec2f(-2.0, 2.0)); let b = tp(u, vec2f(0.0, 2.0)); let c = tp(u, vec2f(2.0, 2.0));\n"
    "  let d = tp(u, vec2f(-2.0, 0.0)); let e = tp(u, vec2f(0.0)); let f = tp(u, vec2f(2.0, 0.0));\n"
    "  let g = tp(u, vec2f(-2.0, -2.0)); let h = tp(u, vec2f(0.0, -2.0)); let i = tp(u, vec2f(2.0, -2.0));\n"
    "  let j = tp(u, vec2f(-1.0, 1.0)); let k = tp(u, vec2f(1.0, 1.0)); let l = tp(u, vec2f(-1.0, -1.0)); let m = tp(u, vec2f(1.0, -1.0));\n"
    "  var r: vec3f;\n"
    "  if (fx.v[0].z > 0.5) {\n"
    "    let g0 = (j + k + l + m) * 0.25; let g1 = (a + b + d + e) * 0.25; let g2 = (b + c + e + f) * 0.25;\n"
    "    let g3 = (d + e + g + h) * 0.25; let g4 = (e + f + h + i) * 0.25;\n"
    "    let w0 = 0.5 * kw(g0); let w1 = 0.125 * kw(g1); let w2 = 0.125 * kw(g2); let w3 = 0.125 * kw(g3); let w4 = 0.125 * kw(g4);\n"
    "    r = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);\n"
    "    let br = max(max(r.r, r.g), r.b); let t = fx.v[1].x; let kn = t * fx.v[1].y + 1e-5;\n"
    "    var q = clamp(br - t + kn, 0.0, 2.0 * kn);\n"
    "    q = q * q / (4.0 * kn);\n"
    "    r = r * (max(q, br - t) / max(br, 1e-5));\n"
    "  } else {\n"
    "    r = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;\n"
    "  }\n"
    "  return vec4f(max(r, vec3f(0.0)), 1.0);\n"
    "}\n";

/* tent upsample of the smaller level, added onto the larger one.
 * u_v[0] = texel size of the source, radius */
static const char *GL_UP =
    "vec3 tp(vec2 o) { return texture(t0, v_uv + o * u_v[0].xy * u_v[0].z).rgb; }\n"
    "void main() {\n"
    "  vec3 s = tp(vec2(-1.0, 1.0)) + 2.0 * tp(vec2(0.0, 1.0)) + tp(vec2(1.0, 1.0))\n"
    "         + 2.0 * tp(vec2(-1.0, 0.0)) + 4.0 * tp(vec2(0.0)) + 2.0 * tp(vec2(1.0, 0.0))\n"
    "         + tp(vec2(-1.0, -1.0)) + 2.0 * tp(vec2(0.0, -1.0)) + tp(vec2(1.0, -1.0));\n"
    "  o_col = vec4(s / 16.0, 0.0);\n"
    "}\n";
static const char *WG_UP =
    "fn tp(uv: vec2f, o: vec2f) -> vec3f { return textureSampleLevel(t0, s_lin, uv + o * fx.v[0].xy * fx.v[0].z, 0.0).rgb; }\n"
    "@fragment fn fs(i0: VO) -> @location(0) vec4f {\n"
    "  let u = i0.uv;\n"
    "  let s = tp(u, vec2f(-1.0, 1.0)) + 2.0 * tp(u, vec2f(0.0, 1.0)) + tp(u, vec2f(1.0, 1.0))\n"
    "        + 2.0 * tp(u, vec2f(-1.0, 0.0)) + 4.0 * tp(u, vec2f(0.0)) + 2.0 * tp(u, vec2f(1.0, 0.0))\n"
    "        + tp(u, vec2f(-1.0, -1.0)) + 2.0 * tp(u, vec2f(0.0, -1.0)) + tp(u, vec2f(1.0, -1.0));\n"
    "  return vec4f(s / 16.0, 0.0);\n"
    "}\n";

/* the final pass: t0 = HDR canvas, t1 = bloom. u_v[0] = exposure, bloom
 * intensity (0 = none), tone, knee; [1] = contrast, saturation, 1/gamma,
 * vignette; [2] = lift, vignette softness; [3] = gain, dither; [4] = tint,
 * aspect */
#define GL_TONE_FNS                                                                                           \
  "vec3 tone(vec3 x, float kind, float k) {\n"                                                                \
  "  if (kind < 0.5) {\n"                                                                                     \
  "    vec3 s = k + (1.0 - k) * (1.0 - exp(-(x - k) / max(1.0 - k, 1e-4)));\n"                              \
  "    return mix(x, s, step(vec3(k), x));\n"                                                                 \
  "  }\n"                                                                                                     \
  "  if (kind < 1.5) return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);\n"    \
  "  if (kind < 2.5) return x / (1.0 + x);\n"                                                                 \
  "  return min(x, vec3(1.0));\n"                                                                             \
  "}\n"
static const char *GL_FINAL =
    GL_TONE_FNS
    "void main() {\n"
    "  vec2 uv = v_uv;\n"
    "  if (u_v[5].x > 0.5) { vec2 o = texture(t2, v_uv).rg; uv += vec2(o.x, -o.y) / u_size; }\n"
    "  vec4 c = texture(t0, uv);\n"
    "  vec3 x = c.a > 1e-5 ? c.rgb / c.a : vec3(0.0);\n"
    "  x *= u_v[0].x;\n"
    "  if (u_v[0].y > 0.0) x += texture(t1, v_uv).rgb * u_v[0].y / max(c.a, 1e-5) * min(c.a * 64.0, 1.0);\n"
    "  x = tone(max(x, vec3(0.0)), u_v[0].z, u_v[0].w);\n"
    "  x = x + u_v[2].rgb * (1.0 - x);\n"
    "  x = x * u_v[3].rgb;\n"
    "  x = pow(max(x, vec3(0.0)), vec3(u_v[1].z));\n"
    "  x = (x - 0.5) * u_v[1].x + 0.5;\n"
    "  float l = dot(x, vec3(0.2126, 0.7152, 0.0722));\n"
    "  x = mix(vec3(l), x, u_v[1].y) * u_v[4].rgb;\n"
    "  vec2 d = (v_uv - 0.5) * vec2(u_v[4].w, 1.0) * 1.414;\n"
    "  float vg = smoothstep(1.0 - u_v[2].w, 1.0 + u_v[2].w * 0.25, length(d) * 0.8);\n"
    "  x *= 1.0 - u_v[1].w * vg;\n"
    "  vec2 p = floor(v_uv * u_size);\n"
    "  float n = fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));\n"
    "  x = clamp(x + (n - 0.5) * u_v[3].w / 255.0, 0.0, 1.0);\n"
    "  o_col = vec4(x * c.a, c.a);\n"
    "}\n";
static const char *WG_FINAL =
    "fn tone(x: vec3f, kind: f32, k: f32) -> vec3f {\n"
    "  if (kind < 0.5) {\n"
    "    let s = k + (1.0 - k) * (1.0 - exp(-(x - k) / max(1.0 - k, 1e-4)));\n"
    "    return mix(x, s, step(vec3f(k), x));\n"
    "  }\n"
    "  if (kind < 1.5) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), vec3f(0.0), vec3f(1.0)); }\n"
    "  if (kind < 2.5) { return x / (1.0 + x); }\n"
    "  return min(x, vec3f(1.0));\n"
    "}\n"
    "@fragment fn fs(i0: VO) -> @location(0) vec4f {\n"
    "  var uv = i0.uv;\n"
    "  if (fx.v[5].x > 0.5) { let o = textureSampleLevel(t2, s_lin, i0.uv, 0.0).rg; uv += vec2f(o.x, -o.y) / fx.size; }\n"
    "  let c = textureSampleLevel(t0, s_lin, uv, 0.0);\n"
    "  var x = select(vec3f(0.0), c.rgb / max(c.a, 1e-5), c.a > 1e-5);\n"
    "  x = x * fx.v[0].x;\n"
    "  if (fx.v[0].y > 0.0) { x = x + textureSampleLevel(t1, s_lin, uv, 0.0).rgb * fx.v[0].y / max(c.a, 1e-5) * min(c.a * 64.0, 1.0); }\n"
    "  x = tone(max(x, vec3f(0.0)), fx.v[0].z, fx.v[0].w);\n"
    "  x = x + fx.v[2].rgb * (1.0 - x);\n"
    "  x = x * fx.v[3].rgb;\n"
    "  x = pow(max(x, vec3f(0.0)), vec3f(fx.v[1].z));\n"
    "  x = (x - 0.5) * fx.v[1].x + 0.5;\n"
    "  let l = dot(x, vec3f(0.2126, 0.7152, 0.0722));\n"
    "  x = mix(vec3f(l), x, fx.v[1].y) * fx.v[4].rgb;\n"
    "  let d = (uv - 0.5) * vec2f(fx.v[4].w, 1.0) * 1.414;\n"
    "  let vg = smoothstep(1.0 - fx.v[2].w, 1.0 + fx.v[2].w * 0.25, length(d) * 0.8);\n"
    "  x = x * (1.0 - fx.v[1].w * vg);\n"
    "  let p = floor(uv * fx.size);\n"
    "  let n = fract(52.9829189 * fract(dot(p, vec2f(0.06711056, 0.00583715))));\n"
    "  x = clamp(x + (n - 0.5) * fx.v[3].w / 255.0, vec3f(0.0), vec3f(1.0));\n"
    "  return vec4f(x * c.a, c.a);\n"
    "}\n";

/* the offscreen canvas onto the canvas as drawn (no chain): exact texels, or
 * bent by the distortion layers (t2) when u_v[0].x is 1 */
static const char *GL_COPY =
    "void main() {\n"
    "  if (u_v[0].x < 0.5) { o_col = texelFetch(t0, ivec2(v_uv * u_size), 0); return; }\n"
    "  vec2 o = texture(t2, v_uv).rg;\n"
    "  o_col = texture(t0, v_uv + vec2(o.x, -o.y) / u_size);\n"
    "}\n";
static const char *WG_COPY =
    "@fragment fn fs(i: VO) -> @location(0) vec4f {\n"
    "  if (fx.v[0].x < 0.5) { return textureLoad(t0, vec2i(i.uv * fx.size), 0); }\n"
    "  let o = textureSampleLevel(t2, s_lin, i.uv, 0.0).rg;\n"
    "  return textureSampleLevel(t0, s_lin, i.uv + vec2f(o.x, -o.y) / fx.size, 0.0);\n"
    "}\n";
static fx_prog prog_copy = { "r2d canvas copy", NULL, NULL, 0, 0, 0, NULL, false };

static fx_prog prog_down = { "r2d bloom down", NULL, NULL, 0, 0, 0, NULL, false };
static fx_prog prog_up = { "r2d bloom up", NULL, NULL, 0, 0, 0, NULL, false };
static fx_prog prog_final = { "r2d post", NULL, NULL, 0, 0, 0, NULL, false };

static void progs_init(void) {
  if (prog_down.glsl) return;
  prog_down.glsl = GL_DOWN, prog_down.wgsl = WG_DOWN;
  prog_up.glsl = GL_UP, prog_up.wgsl = WG_UP;
  prog_final.glsl = GL_FINAL, prog_final.wgsl = WG_FINAL;
  prog_copy.glsl = GL_COPY, prog_copy.wgsl = WG_COPY;
}

/* ── the chain ─────────────────────────────────────────────────────────── */

bool r2d_post_active(void) { return R.has_post || R.offscreen; }
int r2d_post_hud_layer(void) { return R.has_post && R.post.hud_layer > 0 ? R.post.hud_layer : CW_R2D_MAX_LAYERS; }

/* the offscreen canvas: HDR with a chain, else the canvas's own 8 bits */
static fx_fmt canvas_format(void) { return R.has_post ? fx_hdr_format() : FX_RGBA8; }
bool r2d_canvas_hdr(void) { return canvas_format() == FX_RGBA16F; }
void r2d_post_ensure(void) {
  fx_rt_ensure(&P.hdr, R.canvas_w, R.canvas_h, canvas_format());
  P.dist_used = false;
}
bool r2d_dist_ok(void) { return fx_hdr_format() == FX_RGBA16F; }
/* the distortion buffer for a distortion layer, cleared on its first use in the pass */
static void dist_ensure(bool *first) {
  fx_rt_ensure(&P.dist, R.canvas_w, R.canvas_h, FX_RGBA16F);
  *first = !P.dist_used;
  P.dist_used = true;
}
GLuint r2d_post_dist_begin_gl(void) {
  bool first;
  dist_ensure(&first);
  glBindFramebuffer(GL_FRAMEBUFFER, P.dist.fbo);
  glViewport(0, 0, P.dist.w, P.dist.h);
  if (first) {
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  return P.dist.fbo;
}
void *r2d_post_dist_wtex(bool *first) {
  dist_ensure(first);
  return P.dist.wtex;
}
GLuint r2d_post_fbo(void) { return P.hdr.fbo; }
void *r2d_post_wtex(void) { return P.hdr.wtex; }
int r2d_post_wfmt(void) { return r2d_wg_fx_format(P.hdr.fmt); }

void r2d_post_begin_gl(void) {
  r2d_post_ensure();
  glBindFramebuffer(GL_FRAMEBUFFER, P.hdr.fbo);
  glViewport(0, 0, P.hdr.w, P.hdr.h);
  glDisable(GL_SCISSOR_TEST);
  cw_color c = R.pass.clear ? R.pass.clear_color : (cw_color){ 0, 0, 0, 0 };
  glClearColor(c.r * c.a, c.g * c.a, c.b * c.a, c.a);
  glClear(GL_COLOR_BUFFER_BIT);
}

static float def(float v, float d) { return v != 0 ? v : d; }

/* bloom levels the effects detail allows */
static int levels_for_quality(int asked) {
  int e = cw_gfx_effective(CW_GFX_EFFECTS);
  if (e <= CW_QUALITY_VERY_LOW) return 0;
  if (e == CW_QUALITY_LOW) return CW_MIN(asked, 3);
  if (e == CW_QUALITY_MEDIUM) return CW_MIN(asked, 5);
  return asked;
}

void r2d_post_run(const fx_dst *out) {
  progs_init();
  if (!R.has_post) { /* screen reads or distortion only: the canvas as drawn */
    float v[8][4] = { { P.dist_used ? 1.0f : 0.0f, 0, 0, 0 } };
    const fx_rt *in[4] = { &P.hdr, NULL, P.dist_used ? &P.dist : NULL, NULL };
    fx_pass(&prog_copy, out, false, NULL, in, v, R.pass.clear ? FX_BLEND_NONE : FX_BLEND_PREMUL);
    return;
  }
  const cw_r2d_post *s = &R.post;
  int levels = s->bloom ? levels_for_quality(CW_CLAMP(s->bloom_levels > 0 ? s->bloom_levels : 6, 1, MAX_LEVELS)) : 0;
  fx_fmt hf = P.hdr.fmt;
  /* bloom: down to `levels` halvings, then back up, each level added onto the next larger */
  int w = P.hdr.w, h = P.hdr.h, made = 0;
  for (int l = 1; l <= levels; l++) {
    w = CW_MAX(1, w / 2);
    h = CW_MAX(1, h / 2);
    if (w < 2 || h < 2) break;
    fx_rt_ensure(&P.mips[l], w, h, hf);
    const fx_rt *src = l == 1 ? &P.hdr : &P.mips[l - 1];
    float v[8][4] = { { 1.0f / (float)src->w, 1.0f / (float)src->h, l == 1 ? 1.0f : 0.0f, 0 },
                      { def(s->bloom_threshold, 1), def(s->bloom_knee, 0.5f), 0, 0 } };
    fx_dst d = fx_dst_of(&P.mips[l]);
    const fx_rt *in[4] = { src, NULL, NULL, NULL };
    fx_pass(&prog_down, &d, false, NULL, in, v, FX_BLEND_NONE);
    made = l;
  }
  for (int l = made; l >= 2; l--) {
    float v[8][4] = { { 1.0f / (float)P.mips[l].w, 1.0f / (float)P.mips[l].h, def(s->bloom_radius, 1), 0 } };
    fx_dst d = fx_dst_of(&P.mips[l - 1]);
    const fx_rt *in[4] = { &P.mips[l], NULL, NULL, NULL };
    fx_pass(&prog_up, &d, false, NULL, in, v, FX_BLEND_ADD);
  }
  float g = def(s->gamma, 1), sat = s->saturation < 0 ? 0 : def(s->saturation, 1);
  cw_color gain = (s->gain.r == 0 && s->gain.g == 0 && s->gain.b == 0) ? CW_WHITE : s->gain;
  cw_color tint = (s->tint.r == 0 && s->tint.g == 0 && s->tint.b == 0) ? CW_WHITE : s->tint;
  float bi = made ? def(s->bloom_intensity, 0.6f) / (float)made : 0;
  float v[8][4] = {
    { def(s->exposure, 1), bi, (float)s->tone, def(s->tone_knee, 0.75f) },
    { def(s->contrast, 1), sat, 1.0f / g, CW_CLAMP(s->vignette, 0, 1) },
    { s->lift.r, s->lift.g, s->lift.b, def(s->vignette_softness, 0.5f) },
    { gain.r, gain.g, gain.b, s->no_dither ? 0.0f : 1.0f },
    { tint.r, tint.g, tint.b, (float)P.hdr.w / (float)CW_MAX(1, P.hdr.h) },
    { P.dist_used ? 1.0f : 0.0f, 0, 0, 0 },
  };
  const fx_rt *in[4] = { &P.hdr, made ? &P.mips[1] : NULL, P.dist_used ? &P.dist : NULL, NULL };
  fx_pass(&prog_final, out, false, NULL, in, v, FX_BLEND_PREMUL);
  P.frame++;
}

/* the screen texture: the offscreen canvas as it stands, copied (screen_read layers) */
cw_texture cw_r2d_screen_texture(void) {
  if (!r2d_lazy_init()) return (cw_texture){ 0 };
  bool hdr = r2d_canvas_hdr();
  r2d_tex *t = CW_HANDLE_IS_NULL(R.screen_tex) ? NULL : r2d_tex_get(R.screen_tex);
  if (t && (t->w != R.canvas_w || t->h != R.canvas_h || t->hdr != hdr)) {
    cw_texture_destroy(R.screen_tex);
    t = NULL;
  }
  if (!t) R.screen_tex = hdr ? r2d_texture_new_hdr(R.canvas_w, R.canvas_h) : r2d_texture_new_internal(R.canvas_w, R.canvas_h, CW_FILTER_LINEAR, false);
  return R.screen_tex;
}
void r2d_screen_copy_gl(GLuint from) {
  r2d_tex *t = r2d_tex_get(cw_r2d_screen_texture());
  if (!t) return;
  glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, from);
  glBindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, t->fbo);
  glDisable(GL_SCISSOR_TEST);
  glBlitFramebuffer(0, 0, R.canvas_w, R.canvas_h, 0, 0, R.canvas_w, R.canvas_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, from);
}
void *r2d_screen_wtex(void) {
  r2d_tex *t = r2d_tex_get(cw_r2d_screen_texture());
  return t ? R.arrays.data[t->array].wtex : NULL;
}

void r2d_post_shutdown(void) {
  fx_rt_free(&P.hdr);
  fx_rt_free(&P.dist);
  for (int i = 0; i <= MAX_LEVELS; i++) fx_rt_free(&P.mips[i]);
}

/* the scene runner lays its canvases out after cw_r2d_begin: the first
 * screen-space CanvasLayer becomes the chain's hud layer then */
void cw__r2d_set_post_hud(int layer) {
  if (R.has_post) R.post.hud_layer = layer;
}
