/* The built-in passes of the render pipeline (pipeline.h): depth prepass,
 * scene, velocity, GTAO, TAA, bloom, tonemap, LUT grade, FXAA / SMAA, lens
 * effects, and the app's overlay. Every shader is GLSL ES 3.00 with no
 * derivatives (hosts disagree on them) and integer noise (bit-identical on
 * every GPU). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cartwheel/platform.h"
#include "cartwheel/scene2d.h"
#include "pipeline_internal.h"
#include "scene/render3d_internal.h"
#if CW_WGPU
#include "pipeline_wgpu.h"
#endif

#define S(...) #__VA_ARGS__ "\n"
#include "pipeline_post_wgsl.h"

/* a program for this run's backend, from pieces (GLSL or WGSL) */
static unsigned prog_of(const char *name, const char *g1, const char *g2, const char *g3, const char *w1, const char *w2,
                        const char *w3) {
  static char buf[32768];
  bool wg = cw_gpu_backend() == CW_GPU_WEBGPU;
  snprintf(buf, sizeof buf, "%s%s%s", wg ? (w1 ? w1 : "") : (g1 ? g1 : ""), wg ? (w2 ? w2 : "") : (g2 ? g2 : ""),
           wg ? (w3 ? w3 : "") : (g3 ? g3 : ""));
  return wg ? cw_pass_program_wgsl(name, buf) : cw_pass_program(name, buf);
}

/* WebGPU: the projection's z rows, which turn the linear depth back into
 * GL's window depth (a no-op uniform on GL) */
static void set_pz(unsigned prog, const cw_pass_ctx *ctx) {
  const float *e = ctx->camera->projection_matrix.e;
  cw_pass_uniform4f(prog, "u_pz", e[10], e[14], e[11], e[15]);
}

/* ── shared GLSL ────────────────────────────────────────────────────── */

static const char *GLSL_COMMON =
    "#define PI 3.14159265358979\n"
    "#define HALF_PI 1.5707963267949\n"
    S(float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
      uint hash_u(uvec3 v) {
        v = v * 1664525u + 1013904223u;
        v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
        v ^= v >> 16u;
        v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
        return v.x;
      }
      float hash01(uvec3 v) { return float(hash_u(v) >> 8u) / 16777216.0; });

/* ── depth prepass ──────────────────────────────────────────────────── */

static bool prepass_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  return ctx->settings->depth_prepass;
}

static void prepass_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  t3_renderer *r = ctx->renderer;
  t3_renderer_reset_bindings(r);
  t3_renderer_set_framebuffer(r, ctx->pipeline->bound_fbo);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  t3_renderer_render(r, ctx->scene3d, ctx->camera);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  cw__render3d_count(ctx->tree);
}

/* ── scene ──────────────────────────────────────────────────────────── */

static void scene_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  t3_renderer *r = ctx->renderer;
#if CW_WGPU
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    /* three.c's WebGPU renderer (three.js r186's generated WGSL) records its
     * passes (shadow maps first, then the scene) on the frame encoder, into
     * the scene target; cw__pw_flush submits them with the rest of the frame
     * and three.c's frame closes after it (cw__pipeline_wgpu_submitted) */
    const cw__wgpu_pass_targets *pt = cw__pass_wgpu_targets(ctx);
    if (!pt || !pt->color) return;
    /* the scene target's colour (resolving into the scene colour with MSAA)
     * and Depth32Float depth, cleared here; flip_y keeps cartwheel's offscreen
     * row order (bottom-up, as on GL), so every pass reads it as it does there */
    float a = 1;
    t3_color c = t3_renderer_get_clear_color(r, &a);
    t3_external_target et = {
      .color = (uintptr_t)pt->color, .color_resolve = (uintptr_t)pt->resolve, .depth = (uintptr_t)pt->depth,
      .color_format = (int)pt->color_format, .depth_format = (int)pt->depth_format, .samples = pt->samples,
      .load_color = false, .load_depth = false, .clear_rgba = { c.r, c.g, c.b, a }, .clear_depth = 1,
      .x = 0, .y = 0, .w = pt->w, .h = pt->h, .flip_y = pt->flip_y,
    };
    t3_renderer_set_external_encoder(r, (uintptr_t)pt->enc);
    t3_renderer_set_external_target(r, &et);
    cw__pw_timing_span((void *)pt->enc, false); /* three.c's passes: timed as one span */
    t3_renderer_render(r, ctx->scene3d, ctx->camera);
    cw__pw_timing_span((void *)pt->enc, true);
    t3_renderer_set_external_target(r, NULL);
    cw__render3d_count(ctx->tree);
    ctx->pipeline->drew3d = true;
    return;
  }
#endif
  t3_renderer_reset_bindings(r);
  t3_renderer_set_framebuffer(r, ctx->pipeline->bound_fbo);
  if (ctx->pipeline->prepass_ran) {
    /* keep the prepass depth: clear colour only, to what three.c would */
    bool auto_clear = t3_renderer_get_auto_clear(r);
    float a = 1;
    t3_color c = t3_renderer_get_clear_color(r, &a);
    if (ctx->scene3d->has_background && !ctx->scene3d->background_texture) c = ctx->scene3d->background, a = 1;
    glClearColor(c.r, c.g, c.b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    t3_renderer_set_auto_clear(r, false);
    t3_renderer_reset_bindings(r);
    t3_renderer_render(r, ctx->scene3d, ctx->camera);
    t3_renderer_set_auto_clear(r, auto_clear);
  } else {
    t3_renderer_render(r, ctx->scene3d, ctx->camera);
  }
  cw__render3d_count(ctx->tree);
  ctx->pipeline->drew3d = true;
}

/* ── velocity (camera motion from depth) ────────────────────────────── */

static unsigned vel_prog;
static const char *VEL_FS = S(
  uniform sampler2D u_depth;
  uniform mat4 u_inv_vp_j;   /* this frame, jittered (what the depth was drawn with) */
  uniform mat4 u_vp;         /* this frame, unjittered */
  uniform mat4 u_prev_vp;    /* last frame, unjittered */
  out vec4 o;
  void main() {
    float d = texture(u_depth, v_uv).r;
    vec4 w = u_inv_vp_j * vec4(v_uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    w /= w.w;
    vec4 c = u_vp * w, p = u_prev_vp * w;
    o = vec4((c.xy / c.w - p.xy / p.w) * 0.5, 0.0, 1.0);
  });

static void velocity_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  if (!vel_prog) vel_prog = prog_of("velocity", VEL_FS, NULL, NULL, WGSL_COMMON, WGSL_VEL, NULL);
  const cw_rt *v = cw_pipeline_rt_persistent(ctx->pipeline, "velocity", ctx->width, ctx->height, CW_RT_RG16F, NULL);
  if (!v || !vel_prog) return;
  cw_pass_bind_target(ctx, v);
  cw_pass_use(vel_prog);
  set_pz(vel_prog, ctx);
  cw_pass_bind_texture(0, ctx->depth_tex);
  cw_pass_uniform1i(vel_prog, "u_depth", 0);
  cw_pass_uniform_mat4(vel_prog, "u_inv_vp_j", &ctx->pipeline->inv_view_proj_jittered);
  cw_pass_uniform_mat4(vel_prog, "u_vp", &ctx->view_proj);
  cw_pass_uniform_mat4(vel_prog, "u_prev_vp", &ctx->prev_view_proj);
  cw_pass_fullscreen();
  ctx->velocity = v;
}

/* ── GTAO (ground-truth ambient occlusion, Jimenez et al. 2016) ────────
 * Half resolution: per pixel, `slices` directions (rotated by a 4x4
 * pattern) each march `steps` samples per side for the horizon angles, and
 * the cosine-weighted visibility between them is integrated in closed form
 * against the projected normal. Output: (visibility, view depth). The
 * composite does a 4x4 depth-aware filter (one period of the pattern) while
 * upsampling and multiplies the scene colour. */
