/* Backdrop (cartwheel/backdrop.h): worlds of their own drawn behind a tree's
 * scene. Each frame, before the tree's pipeline runs, every backdrop with an
 * opacity above 0 renders its world through that world's own pipeline into
 * its HDR scene target (pipeline.c, cw__pipeline_render_backdrop: the passes
 * up to the colour-only post chain, which is the main tree's). The main
 * pipeline's "backdrop" pass (OPAQUE, before the scene pass) blends the
 * pictures into the scene target over the tree's own background, and the
 * scene pass then keeps that colour and draws no background of its own
 * (pipeline_post.c). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/backdrop.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/gpu.h"
#include "cartwheel/quality.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "lighting_internal.h"
#include "pipeline_internal.h"
#include "quality_internal.h"
#include "scene/render3d_internal.h"
#if CW_WGPU
#include "fx/fx_wgpu.h"
#include "pipeline_wgpu.h"
#endif

typedef struct backdrop {
  /* properties */
  const char *scene, *camera;
  float opacity;
  int32_t order;
  /* runtime */
  cw_node self;
  cw_tree *world;
  cw_node world_root;
  const char *world_scene;
  bool world_failed, warned;
  int32_t renders;
  float cpu_ms;
  int ran_passes; /* passes its world's pipeline ran last frame it rendered */
  /* environment (the world as the tree's image-based light): properties */
  bool environment;
  int32_t env_resolution, env_every;
  /* its captures: two sets of six faces (the scene samples the front set while the back one is drawn), the frame of
   * the capture in flight (faces spread over env_every frames), the front set complete */
  cw_rt cap[2][6];
  int cap_size, cap_front, cap_k;
  bool cap_have, cap_fresh;
  int32_t env_captures;
  float env_cpu_ms;
} backdrop;

_Static_assert(sizeof(((cw_pipeline *)0)->bd) / sizeof(((cw_pipeline *)0)->bd[0]) == CW_BACKDROP_MAX, "the pipeline holds CW_BACKDROP_MAX pictures");

static const cw_node_type *bd_t;
static backdrop *BD(cw_node n) { return cw_node_data_t(n, bd_t); }

/* ── the world ─────────────────────────────────────────────────────── */

static void world_free(backdrop *d) {
  if (d->world) cw_tree_free(d->world);
  d->world = NULL;
  d->world_root = (cw_node){ 0 };
  d->world_scene = NULL;
  d->world_failed = false;
}
static cw_tree *world_make(backdrop *d) {
  if (d->world) return d->world;
  cw_tree *prev = cw_current_tree();
  d->world = cw_tree_new();
  cw_set_current_tree(prev); /* cw_tree_new takes the seat only when it is empty: never ours */
  return d->world;
}
/* the `scene` property into the world; false when there is none or it failed (logged once) */
static bool world_sync(cw_node n, backdrop *d) {
  bool want = d->scene && d->scene[0];
  if (!want) {
    if (d->world_scene) world_free(d);
    return d->world != NULL; /* a world game code built (cw_backdrop_tree) stays */
  }
  if (d->world && d->world_scene == d->scene) return !d->world_failed;
  world_free(d);
  world_make(d);
  d->world_scene = d->scene;
  cw_node root = cw_scene_instance_path(d->scene);
  if (!cw_node_valid(root)) {
    cw_error("Backdrop '%s': scene '%s' could not be loaded (an asset path, e.g. \"stage.cwscene\")", cw_node_name(n), d->scene);
    d->world_failed = true;
    return false;
  }
  cw_node_add_child(cw_tree_root(d->world), root);
  d->world_root = root;
  return true;
}

cw_tree *cw_backdrop_tree(cw_node n) {
  backdrop *d = BD(n);
  if (!d) return NULL;
  world_sync(n, d);
  return world_make(d);
}
cw_node cw_backdrop_root(cw_node n) {
  backdrop *d = BD(n);
  if (!d) return (cw_node){ 0 };
  world_sync(n, d);
  return d->world_root;
}
void cw_backdrop_set_opacity(cw_node n, float o) {
  if (BD(n)) cw_node_set(n, "opacity", cw_val_float(o));
}
float cw_backdrop_opacity(cw_node n) {
  backdrop *d = BD(n);
  return d ? CW_CLAMP(d->opacity, 0.0f, 1.0f) : 0;
}
void cw_backdrop_set_order(cw_node n, int order) {
  if (BD(n)) cw_node_set(n, "order", cw_val_int(order));
}
int cw_backdrop_renders(cw_node n) {
  backdrop *d = BD(n);
  return d ? d->renders : 0;
}
float cw_backdrop_last_cpu_ms(cw_node n) {
  backdrop *d = BD(n);
  return d ? d->cpu_ms : 0;
}

/* ── test switches (native env, a cart's env request) ──────────────── */

static bool test_flag(const char *name) {
  const char *v = cw_test_env(name);
  return v && *v && atoi(v) != 0;
}

/* ── environment: the world captured as a cube ─────────────────────── */

/* The six faces of a capture are drawn by the world's own renderer through a camera per face, as three.c's
 * PMREMGenerator draws a scene capture (r187's CubeCamera: fov -90, aspect 1, its look and up per face, which differ
 * on WebGPU), each into a size x size HDR target of its own with depth; on WebGPU without a y flip, as three.c draws
 * into its own cube targets. So the faces hold a cube target's texels, row for row, on either backend, and the cube
 * made from them (below) is read as three.c reads one of its own. */
