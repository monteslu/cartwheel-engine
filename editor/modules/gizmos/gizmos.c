/* Gizmos module: the built-in gizmos the viewports draw and drive,
 * and node.set_each (several nodes, each its own value, one undo step: a
 * multi-selection drag).
 *
 * 3D visualizers (world lines on the editor layer, every node of the type):
 *   DirectionalLight3D (direction arrow), OmniLight3D (range sphere),
 *   SpotLight3D (cone), Camera3D (frustum), CollisionShape3D (box, sphere,
 *   capsule, convex points), RayCast3D (the ray).
 * 2D (overlay, in world coordinates):
 *   CollisionShape2D (rect, circle, capsule, polygon / segment / chain),
 *   Camera2D (the game's view rect), Light2D (range), LightOccluder2D.
 * Point editing (2D, selected nodes): Line2D.points, Polygon2D.polygon,
 *   LightOccluder2D.polygon, CollisionShape2D.points (POLYGON, SEGMENT,
 *   CHAIN): drag a point (snapped), ctrl+click an edge to insert one,
 *   right-click a point to remove it; every change is node.set with merge
 *   key "points". */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/scene2d.h"
#include "gizmos.h"

static const cw_color LIGHT_COL = { 1, 0.85f, 0.35f, 0.9f };
static const cw_color SHAPE_COL = { 0.35f, 0.78f, 1, 0.95f };
static const cw_color SHAPE_OFF = { 0.5f, 0.5f, 0.55f, 0.7f };
static const cw_color CAM_COL = { 0.8f, 0.7f, 1, 0.9f };

static float fprop(cw_node n, const char *p, float def) {
  cw_value v;
  if (!cw_node_get_prop(n, p, &v)) return def;
  return v.type == CW_T_FLOAT ? (float)v.f : v.type == CW_T_INT || v.type == CW_T_ENUM ? (float)v.i : def;
}
static int eprop(cw_node n, const char *p) {
  cw_value v;
  return cw_node_get_prop(n, p, &v) ? (int)v.i : 0;
}
static bool bprop(cw_node n, const char *p) {
  cw_value v;
  return cw_node_get_prop(n, p, &v) && v.b;
}
static const char *sprop(cw_node n, const char *p) {
  cw_value v;
  return cw_node_get_prop(n, p, &v) && v.type == CW_T_STRING && v.s ? v.s : "";
}
static cw_color dim(cw_color c, bool selected) {
  if (!selected) c.a *= 0.55f;
  return c;
}

/* ── 3D visualizers ─────────────────────────────────────────────────── */

static void dir_light(cw_node n, cwed_gizmo_ctx *c) {
  t3_mat4 w;
  cwed_node3d_world(n, &w);
  cw_color col = dim(LIGHT_COL, c->selected);
  t3_vec3 o = cwed_gizmo_xf(&w, t3_v3(0, 0, 0));
  float s = 0.5f;
  cwed_gizmo_circle3(c, o, t3_vec3_transform_direction(t3_v3(0, 0, 1), &w), s * 0.5f, col);
  for (int i = 0; i < 4; i++) {
    float a = (float)i * 1.5707963f;
    t3_vec3 b = t3_v3(cosf(a) * s * 0.5f, sinf(a) * s * 0.5f, 0);
    cwed_gizmo_line3(c, cwed_gizmo_xf(&w, b), cwed_gizmo_xf(&w, t3_v3(b.x, b.y, -2.5f)), col);
  }
}

static void omni_light(cw_node n, cwed_gizmo_ctx *c) {
  t3_mat4 w;
  cwed_node3d_world(n, &w);
  cw_color col = dim(LIGHT_COL, c->selected);
  cwed_gizmo_sphere3(c, &w, 0.2f, col);
  float r = fprop(n, "range", 0);
  if (r > 0 && c->selected) cwed_gizmo_sphere3(c, &w, r, dim(LIGHT_COL, false));
}

static void spot_light(cw_node n, cwed_gizmo_ctx *c) {
  t3_mat4 w;
  cwed_node3d_world(n, &w);
  float r = fprop(n, "range", 0), a = fprop(n, "angle", 0.5f);
  cwed_gizmo_cone3(c, &w, r > 0 ? r : 2, a > 0 ? a : 0.5f, dim(LIGHT_COL, c->selected));
}

