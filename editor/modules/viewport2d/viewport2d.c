/* 2D Viewport panel (M4.1).
 *
 * Draws the active scene's 2D (cw_canvas_draw_view: the scene's canvas
 * items into a render target through the editor's own camera, whatever
 * Camera2D the scene has) and on top of it: a grid (every pixel from zoom
 * 8 up), rulers along the top and left edges, guides, the game's view rect
 * (CanvasSettings virtual size, else 1280 x 720), gizmos (collision shapes,
 * cameras, lights, occluders, point editing) and the transform tool.
 *
 * Pan: middle or right drag (or space + left drag); zoom: the wheel, around
 * the pointer. Tools: Q select, W move, E rotate, R scale, Y snapping.
 * Click picks the topmost node under the pointer (ctrl toggles, shift
 * adds). Drag from a ruler to make a guide; drag a guide back onto its
 * ruler to remove it. Polygon points: select a Line2D, Polygon2D,
 * LightOccluder2D or a CollisionShape2D polygon / segment / chain and drag
 * its points (ctrl+click an edge inserts, right-click removes).
 *
 * Commands: view2d.camera center zoom, view2d.focus [path], view2d.tool,
 * view2d.snap on step angle scale, view2d.grid on, view2d.pick x y mode,
 * view2d.state, and view2d.guide axis pos [remove] (undoable in the
 * editor-wide history; guides persist in setting viewport2d.guides). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/gl.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/vfs.h"
#include "../gizmos/gizmos.h"

#define RULER 18.0f
#define MAX_GUIDES 64

typedef struct { cwed_doc *doc; cw_vec2 center; float zoom; bool init; } view_cam;
typedef struct { cw_vec2 a, b; cw_color c; float w; int kind; char text[48]; cw_vec2 pts[24]; int npts; } overlay_cmd;

static struct {
  cw_node panel, view, info, tool_btn[CWED_TOOL_COUNT], snap_btn, grid_btn;
  cw_texture target;
  int tw, th;
  CW_ARRAY(view_cam) cams;
  CW_ARRAY(overlay_cmd) overlay;
  cwed_gizmo_ctx ctx;
  float gx[MAX_GUIDES], gy[MAX_GUIDES];
  int ngx, ngy;
  bool panning, space;
  cw_vec2 last;
  int guide_drag;      /* 0 none, 1 new x (from the left ruler), 2 new y (top ruler), 3 move x, 4 move y */
  int guide_index;
  float guide_pos;
  bool syncing;
  uint32_t renders;
} V;

static float px_scale(void) { return cw_ui_scale() > 0 ? cw_ui_scale() : 1; }

static view_cam *cam_for(cwed_doc *d) {
  for (int i = 0; i < V.cams.count; i++)
    if (V.cams.data[i].doc == d) return &V.cams.data[i];
  view_cam c = { d, cw_v2(0, 0), 1, false };
  cw_array_push(&V.cams, c);
  return &V.cams.data[V.cams.count - 1];
}
static void on_doc_closed(const cwed_event *e, void *u) {
  (void)u;
  for (int i = 0; i < V.cams.count; i++)
    if (V.cams.data[i].doc == e->doc) {
      cw_array_remove_swap(&V.cams, i);
      break;
    }
}

static bool doc_is_2d(cwed_doc *d) {
  cw_node r = cwed_doc_root(d);
  if (!cw_node_valid(r)) return false;
  if (cw_node_is(r, "Node2D") || cw_node_is(r, "Control") || cw_node_is(r, "CanvasLayer")) return true;
  if (cw_node_is(r, "Node3D")) return false;
  for (int i = 0; i < cw_node_child_count(r); i++) {
    cw_node c = cw_node_child(r, i);
    if (cw_node_is(c, "Node2D") || cw_node_is(c, "CanvasLayer") || cw_node_is(c, "Control")) return true;
  }
  return false;
}

/* ── guides (setting viewport2d.guides: "x64,y32,...") ──────────────────── */

static void load_guides(void) {
  V.ngx = V.ngy = 0;
  const char *s = cwed_setting("viewport2d.guides");
  while (s && *s) {
    char axis = *s++;
    float v = strtof(s, (char **)&s);
    if (axis == 'x' && V.ngx < MAX_GUIDES) V.gx[V.ngx++] = v;
    else if (axis == 'y' && V.ngy < MAX_GUIDES) V.gy[V.ngy++] = v;
    while (*s == ',' || *s == ' ') s++;
  }
}
static const char *guides_text(void) {
  char *t = cw_frame_alloc(MAX_GUIDES * 2 * 24 + 8), *w = t;
  *w = 0;
  for (int i = 0; i < V.ngx; i++) w += sprintf(w, "%sx%s", w == t ? "" : ",", cwed_gizmo_num(V.gx[i]));
  for (int i = 0; i < V.ngy; i++) w += sprintf(w, "%sy%s", w == t ? "" : ",", cwed_gizmo_num(V.gy[i]));
  return t;
}

typedef struct { const char *before; } guide_state;
static bool guide_exec(cwed_call *c) {
  load_guides();
  const char *axis = cwed_arg(c, "axis");
  if (strcmp(axis, "x") && strcmp(axis, "y")) return cwed_fail(c, "axis is x (a vertical guide at x = pos) or y (horizontal)");
  float pos = (float)cwed_arg_float(c, "pos");
  bool rm = cwed_arg_bool(c, "remove");
  float *g = axis[0] == 'x' ? V.gx : V.gy;
  int *n = axis[0] == 'x' ? &V.ngx : &V.ngy;
  int at = -1;
  for (int i = 0; i < *n; i++)
    if (fabsf(g[i] - pos) < 1e-3f) at = i;
  if (rm && at < 0) return cwed_fail(c, "no %s guide at %s (view2d.state lists them)", axis, cwed_gizmo_num(pos));
  if (!rm && at >= 0) return cwed_fail(c, "there is a %s guide at %s already", axis, cwed_gizmo_num(pos));
  if (!rm && *n >= MAX_GUIDES) return cwed_fail(c, "at most %d guides per axis", MAX_GUIDES);
  guide_state *s = cwed_call_state(c);
  if (!s) {
    s = cw_calloc(1, sizeof *s, CW_MEM_EDITOR);
    cwed_call_set_state(c, s);
  }
  s->before = cw_intern(cwed_setting("viewport2d.guides"));
  if (rm) {
    g[at] = g[*n - 1];
    (*n)--;
  } else {
    g[(*n)++] = pos;
  }
  cwed_setting_put("viewport2d.guides", guides_text());
  cwed_json_key(cwed_reply(c), "guides");
  cwed_json_str(cwed_reply(c), cwed_setting("viewport2d.guides"));
  return true;
}
static void guide_undo(cwed_call *c) {
  guide_state *s = cwed_call_state(c);
  if (s) cwed_setting_put("viewport2d.guides", s->before);
  load_guides();
}

/* ── overlay recording ───────────────────────────────────────────────── */

