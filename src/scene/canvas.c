/* The tree's 2D pass: walk the tree once, collect canvas items with their
 * composed transforms, modulate and z, group them into canvases (default
 * canvas, CanvasLayers, ParallaxLayers), map canvases to render2d layers and
 * submit. Then the overlay passes (UI). See cartwheel/scene2d.h. */
#include <stdlib.h>

#include "scene2d_internal.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"

#define MAX_ITEM_TYPES 64
#define MAX_CANVASES 30

typedef struct item_type {
  const cw_node_type *type;
  cw_canvas_draw_fn fn;
} item_type;
static item_type item_types[MAX_ITEM_TYPES];
static int item_type_count;
static int16_t resolved[256]; /* per node type index: 0 unknown, -1 none, k+1 */

void cw_canvas_register_type(const char *type_name, cw_canvas_draw_fn fn) {
  const cw_node_type *t = cw_node_type_find(type_name);
  CW_ASSERT(t, "cw_canvas_register_type: register node type '%s' first", type_name);
  for (int i = 0; i < item_type_count; i++)
    if (item_types[i].type == t) {
      item_types[i].fn = fn; /* hot reload */
      return;
    }
  CW_ASSERT(item_type_count < MAX_ITEM_TYPES, "too many canvas item types");
  item_types[item_type_count++] = (item_type){ t, fn };
  memset(resolved, 0, sizeof resolved);
}

static const item_type *item_type_of(const cw_node_type *t) {
  if (t->index >= 256) return NULL;
  int16_t k = resolved[t->index];
  if (k == 0) {
    k = -1;
    for (const cw_node_type *c = t; c && k < 0; c = c->base)
      for (int i = 0; i < item_type_count; i++)
        if (item_types[i].type == c) { k = (int16_t)(i + 1); break; }
    resolved[t->index] = k;
  }
  return k > 0 ? &item_types[k - 1] : NULL;
}

/* ── per-tree state ──────────────────────────────────────────────────── */

typedef struct canvas_ctx {
  int layer, seq;
  bool follow, y_sort, lit;
  cw_vec2 parallax;
  int r2d;
} canvas_ctx;

typedef struct item {
  cw_node node;
  const item_type *it;
  int ctx, z;
  cw_xform2d xf;
  cw_color mod;
} item;

typedef struct pass {
  int order;
  cw_canvas_pass_fn fn;
  void *user;
} pass;

typedef struct canvas_state {
  CW_ARRAY(item) items;
  CW_ARRAY(canvas_ctx) ctxs;
  CW_ARRAY(pass) passes;
  CW_ARRAY(cw_node) cameras; /* Camera2D nodes in the tree (enter order) */
  bool hook_added;
  cw_node camera;   /* found by the last walk */
  cw_node settings; /* first CanvasSettings */
  cw_camera2d cam;  /* the value the last frame used */
  bool has_cam;
  cw_rect viewport;
  cw_canvas_stats stats;
} canvas_state;

static void state_free(void *p) {
  canvas_state *s = p;
  cw_array_free(&s->items);
  cw_array_free(&s->ctxs);
  cw_array_free(&s->passes);
  cw_array_free(&s->cameras);
  cw_free(s);
}

static canvas_state *cstate(cw_tree *t) {
  canvas_state *s = cw_tree_slot(t, "canvas");
  if (!s) {
    s = cw_calloc(1, sizeof *s, CW_MEM_RENDER2D);
    cw_tree_set_slot(t, "canvas", s, state_free);
  }
  return s;
}

void cw_canvas_add_pass(cw_tree *t, int order, cw_canvas_pass_fn fn, void *user) {
  canvas_state *s = cstate(t);
  pass p = { order, fn, user };
  cw_array_push(&s->passes, p);
  for (int i = s->passes.count - 1; i > 0 && s->passes.data[i - 1].order > order; i--) {
    s->passes.data[i] = s->passes.data[i - 1];
    s->passes.data[i - 1] = p;
  }
}

