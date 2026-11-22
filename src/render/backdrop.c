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
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "pipeline_internal.h"
#include "scene/render3d_internal.h"
#if CW_WGPU
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
  p->bd_bg_weight = 1;
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
    if (!have || !(d->opacity > 0) || n >= (int)CW_ARRAY_LEN(list)) continue; /* opacity 0: nothing at all */
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
  }
  /* the main tree's renderer draws next: its cached GL state is stale */
  t3_renderer *mr = cw_render3d_renderer(t);
  if (mr) t3_renderer_reset_bindings(mr);
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

static void ensure_pass(cw_tree *t) {
  cw_pipeline *p = cw_pipeline_of(t);
  p->bd_frame = frame_hook;
  if (cw_pipeline_has_pass(p, "backdrop")) return;
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "backdrop", .phase = CW_PHASE_OPAQUE, .order = -1000, .writes = CW_RES_COLOR,
                                           .active = pass_active, .execute = pass_exec, .webgpu = true, .views = CW_PASS_VIEWS_ONCE });
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
    if (d && cw_node_tree(n) == t && world_sync(n, d) && d->world) cw_tree_fixed_update(d->world, dt);
  }
}
static void update_hook(cw_tree *t, double dt, void *u) {
  (void)u;
  bdw *W = cw_tree_slot(t, "backdrop");
  if (!W) return;
  for (int i = 0; i < W->nodes.count; i++) {
    cw_node n = W->nodes.data[i];
    backdrop *d = BD(n);
    if (d && cw_node_tree(n) == t && d->world) cw_tree_update(d->world, dt);
  }
}

/* ── the node type ─────────────────────────────────────────────────── */

static void bd_init(cw_node n, void *data) {
  backdrop *d = data;
  d->self = n;
  d->opacity = 1;
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

static const cw_prop bd_props[] = {
  CW_PROP_FIELD(backdrop, scene, CW_T_STRING, CW_PROP_DEFAULT, "A .cwscene asset instanced into the world it draws (its own camera, lights, environment, fog)."),
  CW_PROP_FIELD(backdrop, camera, CW_T_STRING, CW_PROP_DEFAULT, "The world's Camera3D it is seen through (a path from the root of its scene, e.g. \"Camera\"); empty: the world's current camera."),
  CW_PROP_FIELD_RANGE(backdrop, opacity, CW_T_FLOAT, 0, 1, "0: not drawn and free; 1: covers what is below it. Cross-fade two backdrops by their opacities."),
  CW_PROP_FIELD(backdrop, order, CW_T_INT, CW_PROP_DEFAULT, "Draw order among the backdrops: lower is further back."),
  CW_PROP_ACCESSOR("renders", CW_T_INT, CW_PROP_READONLY, renders_get, NULL, "Pictures rendered so far."),
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
    cw_reply_printf(r, ",\"opacity\":%.4f,\"order\":%d,\"renders\":%d,\"passes\":%d,\"cpu_ms\":%.3f}", (double)d->opacity, (int)d->order,
                    (int)d->renders, d->ran_passes, (double)d->cpu_ms);
  }
  cw_reply_raw(r, "]}");
}

void cw__register_backdrop_types(void) {
  cw_node_type_register(&bd_type);
  bd_t = &bd_type;
  cw_debug_command("backdrop", "backdrop: the Backdrop nodes (opacity, order, renders, passes their world ran, CPU ms)", cmd_backdrop);
}