static unsigned ao_prog, ao_comp_prog, ao_fog_prog;
static unsigned ao_fog_program(void);
static const char *AO_FS = S(
  uniform sampler2D u_depth;
  uniform mat4 u_inv_proj;
  uniform vec4 u_proj_scale;  /* proj[0][0], proj[1][1], full w, full h */
  uniform vec4 u_params;      /* radius, power, slices, steps */
  uniform int u_frame;
  out vec4 o;
  /* positions are rebuilt at the centre of the depth texel they read (a
   * half-res pixel centre sits between full-res texels) */
  vec3 view_pos_px(ivec2 q) {
    q = clamp(q, ivec2(0), ivec2(u_proj_scale.zw) - 1);
    float d = texelFetch(u_depth, q, 0).r;
    vec2 uv = (vec2(q) + 0.5) / u_proj_scale.zw;
    vec4 p = u_inv_proj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
  }
  vec3 view_pos(vec2 uv) { return view_pos_px(ivec2(floor(uv * u_proj_scale.zw))); }
  void main() {
    ivec2 q = ivec2(gl_FragCoord.xy) * 2;
    vec2 uv = (vec2(q) + 0.5) / u_proj_scale.zw;
    float d = texelFetch(u_depth, q, 0).r;
    if (d >= 1.0) { o = vec4(1.0, 1e6, 0.0, 1.0); return; }
    vec3 P = view_pos_px(q);
    vec3 pr = view_pos_px(q + ivec2(1, 0)), pl = view_pos_px(q - ivec2(1, 0));
    vec3 pu = view_pos_px(q + ivec2(0, 1)), pd = view_pos_px(q - ivec2(0, 1));
    vec3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
    vec3 dy = abs(pu.z - P.z) < abs(P.z - pd.z) ? pu - P : P - pd;
    vec3 N = normalize(cross(dx, dy));
    vec3 V = normalize(-P);
    if (dot(N, V) < 0.0) N = -N;
    float R = u_params.x;
    vec2 ruv = vec2(u_proj_scale.x, u_proj_scale.y) * 0.5 * R / max(-P.z, 1e-4);
    int slices = int(u_params.z), steps = int(u_params.w);
    /* 4x4 pattern of slice rotations and step offsets (exact on every GPU) */
    ivec2 ip = ivec2(gl_FragCoord.xy) & 3;
    int idx = ip.x + ip.y * 4;
    int perm[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
    float rot = (float(perm[idx]) + 0.5) / 16.0;
    float jit = float((idx * 7 + u_frame * 5) & 15) / 16.0;
    float vis = 0.0;
    for (int s = 0; s < 8; s++) {
      if (s >= slices) break;
      float phi = (float(s) + rot) / float(slices) * PI;
      vec2 dir = vec2(cos(phi), sin(phi));
      vec3 d3 = vec3(dir, 0.0);
      vec3 ortho = d3 - dot(d3, V) * V;
      vec3 axis = normalize(cross(d3, V));
      vec3 pn = N - axis * dot(N, axis);
      float pl2 = max(length(pn), 1e-4);
      float cosn = clamp(dot(pn, V) / pl2, -1.0, 1.0);
      float n = sign(dot(ortho, pn)) * acos(cosn);
      float hc[2];
      for (int side = 0; side < 2; side++) {
        float sgn = side == 0 ? -1.0 : 1.0;
        float h = -1.0;
        for (int k = 0; k < 16; k++) {
          if (k >= steps) break;
          float t = (float(k) + 1.0 - jit) / float(steps);
          t = t * t * 0.9 + 0.1 * t;
          /* at least 1.5 pixels out: a sample on the pixel itself reads as a
           * horizon at 90 degrees and darkens every tilted surface */
          vec2 off = dir * ruv * t;
          float opx = length(off * u_proj_scale.zw);
          if (opx < 1.5) off *= 1.5 / max(opx, 1e-4);
          vec2 suv = uv + sgn * off;
          /* off screen: no information, not an occluder */
          if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) break;
          vec3 H = view_pos(suv) - P;
          float len = length(H);
          float c = dot(H, V) / max(len, 1e-5);
          float fall = clamp(1.0 - (len * len) / (R * R), 0.0, 1.0);
          h = max(h, mix(-1.0, c, fall));
        }
        hc[side] = h;
      }
      float h0 = n + max(-acos(hc[0]) - n, -HALF_PI);
      float h1 = n + min(acos(hc[1]) - n, HALF_PI);
      float sn = sin(n), cn = cos(n);
      float a = 0.25 * (-cos(2.0 * h0 - n) + cn + 2.0 * h0 * sn) + 0.25 * (-cos(2.0 * h1 - n) + cn + 2.0 * h1 * sn);
      vis += pl2 * a;
    }
    vis = clamp(vis / float(slices), 0.0, 1.0);
    o = vec4(pow(vis, u_params.y), -P.z, 0.0, 1.0);
  });

static const char *AO_COMP_FS = S(
  uniform sampler2D u_src;
  uniform sampler2D u_ao;
  uniform sampler2D u_depth;
  uniform mat4 u_inv_proj;
  uniform vec2 u_ao_size;
  uniform float u_intensity;
  out vec4 o;
  void main() {
    float d = texture(u_depth, v_uv).r;
    vec4 c = texture(u_src, v_uv);
    AO_FUSED_IN
    if (d >= 1.0) { o = c; return; }
    vec4 p = u_inv_proj * vec4(v_uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    float z = -p.z / p.w;
    vec2 base = v_uv * u_ao_size - 0.5;
    vec2 f = floor(base);
    float sum = 0.0, wsum = 0.0;
    for (int j = -1; j <= 2; j++)
      for (int i = -1; i <= 2; i++) {
        vec2 t = f + vec2(float(i), float(j));
        vec2 a = texture(u_ao, (t + 0.5) / u_ao_size).rg;
        vec2 dd = base - t;
        float ws = exp(-dot(dd, dd) * 0.35);
        float rz = abs(a.y - z) / (0.06 * max(z, 1e-3));
        float wz = exp(-rz * rz);
        float w = ws * wz;
        sum += a.x * w;
        wsum += w;
      }
    float ao = wsum > 1e-4 ? sum / wsum : 1.0;
    o = vec4(c.rgb * mix(1.0, ao, u_intensity), c.a);
  });

static bool ssao_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  return ctx->settings->ssao;
}

static void ssao_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_post_settings *s = ctx->settings;
  if (!ao_prog) {
    ao_prog = prog_of("ssao", GLSL_COMMON, AO_FS, NULL, WGSL_COMMON, WGSL_AO, NULL);
    ao_comp_prog = prog_of("ssao.composite", "#define AO_FUSED_IN\n", AO_COMP_FS, NULL, WGSL_COMMON, WGSL_AO_COMP_HEAD,
                           WGSL_AO_COMP_BODY);
  }
  bool fog = ctx->pipeline->fuse_fog;
  if (fog && !ao_fog_prog) ao_fog_prog = ao_fog_program();
  unsigned comp = fog ? ao_fog_prog : ao_comp_prog;
  if (!ao_prog || !comp) return;
  int hw = (ctx->width + 1) / 2, hh = (ctx->height + 1) / 2;
  const cw_rt *ao = cw_pipeline_rt_persistent(ctx->pipeline, "ssao.ao", hw, hh, CW_RT_RG16F, NULL);
  if (!ao) return;
  const t3_mat4 *ip = &ctx->pipeline->inv_proj_jittered;
  cw_pass_bind_target(ctx, ao);
  cw_pass_use(ao_prog);
  set_pz(ao_prog, ctx);
  cw_pass_bind_texture(0, ctx->depth_tex);
  cw_pass_uniform1i(ao_prog, "u_depth", 0);
  cw_pass_uniform_mat4(ao_prog, "u_inv_proj", ip);
  cw_pass_uniform4f(ao_prog, "u_proj_scale", ctx->camera->projection_matrix.e[0], ctx->camera->projection_matrix.e[5],
                    (float)ctx->width, (float)ctx->height);
  cw_pass_uniform4f(ao_prog, "u_params", s->ssao_radius, s->ssao_power, (float)CW_CLAMP(s->ssao_slices, 1, 8),
                    (float)CW_CLAMP(s->ssao_steps, 1, 16));
  cw_pass_uniform1i(ao_prog, "u_frame", s->taa ? (int)(ctx->frame & 15) : 0);
  cw_pass_fullscreen();
  cw_pass_bind_target(ctx, ctx->dst);
  cw_pass_use(comp);
  set_pz(comp, ctx);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, ao->tex);
  cw_pass_bind_texture(2, ctx->depth_tex);
  cw_pass_uniform1i(comp, "u_src", 0);
  cw_pass_uniform1i(comp, "u_ao", 1);
  cw_pass_uniform1i(comp, "u_depth", 2);
  cw_pass_uniform_mat4(comp, "u_inv_proj", ip);
  cw_pass_uniform2f(comp, "u_ao_size", (float)hw, (float)hh);
  cw_pass_uniform1f(comp, "u_intensity", s->ssao_intensity);
  if (fog) cw__fog_fused_uniforms(ctx, comp); /* the fog pass's own uniforms (fog.c) */
  cw_pass_fullscreen();
}

/* ── TAA ────────────────────────────────────────────────────────────────
 * History reprojected by the velocity target, sampled with a 5-tap
 * Catmull-Rom, clipped toward the 3x3 neighbourhood's YCoCg variance box;
 * luma-weighted blend (stable HDR highlights). The history alternates
 * between two persistent targets and becomes the chain's image. */
