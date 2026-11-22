/* SubViewport (cartwheel/subviewport.h): a picture rendered into a texture,
 * either through a Camera3D of the node's own tree (an extra view of the
 * tree's pipeline, pipeline.c) or of a world the node owns (a tree of its
 * own with its own pipeline, rendered into the target as the editor renders a
 * document). The target is a pipeline cw_rt; three.c samples it directly, 2D
 * and UI draw a copy in a render2d target. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/gpu.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/subviewport.h"
#include "pipeline_internal.h"
#include "quality_internal.h"
#include "scene/render3d_internal.h"
#include "pipeline_wgpu.h"

#define MAX_SIZE 4096

typedef struct subvp {
  /* properties */
  cw_vec2 size;
  int32_t update_mode, update_every;
  const char *camera, *scene, *texture_name;
  /* runtime */
  cw_node self;
  int slot;                 /* index among every SubViewport made (the pipeline's target scope, the stagger) */
  cw_texture tex;           /* the picture: a render2d target (2D and UI draw it as it is) */
  int w, h;
  cw_rt *lin;               /* the picture decoded to linear light, for three.c materials (made on first ask) */
  unsigned src_id;          /* WebGPU: the target registered for the decode pass to sample */
  t3_texture *t3;           /* three.c's texture over lin */
  cw_tree *world;           /* a world of its own */
  cw_node world_root;
  const char *world_scene;  /* the scene instanced into it */
  bool world_failed;
  int32_t renders;
  bool once_pending, queued, warned;
  uint64_t visits;          /* frames this viewport was considered for a render (its own clock) */
  uint64_t used2d, used3d;  /* visits + 1 when the texture was last shown by 2D / UI, by a mesh in view (0: never) */
  uint64_t due_count;       /* due frames so far (the quality presets skip some) */
  float cpu_ms;
} subvp;

static const cw_node_type *subvp_t;
static CW_ARRAY(cw_node) all; /* every SubViewport in a tree, any tree: names are global */
static int next_slot;

static subvp *SV(cw_node n) { return cw_node_data_t(n, subvp_t); }
static bool wgpu(void) { return cw_gpu_backend() == CW_GPU_WEBGPU; }

static const char *name_of(cw_node n, const subvp *d) {
  return d->texture_name && d->texture_name[0] ? d->texture_name : cw_intern(cw_node_name(n));
}

cw_node cw_subviewport_find(const char *ref) {
  if (!ref) return (cw_node){ 0 };
  if (!strncmp(ref, "viewport://", 11)) ref += 11;
  const char *k = cw_intern(ref);
  for (int i = 0; i < all.count; i++) {
    subvp *d = SV(all.data[i]);
    if (d && name_of(all.data[i], d) == k) return all.data[i];
  }
  return (cw_node){ 0 };
}

/* ── targets ───────────────────────────────────────────────────────── */

static int dim(float v) { return (int)CW_CLAMP(v + 0.5f, 1, MAX_SIZE); }

void *cw__r2d_target_wgpu_texture(cw_texture h); /* src/render2d/texture.c */
unsigned cw__r2d_target_gl_array(cw_texture h);

static void src_drop(subvp *d) {
#if CW_WGPU
  if (d->src_id) cw__pw_unregister(d->src_id);
#endif
  d->src_id = 0;
}

/* The linear copy at the target's size, and three.c's texture onto it. three.c samples an embedder's texture as
 * it is, so the display-encoded picture is decoded once here (finish) instead of in every material. Half float
 * where the host renders it (8 bits of linear light band in the darks). */