static void overlay_line(cwed_gizmo_ctx *c, cw_vec2 a, cw_vec2 b, cw_color col, float w) {
  (void)c;
  overlay_cmd o = { .a = a, .b = b, .c = col, .w = w, .kind = 0 };
  cw_array_push(&V.overlay, o);
}
static void overlay_poly(cwed_gizmo_ctx *c, const cw_vec2 *pts, int n, cw_color col) {
  (void)c;
  overlay_cmd o = { .c = col, .kind = 1 };
  o.npts = n > 24 ? 24 : n;
  memcpy(o.pts, pts, sizeof(cw_vec2) * (size_t)o.npts);
  cw_array_push(&V.overlay, o);
}
static void overlay_text(cwed_gizmo_ctx *c, cw_vec2 at, const char *text, cw_color col) {
  (void)c;
  overlay_cmd o = { .a = at, .c = col, .kind = 2 };
  snprintf(o.text, sizeof o.text, "%s", text);
  cw_array_push(&V.overlay, o);
}
static void rect_fill(cw_rect r, cw_color col) {
  cw_vec2 p[4] = { { r.x, r.y }, { r.x + r.w, r.y }, { r.x + r.w, r.y + r.h }, { r.x, r.y + r.h } };
  overlay_poly(&V.ctx, p, 4, col);
}

/* ── view transform ──────────────────────────────────────────────────── */

static cw_camera2d camera_of(const view_cam *vc) { return (cw_camera2d){ .position = vc->center, .zoom = vc->zoom }; }

static void fill_ctx(cwed_doc *d, view_cam *vc) {
  cwed_gizmo_ctx *c = &V.ctx;
  c->doc = d;
  c->is_3d = false;
  c->width = (float)V.tw, c->height = (float)V.th;
  c->px = px_scale();
  cw_camera2d cam = camera_of(vc);
  c->view2 = cw_camera2d_view(&cam, (cw_rect){ 0, 0, (float)V.tw, (float)V.th }, (cw_vec2){ 1, 1 });
  c->zoom2 = vc->zoom;
  c->tool = (cwed_tool)(int)cwed_setting_float("viewport2d.tool");
  c->local = false;
  c->snap = cwed_setting_bool("viewport2d.snap");
  c->snap_move = (float)cwed_setting_float("viewport2d.snap_move");
  c->snap_angle = (float)cwed_setting_float("viewport2d.snap_angle");
  c->snap_scale = (float)cwed_setting_float("viewport2d.snap_scale");
  c->guides_x = V.gx, c->guides_y = V.gy;
  c->guide_x_count = V.ngx, c->guide_y_count = V.ngy;
  c->overlay_line_fn = overlay_line;
  c->overlay_poly_fn = overlay_poly;
  c->overlay_text_fn = overlay_text;
  c->line3_fn = NULL;
}

/* ── node bounds (picking, selection, framing) ─────────────────────────── */

static int png_size_cache_n;
static struct { const char *path; int w, h; } png_cache[64];
static bool png_size(const char *path, int *w, int *h) {
  const char *k = cw_intern(path);
  for (int i = 0; i < png_size_cache_n; i++)
    if (png_cache[i].path == k) {
      *w = png_cache[i].w, *h = png_cache[i].h;
      return *w > 0;
    }
  size_t len = 0;
  unsigned char *b = cw_vfs_read(path, &len);
  int W = 0, H = 0;
  if (b && len > 24 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
    W = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
    H = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
  }
  free(b);
  if (png_size_cache_n < 64) png_cache[png_size_cache_n].path = k, png_cache[png_size_cache_n].w = W, png_cache[png_size_cache_n++].h = H;
  *w = W, *h = H;
  return W > 0;
}

static float fprop(cw_node n, const char *p, float def) {
  cw_value v;
  if (!cw_node_get_prop(n, p, &v)) return def;
  return v.type == CW_T_FLOAT ? (float)v.f : v.type == CW_T_INT || v.type == CW_T_ENUM ? (float)v.i : def;
}
static const char *sprop(cw_node n, const char *p) {
  cw_value v;
  return cw_node_get_prop(n, p, &v) && v.type == CW_T_STRING && v.s ? v.s : "";
}
static bool bprop(cw_node n, const char *p) {
  cw_value v;
  return cw_node_get_prop(n, p, &v) && v.b;
}

/* local-space rect of what a node draws; false when it has no extent */
static bool local_rect(cw_node n, cw_rect *r) {
  if (cw_node_is(n, "Sprite2D")) {
    int w, h;
    if (!png_size(sprop(n, "texture"), &w, &h)) return false;
    cw_value reg;
    if (bprop(n, "region_enabled") && cw_node_get_prop(n, "region", &reg)) w = (int)reg.v[2], h = (int)reg.v[3];
    float fw = (float)w / fmaxf(fprop(n, "hframes", 1), 1), fh = (float)h / fmaxf(fprop(n, "vframes", 1), 1);
    cw_value off;
    cw_vec2 o = cw_node_get_prop(n, "offset", &off) ? cw_v2(off.v[0], off.v[1]) : cw_v2(0, 0);
    *r = bprop(n, "centered") ? (cw_rect){ o.x - fw / 2, o.y - fh / 2, fw, fh } : (cw_rect){ o.x, o.y, fw, fh };
    return true;
  }
  if (cw_node_is(n, "CollisionShape2D")) {
    int shape = (int)fprop(n, "shape", 0);
    cw_value sz;
    if (shape == 0 && cw_node_get_prop(n, "size", &sz)) {
      *r = (cw_rect){ -sz.v[0] / 2, -sz.v[1] / 2, sz.v[0], sz.v[1] };
      return true;
    }
    if (shape == 1 || shape == 2) {
      float rr = fprop(n, "radius", 8), h = shape == 2 ? fmaxf(fprop(n, "height", 32) / 2, rr) : rr;
      *r = (cw_rect){ -rr, -h, rr * 2, h * 2 };
      return true;
    }
  }
  const char *pp = cw_node_is(n, "Line2D") || cw_node_is(n, "CollisionShape2D") ? "points"
                 : cw_node_is(n, "Polygon2D") || cw_node_is(n, "LightOccluder2D") ? "polygon" : NULL;
  if (pp) {
    cw_vec2 *pts;
    int k = cw_points_parse(sprop(n, pp), &pts);
    if (!k) return false;
    float x0 = pts[0].x, y0 = pts[0].y, x1 = x0, y1 = y0;
    for (int i = 1; i < k; i++) x0 = fminf(x0, pts[i].x), y0 = fminf(y0, pts[i].y), x1 = fmaxf(x1, pts[i].x), y1 = fmaxf(y1, pts[i].y);
    float pad = cw_node_is(n, "Line2D") ? fprop(n, "width", 2) / 2 : 0;
    *r = (cw_rect){ x0 - pad, y0 - pad, x1 - x0 + pad * 2, y1 - y0 + pad * 2 };
    return true;
  }
  if (cw_node_is(n, "Label2D")) {
    const char *t = sprop(n, "text");
    float s = fprop(n, "size", 16);
    *r = (cw_rect){ 0, 0, fmaxf(s * 0.55f * (float)strlen(t), s), s * 1.3f };
    return true;
  }
  if (cw_node_is(n, "TileMap2D")) {
    cw_value ts;
    int w = 0, h = 0;
    cw_tilemap2d_size(n, &w, &h);
    if (w <= 0 || h <= 0 || !cw_node_get_prop(n, "tile_size", &ts)) return false;
    cw_value cs;
    float cw_ = ts.v[0], ch = ts.v[1];
    if (cw_node_get_prop(n, "cell_size", &cs) && cs.v[0] > 0) cw_ = cs.v[0], ch = cs.v[1];
    *r = (cw_rect){ 0, 0, cw_ * (float)w, ch * (float)h };
    return true;
  }
  return false;
}

