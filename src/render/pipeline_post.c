/* The built-in passes of the render pipeline (pipeline.h): depth prepass,
 * scene, velocity, GTAO, TAA, bloom, tonemap, LUT grade, FXAA / SMAA, lens
 * effects, and the app's overlay. Every shader is GLSL ES 3.00 with no
 * derivatives (hosts disagree on them) and integer noise (bit-identical on
 * every GPU). */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/ui.h"
#include "pipeline_internal.h"
#include "quality_internal.h"
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

/* CW_TEST_PREPASS_SHIFT=1 (native test runs): the prepass writes its depth squeezed into 0..0.5, so the colour pass
 * fails its depth test where the prepass drew (the control of tests/split-gpu-share.mjs: the picture must change) */
static bool prepass_shift(void) {
#if CW_CART
  return false;
#else
  static int v = -1;
  if (v < 0) v = getenv("CW_TEST_PREPASS_SHIFT") && atoi(getenv("CW_TEST_PREPASS_SHIFT")) ? 1 : 0;
  return v == 1;
#endif
}

/* GL: the prepass draws into an FBO that holds only the scene target's depth. With the scene's colour attachment
 * bound and colour writes masked off, Mesa's radeonsi still runs much of each colour program's fragment work (the
 * sun's cascaded shadow filter above all: Ember Throne's meadow prepass 0.50 ms on the 890M, 0.35 with the sun's
 * shadow off), where WebGPU's pipelines know at creation that no colour is written (0.34 either way). With no colour
 * buffer at all the driver skips that work. The depth written is the same, so the picture is too.
 * CW_TEST_PREPASS_COLOR=1 (native test runs) draws into the scene target as before: the timing control. */
static bool prepass_color(void) {
#if CW_CART
  return false;
#else
  static int v = -1;
  if (v < 0) v = getenv("CW_TEST_PREPASS_COLOR") && atoi(getenv("CW_TEST_PREPASS_COLOR")) ? 1 : 0;
  return v == 1;
#endif
}
static unsigned prepass_depth_fbo(const cw_pipeline *p) {
  static unsigned fbo;
  if (prepass_color() || (!p->bound_depth_tex && !p->bound_depth_rb)) return p->bound_fbo;
  if (!fbo) {
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
  } else
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  /* attached every time: the scene target may be another one (split screen, a resize, MSAA on or off) */
  if (p->bound_depth_rb) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, p->bound_depth_rb);
  else glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, p->bound_depth_tex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return p->bound_fbo;
  return fbo;
}

static void prepass_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  t3_renderer *r = ctx->renderer;
#if CW_WGPU && defined(T3_WGPU_DEPTH_PREPASS)
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    /* the scene target's depth (multisampled with MSAA), cleared here and drawn by the colour programs with no colour
     * written; the colour is cleared too and never resolved (the scene pass clears it again and loads this depth) */
    const cw__wgpu_pass_targets *pt = cw__pass_wgpu_targets(ctx);
    if (!pt || !pt->color) return;
    float a = 1;
    t3_color c = t3_renderer_get_clear_color(r, &a);
    t3_external_target et = {
      .color = (uintptr_t)pt->color, .color_resolve = (uintptr_t)pt->resolve, .depth = (uintptr_t)pt->depth,
      .color_format = (int)pt->color_format, .depth_format = (int)pt->depth_format, .samples = pt->samples,
      .load_color = false, .load_depth = false, .clear_rgba = { c.r, c.g, c.b, a }, .clear_depth = cw_gpu_reversed_depth() ? 0.0f : 1.0f,
      .x = 0, .y = 0, .w = pt->w, .h = pt->h, .flip_y = pt->flip_y, .keep_multisampled = true,
    };
    /* (a one-pass split screen drawn view by view: the views before this one kept) */
    if (ctx->pipeline->arr_part > 1) et.load_color = et.load_depth = true;
    if (prepass_shift()) et.depth_min = cw_gpu_reversed_depth() ? 0.5f : 0, et.depth_max = cw_gpu_reversed_depth() ? 1 : 0.5f;
    t3_renderer_set_external_encoder(r, (uintptr_t)pt->enc);
    t3_renderer_set_external_target(r, &et);
    cw__pw_timing_span((void *)pt->enc, false);
    t3_renderer_render_prepass(r, ctx->scene3d, ctx->camera);
    cw__pw_timing_span((void *)pt->enc, true);
    t3_renderer_set_external_target(r, NULL);
    cw__render3d_count(ctx->tree);
    return;
  }
#endif
  t3_renderer_reset_bindings(r);
  t3_renderer_set_framebuffer(r, prepass_depth_fbo(ctx->pipeline));
  /* Blended/transmissive surfaces must not occlude their backgrounds before
   * the colour pass. Opaque alpha-tested cutouts retain their normal discard. */
  if (prepass_shift()) glDepthRangef(0, 0.5f);
  t3_renderer_render_prepass(r, ctx->scene3d, ctx->camera);
  if (prepass_shift()) glDepthRangef(0, 1);
  cw__render3d_count(ctx->tree);
}

/* ── scene ──────────────────────────────────────────────────────────── */

static void scene_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  t3_renderer *r = ctx->renderer;
#if CW_WGPU
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    /* three.c's WebGPU renderer (three.js r187's generated WGSL) records its
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
      .load_color = false, .load_depth = ctx->pipeline->prepass_ran, /* a depth prepass drew it */
      .clear_rgba = { c.r, c.g, c.b, a }, .clear_depth = cw_gpu_reversed_depth() ? 0.0f : 1.0f,
      .x = 0, .y = 0, .w = pt->w, .h = pt->h, .flip_y = pt->flip_y,
#ifdef T3_EXTERNAL_TARGET_KEEP_MULTISAMPLED
      .keep_multisampled = ctx->pipeline->msaa_keep, /* the viewmodel pass resolves (pipeline.c) */
#endif
    };
    /* a one-pass split screen drawn view by view (pipeline.c arr_part): the views before this one kept, and the
     * samples kept for the next view (the last view's opaque passes resolve) */
    const cw_pipeline *pl = ctx->pipeline;
    if (pl->arr_part > 1) et.load_color = et.load_depth = true;
#ifdef T3_EXTERNAL_TARGET_KEEP_MULTISAMPLED
    if (pl->arr_part && pl->arr_part < pl->arr_n) et.keep_multisampled = true;
#endif
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

/* ── viewmodel (a first person weapon: its own lens, over the scene) ──────
 * Camera3D viewmodel_layers are drawn after the scene by a second camera at the
 * same pose with viewmodel_fov, into the same colour and depth. The pass uses
 * the near end of the depth range (0 to VIEWMODEL_DEPTH): the weapon always
 * lands in front of the world and sorts against itself, and the scene's depth
 * stays right everywhere else for the passes that read it (AO, decals, particles). */
#define VIEWMODEL_DEPTH 0.02f

static bool viewmodel_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  if (!ctx->camera || !ctx->scene3d) return false;
  if (ctx->view_count > 0) { /* one-pass split screen: any view with viewmodel layers */
    for (int k = 0; k < ctx->view_count && k < ctx->pipeline->nviews; k++)
      if (cw__render3d_viewmodel_camera_of(ctx->pipeline->views[k].camera)) return true;
    return false;
  }
  return cw__render3d_viewmodel_camera(ctx->tree) != NULL;
}

/* one-pass split screen: the view's viewmodel camera drawn inside its rectangle, through an array camera of one
 * (its viewport placed as the scene pass places the view's) */
static t3_array_camera *vm_arr;
static t3_camera *viewmodel_view_camera(cw_pass_ctx *ctx, t3_camera *vc) {
  if (ctx->view_count <= 0) return vc;
  if (!vm_arr) vm_arr = t3_array_camera_new();
  for (int i = 0; i < vm_arr->count; i++) t3_release(vm_arr->cameras[i]);
  vm_arr->count = 0;
  const int *r = ctx->view_rect;
  t3_array_camera_add(vm_arr, vc, r[0], cw_gpu_backend() == CW_GPU_WEBGPU ? r[1] : ctx->height - r[1] - r[3], r[2], r[3]);
  return &vm_arr->base;
}

static void viewmodel_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  t3_renderer *r = ctx->renderer;
  t3_camera *vc = ctx->view_count > 0 ? cw__render3d_viewmodel_camera_of(ctx->camera_node) : cw__render3d_viewmodel_camera(ctx->tree);
  if (!vc || !r) return;
  vc = viewmodel_view_camera(ctx, vc);
  /* the scene's background (a clear colour, or a sky cube / texture drawn without a depth test) would cover the
   * whole picture the scene pass drew: the viewmodel pass draws none of it */
  bool had_bg = ctx->scene3d->has_background;
  struct t3_texture *bg_tex = ctx->scene3d->background_texture;
  ctx->scene3d->has_background = false;
  ctx->scene3d->background_texture = NULL;
#if CW_WGPU
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    const cw__wgpu_pass_targets *pt = cw__pass_wgpu_targets(ctx);
    if (!pt || !pt->color) {
      ctx->scene3d->has_background = had_bg;
      ctx->scene3d->background_texture = bg_tex;
      return;
    }
    t3_external_target et = {
      .color = (uintptr_t)pt->color, .color_resolve = (uintptr_t)pt->resolve, .depth = (uintptr_t)pt->depth,
      .color_format = (int)pt->color_format, .depth_format = (int)pt->depth_format, .samples = pt->samples,
      .load_color = true, .load_depth = true, .clear_depth = cw_gpu_reversed_depth() ? 0.0f : 1.0f,
      .x = 0, .y = 0, .w = pt->w, .h = pt->h, .flip_y = pt->flip_y,
      .depth_min = cw_gpu_reversed_depth() ? 1 - VIEWMODEL_DEPTH : 0, .depth_max = cw_gpu_reversed_depth() ? 1 : VIEWMODEL_DEPTH,
    };
#ifdef T3_EXTERNAL_TARGET_KEEP_MULTISAMPLED
    /* one-pass split screen: the samples kept for the next view's gun, the last view's pass resolves */
    et.keep_multisampled = ctx->view_count > 0 && ctx->view_index < ctx->view_count - 1;
#endif
    t3_renderer_set_external_encoder(r, (uintptr_t)pt->enc);
    t3_renderer_set_external_target(r, &et);
    cw__pw_timing_span((void *)pt->enc, false);
    t3_renderer_render(r, ctx->scene3d, vc);
    cw__pw_timing_span((void *)pt->enc, true);
    t3_renderer_set_external_target(r, NULL);
    ctx->scene3d->has_background = had_bg;
    ctx->scene3d->background_texture = bg_tex;
    cw__render3d_count(ctx->tree);
    return;
  }
#endif
  t3_renderer_reset_bindings(r);
  t3_renderer_set_framebuffer(r, ctx->pipeline->bound_fbo);
  bool auto_clear = t3_renderer_get_auto_clear(r);
  t3_renderer_set_auto_clear(r, false); /* keep the picture and the depth */
  glDepthRangef(0, VIEWMODEL_DEPTH);
  t3_renderer_render(r, ctx->scene3d, vc);
  glDepthRangef(0, 1);
  ctx->scene3d->has_background = had_bg;
  ctx->scene3d->background_texture = bg_tex;
  t3_renderer_set_auto_clear(r, auto_clear);
  cw__render3d_count(ctx->tree);
}

/* ── velocity (camera motion from depth, then the meshes that moved: velocity.c) ── */