static void t3_point(subvp *d) {
  if (!d->t3 && !d->lin) return;
  if (d->lin && d->lin->w == d->w && d->lin->h == d->h) return;
  cw_rt *old = d->lin;
  d->lin = cw_rt_new(d->w, d->h, cw_pipeline_caps_get()->float_render ? CW_RT_RGBA16F : CW_RT_RGBA8, false);
  if (d->t3 && d->lin) { /* before the old one is freed */
#if CW_WGPU
    if (wgpu()) t3_texture_adopt_wgpu(d->t3, cw__pw_texture(d->lin->tex), cw__pw_view(d->lin->tex), d->w, d->h, 1, false);
    else
#endif
      t3_texture_adopt_gl(d->t3, d->lin->tex, d->w, d->h, 1, false);
  }
  cw_rt_free(old);
}

/* the target at the size the properties ask for (needs the GPU); false before there is one */
static bool ensure_rt(subvp *d) {
  int w = dim(d->size.x), h = dim(d->size.y);
  if (cw_texture_valid(d->tex) && d->w == w && d->h == h) return true;
  if (!cw_texture_valid(d->tex)) {
    d->tex = cw_target_create(&(cw_target_desc){ .width = w, .height = h, .filter = CW_FILTER_LINEAR });
    if (!cw_texture_valid(d->tex)) return false;
  } else {
    cw_target_resize(d->tex, w, h);
  }
  d->w = w;
  d->h = h;
  src_drop(d); /* the target's storage is new */
  t3_point(d);
  d->once_pending = true; /* a new target is empty: every mode but DISABLED fills it */
  return true;
}

static void free_gpu(subvp *d) {
  if (d->t3) t3_release(d->t3);
  d->t3 = NULL;
  cw_rt_free(d->lin);
  d->lin = NULL;
  src_drop(d);
  if (cw_texture_valid(d->tex)) cw_texture_destroy(d->tex);
  d->tex = (cw_texture){ 0 };
}

cw_texture cw_subviewport_texture(cw_node n) {
  subvp *d = SV(n);
  if (!d || !ensure_rt(d)) return (cw_texture){ 0 };
  d->used2d = d->visits + 1;
  return d->tex;
}

cw_texture cw__subviewport_texture_by_ref(const char *ref) { return cw_subviewport_texture(cw_subviewport_find(ref)); }

t3_texture *cw_subviewport_t3_texture(cw_node n) {
  subvp *d = SV(n);
  if (!d || !ensure_rt(d)) return NULL;
  if (!d->t3) {
    d->lin = cw_rt_new(d->w, d->h, cw_pipeline_caps_get()->float_render ? CW_RT_RGBA16F : CW_RT_RGBA8, false);
    if (!d->lin) return NULL;
#if CW_WGPU
    if (wgpu()) d->t3 = t3_texture_from_wgpu(cw__pw_texture(d->lin->tex), cw__pw_view(d->lin->tex), d->w, d->h, 1, false, false);
    else
#endif
      d->t3 = t3_texture_from_gl(d->lin->tex, d->w, d->h, 1, false, false);
    d->once_pending = true; /* the copy is filled by a render */
  }
  return d->t3;
}

/* is a sphere at the node (radius in its own units) inside a camera's view this frame */
static bool in_view(cw_node user, float radius) {
  cw_tree *t = cw_node_tree(user);
  if (!t) return false;
  for (cw_node p = user; !CW_HANDLE_IS_NULL(p); p = cw_node_parent(p)) {
    const cw_node3d *nd = cw_node_data(p, "Node3D");
    if (nd && !nd->visible) return false;
  }
  const t3_mat4 *g = cw_node3d_global_transform(user);
  if (!g) return true;
  const float *e = g->e;
  float sx = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]), sy = sqrtf(e[4] * e[4] + e[5] * e[5] + e[6] * e[6]),
        sz = sqrtf(e[8] * e[8] + e[9] * e[9] + e[10] * e[10]);
  t3_sphere s = { { e[12], e[13], e[14] }, radius * CW_MAX(sx, CW_MAX(sy, sz)) };
  cw_pipeline *p = cw_pipeline_of(t);
  const cw_view *views;
  int nv = cw_pipeline_views(p, &views);
  for (int i = 0; i < CW_MAX(nv, 1); i++) {
    t3_camera *c = cw_camera3d_t3(nv ? views[i].camera : cw_render3d_camera(t));
    if (!c) continue;
    t3_mat4 vp;
    t3_frustum f;
    t3_mat4_multiply(&vp, &c->projection_matrix, &c->matrix_world_inverse);
    t3_frustum_from_matrix(&f, &vp);
    if (t3_frustum_intersects_sphere(&f, s)) return true;
  }
  return false;
}