static unsigned taa_prog;
static const char *TAA_FS = S(
  uniform sampler2D u_src;
  uniform sampler2D u_hist;
  uniform sampler2D u_vel;
  uniform vec2 u_size;
  uniform float u_feedback;
  uniform int u_fresh;
  out vec4 o;
  vec3 to_ycocg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
  vec3 from_ycocg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
  vec3 hist_catmull(vec2 uv) {
    vec2 pos = uv * u_size;
    vec2 c = floor(pos - 0.5) + 0.5;
    vec2 f = pos - c;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (c - 1.0) / u_size, t3 = (c + 2.0) / u_size, t12 = (c + w2 / w12) / u_size;
    vec3 r = texture(u_hist, vec2(t12.x, t0.y)).rgb * (w12.x * w0.y)
           + texture(u_hist, vec2(t0.x, t12.y)).rgb * (w0.x * w12.y)
           + texture(u_hist, vec2(t12.x, t12.y)).rgb * (w12.x * w12.y)
           + texture(u_hist, vec2(t3.x, t12.y)).rgb * (w3.x * w12.y)
           + texture(u_hist, vec2(t12.x, t3.y)).rgb * (w12.x * w3.y);
    float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / ws, vec3(0.0));
  }
  void main() {
    vec4 cur = texture(u_src, v_uv);
    if (u_fresh != 0) { o = cur; return; }
    vec2 px = 1.0 / u_size;
    vec3 m1 = vec3(0.0), m2 = vec3(0.0), mn = vec3(1e9), mx = vec3(-1e9);
    for (int j = -1; j <= 1; j++)
      for (int i = -1; i <= 1; i++) {
        vec3 c = to_ycocg(texture(u_src, v_uv + vec2(float(i), float(j)) * px).rgb);
        m1 += c; m2 += c * c; mn = min(mn, c); mx = max(mx, c);
      }
    m1 /= 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - m1 * m1, vec3(0.0)));
    vec3 bmin = max(mn, m1 - sigma * 1.25), bmax = min(mx, m1 + sigma * 1.25);
    vec2 prev = v_uv - texture(u_vel, v_uv).xy;
    vec3 c0 = to_ycocg(cur.rgb);
    vec3 h;
    if (any(lessThan(prev, vec2(0.0))) || any(greaterThan(prev, vec2(1.0)))) h = c0;
    else {
      h = to_ycocg(hist_catmull(prev));
      /* clip the history toward the box centre (not a per-axis clamp) */
      vec3 ctr = 0.5 * (bmax + bmin), ext = 0.5 * (bmax - bmin) + 1e-5;
      vec3 v = h - ctr;
      vec3 a = abs(v / ext);
      float m = max(a.x, max(a.y, a.z));
      if (m > 1.0) h = ctr + v / m;
    }
    float wc = (1.0 - u_feedback) / (1.0 + c0.x), wh = u_feedback / (1.0 + h.x);
    vec3 r = (c0 * wc + h * wh) / (wc + wh);
    o = vec4(from_ycocg(r), cur.a);
  });

static bool taa_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  return ctx->settings->taa;
}

static void taa_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  if (!taa_prog) taa_prog = prog_of("taa", TAA_FS, NULL, NULL, WGSL_TAA, NULL, NULL);
  if (!taa_prog || !ctx->velocity) return;
  /* ping-pong between two histories by frame parity */
  bool odd = (ctx->frame & 1) != 0;
  bool fresh_a = false, fresh_b = false;
  cw_rt_format f = ctx->src->format;
  const cw_rt *a = cw_pipeline_rt_persistent(ctx->pipeline, "taa.a", ctx->width, ctx->height, f, &fresh_a);
  const cw_rt *b = cw_pipeline_rt_persistent(ctx->pipeline, "taa.b", ctx->width, ctx->height, f, &fresh_b);
  if (!a || !b) return;
  const cw_rt *dst = odd ? a : b, *hist = odd ? b : a;
  bool fresh = fresh_a || fresh_b || ctx->pipeline->taa_reset;
  ctx->pipeline->taa_reset = false;
  cw_pass_bind_target(ctx, dst);
  cw_pass_use(taa_prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, hist->tex);
  cw_pass_bind_texture(2, ctx->velocity->tex);
  cw_pass_uniform1i(taa_prog, "u_src", 0);
  cw_pass_uniform1i(taa_prog, "u_hist", 1);
  cw_pass_uniform1i(taa_prog, "u_vel", 2);
  cw_pass_uniform2f(taa_prog, "u_size", (float)ctx->width, (float)ctx->height);
  cw_pass_uniform1f(taa_prog, "u_feedback", CW_CLAMP(ctx->settings->taa_feedback, 0.0f, 0.98f));
  cw_pass_uniform1i(taa_prog, "u_fresh", fresh ? 1 : 0);
  cw_pass_fullscreen();
  ctx->result = dst;
  ctx->pipeline->taa_frames = fresh ? 1 : ctx->pipeline->taa_frames + 1;
}

/* ── bloom: dual-filter chain (Jimenez 2014, "next generation post") ────
 * Down: 13-tap filter per level (Karis average on the first), from the
 * prefiltered source. Up: 3x3 tent of the coarser level added to each
 * finer one. Composite (energy conserving): out = src - k*B(src) +
 * k*bloom/levels, where B is the threshold/knee prefilter, so the energy
 * that blooms is taken from where it came from (threshold 0: a lerp). */
static unsigned bl_down_prog, bl_up_prog, bl_comp_prog;
static const char *BL_PREFILTER =
    S(uniform vec4 u_thresh; /* threshold, threshold - knee, 2 knee, 0.25 / knee */
      vec3 prefilter(vec3 c) {
        float br = max(c.r, max(c.g, c.b));
        float rq = clamp(br - u_thresh.y, 0.0, u_thresh.z);
        rq = u_thresh.w * rq * rq;
        float w = max(rq, br - u_thresh.x) / max(br, 1e-5);
        return c * w;
      });
static const char *BL_DOWN_FS = S(
  uniform sampler2D u_src;
  uniform vec2 u_texel;  /* source texel */
  uniform int u_first;
  out vec4 o;
  vec3 tap(vec2 uv) {
    vec3 c = texture(u_src, uv).rgb;
    return u_first != 0 ? prefilter(c) : c;
  }
  vec3 karis(vec3 a, vec3 b, vec3 c, vec3 d) {
    float wa = 1.0 / (1.0 + luma(a)), wb = 1.0 / (1.0 + luma(b)), wc = 1.0 / (1.0 + luma(c)), wd = 1.0 / (1.0 + luma(d));
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
  }
  void main() {
    vec2 t = u_texel;
    vec3 A = tap(v_uv + t * vec2(-2.0, 2.0)), B = tap(v_uv + t * vec2(0.0, 2.0)), C = tap(v_uv + t * vec2(2.0, 2.0));
    vec3 D = tap(v_uv + t * vec2(-1.0, 1.0)), E = tap(v_uv + t * vec2(1.0, 1.0));
    vec3 F = tap(v_uv + t * vec2(-2.0, 0.0)), G = tap(v_uv), H = tap(v_uv + t * vec2(2.0, 0.0));
    vec3 I = tap(v_uv + t * vec2(-1.0, -1.0)), J = tap(v_uv + t * vec2(1.0, -1.0));
    vec3 K = tap(v_uv + t * vec2(-2.0, -2.0)), L = tap(v_uv + t * vec2(0.0, -2.0)), M = tap(v_uv + t * vec2(2.0, -2.0));
    vec3 r;
    if (u_first != 0) {
      r = karis(D, E, I, J) * 0.5 + karis(A, B, F, G) * 0.125 + karis(B, C, G, H) * 0.125 + karis(F, G, K, L) * 0.125 +
          karis(G, H, L, M) * 0.125;
    } else {
      r = (D + E + I + J) * 0.125 + (A + C + K + M) * 0.03125 + (B + F + H + L) * 0.0625 + G * 0.125;
    }
    o = vec4(r, 1.0);
  });
static const char *BL_UP_FS = S(
  uniform sampler2D u_coarse;
  uniform sampler2D u_fine;
  uniform vec2 u_texel;  /* coarse texel */
  uniform float u_radius;
  out vec4 o;
  void main() {
    vec2 t = u_texel * u_radius;
    vec3 s = texture(u_coarse, v_uv).rgb * 4.0;
    s += (texture(u_coarse, v_uv + vec2(-t.x, 0.0)).rgb + texture(u_coarse, v_uv + vec2(t.x, 0.0)).rgb +
          texture(u_coarse, v_uv + vec2(0.0, -t.y)).rgb + texture(u_coarse, v_uv + vec2(0.0, t.y)).rgb) * 2.0;
    s += texture(u_coarse, v_uv - t).rgb + texture(u_coarse, v_uv + t).rgb + texture(u_coarse, v_uv + vec2(-t.x, t.y)).rgb +
         texture(u_coarse, v_uv + vec2(t.x, -t.y)).rgb;
    o = vec4(texture(u_fine, v_uv).rgb + s / 16.0, 1.0);
  });