static unsigned vel_prog;
static const char *VEL_FS = S(
  uniform sampler2D u_depth;
  uniform mat4 u_inv_vp_j;   /* this frame, jittered (what the depth was drawn with) */
  uniform mat4 u_vp;         /* this frame, unjittered */
  uniform mat4 u_prev_vp;    /* last frame, unjittered */
  uniform float u_vm_depth;  /* window depth below which a pixel is the viewmodel's (0: none) */
  out vec4 o;
  void main() {
    float d = texture(u_depth, v_uv).r;
    /* the first person viewmodel is drawn by its own camera and stays put on the screen */
    if (d < u_vm_depth) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }
    vec4 w = u_inv_vp_j * vec4(v_uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    w /= w.w;
    vec4 c = u_vp * w, p = u_prev_vp * w;
    o = vec4((c.xy / c.w - p.xy / p.w) * 0.5, 0.0, 1.0);
  });

static void velocity_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  if (!vel_prog) vel_prog = prog_of("velocity", VEL_FS, NULL, NULL, WGSL_COMMON, WGSL_VEL, NULL);
  /* two targets by frame parity: this frame's and the last one's (TAA reads both) */
  bool odd = (ctx->frame & 1) != 0, fresh = false;
  const cw_rt *v = cw_pipeline_rt_persistent(ctx->pipeline, odd ? "velocity.b" : "velocity", ctx->width, ctx->height, CW_RT_RG16F, NULL);
  const cw_rt *vp = cw_pipeline_rt_persistent(ctx->pipeline, odd ? "velocity" : "velocity.b", ctx->width, ctx->height, CW_RT_RG16F, &fresh);
  if (!v || !vp || !vel_prog) return;
  ctx->pipeline->velocity_prev = !fresh && ctx->pipeline->velocity_frame + 1 == ctx->frame ? vp : NULL;
  ctx->pipeline->velocity_frame = ctx->frame;
  cw_pass_bind_target(ctx, v);
  cw_pass_use(vel_prog);
  set_pz(vel_prog, ctx);
  cw_pass_bind_texture(0, ctx->depth_tex);
  cw_pass_uniform1i(vel_prog, "u_depth", 0);
  cw_pass_uniform_mat4(vel_prog, "u_inv_vp_j", &ctx->pipeline->inv_view_proj_jittered);
  cw_pass_uniform_mat4(vel_prog, "u_vp", &ctx->view_proj);
  cw_pass_uniform_mat4(vel_prog, "u_prev_vp", &ctx->prev_view_proj);
  /* CW_TEST_VIEWMODEL_MOTION=camera (a gate control): the viewmodel's pixels take the camera's motion like the rest */
  const char *vm_test = cw_test_env("CW_TEST_VIEWMODEL_MOTION");
  bool vm_still = cw__render3d_viewmodel_camera(ctx->tree) && !(vm_test && !strcmp(vm_test, "camera"));
  cw_pass_uniform1f(vel_prog, "u_vm_depth", vm_still ? VIEWMODEL_DEPTH : 0.0f);
  cw_pass_fullscreen();
  cw__velocity_objects(ctx, v);
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
  uniform vec4 u_proj_scale;  /* proj[0][0], proj[1][1], full w, full h (the view's) */
  uniform vec4 u_vrect;       /* the view's corner in the depth, in the AO target (a one-pass split screen's; 0 otherwise) */
  uniform vec4 u_params;      /* radius, power, slices, steps */
  uniform int u_frame;
  out vec4 o;
  /* positions are rebuilt at the centre of the depth texel they read (a
   * half-res pixel centre sits between full-res texels) */
  vec3 view_pos_px(ivec2 q) {
    q = clamp(q, ivec2(0), ivec2(u_proj_scale.zw) - 1);
    float d = texelFetch(u_depth, q + ivec2(u_vrect.xy), 0).r;
    vec2 uv = (vec2(q) + 0.5) / u_proj_scale.zw;
    vec4 p = u_inv_proj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
  }
  vec3 view_pos(vec2 uv) { return view_pos_px(ivec2(floor(uv * u_proj_scale.zw))); }
  void main() {
    ivec2 fc = ivec2(gl_FragCoord.xy) - ivec2(u_vrect.zw);
    ivec2 q = fc * 2;
    vec2 uv = (vec2(q) + 0.5) / u_proj_scale.zw;
    float d = texelFetch(u_depth, q + ivec2(u_vrect.xy), 0).r;
    if (d >= 1.0) { o = vec4(1.0, 1e6, 0.0, 1.0); return; }
    vec3 P = view_pos_px(q);
    vec3 pr = view_pos_px(q + ivec2(1, 0)), pl = view_pos_px(q - ivec2(1, 0));
    vec3 pu = view_pos_px(q + ivec2(0, 1)), pd = view_pos_px(q - ivec2(0, 1));
    /* the smaller depth step of the two sides; at the frame's edge the side
     * outside it (clamped onto P) is never taken: its zero difference made
     * the normal NaN, which GL and WebGPU turn into different shades */
    ivec2 lim = ivec2(u_proj_scale.zw) - 1;
    bool xr = q.x >= lim.x || (q.x > 0 && abs(pl.z - P.z) <= abs(pr.z - P.z));
    bool yr = q.y >= lim.y || (q.y > 0 && abs(pd.z - P.z) <= abs(pu.z - P.z));
    vec3 dx = xr ? P - pl : pr - P;
    vec3 dy = yr ? P - pd : pu - P;
    vec3 N = normalize(cross(dx, dy));
    vec3 V = normalize(-P);
    if (dot(N, V) < 0.0) N = -N;
    float R = u_params.x;
    vec2 ruv = vec2(u_proj_scale.x, u_proj_scale.y) * 0.5 * R / max(-P.z, 1e-4);
    int slices = int(u_params.z), steps = int(u_params.w);
    /* 4x4 pattern of slice rotations and step offsets (exact on every GPU) */
    ivec2 ip = fc & 3;
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
  uniform vec4 u_view;     /* the view's rectangle in uv (a one-pass split screen's; 0, 0, 1, 1 otherwise) */
  uniform vec4 u_ao_rect;  /* and in the AO target's texels, x0, y0, x1, y1 (its texels outside are another view's) */
  out vec4 o;
  void main() {
    float d = texture(u_depth, v_uv).r;
    vec4 c = texture(u_src, v_uv);
    vec2 lq = (v_uv - u_view.xy) / u_view.zw;
    AO_FUSED_IN
    if (d >= 1.0) { o = c; return; }
    vec4 p = u_inv_proj * vec4(lq * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    float z = -p.z / p.w;
    vec2 base = v_uv * u_ao_size - 0.5;
    vec2 f = floor(base);
    float sum = 0.0, wsum = 0.0;
    for (int j = -1; j <= 2; j++)
      for (int i = -1; i <= 2; i++) {
        vec2 t = clamp(f + vec2(float(i), float(j)), u_ao_rect.xy, u_ao_rect.zw);
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
  /* a one-pass split screen's view: its rectangle in the depth and in the AO target (the views' AO side by side, as
   * each view's own would be) */
  int vr[4] = { 0, 0, ctx->width, ctx->height }, ar[4] = { 0, 0, hw, hh };
  if (ctx->view_count > 0) {
    memcpy(vr, ctx->view_rect, sizeof vr);
    cw__view_clip_rect(hw, hh, ar);
  }
  const t3_mat4 *ip = ctx->view_count > 0 ? &ctx->inv_proj : &ctx->pipeline->inv_proj_jittered;
  cw_pass_bind_target(ctx, ao);
  cw_pass_use(ao_prog);
  set_pz(ao_prog, ctx);
  cw_pass_bind_texture(0, ctx->depth_tex);
  cw_pass_uniform1i(ao_prog, "u_depth", 0);
  cw_pass_uniform_mat4(ao_prog, "u_inv_proj", ip);
  cw_pass_uniform4f(ao_prog, "u_proj_scale", ctx->camera->projection_matrix.e[0], ctx->camera->projection_matrix.e[5],
                    (float)vr[2], (float)vr[3]);
  cw_pass_uniform4f(ao_prog, "u_vrect", (float)vr[0], (float)vr[1], (float)ar[0], (float)ar[1]);
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
  {
    float vu[4];
    cw_pass_view_uv(ctx, vu);
    cw_pass_uniform4f(comp, "u_view", vu[0], vu[1], vu[2], vu[3]);
    cw_pass_uniform4f(comp, "u_ao_rect", (float)ar[0], (float)ar[1], (float)(ar[0] + ar[2] - 1), (float)(ar[1] + ar[3] - 1));
  }
  if (fog) cw__fog_fused_uniforms(ctx, comp); /* the fog pass's own uniforms (fog.c) */
  cw_pass_fullscreen();
}

/* ── TAA ────────────────────────────────────────────────────────────────
 * History reprojected by the velocity target, sampled with a 5-tap
 * Catmull-Rom from where the nearest surface of the 3x3 neighbourhood was, clipped toward the 3x3 neighbourhood's YCoCg variance box;
 * luma-weighted blend (stable HDR highlights). The history alternates
 * between two persistent targets and becomes the chain's image.
 *
 * Things that move: the frame is drawn with a sub-pixel jitter, and where the history is used little that
 * jitter would be in the picture (a mover would sit up to half a pixel off where it is). So a pixel that moves
 * takes this frame unjittered: a 4x4 Catmull-Rom of the jittered frame at the pixel's own centre, in a
 * tonemapped space (c / (1 + max c), so a bright edge does not grow) and held inside its 2x2 neighbours (no
 * ringing). Its history weight falls with its speed (u_motion.x per pixel, down to u_motion.y): a history
 * resampled every frame softens what moves, and a turning thing's shading moves on. A pixel whose history
 * surface left (the last frame's velocity there is not this pixel's: the ground a mover uncovered) drops its
 * history. Still pixels are as before: the jittered frame into a full-weight history. */
static unsigned taa_prog;
static const char *TAA_FS = S(
  uniform sampler2D u_src;
  uniform sampler2D u_hist;
  uniform sampler2D u_vel;
  uniform sampler2D u_depth;
  uniform sampler2D u_vel_prev;
  uniform int u_has_prev;
  uniform vec2 u_size;
  uniform vec2 u_jit;     /* this frame's projection jitter, pixels */
  uniform vec4 u_motion;  /* x: history weight falloff per pixel of motion, y: its floor, z: unjittered frame per pixel of motion,
                           * w: disocclusion, the velocity change allowed as a share of the speed */
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
  /* this frame at the pixel's centre without the jitter: the jittered texel t shows what is at t + jitter */
  vec3 unjittered() {
    vec2 pos = v_uv * u_size - u_jit - 0.5;
    vec2 b0 = floor(pos), f = pos - b0;
    vec2 w[4];
    w[0] = f * (-0.5 + f * (1.0 - 0.5 * f));
    w[1] = 1.0 + f * f * (-2.5 + 1.5 * f);
    w[2] = f * (0.5 + f * (2.0 - 1.5 * f));
    w[3] = f * f * (-0.5 + 0.5 * f);
    vec3 acc = vec3(0.0), lo = vec3(1e9), hi = vec3(-1e9);
    ivec2 top = ivec2(u_size) - 1;
    for (int j = 0; j < 4; j++)
      for (int i = 0; i < 4; i++) {
        vec3 s = texelFetch(u_src, clamp(ivec2(b0) + ivec2(i - 1, j - 1), ivec2(0), top), 0).rgb;
        s = s / (1.0 + max(s.r, max(s.g, s.b)));
        acc += (w[i].x * w[j].y) * s;
        if (i >= 1 && i <= 2 && j >= 1 && j <= 2) { lo = min(lo, s); hi = max(hi, s); }
      }
    acc = clamp(acc, lo, hi);
    return acc / max(1.0 - max(acc.r, max(acc.g, acc.b)), 1e-4);
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
    /* the motion of the nearest surface in the 3x3 neighbourhood: a moving object's antialiased edge follows
     * the object, on both sides of the edge */
    vec2 near_uv = v_uv;
    float near_d = texture(u_depth, v_uv).r;
    for (int j = -1; j <= 1; j++)
      for (int i = -1; i <= 1; i++) {
        vec2 q = v_uv + vec2(float(i), float(j)) * px;
        float d = texture(u_depth, q).r;
        if (d < near_d) { near_d = d; near_uv = q; }
      }
    vec2 vel = texture(u_vel, near_uv).xy;
    vec2 prev = v_uv - vel;
    float speed = length(vel * u_size);
    /* the history surface left: the last frame's motion where the history is read is not this pixel's */
    bool gone = false;
    if (u_has_prev != 0 && u_motion.w > 0.0) {
      vec2 vq = texture(u_vel_prev, prev).xy;
      gone = length((vel - vq) * u_size) > max(0.5, u_motion.w * max(speed, length(vq * u_size)));
    }
    float moving = u_motion.z < 0.0 ? 1.0 : clamp(speed * u_motion.z, 0.0, 1.0);
    vec3 now = cur.rgb;
    if (moving > 0.0 || gone) now = mix(cur.rgb, unjittered(), gone ? 1.0 : moving);
    vec3 c0 = to_ycocg(now);
    vec3 h;
    if (gone || any(lessThan(prev, vec2(0.0))) || any(greaterThan(prev, vec2(1.0)))) h = c0;
    else {
      h = to_ycocg(hist_catmull(prev));
      /* clip the history toward the box centre (not a per-axis clamp) */
      vec3 ctr = 0.5 * (bmax + bmin), ext = 0.5 * (bmax - bmin) + 1e-5;
      vec3 v = h - ctr;
      vec3 a = abs(v / ext);
      float m = max(a.x, max(a.y, a.z));
      if (m > 1.0) h = ctr + v / m;
    }
    float fb = mix(u_feedback, min(u_feedback, u_motion.y), u_motion.x < 0.0 ? 1.0 : clamp(speed * u_motion.x, 0.0, 1.0));
    float wc = (1.0 - fb) / (1.0 + c0.x), wh = fb / (1.0 + h.x);
    vec3 r = (c0 * wc + h * wh) / (wc + wh);
    o = vec4(from_ycocg(r), cur.a);
  });

static bool taa_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  return ctx->settings->taa;
}

/* the moving-pixel terms (the TAA comment above), measured by tests/motion-vectors.mjs against a 36-sample picture.
 * Its controls: CW_TEST_TAA_STILL_ONLY=1 turns them off (TAA as it was before them); CW_TEST_TAA_STILL_ONLY=soft treats
 * every pixel as moving, still ones too: the unjittered frame and the low history weight (the loss its still-picture
 * check must catch) */
static void taa_motion(float m[4]) {
  m[0] = 1.0f, m[1] = 0.25f, m[2] = 2.0f, m[3] = 0.25f;
  const char *t = cw_test_env("CW_TEST_TAA_STILL_ONLY");
  if (t && !strcmp(t, "1")) m[0] = m[2] = m[3] = 0;
  if (t && !strcmp(t, "soft")) m[0] = m[2] = -1;
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
  const cw_rt *vprev = ctx->pipeline->velocity_prev;
  float m[4];
  taa_motion(m);
  cw_pass_bind_target(ctx, dst);
  cw_pass_use(taa_prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, hist->tex);
  cw_pass_bind_texture(2, ctx->velocity->tex);
  cw_pass_uniform1i(taa_prog, "u_src", 0);
  cw_pass_uniform1i(taa_prog, "u_hist", 1);
  cw_pass_uniform1i(taa_prog, "u_vel", 2);
  cw_pass_bind_texture(3, ctx->depth_tex);
  cw_pass_uniform1i(taa_prog, "u_depth", 3);
  cw_pass_bind_texture(4, vprev ? vprev->tex : ctx->velocity->tex);
  cw_pass_uniform1i(taa_prog, "u_vel_prev", 4);
  cw_pass_uniform1i(taa_prog, "u_has_prev", vprev ? 1 : 0);
  cw_pass_uniform2f(taa_prog, "u_size", (float)ctx->width, (float)ctx->height);
  cw_pass_uniform2f(taa_prog, "u_jit", ctx->jitter[0], ctx->jitter[1]);
  cw_pass_uniform4f(taa_prog, "u_motion", m[0], m[1], m[2], m[3]);
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
static unsigned bl_down_prog, bl_up_prog, bl_comp_prog, bl_down4_prog;
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
/* the light first level (effects detail Low): the four bilinear taps of BL_DOWN_FS's inner box (a 4x4 texel footprint),
 * prefiltered and Karis-averaged, a third of the full first level's reads */
static const char *BL_DOWN4_FS = S(
  uniform sampler2D u_src;
  uniform vec2 u_texel;
  out vec4 o;
  void main() {
    vec2 t = u_texel;
    vec3 D = prefilter(texture(u_src, v_uv + t * vec2(-1.0, 1.0)).rgb), E = prefilter(texture(u_src, v_uv + t * vec2(1.0, 1.0)).rgb);
    vec3 I = prefilter(texture(u_src, v_uv + t * vec2(-1.0, -1.0)).rgb), J = prefilter(texture(u_src, v_uv + t * vec2(1.0, -1.0)).rgb);
    float wd = 1.0 / (1.0 + luma(D)), we = 1.0 / (1.0 + luma(E)), wi = 1.0 / (1.0 + luma(I)), wj = 1.0 / (1.0 + luma(J));
    o = vec4((D * wd + E * we + I * wi + J * wj) / (wd + we + wi + wj), 1.0);
  });
static const char *WGSL_BL_DOWN4 =
    "struct U { u_thresh: vec4f, u_texel: vec2f };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@group(0) @binding(1) var u_src: texture_2d<f32>;\n"
    "fn tp(p: vec2f) -> vec3f { return prefilter(textureSampleLevel(u_src, cw_linear, p, 0.0).rgb); }\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  let t = u.u_texel;\n"
    "  let D = tp(uv + t * vec2f(-1.0, 1.0)); let E = tp(uv + t * vec2f(1.0, 1.0));\n"
    "  let I = tp(uv + t * vec2f(-1.0, -1.0)); let J = tp(uv + t * vec2f(1.0, -1.0));\n"
    "  let wd = 1.0 / (1.0 + luma(D)); let we = 1.0 / (1.0 + luma(E)); let wi = 1.0 / (1.0 + luma(I)); let wj = 1.0 / (1.0 + luma(J));\n"
    "  return vec4f((D * wd + E * we + I * wi + J * wj) / (wd + we + wi + wj), 1.0);\n"
    "}\n";
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
  bool light = cw__quality_light_bloom();
  if (light && !bl_down4_prog) {
    char w[4096];
    snprintf(w, sizeof w, "%s%s", WGSL_BL_DOWN4, WGSL_BL_PREFILTER);
    bl_down4_prog = prog_of("bloom.down4", GLSL_COMMON, BL_PREFILTER, BL_DOWN4_FS, WGSL_COMMON, w, NULL);
  }
  if (light && !bl_down4_prog) light = false;
  if (!bl_down_prog || !bl_up_prog || !bl_comp_prog) return;
  cw_rt_format f = ctx->src->format == CW_RT_RGBA8 || ctx->src->format == CW__RT_RG11B10F ? ctx->src->format : CW_RT_RGBA16F;
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
  for (int i = 0; i < levels; i++) {
    w = (w + 1) / 2;
    h = (h + 1) / 2;
    if (w < 2 || h < 2) break;
    down[i] = cw_pass_rt(ctx, w, h, f, false);
    if (!down[i]) break;
    /* the first level: the light one on effects detail Low (bloom.down4), else the 13-tap one */
    unsigned prog = i == 0 && light ? bl_down4_prog : bl_down_prog;
    if (i == 0 || (i == 1 && light)) {
      cw_pass_use(prog);
      cw_pass_uniform1i(prog, "u_src", 0);
      bloom_thresh(prog, s);
    }
    cw_pass_bind_target(ctx, down[i]);
    cw_pass_bind_texture(0, prev->tex);
    cw_pass_uniform2f(prog, "u_texel", 1.0f / (float)prev->w, 1.0f / (float)prev->h);
    if (prog == bl_down_prog) cw_pass_uniform1i(prog, "u_first", i == 0);
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

/* ── automatic exposure (eye adaptation) ────────────────────────────────
 * The picture's average log2 luminance (the geometric mean, which a few very bright pixels do not drag) through a
 * chain of 8x reductions (the first reads the frame four times a texel, the rest every texel), then one 1x1 pass that moves the adaptation state toward
 *   goal = clamp(log2(key) - average, min_ev, max_ev)   (stops)
 * by 1 - e^(-dt x speed): the speed toward a brighter picture when the goal is below the state, else the speed toward
 * a darker one. The state stays on the GPU (two 1x1 RG16F targets, r + g / 1024 stops, alternating by frame) and the
 * tonemap pass reads it, so nothing is read back: no stall, no latency that differs by host. dt is the frame's time
 * on the engine clock (the fixed step in a deterministic run), so every host makes the same sequence. A new state
 * (the first frame, cw_pipeline_exposure_reset) starts at its goal. */
static unsigned exp_log_prog, exp_adapt_prog;
static const char *EXP_LOG_FS = S(
  uniform sampler2D u_src;
  uniform vec2 u_texel;
  uniform int u_first;
  out vec4 o;
  void main() {
    float s = 0.0;
    if (u_first != 0) {
      /* the frame itself: four reads spread over the 8 x 8 block (each the mean of 2 x 2 pixels) */
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          vec4 c = texture(u_src, v_uv + (vec2(float(i), float(j)) * 4.0 - 2.0) * u_texel);
          s += log2(max(dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)), 1.0 / 65536.0)) * 4.0;
        }
    } else {
      for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) s += texture(u_src, v_uv + (vec2(float(i), float(j)) * 2.0 - 3.0) * u_texel).r;
    }
    o = vec4(clamp(s / 16.0, -16.0, 16.0), 0.0, 0.0, 1.0);
  });