static void face_look(t3_camera *c, bool wg, int face, t3_vec3 at) {
  static const float gl_dir[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
  static const float gl_up[6][3] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };
  static const float wg_dir[6][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
  static const float wg_up[6][3] = { { 0, -1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, -1, 0 }, { 0, -1, 0 } };
  const float *d = wg ? wg_dir[face] : gl_dir[face], *u = wg ? wg_up[face] : gl_up[face];
  c->base.position = at;
  c->base.up = t3_v3(u[0], u[1], u[2]);
  t3_object_look_at(c, at.x + d[0], at.y + d[1], at.z + d[2]);
  t3_object_update_matrix_world(c, false);
}

static bool on_wgpu(void) { return cw_gpu_backend() == CW_GPU_WEBGPU; }
/* GL: the state the worlds' renders leave that the tree's own renderer does not set itself: the framebuffer, and the
 * viewport and scissor box of the frame (a world's last draw can be its volumetric fog's spot shadow tile: three.c's
 * G-buffer render for the tree's screen-space reflections then drew into that tile's rectangle and held nothing) */
static void gl_frame_state(int w, int h) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glViewport(0, 0, w, h);
  glScissor(0, 0, w, h);
}
/* one face: sc through cam into rt (cleared to the scene's background colour, or the renderer's clear colour) */
static void render_face(t3_renderer *r, t3_scene *sc, t3_camera *cam, const cw_rt *rt, unsigned restore_fbo) {
  bool had_bg = sc->has_background;
  t3_color bgc = sc->background;
  float ca = 1;
  t3_color cc = t3_renderer_get_clear_color(r, &ca);
  if (!had_bg && !sc->background_texture) sc->has_background = true, sc->background = cc;
  t3_color clear = sc->has_background ? sc->background : cc;
#if CW_WGPU
  if (on_wgpu()) {
    t3_external_target et = { .samples = 1, .load_color = false, .load_depth = false, .clear_rgba = { clear.r, clear.g, clear.b, 1 },
                              .clear_depth = cw_gpu_reversed_depth() ? 0.0f : 1.0f, .x = 0, .y = 0, .w = rt->w, .h = rt->h, .flip_y = false };
    et.color = (uintptr_t)cw__pw_attach(rt->tex), et.depth = (uintptr_t)cw__pw_attach(rt->depth_tex);
    et.color_format = (int)cw__pw_format(rt->tex), et.depth_format = (int)cw__pw_format(rt->depth_tex);
    t3_renderer_set_external_encoder(r, 0);
    t3_renderer_set_external_target(r, &et);
    t3_renderer_render(r, sc, cam);
    t3_renderer_set_external_target(r, NULL);
  } else
#endif
  {
    /* the target's framebuffer and the renderer's viewport over its bottom-left rt->w x rt->h (three.c measures it from
     * the top of its drawing buffer), cleared by the render (scissored to it) */
    (void)clear;
    int bw, bh, vx, vy, vw, vh, sx, sy, sw, sh;
    t3_renderer_get_size(r, &bw, &bh);
    t3_renderer_get_viewport(r, &vx, &vy, &vw, &vh);
    bool st = t3_renderer_get_scissor(r, &sx, &sy, &sw, &sh);
    bool auto_clear = t3_renderer_get_auto_clear(r);
    t3_renderer_reset_bindings(r);
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    t3_renderer_set_framebuffer(r, rt->fbo);
    t3_renderer_set_viewport(r, 0, bh - rt->h, rt->w, rt->h);
    t3_renderer_set_scissor(r, 0, bh - rt->h, rt->w, rt->h);
    t3_renderer_set_scissor_test(r, true);
    t3_renderer_set_auto_clear(r, true);
    t3_renderer_render(r, sc, cam);
    t3_renderer_set_auto_clear(r, auto_clear);
    t3_renderer_set_viewport(r, vx, vy, vw, vh);
    t3_renderer_set_scissor(r, sx, sy, sw, sh);
    t3_renderer_set_scissor_test(r, st);
    t3_renderer_set_framebuffer(r, restore_fbo);
    t3_renderer_reset_bindings(r);
    glBindFramebuffer(GL_FRAMEBUFFER, restore_fbo);
  }
  sc->has_background = had_bg;
  sc->background = bgc;
}

/* faces f0 .. f1 - 1 of a scene from `at` into faces[] (around them: the shadow maps held, as the last render drew them;
 * the clustered lights seen from each face's camera; on WebGPU the frame's work so far submitted first) */
static void render_faces(t3_renderer *r, cw_tree *lit, t3_scene *sc, t3_vec3 at, float near, float far, cw_rt *faces, int f0, int f1,
                         unsigned restore_fbo) {
  if (f0 >= f1) return;
  bool wg = on_wgpu();
  t3_renderer_set_shadow_hold(r, true);
  if (lit) cw__lighting_capture(lit, true);
#if CW_WGPU
  if (wg) cw__pw_flush();
#endif
  if (!wg) t3_renderer_reset_bindings(r);
  t3_camera *c = t3_perspective_camera_new(-90, 1, near, far);
  for (int f = f0; f < f1; f++) {
    face_look(c, wg, f, at);
    render_face(r, sc, c, &faces[f], restore_fbo);
  }
  t3_release(c);
  if (lit) cw__lighting_capture(lit, false);
  t3_renderer_set_shadow_hold(r, false);
  if (!wg) t3_renderer_reset_bindings(r);
}

static int pow2_in(int v, int lo, int hi) {
  v = CW_CLAMP(v, lo, hi);
  int p = lo;
  while (p * 2 <= v) p *= 2;
  return p;
}
static void faces_free(cw_rt *f) {
  for (int i = 0; i < 6; i++) cw__rt_destroy(&f[i]);
}
static bool faces_make(cw_rt *f, int size) {
  for (int i = 0; i < 6; i++)
    if (!cw__rt_create(&f[i], size, size, CW_RT_RGBA16F, true)) {
      faces_free(f);
      return false;
    }
  return true;
}
static void cap_free(backdrop *d) {
  if (!d->cap_size) return;
  faces_free(d->cap[0]), faces_free(d->cap[1]);
  d->cap_size = 0;
  d->cap_have = d->cap_fresh = false;
  d->cap_k = 0;
}

/* frames between two captures (env_every; spread wider on the lower effects presets, as reflection probes are:
 * Very Low x8, Low x4, Medium x2) */
static int env_every_now(const backdrop *d) {
  int n = CW_MAX(1, d->env_every), e = cw__quality_effects();
  return n * (e <= CW_QUALITY_VERY_LOW ? 8 : e == CW_QUALITY_LOW ? 4 : e == CW_QUALITY_MEDIUM ? 2 : 1);
}

/* this frame's part of the backdrop's capture, from its camera's position in its world (after its picture: the world's
 * matrices, lights and shadow maps are this frame's). The first capture is made whole; then the six faces of the next
 * are spread over env_every frames into the back set, which becomes the front when its last face is drawn. */
static void env_capture(backdrop *d, cw_tree *wt, cw_node camn) {
  t3_renderer *r = cw_render3d_renderer(wt);
  t3_scene *sc = cw_render3d_scene(wt);
  t3_camera *vc = cw_camera3d_t3(camn);
  d->cap_fresh = false;
  if (!r || !sc || !vc) return;
  int size = pow2_in(d->env_resolution, 16, 512);
  if (size != d->cap_size) {
    cap_free(d);
    if (!faces_make(d->cap[0], size)) return;
    if (!faces_make(d->cap[1], size)) {
      faces_free(d->cap[0]);
      return;
    }
    d->cap_size = size;
  }
  double t0 = cw__render3d_now_ms();
  /* the faces over the first every - 1 frames, none in the last (the frame the scene filters the finished capture in) */
  int every = env_every_now(d), back = d->cap_front ^ 1, spread = every > 1 ? every - 1 : 1;
  int f0 = d->cap_have ? CW_MIN(6, 6 * d->cap_k / spread) : 0, f1 = d->cap_have ? CW_MIN(6, 6 * (d->cap_k + 1) / spread) : 6;
  t3_vec3 at = t3_mat4_get_position(&vc->base.matrix_world);
  render_faces(r, wt, sc, at, vc->near, vc->far, d->cap[back], f0, f1, 0);
  d->cap_k++;
  if (!d->cap_have || d->cap_k >= every) {
    d->cap_front = back;
    d->cap_have = d->cap_fresh = true;
    d->cap_k = 0;
    d->env_captures++;
  }
  d->env_cpu_ms = (float)(cw__render3d_now_ms() - t0);
}