static const char *BL_COMP_FS = S(
  uniform sampler2D u_src;
  uniform sampler2D u_bloom;
  uniform vec2 u_texel;  /* bloom texel */
  uniform float u_radius;
  uniform float u_k;     /* intensity */
  uniform float u_norm;  /* 1 / levels */
  out vec4 o;
  void main() {
    vec4 c = texture(u_src, v_uv);
    vec2 t = u_texel * u_radius;
    vec3 s = texture(u_bloom, v_uv).rgb * 4.0;
    s += (texture(u_bloom, v_uv + vec2(-t.x, 0.0)).rgb + texture(u_bloom, v_uv + vec2(t.x, 0.0)).rgb +
          texture(u_bloom, v_uv + vec2(0.0, -t.y)).rgb + texture(u_bloom, v_uv + vec2(0.0, t.y)).rgb) * 2.0;
    s += texture(u_bloom, v_uv - t).rgb + texture(u_bloom, v_uv + t).rgb + texture(u_bloom, v_uv + vec2(-t.x, t.y)).rgb +
         texture(u_bloom, v_uv + vec2(t.x, -t.y)).rgb;
    vec3 bloom = s / 16.0 * u_norm;
    o = vec4(max(c.rgb + u_k * (bloom - prefilter(c.rgb)), vec3(0.0)), c.a);
  });

static bool bloom_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  return ctx->settings->bloom;
}

static void bloom_thresh(unsigned prog, const cw_post_settings *s) {
  float t = CW_MAX(s->bloom_threshold, 0.0f);
  float knee = CW_MAX(t * CW_CLAMP(s->bloom_knee, 0.0f, 1.0f), 1e-5f);
  cw_pass_uniform4f(prog, "u_thresh", t, t - knee, 2 * knee, 0.25f / knee);
}

static void bloom_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_post_settings *s = ctx->settings;
  if (!bl_down_prog) {
    /* WGSL: the prefilter reads u.u_thresh, so it follows the block */
    char w[8192];
    snprintf(w, sizeof w, "%s%s", WGSL_BL_DOWN, WGSL_BL_PREFILTER);
    bl_down_prog = prog_of("bloom.down", GLSL_COMMON, BL_PREFILTER, BL_DOWN_FS, WGSL_COMMON, w, NULL);
    bl_up_prog = prog_of("bloom.up", BL_UP_FS, NULL, NULL, WGSL_BL_UP, NULL, NULL);
    snprintf(w, sizeof w, "%s%s", WGSL_BL_COMP, WGSL_BL_PREFILTER);
    bl_comp_prog = prog_of("bloom.composite", GLSL_COMMON, BL_PREFILTER, BL_COMP_FS, WGSL_COMMON, w, NULL);
  }
  if (!bl_down_prog || !bl_up_prog || !bl_comp_prog) return;
  cw_rt_format f = ctx->src->format == CW_RT_RGBA8 ? CW_RT_RGBA8 : CW_RT_RGBA16F;
  enum { MAXL = 10 };
  const cw_rt *down[MAXL] = { 0 }, *up[MAXL] = { 0 };
  /* render scale: at 2x the first level holds what the full frame held at
   * 1x, so the chain gets one level more and the finest one(s) are left out
   * of the sum; the glow keeps its size and strength on screen */
  float ratio = cw_pixel_ratio();
  int skip = ratio > 1 ? CW_CLAMP((int)floorf(log2f(ratio) + 0.5f), 0, 3) : 0;
  int levels = CW_CLAMP(s->bloom_levels, 1, MAXL - 3) + skip;
  int w = ctx->width, h = ctx->height, n = 0;
  const cw_rt *prev = ctx->src;
  cw_pass_use(bl_down_prog);
  cw_pass_uniform1i(bl_down_prog, "u_src", 0);
  bloom_thresh(bl_down_prog, s);
  for (int i = 0; i < levels; i++) {
    w = (w + 1) / 2;
    h = (h + 1) / 2;
    if (w < 2 || h < 2) break;
    down[i] = cw_pass_rt(ctx, w, h, f, false);
    if (!down[i]) break;
    cw_pass_bind_target(ctx, down[i]);
    cw_pass_bind_texture(0, prev->tex);
    cw_pass_uniform2f(bl_down_prog, "u_texel", 1.0f / (float)prev->w, 1.0f / (float)prev->h);
    cw_pass_uniform1i(bl_down_prog, "u_first", i == 0);
    cw_pass_fullscreen();
    prev = down[i];
    n++;
  }
  if (!n) return;
  if (skip > n - 1) skip = n - 1;
  /* up: up[i] = down[i] + tent(up[i + 1]); the coarsest is its own up */
  up[n - 1] = down[n - 1];
  cw_pass_use(bl_up_prog);
  cw_pass_uniform1i(bl_up_prog, "u_coarse", 0);
  cw_pass_uniform1i(bl_up_prog, "u_fine", 1);
  cw_pass_uniform1f(bl_up_prog, "u_radius", s->bloom_radius);
  for (int i = n - 2; i >= skip; i--) {
    up[i] = cw_pass_rt(ctx, down[i]->w, down[i]->h, f, false);
    if (!up[i]) return;
    cw_pass_bind_target(ctx, up[i]);
    cw_pass_bind_texture(0, up[i + 1]->tex);
    cw_pass_bind_texture(1, down[i]->tex);
    cw_pass_uniform2f(bl_up_prog, "u_texel", 1.0f / (float)up[i + 1]->w, 1.0f / (float)up[i + 1]->h);
    cw_pass_fullscreen();
  }
  if (ctx->pipeline->fuse_bloom) {
    /* the tonemap pass composites the glow (pipeline_internal.h) */
    cw_pipeline *pl = ctx->pipeline;
    pl->bloom_glow = up[skip];
    pl->bloom_texel[0] = 1.0f / (float)up[skip]->w;
    pl->bloom_texel[1] = 1.0f / (float)up[skip]->h;
    pl->bloom_norm = 1.0f / (float)(n - skip);
    for (int i = 0; i < n; i++) {
      if (down[i] != up[skip]) cw_pass_rt_release(ctx, down[i]);
      if (up[i] && up[i] != down[i] && up[i] != up[skip]) cw_pass_rt_release(ctx, up[i]);
    }
    return;
  }
  cw_pass_bind_target(ctx, ctx->dst);
  cw_pass_use(bl_comp_prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, up[skip]->tex);
  cw_pass_uniform1i(bl_comp_prog, "u_src", 0);
  cw_pass_uniform1i(bl_comp_prog, "u_bloom", 1);
  cw_pass_uniform2f(bl_comp_prog, "u_texel", 1.0f / (float)up[skip]->w, 1.0f / (float)up[skip]->h);
  cw_pass_uniform1f(bl_comp_prog, "u_radius", s->bloom_radius);
  cw_pass_uniform1f(bl_comp_prog, "u_k", CW_CLAMP(s->bloom_intensity, 0.0f, 1.0f));
  cw_pass_uniform1f(bl_comp_prog, "u_norm", 1.0f / (float)(n - skip));
  bloom_thresh(bl_comp_prog, s);
  cw_pass_fullscreen();
  if (s->debug_view == CW_VIEW_BLOOM) {
    const cw_rt *keep = cw_pipeline_rt_persistent(ctx->pipeline, "bloom.view", up[skip]->w, up[skip]->h, f, NULL);
    if (keep) cw__pass_copy(ctx, up[skip]->tex, keep);
  }
  for (int i = 0; i < n; i++) {
    cw_pass_rt_release(ctx, down[i]);
    if (up[i] && up[i] != down[i]) cw_pass_rt_release(ctx, up[i]);
  }
}

/* ── tonemap: exposure, tone curve, sRGB transfer ────────────────────── */