static void camera3d(cw_node n, cwed_gizmo_ctx *c) {
  t3_mat4 w;
  cwed_node3d_world(n, &w);
  float fov = fprop(n, "fov", 70) * 0.017453293f, d = 1.2f;
  float h = tanf(fov * 0.5f) * d, wd = h * 16.0f / 9.0f;
  cw_color col = dim(CAM_COL, c->selected);
  t3_vec3 o = cwed_gizmo_xf(&w, t3_v3(0, 0, 0)), k[4];
  for (int i = 0; i < 4; i++) k[i] = cwed_gizmo_xf(&w, t3_v3((i & 1) ? wd : -wd, (i & 2) ? h : -h, -d));
  for (int i = 0; i < 4; i++) cwed_gizmo_line3(c, o, k[i], col);
  cwed_gizmo_line3(c, k[0], k[1], col);
  cwed_gizmo_line3(c, k[1], k[3], col);
  cwed_gizmo_line3(c, k[3], k[2], col);
  cwed_gizmo_line3(c, k[2], k[0], col);
  /* "up" triangle */
  cwed_gizmo_line3(c, cwed_gizmo_xf(&w, t3_v3(-wd * 0.4f, h * 1.1f, -d)), cwed_gizmo_xf(&w, t3_v3(0, h * 1.5f, -d)), col);
  cwed_gizmo_line3(c, cwed_gizmo_xf(&w, t3_v3(wd * 0.4f, h * 1.1f, -d)), cwed_gizmo_xf(&w, t3_v3(0, h * 1.5f, -d)), col);
}

static void shape3d(cw_node n, cwed_gizmo_ctx *c) {
  t3_mat4 w;
  cwed_node3d_world(n, &w);
  cw_color col = bprop(n, "disabled") ? SHAPE_OFF : SHAPE_COL;
  if (!c->selected) col.a *= 0.7f;
  cw_value sz;
  switch (eprop(n, "shape")) {
  case 0: /* BOX */
    if (cw_node_get_prop(n, "size", &sz)) cwed_gizmo_box3(c, &w, t3_v3(sz.v[0] * 0.5f, sz.v[1] * 0.5f, sz.v[2] * 0.5f), col);
    break;
  case 1: cwed_gizmo_sphere3(c, &w, fprop(n, "radius", 0.5f), col); break;
  case 2: cwed_gizmo_capsule3(c, &w, fprop(n, "radius", 0.5f), fprop(n, "height", 2), col); break;
  case 3: { /* CONVEX: a small cross per point */
    cw_vec2 *unused;
    (void)unused;
    const char *s = sprop(n, "points");
    while (*s) {
      float x, y, z;
      int used = 0;
      if (sscanf(s, " %f %f %f%n", &x, &y, &z, &used) < 3) break;
      t3_vec3 p = t3_v3(x, y, z);
      const float k = 0.05f;
      cwed_gizmo_line3(c, cwed_gizmo_xf(&w, t3_v3(p.x - k, p.y, p.z)), cwed_gizmo_xf(&w, t3_v3(p.x + k, p.y, p.z)), col);
      cwed_gizmo_line3(c, cwed_gizmo_xf(&w, t3_v3(p.x, p.y - k, p.z)), cwed_gizmo_xf(&w, t3_v3(p.x, p.y + k, p.z)), col);
      s += used;
      while (*s == ',' || *s == ' ') s++;
    }
    break;
  }
  default: break;
  }
}

static void raycast3d(cw_node n, cwed_gizmo_ctx *c) {
  t3_mat4 w;
  cwed_node3d_world(n, &w);
  cw_value t;
  if (!cw_node_get_prop(n, "target", &t) || t.type != CW_T_VEC3) return;
  cwed_gizmo_line3(c, cwed_gizmo_xf(&w, t3_v3(0, 0, 0)), cwed_gizmo_xf(&w, t3_v3(t.v[0], t.v[1], t.v[2])), dim(SHAPE_COL, c->selected));
}

/* ── 2D visualizers ─────────────────────────────────────────────────── */