/* ── each frame, before the tree's pipeline: the pictures ──────────── */

typedef struct bdw { CW_ARRAY(cw_node) nodes; } bdw;
static void bdw_free(void *p) {
  bdw *W = p;
  cw_array_free(&W->nodes);
  cw_free(W);
}

static bool find_cam(cw_node n, void *user) {
  cw_node *found = user;
  if (!cw_node_valid(*found) && cw_node_is(n, "Camera3D")) *found = n;
  return !cw_node_valid(*found);
}
static float exposure_of(cw_pipeline *p) { return p->s.exposure * exp2f(p->s.exposure_ev); }

static int order_cmp(const void *a, const void *b) {
  const backdrop *x = *(backdrop *const *)a, *y = *(backdrop *const *)b;
  if (x->order != y->order) return x->order < y->order ? -1 : 1;
  return 0;
}

/* pipeline.c calls this at the start of the tree's cw_pipeline_render, with the frame's size */
static void frame_hook(cw_tree *t, cw_pipeline *p, int w, int h, double alpha) {
  p->bd_n = 0;
  p->bd_env_n = 0;
  p->bd_bg_weight = 1;
  p->bd_w = w, p->bd_h = h;
  bdw *W = cw_tree_slot(t, "backdrop");
  if (!W) return;
  backdrop *list[64];
  int n = 0;
  for (int i = 0; i < W->nodes.count; i++) {
    cw_node nd = W->nodes.data[i];
    backdrop *d = BD(nd);
    if (!d || cw_node_tree(nd) != t) { cw_array_remove_swap(&W->nodes, i); i--; continue; }
    d->ran_passes = 0; /* (this frame: none unless it renders below) */
    bool have = world_sync(nd, d);
    /* opacity 0, or hidden by itself or a Node3D, Node2D or CanvasLayer above it (cw_node_visible_in_tree): nothing at all */
    if (!have || !(d->opacity > 0) || !cw_node_visible_in_tree(nd) || n >= (int)CW_ARRAY_LEN(list)) continue;
    list[n++] = d;
  }
  if (!n) return;
  /* stable by order (insertion sort keeps the tree order of ties) */
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && order_cmp(&list[j - 1], &list[j]) > 0; j--) {
      backdrop *x = list[j];
      list[j] = list[j - 1];
      list[j - 1] = x;
    }
  static bool over_warned;
  if (n > CW_BACKDROP_MAX && !over_warned) {
    over_warned = true;
    cw_warn("Backdrop: %d backdrops have an opacity above 0; the %d furthest back are drawn (CW_BACKDROP_MAX)", n, CW_BACKDROP_MAX);
  }
  n = CW_MIN(n, CW_BACKDROP_MAX);
  bool ignore_opacity = test_flag("CW_TEST_BACKDROP_OPACITY_IGNORED"); /* a gate control: every backdrop drawn at 1 */
  float main_exp = exposure_of(p);
  cw_tree *prev = cw_current_tree();
  for (int i = 0; i < n; i++) {
    backdrop *d = list[i];
    cw_tree *wt = d->world;
    cw_node cam = { 0 };
    if (d->camera && d->camera[0]) {
      cam = cw_node_get(cw_node_valid(d->world_root) ? d->world_root : cw_tree_root(wt), d->camera);
      if (!cw_node_is(cam, "Camera3D")) {
        if (!d->warned) cw_warn("Backdrop '%s': camera '%s' is not a Camera3D of its world (a path from the root of its scene)", cw_node_name(d->self), d->camera);
        d->warned = true;
        continue;
      }
    }
    double t0 = cw__render3d_now_ms();
    cw_set_current_tree(wt);
    cw_tree_pre_render(wt, alpha);
    if (!cw_node_valid(cam) && !cw_node_valid(cw_render3d_camera(wt))) {
      cw_node walk = { 0 };
      cw_node_walk(cw_tree_root(wt), find_cam, &walk);
      if (!cw_node_valid(walk)) {
        if (!d->warned) cw_warn("Backdrop '%s': its world has no Camera3D; nothing is drawn", cw_node_name(d->self));
        d->warned = true;
        cw_set_current_tree(prev);
        continue;
      }
    }
    cw_pipeline *wp = cw_pipeline_of(wt);
    t3_renderer *r = cw_render3d_renderer(wt);
    if (r) t3_renderer_reset_bindings(r);
    const cw_rt *pic = cw__pipeline_render_backdrop(wt, cam, w, h, alpha);
    r = cw_render3d_renderer(wt);
    if (r) t3_renderer_reset_bindings(r);
    if (!d->environment) cap_free(d);
    cw_set_current_tree(prev);
    d->cpu_ms = (float)(cw__render3d_now_ms() - t0);
    const cw_pass_stat *st;
    int ns = cw_pipeline_pass_stats(wp, &st), ran = 0;
    for (int k = 0; k < ns; k++) ran += st[k].ran;
    d->ran_passes = ran;
    if (!pic) continue;
    d->renders++;
    float o = ignore_opacity ? 1 : CW_CLAMP(d->opacity, 0.0f, 1.0f);
    p->bd[p->bd_n++] = (struct cw__backdrop_pic){ .tex = pic->tex, .opacity = o, .scale = main_exp > 0 ? exposure_of(wp) / main_exp : 1 };
    /* its world as the tree's environment: captured in the tree's "backdrop_env" pass (cartwheel/backdrop.h environment) */
    if (d->environment)
      p->bd_env[p->bd_env_n++] = (struct cw__backdrop_env){ .bd = d, .world = wt, .camera = cw_node_valid(cam) ? cam : cw_render3d_camera(wt), .pic = p->bd_n - 1 };
  }
  /* the main tree's renderer draws next: its cached GL state is stale (the bindings, and its draw state) */
  if (!on_wgpu()) gl_frame_state(w, h);
  t3_renderer *mr = cw_render3d_renderer(t);
  if (mr) t3_renderer_reset_bindings(mr), t3_renderer_reset_state(mr);
  float wgt = 1;
  for (int i = 0; i < p->bd_n; i++) wgt *= 1 - p->bd[i].opacity;
  p->bd_bg_weight = wgt;
}