void cw_canvas_remove_pass(cw_tree *t, cw_canvas_pass_fn fn, void *user) {
  canvas_state *s = cstate(t);
  for (int i = 0; i < s->passes.count; i++)
    if (s->passes.data[i].fn == fn && s->passes.data[i].user == user) {
      memmove(s->passes.data + i, s->passes.data + i + 1, sizeof(pass) * (size_t)(s->passes.count - i - 1));
      s->passes.count--;
      return;
    }
}

/* ── cameras: updated on fixed ticks after physics ─────────────────── */

static void camera_tick(cw_node n, cw__camera2d *c, float dt) {
  const float *m = cw_node2d_global_transform(n);
  cw_vec2 target = cw_v2(m[4], m[5]);
  float rot = c->rotating ? cw_atan2(m[1], m[0]) : 0;
  if (!c->started) {
    c->started = true;
    c->pos = c->prev_pos = target;
    c->rot = c->prev_rot = rot;
    return;
  }
  c->prev_pos = c->pos;
  c->prev_rot = c->rot;
  if (c->smoothing) {
    cw_camera2d tmp = { .position = c->pos };
    cw_camera2d_follow(&tmp, target, c->smoothing_speed, dt);
    c->pos = tmp.position;
  } else {
    c->pos = target;
  }
  c->rot = rot;
}

static void camera_hook(cw_tree *t, double dt, void *u) {
  (void)u;
  if (cw_tree_paused(t)) return;
  canvas_state *s = cstate(t);
  for (int i = 0; i < s->cameras.count; i++) {
    cw__camera2d *c = cw_node_data_t(s->cameras.data[i], cw__camera2d_t);
    if (c) camera_tick(s->cameras.data[i], c, (float)dt);
  }
}

void cw__canvas_camera_enter(cw_node n) {
  cw_tree *t = cw_node_tree(n);
  canvas_state *s = cstate(t);
  cw_array_push(&s->cameras, n);
  if (!s->hook_added) {
    s->hook_added = true;
    cw_tree_add_hook(t, 0, 200, camera_hook, NULL); /* after physics (100) */
  }
  cw__camera2d *c = cw_node_data_t(n, cw__camera2d_t);
  if (c) c->started = false;
}

void cw__canvas_camera_exit(cw_node n) {
  cw_tree *t = cw_node_tree(n);
  canvas_state *s = cw_tree_slot(t, "canvas");
  if (!s) return;
  for (int i = 0; i < s->cameras.count; i++)
    if (s->cameras.data[i].id == n.id) {
      memmove(s->cameras.data + i, s->cameras.data + i + 1, sizeof(cw_node) * (size_t)(s->cameras.count - i - 1));
      s->cameras.count--;
      return;
    }
}

/* ── the walk ───────────────────────────────────────────────────────── */

typedef struct wstate {
  int ctx;
  cw_xform2d xf;
  cw_color mod;
  int z;
} wstate;

static int new_ctx(canvas_state *s, int layer, bool follow, bool y_sort, cw_vec2 parallax) {
  canvas_ctx c = { layer, s->ctxs.count, follow, y_sort, false, parallax, 0 };
  cw_array_push(&s->ctxs, c);
  return s->ctxs.count - 1;
}

static cw_xform2d local_xform(const cw_node2d *d, double alpha, cw_vec2 extra) {
  cw_vec2 p = d->position;
  float r = d->rotation;
  if (d->interpolate && alpha < 1) {
    p = cw_v2_lerp(d->prev_position, d->position, (float)alpha);
    r = d->prev_rotation + (d->rotation - d->prev_rotation) * (float)alpha;
  }
  p = cw_v2_add(p, extra);
  cw_xform2d m = cw_xform2d_trs(p, r, d->scale);
  if (d->skew != 0) {
    float ts = tanf(d->skew); /* rendering only */
    m.c += m.a * ts;
    m.d += m.b * ts;
  }
  return m;
}