static void poly2(cwed_gizmo_ctx *c, const cw_xform2d *w, const cw_vec2 *p, int n, bool closed, cw_color col) {
  for (int i = 0; i + 1 < n + (closed ? 1 : 0); i++)
    cwed_gizmo_line2(c, cw_xform2d_apply(w, p[i]), cw_xform2d_apply(w, p[(i + 1) % n]), col, 1.5f * c->px);
}

static void ellipse2(cwed_gizmo_ctx *c, const cw_xform2d *w, cw_vec2 o, float rx, float ry, cw_color col) {
  cw_vec2 p[32];
  for (int i = 0; i < 32; i++) p[i] = cw_v2(o.x + cosf((float)i * 0.19634954f) * rx, o.y + sinf((float)i * 0.19634954f) * ry);
  poly2(c, w, p, 32, true, col);
}

static void shape2d(cw_node n, cwed_gizmo_ctx *c) {
  cw_xform2d w = cwed_node2d_world(n);
  cw_color col = bprop(n, "disabled") ? SHAPE_OFF : SHAPE_COL;
  if (!c->selected) col.a *= 0.7f;
  cw_value sz;
  int shape = eprop(n, "shape");
  if (shape == 0 && cw_node_get_prop(n, "size", &sz)) {
    float hx = sz.v[0] * 0.5f, hy = sz.v[1] * 0.5f;
    cw_vec2 p[4] = { { -hx, -hy }, { hx, -hy }, { hx, hy }, { -hx, hy } };
    poly2(c, &w, p, 4, true, col);
  } else if (shape == 1) {
    float r = fprop(n, "radius", 8);
    ellipse2(c, &w, cw_v2(0, 0), r, r, col);
  } else if (shape == 2) {
    float r = fprop(n, "radius", 8), h = fprop(n, "height", 32) * 0.5f - r;
    if (h < 0) h = 0;
    cw_vec2 p[34];
    int k = 0;
    for (int i = 0; i <= 16; i++) p[k++] = cw_v2(cosf((float)i / 16.0f * 3.1415927f + 3.1415927f) * r, -h + sinf((float)i / 16.0f * 3.1415927f + 3.1415927f) * r);
    for (int i = 0; i <= 16; i++) p[k++] = cw_v2(cosf((float)i / 16.0f * 3.1415927f) * r, h + sinf((float)i / 16.0f * 3.1415927f) * r);
    poly2(c, &w, p, k, true, col);
  } else {
    cw_vec2 *pts;
    int k = cw_points_parse(sprop(n, "points"), &pts);
    poly2(c, &w, pts, k, shape == 3 || bprop(n, "loop"), col);
  }
}

static void camera2d(cw_node n, cwed_gizmo_ctx *c) {
  /* the game's view: the CanvasSettings virtual size, else 1280 x 720 */
  float vw = 1280, vh = 720;
  cw_node root = cwed_doc_root(c->doc);
  for (int i = 0; i < cw_node_child_count(root) + 1; i++) {
    cw_node s = i ? cw_node_child(root, i - 1) : root;
    if (cw_node_is(s, "CanvasSettings") && fprop(s, "virtual_width", 0) > 0) {
      vw = fprop(s, "virtual_width", 0);
      vh = fprop(s, "virtual_height", 0);
      break;
    }
  }
  cw_xform2d w = cwed_node2d_world(n);
  float z = fprop(n, "zoom", 1);
  if (z <= 0) z = 1;
  cw_value off;
  cw_vec2 o = cw_node_get_prop(n, "offset", &off) ? cw_v2(off.v[0], off.v[1]) : cw_v2(0, 0);
  float hx = vw * 0.5f / z, hy = vh * 0.5f / z;
  cw_vec2 center = cw_v2(w.tx + o.x, w.ty + o.y);
  if (bprop(n, "anchor_top_left")) center = cw_v2(center.x + hx, center.y + hy);
  cw_xform2d id = cw_xform2d_identity();
  cw_vec2 p[4] = { { center.x - hx, center.y - hy }, { center.x + hx, center.y - hy }, { center.x + hx, center.y + hy }, { center.x - hx, center.y + hy } };
  poly2(c, &id, p, 4, true, dim(CAM_COL, c->selected));
}