float cw__backdrop_bg_weight(const cw_pipeline *p) {
  return p && p->bd_n > 0 && !p->rt_scope && !p->backdrop_run ? p->bd_bg_weight : 1;
}

/* ── the pass: the pictures into the scene target ──────────────────── */

static unsigned bd_prog;
static unsigned program(void) {
  if (!bd_prog)
    bd_prog = cw__pass_program2("backdrop",
      "uniform sampler2D u_src;\nuniform vec4 u_rect;\nuniform vec4 u_k;\nout vec4 o;\n"
      "void main() { o = vec4(texture(u_src, u_rect.xy + v_uv * u_rect.zw).rgb * (u_k.x * u_k.y), u_k.y); }\n",
      "struct U { u_rect: vec4f, u_k: vec4f };\n"
      "@group(0) @binding(0) var<uniform> u: U;\n"
      "@group(0) @binding(1) var u_src: texture_2d<f32>;\n"
      "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
      "  let c = textureSampleLevel(u_src, cw_linear, u.u_rect.xy + uv * u.u_rect.zw, 0.0).rgb;\n"
      "  return vec4f(c * (u.u_k.x * u.u_k.y), u.u_k.y);\n}\n");
  return bd_prog;
}

static bool pass_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_pipeline *p = ctx->pipeline;
  return p->bd_n > 0 && !p->rt_scope && !p->backdrop_run && ctx->scene3d && !test_flag("CW_TEST_BACKDROP_ON_TOP");
}
static bool ontop_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_pipeline *p = ctx->pipeline;
  return p->bd_n > 0 && !p->rt_scope && !p->backdrop_run && ctx->scene3d && test_flag("CW_TEST_BACKDROP_ON_TOP");
}

/* the part of the whole picture this run draws, as a uv rectangle of the backdrop pictures */
static void run_rect(const cw_pass_ctx *ctx, float r[4]) {
  const cw_pipeline *p = ctx->pipeline;
  r[0] = r[1] = 0, r[2] = r[3] = 1;
  if (ctx->view_count > 0 || p->nviews < 2 || p->view_i < 0 || p->view_i >= p->nviews) return; /* one target for the whole screen */
  const cw_view *v = &p->views[p->view_i];
  r[0] = v->x, r[1] = 1 - v->y - v->h, r[2] = v->w, r[3] = v->h; /* uv: (0, 0) at the bottom left */
}

static t3_scene *bg_scene;

/* the tree's own background under backdrops that leave some of it: its clear colour, or its sky (three.c draws
 * the background of an empty scene that shares it) */
static void main_background(cw_pass_ctx *ctx) {
  t3_scene *sc = ctx->scene3d;
  t3_renderer *r = ctx->renderer;
  float a = 1;
  t3_color c = r ? t3_renderer_get_clear_color(r, &a) : (t3_color){ 0, 0, 0 };
  if (sc->has_background && !sc->background_texture) c = sc->background;
  if (!sc->background_texture || !r) {
    cw_pass_clear(c.r, c.g, c.b, 1, false);
    return;
  }
  if (!bg_scene) bg_scene = t3_scene_new();
  bg_scene->has_background = sc->has_background;
  bg_scene->background = sc->background;
  t3_scene_set_background_texture(bg_scene, sc->background_texture);
  bg_scene->background_intensity = sc->background_intensity;
  bg_scene->background_blurriness = sc->background_blurriness;
  bg_scene->background_rotation = sc->background_rotation;
  cw_pipeline *p = ctx->pipeline;
  t3_camera *cam = ctx->view_count > 0 && p->arr_cam ? &p->arr_cam->base : ctx->camera;
#if CW_WGPU
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    const cw__wgpu_pass_targets *pt = cw__pass_wgpu_targets(ctx);
    if (pt && pt->color) {
      t3_external_target et = {
        .color = (uintptr_t)pt->color, .color_resolve = (uintptr_t)pt->resolve, .depth = (uintptr_t)pt->depth,
        .color_format = (int)pt->color_format, .depth_format = (int)pt->depth_format, .samples = pt->samples,
        .load_color = false, .load_depth = true, .clear_rgba = { c.r, c.g, c.b, 1 }, .clear_depth = cw_gpu_reversed_depth() ? 0.0f : 1.0f,
        .x = 0, .y = 0, .w = pt->w, .h = pt->h, .flip_y = pt->flip_y,
#ifdef T3_EXTERNAL_TARGET_KEEP_MULTISAMPLED
        .keep_multisampled = true, /* the blends and the scene pass go on in the samples */
#endif
      };
      t3_renderer_set_external_encoder(r, (uintptr_t)pt->enc);
      t3_renderer_set_external_target(r, &et);
      t3_renderer_render(r, bg_scene, cam);
      t3_renderer_set_external_target(r, NULL);
    }
    t3_scene_set_background_texture(bg_scene, NULL);
    return;
  }
#endif
  glClearColor(c.r, c.g, c.b, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  t3_renderer_reset_bindings(r);
  t3_renderer_set_framebuffer(r, p->bound_fbo);
  bool auto_clear = t3_renderer_get_auto_clear(r);
  t3_renderer_set_auto_clear(r, false);
  t3_renderer_render(r, bg_scene, cam);
  t3_renderer_set_auto_clear(r, auto_clear);
  t3_renderer_reset_bindings(r);
  glBindFramebuffer(GL_FRAMEBUFFER, p->bound_fbo);
  glViewport(0, 0, cw__vclip.bound_w, cw__vclip.bound_h);
  t3_scene_set_background_texture(bg_scene, NULL);
}

static void blend_all(cw_pass_ctx *ctx) {
  cw_pipeline *p = ctx->pipeline;
  unsigned prog = program();
  if (!prog) return;
  float rc[4];
  run_rect(ctx, rc);
  cw__pass_state_reset();
  cw_pass_use(prog);
  cw_pass_uniform4f(prog, "u_rect", rc[0], rc[1], rc[2], rc[3]);
  cw_pass_uniform1i(prog, "u_src", 0);
  for (int i = 0; i < p->bd_n; i++) {
    const struct cw__backdrop_pic *b = &p->bd[i];
    /* the first over nothing left of the background: written as it is */
    bool first_full = i == 0 && b->opacity >= 1;
    cw_pass_set_blend(first_full ? CW_PASS_BLEND_NONE : CW_PASS_BLEND_PREMUL);
    cw_pass_uniform4f(prog, "u_k", b->scale, b->opacity, 0, 0);
    cw_pass_bind_texture(0, b->tex);
    cw_pass_fullscreen();
  }
  cw_pass_set_blend(CW_PASS_BLEND_NONE);
}