static const char *EXP_ADAPT_FS = S(
  uniform sampler2D u_avg;
  uniform sampler2D u_prev;
  uniform vec4 u_p; /* key, min stops, max stops, 1: start at the goal */
  uniform vec4 u_k; /* this frame's blend toward a brighter picture, toward a darker one */
  out vec4 o;
  void main() {
    float avg = texelFetch(u_avg, ivec2(0), 0).r;
    float goal = clamp(log2(u_p.x) - avg, u_p.y, u_p.z);
    vec2 pr = texelFetch(u_prev, ivec2(0), 0).rg;
    float prev = pr.r + pr.g / 1024.0;
    float ev = u_p.w > 0.5 ? goal : prev + (goal - prev) * (goal < prev ? u_k.x : u_k.y);
    float hi = floor(ev * 16.0 + 0.5) / 16.0;
    o = vec4(hi, (ev - hi) * 1024.0, 0.0, 1.0);
  });

/* a 1x1 state of 0 stops, for the tonemap pass when automatic exposure is off */
static unsigned exposure_neutral(void) {
  static unsigned tex;
  if (!tex) {
    static const uint16_t zero[2] = { 0, 0 };
    tex = cw_pass_texture_new(1, 1, CW_RT_RG16F, zero);
  }
  return tex;
}

static bool exposure_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  return ctx->settings->auto_exposure && cw__caps.float_render;
}

/* 1 - e^(-x) for a frame's small x, from + - * / alone (the same bits on every host) */
static float adapt_blend(float x) {
  x = CW_CLAMP(x, 0.0f, 4.0f);
  return 1.0f - 1.0f / (1.0f + x + x * x * 0.5f + x * x * x * (1.0f / 6.0f) + x * x * x * x * (1.0f / 24.0f));
}