static void light2d(cw_node n, cwed_gizmo_ctx *c) {
  cw_xform2d w = cwed_node2d_world(n);
  float r = fprop(n, "range", 128);
  cw_xform2d id = cw_xform2d_identity();
  ellipse2(c, &id, cw_v2(w.tx, w.ty), r, r, dim(LIGHT_COL, c->selected));
}

static void occluder2d(cw_node n, cwed_gizmo_ctx *c) {
  cw_xform2d w = cwed_node2d_world(n);
  cw_vec2 *pts;
  int k = cw_points_parse(sprop(n, "polygon"), &pts);
  poly2(c, &w, pts, k, bprop(n, "closed"), dim(cw_rgba(0.9f, 0.5f, 1, 1), c->selected));
}

/* ── point editing (2D) ──────────────────────────────────────────────── */

static const char *points_prop(cw_node n) {
  if (cw_node_is(n, "Line2D")) return "points";
  if (cw_node_is(n, "Polygon2D") || cw_node_is(n, "LightOccluder2D")) return "polygon";
  if (cw_node_is(n, "CollisionShape2D") && eprop(n, "shape") >= 3) return "points";
  return NULL;
}
static bool points_closed(cw_node n) {
  if (cw_node_is(n, "Polygon2D")) return true;
  if (cw_node_is(n, "CollisionShape2D")) return eprop(n, "shape") == 3 || bprop(n, "loop");
  return bprop(n, "closed");
}

static struct { cw_node node; int point; } P = { { 0 }, -1 };

static const char *points_text(const cw_vec2 *p, int n) {
  size_t cap = 16 + (size_t)n * 40;
  char *s = cw_frame_alloc(cap), *w = s;
  *w = 0;
  for (int i = 0; i < n; i++) w += sprintf(w, "%s%s %s", i ? ", " : "", cwed_gizmo_num(p[i].x), cwed_gizmo_num(p[i].y));
  return s;
}

static void commit_points(cwed_gizmo_ctx *c, cw_node n, const char *prop, const cw_vec2 *p, int k) {
  cwed_runf("node.set path=%s prop=%s value=%s merge=points", cwed_quote(cwed_doc_node_path(c->doc, n)), prop,
            cwed_quote(points_text(p, k)));
}

static void points_draw(cw_node n, cwed_gizmo_ctx *c) {
  const char *prop = points_prop(n);
  if (!prop || !c->selected) return;
  cw_xform2d w = cwed_node2d_world(n);
  cw_vec2 *pts;
  int k = cw_points_parse(sprop(n, prop), &pts);
  poly2(c, &w, pts, k, points_closed(n), cw_rgba(1, 0.75f, 0.3f, 0.9f));
  for (int i = 0; i < k; i++) {
    cw_vec2 v = cwed_gizmo_to_view2(c, cw_xform2d_apply(&w, pts[i]));
    bool hot = P.node.id == n.id && P.point == i;
    cwed_gizmo_overlay_square(c, v, (hot ? 6.0f : 4.5f) * c->px, hot ? cw_rgba(1, 0.9f, 0.3f, 1) : cw_rgba(1, 1, 1, 1));
    cwed_gizmo_overlay_square(c, v, (hot ? 4.5f : 3.0f) * c->px, cw_rgba(0.15f, 0.15f, 0.18f, 1));
  }
}