static void walk(canvas_state *s, cw_node n, wstate st, double alpha) {
  node_rec *r = cw__node_rec(n);
  if (!r) return;
  cw__canvas_layer *cl = cw__canvas_layer_t ? cw_node_data_t(n, cw__canvas_layer_t) : NULL;
  if (cl) {
    if (!cl->visible) return;
    st.ctx = new_ctx(s, cl->layer, cl->follow_viewport, cl->y_sort, cw_v2(1, 1));
    cw_vec2 sc = cl->scale;
    st.xf = cw_xform2d_trs(cl->offset, cl->rotation, sc);
    st.z = 0;
  } else {
    cw_node2d *d = cw_node_data_t(n, cw__node2d_t);
    if (d) {
      if (!d->visible) return;
      cw_vec2 extra = cw_v2(0, 0);
      cw__parallax_layer *pl = cw_node_data_t(n, cw__parallax_t);
      if (pl) {
        canvas_ctx *pc = &s->ctxs.data[st.ctx];
        st.ctx = new_ctx(s, pc->layer, true, pc->y_sort, pl->motion_scale);
        extra = pl->motion_offset;
      }
      cw_xform2d l = local_xform(d, alpha, extra);
      st.xf = cw_xform2d_mul(&st.xf, &l);
      st.mod = cw__col_mul(st.mod, cw__col4(d->modulate));
      st.z = d->z_relative ? st.z + d->z_index : d->z_index;
      if (st.z < -32768) st.z = -32768;
      if (st.z > 32767) st.z = 32767;
      const item_type *it = item_type_of(r->type);
      if (it) {
        item i = { n, it, st.ctx, st.z, st.xf, cw__col_mul(st.mod, cw__col4(d->self_modulate)) };
        cw_array_push(&s->items, i);
        cw__light2d *L = cw_node_data_t(n, cw__light2d_t);
        if (L && L->enabled) s->ctxs.data[st.ctx].lit = true;
      }
      cw__camera2d *cam = cw_node_data_t(n, cw__camera2d_t);
      if (cam && cam->current && CW_HANDLE_IS_NULL(s->camera)) s->camera = n;
    } else if (CW_HANDLE_IS_NULL(s->settings) && cw_node_data_t(n, cw__settings2d_t)) {
      s->settings = n;
    }
  }
  r = cw__node_rec(n);
  for (int i = 0; i < r->children.count; i++) walk(s, r->children.data[i], st, alpha);
}