static void exposure_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_post_settings *s = ctx->settings;
  cw_pipeline *pl = ctx->pipeline;
  if (!exp_log_prog) exp_log_prog = prog_of("exposure_average", EXP_LOG_FS, NULL, NULL, WGSL_EXP_LOG, NULL, NULL);
  if (!exp_adapt_prog) exp_adapt_prog = prog_of("exposure_adapt", EXP_ADAPT_FS, NULL, NULL, WGSL_EXP_ADAPT, NULL, NULL);
  if (!exp_log_prog || !exp_adapt_prog || !ctx->src) return;
  const cw_rt *cur = ctx->src;
  int w = cur->w, h = cur->h;
  bool first = true;
  do {
    w = CW_MAX(1, (w + 7) / 8);
    h = CW_MAX(1, (h + 7) / 8);
    const cw_rt *rt = cw_pass_rt(ctx, w, h, CW_RT_R16F, false);
    if (!rt) return;
    cw_pass_bind_target(ctx, rt);
    cw_pass_use(exp_log_prog);
    cw_pass_bind_texture(0, cur->tex);
    cw_pass_uniform1i(exp_log_prog, "u_src", 0);
    cw_pass_uniform2f(exp_log_prog, "u_texel", 1.0f / (float)cur->w, 1.0f / (float)cur->h);
    cw_pass_uniform1i(exp_log_prog, "u_first", first ? 1 : 0);
    cw_pass_fullscreen();
    if (!first) cw_pass_rt_release(ctx, cur);
    cur = rt;
    first = false;
  } while (w > 1 || h > 1);
  /* the state: each split screen view its own (they look at different things) */
  char ka[32], kb[32];
  int view = pl->view_i >= 0 ? pl->view_i : 0;
  snprintf(ka, sizeof ka, "exposure.%d.a", view);
  snprintf(kb, sizeof kb, "exposure.%d.b", view);
  bool fresh_a = false, fresh_b = false;
  const cw_rt *a = cw_pipeline_rt_persistent(pl, ka, 1, 1, CW_RT_RG16F, &fresh_a);
  const cw_rt *b = cw_pipeline_rt_persistent(pl, kb, 1, 1, CW_RT_RG16F, &fresh_b);
  if (!a || !b) {
    cw_pass_rt_release(ctx, cur);
    return;
  }
  bool odd = (ctx->frame & 1) != 0;
  const cw_rt *dst = odd ? a : b, *prev = odd ? b : a;
  /* (a state not updated last frame, the pass off for a while, starts over at its goal) */
  if (pl->auto_exposure_reset) {
    pl->auto_exposure_reset = false;
    for (int i = 0; i < CW_MAX_VIEWS; i++) pl->auto_exposure_seen[i] = UINT64_MAX - 1;
  }
  bool fresh = fresh_a || fresh_b || pl->auto_exposure_seen[view] + 1 != ctx->frame;
  pl->auto_exposure_seen[view] = ctx->frame;
  float dt = (float)CW_CLAMP(cw_frame_dt(), 0.0, 0.1);
  float k_bright = adapt_blend(dt * CW_MAX(s->auto_exposure_speed_up, 0.0f)), k_dark = adapt_blend(dt * CW_MAX(s->auto_exposure_speed_down, 0.0f));
  /* CW_TEST_AUTO_EXPOSURE=snap (a gate control): no adaptation, the state jumps to its goal every frame */
  const char *t = cw_test_env("CW_TEST_AUTO_EXPOSURE");
  if (t && !strcmp(t, "snap")) fresh = true;
  cw_pass_bind_target(ctx, dst);
  cw_pass_use(exp_adapt_prog);
  cw_pass_bind_texture(0, cur->tex);
  cw_pass_bind_texture(1, prev->tex);
  cw_pass_uniform1i(exp_adapt_prog, "u_avg", 0);
  cw_pass_uniform1i(exp_adapt_prog, "u_prev", 1);
  cw_pass_uniform4f(exp_adapt_prog, "u_p", CW_MAX(s->auto_exposure_key, 1e-4f), CW_MIN(s->auto_exposure_min_ev, s->auto_exposure_max_ev),
                    s->auto_exposure_max_ev, fresh ? 1.0f : 0.0f);
  cw_pass_uniform4f(exp_adapt_prog, "u_k", k_bright, k_dark, 0, 0);
  cw_pass_fullscreen();
  cw_pass_rt_release(ctx, cur);
  pl->auto_exposure_tex = dst->tex;
}

/* ── tonemap: exposure, tone curve, sRGB transfer ────────────────────── */

static const char *TM_FS = S(
  uniform sampler2D u_src;
  /* the exposure the curves use: the settings' times 2^(automatic exposure), read from the 1x1 adaptation state
   * (r + g / 1024 stops; a texture of zeros when automatic exposure is off) */
  uniform float u_tm_exposure;
  uniform sampler2D u_auto;
  float toneMappingExposure;
  void tm_exposure_set() { toneMappingExposure = u_tm_exposure * exp2(dot(texelFetch(u_auto, ivec2(0), 0).rg, vec2(1.0, 1.0 / 1024.0))); }
  uniform int u_curve;
  out vec4 o;
  /* three.js r187's curves (ToneMappingFunctions; the GLSL tonemapping_pars_fragment) */
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
  /* r187's sRGBTransferOETF */
  vec3 LinearTosRGB(vec3 v) {
    return mix(pow(v, vec3(0.41666)) * 1.055 - vec3(0.055), v * 12.92, vec3(lessThanEqual(v, vec3(0.0031308))));
  });
/* the tonemap pass's main, also the fused pass's middle (FINAL_FS) */
static const char *TM_MAIN = S(
  void main() {
    tm_exposure_set();
    vec4 c = TM_SRC;
    TM_FUSED_IN
    vec3 x = max(c.rgb, vec3(0.0));
    if (u_curve == 0) x = clamp(x * toneMappingExposure, 0.0, 1.0);
    else if (u_curve == 1) x = clamp(LinearToneMapping(x), 0.0, 1.0);
    else if (u_curve == 2) x = ReinhardToneMapping(x);
    else if (u_curve == 3) x = OptimizedCineonToneMapping(x);
    else if (u_curve == 4) x = ACESFilmicToneMapping(x);
    else if (u_curve == 5) x = AgXToneMapping(x);
    else x = clamp(NeutralToneMapping(x), 0.0, 1.0);
    /* r187 applies the output transfer after every curve: Cineon's pow(2.2) returns it to linear first */
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
  snprintf(g, sizeof g, "#define AO_FUSED_IN c = fog_over(c, fog_eval(lq, d));\n%s%s%s%s%s", cw__fusion_raw() ? "#define FUSE_RAW 1\n" : "", FUSE_ROUND, cw__fog_glsl(), FUSE_FOG, AO_COMP_FS);
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

/* the distortion apply (src/fx/distort.c's) as the tonemap pass's read of its source: the same guards, the
 * value rounded as the separate pass's RGBA16F target rounded it */
static const char *TM_DIST_GLSL =
    "uniform sampler2D u_dist, u_depth;\n"
    "uniform vec4 u_dp;    /* (unused), how much nearer than the haze a sample may be, max shift, on */\n"
    "uniform vec4 u_dsoft; /* near, far, ortho */\n"
    "float dlin(float d) { return u_dsoft.z > 0.5 ? u_dsoft.x + d * (u_dsoft.y - u_dsoft.x) : u_dsoft.x * u_dsoft.y / (u_dsoft.y - d * (u_dsoft.y - u_dsoft.x)); }\n"
    "vec4 distort_src(vec2 uv) {\n"
    "  vec4 s0 = texture(u_src, uv);\n"
    "  if (u_dp.w < 0.5) return s0;\n"
    "  vec4 d = texture(u_dist, uv);\n"
    "  if (d.a <= 1e-5) return s0;\n"
    "  float hz = d.b / d.a;\n"
    "  if (dlin(texture(u_depth, uv).r) < hz - u_dp.y) return s0;\n"
    "  vec2 off = clamp(d.rg, vec2(-u_dp.z), vec2(u_dp.z));\n"
    "  vec2 uv2 = uv + off;\n"
    "  if (dlin(texture(u_depth, uv2).r) < hz - u_dp.y) return s0;\n"
    "  vec3 c = texture(u_src, uv2).rgb;\n"
    "  return vec4(q16(c), s0.a);\n"
    "}\n";
static const char *TM_DIST_WGSL =
    "@group(0) @binding(4) var u_dist: texture_2d<f32>;\n"
    "@group(0) @binding(5) var u_depth: texture_2d<f32>;\n"
    "fn distort_src(uv: vec2f) -> vec4f {\n"
    "  let s0 = textureSampleLevel(u_src, cw_linear, uv, 0.0);\n"
    "  if (u.u_dp.w < 0.5) { return s0; }\n"
    "  let d = textureSampleLevel(u_dist, cw_linear, uv, 0.0);\n"
    "  if (d.a <= 1e-5) { return s0; }\n"
    "  let hz = d.b / d.a;\n"
    "  if (cw_load(u_depth, uv).r < hz - u.u_dp.y) { return s0; }\n"
    "  let off = clamp(d.rg, vec2f(-u.u_dp.z), vec2f(u.u_dp.z));\n"
    "  let uv2 = uv + off;\n"
    "  if (cw_load(u_depth, uv2).r < hz - u.u_dp.y) { return s0; }\n"
    "  let c = textureSampleLevel(u_src, cw_linear, uv2, 0.0).rgb;\n"
    "  return vec4f(q16(c), s0.a);\n"
    "}\n";

/* a copy of src with `from` replaced by `to` (once; the source must hold it) */
static const char *swap_once(char *buf, size_t cap, const char *src, const char *from, const char *to) {
  const char *at = strstr(src, from);
  if (!at) {
    snprintf(buf, cap, "%s", src);
    return buf;
  }
  snprintf(buf, cap, "%.*s%s%s", (int)(at - src), src, to, at + strlen(from));
  return buf;
}

static unsigned tm_cheap_variant(bool bloom, bool grade, bool lens, bool aa, bool fog);
/* the tonemap program: plain, or with bloom's composite, the grade, the lens and / or the distortion fused in, or FXAA
 * (tm_fxaa_variant) */
static unsigned tm_variant(bool bloom, bool grade, bool lens, bool dist, bool aa, bool fog) {
  static unsigned progs[16];
  int k = (bloom ? 1 : 0) | (grade ? 2 : 0) | (lens ? 4 : 0) | (dist ? 8 : 0);
  if (aa || fog) return tm_cheap_variant(bloom, grade, lens, aa, fog);
  if (progs[k]) return progs[k];
  static const char *const names[16] = { "tonemap", "tonemap+bloom", "tonemap+grade", "tonemap+bloom+grade",
                                         "tonemap+lens", "tonemap+bloom+lens", "tonemap+grade+lens", "tonemap+bloom+grade+lens",
                                         "tonemap+dist", "tonemap+bloom+dist", "tonemap+grade+dist", "tonemap+bloom+grade+dist",
                                         "tonemap+lens+dist", "tonemap+bloom+lens+dist", "tonemap+grade+lens+dist",
                                         "tonemap+bloom+grade+lens+dist" };
  if (!k)
    return progs[0] = prog_of("tonemap", "#define TM_SRC texture(u_src, v_uv)\n#define TM_FUSED_IN\n#define TM_FUSED_OUT\n#define TM_FUSED_LENS\n", TM_FS, TM_MAIN,
                              WGSL_TM, NULL, NULL);
  static char buf[49152];
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    static char head[4096], fs[1024];
    const char *h = dist ? swap_once(head, sizeof head, WGSL_TM_FUSED_HEAD, "u_frame: i32 };", "u_frame: i32, u_dp: vec4f, u_dsoft: vec4f };")
                         : WGSL_TM_FUSED_HEAD;
    const char *fsrc = dist ? swap_once(fs, sizeof fs, WGSL_TM_FUSED_FS, "textureSampleLevel(u_src, cw_linear, uv, 0.0)", "distort_src(uv)")
                            : WGSL_TM_FUSED_FS;
    snprintf(buf, sizeof buf, "%s%s%s%s%s%s%s%s%s%s%s%s", h, WGSL_COMMON, WGSL_BL_PREFILTER, cw__fuse_round_wgsl(),
             WGSL_FUSE_BLOOM_GRADE, WGSL_FUSE_LENS, WGSL_TM_FNS, dist ? TM_DIST_WGSL : "", fsrc, bloom ? "c = fuse_bloom(c, uv);\n" : "",
             WGSL_TM_FUSED_CURVES, "");
    /* the steps after the curve */
    size_t n = strlen(buf);
    snprintf(buf + n, sizeof buf - n, "%s  var o = vec4f(x, 1.0);\n%s  return o;\n}\n", grade ? "x = fuse_grade(x);\n" : "",
             lens ? "o = fuse_lens(o, uv, pos);\n" : "");
    return progs[k] = cw_pass_program_wgsl(names[k], buf);
  }
  snprintf(buf, sizeof buf, "#define TM_SRC %s\n#define TM_FUSED_IN %s\n#define TM_FUSED_OUT %s\n#define TM_FUSED_LENS %s\n%s%s%s%s%s%s%s%s%s",
           dist ? "distort_src(v_uv)" : "texture(u_src, v_uv)", bloom ? "c = fuse_bloom(c);" : "", grade ? "x = fuse_grade(x);" : "",
           lens ? "o = fuse_lens(o);" : "", cw__fusion_raw() ? "#define FUSE_RAW 1\n" : "", GLSL_COMMON, BL_PREFILTER, TM_FS, FUSE_ROUND,
           FUSE_FS, FUSE_LENS, dist ? TM_DIST_GLSL : "", TM_MAIN);
  return progs[k] = cw_pass_program(names[k], buf);
}

/* CW_TEST_BLOOM_UNIT=stale: the stale unit left as it was (the post-verylow-underwater gate's control) */
static bool stale_bloom_unit_test(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("CW_TEST_BLOOM_UNIT");
    v = e && !strcmp(e, "stale");
  }
  return v;
}