/* MeshInstance3D (render3d.c): the texture a `viewport_texture` property names; user and radius say where it is shown */
t3_texture *cw__subviewport_t3_by_ref(const char *ref, cw_node user, float radius) {
  cw_node n = cw_subviewport_find(ref);
  subvp *d = SV(n);
  if (!d) return NULL;
  t3_texture *t = cw_subviewport_t3_texture(n);
  if (t && d->used3d != d->visits + 1 && in_view(user, radius)) d->used3d = d->visits + 1;
  return t;
}

/* ── a world of its own ────────────────────────────────────────────── */

static void world_free(subvp *d) {
  if (d->world) cw_tree_free(d->world);
  d->world = NULL;
  d->world_root = (cw_node){ 0 };
  d->world_scene = NULL;
  d->world_failed = false;
}

static cw_tree *world_make(subvp *d) {
  if (d->world) return d->world;
  cw_tree *prev = cw_current_tree();
  d->world = cw_tree_new();
  cw_set_current_tree(prev); /* cw_tree_new takes the seat only when it is empty: never ours */
  return d->world;
}

/* the `scene` property into the world; false when there is none or it failed (logged once) */
static bool world_sync(cw_node n, subvp *d) {
  bool want = d->scene && d->scene[0];
  if (!want) {
    if (d->world_scene) world_free(d); /* the property was cleared: the instanced world goes */
    return d->world != NULL;           /* a world game code built (cw_subviewport_tree) stays */
  }
  if (d->world && d->world_scene == d->scene) return !d->world_failed;
  world_free(d);
  world_make(d);
  d->world_scene = d->scene;
  cw_node root = cw_scene_instance_path(d->scene);
  if (!cw_node_valid(root)) {
    cw_error("SubViewport '%s': scene '%s' could not be loaded (an asset path, e.g. \"portrait.cwscene\")", cw_node_name(n), d->scene);
    d->world_failed = true;
    return false;
  }
  cw_node_add_child(cw_tree_root(d->world), root);
  d->world_root = root;
  return true;
}

cw_tree *cw_subviewport_tree(cw_node n) {
  subvp *d = SV(n);
  if (!d) return NULL;
  world_sync(n, d);
  return world_make(d);
}
cw_node cw_subviewport_root(cw_node n) {
  subvp *d = SV(n);
  if (!d) return (cw_node){ 0 };
  world_sync(n, d);
  return d->world_root;
}

/* ── rendering ─────────────────────────────────────────────────────── */