static const char *TM_FS = S(
  uniform sampler2D u_src;
  uniform float toneMappingExposure;
  uniform int u_curve;
  out vec4 o;
  /* three.js r186's curves (ToneMappingFunctions; the GLSL tonemapping_pars_fragment) */
  vec3 LinearToneMapping(vec3 color) { return toneMappingExposure * color; }
  vec3 ReinhardToneMapping(vec3 color) {
    color *= toneMappingExposure;
    return clamp(color / (vec3(1.0) + color), 0.0, 1.0);
  }
  vec3 OptimizedCineonToneMapping(vec3 color) {
    color *= toneMappingExposure;
    color = max(vec3(0.0), color - 0.004);
    return pow((color * (6.2 * color + 0.5)) / (color * (6.2 * color + 1.7) + 0.06), vec3(2.2));
  }
  vec3 RRTAndODTFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
  }
  vec3 ACESFilmicToneMapping(vec3 color) {
    const mat3 ACESInputMat = mat3(vec3(0.59719, 0.07600, 0.02840), vec3(0.35458, 0.90834, 0.13383), vec3(0.04823, 0.01566, 0.83777));
    const mat3 ACESOutputMat = mat3(vec3(1.60475, -0.10208, -0.00327), vec3(-0.53108, 1.10813, -0.07276), vec3(-0.07367, -0.00605, 1.07602));
    color *= toneMappingExposure / 0.6;
    color = ACESInputMat * color;
    color = RRTAndODTFit(color);
    color = ACESOutputMat * color;
    return clamp(color, 0.0, 1.0);
  }
  /* AgX: Blender's view transform, the minimal fit (Benjamin Wrensch) in
   * three.js's AgXToneMapping form: linear sRGB in, linear sRGB out */
  vec3 agxDefaultContrastApprox(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
  }
  vec3 AgXToneMapping(vec3 color) {
    const mat3 LINEAR_SRGB_TO_LINEAR_REC2020 = mat3(vec3(0.6274, 0.0691, 0.0164), vec3(0.3293, 0.9195, 0.0880), vec3(0.0433, 0.0113, 0.8956));
    const mat3 LINEAR_REC2020_TO_LINEAR_SRGB = mat3(vec3(1.6605, -0.1246, -0.0182), vec3(-0.5876, 1.1329, -0.1006), vec3(-0.0728, -0.0083, 1.1187));
    const mat3 AgXInsetMatrix = mat3(vec3(0.856627153315983, 0.137318972929847, 0.11189821299995),
                                     vec3(0.0951212405381588, 0.761241990602591, 0.0767994186031903),
                                     vec3(0.0482516061458583, 0.101439036467562, 0.811302368396859));
    const mat3 AgXOutsetMatrix = mat3(vec3(1.1271005818144368, -0.1413297634984383, -0.14132976349843826),
                                      vec3(-0.11060664309660323, 1.157823702216272, -0.11060664309660294),
                                      vec3(-0.016493938717834573, -0.016493938717834257, 1.2519364065950405));
    const float AgxMinEv = -12.47393;
    const float AgxMaxEv = 4.026069;
    color *= toneMappingExposure;
    color = LINEAR_SRGB_TO_LINEAR_REC2020 * color;
    color = AgXInsetMatrix * color;
    color = max(color, 1e-10);
    color = log2(color);
    color = (color - AgxMinEv) / (AgxMaxEv - AgxMinEv);
    color = clamp(color, 0.0, 1.0);
    color = agxDefaultContrastApprox(color);
    color = AgXOutsetMatrix * color;
    color = pow(max(vec3(0.0), color), vec3(2.2));
    color = LINEAR_REC2020_TO_LINEAR_SRGB * color;
    return clamp(color, 0.0, 1.0);
  }
  /* Khronos PBR Neutral */
  vec3 NeutralToneMapping(vec3 color) {
    const float StartCompression = 0.8 - 0.04;
    const float Desaturation = 0.15;
    color *= toneMappingExposure;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < StartCompression) return color;
    float d = 1.0 - StartCompression;
    float newPeak = 1.0 - d * d / (peak + d - StartCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (Desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
  }
  /* r186's sRGBTransferOETF */
  vec3 LinearTosRGB(vec3 v) {
    return mix(pow(v, vec3(0.41666)) * 1.055 - vec3(0.055), v * 12.92, vec3(lessThanEqual(v, vec3(0.0031308))));
  });
/* the tonemap pass's main, also the fused pass's middle (FINAL_FS) */
static const char *TM_MAIN = S(
  void main() {
    vec4 c = texture(u_src, v_uv);
    TM_FUSED_IN
    vec3 x = max(c.rgb, vec3(0.0));
    if (u_curve == 0) x = clamp(x * toneMappingExposure, 0.0, 1.0);
    else if (u_curve == 1) x = clamp(LinearToneMapping(x), 0.0, 1.0);
    else if (u_curve == 2) x = ReinhardToneMapping(x);
    else if (u_curve == 3) x = OptimizedCineonToneMapping(x);
    else if (u_curve == 4) x = ACESFilmicToneMapping(x);
    else if (u_curve == 5) x = AgXToneMapping(x);
    else x = clamp(NeutralToneMapping(x), 0.0, 1.0);
    /* r186 applies the output transfer after every curve: Cineon's pow(2.2) returns it to linear first */
    x = LinearTosRGB(x);
    TM_FUSED_OUT
    o = vec4(x, 1.0);
    TM_FUSED_LENS
  });

/* ── post fusion: steps run inside another pass ────────────────────────
 * pipeline.c plan_fusion decides, for GL and WebGPU alike: the height fog
 * inside the SSAO composite (not under TAA), bloom's composite before the tone curve and the
 * LUT grade after it inside the tonemap pass, the lens effects inside the
 * anti-aliasing pass or, with none, inside the tonemap pass.
 * Each value is rounded where the separate passes' targets rounded it: the
 * fog pass's output and its blend into the RGBA16F scene, the composite's
 * RGBA16F, the tonemap's and the anti-aliasing's RGBA8. Rounded as AMD's
 * colour export does it (radeonsi, RADV): a value goes to the target as a
 * half float rounded toward zero (v_cvt_pkrtz), an 8-bit unorm target then
 * rounds that half to the nearest of 255 steps, and the blend unit's RGBA16F
 * result is rounded toward zero too. Against the passes run separately
 * (CW_TEST_NO_FUSION=1) a game's frame differs in under 0.05% of its pixels,
 * by at most 2/255, on AMD GPUs (tests/post-fusion.mjs); a GPU that rounds
 * otherwise stays within a step at the rounding points.
 * CW_TEST_FUSION_RAW=1 (native, the test's control) leaves the rounding out. */
/* (plain strings: the preprocessor lines must reach the GLSL compiler, not C's) */
static const char *FUSE_ROUND =
    "uint h16bits(float v) {\n"
    "  uint h = packHalf2x16(vec2(v, 0.0)) & 0xffffu;\n"
    "  if (abs(unpackHalf2x16(h).x) > abs(v) && (h & 0x7fffu) != 0u) h -= 1u;\n"
    "  return h;\n"
    "}\n"
    "float h16(float v) { return unpackHalf2x16(h16bits(v)).x; }\n"
    "#ifdef FUSE_RAW\n"
    "vec3 q16(vec3 v) { return v; }\n"
    "float r16(float v) { return v; }\n"
    "vec3 q8(vec3 v) { return v; }\n"
    "vec4 q8a(vec4 v) { return v; }\n"
    "#else\n"
    "vec3 q16(vec3 v) { return vec3(h16(v.r), h16(v.g), h16(v.b)); }\n"
    "float r16(float v) { return h16(v); }\n"
    "vec3 q8(vec3 v) { return floor(q16(clamp(v, 0.0, 1.0)) * 255.0 + 0.5) / 255.0; }\n"
    "vec4 q8a(vec4 v) { v = clamp(v, 0.0, 1.0); return floor(vec4(h16(v.r), h16(v.g), h16(v.b), h16(v.a)) * 255.0 + 0.5) / 255.0; }\n"
    "#endif\n";
const char *cw__fuse_round_glsl(void) { return FUSE_ROUND; }
const char *cw__fuse_round_wgsl(void) { return cw__fusion_raw() ? WGSL_FUSE_ROUND_RAW : WGSL_FUSE_ROUND; }
const char *cw__post_common_glsl(void) { return GLSL_COMMON; }
static const char *FUSE_FS = S(
  uniform sampler2D u_bloom;
  uniform vec2 u_texel;
  uniform float u_radius;
  uniform float u_k;
  uniform float u_norm;
  uniform sampler3D u_lut;
  uniform vec4 u_scale;
  uniform vec3 u_dmin;
  uniform vec3 u_dinv;
  vec4 fuse_bloom(vec4 c) {
    vec2 t = u_texel * u_radius;
    vec3 s = texture(u_bloom, v_uv).rgb * 4.0;
    s += (texture(u_bloom, v_uv + vec2(-t.x, 0.0)).rgb + texture(u_bloom, v_uv + vec2(t.x, 0.0)).rgb +
          texture(u_bloom, v_uv + vec2(0.0, -t.y)).rgb + texture(u_bloom, v_uv + vec2(0.0, t.y)).rgb) * 2.0;
    s += texture(u_bloom, v_uv - t).rgb + texture(u_bloom, v_uv + t).rgb + texture(u_bloom, v_uv + vec2(-t.x, t.y)).rgb +
         texture(u_bloom, v_uv + vec2(t.x, -t.y)).rgb;
    vec3 bloom = s / 16.0 * u_norm;
    return vec4(q16(max(c.rgb + u_k * (bloom - prefilter(c.rgb)), vec3(0.0))), c.a);
  }
  vec3 fuse_grade(vec3 c) {
    c = q8(c);
    vec3 t = clamp((c - u_dmin) * u_dinv, 0.0, 1.0);
    vec3 g = texture(u_lut, t * u_scale.x + u_scale.y).rgb;
    return mix(c, g, u_scale.z);
  });