static void pass_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  cw_pipeline *p = ctx->pipeline;
  if (p->bd_n <= 0) return;
  if (p->bd[0].opacity < 1) {
    main_background(ctx);
    cw__pass_state_reset();
  }
  blend_all(ctx);
  p->bd_drawn = true; /* the scene pass keeps this colour and draws no background */
}

/* CW_TEST_BACKDROP_ON_TOP=1 (a gate control): the pictures drawn over the finished scene instead of under it */
static void ontop_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  blend_all(ctx);
}

/* ── environment: the tree's image-based light from its backdrops ──── */

/* Per tree (slot "backdrop_env"): the cube the backdrops' captures are blended into, its PMREM (the scene's
 * environment while a backdrop lights it), and the tree's own environment, kept to blend under them by the share of
 * its background they leave and to give back when no backdrop lights it. */
typedef struct bd_env {
  cw_rt blend[6], own[6];
  int blend_size, own_size;
  t3_scene *own_scene;
  int cube_size;
  unsigned gl_cube, gl_fbo;
#if CW_WGPU
  WGPUTexture wcube;
  WGPUTextureView wcube_v;
#endif
  t3_texture *cube_tex;
  t3_pmrem_generator *pmrem;
  t3_render_target *env;
  t3_texture *own_env; /* retained */
  t3_euler own_rot;
  bool set, built;
  float last_w[CW_BACKDROP_MAX + 1];
  const void *last_src[CW_BACKDROP_MAX + 1];
  const t3_texture *last_own;
  int last_n;
  int32_t updates;
  float cpu_ms;
} bd_env;

static void env_cube_free(bd_env *E) {
  if (E->cube_tex) t3_release(E->cube_tex), E->cube_tex = NULL; /* (the wrapper goes before the texture it wraps) */
  if (E->gl_cube) glDeleteTextures(1, &E->gl_cube), E->gl_cube = 0;
  if (E->gl_fbo) glDeleteFramebuffers(1, &E->gl_fbo), E->gl_fbo = 0;
#if CW_WGPU
  if (E->wcube_v) wgpuTextureViewRelease(E->wcube_v), E->wcube_v = NULL;
  if (E->wcube) wgpuTextureRelease(E->wcube), E->wcube = NULL;
#endif
  E->cube_size = 0;
}
/* everything but the tree's own environment (env_restore gives that back) */
static void env_gpu_free(bd_env *E) {
  if (E->blend_size) faces_free(E->blend), E->blend_size = 0;
  if (E->own_size) faces_free(E->own), E->own_size = 0;
  if (E->env) t3_release(E->env), E->env = NULL;
  env_cube_free(E);
  if (E->pmrem) t3_pmrem_generator_destroy(E->pmrem), E->pmrem = NULL;
  E->built = false;
  E->last_n = -1;
}
static void env_slot_free(void *p) {
  bd_env *E = p;
  env_gpu_free(E);
  if (E->own_scene) t3_release(E->own_scene);
  if (E->own_env) t3_release(E->own_env);
  cw_free(E);
}
static bd_env *env_of(cw_tree *t, bool make) {
  bd_env *E = cw_tree_slot(t, "backdrop_env");
  if (E || !make) return E;
  E = cw_calloc(1, sizeof *E, CW_MEM_RENDER);
  E->last_n = -1;
  cw_tree_set_slot(t, "backdrop_env", E, env_slot_free);
  return E;
}
static bool euler_zero(t3_euler e) { return e.x == 0 && e.y == 0 && e.z == 0; }

/* the tree's own environment back in the scene, and what stood in for it freed */
static void env_restore(bd_env *E, t3_scene *sc) {
  if (E->set && sc) {
    if (E->env && sc->environment == E->env->texture) t3_scene_set_environment(sc, E->own_env);
    sc->environment_rotation = E->own_rot;
  }
  E->set = false;
  if (E->own_env) t3_release(E->own_env), E->own_env = NULL;
  env_gpu_free(E);
}

static unsigned blend_prog;
static unsigned blend_program(void) {
  if (!blend_prog)
    blend_prog = cw__pass_program2("backdrop.env",
      "uniform sampler2D u_src;\nuniform vec4 u_k;\nout vec4 o;\n"
      "void main() { o = vec4(texture(u_src, v_uv).rgb * u_k.x, u_k.y); }\n",
      "struct U { u_k: vec4f };\n"
      "@group(0) @binding(0) var<uniform> u: U;\n"
      "@group(0) @binding(1) var u_src: texture_2d<f32>;\n"
      "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
      "  return vec4f(textureSampleLevel(u_src, cw_linear, uv, 0.0).rgb * u.u_k.x, u.u_k.y);\n}\n");
  return blend_prog;
}

/* the cube (made at this size) holding faces[] (each size x size), wrapped for three.c as a cube target's texture */
static bool env_cube_fill(bd_env *E, const cw_rt *faces, int size, unsigned bound_fbo) {
  bool wg = on_wgpu();
  if (E->cube_size != size) {
    env_cube_free(E);
#if CW_WGPU
    if (wg) {
      E->wcube = cw__wg_texture(WGPUTextureDimension_2D, size, size, 6, 1, WGPUTextureFormat_RGBA16Float,
                                WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc,
                                WGPUTextureViewDimension_Cube, "backdrop env cube");
      E->wcube_v = cw__wg_view(E->wcube, WGPUTextureViewDimension_Cube, 0, 1, 0, 6);
      E->cube_tex = t3_texture_from_wgpu(E->wcube, E->wcube_v, size, size, 1, true, false);
    } else
#endif
    {
      glGenTextures(1, &E->gl_cube);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_CUBE_MAP, E->gl_cube);
      for (int f = 0; f < 6; f++)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F, size, size, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
      glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
      glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
      glGenFramebuffers(1, &E->gl_fbo);
      E->cube_tex = t3_texture_from_gl(E->gl_cube, size, size, 1, true, false);
    }
    if (!E->cube_tex) {
      env_cube_free(E);
      return false;
    }
    /* a cube target's texture, as three.c's own captures are (not a CubeTexture: no mirrored x, made by the GPU) */
    E->cube_tex->needs_flip_env_map = false;
    E->cube_tex->gpu_made = true;
    E->cube_tex->type = T3_HALF_FLOAT_TYPE;
    E->cube_size = size;
  }