static bool points_input(cw_node n, cwed_gizmo_ctx *c, const cw_ui_event *e) {
  static const cwed_gizmo_desc *self;
  extern const cwed_gizmo_desc cwed__points_gizmo;
  self = &cwed__points_gizmo;
  const char *prop = points_prop(n);
  if (!prop) return false;
  cw_xform2d w = cwed_node2d_world(n), inv = cw_xform2d_invert(&w);
  cw_vec2 *src;
  int k = cw_points_parse(sprop(n, prop), &src);
  cw_vec2 pts[512];
  if (k > 510) k = 510;
  memcpy(pts, src, sizeof(cw_vec2) * (size_t)k);
  float hit = 7 * c->px;
  int under = -1;
  for (int i = 0; i < k; i++)
    if (cw_v2_length(cw_v2_sub(cwed_gizmo_to_view2(c, cw_xform2d_apply(&w, pts[i])), c->pointer)) < hit) under = i;
  if (e->type == CW_UI_MOUSE_DOWN && e->button == 1 && under >= 0 && k > 2) { /* right-click removes */
    memmove(pts + under, pts + under + 1, sizeof(cw_vec2) * (size_t)(k - under - 1));
    commit_points(c, n, prop, pts, k - 1);
    cwed_history_seal();
    return true;
  }
  if (e->type == CW_UI_MOUSE_DOWN && e->button == 0) {
    if (under < 0 && e->ctrl) { /* ctrl+click on an edge inserts */
      int best = -1;
      float bd = hit;
      int edges = k - 1 + (points_closed(n) ? 1 : 0);
      for (int i = 0; i < edges; i++) {
        float d = cwed_gizmo_dist_segment(c->pointer, cwed_gizmo_to_view2(c, cw_xform2d_apply(&w, pts[i])),
                                          cwed_gizmo_to_view2(c, cw_xform2d_apply(&w, pts[(i + 1) % k])));
        if (d < bd) bd = d, best = i;
      }
      if (best < 0) return false;
      cw_vec2 lp = cw_xform2d_apply(&inv, cwed_gizmo_from_view2(c, c->pointer));
      if (c->snap) lp = cw_v2(cwed_gizmo_snap(lp.x, c->snap_move), cwed_gizmo_snap(lp.y, c->snap_move));
      memmove(pts + best + 2, pts + best + 1, sizeof(cw_vec2) * (size_t)(k - best - 1));
      pts[best + 1] = lp;
      cwed_history_seal();
      commit_points(c, n, prop, pts, k + 1);
      P.node = n;
      P.point = best + 1;
      c->capture = self;
      return true;
    }
    if (under < 0) return false;
    cwed_history_seal();
    P.node = n;
    P.point = under;
    c->capture = self;
    return true;
  }
  if (P.node.id != n.id || P.point < 0 || P.point >= k) return false;
  if (e->type == CW_UI_MOUSE_MOVE && (e->buttons & 1)) {
    cw_vec2 lp = cw_xform2d_apply(&inv, cwed_gizmo_from_view2(c, c->pointer));
    if (c->snap) lp = cw_v2(cwed_gizmo_snap(lp.x, c->snap_move), cwed_gizmo_snap(lp.y, c->snap_move));
    if (lp.x != pts[P.point].x || lp.y != pts[P.point].y) {
      pts[P.point] = lp;
      commit_points(c, n, prop, pts, k);
    }
    return true;
  }
  if (e->type == CW_UI_MOUSE_UP) {
    P.point = -1;
    c->capture = NULL;
    cwed_history_seal();
    return true;
  }
  return false;
}

/* ── node.set_each ───────────────────────────────────────────────────── */

typedef struct { int count; cw_node nodes[64]; const char *props[64]; cw_value old[64]; } each_state;