static void tonemap_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_post_settings *s = ctx->settings;
  cw_pipeline *pl = ctx->pipeline;
  bool bloom = pl->fuse_bloom && pl->bloom_glow, grade = pl->fuse_grade, lens = pl->fuse_lens_tm, dist = pl->fuse_distort;
  int n = 0;
  float dmin[3], dmax[3];
  unsigned lut = grade ? cw__lut_texture(s->grade_lut, &n, dmin, dmax) : 0;
  if (grade && !lut) grade = false;
  bool aa = pl->fuse_aa_tm, fog = pl->fuse_fog_tm;
  unsigned prog = tm_variant(bloom, grade, lens, dist, aa, fog);
  if (!prog) return;
  const cw_rt *dbuf = dist ? cw__fx_take_distortion(ctx->tree) : NULL;
  /* the exposure state before any texture is bound: the neutral one is made on first use, and making a texture
   * binds it, which would replace the unit already bound (the first frame came out black) */
  unsigned auto_tex = s->auto_exposure && pl->auto_exposure_tex ? pl->auto_exposure_tex : exposure_neutral();
  cw_pass_use(prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_uniform1i(prog, "u_src", 0);
  cw_pass_uniform1f(prog, "u_tm_exposure", s->exposure * exp2f(s->exposure_ev));
  /* unit 5 (WebGPU binding 6): the automatic exposure's state, or zeros (2^0) when it is off or did not run */
  cw_pass_bind_texture(5, auto_tex);
  cw_pass_uniform1i(prog, "u_auto", 5);
  pl->auto_exposure_tex = 0;
  cw_pass_uniform1i(prog, "u_curve", (int)CW_CLAMP((int)s->tonemap, 0, CW_TONEMAP_COUNT - 1));
  if (bloom) {
    cw_pass_bind_texture(1, pl->bloom_glow->tex);
    cw_pass_uniform1i(prog, "u_bloom", 1);
    cw_pass_uniform2f(prog, "u_texel", pl->bloom_texel[0], pl->bloom_texel[1]);
    cw_pass_uniform1f(prog, "u_radius", s->bloom_radius);
    cw_pass_uniform1f(prog, "u_k", CW_CLAMP(s->bloom_intensity, 0.0f, 1.0f));
    cw_pass_uniform1f(prog, "u_norm", pl->bloom_norm);
    bloom_thresh(prog, s);
  } else if (cw_gpu_backend() == CW_GPU_WEBGPU && !stale_bloom_unit_test())
    /* WebGPU: every fused variant declares u_bloom (binding 2, unit 1) and binds whatever that unit holds; with bloom
     * off (Very Low) it held an earlier pass's texture, the underwater view's depth, which a texture_2d<f32> binding
     * rejects. The empty unit binds a dummy 2D texture the program never samples. */
    cw_pass_bind_texture(1, 0);
  if (grade) grade_uniforms(ctx, prog, lut, n, dmin, dmax, 2);
  /* WebGPU: every fused variant declares u_lut (binding 3, unit 2) and binds
   * whatever that unit holds; without the grade it held an earlier pass's 2D
   * texture, which a texture_3d binding rejects. The empty unit binds a
   * dummy 3D texture the program never samples. */
  else if (cw_gpu_backend() == CW_GPU_WEBGPU) cw_pass_bind_texture_3d(2, 0);
  if (lens) cw__lens_uniforms(ctx, prog, false);
  if (aa) cw_pass_uniform2f(prog, "u_rcp", 1.0f / (float)ctx->src->w, 1.0f / (float)ctx->src->h);
  if (fog) { /* unit 4 (WebGPU binding 5): the depth, with the fog's uniforms (fog.c) */
    cw_pass_bind_texture(4, ctx->depth_tex ? ctx->depth_tex : ctx->src->tex);
    cw_pass_uniform1i(prog, "u_depth", 4);
    cw__fog_fused_uniforms(ctx, prog);
  }
  if (dist) { /* units 3 and 4 (WebGPU bindings 4 and 5): the buffer and the depth; none made this frame: off */
    cw_pass_bind_texture(3, dbuf ? dbuf->tex : ctx->src->tex);
    cw_pass_uniform1i(prog, "u_dist", 3);
    cw_pass_bind_texture(4, ctx->depth_tex ? ctx->depth_tex : ctx->src->tex);
    cw_pass_uniform1i(prog, "u_depth", 4);
    cw__fx_distortion_uniforms(ctx, prog, dbuf && ctx->depth_tex);
  }
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
/* (its luma: the display value's, or with FXAA fused into the tonemap pass an approximation of it from the HDR scene) */
#define FXAA_LUMA_DOT "#define FXAA_LUMA(c) dot(c, vec3(0.299, 0.587, 0.114))\n"

static unsigned fxaa_prog, fxaa_lens_prog;
static const char *FXAA_FS = S(
  uniform sampler2D u_src;
  uniform vec2 u_rcp;
  out vec4 o;
  float L(vec2 p) { return FXAA_LUMA(texture(u_src, p).rgb); }
  vec4 fxaa() {
    vec2 posM = v_uv;
    vec4 rgbyM = texture(u_src, posM);
    float lumaM = FXAA_LUMA(rgbyM.rgb);
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
  if (!fxaa_prog) fxaa_prog = prog_of("fxaa", "#define FXAA_FUSED_OUT\n" FXAA_LUMA_DOT, FXAA_FS, NULL, WGSL_FXAA_HEAD, WGSL_FXAA, WGSL_FXAA_FS);
  if (lens && !fxaa_lens_prog) {
    static char b[32768];
    if (cw_gpu_backend() == CW_GPU_WEBGPU) {
      snprintf(b, sizeof b, "%s%s%s%s%s%s", WGSL_FXAA_LENS_HEAD, WGSL_COMMON, cw__fuse_round_wgsl(), WGSL_FUSE_LENS, WGSL_FXAA,
               WGSL_FXAA_LENS_FS);
      fxaa_lens_prog = cw_pass_program_wgsl("fxaa+lens", b);
    } else {
      snprintf(b, sizeof b, "#define FXAA_FUSED_OUT o = fuse_lens(o);\n" FXAA_LUMA_DOT "%s%s%s%s%s", cw__fusion_raw() ? "#define FUSE_RAW 1\n" : "",
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

/* ── the tonemap pass on Very Low (effects detail Very Low; pipeline.c plan_fusion) ───────────────
 * The height fog is applied to the scene's HDR value as the pass reads it (the fog pass's blend, rounded as
 * its target rounded it), and FXAA runs inside the pass: its console variant (FXAA_CONSOLE_GLSL) reads the
 * HDR scene, its edge test on a display brightness estimated from each read (the exposed luminance through
 * x / (1 + x), then a square root: close to the tone curves' output in sRGB), and the colour it picks is
 * then tone mapped once, with the grade and the lens after it. Two full-frame passes fewer. Where FXAA
 * blends, it blends HDR values, so an edge against a bright sky stays a little harder than the separate
 * passes draw it; CW_TEST_NO_FUSION=1 runs the passes apart. */
static unsigned swap_must(char *buf, size_t cap, const char *src, const char *from, const char *to) {
  if (!strstr(src, from)) {
    cw_error("tonemap+fxaa: the FXAA source has no '%s'", from);
    return 0;
  }
  swap_once(buf, cap, src, from, to);
  return 1;
}
/* FXAA 3.11's console variant (FXAA_PC_CONSOLE) on the HDR scene: four half-texel diagonal reads and the centre,
 * then two or four reads along the edge, no search (about half the reads of the quality preset in flat areas, a third
 * at edges). Its edge test runs on dluma (a display brightness estimated from the HDR value); the colour is the centre
 * tone mapped (D), or at an edge the average of its reads (each after the fog and the glow, F) in a compressed space
 * (x / (1 + exposed luminance), undone exactly after the average) tone mapped once: the separate passes average display
 * values, which this is close to, with one run of the tone curve at an edge instead of four. */
static const char *FXAA_CONSOLE_GLSL = S(
  vec3 fxaa_disp() {
    vec2 pos = v_uv;
    vec3 rgbM = T(pos).rgb;
    float lumaM = dluma(rgbM);
    float lumaNw = dluma(T(pos + vec2(-0.5, -0.5) * u_rcp).rgb), lumaSw = dluma(T(pos + vec2(-0.5, 0.5) * u_rcp).rgb);
    float lumaNe = dluma(T(pos + vec2(0.5, -0.5) * u_rcp).rgb) + 1.0 / 384.0, lumaSe = dluma(T(pos + vec2(0.5, 0.5) * u_rcp).rgb);
    float lumaMax = max(max(lumaNe, lumaSe), max(lumaNw, lumaSw)), lumaMin = min(min(lumaNe, lumaSe), min(lumaNw, lumaSw));
    float lumaMaxM = max(lumaMax, lumaM), lumaMinM = min(lumaMin, lumaM);
    if (lumaMaxM - lumaMinM < max(0.0312, lumaMax * 0.125)) return D(rgbM);
    float dirSwMinusNe = lumaSw - lumaNe, dirSeMinusNw = lumaSe - lumaNw;
    vec2 dir1 = normalize(vec2(dirSwMinusNe + dirSeMinusNw, dirSwMinusNe - dirSeMinusNw) + 1e-8);
    vec2 dir2 = clamp(dir1 / (min(abs(dir1.x), abs(dir1.y)) * 8.0), -2.0, 2.0);
    vec3 rgbA = (KC(F(T(pos - dir1 * u_rcp * 0.5).rgb)) + KC(F(T(pos + dir1 * u_rcp * 0.5).rgb))) * 0.5;
    vec3 rgbB = rgbA * 0.5 + (KC(F(T(pos - dir2 * u_rcp * 2.0).rgb)) + KC(F(T(pos + dir2 * u_rcp * 2.0).rgb))) * 0.25;
    float lumaB = sqrt(max(dot(rgbB, vec3(0.299, 0.587, 0.114)), 0.0) * toneMappingExposure);
    return tmap(KU(lumaB < lumaMin || lumaB > lumaMax ? rgbA : rgbB));
  });
/* the compressed space FXAA averages in: x / (1 + k L(x)) and its inverse (k the exposure, L the luma dluma reads) */
static const char *FXAA_K_GLSL = S(
  vec3 KC(vec3 c) { c = max(c, vec3(0.0)); return c / (1.0 + dot(c, vec3(0.299, 0.587, 0.114)) * toneMappingExposure); }
  vec3 KU(vec3 x) { return x / max(1.0 - dot(x, vec3(0.299, 0.587, 0.114)) * toneMappingExposure, 1e-4); });
/* the tone curve and output transfer of TM_MAIN, as a function */
static const char *TM_CURVE_FN_GLSL = S(
  vec3 tmap(vec3 c) {
    vec3 x = max(c, vec3(0.0));
    if (u_curve == 0) x = clamp(x * toneMappingExposure, 0.0, 1.0);
    else if (u_curve == 1) x = clamp(LinearToneMapping(x), 0.0, 1.0);
    else if (u_curve == 2) x = ReinhardToneMapping(x);
    else if (u_curve == 3) x = OptimizedCineonToneMapping(x);
    else if (u_curve == 4) x = ACESFilmicToneMapping(x);
    else if (u_curve == 5) x = AgXToneMapping(x);
    else x = clamp(NeutralToneMapping(x), 0.0, 1.0);
    return LinearTosRGB(x);
  });

/* CW_TEST_VERYLOW_POST=fog, =aa or =bloom (native test runs): the Low and Very Low tonemap pass leaves that step out
 * while still reporting it fused (the controls of tests/quality-verylow-post.mjs: the picture must change) */
static const char *verylow_post_test(void) {
#if CW_CART
  return "";
#else
  const char *v = getenv("CW_TEST_VERYLOW_POST");
  return v ? v : "";
#endif
}
/* the glow at a pixel as bloom's composite reads it (FUSE_FS fuse_bloom's 9 taps), for the cheap variant's g_bl */
static const char *BLOOM_AT_GLSL =
    "vec3 bloom_at(vec2 uv) {\n"
    "  vec2 t = u_texel * u_radius;\n"
    "  vec3 s = texture(u_bloom, uv).rgb * 4.0;\n"
    "  s += (texture(u_bloom, uv + vec2(-t.x, 0.0)).rgb + texture(u_bloom, uv + vec2(t.x, 0.0)).rgb +\n"
    "        texture(u_bloom, uv + vec2(0.0, -t.y)).rgb + texture(u_bloom, uv + vec2(0.0, t.y)).rgb) * 2.0;\n"
    "  s += texture(u_bloom, uv - t).rgb + texture(u_bloom, uv + t).rgb + texture(u_bloom, uv + vec2(-t.x, t.y)).rgb +\n"
    "       texture(u_bloom, uv + vec2(t.x, -t.y)).rgb;\n"
    "  return s / 16.0 * u_norm;\n"
    "}\n";
static const char *BLOOM_AT_WGSL =
    "fn bloom_at(uv: vec2f) -> vec3f {\n"
    "  let t = u.u_texel * u.u_radius;\n"
    "  var s = BG(uv) * 4.0;\n"
    "  s += (BG(uv + vec2f(-t.x, 0.0)) + BG(uv + vec2f(t.x, 0.0)) + BG(uv + vec2f(0.0, -t.y)) + BG(uv + vec2f(0.0, t.y))) * 2.0;\n"
    "  s += BG(uv - t) + BG(uv + t) + BG(uv + vec2f(-t.x, t.y)) + BG(uv + vec2f(t.x, -t.y));\n"
    "  return s / 16.0 * u.u_norm;\n"
    "}\n";
/* The cheap tonemap pass (effects detail Low and Very Low): the tone curve with FXAA and / or the height fog fused in,
 * and on Low bloom's composite: the glow at the pixel (g_bl) composited into every read FXAA blends, as bloom's
 * composite would have (the glow is smooth over FXAA's few pixels; the edge search reads the scene without it) */
static unsigned tm_cheap_variant(bool bloom, bool grade, bool lens, bool aa, bool fog) {
  static unsigned progs[32];
  int k = (grade ? 1 : 0) | (lens ? 2 : 0) | (aa ? 4 : 0) | (fog ? 8 : 0) | (bloom ? 16 : 0);
  if (progs[k]) return progs[k];
  static char name[64];
  snprintf(name, sizeof name, "tonemap%s%s%s%s%s", bloom ? "+bloom" : "", fog ? "+fog" : "", grade ? "+grade" : "", aa ? "+fxaa" : "",
           lens ? "+lens" : "");
  /* the controls (native test runs): the step left out of the shader, the pass still reported fused */
  bool fog_on = fog && strcmp(verylow_post_test(), "fog"), aa_on = aa && strcmp(verylow_post_test(), "aa");
  bool bl_on = bloom && strcmp(verylow_post_test(), "bloom");
  static char buf[65536];
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    static char head[4096];
    char fields[1024], fsrc[2048];
    snprintf(fields, sizeof fields, "u_frame: i32%s%s };", aa ? ", u_rcp: vec2f" : "", fog ? ", " WGSL_FOG_FIELDS "u_pz: vec4f" : "");
    if (!swap_must(head, sizeof head, WGSL_TM_FUSED_HEAD, "u_frame: i32 };", fields)) return 0;
    /* the fragment: the glow and the fog at the pixel (g_bl, g_fog), then the display colour (FXAA's, or the pixel's
     * through the curve) */
    snprintf(fsrc, sizeof fsrc,
             "var<private> g_fog: vec4f;\n"
             "var<private> g_bl: vec3f;\n"
             "fn F(c0: vec3f) -> vec3f { let c = %s; %s }\n"
             "fn D(c: vec3f) -> vec3f { return tmap(F(c)); }\n"
             "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
             "  tm_exposure_set();\n"
             "  g_bl = %s;\n"
             "  g_fog = %s;\n"
             "  var x = %s;\n%s"
             "  var o = vec4f(x, 1.0);\n%s"
             "  return o;\n}\n",
             fog_on ? "fog_over(vec4f(c0, 1.0), g_fog).rgb" : "c0",
             bl_on ? "return q16(max(c + u.u_k * (g_bl - prefilter(c)), vec3f(0.0)));" : "return c;", bloom ? "bloom_at(uv)" : "vec3f(0.0)",
             fog_on ? "fog_eval(uv, win_depth(cw_load(u_depth, uv).r, u.u_pz))" : "vec4f(-1.0)",
             aa_on ? "fxaa_disp(uv)" : "D(textureSampleLevel(u_src, cw_linear, uv, 0.0).rgb)", grade ? "  x = fuse_grade(x);\n" : "",
             lens ? "  o = fuse_lens(o, uv, pos);\n" : "");
    snprintf(buf, sizeof buf, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s", head, fog ? "@group(0) @binding(5) var u_depth: texture_2d<f32>;\n" : "",
             WGSL_COMMON, WGSL_BL_PREFILTER, cw__fuse_round_wgsl(), WGSL_FUSE_BLOOM_GRADE, WGSL_FUSE_LENS, fog ? WGSL_FOG_FN : "",
             fog ? WGSL_FUSE_FOG : "", WGSL_TM_FNS,
             "fn dluma(c: vec3f) -> f32 { let y = max(dot(c, vec3f(0.299, 0.587, 0.114)), 0.0) * tm_exp; "
             "return sqrt(y / (1.0 + y)); }\n",
             "fn tmap(cin: vec3f) -> vec3f {\n  let c = vec4f(cin, 1.0);\n", WGSL_TM_FUSED_CURVES, "  return x;\n}\n",
             bloom ? BLOOM_AT_WGSL : "", aa ? WGSL_FXAA_CONSOLE : "", fsrc);
    return progs[k] = cw_pass_program_wgsl(name, buf);
  }
  char main_fs[1536];
  snprintf(main_fs, sizeof main_fs,
           "vec4 g_fog;\n"
           "vec3 g_bl;\n"
           "vec3 F(vec3 c0) { vec3 c = %s; %s }\n"
           "vec3 D(vec3 c) { return tmap(F(c)); }\n"
           "vec4 T(vec2 p) { return texture(u_src, p); }\n"
           "%s"
           "void main() {\n"
           "  tm_exposure_set();\n"
           "  g_bl = %s;\n"
           "  g_fog = %s;\n"
           "  vec3 x = %s;\n%s"
           "  o = vec4(x, 1.0);\n%s"
           "}\n",
           fog_on ? "fog_over(vec4(c0, 1.0), g_fog).rgb" : "c0",
           bl_on ? "return q16(max(c + u_k * (g_bl - prefilter(c)), vec3(0.0)));" : "return c;", aa ? "uniform vec2 u_rcp;\n" : "",
           bloom ? "bloom_at(v_uv)" : "vec3(0.0)", fog_on ? "fog_eval(v_uv, texture(u_depth, v_uv).r)" : "vec4(-1.0)",
           aa_on ? "fxaa_disp()" : "D(texture(u_src, v_uv).rgb)", grade ? "  x = fuse_grade(x);\n" : "", lens ? "  o = fuse_lens(o);\n" : "");
  /* the fxaa function is declared after F, D and T: it reads them */
  static char fxaa_part[8192];
  snprintf(fxaa_part, sizeof fxaa_part, "%s%s", aa ? FXAA_K_GLSL : "", aa ? FXAA_CONSOLE_GLSL : "");
  static char mainb[16384];
  const char *split = strstr(main_fs, "void main()");
  snprintf(mainb, sizeof mainb, "%.*s%s%s", (int)(split - main_fs), main_fs, fxaa_part, split);
  snprintf(buf, sizeof buf, "%s%s%s%s%s%s%s%s%s%s%s"
           "float dluma(vec3 c) { float y = max(dot(c, vec3(0.299, 0.587, 0.114)), 0.0) * toneMappingExposure; return sqrt(y / (1.0 + y)); }\n"
           "%s%s%s",
           cw__fusion_raw() ? "#define FUSE_RAW 1\n" : "", GLSL_COMMON, BL_PREFILTER, TM_FS, FUSE_ROUND, FUSE_FS, FUSE_LENS,
           fog ? cw__fog_glsl() : "", fog ? "uniform sampler2D u_depth;\n" : "", fog ? FUSE_FOG : "", "", TM_CURVE_FN_GLSL,
           bloom ? BLOOM_AT_GLSL : "", mainb);
  return progs[k] = cw_pass_program(name, buf);
}

/* ── depth of field ─────────────────────────────────────────────────── */

static unsigned dof_prog;
static const char *DOF_FS = S(
  uniform sampler2D u_src;
  uniform sampler2D u_depth;
  uniform vec4 u_pz;      /* the projection's z rows (window depth to view depth) */
  uniform vec4 u_dof;     /* focus distance, half the sharp range, far blur px, near blur px */
  uniform vec2 u_texel;
  out vec4 o;
  float vdepth(vec2 uv) {
    ivec2 sz = textureSize(u_depth, 0);
    float d = texelFetch(u_depth, clamp(ivec2(uv * vec2(sz)), ivec2(0), sz - 1), 0).r;
    if (d >= 1.0) return 1e6;
    float ndc = d * 2.0 - 1.0;
    return -(u_pz.y - ndc * u_pz.w) / (ndc * u_pz.z - u_pz.x);
  }
  float coc(float z) {
    float dz = z - u_dof.x, a = max(abs(dz) - u_dof.y, 0.0);
    float k = a / (a + max(u_dof.x, 0.05) * 0.5); /* grows toward 1 with distance from the sharp band (a lens's 1/z falloff) */
    return k * (dz > 0.0 ? u_dof.z : u_dof.w);
  }
  void main() {
    vec4 c0 = texture(u_src, v_uv);
    float z0 = vdepth(v_uv), r0 = coc(z0);
    float maxr = max(u_dof.z, u_dof.w);
    if (maxr < 0.5) { o = c0; return; }
    vec3 acc = c0.rgb; float wsum = 1.0;
    for (int i = 0; i < 32; i++) {
      float fi = float(i) + 0.5, rr = sqrt(fi / 32.0) * maxr, a = fi * 2.39996323;
      vec2 off = vec2(cos(a), sin(a)) * rr;
      vec2 uv = v_uv + off * u_texel;
      float z = vdepth(uv), r = coc(z);
      if (z > z0) r = min(r, r0); /* behind the pixel: no more blur than the pixel's own */
      float w = clamp(r - rr + 1.0, 0.0, 1.0);
      acc += texture(u_src, uv).rgb * w; wsum += w;
    }
    o = vec4(acc / wsum, c0.a);
  });

static bool dof_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_post_settings *s = ctx->settings;
  return s->dof && s->dof_blur > 0.25f && ctx->camera;
}

static void dof_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_post_settings *s = ctx->settings;
  if (!dof_prog) dof_prog = prog_of("dof", GLSL_COMMON, DOF_FS, NULL, WGSL_COMMON, WGSL_DOF, NULL);
  if (!dof_prog) return;
  float px = s->dof_blur * (float)ctx->height / 1080.0f;
  cw_pass_use(dof_prog);
  set_pz(dof_prog, ctx);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, ctx->depth_tex);
  cw_pass_uniform1i(dof_prog, "u_src", 0);
  cw_pass_uniform1i(dof_prog, "u_depth", 1);
  cw_pass_uniform4f(dof_prog, "u_dof", s->dof_focus_distance, s->dof_focus_range * 0.5f, px, px * CW_CLAMP(s->dof_near_blur, 0.0f, 1.0f));
  cw_pass_uniform2f(dof_prog, "u_texel", 1.0f / (float)ctx->width, 1.0f / (float)ctx->height);
  cw_pass_fullscreen();
}

/* ── motion blur ────────────────────────────────────────────────────────
 * A reconstruction filter over the velocity target (McGuire, Hennessy, Bukowski, Osman 2012, "A Reconstruction
 * Filter for Plausible Motion Blur"), so the camera's motion and every moving mesh's own blur alike:
 *   tiles     the largest motion of each 16 x 16 pixel tile (two 4 x 4 reductions), then of its 3 x 3 neighbours
 *   gather    each pixel reads `samples` taps along its tile's largest motion, centred on itself and as long as
 *             that motion over the open shutter; a tap counts by how far its own motion (when it is in front)
 *             or the pixel's (when the pixel is in front) would smear it over this pixel, so a moving thing
 *             blurs past its outline over what stands still behind it, and nothing still in front is blurred
 *             by what moves behind it
 * A pixel whose tile holds no motion over half a pixel is copied. The first person viewmodel has no motion in
 * the velocity target and is the nearest thing on its pixels, so it stays sharp; the UI is drawn after POST. */
static unsigned mb_tile_prog, mb_prog;
static const char *MB_TILE_FS = S(
  uniform sampler2D u_vel;
  uniform vec2 u_texel;
  uniform float u_span; /* 4: a 4 x 4 block of the source; 3: the tile and its neighbours */
  out vec4 o;
  void main() {
    vec2 best = vec2(0.0);
    float bl = -1.0;
    int n = int(u_span);
    for (int j = 0; j < n; j++)
      for (int i = 0; i < n; i++) {
        vec2 v = texture(u_vel, v_uv + (vec2(float(i), float(j)) - 0.5 * (u_span - 1.0)) * u_texel).rg;
        float l = dot(v, v);
        if (l > bl) { bl = l; best = v; }
      }
    o = vec4(best, 0.0, 1.0);
  });
static const char *MB_FS = S(
  uniform sampler2D u_src;
  uniform sampler2D u_vel;
  uniform sampler2D u_tile;
  uniform sampler2D u_depth;
  uniform vec4 u_pz;    /* the projection's z rows (window depth to view depth) */
  uniform vec4 u_mb;    /* shutter, longest blur (pixels), samples, 0 */
  uniform vec2 u_size;  /* the frame, pixels */
  uniform vec2 u_tiles; /* the tile target, texels */
  uniform int u_frame;
  out vec4 o;
  float vdepth(vec2 uv) {
    ivec2 sz = textureSize(u_depth, 0);
    float d = texelFetch(u_depth, clamp(ivec2(uv * vec2(sz)), ivec2(0), sz - 1), 0).r;
    if (d >= 1.0) return 1e6;
    float ndc = d * 2.0 - 1.0;
    return -(u_pz.y - ndc * u_pz.w) / (ndc * u_pz.z - u_pz.x);
  }
  vec2 vel_at(vec2 uv) {
    ivec2 sz = textureSize(u_vel, 0);
    return texelFetch(u_vel, clamp(ivec2(uv * vec2(sz)), ivec2(0), sz - 1), 0).rg;
  }
  /* a pixel's motion over the open shutter, in pixels, held to the longest blur */
  vec2 motion(vec2 v) {
    vec2 p = v * u_mb.x * u_size;
    float l = length(p);
    return l > u_mb.y ? p * (u_mb.y / max(l, 1e-6)) : p;
  }
  float cone(float d, float l) { return clamp(1.0 - d / (0.5 * l + 1e-4), 0.0, 1.0); }
  float cyl(float d, float l) { return 1.0 - smoothstep(0.475 * l, 0.525 * l + 1e-4, d); }
  void main() {
    vec4 c0 = texture(u_src, v_uv);
    vec2 tuv = (floor(v_uv * u_tiles) + 0.5) / u_tiles;
    vec2 vmax = motion(texture(u_tile, tuv).rg);
    float len = length(vmax);
    if (len < 0.5) { o = c0; return; }
    float lp = length(motion(vel_at(v_uv)));
    float zp = vdepth(v_uv);
    float ext = max(0.02 * zp, 0.01);
    int n = int(u_mb.z);
    float jit = hash01(uvec3(uint(gl_FragCoord.x), uint(gl_FragCoord.y), uint(u_frame))) - 0.5;
    float w0 = 1.0 / max(lp, 1.0);
    vec3 sum = c0.rgb * w0;
    float wsum = w0;
    vec2 step_uv = vmax / u_size;
    for (int i = 0; i < 32; i++) {
      if (i >= n) break;
      float t = ((float(i) + 0.5 + jit) / float(n)) * 2.0 - 1.0;
      vec2 q = v_uv + step_uv * (t * 0.5);
      float d = abs(t) * len * 0.5;
      float ls = length(motion(vel_at(q)));
      float zs = vdepth(q);
      float fore = clamp(1.0 - (zs - zp) / ext, 0.0, 1.0);
      float back = clamp(1.0 - (zp - zs) / ext, 0.0, 1.0);
      float w = fore * cone(d, ls) + back * cone(d, lp) + cyl(d, ls) * cyl(d, lp) * 2.0;
      sum += texture(u_src, q).rgb * w;
      wsum += w;
    }
    o = vec4(sum / wsum, c0.a);
  });

static bool motion_blur_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  /* reduced motion (the player's accessibility setting, cartwheel/ui.h) turns it off */
  return ctx->settings->motion_blur && ctx->settings->motion_blur_samples > 0 && ctx->settings->motion_blur_shutter > 0 &&
         !cw_ui_reduced_motion() && cw__caps.float_render;
}

static void motion_blur_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_post_settings *s = ctx->settings;
  if (!mb_tile_prog) mb_tile_prog = prog_of("motion_blur_tiles", MB_TILE_FS, NULL, NULL, WGSL_MB_TILE, NULL, NULL);
  if (!mb_prog) mb_prog = prog_of("motion_blur", GLSL_COMMON, MB_FS, NULL, WGSL_COMMON, WGSL_MB, NULL);
  if (!mb_tile_prog || !mb_prog || !ctx->velocity || !ctx->dst) return;
  /* the tiles: 4 x 4 blocks twice, then each tile's neighbourhood */
  const cw_rt *from = ctx->velocity, *tile = NULL;
  for (int k = 0; k < 3; k++) {
    int w = k < 2 ? CW_MAX(1, (from->w + 3) / 4) : from->w, h = k < 2 ? CW_MAX(1, (from->h + 3) / 4) : from->h;
    const cw_rt *rt = cw_pass_rt(ctx, w, h, CW_RT_RG16F, false);
    if (!rt) return;
    cw_pass_bind_target(ctx, rt);
    cw_pass_use(mb_tile_prog);
    cw_pass_bind_texture(0, from->tex);
    cw_pass_uniform1i(mb_tile_prog, "u_vel", 0);
    cw_pass_uniform2f(mb_tile_prog, "u_texel", 1.0f / (float)from->w, 1.0f / (float)from->h);
    cw_pass_uniform1f(mb_tile_prog, "u_span", k < 2 ? 4.0f : 3.0f);
    cw_pass_fullscreen();
    if (from != ctx->velocity) cw_pass_rt_release(ctx, from);
    from = tile = rt;
  }
  cw_pass_bind_target(ctx, ctx->dst);
  cw_pass_use(mb_prog);
  set_pz(mb_prog, ctx);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, ctx->velocity->tex);
  cw_pass_bind_texture(2, tile->tex);
  cw_pass_bind_texture(3, ctx->depth_tex);
  cw_pass_uniform1i(mb_prog, "u_src", 0);
  cw_pass_uniform1i(mb_prog, "u_vel", 1);
  cw_pass_uniform1i(mb_prog, "u_tile", 2);
  cw_pass_uniform1i(mb_prog, "u_depth", 3);
  cw_pass_uniform4f(mb_prog, "u_mb", CW_CLAMP(s->motion_blur_shutter, 0.0f, 2.0f), CW_MAX(s->motion_blur_max, 1.0f) * (float)ctx->height / 1080.0f,
                    (float)CW_CLAMP(s->motion_blur_samples, 1, 32), 0);
  cw_pass_uniform2f(mb_prog, "u_size", (float)ctx->width, (float)ctx->height);
  cw_pass_uniform2f(mb_prog, "u_tiles", (float)tile->w, (float)tile->h);
  cw_pass_uniform1i(mb_prog, "u_frame", (int)(ctx->frame & 0x7fffffff));
  cw_pass_fullscreen();
  cw_pass_rt_release(ctx, tile);
}