/* the world corners of a node's rect */
static void world_quad(cw_node n, cw_rect r, cw_vec2 q[4]) {
  cw_xform2d w = cwed_node2d_world(n);
  cw_vec2 l[4] = { { r.x, r.y }, { r.x + r.w, r.y }, { r.x + r.w, r.y + r.h }, { r.x, r.y + r.h } };
  for (int i = 0; i < 4; i++) q[i] = cw_xform2d_apply(&w, l[i]);
}

static bool in_quad(cw_vec2 p, const cw_vec2 q[4]) {
  int sign = 0;
  for (int i = 0; i < 4; i++) {
    float c = cw_v2_cross(cw_v2_sub(q[(i + 1) % 4], q[i]), cw_v2_sub(p, q[i]));
    int s = c > 0 ? 1 : c < 0 ? -1 : 0;
    if (!s) continue;
    if (!sign) sign = s;
    else if (s != sign) return false;
  }
  return true;
}

typedef struct { cw_vec2 world, px; cw_node hit; float hit_d; cwed_doc *d; } pick_walk;
static bool pick_fn(cw_node n, void *u) {
  pick_walk *w = u;
  if (cw_node_is(n, "Node2D")) {
    cw_rect r;
    if (bprop(n, "visible") == false && cw_node_find_prop(n, "visible", NULL)) return false; /* hidden subtree */
    if (local_rect(n, &r)) {
      cw_vec2 q[4];
      world_quad(n, r, q);
      if (in_quad(w->world, q)) w->hit = n, w->hit_d = 0; /* later in tree order draws on top */
    } else {
      cw_xform2d x = cwed_node2d_world(n);
      float d = cw_v2_length(cw_v2_sub(cwed_gizmo_to_view2(&V.ctx, cw_v2(x.tx, x.ty)), w->px));
      if (d < 8 * V.ctx.px && w->hit_d > 0) w->hit = n, w->hit_d = d;
    }
  }
  return true;
}

static cw_node selectable(cwed_doc *d, cw_node n) {
  cw_node root = cwed_doc_root(d), pick = n;
  for (cw_node p = n; cw_node_valid(p) && p.id != root.id; p = cw_node_parent(p))
    if (cw_scene_instance_source(p)) pick = p;
  return pick;
}

static cw_node pick_at(cwed_doc *d, cw_vec2 px) {
  pick_walk w = { cwed_gizmo_from_view2(&V.ctx, px), px, { 0 }, 1e9f, d };
  cw_node_walk(cwed_doc_root(d), pick_fn, &w);
  return CW_HANDLE_IS_NULL(w.hit) ? w.hit : selectable(d, w.hit);
}

static void pick_select(cwed_doc *d, cw_vec2 px, int mode) {
  cw_node n = pick_at(d, px);
  if (CW_HANDLE_IS_NULL(n)) {
    if (mode == 0) cwed_run("select.clear", NULL);
    return;
  }
  cwed_runf("select paths=%s mode=%s", cwed_quote(cwed_doc_node_path(d, n)), mode == 0 ? "set" : mode == 1 ? "add" : "toggle");
}

/* ── frame: the scene image and the overlays ──────────────────────────── */

static float nice_step(float min_world) {
  float p = powf(10, floorf(log10f(fmaxf(min_world, 1e-6f))));
  if (p >= min_world) return p;
  if (2 * p >= min_world) return 2 * p;
  if (5 * p >= min_world) return 5 * p;
  return 10 * p;
}

static void game_rect(cwed_doc *d, float *w, float *h) {
  *w = 1280, *h = 720;
  cw_node root = cwed_doc_root(d);
  for (int i = -1; i < cw_node_child_count(root); i++) {
    cw_node s = i < 0 ? root : cw_node_child(root, i);
    if (cw_node_is(s, "CanvasSettings") && fprop(s, "virtual_width", 0) > 0) {
      *w = fprop(s, "virtual_width", 0);
      *h = fprop(s, "virtual_height", 0);
      return;
    }
  }
}