static bool each_exec(cwed_call *c) {
  cwed_doc *d = cwed_call_doc(c);
  const char *p = cwed_arg(c, "edits");
  each_state *s = cw_calloc(1, sizeof *s, CW_MEM_EDITOR);
  char err[2400] = "";
  while (*p && !err[0]) {
    const char *end = strchr(p, '|');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char item[2048];
    snprintf(item, sizeof item, "%.*s", (int)len, p);
    p += len + (end ? 1 : 0);
    char *colon = strchr(item, ':'), *eq = colon ? strchr(colon, '=') : NULL;
    if (!colon || !eq) {
      snprintf(err, sizeof err, "'%s' is not path:prop=value", item);
      break;
    }
    *colon = 0, *eq = 0;
    const char *path = item, *prop = colon + 1, *text = eq + 1;
    cw_node n = cwed_doc_node(d, path);
    const cw_prop *pp = cw_node_valid(n) ? cw_node_find_prop(n, prop, NULL) : NULL;
    if (!pp || (pp->flags & CW_PROP_READONLY) || s->count >= 64) {
      snprintf(err, sizeof err, "'%s' has no writable property '%s'", path, prop);
      break;
    }
    cw_value v;
    if (!cw_node_get_prop(n, prop, &s->old[s->count])) {
      snprintf(err, sizeof err, "cannot read %s of %s", prop, path);
      break;
    }
    /* parse like node.set: through a scratch call is not available, so by the property's type */
    bool ok;
    if (pp->type == CW_T_ENUM) ok = (v = cw_val_enum(text), true);
    else if (pp->type == CW_T_STRING) {
      size_t l = strlen(text);
      if (l >= 2 && text[0] == '"' && text[l - 1] == '"') ok = cw_value_parse(text, CW_T_STRING, &v);
      else ok = (v = cw_val_string(text), true);
    } else ok = cw_value_parse(text, pp->type, &v);
    if (!ok || !cw_node_set(n, prop, v)) {
      snprintf(err, sizeof err, "%s refused '%s' for %s (%s wanted)", path, text, prop, cw_type_name(pp->type));
      break;
    }
    s->nodes[s->count] = n;
    s->props[s->count] = cw_intern(prop);
    s->count++;
  }
  if (!err[0] && !s->count) snprintf(err, sizeof err, "no edits given (edits=\"Path:prop=value|Other:prop=value\")");
  if (err[0]) {
    for (int i = s->count - 1; i >= 0; i--) cw_node_set(s->nodes[i], s->props[i], s->old[i]);
    cw_free(s);
    return cwed_fail(c, "%s", err);
  }
  each_state *prev = cwed_call_state(c);
  if (prev) cw_free(prev);
  cwed_call_set_state(c, s);
  cwed_json_key(cwed_reply(c), "count");
  cwed_json_printf(cwed_reply(c), "%d", s->count);
  return true;
}
static void each_undo(cwed_call *c) {
  each_state *s = cwed_call_state(c);
  for (int i = s ? s->count - 1 : -1; i >= 0; i--)
    if (cw_node_valid(s->nodes[i])) cw_node_set(s->nodes[i], s->props[i], s->old[i]);
}
static bool each_merge(cwed_call *prev, cwed_call *next) {
  each_state *a = cwed_call_state(prev), *b = cwed_call_state(next);
  if (!a || !b || a->count != b->count) return false;
  for (int i = 0; i < a->count; i++)
    if (a->nodes[i].id != b->nodes[i].id || a->props[i] != b->props[i]) return false;
  return true;
}

/* ── registration ────────────────────────────────────────────────────── */

static const cwed_gizmo_desc BUILTIN[] = {
  { "directional_light", "DirectionalLight3D", true, dir_light, NULL },
  { "omni_light", "OmniLight3D", true, omni_light, NULL },
  { "spot_light", "SpotLight3D", true, spot_light, NULL },
  { "camera3d", "Camera3D", true, camera3d, NULL },
  { "collision_shape3d", "CollisionShape3D", true, shape3d, NULL },
  { "raycast3d", "RayCast3D", true, raycast3d, NULL },
  { "collision_shape2d", "CollisionShape2D", false, shape2d, NULL },
  { "camera2d", "Camera2D", false, camera2d, NULL },
  { "light2d", "Light2D", false, light2d, NULL },
  { "occluder2d", "LightOccluder2D", false, occluder2d, NULL },
};
const cwed_gizmo_desc cwed__points_gizmo = { "points2d", "Node2D", false, points_draw, points_input };

static void init(void) {
  for (size_t i = 0; i < sizeof BUILTIN / sizeof BUILTIN[0]; i++) cwed_gizmo_register(&BUILTIN[i]);
  cwed_gizmo_register(&cwed__points_gizmo);
  static const cwed_param each_p[] = {
    { "edits", CWED_ARG_STRING, NULL, "path:prop=value items separated by | (Player:position=vec3(0, 1, 0)|Crate:rotation=vec3(0, 0.5, 0))" },
    { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "node.set_each", .title = "Set Properties (Each Node)", .params = each_p,
    .flags = CWED_CMD_UNDO | CWED_CMD_SCENE | CWED_CMD_HIDDEN,
    .doc = "Set properties on several nodes, each its own value, as ONE undo step (a gizmo dragging a multi-selection). "
           "With merge=<key>, consecutive runs over the same nodes and properties merge.",
    .example = "node.set_each edits=\"{sel}:position=vec3(0, 2, 0)|{sel2}:position=vec3(1, 0, 0)\"", .exec = each_exec,
    .undo = each_undo, .merge = each_merge });
}

CWED_MODULE(gizmos) = { .title = "Gizmos", .init = init };