static int ctx_cmp(const void *a, const void *b) {
  const canvas_ctx *x = *(canvas_ctx *const *)a, *y = *(canvas_ctx *const *)b;
  if (x->layer != y->layer) return x->layer < y->layer ? -1 : 1;
  return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static cw_camera2d camera_value(canvas_state *s, double alpha, cw_rect vp) {
  cw_camera2d cam = { .zoom = 1 };
  cw__camera2d *c = cw_node_data_t(s->camera, cw__camera2d_t);
  if (!c) return cam;
  cw_vec2 p;
  float rot;
  if (c->started) {
    float a = alpha < 1 ? (float)alpha : 1;
    p = cw_v2_lerp(c->prev_pos, c->pos, a);
    rot = c->prev_rot + (c->rot - c->prev_rot) * a;
  } else {
    p = cw_node2d_global_position(s->camera);
    rot = 0;
  }
  cam.position = p;
  cam.zoom = c->zoom > 0 ? c->zoom : 1;
  cam.rotation = rot;
  cam.offset = c->offset;
  cam.anchor_top_left = c->anchor_top_left;
  cam.use_limits = true;
  cam.limits = cw_rect_make((float)c->limit_left, (float)c->limit_top, (float)c->limit_right - (float)c->limit_left,
                            (float)c->limit_bottom - (float)c->limit_top);
  cw_camera2d_apply_limits(&cam, vp);
  return cam;
}

static bool canvas_draw(cw_tree *t, int w, int h, double alpha, bool clear, const cw_canvas_view *v);
bool cw_canvas_draw(cw_tree *t, int w, int h, double alpha, bool clear) { return canvas_draw(t, w, h, alpha, clear, NULL); }
bool cw_canvas_draw_view(cw_tree *t, const cw_canvas_view *v, double alpha) {
  int w = 0, h = 0;
  if (v && cw_texture_valid(v->target)) cw_texture_size(v->target, &w, &h);
  return canvas_draw(t, w, h, alpha, v ? v->clear : false, v);
}

static bool canvas_draw(cw_tree *t, int w, int h, double alpha, bool clear, const cw_canvas_view *v) {
  canvas_state *s = cstate(t);
  cw_array_clear(&s->items);
  cw_array_clear(&s->ctxs);
  s->camera = (cw_node){ 0 };
  s->settings = (cw_node){ 0 };
  memset(&s->stats, 0, sizeof s->stats);
  new_ctx(s, 0, true, false, cw_v2(1, 1)); /* the default canvas */
  wstate st = { 0, cw_xform2d_identity(), CW_WHITE, 0 };
  walk(s, cw_tree_root(t), st, alpha);
  cw__canvas_settings *cs = cw_node_data_t(s->settings, cw__settings2d_t);
  if (cs) s->ctxs.data[0].y_sort = cs->y_sort;
  /* screenshot by layer (cartwheel/debug.h): the tree's 2D is WORLD, the
   * overlay passes (Control nodes) are UI; without WORLD the pipeline has
   * cleared to transparent already */
  uint32_t layers = cw_render_layers();
  bool world = (layers & CW_LAYER_WORLD) != 0;
  if (!world) clear = false;
  if (!s->items.count && !s->passes.count && !v) {
    s->has_cam = false;
    if (clear && cw_gpu_backend() == CW_GPU_WEBGPU) {
      if (cs) cw_gpu_clear_window(cs->clear_color[0], cs->clear_color[1], cs->clear_color[2], 1);
      else cw_gpu_clear_window(0, 0, 0, 1);
    } else if (clear) {
      glViewport(0, 0, w, h);
      if (cs) glClearColor(cs->clear_color[0], cs->clear_color[1], cs->clear_color[2], 1);
      else glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    return false;
  }
  cw_r2d_pass_desc pd = { .clear = clear };
  if (cs) {
    pd.clear_color = cw__col4(cs->clear_color);
    pd.virtual_width = cs->virtual_width;
    pd.virtual_height = cs->virtual_height;
    pd.scale = (cw_scale_mode)cs->scale_mode;
    pd.letterbox = cw__col4(cs->letterbox);
    pd.ambient = cw__col4(cs->ambient);
  } else {
    pd.clear_color = CW_BLACK;
    pd.ambient = CW_WHITE;
  }
  pd.time = (float)((double)cw_tree_tick(t) * cw_fixed_dt());
  if (v) { /* an editor view: its own target and camera, no virtual canvas */
    pd.target = v->target;
    pd.virtual_width = pd.virtual_height = 0;
    pd.scale = CW_SCALE_NONE;
    pd.clear_color = v->clear_color;
    if (!world) clear = v->clear;
  }
  (void)w;
  (void)h;
  if ((s->items.count && world) || (v && (world || clear))) {
    cw_r2d_begin(&pd);
    cw_vec2 csz = cw_r2d_canvas_size();
    s->viewport = cw_rect_make(0, 0, csz.x, csz.y);
    s->has_cam = !CW_HANDLE_IS_NULL(s->camera);
    s->cam = camera_value(s, alpha, s->viewport);
    if (v && v->use_camera) {
      s->has_cam = true;
      s->cam = v->camera;
    }
    /* canvases -> render layers */
    canvas_ctx **order = cw_frame_alloc(sizeof(canvas_ctx *) * (size_t)s->ctxs.count);
    for (int i = 0; i < s->ctxs.count; i++) order[i] = &s->ctxs.data[i];
    qsort(order, (size_t)s->ctxs.count, sizeof *order, ctx_cmp);
    int used = 0;
    /* the r2d layer table outlives passes: what this draw sets is put back
     * after it, so an app or editor pass that follows sees its own settings
     * (screen space on layer 0 by default), not this tree's camera */
    int touched = s->ctxs.count < MAX_CANVASES ? s->ctxs.count : MAX_CANVASES;
    cw_r2d_layer_desc *saved = cw_frame_alloc(sizeof *saved * (size_t)(touched ? touched : 1));
    for (int i = 0; i < touched; i++) saved[i] = *cw_r2d_layer_get(i);
    for (int i = 0; i < s->ctxs.count; i++) {
      canvas_ctx *c = order[i];
      if (used >= MAX_CANVASES) {
        cw_warn("2D: more than %d canvases (CanvasLayers + ParallaxLayers) in one frame; the rest share the last", MAX_CANVASES);
        c->r2d = MAX_CANVASES - 1;
        continue;
      }
      c->r2d = used++;
      cw_r2d_layer_desc ld = { 0 };
      ld.has_camera = c->follow && s->has_cam;
      ld.camera = s->cam;
      ld.parallax = c->parallax;
      ld.y_sort = c->y_sort;
      ld.pixel_snap = cs && cs->pixel_snap;
      ld.lit = c->lit;
      cw_r2d_layer_set(c->r2d, &ld);
    }
    s->stats.layers = used;
    for (int i = 0; i < (world ? s->items.count : 0); i++) {
      item *it = &s->items.data[i];
      void *data = cw_node_data_t(it->node, it->it->type);
      if (!data) continue;
      canvas_ctx *c = &s->ctxs.data[it->ctx];
      cw_r2d_use_layer(c->r2d);
      cw_r2d_set_z(it->z);
      cw_r2d_set_transform(&it->xf);
      cw_canvas_item ci = { it->xf, it->mod, c->r2d, it->z, (float)alpha };
      it->it->fn(it->node, data, &ci);
      if (cw_node_data_t(it->node, cw__light2d_t)) s->stats.lights++;
    }
    cw_r2d_set_transform(NULL);
    s->stats.items = s->items.count;
    cw_r2d_end();
    for (int i = 0; i < touched; i++) cw_r2d_layer_set(i, &saved[i]);
    cw_r2d_use_layer(0);
    cw_r2d_set_z(0);
  } else if (clear && cw_gpu_backend() == CW_GPU_WEBGPU) {
    cw_gpu_clear_window(pd.clear_color.r, pd.clear_color.g, pd.clear_color.b, 1);
  } else if (clear) {
    glViewport(0, 0, w, h);
    glClearColor(pd.clear_color.r, pd.clear_color.g, pd.clear_color.b, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }
  if ((layers & CW_LAYER_UI) && !(v && v->skip_overlays))
    for (int i = 0; i < s->passes.count; i++) s->passes.data[i].fn(t, w, h, alpha, s->passes.data[i].user);
  t3_renderer *r3 = cw_render3d_renderer(t);
  if (r3) t3_renderer_reset_bindings(r3);
  return true;
}

cw_node cw_canvas_camera(cw_tree *t) { return cstate(t)->camera; }
cw_camera2d cw_canvas_camera_value(cw_tree *t) { return cstate(t)->cam; }

cw_vec2 cw_canvas_screen_to_world(cw_tree *t, cw_vec2 px) {
  canvas_state *s = cstate(t);
  if (!s->has_cam) return px;
  return cw_camera2d_screen_to_world(&s->cam, s->viewport, px);
}
cw_vec2 cw_canvas_world_to_screen(cw_tree *t, cw_vec2 w) {
  canvas_state *s = cstate(t);
  if (!s->has_cam) return w;
  return cw_camera2d_world_to_screen(&s->cam, s->viewport, w);
}
cw_canvas_stats cw_canvas_get_stats(cw_tree *t) { return cstate(t)->stats; }