static void build_overlay(cwed_doc *d, view_cam *vc) {
  cw_array_clear(&V.overlay);
  cwed_gizmo_ctx *c = &V.ctx;
  float W = (float)V.tw, H = (float)V.th, s = c->px;
  cw_vec2 tl = cwed_gizmo_from_view2(c, cw_v2(0, 0)), br = cwed_gizmo_from_view2(c, cw_v2(W, H));
  /* grid: the snap step (pixel grid when zoomed in), drawn when lines are >= 6 px apart */
  if (cwed_setting_bool("viewport2d.grid")) {
    float step = vc->zoom >= 8 ? 1 : fmaxf(c->snap_move, 1);
    while (step * vc->zoom < 6 * s) step *= 2;
    cw_color gc = cw_rgba(1, 1, 1, vc->zoom >= 8 ? 0.10f : 0.06f);
    int lines = 0;
    for (float x = floorf(tl.x / step) * step; x <= br.x && lines < 600; x += step, lines++)
      overlay_line(c, cwed_gizmo_to_view2(c, cw_v2(x, tl.y)), cwed_gizmo_to_view2(c, cw_v2(x, br.y)), gc, 1);
    for (float y = floorf(tl.y / step) * step; y <= br.y && lines < 1200; y += step, lines++)
      overlay_line(c, cwed_gizmo_to_view2(c, cw_v2(tl.x, y)), cwed_gizmo_to_view2(c, cw_v2(br.x, y)), gc, 1);
    /* the origin axes */
    overlay_line(c, cwed_gizmo_to_view2(c, cw_v2(tl.x, 0)), cwed_gizmo_to_view2(c, cw_v2(br.x, 0)), cw_rgba(0.85f, 0.35f, 0.38f, 0.6f), 1.5f * s);
    overlay_line(c, cwed_gizmo_to_view2(c, cw_v2(0, tl.y)), cwed_gizmo_to_view2(c, cw_v2(0, br.y)), cw_rgba(0.45f, 0.82f, 0.4f, 0.6f), 1.5f * s);
  }
  /* the game's view */
  float gw, gh;
  game_rect(d, &gw, &gh);
  cw_vec2 g0 = cwed_gizmo_to_view2(c, cw_v2(0, 0)), g1 = cwed_gizmo_to_view2(c, cw_v2(gw, gh));
  cw_color gcol = cw_rgba(0.75f, 0.55f, 1, 0.7f);
  overlay_line(c, g0, cw_v2(g1.x, g0.y), gcol, 1.5f * s);
  overlay_line(c, cw_v2(g1.x, g0.y), g1, gcol, 1.5f * s);
  overlay_line(c, g1, cw_v2(g0.x, g1.y), gcol, 1.5f * s);
  overlay_line(c, cw_v2(g0.x, g1.y), g0, gcol, 1.5f * s);
  /* gizmos, selection, the transform tool */
  cwed_gizmos_draw_all(c);
  for (int i = 0; i < cwed_selection_count(); i++) {
    cw_node n = cwed_selection_get(i);
    cw_rect r;
    if (!cw_node_is(n, "Node2D") || !local_rect(n, &r)) continue;
    cw_vec2 q[4];
    world_quad(n, r, q);
    for (int k = 0; k < 4; k++) overlay_line(c, cwed_gizmo_to_view2(c, q[k]), cwed_gizmo_to_view2(c, q[(k + 1) % 4]), cw_rgba(1, 0.62f, 0.2f, 1), 1.5f * s);
  }
  cwed_transform_draw(c);
  /* guides */
  cw_color guc = cw_rgba(0.35f, 0.75f, 1, 0.85f);
  for (int i = 0; i < V.ngx; i++) {
    float x = cwed_gizmo_to_view2(c, cw_v2(V.gx[i], 0)).x;
    overlay_line(c, cw_v2(x, 0), cw_v2(x, H), guc, 1);
  }
  for (int i = 0; i < V.ngy; i++) {
    float y = cwed_gizmo_to_view2(c, cw_v2(0, V.gy[i])).y;
    overlay_line(c, cw_v2(0, y), cw_v2(W, y), guc, 1);
  }
  if (V.guide_drag) {
    bool vx = V.guide_drag == 1 || V.guide_drag == 3;
    cw_vec2 p = cwed_gizmo_to_view2(c, cw_v2(V.guide_pos, V.guide_pos));
    if (vx) overlay_line(c, cw_v2(p.x, 0), cw_v2(p.x, H), cw_rgba(1, 0.85f, 0.3f, 1), 1.5f);
    else overlay_line(c, cw_v2(0, p.y), cw_v2(W, p.y), cw_rgba(1, 0.85f, 0.3f, 1), 1.5f);
  }
  /* rulers */
  float R = RULER * s;
  rect_fill((cw_rect){ 0, 0, W, R }, cw_rgba(0.13f, 0.14f, 0.17f, 0.95f));
  rect_fill((cw_rect){ 0, 0, R, H }, cw_rgba(0.13f, 0.14f, 0.17f, 0.95f));
  float major = nice_step(70 * s / vc->zoom);
  cw_color tc = cw_rgba(0.6f, 0.64f, 0.72f, 1);
  for (float x = floorf(tl.x / major) * major; x <= br.x; x += major) {
    float vx = cwed_gizmo_to_view2(c, cw_v2(x, 0)).x;
    if (vx < R) continue;
    overlay_line(c, cw_v2(vx, 0), cw_v2(vx, R), tc, 1);
    for (int k = 1; k < 10; k++) {
      float mx = cwed_gizmo_to_view2(c, cw_v2(x + major * (float)k / 10, 0)).x;
      overlay_line(c, cw_v2(mx, R * (k == 5 ? 0.45f : 0.7f)), cw_v2(mx, R), cw_rgba(0.6f, 0.64f, 0.72f, 0.5f), 1);
    }
    overlay_text(c, cw_v2(vx + 3 * s, 1 * s), cwed_gizmo_num(x), tc);
  }
  for (float y = floorf(tl.y / major) * major; y <= br.y; y += major) {
    float vy = cwed_gizmo_to_view2(c, cw_v2(0, y)).y;
    if (vy < R) continue;
    overlay_line(c, cw_v2(0, vy), cw_v2(R, vy), tc, 1);
    for (int k = 1; k < 10; k++) {
      float my = cwed_gizmo_to_view2(c, cw_v2(0, y + major * (float)k / 10)).y;
      overlay_line(c, cw_v2(R * (k == 5 ? 0.45f : 0.7f), my), cw_v2(R, my), cw_rgba(0.6f, 0.64f, 0.72f, 0.5f), 1);
    }
    overlay_text(c, cw_v2(2 * s, vy + 2 * s), cwed_gizmo_num(y), tc);
  }
  rect_fill((cw_rect){ 0, 0, R, R }, cw_rgba(0.1f, 0.11f, 0.13f, 1));
}

static void render(cwed_doc *d) {
  view_cam *vc = cam_for(d);
  fill_ctx(d, vc);
  cwed_doc_enter(d);
  cw_canvas_draw_view(cwed_doc_tree(d), &(cw_canvas_view){ .target = V.target, .use_camera = true, .camera = camera_of(vc), .clear = true,
                                                            .clear_color = cw_rgba(0.16f, 0.17f, 0.2f, 1), .skip_overlays = true }, 1.0);
  cwed_doc_leave();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  build_overlay(d, vc);
  V.renders++;
}

/* ── the view control ────────────────────────────────────────────────── */

static void view_draw(cw_node n, void *data) {
  (void)data;
  cw_rect r = cw_control_rect(n);
  cw_ui_draw_rect((cw_rect){ 0, 0, r.w, r.h }, cw_rgba(0.1f, 0.11f, 0.13f, 1));
  cwed_doc *d = cwed_doc_active();
  if (!d || !doc_is_2d(d) || !cw_texture_valid(V.target)) {
    cw_ui_draw_text_in(cw_ui_font_sans(), 15, d ? "This scene is 3D: see the 3D viewport" : "No scene open", (cw_rect){ 0, 0, r.w, r.h },
                       CW_HALIGN_CENTER, cw_rgba(0.55f, 0.58f, 0.64f, 1), false);
    return;
  }
  float s = px_scale();
  cw_ui_draw_texture(V.target, (cw_rect){ 0, 0, (float)V.tw / s, (float)V.th / s }, (cw_rect){ 0 }, CW_WHITE);
  cw_ui_push_clip((cw_rect){ 0, 0, r.w, r.h });
  for (int i = 0; i < V.overlay.count; i++) {
    overlay_cmd *o = &V.overlay.data[i];
    if (o->kind == 0) cw_ui_draw_line(cw_v2_scale(o->a, 1 / s), cw_v2_scale(o->b, 1 / s), o->w / s, o->c);
    else if (o->kind == 1) {
      cw_vec2 p[24];
      for (int k = 0; k < o->npts; k++) p[k] = cw_v2_scale(o->pts[k], 1 / s);
      cw_ui_draw_polygon(p, o->npts, o->c);
    } else {
      cw_ui_draw_text(cw_ui_font_sans(), 10, o->text, cw_v2_scale(o->a, 1 / s), o->c);
    }
  }
  cw_ui_pop_clip();
}

static void zoom_at(view_cam *vc, cw_vec2 px, float factor) {
  cw_vec2 before = cwed_gizmo_from_view2(&V.ctx, px);
  vc->zoom = fminf(fmaxf(vc->zoom * factor, 0.02f), 64);
  cw_camera2d cam = camera_of(vc);
  cw_vec2 after = cw_camera2d_screen_to_world(&cam, (cw_rect){ 0, 0, (float)V.tw, (float)V.th }, px);
  vc->center = cw_v2_add(vc->center, cw_v2_sub(before, after));
}

static int guide_hit(cw_vec2 px) {
  for (int i = 0; i < V.ngx; i++)
    if (fabsf(cwed_gizmo_to_view2(&V.ctx, cw_v2(V.gx[i], 0)).x - px.x) < 4 * V.ctx.px) return i;
  for (int i = 0; i < V.ngy; i++)
    if (fabsf(cwed_gizmo_to_view2(&V.ctx, cw_v2(0, V.gy[i])).y - px.y) < 4 * V.ctx.px) return 1000 + i;
  return -1;
}