/* the display-encoded target into the linear copy (sRGB EOTF), rows as they are */
static void decode(subvp *d) {
  static unsigned prog;
  if (!prog)
    prog = cw__pass_program2("subviewport.decode",
      "uniform highp sampler2DArray u_src;\nout vec4 o;\n"
      "vec3 lin(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }\n"
      "void main() { o = vec4(lin(texture(u_src, vec3(v_uv, 0.0)).rgb), 1.0); }\n",
      "@group(0) @binding(1) var u_src: texture_2d<f32>;\n"
      "fn lin(c: vec3f) -> vec3f { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3f(2.4)), step(vec3f(0.04045), c)); }\n"
      "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
      "  return vec4f(lin(textureSampleLevel(u_src, cw_linear, uv, 0.0).rgb), 1.0);\n}\n");
  if (!prog) return;
  cw_tree *mine = cw_node_tree(d->self);
  cw_pass_ctx ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.tree = mine;
  ctx.pipeline = mine ? cw_pipeline_of(mine) : NULL;
  ctx.width = d->w;
  ctx.height = d->h;
  ctx.output = d->lin;
  cw_pass_bind_target(&ctx, d->lin);
  cw__pass_state_reset();
  cw_pass_use(prog);
#if CW_WGPU
  if (wgpu()) {
    if (!d->src_id) {
      WGPUTexture tex = cw__r2d_target_wgpu_texture(d->tex);
      WGPUTextureView view = wgpuTextureCreateView(tex, &(WGPUTextureViewDescriptor){
        .format = wgpuTextureGetFormat(tex), .dimension = WGPUTextureViewDimension_2D, .mipLevelCount = 1, .arrayLayerCount = 1,
        .aspect = WGPUTextureAspect_All, .usage = WGPUTextureUsage_TextureBinding });
      d->src_id = cw__pw_register(tex, view);
    }
    cw_pass_bind_texture(0, d->src_id);
    cw_pass_fullscreen();
    cw__pw_flush();
    return;
  }
#endif
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, cw__r2d_target_gl_array(d->tex));
  cw_pass_uniform1i(prog, "u_src", 0);
  cw_pass_fullscreen();
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, cw__gl_window_fbo);
  t3_renderer *mr = mine ? cw_render3d_renderer(mine) : NULL;
  if (mr) t3_renderer_reset_bindings(mr);
}

/* after a render: count it, and refresh three.c's linear copy when a material wears the picture */
static void finish(subvp *d) {
  d->queued = false;
  d->renders++;
  d->once_pending = false;
  if (d->t3 && d->lin) decode(d);
  cw_emit(d->self, "rendered", NULL, 0);
}
static subvp *by_slot(int slot) {
  for (int i = 0; i < all.count; i++) {
    subvp *d = SV(all.data[i]);
    if (d && d->slot == slot) return d;
  }
  return NULL;
}
static void finish_cb(void *user, float cpu_ms) {
  subvp *d = by_slot((int)(intptr_t)user);
  if (!d) return;
  d->cpu_ms = cpu_ms;
  finish(d);
}

static double now_ms(void) { return cw__render3d_now_ms(); }

static bool find_type(cw_node n, void *user) {
  struct { const char *type; cw_node found; } *q = user;
  if (!cw_node_valid(q->found) && cw_node_is(n, q->type)) q->found = n;
  return !cw_node_valid(q->found);
}
static cw_node first_of(cw_tree *w, const char *type) {
  struct { const char *type; cw_node found; } q = { type, { 0 } };
  cw_node_walk(cw_tree_root(w), find_type, &q);
  return q.found;
}

/* The world into the target: its 3D through its own pipeline (as the editor renders a document into a target),
 * then its 2D canvas over that, or alone on the canvas colour when the world has no 3D camera. */
static void render_world(subvp *d, double alpha) {
  cw_tree *w = d->world;
  cw_tree *prev = cw_current_tree();
  cw_set_current_tree(w);
  cw_tree_pre_render(w, alpha);
  bool has3d = cw_node_valid(cw_render3d_camera(w));
  if (has3d) {
    t3_renderer *r = cw_render3d_renderer(w);
    if (r) t3_renderer_reset_bindings(r);
    cw_pipeline *p = cw_pipeline_of(w);
    if (cw_pipeline_has_pass(p, "canvas")) cw_pipeline_set_pass_enabled(p, "canvas", false); /* the 2D goes into the target below */
    cw_pipeline_set_output(p, cw_target_rt(d->tex));
    cw_pipeline_render(w, d->w, d->h, alpha);
    cw_pipeline_set_output(p, NULL);
    r = cw_render3d_renderer(w);
    if (r) t3_renderer_reset_bindings(r);
  }
  cw_canvas_view view = { .target = d->tex, .clear = !has3d, .clear_color = CW_BLACK };
  cw_value cc;
  cw_node cs = has3d ? (cw_node){ 0 } : first_of(w, "CanvasSettings");
  if (cw_node_valid(cs) && cw_node_get_prop(cs, "clear_color", &cc)) view.clear_color = (cw_color){ cc.v[0], cc.v[1], cc.v[2], cc.v[3] };
  cw_canvas_draw_view(w, &view, alpha);
  cw_set_current_tree(prev);
  /* the tree that shows the picture draws next: its renderer's cached GL state is stale too */
  cw_tree *mine = cw_node_tree(d->self);
  t3_renderer *mr = mine ? cw_render3d_renderer(mine) : NULL;
  if (mr) t3_renderer_reset_bindings(mr);
}