/* ── glints: star streaks on bright points ───────────────────────────────
 * What is brighter than glint_threshold (a lamp, a spark, a sun glitter, a gem's highlight) throws thin rays, as a
 * star filter or an iris does. At a quarter of the frame's size: the bright part (4 reads a texel: a 4 x 4 box of
 * the frame, so a one-pixel light does not flicker), then for each of glint_points rays two streak passes of 7
 * reads back toward the light (the second with 7 times the step: 49 evenly spaced taps, their weights one
 * exponential falling to 2% at the ray's end), summed, and one pass adds the sum to the frame in scene-linear
 * light before the tone curve. */
static unsigned glint_bright_prog, glint_streak_prog, glint_add_prog;
static const char *GLINT_BRIGHT_FS = S(
  uniform sampler2D u_src;
  uniform vec2 u_texel;
  uniform float u_threshold;
  out vec4 o;
  void main() {
    vec3 c = vec3(0.0);
    for (int j = 0; j < 2; j++)
      for (int i = 0; i < 2; i++) c += texture(u_src, v_uv + (vec2(float(i), float(j)) * 2.0 - 1.0) * u_texel).rgb;
    c = max(c * 0.25, vec3(0.0));
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    /* (what is over the threshold, up to twice it: a whole bright wall does not wash the frame out) */
    o = vec4(c * (clamp(l - u_threshold, 0.0, 2.0 * u_threshold) / max(l, 1e-4)), 1.0);
  });