static bool view_input(cw_node n, void *data, const cw_ui_event *e) {
  (void)data;
  cwed_doc *d = cwed_doc_active();
  if (!d || !doc_is_2d(d)) return false;
  view_cam *vc = cam_for(d);
  float s = px_scale(), R = RULER * s;
  load_guides();
  fill_ctx(d, vc);
  cw_vec2 px = cw_v2_scale(e->pos, s);
  V.ctx.pointer = px;
  V.ctx.pointer_inside = true;
  switch (e->type) {
  case CW_UI_MOUSE_DOWN: {
    cw_control_grab_focus(n);
    if (e->button == 0 && !V.space) {
      if (px.y < R && px.x >= R) { V.guide_drag = 2; V.guide_pos = cwed_gizmo_from_view2(&V.ctx, px).y; return true; }
      if (px.x < R && px.y >= R) { V.guide_drag = 1; V.guide_pos = cwed_gizmo_from_view2(&V.ctx, px).x; return true; }
      if (cwed_transform_input(&V.ctx, e)) return true;
      if (cwed_gizmos_input_all(&V.ctx, e)) return true;
      int g = guide_hit(px);
      if (g >= 0 && V.ctx.tool == CWED_TOOL_SELECT) {
        V.guide_drag = g >= 1000 ? 4 : 3;
        V.guide_index = g >= 1000 ? g - 1000 : g;
        V.guide_pos = g >= 1000 ? V.gy[V.guide_index] : V.gx[V.guide_index];
        return true;
      }
      pick_select(d, px, e->ctrl ? 2 : e->shift ? 1 : 0);
      return true;
    }
    if (e->button == 1 && cwed_gizmos_input_all(&V.ctx, e)) return true;
    V.panning = true;
    V.last = e->pos;
    return true;
  }
  case CW_UI_MOUSE_MOVE: {
    if (V.guide_drag) {
      cw_vec2 w = cwed_gizmo_from_view2(&V.ctx, px);
      bool vx = V.guide_drag == 1 || V.guide_drag == 3;
      float v = vx ? w.x : w.y;
      V.guide_pos = V.ctx.snap ? cwed_gizmo_snap(v, fmaxf(V.ctx.snap_move, 1)) : roundf(v);
      return true;
    }
    if (cwed_transform_dragging()) return cwed_transform_input(&V.ctx, e);
    if (V.ctx.capture) return cwed_gizmos_input_all(&V.ctx, e);
    if (V.panning || (V.space && (e->buttons & 1))) {
      cw_vec2 dl = cw_v2_sub(e->pos, V.last);
      V.last = e->pos;
      vc->center = cw_v2_sub(vc->center, cw_v2_scale(dl, s / vc->zoom));
      return true;
    }
    V.last = e->pos;
    return false;
  }
  case CW_UI_MOUSE_UP: {
    if (V.guide_drag) {
      bool vx = V.guide_drag == 1 || V.guide_drag == 3;
      bool on_ruler = vx ? px.x < R : px.y < R;
      if (V.guide_drag >= 3) {
        float old = vx ? V.gx[V.guide_index] : V.gy[V.guide_index];
        if (on_ruler || old != V.guide_pos) cwed_runf("view2d.guide axis=%s pos=%s remove=true", vx ? "x" : "y", cwed_gizmo_num(old));
        if (!on_ruler && old != V.guide_pos) cwed_runf("view2d.guide axis=%s pos=%s", vx ? "x" : "y", cwed_gizmo_num(V.guide_pos));
      } else if (!on_ruler) {
        cwed_runf("view2d.guide axis=%s pos=%s", vx ? "x" : "y", cwed_gizmo_num(V.guide_pos));
      }
      V.guide_drag = 0;
      return true;
    }
    if (cwed_transform_dragging()) return cwed_transform_input(&V.ctx, e);
    if (V.ctx.capture) {
      bool r = cwed_gizmos_input_all(&V.ctx, e);
      V.ctx.capture = NULL;
      return r;
    }
    V.panning = false;
    return true;
  }
  case CW_UI_WHEEL:
    zoom_at(vc, px, powf(1.15f, e->wheel.y));
    return true;
  case CW_UI_KEY: {
    if (e->key == 44) { /* space: pan with the left button */
      V.space = !e->repeat || V.space;
      return true;
    }
    if (e->ctrl || e->alt) return false;
    const char *line = NULL;
    switch (e->key) {
    case 20: line = "view2d.tool tool=select"; break;
    case 26: line = "view2d.tool tool=move"; break;
    case 8: line = "view2d.tool tool=rotate"; break;
    case 21: line = "view2d.tool tool=scale"; break;
    case 28: line = "view2d.snap on=toggle"; break;
    case 9: line = "view2d.focus"; break;
    default: break;
    }
    if (line) {
      cwed_run(line, NULL);
      return true;
    }
    return false;
  }
  default: return false;
  }
}

static void register_view_type(void) {
  static cw_node_type t = { .name = "EditorViewport2D", .parent = "Control", .doc = "The editor's 2D viewport (editor module viewport2d)." };
  if (!cw_node_type_find("EditorViewport2D")) {
    cw_node_type_register(&t);
    cw_ui_class_register("EditorViewport2D", &(cw_ui_class){ .draw = view_draw, .input = view_input });
  }
}

/* ── commands ───────────────────────────────────────────────────────── */

static void reply_cam(cwed_call *c, view_cam *vc) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "center");
  cwed_json_str(j, cwed_gizmo_vec2_text(vc->center));
  cwed_json_printf(j, ",\"zoom\":%s", cwed_gizmo_num(vc->zoom));
}

static bool cmd_camera(cwed_call *c) {
  cwed_doc *d = cwed_doc_active();
  if (!d) return cwed_fail(c, "no scene open");
  view_cam *vc = cam_for(d);
  vc->init = true;
  if (cwed_arg_given(c, "center")) {
    cw_value v;
    if (!cw_value_parse(cwed_arg(c, "center"), CW_T_VEC2, &v)) return cwed_fail(c, "center wants vec2(x, y)");
    vc->center = cw_v2(v.v[0], v.v[1]);
  }
  if (cwed_arg_given(c, "zoom")) {
    float z = (float)cwed_arg_float(c, "zoom");
    if (z < 0.02f || z > 64) return cwed_fail(c, "zoom is between 0.02 and 64");
    vc->zoom = z;
  }
  reply_cam(c, vc);
  return true;
}