/* frames between renders the effects preset adds (High and up: none) */
static int quality_every(void) {
  int e = cw__quality_effects();
  return e <= CW_QUALITY_VERY_LOW ? 3 : e == CW_QUALITY_LOW ? 2 : 1;
}

static bool due(subvp *d) {
  d->visits++;
  if (d->update_mode == CW_SUBVIEWPORT_DISABLED || d->queued) return false;
  if (d->once_pending) return true;
  if (d->update_mode == CW_SUBVIEWPORT_ONCE) return false;
  /* shown since the last time this was asked: a mesh in view this frame, a 2D node or control last frame */
  if (d->update_mode == CW_SUBVIEWPORT_WHEN_VISIBLE && d->used2d < d->visits && d->used3d < d->visits) return false;
  int every = CW_MAX(d->update_every, 1) * quality_every();
  return (d->due_count++ + (uint64_t)d->slot) % (uint64_t)every == 0;
}

typedef struct vpw { CW_ARRAY(cw_node) nodes; } vpw;
static void vpw_free(void *p) {
  vpw *W = p;
  cw_array_free(&W->nodes);
  cw_free(W);
}

/* pre_render, after every node: render the worlds, queue the camera views */
static void render_hook(cw_tree *t, double alpha, void *u) {
  (void)u;
  vpw *W = cw_tree_slot(t, "subviewport");
  if (!W) return;
  for (int i = 0; i < W->nodes.count; i++) {
    cw_node n = W->nodes.data[i];
    subvp *d = SV(n);
    if (!d || cw_node_tree(n) != t) { cw_array_remove_swap(&W->nodes, i); i--; continue; }
    bool own = world_sync(n, d);
    if (!own && !(d->camera && d->camera[0])) {
      if (!d->warned && d->update_mode != CW_SUBVIEWPORT_DISABLED && !d->world_failed)
        cw_warn("SubViewport '%s' has nothing to render: set `camera` (a Camera3D of this scene) or `scene` (a world of its own)", cw_node_name(n));
      d->warned = true;
      continue;
    }
    /* hidden by itself or an ancestor (a Node3D, Node2D or CanvasLayer above it): no render; what it drew last stays
     * in its texture, and its world (below) pauses */
    if (!cw_node_visible_in_tree(n)) continue;
    if (!ensure_rt(d) || !due(d)) continue;
    if (own) {
      double t0 = now_ms();
      render_world(d, alpha);
      d->cpu_ms = (float)(now_ms() - t0);
      finish(d);
      continue;
    }
    cw_node cam = cw_node_get(n, d->camera);
    if (!cw_node_is(cam, "Camera3D")) {
      if (!d->warned) cw_warn("SubViewport '%s': camera '%s' is not a Camera3D (a path from the SubViewport)", cw_node_name(n), d->camera);
      d->warned = true;
      continue;
    }
    if (cw__pipeline_queue_view(cw_pipeline_of(t), cam, cw_target_rt(d->tex), d->slot, finish_cb, (void *)(intptr_t)d->slot)) d->queued = true;
    else if (!d->warned) {
      d->warned = true;
      cw_warn("SubViewport '%s': more camera views than the pipeline draws in one frame (16); it waits for a free frame", cw_node_name(n));
    }
  }
}