#if CW_WGPU
  if (wg) {
    cw__pw_flush(); /* (the blend's passes first) */
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(cw__wgpu_device(), NULL);
    for (int f = 0; f < 6; f++) {
      WGPUTexelCopyTextureInfo s = { .texture = cw__pw_texture(faces[f].tex), .aspect = WGPUTextureAspect_All };
      WGPUTexelCopyTextureInfo d = { .texture = E->wcube, .origin = { 0, 0, (uint32_t)f }, .aspect = WGPUTextureAspect_All };
      wgpuCommandEncoderCopyTextureToTexture(enc, &s, &d, &(WGPUExtent3D){ (uint32_t)size, (uint32_t)size, 1 });
    }
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(cw__wgpu_queue(), 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
    return true;
  }
#endif
  glDisable(GL_SCISSOR_TEST);
  for (int f = 0; f < 6; f++) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, faces[f].fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, E->gl_fbo);
    /* mirrored in x on GL (the faces into face f ^ 1 for the x faces, each read right to left): three.c's GL face cameras
     * into a framebuffer of ours give the cube three.c samples mirrored left for right (its own cube targets are drawn
     * through its WebGPU-shaped programs' conventions); WebGPU's copy needs none */
    int to = f < 2 ? f ^ 1 : f;
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + to, E->gl_cube, 0);
    glBlitFramebuffer(0, 0, size, size, size, 0, 0, size, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, bound_fbo);
  return true;
}

typedef struct env_src { const cw_rt *faces; int size; float w; } env_src;

/* this frame's sources: each lighting backdrop's capture by the share of the picture it has (its opacity times what
 * the backdrops in front of it leave, times its exposure over the tree's), and the tree's own environment by the share
 * of the background they all leave */
static int env_sources(const cw_pipeline *p, env_src *out, int *size, float *rest) {
  int n = 0;
  *size = 16;
  for (int i = 0; i < p->bd_env_n; i++) {
    const struct cw__backdrop_env *e = &p->bd_env[i];
    if (!e->faces) continue;
    float w = p->bd[e->pic].opacity * p->bd[e->pic].scale;
    for (int j = e->pic + 1; j < p->bd_n; j++) w *= 1 - p->bd[j].opacity;
    out[n++] = (env_src){ e->faces, e->size, w };
    *size = CW_MAX(*size, e->size);
  }
  *rest = p->bd_bg_weight;
  return n;
}

static bool env_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_pipeline *p = ctx->pipeline;
  if (p->rt_scope || p->backdrop_run || !ctx->scene3d) return false;
  bd_env *E = env_of(ctx->tree, false);
  return p->bd_env_n > 0 || (E && (E->set || E->own_env));
}

static void env_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  cw_pipeline *p = ctx->pipeline;
  t3_scene *sc = ctx->scene3d;
  t3_renderer *r = ctx->renderer;
  bd_env *E = env_of(ctx->tree, p->bd_env_n > 0);
  if (!E || !sc || !r) return;
  double t0 = cw__render3d_now_ms();
  /* the tree's own environment: what the scene has, unless it is ours (then the one it had) */
  t3_texture *ours = E->env ? E->env->texture : NULL;
  if (!(E->set && ours && sc->environment == ours) && sc->environment != E->own_env) {
    if (sc->environment) t3_retain(sc->environment);
    if (E->own_env) t3_release(E->own_env);
    E->own_env = sc->environment;
  }
  if (!E->set || !euler_zero(sc->environment_rotation)) E->own_rot = sc->environment_rotation;
  if (p->bd_env_n <= 0) {
    env_restore(E, sc);
    return;
  }
  /* this frame's part of each lighting backdrop's capture, by its world's renderer (the main renderer's cached state is
   * stale after) */
  cw_tree *prev = cw_current_tree();
  for (int i = 0; i < p->bd_env_n; i++) {
    struct cw__backdrop_env *e = &p->bd_env[i];
    backdrop *d = e->bd;
    cw_set_current_tree(e->world);
    env_capture(d, e->world, e->camera);
    e->faces = d->cap_have ? d->cap[d->cap_front] : NULL;
    e->size = d->cap_size;
    e->fresh = d->cap_fresh;
  }
  cw_set_current_tree(prev);
  if (!on_wgpu()) gl_frame_state(p->bd_w, p->bd_h), t3_renderer_reset_bindings(r), t3_renderer_reset_state(r), glBindFramebuffer(GL_FRAMEBUFFER, p->bound_fbo);
  env_src src[CW_BACKDROP_MAX + 1];
  int size;
  float rest;
  int n = env_sources(p, src, &size, &rest);
  bool fresh = false;
  for (int i = 0; i < p->bd_env_n; i++) fresh |= p->bd_env[i].fresh;
  bool with_own = E->own_env && rest > 1e-4f;
  /* the tree's own environment as a cube of faces like the captures', its rotation drawn in */
  if (with_own) {
    if (E->own_size != size) {
      if (E->own_size) faces_free(E->own);
      E->own_size = faces_make(E->own, size) ? size : 0;
    }
    if (E->own_size) {
      if (!E->own_scene) E->own_scene = t3_scene_new();
      t3_scene *os = E->own_scene;
      bool pm = E->own_env->mip_levels > 0, equi = !E->own_env->is_cube && !pm;
      t3_scene_set_background_texture(os, E->own_env);
      os->background_intensity = 1;
      os->background_blurriness = equi ? 0.001f : 0; /* (three.c draws an equirect background through its PMREM) */
      os->background_rotation = E->own_rot;
      if (fresh || E->last_own != E->own_env || !E->built) render_faces(r, NULL, os, (t3_vec3){ 0, 0, 0 }, 0.1f, 10, E->own, 0, 6, p->bound_fbo);
      t3_scene_set_background_texture(os, NULL);
      src[n++] = (env_src){ E->own, size, rest };
    }
  }
  /* blend and filter again when a capture finished or the sources changed; a share that moves (a cross-fade) waits for
   * the next capture to finish (every env_every frames), so the filter runs at most once in that many frames */
  bool dirty = !E->built || fresh || n != E->last_n || E->last_own != (with_own ? E->own_env : NULL);
  for (int i = 0; i < n && !dirty; i++) dirty = (src[i].faces != E->last_src[i] && src[i].faces != E->own) || (fabsf(src[i].w - E->last_w[i]) > 1e-5f && !E->last_w[i] != !src[i].w);
  if (dirty) {
    const cw_rt *faces = NULL;
    if (n == 1 && src[0].w == 1 && src[0].size == size) faces = src[0].faces; /* one source, whole: its faces as they are */
    else {
      if (E->blend_size != size) {
        if (E->blend_size) faces_free(E->blend);
        E->blend_size = faces_make(E->blend, size) ? size : 0;
      }
      unsigned prog = blend_program();
      if (E->blend_size && prog) {
        bool clip = cw__vclip.on;
        cw__vclip.on = false;
        for (int f = 0; f < 6; f++) {
          cw_pass_bind_target(ctx, &E->blend[f]);
          cw__pass_state_reset();
          if (!n) cw_pass_clear(0, 0, 0, 1, false);
          cw_pass_use(prog);
          cw_pass_uniform1i(prog, "u_src", 0);
          for (int i = 0; i < n; i++) {
            cw_pass_set_blend(i ? CW_PASS_BLEND_ADD : CW_PASS_BLEND_NONE);
            cw_pass_uniform4f(prog, "u_k", src[i].w, i ? 0.0f : 1.0f, 0, 0);
            cw_pass_bind_texture(0, src[i].faces[f].tex);
            cw_pass_fullscreen();
          }
          cw_pass_set_blend(CW_PASS_BLEND_NONE);
        }
        cw__vclip.on = clip;
        faces = E->blend;
      }
    }
    if (faces && env_cube_fill(E, faces, size, p->bound_fbo)) {
      bool wg = on_wgpu();
      if (!E->pmrem) E->pmrem = t3_pmrem_generator_new(r);
      if (wg) t3_renderer_set_external_encoder(r, 0);
      else t3_renderer_reset_bindings(r);
      t3_render_target *next = t3_pmrem_from_cubemap_into(E->pmrem, E->cube_tex, E->env);
      if (!wg) t3_renderer_reset_bindings(r), glBindFramebuffer(GL_FRAMEBUFFER, p->bound_fbo);
      if (next) {
        if (E->env) t3_release(E->env); /* (into the same target: the reference the call returned stands for it) */
        E->env = next;
        E->built = true;
        E->updates++;
      }
    }
    E->last_n = n;
    E->last_own = with_own ? E->own_env : NULL;
    for (int i = 0; i < n; i++) E->last_src[i] = src[i].faces, E->last_w[i] = src[i].w;
  }
  if (E->env) {
    if (sc->environment != E->env->texture) t3_scene_set_environment(sc, E->env->texture);
    sc->environment_rotation = (t3_euler){ 0 }; /* (the tree's own rotation is drawn into its share) */
    E->set = true;
  }
  if (!on_wgpu()) gl_frame_state(p->bd_w, p->bd_h), glBindFramebuffer(GL_FRAMEBUFFER, p->bound_fbo);
  E->cpu_ms = (float)(cw__render3d_now_ms() - t0);
}