/* the lens pass (LENS_FS, chromatic aberration off) on the RGBA8 value the
 * pass before it wrote (needs GLSL_COMMON's luma and hash01, FUSE_ROUND) */
static const char *FUSE_LENS = S(
  uniform vec4 u_on;      /* ca (off when fused), vignette, grain, aspect */
  uniform vec4 u_amt;     /* ca, vignette, vignette smoothness, grain */
  uniform int u_frame;
  vec4 fuse_lens(vec4 c) {
    c = q8a(c);
    vec2 d = v_uv - 0.5;
    if (u_on.y > 0.5) {
      float r = length(d * vec2(u_on.w, 1.0)) / length(vec2(0.5 * u_on.w, 0.5));
      float f = smoothstep(1.0 - u_amt.z, 1.0 + u_amt.z * 0.25, r * (1.0 + 0.5 * u_amt.z));
      c.rgb *= 1.0 - u_amt.y * f;
    }
    if (u_on.z > 0.5) {
      float n = hash01(uvec3(uvec2(gl_FragCoord.xy), uint(u_frame))) - 0.5;
      float l = luma(c.rgb);
      c.rgb = max(c.rgb + n * u_amt.w * (1.0 - 0.5 * l), vec3(0.0));
    }
    return c;
  });
const char *cw__fuse_lens_glsl(void) { return FUSE_LENS; }
const char *cw__fuse_lens_wgsl(void) { return WGSL_FUSE_LENS; }
/* the fog over the scene as the fog pass's blend leaves it: its output
 * exported to RGBA16F, then dst' = src + dst (1 - src.a), the blend's result
 * rounded toward zero too */
static const char *FUSE_FOG = S(
  vec4 fog_over(vec4 c, vec4 f) {
    if (f.a < 0.0) return c;
    vec4 s = vec4(q16(f.rgb), r16(f.a));
    return vec4(q16(s.rgb + c.rgb * (1.0 - s.a)), r16(s.a + c.a * (1.0 - s.a)));
  });

static unsigned ao_fog_program(void) {
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    static char w[24576];
    snprintf(w, sizeof w, "%s%s%s%s%s%s%s", WGSL_COMMON, WGSL_AO_COMP_FOG_HEAD, cw__fuse_round_wgsl(), WGSL_FOG_FN, WGSL_FUSE_FOG,
             WGSL_AO_COMP_FOG_IN, WGSL_AO_COMP_BODY);
    return cw_pass_program_wgsl("ssao.composite+fog", w);
  }
  static char g[24576];
  snprintf(g, sizeof g, "#define AO_FUSED_IN c = fog_over(c, fog_eval(v_uv, d));\n%s%s%s%s%s", cw__fusion_raw() ? "#define FUSE_RAW 1\n" : "", FUSE_ROUND, cw__fog_glsl(), FUSE_FOG, AO_COMP_FS);
  return cw_pass_program("ssao.composite+fog", g);
}

static bool tonemap_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  (void)ctx;
  return true; /* the scene is always linear HDR: this pass makes display values */
}

static unsigned grade_prog;
static void grade_uniforms(cw_pass_ctx *ctx, unsigned prog, unsigned lut, int n, const float *dmin, const float *dmax, int unit) {
  cw_pass_bind_texture_3d(unit, lut);
  cw_pass_uniform1i(prog, "u_lut", unit);
  cw_pass_uniform4f(prog, "u_scale", (float)(n - 1) / (float)n, 0.5f / (float)n, CW_CLAMP(ctx->settings->grade_intensity, 0.0f, 1.0f), 0);
  cw_pass_uniform3f(prog, "u_dmin", dmin[0], dmin[1], dmin[2]);
  cw_pass_uniform3f(prog, "u_dinv", 1 / (dmax[0] - dmin[0]), 1 / (dmax[1] - dmin[1]), 1 / (dmax[2] - dmin[2]));
}

void cw__lens_uniforms(cw_pass_ctx *ctx, unsigned prog, bool ca) {
  const cw_post_settings *s = ctx->settings;
  cw_pass_uniform4f(prog, "u_on", ca ? s->chromatic_aberration : 0, s->vignette, s->grain,
                    (float)ctx->width / (float)CW_MAX(ctx->height, 1));
  cw_pass_uniform4f(prog, "u_amt", s->ca_intensity, s->vignette_intensity, CW_CLAMP(s->vignette_smoothness, 0.01f, 1.0f),
                    s->grain_intensity);
  cw_pass_uniform1i(prog, "u_frame", (int)(ctx->frame & 0x7fffffff));
}

/* the tonemap program: plain, or with bloom's composite, the grade and / or
 * the lens fused in */
static unsigned tm_variant(bool bloom, bool grade, bool lens) {
  static unsigned progs[8];
  int k = (bloom ? 1 : 0) | (grade ? 2 : 0) | (lens ? 4 : 0);
  if (progs[k]) return progs[k];
  static const char *const names[8] = { "tonemap", "tonemap+bloom", "tonemap+grade", "tonemap+bloom+grade",
                                        "tonemap+lens", "tonemap+bloom+lens", "tonemap+grade+lens", "tonemap+bloom+grade+lens" };
  if (!k)
    return progs[0] = prog_of("tonemap", "#define TM_FUSED_IN\n#define TM_FUSED_OUT\n#define TM_FUSED_LENS\n", TM_FS, TM_MAIN,
                              WGSL_TM, NULL, NULL);
  static char buf[49152];
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    snprintf(buf, sizeof buf, "%s%s%s%s%s%s%s%s%s%s%s", WGSL_TM_FUSED_HEAD, WGSL_COMMON, WGSL_BL_PREFILTER, cw__fuse_round_wgsl(),
             WGSL_FUSE_BLOOM_GRADE, WGSL_FUSE_LENS, WGSL_TM_FNS, WGSL_TM_FUSED_FS, bloom ? "c = fuse_bloom(c, uv);\n" : "",
             WGSL_TM_FUSED_CURVES, "");
    /* the steps after the curve */
    size_t n = strlen(buf);
    snprintf(buf + n, sizeof buf - n, "%s  var o = vec4f(x, 1.0);\n%s  return o;\n}\n", grade ? "x = fuse_grade(x);\n" : "",
             lens ? "o = fuse_lens(o, uv, pos);\n" : "");
    return progs[k] = cw_pass_program_wgsl(names[k], buf);
  }
  snprintf(buf, sizeof buf, "#define TM_FUSED_IN %s\n#define TM_FUSED_OUT %s\n#define TM_FUSED_LENS %s\n%s%s%s%s%s%s%s%s",
           bloom ? "c = fuse_bloom(c);" : "", grade ? "x = fuse_grade(x);" : "", lens ? "o = fuse_lens(o);" : "",
           cw__fusion_raw() ? "#define FUSE_RAW 1\n" : "", GLSL_COMMON, BL_PREFILTER, TM_FS, FUSE_ROUND, FUSE_FS, FUSE_LENS, TM_MAIN);
  return progs[k] = cw_pass_program(names[k], buf);
}

static void tonemap_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_post_settings *s = ctx->settings;
  cw_pipeline *pl = ctx->pipeline;
  bool bloom = pl->fuse_bloom && pl->bloom_glow, grade = pl->fuse_grade, lens = pl->fuse_lens_tm;
  int n = 0;
  float dmin[3], dmax[3];
  unsigned lut = grade ? cw__lut_texture(s->grade_lut, &n, dmin, dmax) : 0;
  if (grade && !lut) grade = false;
  unsigned prog = tm_variant(bloom, grade, lens);
  if (!prog) return;
  cw_pass_use(prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_uniform1i(prog, "u_src", 0);
  cw_pass_uniform1f(prog, "toneMappingExposure", s->exposure * exp2f(s->exposure_ev));
  cw_pass_uniform1i(prog, "u_curve", (int)CW_CLAMP((int)s->tonemap, 0, CW_TONEMAP_COUNT - 1));
  if (bloom) {
    cw_pass_bind_texture(1, pl->bloom_glow->tex);
    cw_pass_uniform1i(prog, "u_bloom", 1);
    cw_pass_uniform2f(prog, "u_texel", pl->bloom_texel[0], pl->bloom_texel[1]);
    cw_pass_uniform1f(prog, "u_radius", s->bloom_radius);
    cw_pass_uniform1f(prog, "u_k", CW_CLAMP(s->bloom_intensity, 0.0f, 1.0f));
    cw_pass_uniform1f(prog, "u_norm", pl->bloom_norm);
    bloom_thresh(prog, s);
  }
  if (grade) grade_uniforms(ctx, prog, lut, n, dmin, dmax, 2);
  /* WebGPU: every fused variant declares u_lut (binding 3, unit 2) and binds
   * whatever that unit holds; without the grade it held an earlier pass's 2D
   * texture, which a texture_3d binding rejects. The empty unit binds a
   * dummy 3D texture the program never samples. */
  else if (cw_gpu_backend() == CW_GPU_WEBGPU) cw_pass_bind_texture_3d(2, 0);
  if (lens) cw__lens_uniforms(ctx, prog, false);
  cw_pass_fullscreen();
  if (grade && cw_gpu_backend() != CW_GPU_WEBGPU) {
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0);
  }
  if (pl->bloom_glow) {
    cw_pass_rt_release(ctx, pl->bloom_glow);
    pl->bloom_glow = NULL;
  }
}