/* the worlds tick with the tree that owns them */
static void fixed_hook(cw_tree *t, double dt, void *u) {
  (void)u;
  vpw *W = cw_tree_slot(t, "subviewport");
  if (!W || cw_tree_paused(t)) return;
  for (int i = 0; i < W->nodes.count; i++) {
    subvp *d = SV(W->nodes.data[i]);
    if (d && d->world && cw_node_visible_in_tree(W->nodes.data[i])) cw_tree_fixed_update(d->world, dt);
  }
}
static void update_hook(cw_tree *t, double dt, void *u) {
  (void)u;
  vpw *W = cw_tree_slot(t, "subviewport");
  if (!W) return;
  for (int i = 0; i < W->nodes.count; i++) {
    subvp *d = SV(W->nodes.data[i]);
    if (d && d->world && cw_node_visible_in_tree(W->nodes.data[i])) cw_tree_update(d->world, dt);
  }
}

void cw_subviewport_request_update(cw_node n) {
  subvp *d = SV(n);
  if (d) d->once_pending = true;
}
int cw_subviewport_renders(cw_node n) {
  subvp *d = SV(n);
  return d ? d->renders : 0;
}
float cw_subviewport_last_cpu_ms(cw_node n) {
  subvp *d = SV(n);
  return d ? d->cpu_ms : 0;
}

/* ── the node type ─────────────────────────────────────────────────── */

static void vp_init(cw_node n, void *data) {
  subvp *d = data;
  d->self = n;
  d->size = cw_v2(512, 512);
  d->update_mode = CW_SUBVIEWPORT_ALWAYS;
  d->update_every = 1;
  d->once_pending = true;
  d->slot = next_slot++;
}
static void vp_enter(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  vpw *W = cw_tree_slot(t, "subviewport");
  if (!W) {
    W = cw_calloc(1, sizeof *W, CW_MEM_RENDER);
    cw_tree_set_slot(t, "subviewport", W, vpw_free);
    cw_tree_add_hook(t, 0, 200, fixed_hook, NULL);
    cw_tree_add_hook(t, 1, 200, update_hook, NULL);
    cw_tree_add_hook(t, 2, 200, render_hook, NULL);
  }
  bool have = false;
  for (int i = 0; i < W->nodes.count; i++) have |= W->nodes.data[i].id == n.id;
  if (!have) cw_array_push(&W->nodes, n);
  cw_array_push(&all, n);
}
static void vp_exit(cw_node n, void *data) {
  subvp *d = data;
  for (int i = 0; i < all.count; i++)
    if (all.data[i].id == n.id) { cw_array_remove_swap(&all, i); break; }
  d->queued = false;
}
static void vp_destroy(cw_node n, void *data) {
  subvp *d = data;
  for (int i = 0; i < all.count; i++)
    if (all.data[i].id == n.id) { cw_array_remove_swap(&all, i); break; }
  world_free(d);
  free_gpu(d);
}
static void vp_prop_changed(cw_node n, void *data, const cw_prop *prop) {
  (void)n;
  subvp *d = data;
  d->warned = false;
  if (!strcmp(prop->name, "update_mode") && d->update_mode == CW_SUBVIEWPORT_ONCE) d->once_pending = true;
  if (!strcmp(prop->name, "camera") || !strcmp(prop->name, "scene")) d->once_pending = true;
}
static void renders_get(cw_node n, void *data, cw_value *o) {
  (void)n;
  *o = cw_val_int(((subvp *)data)->renders);
}