static bool bounds_of(cw_node n, cw_vec2 *lo, cw_vec2 *hi) {
  bool any = false;
  if (cw_node_is(n, "Node2D")) {
    cw_rect r;
    cw_vec2 q[4];
    if (local_rect(n, &r)) world_quad(n, r, q);
    else {
      cw_xform2d x = cwed_node2d_world(n);
      for (int i = 0; i < 4; i++) q[i] = cw_v2(x.tx, x.ty);
    }
    for (int i = 0; i < 4; i++) {
      if (!any) *lo = *hi = q[i], any = true;
      lo->x = fminf(lo->x, q[i].x), lo->y = fminf(lo->y, q[i].y), hi->x = fmaxf(hi->x, q[i].x), hi->y = fmaxf(hi->y, q[i].y);
    }
  }
  for (int i = 0; i < cw_node_child_count(n); i++) {
    cw_vec2 a, b;
    if (!bounds_of(cw_node_child(n, i), &a, &b)) continue;
    if (!any) *lo = a, *hi = b, any = true;
    lo->x = fminf(lo->x, a.x), lo->y = fminf(lo->y, a.y), hi->x = fmaxf(hi->x, b.x), hi->y = fmaxf(hi->y, b.y);
  }
  return any;
}

static bool frame_bounds(view_cam *vc, cw_vec2 lo, cw_vec2 hi) {
  vc->center = cw_v2((lo.x + hi.x) / 2, (lo.y + hi.y) / 2);
  float w = fmaxf(hi.x - lo.x, 16), h = fmaxf(hi.y - lo.y, 16);
  float W = V.tw > 0 ? (float)V.tw : 800, H = V.th > 0 ? (float)V.th : 450;
  vc->zoom = fminf(fmaxf(fminf(W / (w * 1.15f), H / (h * 1.15f)), 0.02f), 16);
  return true;
}

static bool cmd_focus(cwed_call *c) {
  cwed_doc *d = cwed_doc_active();
  if (!d) return cwed_fail(c, "no scene open");
  view_cam *vc = cam_for(d);
  cw_vec2 lo = { 0 }, hi = { 0 };
  bool any = false;
  if (cwed_arg_given(c, "path")) {
    cw_node n = cwed_arg_node(c, "path");
    if (CW_HANDLE_IS_NULL(n)) return false;
    any = bounds_of(n, &lo, &hi);
  } else {
    for (int i = 0; i < cwed_selection_count(); i++) {
      cw_vec2 a, b;
      if (!bounds_of(cwed_selection_get(i), &a, &b)) continue;
      if (!any) lo = a, hi = b, any = true;
      lo.x = fminf(lo.x, a.x), lo.y = fminf(lo.y, a.y), hi.x = fmaxf(hi.x, b.x), hi.y = fmaxf(hi.y, b.y);
    }
    if (!any) any = bounds_of(cwed_doc_root(d), &lo, &hi);
  }
  if (!any) return cwed_fail(c, "nothing 2D to frame");
  vc->init = true;
  frame_bounds(vc, lo, hi);
  reply_cam(c, vc);
  return true;
}

static const char *const TOOL_NAMES[CWED_TOOL_COUNT] = { "select", "move", "rotate", "scale" };
static bool cmd_tool(cwed_call *c) {
  const char *t = cwed_arg(c, "tool");
  for (int i = 0; i < CWED_TOOL_COUNT; i++)
    if (!strcmp(t, TOOL_NAMES[i])) {
      cwed_setting_put("viewport2d.tool", cw_frame_printf("%d", i));
      return true;
    }
  return cwed_fail(c, "tool is select, move, rotate or scale");
}
static bool on_arg(cwed_call *c, const char *key) {
  if (!strcmp(cwed_arg(c, "on"), "toggle")) return !cwed_setting_bool(key);
  return cwed_arg_bool(c, "on");
}
static bool cmd_snap(cwed_call *c) {
  if (cwed_arg_given(c, "on")) cwed_setting_put("viewport2d.snap", on_arg(c, "viewport2d.snap") ? "true" : "false");
  if (cwed_arg_given(c, "step")) cwed_setting_put("viewport2d.snap_move", cwed_gizmo_num(cwed_arg_float(c, "step")));
  if (cwed_arg_given(c, "angle")) cwed_setting_put("viewport2d.snap_angle", cwed_gizmo_num(cwed_arg_float(c, "angle")));
  if (cwed_arg_given(c, "scale")) cwed_setting_put("viewport2d.snap_scale", cwed_gizmo_num(cwed_arg_float(c, "scale")));
  cwed_json_printf(cwed_reply(c), "\"snap\":%s,\"step\":%s,\"angle\":%s,\"scale\":%s", cwed_setting_bool("viewport2d.snap") ? "true" : "false",
                   cwed_setting("viewport2d.snap_move"), cwed_setting("viewport2d.snap_angle"), cwed_setting("viewport2d.snap_scale"));
  return true;
}
static bool cmd_grid(cwed_call *c) {
  cwed_setting_put("viewport2d.grid", on_arg(c, "viewport2d.grid") ? "true" : "false");
  return true;
}
static bool cmd_pick(cwed_call *c) {
  cwed_doc *d = cwed_doc_active();
  if (!d) return cwed_fail(c, "no scene open");
  if (!V.tw) return cwed_fail(c, "the 2D viewport has not drawn yet (panel.show id=viewport2d, then a frame)");
  fill_ctx(d, cam_for(d));
  cw_vec2 px = cw_v2((float)cwed_arg_float(c, "x"), (float)cwed_arg_float(c, "y"));
  cw_node n = pick_at(d, px);
  cwed_json_key(cwed_reply(c), "node");
  cwed_json_str(cwed_reply(c), CW_HANDLE_IS_NULL(n) ? "" : cwed_doc_node_path(d, n));
  const char *m = cwed_arg(c, "mode");
  if (!cwed_arg_bool(c, "dry")) pick_select(d, px, !strcmp(m, "add") ? 1 : !strcmp(m, "toggle") ? 2 : 0);
  return true;
}
static bool cmd_state(cwed_call *c) {
  cwed_doc *d = cwed_doc_active();
  load_guides();
  cwed_json *j = cwed_reply(c);
  cwed_json_printf(j, "\"size\":[%d,%d],\"renders\":%u,\"tool\":\"%s\",\"snap\":%s,\"grid\":%s,\"is_2d\":%s", V.tw, V.th, V.renders,
                   TOOL_NAMES[(int)cwed_setting_float("viewport2d.tool") % CWED_TOOL_COUNT], cwed_setting_bool("viewport2d.snap") ? "true" : "false",
                   cwed_setting_bool("viewport2d.grid") ? "true" : "false", d && doc_is_2d(d) ? "true" : "false");
  cwed_json_key(j, "guides");
  cwed_json_str(j, cwed_setting("viewport2d.guides"));
  if (d) {

    reply_cam(c, cam_for(d));
    fill_ctx(d, cam_for(d));
    if (cwed_selection_count() && cw_node_is(cwed_selection_get(0), "Node2D")) {
      cw_xform2d w = cwed_node2d_world(cwed_selection_get(0));
      cw_vec2 p = cwed_gizmo_to_view2(&V.ctx, cw_v2(w.tx, w.ty));
      cwed_json_printf(j, ",\"pivot_px\":[%s,%s]", cwed_gizmo_num(p.x), cwed_gizmo_num(p.y));
    }
  }
  if (cw_node_valid(V.view)) {
    cw_rect g = cw_control_global_rect(V.view);
    cwed_json_printf(j, ",\"view_rect\":[%s,%s,%s,%s]", cwed_gizmo_num(g.x), cwed_gizmo_num(g.y), cwed_gizmo_num(g.w), cwed_gizmo_num(g.h));
  }
  return true;
}