/* ── grade: 3D LUT on display values ─────────────────────────────────── */

static const char *GRADE_FS = S(
  uniform sampler2D u_src;
  uniform sampler3D u_lut;
  uniform vec4 u_scale;    /* (n - 1) / n, 0.5 / n, intensity, 0 */
  uniform vec3 u_dmin;
  uniform vec3 u_dinv;     /* 1 / (dmax - dmin) */
  out vec4 o;
  void main() {
    vec4 c = texture(u_src, v_uv);
    vec3 t = clamp((c.rgb - u_dmin) * u_dinv, 0.0, 1.0);
    vec3 g = texture(u_lut, t * u_scale.x + u_scale.y).rgb;
    o = vec4(mix(c.rgb, g, u_scale.z), c.a);
  });

static bool grade_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  return ctx->settings->grade && cw__lut_texture(ctx->settings->grade_lut, NULL, NULL, NULL);
}

static void grade_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  if (!grade_prog) grade_prog = prog_of("grade", GRADE_FS, NULL, NULL, WGSL_GRADE, NULL, NULL);
  int n = 0;
  float dmin[3], dmax[3];
  unsigned lut = cw__lut_texture(ctx->settings->grade_lut, &n, dmin, dmax);
  if (!grade_prog || !lut) return;
  cw_pass_use(grade_prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture_3d(1, lut);
  cw_pass_uniform1i(grade_prog, "u_src", 0);
  cw_pass_uniform1i(grade_prog, "u_lut", 1);
  cw_pass_uniform4f(grade_prog, "u_scale", (float)(n - 1) / (float)n, 0.5f / (float)n,
                    CW_CLAMP(ctx->settings->grade_intensity, 0.0f, 1.0f), 0);
  cw_pass_uniform3f(grade_prog, "u_dmin", dmin[0], dmin[1], dmin[2]);
  cw_pass_uniform3f(grade_prog, "u_dinv", 1 / (dmax[0] - dmin[0]), 1 / (dmax[1] - dmin[1]), 1 / (dmax[2] - dmin[2]));
  cw_pass_fullscreen();
  if (cw_gpu_backend() != CW_GPU_WEBGPU) {
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0);
  }
}

/* ── FXAA 3.11 (quality preset 12) ───────────────────────────────────── */

static unsigned fxaa_prog, fxaa_lens_prog;
static const char *FXAA_FS = S(
  uniform sampler2D u_src;
  uniform vec2 u_rcp;
  out vec4 o;
  float L(vec2 p) { return dot(texture(u_src, p).rgb, vec3(0.299, 0.587, 0.114)); }
  vec4 fxaa() {
    vec2 posM = v_uv;
    vec4 rgbyM = texture(u_src, posM);
    float lumaM = dot(rgbyM.rgb, vec3(0.299, 0.587, 0.114));
    float lumaS = L(posM + vec2(0.0, 1.0) * u_rcp), lumaE = L(posM + vec2(1.0, 0.0) * u_rcp);
    float lumaN = L(posM + vec2(0.0, -1.0) * u_rcp), lumaW = L(posM + vec2(-1.0, 0.0) * u_rcp);
    float maxSM = max(lumaS, lumaM), minSM = min(lumaS, lumaM);
    float maxESM = max(lumaE, maxSM), minESM = min(lumaE, minSM);
    float maxWN = max(lumaN, lumaW), minWN = min(lumaN, lumaW);
    float rangeMax = max(maxWN, maxESM), rangeMin = min(minWN, minESM);
    float range = rangeMax - rangeMin;
    if (range < max(0.0312, rangeMax * 0.125)) return rgbyM;
    float lumaNW = L(posM + vec2(-1.0, -1.0) * u_rcp), lumaSE = L(posM + vec2(1.0, 1.0) * u_rcp);
    float lumaNE = L(posM + vec2(1.0, -1.0) * u_rcp), lumaSW = L(posM + vec2(-1.0, 1.0) * u_rcp);
    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0 * lumaM + lumaNS, edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE, edgeVert2 = -2.0 * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2), edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    float edgeHorz3 = -2.0 * lumaW + lumaNWSW, edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4, edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = u_rcp.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }
    if (horzSpan) lengthSign = u_rcp.y;
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = clamp(abs(subpixB) * subpixRcpRange, 0.0, 1.0);
    vec2 posB = posM;
    vec2 offNP = horzSpan ? vec2(u_rcp.x, 0.0) : vec2(0.0, u_rcp.y);
    if (!horzSpan) posB.x += lengthSign * 0.5;
    if (horzSpan) posB.y += lengthSign * 0.5;
    vec2 posN = posB - offNP, posP = posB + offNP;
    float subpixD = -2.0 * subpixC + 3.0;
    float lumaEndN = L(posN);
    float subpixE = subpixC * subpixC;
    float lumaEndP = L(posP);
    if (!pairN) lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled, doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * 1.5;
    if (!doneP) posP += offNP * 1.5;
    float qs[3] = float[3](2.0, 4.0, 12.0);
    for (int i = 0; i < 3; i++) {
      if (doneN && doneP) break;
      if (!doneN) lumaEndN = L(posN) - lumaNN * 0.5;
      if (!doneP) lumaEndP = L(posP) - lumaNN * 0.5;
      doneN = abs(lumaEndN) >= gradientScaled;
      doneP = abs(lumaEndP) >= gradientScaled;
      if (!doneN) posN -= offNP * qs[i];
      if (!doneP) posP += offNP * qs[i];
    }
    float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;
    float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero, goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLengthRcp = 1.0 / (dstP + dstN);
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = dst * (-spanLengthRcp) + 0.5;
    float subpixH = subpixG * 0.75;
    float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan) posM.x += pixelOffsetSubpix * lengthSign;
    if (horzSpan) posM.y += pixelOffsetSubpix * lengthSign;
    return vec4(texture(u_src, posM).rgb, rgbyM.a);
  }
  void main() {
    o = fxaa();
    FXAA_FUSED_OUT
  });

/* SMAA lives in pipeline_smaa.c */
void cw__smaa_exec(cw_pass_ctx *ctx);

static bool aa_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  return ctx->settings->aa == CW_AA_FXAA || ctx->settings->aa == CW_AA_SMAA;
}

static void aa_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  if (ctx->settings->aa == CW_AA_SMAA) {
    cw__smaa_exec(ctx);
    return;
  }
  bool lens = ctx->pipeline->fuse_lens;
  if (!fxaa_prog) fxaa_prog = prog_of("fxaa", "#define FXAA_FUSED_OUT\n", FXAA_FS, NULL, WGSL_FXAA_HEAD, WGSL_FXAA, WGSL_FXAA_FS);
  if (lens && !fxaa_lens_prog) {
    static char b[32768];
    if (cw_gpu_backend() == CW_GPU_WEBGPU) {
      snprintf(b, sizeof b, "%s%s%s%s%s%s", WGSL_FXAA_LENS_HEAD, WGSL_COMMON, cw__fuse_round_wgsl(), WGSL_FUSE_LENS, WGSL_FXAA,
               WGSL_FXAA_LENS_FS);
      fxaa_lens_prog = cw_pass_program_wgsl("fxaa+lens", b);
    } else {
      snprintf(b, sizeof b, "#define FXAA_FUSED_OUT o = fuse_lens(o);\n%s%s%s%s%s", cw__fusion_raw() ? "#define FUSE_RAW 1\n" : "",
               GLSL_COMMON, FUSE_ROUND, FUSE_LENS, FXAA_FS);
      fxaa_lens_prog = cw_pass_program("fxaa+lens", b);
    }
  }
  unsigned prog = lens ? fxaa_lens_prog : fxaa_prog;
  if (!prog) return;
  cw_pass_use(prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_uniform1i(prog, "u_src", 0);
  cw_pass_uniform2f(prog, "u_rcp", 1.0f / (float)ctx->width, 1.0f / (float)ctx->height);
  if (lens) cw__lens_uniforms(ctx, prog, false);
  cw_pass_fullscreen();
}

/* ── lens: chromatic aberration, vignette, film grain ───────────────── */