static const char *const mode_names[] = { "DISABLED", "ONCE", "ALWAYS", "WHEN_VISIBLE", NULL };
static const cw_prop vp_props[] = {
  CW_PROP_FIELD(subvp, size, CW_T_VEC2, CW_PROP_DEFAULT, "The texture's size in pixels (1 to 4096 each way)."),
  { .name = "update_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(subvp, update_mode), .size = sizeof(int32_t),
    .enum_names = mode_names,
    .doc = "When it renders: ALWAYS each frame, WHEN_VISIBLE each frame something showed its texture, ONCE one render then it holds the picture, DISABLED never." },
  CW_PROP_FIELD_RANGE(subvp, update_every, CW_T_INT, 1, 600, "Frames between renders (ALWAYS, WHEN_VISIBLE); the Low and Very Low effects presets double and triple it."),
  CW_PROP_FIELD(subvp, camera, CW_T_STRING, CW_PROP_DEFAULT, "Camera3D of this scene (path from this node) whose view it renders: a monitor, a mirror, a minimap."),
  CW_PROP_FIELD(subvp, scene, CW_T_STRING, CW_PROP_DEFAULT, "A .cwscene asset instanced into a world of its own (its own cameras, lights, environment and 2D canvas); wins over `camera`."),
  CW_PROP_FIELD(subvp, texture_name, CW_T_STRING, CW_PROP_DEFAULT, "The name in \"viewport://Name\" texture references; empty: this node's name."),
  CW_PROP_ACCESSOR("renders", CW_T_INT, CW_PROP_READONLY, renders_get, NULL, "Renders so far."),
  CW_PROP_END,
};
static const char *const vp_signals[] = { "rendered", NULL };
static cw_node_type subvp_type = {
  .name = "SubViewport", .parent = "Node", .data_size = sizeof(subvp), .props = vp_props, .signals = vp_signals,
  .doc = "Renders a camera of this scene, or a scene of its own, into a texture that 2D nodes, UI controls (\"viewport://Name\") and 3D materials (MeshInstance3D.viewport_texture) show.",
  .init = vp_init, .enter_tree = vp_enter, .exit_tree = vp_exit, .destroy = vp_destroy, .prop_changed = vp_prop_changed,
};

/* ── debug channel ─────────────────────────────────────────────────── */

static void reply_one(cw_reply *r, cw_node n, subvp *d) {
  cw_reply_raw(r, "{\"name\":");
  cw_reply_str(r, name_of(n, d));
  cw_reply_printf(r, ",\"size\":[%d,%d],\"mode\":\"%s\",\"source\":\"%s\",\"renders\":%d,\"cpu_ms\":%.3f,\"shown_2d\":%s,\"shown_3d\":%s}",
                  dim(d->size.x), dim(d->size.y), mode_names[CW_CLAMP(d->update_mode, 0, 3)],
                  d->world ? "scene" : d->camera && d->camera[0] ? "camera" : "none", d->renders, (double)d->cpu_ms,
                  d->used2d && d->used2d >= d->visits ? "true" : "false", d->used3d && d->used3d >= d->visits ? "true" : "false");
}

static void cmd_subviewport(int argc, const char **argv, cw_reply *r) {
  if (argc > 1) {
    cw_tree *t = cw_current_tree();
    cw_node n = t ? cw_node_get(cw_tree_root(t), argv[1]) : (cw_node){ 0 };
    if (!cw_node_valid(n)) n = cw_subviewport_find(argv[1]);
    subvp *d = SV(n);
    if (!d) { cw_reply_error(r, "'%s' is not a SubViewport (a node path or a viewport name; try: subviewport)", argv[1]); return; }
    if (argc > 2 && !strcmp(argv[2], "update")) cw_subviewport_request_update(n);
    reply_one(r, n, d);
    return;
  }
  cw_reply_raw(r, "{\"viewports\":[");
  for (int i = 0; i < all.count; i++) {
    subvp *d = SV(all.data[i]);
    if (!d) continue;
    if (i) cw_reply_raw(r, ",");
    reply_one(r, all.data[i], d);
  }
  cw_reply_raw(r, "]}");
}

void cw__register_subviewport_types(void) {
  cw_node_type_register(&subvp_type);
  subvp_t = &subvp_type;
  cw_debug_command("subviewport", "subviewport [<node or name> [update]]: SubViewports (size, mode, renders, CPU ms, whether the texture was shown)",
                   cmd_subviewport);
}