static bool cmd_project(cwed_call *c) {
  cwed_doc *d = cwed_doc_active();
  if (!d) return cwed_fail(c, "no scene open");
  if (!V.tw) return cwed_fail(c, "the 2D viewport has not drawn yet");
  fill_ctx(d, cam_for(d));
  cw_rect g = cw_control_global_rect(V.view);
  float s = px_scale();
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "points");
  cwed_json_raw(j, "[");
  const char *p = cwed_arg(c, "points");
  for (int k = 0; *p; k++) {
    const char *e = strchr(p, '|');
    char item[256];
    snprintf(item, sizeof item, "%.*s", e ? (int)(e - p) : (int)strlen(p), p);
    p += strlen(item) + (e ? 1 : 0);
    cw_value v;
    if (!cw_value_parse(item, CW_T_VEC2, &v)) return cwed_fail(c, "'%s' is not vec2(x, y)", item);
    cw_vec2 px = cwed_gizmo_to_view2(&V.ctx, cw_v2(v.v[0], v.v[1]));
    cwed_json_printf(j, "%s[%s,%s]", k ? "," : "", cwed_gizmo_num(g.x + px.x / s), cwed_gizmo_num(g.y + px.y / s));
  }
  cwed_json_raw(j, "]");
  return true;
}

/* ── panel ───────────────────────────────────────────────────────────── */

static void on_btn(cw_node b, const cw_value *a, int argc, void *user) {
  (void)b, (void)a, (void)argc;
  if (!V.syncing) cwed_run((const char *)user, NULL);
}
static cw_node mkbtn(const char *text, const char *tip, const char *line, bool toggle) {
  cw_node b = cw_node_new("Button", NULL);
  cw_button_set_text(b, text);
  cw_control_set_tooltip(b, tip);
  cw_node_set(b, "focus_mode", cw_val_int(CW_FOCUS_NONE));
  cw_node_set(b, "theme_overrides", cw_val_string("font_size = 12"));
  if (toggle) cw_button_set_toggle_mode(b, true);
  cw_connect(b, "pressed", (cw_node){ 0 }, NULL, on_btn, (void *)line, 0);
  return b;
}
static void sync_toolbar(void) {
  V.syncing = true;
  int tool = (int)cwed_setting_float("viewport2d.tool");
  for (int i = 0; i < CWED_TOOL_COUNT; i++)
    if (cw_node_valid(V.tool_btn[i])) cw_button_set_pressed(V.tool_btn[i], i == tool);
  if (cw_node_valid(V.snap_btn)) cw_button_set_pressed(V.snap_btn, cwed_setting_bool("viewport2d.snap"));
  if (cw_node_valid(V.grid_btn)) cw_button_set_pressed(V.grid_btn, cwed_setting_bool("viewport2d.grid"));
  V.syncing = false;
}