t3_texture *cw__backdrop_env_texture(cw_tree *t) {
  bd_env *E = t ? env_of(t, false) : NULL;
  return E && E->set && E->env && !test_flag("CW_TEST_WATER_BACKDROP_OFF") ? E->env->texture : NULL;
}

/* ── screen-space reflections' view of the backdrops ───────────────── */

static bool ssrc_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_pipeline *p = ctx->pipeline;
  return p->bd_n > 0 && !p->rt_scope && !p->backdrop_run && ctx->scene3d && ctx->settings->ssr && ctx->settings->ssr_backdrop;
}
static void ssrc_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  cw_pipeline *p = ctx->pipeline;
  if (p->bd_ssr_frame == p->frame + 1) return; /* (once a frame, behind every view) */
  unsigned prog = program();
  if (!prog || p->bd_w <= 0 || p->bd_h <= 0) return;
  const cw_rt *rt = cw_pipeline_rt_persistent(p, "backdrop.ssr", (p->bd_w + 1) / 2, (p->bd_h + 1) / 2, CW_RT_RGBA16F, NULL);
  if (!rt) return;
  bool clip = cw__vclip.on;
  cw__vclip.on = false;
  cw_pass_bind_target(ctx, rt);
  cw__pass_state_reset();
  cw_pass_clear(0, 0, 0, 0, false);
  cw_pass_use(prog);
  cw_pass_uniform4f(prog, "u_rect", 0, 0, 1, 1);
  cw_pass_uniform1i(prog, "u_src", 0);
  cw_pass_set_blend(CW_PASS_BLEND_PREMUL);
  for (int i = 0; i < p->bd_n; i++) {
    cw_pass_uniform4f(prog, "u_k", p->bd[i].scale, p->bd[i].opacity, 0, 0);
    cw_pass_bind_texture(0, p->bd[i].tex);
    cw_pass_fullscreen();
  }
  cw_pass_set_blend(CW_PASS_BLEND_NONE);
  cw__vclip.on = clip;
  p->bd_ssr = rt;
  p->bd_ssr_frame = p->frame + 1;
}

bool cw__backdrop_ssr(const cw_pass_ctx *ctx, unsigned *tex, float rect[4]) {
  const cw_pipeline *p = ctx ? ctx->pipeline : NULL;
  if (!p || p->bd_ssr_frame != p->frame + 1 || !p->bd_ssr || p->rt_scope || p->backdrop_run || p->bd_n <= 0) return false;
  if (test_flag("CW_TEST_BACKDROP_SSR_OFF")) return false; /* a gate control: the fallback's pass runs, the trace ignores it */
  *tex = p->bd_ssr->tex;
  if (ctx->view_count > 0) cw_pass_view_uv(ctx, rect);
  else run_rect(ctx, rect);
  return true;
}

static void ensure_pass(cw_tree *t) {
  cw_pipeline *p = cw_pipeline_of(t);
  p->bd_frame = frame_hook;
  if (cw_pipeline_has_pass(p, "backdrop")) return;
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "backdrop", .phase = CW_PHASE_OPAQUE, .order = -1000, .writes = CW_RES_COLOR,
                                           .active = pass_active, .execute = pass_exec, .webgpu = true, .views = CW_PASS_VIEWS_ONCE });
  /* the backdrops' worlds as the tree's environment (after the sky's, before the gi pass reads it), and the pictures as
   * the screen-space reflections see them */
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "backdrop_env", .phase = CW_PHASE_SHADOW, .order = 120, .active = env_active,
                                           .execute = env_exec, .webgpu = true, .views = CW_PASS_VIEWS_ONCE });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "backdrop_ssr", .phase = CW_PHASE_SHADOW, .order = 130, .active = ssrc_active,
                                           .execute = ssrc_exec, .webgpu = true, .views = CW_PASS_VIEWS_ONCE });
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "backdrop_ontop", .phase = CW_PHASE_SKY, .order = 5000, .writes = CW_RES_COLOR,
                                           .active = ontop_active, .execute = ontop_exec, .webgpu = true, .views = CW_PASS_VIEWS_ONCE });
}

/* ── ticking with the owner ────────────────────────────────────────── */