static const char *GLINT_STREAK_FS = S(
  uniform sampler2D u_src;
  uniform vec2 u_step;  /* uv a read apart, along the ray */
  uniform float u_fall; /* a read's weight against the one before it */
  out vec4 o;
  void main() {
    vec3 s = vec3(0.0);
    float ws = 0.0;
    for (int i = 0; i < 7; i++) {
      float w = pow(u_fall, float(i));
      s += texture(u_src, v_uv + u_step * float(i)).rgb * w;
      ws += w;
    }
    o = vec4(s / ws, 1.0);
  });
static const char *GLINT_ADD_FS = S(
  uniform sampler2D u_src;
  uniform sampler2D u_glint;
  uniform float u_k;
  out vec4 o;
  void main() {
    vec4 c = texture(u_src, v_uv);
    o = vec4(c.rgb + texture(u_glint, v_uv).rgb * u_k, c.a);
  });

static bool glints_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  return ctx->settings->glints && ctx->settings->glint_intensity > 0 && cw__caps.float_render && cw__caps.float_blend;
}

static void glints_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_post_settings *s = ctx->settings;
  if (!glint_bright_prog) glint_bright_prog = prog_of("glint_bright", GLINT_BRIGHT_FS, NULL, NULL, WGSL_GLINT_BRIGHT, NULL, NULL);
  if (!glint_streak_prog) glint_streak_prog = prog_of("glint_streak", GLINT_STREAK_FS, NULL, NULL, WGSL_GLINT_STREAK, NULL, NULL);
  if (!glint_add_prog) glint_add_prog = prog_of("glint_add", GLINT_ADD_FS, NULL, NULL, WGSL_GLINT_ADD, NULL, NULL);
  if (!glint_bright_prog || !glint_streak_prog || !glint_add_prog || !ctx->dst) return;
  int w = CW_MAX(1, ctx->width / 4), h = CW_MAX(1, ctx->height / 4);
  cw_rt_format f = ctx->src->format;
  const cw_rt *bright = cw_pass_rt(ctx, w, h, f, false), *tmp = cw_pass_rt(ctx, w, h, f, false), *sum = cw_pass_rt(ctx, w, h, f, false);
  if (!bright || !tmp || !sum) return;
  cw_pass_bind_target(ctx, bright);
  cw_pass_use(glint_bright_prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_uniform1i(glint_bright_prog, "u_src", 0);
  cw_pass_uniform2f(glint_bright_prog, "u_texel", 1.0f / (float)ctx->src->w, 1.0f / (float)ctx->src->h);
  cw_pass_uniform1f(glint_bright_prog, "u_threshold", CW_MAX(s->glint_threshold, 0.0f));
  cw_pass_fullscreen();
  /* the rays: reach texels from the light, the first pass's reads reach / 49 apart */
  int dirs = CW_CLAMP(s->glint_points, 2, 8);
  float reach = CW_CLAMP(s->glint_length, 0.01f, 1.0f) * (float)h, base = reach / 49.0f;
  cw_pass_bind_target(ctx, sum);
  cw_pass_clear(0, 0, 0, 0, false);
  for (int d = 0; d < dirs; d++) {
    float sn, cs;
    cw_sincos(s->glint_rotation + (float)d * 6.2831853f / (float)dirs, &sn, &cs);
    for (int k = 0; k < 2; k++) {
      float step = k ? base * 7.0f : base;
      cw_pass_bind_target(ctx, k ? sum : tmp);
      cw_pass_use(glint_streak_prog);
      if (k) cw_pass_set_blend(CW_PASS_BLEND_ADD);
      cw_pass_bind_texture(0, k ? tmp->tex : bright->tex);
      cw_pass_uniform1i(glint_streak_prog, "u_src", 0);
      cw_pass_uniform2f(glint_streak_prog, "u_step", cs * step / (float)w, sn * step / (float)h);
      /* 2% left at the ray's end: 0.02^(step / reach) a read */
      cw_pass_uniform1f(glint_streak_prog, "u_fall", k ? 0.5719f : 0.9233f); /* 0.02^(7/49), 0.02^(1/49) */
      cw_pass_fullscreen();
      cw_pass_set_blend(CW_PASS_BLEND_NONE);
    }
  }
  cw_pass_bind_target(ctx, ctx->dst);
  cw_pass_use(glint_add_prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, sum->tex);
  cw_pass_uniform1i(glint_add_prog, "u_src", 0);
  cw_pass_uniform1i(glint_add_prog, "u_glint", 1);
  cw_pass_uniform1f(glint_add_prog, "u_k", s->glint_intensity * 2.5f);
  cw_pass_fullscreen();
  cw_pass_rt_release(ctx, bright);
  cw_pass_rt_release(ctx, tmp);
  cw_pass_rt_release(ctx, sum);
}