static cw_node create(void *user) {
  (void)user;
  register_view_type();
  cw_node box = cw_node_new("VBoxContainer", NULL);
  cw_node_set(box, "theme_overrides", cw_val_string("separation = 2"));
  cw_node bar = cw_node_new("HBoxContainer", "Toolbar");
  static const char *const lines[CWED_TOOL_COUNT] = { "view2d.tool tool=select", "view2d.tool tool=move", "view2d.tool tool=rotate",
                                                      "view2d.tool tool=scale" };
  static const char *const labels[CWED_TOOL_COUNT] = { "Sel", "Move", "Rot", "Scale" };
  static const char *const tips[CWED_TOOL_COUNT] = { "Select (Q)", "Move (W)", "Rotate (E)", "Scale (R)" };
  for (int i = 0; i < CWED_TOOL_COUNT; i++) {
    V.tool_btn[i] = mkbtn(labels[i], tips[i], lines[i], true);
    cw_node_add_child(bar, V.tool_btn[i]);
  }
  V.snap_btn = mkbtn("Snap", "Snapping (Y): grid step view2d.snap step=8; guides attract", "view2d.snap on=toggle", true);
  V.grid_btn = mkbtn("Grid", "Show the grid (every pixel from zoom 8)", "view2d.grid on=toggle", true);
  cw_node_add_child(bar, V.snap_btn);
  cw_node_add_child(bar, V.grid_btn);
  V.info = cw_node_new("Label", "Info");
  cw_node_set(V.info, "theme_overrides", cw_val_string("font_size = 12; font_color = #8a93a3"));
  cw_node_set(V.info, "ellipsis", cw_val_bool(true));
  cw_node_set(V.info, "horizontal_alignment", cw_val_int(CW_HALIGN_RIGHT));
  cw_control_set_size_flags(V.info, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  cw_node_add_child(bar, V.info);
  cw_node_add_child(box, bar);
  V.view = cw_node_new("EditorViewport2D", "View");
  cw_control_set_size_flags(V.view, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_node_set(V.view, "focus_mode", cw_val_int(CW_FOCUS_CLICK));
  cw_node_set(V.view, "clip_contents", cw_val_bool(true));
  cw_node_add_child(box, V.view);
  V.panel = box;
  sync_toolbar();
  return box;
}

static void frame(double dt) {
  (void)dt;
  if (!cw_node_valid(V.view)) return;
  sync_toolbar();
  const cw_raw_input *in = cw_input_raw();
  if (in && !in->keys[44]) V.space = false;
  cwed_doc *d = cwed_doc_active();
  if (!d || !doc_is_2d(d) || !cw_control_visible_in_tree(V.view)) return;
  cw_rect r = cw_control_rect(V.view);
  float s = px_scale();
  int w = (int)(r.w * s), h = (int)(r.h * s);
  if (w < 8 || h < 8) return;
  if (!cw_texture_valid(V.target)) {
    V.target = cw_target_create(&(cw_target_desc){ .width = w, .height = h, .filter = CW_FILTER_NEAREST });
    V.tw = w, V.th = h;
  } else if (w != V.tw || h != V.th) {
    cw_target_resize(V.target, w, h);
    V.tw = w, V.th = h;
  }
  view_cam *vc = cam_for(d);
  if (!vc->init) {
    vc->init = true;
    cw_vec2 lo, hi;
    float gw, gh;
    game_rect(d, &gw, &gh);
    if (bounds_of(cwed_doc_root(d), &lo, &hi)) {
      lo.x = fminf(lo.x, 0), lo.y = fminf(lo.y, 0), hi.x = fmaxf(hi.x, gw), hi.y = fmaxf(hi.y, gh);
      frame_bounds(vc, lo, hi);
    } else frame_bounds(vc, cw_v2(0, 0), cw_v2(gw, gh));
  }
  load_guides();
  cw_vec2 p = cw_ui_pointer();
  cw_rect g = cw_control_global_rect(V.view);
  V.ctx.pointer = cw_v2((p.x - g.x) * s, (p.y - g.y) * s);
  V.ctx.pointer_inside = p.x >= g.x && p.y >= g.y && p.x < g.x + g.w && p.y < g.y + g.h;
  render(d);
  cw_vec2 wp = cwed_gizmo_from_view2(&V.ctx, V.ctx.pointer);
  cw_label_set_text(V.info, cw_frame_printf("%s%s  zoom %s  %s", TOOL_NAMES[(int)cwed_setting_float("viewport2d.tool") % CWED_TOOL_COUNT],
                                            cwed_setting_bool("viewport2d.snap") ? "  snap" : "", cwed_gizmo_num(roundf(vc->zoom * 100) / 100),
                                            V.ctx.pointer_inside ? cw_frame_printf("(%.0f, %.0f)", wp.x, wp.y) : ""));
}

static void on_doc_active(const cwed_event *e, void *u) {
  (void)u;
  if (e->doc && doc_is_2d(e->doc) && cw_node_valid(V.panel)) cwed_run("panel.show id=viewport2d", NULL);
}

static void shutdown(void) {
  if (cw_texture_valid(V.target)) cw_texture_destroy(V.target);
  V.target = (cw_texture){ 0 };
  cw_array_free(&V.cams);
  cw_array_free(&V.overlay);
}

static void init(void) {
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport2d.tool", .type = CWED_ARG_INT, .def = "0", .min = 0, .max = 3,
                                            .doc = "2D tool: 0 select, 1 move, 2 rotate, 3 scale", .hidden = true });
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport2d.snap", .type = CWED_ARG_BOOL, .def = "true", .doc = "2D snapping (grid and guides)" });
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport2d.snap_move", .type = CWED_ARG_FLOAT, .def = "8", .min = 0.5, .max = 1024,
                                            .doc = "2D grid / move snap step (pixels)" });
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport2d.snap_angle", .type = CWED_ARG_FLOAT, .def = "15", .min = 0.1, .max = 180,
                                            .doc = "2D rotate snap step (degrees)" });
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport2d.snap_scale", .type = CWED_ARG_FLOAT, .def = "0.1", .min = 0.001, .max = 10,
                                            .doc = "2D scale snap step" });
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport2d.grid", .type = CWED_ARG_BOOL, .def = "true", .doc = "Show the 2D grid" });
  cwed_setting_define(&(cwed_setting_desc){ .key = "viewport2d.guides", .type = CWED_ARG_STRING, .def = "",
                                            .doc = "2D guides (view2d.guide)", .hidden = true });
  cwed_panel_register(&(cwed_panel_desc){ .id = "viewport2d", .title = "2D", .slot = "center", .order = 20, .create = create });
  static const cwed_param cam_p[] = {
    { "center", CWED_ARG_STRING, "", "world point at the view centre, vec2(x, y)" }, { "zoom", CWED_ARG_FLOAT, "", "view pixels per world unit" }, { 0 } };
  static const cwed_param focus_p[] = { { "path", CWED_ARG_PATH, "", "node to frame (default: the selection, else the scene)" }, { 0 } };
  static const cwed_param tool_p[] = { { "tool", CWED_ARG_STRING, NULL, "select, move, rotate or scale" }, { 0 } };
  static const cwed_param on_p[] = { { "on", CWED_ARG_STRING, "toggle", "true, false or toggle" }, { 0 } };
  static const cwed_param snap_p[] = {
    { "on", CWED_ARG_STRING, "", "true, false or toggle" }, { "step", CWED_ARG_FLOAT, "", "grid / move step (pixels)" },
    { "angle", CWED_ARG_FLOAT, "", "rotate step (degrees)" }, { "scale", CWED_ARG_FLOAT, "", "scale step" }, { 0 } };
  static const cwed_param pick_p[] = {
    { "x", CWED_ARG_FLOAT, NULL, "viewport pixel x" }, { "y", CWED_ARG_FLOAT, NULL, "viewport pixel y" },
    { "mode", CWED_ARG_STRING, "set", "set, add or toggle" }, { "dry", CWED_ARG_BOOL, "false", "only report the node" }, { 0 } };
  static const cwed_param guide_p[] = {
    { "axis", CWED_ARG_STRING, NULL, "x (a vertical line at x = pos) or y (horizontal)" }, { "pos", CWED_ARG_FLOAT, NULL, "world position" },
    { "remove", CWED_ARG_BOOL, "false", "remove the guide at pos instead" }, { 0 } };
  const uint32_t Q = CWED_CMD_SCENE | CWED_CMD_HIDDEN;
  cwed_command_register(&(cwed_command_desc){ .name = "view2d.camera", .title = "2D View: Camera", .params = cam_p, .flags = Q,
    .doc = "Set the 2D viewport's view: the world point at its centre and the zoom.", .exec = cmd_camera });
  cwed_command_register(&(cwed_command_desc){ .name = "view2d.focus", .title = "2D View: Frame Selection", .params = focus_p, .flags = CWED_CMD_SCENE,
    .doc = "Frame a node, the selection, or the whole scene in the 2D view.", .exec = cmd_focus });
  cwed_command_register(&(cwed_command_desc){ .name = "view2d.tool", .title = "2D View: Tool", .params = tool_p, .flags = CWED_CMD_HIDDEN,
    .doc = "Pick the 2D tool: select, move, rotate, scale.", .exec = cmd_tool });
  cwed_command_register(&(cwed_command_desc){ .name = "view2d.snap", .title = "2D View: Snapping", .params = snap_p,
    .doc = "2D snapping on/off and its steps (grid pixels, angle degrees, scale step).", .exec = cmd_snap });
  cwed_command_register(&(cwed_command_desc){ .name = "view2d.grid", .title = "2D View: Grid", .params = on_p,
    .doc = "Show or hide the 2D grid.", .exec = cmd_grid });
  cwed_command_register(&(cwed_command_desc){ .name = "view2d.pick", .title = "2D View: Pick", .params = pick_p, .flags = Q,
    .doc = "Select the topmost node under a 2D viewport pixel (what a click does); replies with its path.", .exec = cmd_pick });
  cwed_command_register(&(cwed_command_desc){ .name = "view2d.state", .title = "2D View: State", .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "The 2D viewport: size, view, tool, snapping, guides, the selection's pivot in viewport pixels.", .exec = cmd_state });
  static const cwed_param proj_p[] = { { "points", CWED_ARG_STRING, NULL, "world points vec2(x, y) separated by |" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "view2d.project", .title = "2D View: Project Points", .params = proj_p,
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN, .doc = "Where world points are in the window (UI units): what a pointer script aims at.",
    .exec = cmd_project });
  cwed_command_register(&(cwed_command_desc){ .name = "view2d.guide", .title = "2D View: Guide", .params = guide_p, .flags = CWED_CMD_UNDO,
    .doc = "Add a guide line (or remove one) in the 2D view; snapped moves stick to guides. Undo: Edit > Undo Editor Change.",
    .example = "view2d.guide axis=x pos=64", .exec = guide_exec, .undo = guide_undo });
  cwed_menu_add("View/2D: Frame Selection", "view2d.focus", 210);
  cwed_menu_add("View/2D: Toggle Snapping", "view2d.snap on=toggle", 211);
  cwed_subscribe(CWED_EV_DOC_CLOSED, on_doc_closed, NULL);
  cwed_subscribe(CWED_EV_DOC_ACTIVE | CWED_EV_DOC_OPENED, on_doc_active, NULL);
}

CWED_MODULE(viewport2d) = { .title = "2D Viewport", .milestone = "M4.1", .init = init, .frame = frame, .shutdown = shutdown };