static void fixed_hook(cw_tree *t, double dt, void *u) {
  (void)u;
  bdw *W = cw_tree_slot(t, "backdrop");
  if (!W || cw_tree_paused(t)) return;
  for (int i = 0; i < W->nodes.count; i++) {
    cw_node n = W->nodes.data[i];
    backdrop *d = BD(n);
    if (d && cw_node_tree(n) == t && world_sync(n, d) && d->world && cw_node_visible_in_tree(n)) cw_tree_fixed_update(d->world, dt); /* (hidden: its world pauses) */
  }
}
static void update_hook(cw_tree *t, double dt, void *u) {
  (void)u;
  bdw *W = cw_tree_slot(t, "backdrop");
  if (!W) return;
  for (int i = 0; i < W->nodes.count; i++) {
    cw_node n = W->nodes.data[i];
    backdrop *d = BD(n);
    if (d && cw_node_tree(n) == t && d->world && cw_node_visible_in_tree(n)) cw_tree_update(d->world, dt);
  }
}

/* ── the node type ─────────────────────────────────────────────────── */

static void bd_init(cw_node n, void *data) {
  backdrop *d = data;
  d->self = n;
  d->opacity = 1;
  d->env_resolution = 128;
  d->env_every = 4;
}
static void bd_enter(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  bdw *W = cw_tree_slot(t, "backdrop");
  if (!W) {
    W = cw_calloc(1, sizeof *W, CW_MEM_RENDER);
    cw_tree_set_slot(t, "backdrop", W, bdw_free);
    cw_tree_add_hook(t, 0, 210, fixed_hook, NULL);
    cw_tree_add_hook(t, 1, 210, update_hook, NULL);
  }
  bool have = false;
  for (int i = 0; i < W->nodes.count; i++) have |= W->nodes.data[i].id == n.id;
  if (!have) cw_array_push(&W->nodes, n);
  ensure_pass(t);
}
static void bd_destroy(cw_node n, void *data) {
  (void)n;
  cap_free(data);
  world_free(data);
}
static void bd_prop_changed(cw_node n, void *data, const cw_prop *prop) {
  (void)n, (void)prop;
  backdrop *d = data;
  d->warned = false;
  d->opacity = CW_CLAMP(d->opacity, 0.0f, 1.0f);
}
static void renders_get(cw_node n, void *data, cw_value *o) {
  (void)n;
  *o = cw_val_int(((backdrop *)data)->renders);
}
static void env_captures_get(cw_node n, void *data, cw_value *o) {
  (void)n;
  *o = cw_val_int(((backdrop *)data)->env_captures);
}

static const cw_prop bd_props[] = {
  CW_PROP_FIELD(backdrop, scene, CW_T_STRING, CW_PROP_DEFAULT, "A .cwscene asset instanced into the world it draws (its own camera, lights, environment, fog)."),
  CW_PROP_FIELD(backdrop, camera, CW_T_STRING, CW_PROP_DEFAULT, "The world's Camera3D it is seen through (a path from the root of its scene, e.g. \"Camera\"); empty: the world's current camera."),
  CW_PROP_FIELD_RANGE(backdrop, opacity, CW_T_FLOAT, 0, 1, "0: not drawn and free; 1: covers what is below it. Cross-fade two backdrops by their opacities."),
  CW_PROP_FIELD(backdrop, order, CW_T_INT, CW_PROP_DEFAULT, "Draw order among the backdrops: lower is further back."),
  CW_PROP_ACCESSOR("renders", CW_T_INT, CW_PROP_READONLY, renders_get, NULL, "Pictures rendered so far."),
  CW_PROP_FIELD(backdrop, environment, CW_T_BOOL, CW_PROP_DEFAULT, "Its world lights the scene: captured as a cube from its camera, it is the scene's environment (image-based light: reflections on glossy and rough surfaces), blended with the other backdrops' by their opacities."),
  CW_PROP_FIELD_RANGE(backdrop, env_resolution, CW_T_INT, 16, 512, "The capture's face size in texels (a power of two: rounded down)."),
  CW_PROP_FIELD_RANGE(backdrop, env_every, CW_T_INT, 1, 600, "Frames a capture's six faces are spread over (1: all six every frame); the lower effects presets spread them wider."),
  CW_PROP_ACCESSOR("env_captures", CW_T_INT, CW_PROP_READONLY, env_captures_get, NULL, "Environment captures finished so far."),
  CW_PROP_END,
};
static cw_node_type bd_type = {
  .name = "Backdrop", .parent = "Node", .data_size = sizeof(backdrop), .props = bd_props,
  .doc = "A 3D world of its own (its own camera, lights, sky and fog) drawn behind the scene in place of its background, with an opacity: cross-fade backdrops while the scene stays on top.",
  .init = bd_init, .enter_tree = bd_enter, .destroy = bd_destroy, .prop_changed = bd_prop_changed,
};

/* ── debug channel ─────────────────────────────────────────────────── */

static void cmd_backdrop(int argc, const char **argv, cw_reply *r) {
  (void)argc, (void)argv;
  cw_tree *t = cw_current_tree();
  bdw *W = t ? cw_tree_slot(t, "backdrop") : NULL;
  cw_pipeline *p = t ? cw_pipeline_of(t) : NULL;
  cw_reply_printf(r, "{\"drawn\":%d,\"background_weight\":%.4f,\"backdrops\":[", p ? p->bd_n : 0, p ? (double)p->bd_bg_weight : 1.0);
  for (int i = 0; W && i < W->nodes.count; i++) {
    cw_node n = W->nodes.data[i];
    backdrop *d = BD(n);
    if (!d) continue;
    if (i) cw_reply_raw(r, ",");
    cw_reply_raw(r, "{\"name\":");
    cw_reply_str(r, cw_node_name(n));
    cw_reply_raw(r, ",\"scene\":");
    cw_reply_str(r, d->scene ? d->scene : "");
    cw_reply_printf(r, ",\"opacity\":%.4f,\"order\":%d,\"renders\":%d,\"passes\":%d,\"cpu_ms\":%.3f", (double)d->opacity, (int)d->order,
                    (int)d->renders, d->ran_passes, (double)d->cpu_ms);
    cw_reply_printf(r, ",\"environment\":%s,\"env_size\":%d,\"env_every\":%d,\"env_captures\":%d,\"env_cpu_ms\":%.3f}", d->environment ? "true" : "false",
                    d->cap_size, d->environment ? env_every_now(d) : 0, (int)d->env_captures, (double)d->env_cpu_ms);
  }
  bd_env *E = t ? env_of(t, false) : NULL;
  cw_reply_printf(r, "],\"environment\":{\"set\":%s,\"updates\":%d,\"cube\":%d,\"cpu_ms\":%.3f}}", E && E->set ? "true" : "false",
                  E ? (int)E->updates : 0, E ? E->cube_size : 0, E ? (double)E->cpu_ms : 0.0);
}

void cw__register_backdrop_types(void) {
  cw_node_type_register(&bd_type);
  bd_t = &bd_type;
  cw_debug_command("backdrop", "backdrop: the Backdrop nodes (opacity, order, renders, passes their world ran, CPU ms)", cmd_backdrop);
}