/* ── lens: chromatic aberration, vignette, film grain ───────────────── */

static unsigned lens_prog;
static const char *LENS_FS = S(
  uniform sampler2D u_src;
  uniform vec4 u_on;      /* ca, vignette, grain, aspect */
  uniform vec4 u_amt;     /* ca, vignette, vignette smoothness, grain */
  uniform int u_frame;
  uniform vec4 u_vr0, u_vr1, u_vr2, u_vr3; /* split screen, a lens per view: the views' rectangles (uv, from the bottom left) */
  uniform vec4 u_vrn;     /* their count (0: one lens over the target), the target's width and height */
  out vec4 o;
  bool inr(vec4 r, vec2 q) { return q.x >= r.x && q.y >= r.y && q.x < r.x + r.z && q.y < r.y + r.w; }
  void main() {
    vec4 c = texture(u_src, v_uv);
    vec4 vr = vec4(0.0, 0.0, 1.0, 1.0);
    vec4 on = u_on;
    if (u_vrn.x > 0.5) {
      if (inr(u_vr0, v_uv)) vr = u_vr0;
      else if (u_vrn.x > 1.5 && inr(u_vr1, v_uv)) vr = u_vr1;
      else if (u_vrn.x > 2.5 && inr(u_vr2, v_uv)) vr = u_vr2;
      else if (u_vrn.x > 3.5 && inr(u_vr3, v_uv)) vr = u_vr3;
      else { o = c; return; } /* no view: as the layout drew it */
      on.w = vr.z * u_vrn.y / max(vr.w * u_vrn.z, 1.0);
    }
    vec2 d = (v_uv - vr.xy) / vr.zw - 0.5;
    if (on.x > 0.5) {
      vec2 off = d * (u_amt.x * 2.0) * vr.zw;
      c.r = texture(u_src, v_uv - off).r;
      c.b = texture(u_src, v_uv + off).b;
    }
    if (on.y > 0.5) {
      float r = length(d * vec2(on.w, 1.0)) / length(vec2(0.5 * on.w, 0.5));
      float f = smoothstep(1.0 - u_amt.z, 1.0 + u_amt.z * 0.25, r * (1.0 + 0.5 * u_amt.z));
      c.rgb *= 1.0 - u_amt.y * f;
    }
    if (u_on.z > 0.5) {
      float n = hash01(uvec3(uvec2(max(gl_FragCoord.xy - (u_vrn.x > 0.5 ? floor(vr.xy * u_vrn.yz + 0.5) : vec2(0.0)), vec2(0.0))), uint(u_frame))) - 0.5;
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
  /* split screen with a shared chain and a lens per view (vignette_per_view): each view's rectangle */
  const cw_pipeline *p = ctx->pipeline;
  int n = ctx->settings->vignette_per_view && p->nviews >= 2 && p->view_post_split ? p->nviews : 0;
  static const char *const R[4] = { "u_vr0", "u_vr1", "u_vr2", "u_vr3" };
  for (int i = 0; i < 4; i++) {
    const cw_view *v = &p->views[i < n ? i : 0];
    cw_pass_uniform4f(lens_prog, R[i], v->x, 1 - v->y - v->h, v->w, v->h);
  }
  cw_pass_uniform4f(lens_prog, "u_vrn", (float)n, (float)ctx->width, (float)ctx->height, 0);
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
      ivec2 lim = ivec2(u_size) - 1; /* the edge: the side outside the frame is never taken (AO_FS) */
      vec3 dx = q.x >= lim.x || (q.x > 0 && abs(pl.z - P.z) <= abs(pr.z - P.z)) ? P - pl : pr - P;
      vec3 dy = q.y >= lim.y || (q.y > 0 && abs(pd.z - P.z) <= abs(pu.z - P.z)) ? P - pd : pu - P;
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
                                           .active = prepass_active, .execute = prepass_exec,
#if defined(T3_WGPU_DEPTH_PREPASS)
                                           .webgpu = true
#endif
                                         });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "scene", .phase = CW_PHASE_OPAQUE, .order = 0,
                                           .writes = CW_RES_COLOR | CW_RES_DEPTH, .execute = scene_exec, .webgpu = true,
                                           .views = CW_PASS_VIEWS_ONCE /* a t3_array_camera: every view in one render */ });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "viewmodel", .phase = CW_PHASE_OPAQUE, .order = 10,
                                           .writes = CW_RES_COLOR | CW_RES_DEPTH,
                                           .active = viewmodel_active, .execute = viewmodel_exec, .webgpu = true,
                                           .views = CW_PASS_VIEWS_EACH /* each view's own, in its rectangle */ });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "velocity", .phase = CW_PHASE_POST, .order = 0,
                                           .reads = CW_RES_DEPTH, .writes = CW_RES_VELOCITY, .execute = velocity_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "ssao", .phase = CW_PHASE_POST, .order = 100,
                                           .reads = CW_RES_COLOR | CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                           .active = ssao_active, .execute = ssao_exec, .webgpu = true, .views = CW_PASS_VIEWS_EACH });
  void cw__gi_add_post_passes(cw_pipeline *p); /* src/gi: the "gi" SHADOW pass, SSGI (120), SSR (150) */
  cw__gi_add_post_passes(p);
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "taa", .phase = CW_PHASE_POST, .order = 200,
                                           .reads = CW_RES_COLOR | CW_RES_VELOCITY | CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                           .active = taa_active, .execute = taa_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "dof", .phase = CW_PHASE_POST, .order = 250,
                                           .reads = CW_RES_COLOR | CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                           .active = dof_active, .execute = dof_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "motion_blur", .phase = CW_PHASE_POST, .order = 270,
                                           .reads = CW_RES_COLOR | CW_RES_VELOCITY | CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                           .active = motion_blur_active, .execute = motion_blur_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "exposure", .phase = CW_PHASE_POST, .order = 290, .reads = CW_RES_COLOR,
                                           .active = exposure_active, .execute = exposure_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "bloom", .phase = CW_PHASE_POST, .order = 300,
                                           .reads = CW_RES_COLOR, .writes = CW_RES_COLOR,
                                           .active = bloom_active, .execute = bloom_exec, .webgpu = true });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "glints", .phase = CW_PHASE_POST, .order = 320, .reads = CW_RES_COLOR,
                                           .writes = CW_RES_COLOR, .active = glints_active, .execute = glints_exec, .webgpu = true });
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