static unsigned lens_prog;
static const char *LENS_FS = S(
  uniform sampler2D u_src;
  uniform vec4 u_on;      /* ca, vignette, grain, aspect */
  uniform vec4 u_amt;     /* ca, vignette, vignette smoothness, grain */
  uniform int u_frame;
  out vec4 o;
  void main() {
    vec2 d = v_uv - 0.5;
    vec4 c = texture(u_src, v_uv);
    if (u_on.x > 0.5) {
      vec2 off = d * (u_amt.x * 2.0);
      c.r = texture(u_src, v_uv - off).r;
      c.b = texture(u_src, v_uv + off).b;
    }
    if (u_on.y > 0.5) {
      float r = length(d * vec2(u_on.w, 1.0)) / length(vec2(0.5 * u_on.w, 0.5));
      float f = smoothstep(1.0 - u_amt.z, 1.0 + u_amt.z * 0.25, r * (1.0 + 0.5 * u_amt.z));
      c.rgb *= 1.0 - u_amt.y * f;
    }
    if (u_on.z > 0.5) {
      float n = hash01(uvec3(uvec2(gl_FragCoord.xy), uint(u_frame))) - 0.5;
      float l = luma(c.rgb);
      c.rgb = max(c.rgb + n * u_amt.w * (1.0 - 0.5 * l), vec3(0.0));
    }
    o = c;
  });

static bool lens_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_post_settings *s = ctx->settings;
  return s->chromatic_aberration || s->vignette || s->grain;
}

static void lens_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  if (!lens_prog) lens_prog = prog_of("lens", GLSL_COMMON, LENS_FS, NULL, WGSL_COMMON, WGSL_LENS, NULL);
  if (!lens_prog) return;
  cw_pass_use(lens_prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_uniform1i(lens_prog, "u_src", 0);
  cw__lens_uniforms(ctx, lens_prog, true);
  cw_pass_fullscreen();
}

/* ── debug views ────────────────────────────────────────────────────── */

static unsigned view_prog;
static const char *VIEW_FS = S(
  uniform sampler2D u_depth;
  uniform sampler2D u_buf;
  uniform mat4 u_inv_proj;
  uniform vec2 u_size;
  uniform int u_view;
  uniform float u_norm;
  out vec4 o;
  vec3 vpos(ivec2 q) {
    q = clamp(q, ivec2(0), ivec2(u_size) - 1);
    float d = texelFetch(u_depth, q, 0).r;
    vec4 p = u_inv_proj * vec4((vec2(q) + 0.5) / u_size * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
  }
  void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    if (u_view == 1) { float z = -vpos(q).z; o = vec4(vec3(1.0 / (1.0 + z / 4.0)), 1.0); }
    else if (u_view == 2) {
      vec3 P = vpos(q), pr = vpos(q + ivec2(1, 0)), pl = vpos(q - ivec2(1, 0)), pu = vpos(q + ivec2(0, 1)), pd = vpos(q - ivec2(0, 1));
      vec3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
      vec3 dy = abs(pu.z - P.z) < abs(P.z - pd.z) ? pu - P : P - pd;
      vec3 N = normalize(cross(dx, dy));
      if (dot(N, -P) < 0.0) N = -N;
      o = vec4(N * 0.5 + 0.5, 1.0);
    } else if (u_view == 3) o = vec4(vec3(texture(u_buf, v_uv).r), 1.0);
    else if (u_view == 4) o = vec4(texture(u_buf, v_uv).xy * 20.0 + 0.5, 0.5, 1.0);
    else { vec3 b = texture(u_buf, v_uv).rgb * u_norm; o = vec4(b / (1.0 + b), 1.0); }
  });

static bool view_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  return ctx->settings->debug_view > CW_VIEW_NONE && ctx->settings->debug_view < CW_VIEW_COUNT;
}

static void view_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  cw_debug_view v = ctx->settings->debug_view;
  if (!view_prog) view_prog = prog_of("debug_view", VIEW_FS, NULL, NULL, WGSL_COMMON, WGSL_VIEW, NULL);
  if (!view_prog) return;
  unsigned buf = 0;
  float norm = 1;
  if (v == CW_VIEW_AO) {
    const cw_rt *t = cw_pipeline_rt_persistent(ctx->pipeline, "ssao.ao", (ctx->width + 1) / 2, (ctx->height + 1) / 2, CW_RT_RG16F, NULL);
    buf = t ? t->tex : 0;
  } else if (v == CW_VIEW_VELOCITY) buf = ctx->velocity ? ctx->velocity->tex : 0;
  else if (v == CW_VIEW_BLOOM) {
    /* what the bloom pass kept this frame, at its size and format (its chain's
     * finest summed level and the scene's format): asking for any other size
     * or format would recreate it empty between that write and this read */
    const cw_rt *t = cw__pipeline_rt_find(ctx->pipeline, "bloom.view");
    buf = t ? t->tex : 0;
    norm = 1.0f / (float)CW_CLAMP(ctx->settings->bloom_levels, 1, 10);
  }
  cw_pass_use(view_prog);
  set_pz(view_prog, ctx);
  cw_pass_bind_texture(0, ctx->depth_tex);
  cw_pass_bind_texture(1, buf);
  cw_pass_uniform1i(view_prog, "u_depth", 0);
  cw_pass_uniform1i(view_prog, "u_buf", 1);
  cw_pass_uniform_mat4(view_prog, "u_inv_proj", &ctx->pipeline->inv_proj_jittered);
  cw_pass_uniform2f(view_prog, "u_size", (float)ctx->width, (float)ctx->height);
  cw_pass_uniform1i(view_prog, "u_view", (int)v);
  cw_pass_uniform1f(view_prog, "u_norm", norm);
  cw_pass_fullscreen();
}

/* ── the tree's 2D pass and its UI overlays (scene2d.h) ──────────────── */

static void canvas_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  /* clears first when no 3D was drawn this frame (a 2D-only tree) */
  cw_canvas_draw(ctx->tree, ctx->output->w, ctx->output->h, ctx->alpha, !ctx->pipeline->drew3d);
  if (ctx->renderer) t3_renderer_reset_bindings(ctx->renderer);
}

/* ── the app's render callback ──────────────────────────────────────── */

static bool app_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  return ctx->pipeline->app_render != NULL;
}

static void app_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  cw_pipeline *p = ctx->pipeline;
  p->app_render(p->app_user, ctx->alpha);
  /* its GL calls (render2d) invalidate three.c's state cache */
  if (ctx->renderer) t3_renderer_reset_bindings(ctx->renderer);
}

/* ── registration ───────────────────────────────────────────────────── */

void cw__pipeline_add_builtin_passes(cw_pipeline *p) {
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "depth_prepass", .phase = CW_PHASE_PREPASS, .order = 0,
                                           .active = prepass_active, .execute = prepass_exec });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "scene", .phase = CW_PHASE_OPAQUE, .order = 0,
                                           .writes = CW_RES_COLOR | CW_RES_DEPTH, .execute = scene_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "velocity", .phase = CW_PHASE_POST, .order = 0,
                                           .reads = CW_RES_DEPTH, .writes = CW_RES_VELOCITY, .execute = velocity_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "ssao", .phase = CW_PHASE_POST, .order = 100,
                                           .reads = CW_RES_COLOR | CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                           .active = ssao_active, .execute = ssao_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "taa", .phase = CW_PHASE_POST, .order = 200,
                                           .reads = CW_RES_COLOR | CW_RES_VELOCITY, .writes = CW_RES_COLOR,
                                           .active = taa_active, .execute = taa_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "bloom", .phase = CW_PHASE_POST, .order = 300,
                                           .reads = CW_RES_COLOR, .writes = CW_RES_COLOR,
                                           .active = bloom_active, .execute = bloom_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "tonemap", .phase = CW_PHASE_POST, .order = 500,
                                           .reads = CW_RES_COLOR, .writes = CW_RES_COLOR, .ldr_output = true,
                                           .active = tonemap_active, .execute = tonemap_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "grade", .phase = CW_PHASE_POST, .order = 550,
                                           .reads = CW_RES_COLOR, .writes = CW_RES_COLOR,
                                           .active = grade_active, .execute = grade_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "aa", .phase = CW_PHASE_POST, .order = 600,
                                           .reads = CW_RES_COLOR, .writes = CW_RES_COLOR,
                                           .active = aa_active, .execute = aa_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "lens", .phase = CW_PHASE_POST, .order = 700,
                                           .reads = CW_RES_COLOR, .writes = CW_RES_COLOR,
                                           .active = lens_active, .execute = lens_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "debug_view", .phase = CW_PHASE_POST, .order = 10000,
                                           .reads = CW_RES_DEPTH, .writes = CW_RES_COLOR, .ldr_output = true,
                                           .active = view_active, .execute = view_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "canvas", .phase = CW_PHASE_OVERLAY, .order = -100,
                                           .reads = CW_RES_OUTPUT, .writes = CW_RES_OUTPUT, .execute = canvas_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "app", .phase = CW_PHASE_OVERLAY, .order = 0,
                                           .reads = CW_RES_OUTPUT, .writes = CW_RES_OUTPUT,
                                           .active = app_active, .execute = app_exec, .webgpu = true });
}
