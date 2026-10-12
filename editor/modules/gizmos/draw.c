/* Gizmo toolkit: drawing helpers, camera math, snapping, value text. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gizmos.h"

/* ── transforms of a scene that never processed ────────────────────── */

static t3_vec3 prop3(cw_node n, const char *p, t3_vec3 def) {
  cw_value v;
  if (!cw_node_get_prop(n, p, &v) || v.type != CW_T_VEC3) return def;
  return t3_v3(v.v[0], v.v[1], v.v[2]);
}

void cwed_node3d_world(cw_node n, t3_mat4 *out) {
  t3_mat4_identity(out);
  if (!cw_node_valid(n)) return;
  cw_node p = cw_node_parent(n);
  t3_mat4 parent;
  t3_mat4_identity(&parent);
  for (; cw_node_valid(p); p = cw_node_parent(p))
    if (cw_node_is(p, "Node3D")) {
      cwed_node3d_world(p, &parent);
      break;
    }
  if (!cw_node_is(n, "Node3D")) {
    *out = parent;
    return;
  }
  t3_mat4 local;
  t3_mat4_compose(&local, cw_node3d_position(n), cw_node3d_quaternion(n), prop3(n, "scale", t3_v3(1, 1, 1)));
  t3_mat4_multiply(out, &parent, &local);
}

cw_xform2d cwed_node2d_world(cw_node n) {
  cw_xform2d m = cw_xform2d_identity();
  if (!cw_node_valid(n) || cw_node_is(n, "CanvasLayer")) return m;
  cw_node p = cw_node_parent(n);
  cw_xform2d parent = cw_node_valid(p) ? cwed_node2d_world(p) : m;
  if (!cw_node_is(n, "Node2D")) return parent;
  cw_value pos, rot, sc;
  cw_vec2 P = { 0, 0 }, S = { 1, 1 };
  float R = 0;
  if (cw_node_get_prop(n, "position", &pos)) P = cw_v2(pos.v[0], pos.v[1]);
  if (cw_node_get_prop(n, "rotation", &rot)) R = (float)rot.f;
  if (cw_node_get_prop(n, "scale", &sc)) S = cw_v2(sc.v[0], sc.v[1]);
  cw_xform2d local = cw_xform2d_trs(P, R, S);
  return cw_xform2d_mul(&parent, &local);
}

t3_vec3 cwed_gizmo_xf(const t3_mat4 *xf, t3_vec3 p) { return xf ? t3_vec3_apply_mat4(p, xf) : p; }

/* ── world lines ─────────────────────────────────────────────────────── */

void cwed_gizmo_line3(cwed_gizmo_ctx *c, t3_vec3 a, t3_vec3 b, cw_color col) {
  if (c->line3_fn) c->line3_fn(c, a, b, col);
}

static void basis(t3_vec3 axis, t3_vec3 *u, t3_vec3 *v) {
  axis = t3_vec3_normalize(axis);
  t3_vec3 h = fabsf(axis.y) < 0.9f ? t3_v3(0, 1, 0) : t3_v3(1, 0, 0);
  *u = t3_vec3_normalize(t3_vec3_cross(axis, h));
  *v = t3_vec3_cross(axis, *u);
}

void cwed_gizmo_circle3(cwed_gizmo_ctx *c, t3_vec3 center, t3_vec3 axis, float radius, cw_color col) {
  t3_vec3 u, v;
  basis(axis, &u, &v);
  const int N = 32;
  t3_vec3 prev = t3_vec3_add(center, t3_vec3_scale(u, radius));
  for (int i = 1; i <= N; i++) {
    float a = (float)i / (float)N * 6.2831853f;
    t3_vec3 p = t3_vec3_add(center, t3_vec3_add(t3_vec3_scale(u, cosf(a) * radius), t3_vec3_scale(v, sinf(a) * radius)));
    cwed_gizmo_line3(c, prev, p, col);
    prev = p;
  }
}

static void xf_circle(cwed_gizmo_ctx *c, const t3_mat4 *xf, t3_vec3 center, int axis, float r, cw_color col) {
  const int N = 32;
  t3_vec3 prev = { 0 };
  for (int i = 0; i <= N; i++) {
    float a = (float)i / (float)N * 6.2831853f, s = sinf(a) * r, k = cosf(a) * r;
    t3_vec3 p = axis == 0 ? t3_v3(0, k, s) : axis == 1 ? t3_v3(k, 0, s) : t3_v3(k, s, 0);
    p = cwed_gizmo_xf(xf, t3_vec3_add(center, p));
    if (i) cwed_gizmo_line3(c, prev, p, col);
    prev = p;
  }
}

void cwed_gizmo_box3(cwed_gizmo_ctx *c, const t3_mat4 *xf, t3_vec3 h, cw_color col) {
  t3_vec3 p[8];
  for (int i = 0; i < 8; i++)
    p[i] = cwed_gizmo_xf(xf, t3_v3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z));
  static const int E[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
  for (int i = 0; i < 12; i++) cwed_gizmo_line3(c, p[E[i][0]], p[E[i][1]], col);
}

void cwed_gizmo_sphere3(cwed_gizmo_ctx *c, const t3_mat4 *xf, float r, cw_color col) {
  for (int a = 0; a < 3; a++) xf_circle(c, xf, t3_v3(0, 0, 0), a, r, col);
}

void cwed_gizmo_cylinder3(cwed_gizmo_ctx *c, const t3_mat4 *xf, float r, float h, cw_color col) {
  xf_circle(c, xf, t3_v3(0, h * 0.5f, 0), 1, r, col);
  xf_circle(c, xf, t3_v3(0, -h * 0.5f, 0), 1, r, col);
  for (int i = 0; i < 4; i++) {
    float a = (float)i * 1.5707963f;
    t3_vec3 o = t3_v3(cosf(a) * r, 0, sinf(a) * r);
    cwed_gizmo_line3(c, cwed_gizmo_xf(xf, t3_v3(o.x, h * 0.5f, o.z)), cwed_gizmo_xf(xf, t3_v3(o.x, -h * 0.5f, o.z)), col);
  }
}

void cwed_gizmo_capsule3(cwed_gizmo_ctx *c, const t3_mat4 *xf, float r, float h, cw_color col) {
  float half = h * 0.5f - r;
  if (half < 0) half = 0;
  xf_circle(c, xf, t3_v3(0, half, 0), 1, r, col);
  xf_circle(c, xf, t3_v3(0, -half, 0), 1, r, col);
  for (int i = 0; i < 4; i++) {
    float a = (float)i * 1.5707963f;
    t3_vec3 o = t3_v3(cosf(a) * r, 0, sinf(a) * r);
    cwed_gizmo_line3(c, cwed_gizmo_xf(xf, t3_v3(o.x, half, o.z)), cwed_gizmo_xf(xf, t3_v3(o.x, -half, o.z)), col);
  }
  /* the end caps: half circles in the XY and ZY planes */
  const int N = 12;
  for (int s = -1; s <= 1; s += 2)
    for (int plane = 0; plane < 2; plane++) {
      t3_vec3 prev = { 0 };
      for (int i = 0; i <= N; i++) {
        float a = (float)i / (float)N * 3.1415927f;
        float x = cosf(a) * r, y = sinf(a) * r * (float)s + half * (float)s;
        t3_vec3 p = plane ? t3_v3(0, y, x) : t3_v3(x, y, 0);
        p = cwed_gizmo_xf(xf, p);
        if (i) cwed_gizmo_line3(c, prev, p, col);
        prev = p;
      }
    }
}

void cwed_gizmo_cone3(cwed_gizmo_ctx *c, const t3_mat4 *xf, float len, float angle, cw_color col) {
  float r = tanf(angle) * len;
  xf_circle(c, xf, t3_v3(0, 0, -len), 2, r, col);
  for (int i = 0; i < 4; i++) {
    float a = (float)i * 1.5707963f;
    cwed_gizmo_line3(c, cwed_gizmo_xf(xf, t3_v3(0, 0, 0)), cwed_gizmo_xf(xf, t3_v3(cosf(a) * r, sinf(a) * r, -len)), col);
  }
}

/* ── overlay ─────────────────────────────────────────────────────────── */

void cwed_gizmo_overlay_line(cwed_gizmo_ctx *c, cw_vec2 a, cw_vec2 b, cw_color col, float w) {
  if (c->overlay_line_fn) c->overlay_line_fn(c, a, b, col, w);
}
void cwed_gizmo_overlay_circle(cwed_gizmo_ctx *c, cw_vec2 o, float r, cw_color col, float w) {
  const int N = 40;
  cw_vec2 prev = { o.x + r, o.y };
  for (int i = 1; i <= N; i++) {
    float a = (float)i / (float)N * 6.2831853f;
    cw_vec2 p = { o.x + cosf(a) * r, o.y + sinf(a) * r };
    cwed_gizmo_overlay_line(c, prev, p, col, w);
    prev = p;
  }
}
void cwed_gizmo_overlay_disc(cwed_gizmo_ctx *c, cw_vec2 o, float r, cw_color col) {
  cw_vec2 pts[20];
  for (int i = 0; i < 20; i++) pts[i] = cw_v2(o.x + cosf((float)i * 0.31415927f) * r, o.y + sinf((float)i * 0.31415927f) * r);
  if (c->overlay_poly_fn) c->overlay_poly_fn(c, pts, 20, col);
}
void cwed_gizmo_overlay_square(cwed_gizmo_ctx *c, cw_vec2 o, float h, cw_color col) {
  cw_vec2 pts[4] = { { o.x - h, o.y - h }, { o.x + h, o.y - h }, { o.x + h, o.y + h }, { o.x - h, o.y + h } };
  if (c->overlay_poly_fn) c->overlay_poly_fn(c, pts, 4, col);
}
void cwed_gizmo_overlay_text(cwed_gizmo_ctx *c, cw_vec2 at, const char *text, cw_color col) {
  if (c->overlay_text_fn) c->overlay_text_fn(c, at, text, col);
}

static void to_clip(const cwed_gizmo_ctx *c, t3_vec3 p, float out[4]) {
  const float *e = c->view_proj.e;
  out[0] = e[0] * p.x + e[4] * p.y + e[8] * p.z + e[12];
  out[1] = e[1] * p.x + e[5] * p.y + e[9] * p.z + e[13];
  out[2] = e[2] * p.x + e[6] * p.y + e[10] * p.z + e[14];
  out[3] = e[3] * p.x + e[7] * p.y + e[11] * p.z + e[15];
}
static cw_vec2 clip_to_px(const cwed_gizmo_ctx *c, const float k[4]) {
  float w = k[3] != 0 ? k[3] : 1e-6f;
  return cw_v2((k[0] / w * 0.5f + 0.5f) * c->width, (0.5f - k[1] / w * 0.5f) * c->height);
}

bool cwed_gizmo_project(const cwed_gizmo_ctx *c, t3_vec3 p, cw_vec2 *out) {
  float k[4];
  to_clip(c, p, k);
  if (k[3] <= 1e-5f) return false;
  *out = clip_to_px(c, k);
  return true;
}

void cwed_gizmo_overlay_line3(cwed_gizmo_ctx *c, t3_vec3 a, t3_vec3 b, cw_color col, float w) {
  float ka[4], kb[4];
  to_clip(c, a, ka);
  to_clip(c, b, kb);
  const float eps = 1e-4f;
  if (ka[3] < eps && kb[3] < eps) return;
  if (ka[3] < eps || kb[3] < eps) { /* clip at w = eps */
    float t = (eps - ka[3]) / (kb[3] - ka[3]);
    float m[4];
    for (int i = 0; i < 4; i++) m[i] = ka[i] + (kb[i] - ka[i]) * t;
    if (ka[3] < eps) memcpy(ka, m, sizeof m);
    else memcpy(kb, m, sizeof m);
  }
  cwed_gizmo_overlay_line(c, clip_to_px(c, ka), clip_to_px(c, kb), col, w);
}

float cwed_gizmo_world_per_px(const cwed_gizmo_ctx *c, t3_vec3 p) {
  const float *v = c->view.e, *pr = c->proj.e;
  float vz = v[2] * p.x + v[6] * p.y + v[10] * p.z + v[14];
  float f = pr[5] != 0 ? pr[5] : 1;
  if (pr[15] == 1) return 2.0f / (f * (c->height > 0 ? c->height : 1)); /* orthographic */
  float d = -vz;
  if (d < 1e-3f) d = 1e-3f;
  return 2.0f * d / (f * (c->height > 0 ? c->height : 1));
}

bool cwed_gizmo_ray_plane(const cwed_gizmo_ctx *c, t3_vec3 point, t3_vec3 normal, t3_vec3 *hit) {
  float den = t3_vec3_dot(normal, c->ray_dir);
  if (fabsf(den) < 1e-6f) return false;
  float t = t3_vec3_dot(t3_vec3_sub(point, c->ray_origin), normal) / den;
  if (t < 0) return false;
  *hit = t3_vec3_add(c->ray_origin, t3_vec3_scale(c->ray_dir, t));
  return true;
}

float cwed_gizmo_ray_axis(const cwed_gizmo_ctx *c, t3_vec3 o, t3_vec3 a) {
  a = t3_vec3_normalize(a);
  t3_vec3 d = c->ray_dir, w = t3_vec3_sub(o, c->ray_origin);
  float b = t3_vec3_dot(a, d), dd = t3_vec3_dot(a, w), e = t3_vec3_dot(d, w);
  float den = 1 - b * b;
  if (fabsf(den) < 1e-6f) return 0;
  return (b * e - dd) / den;
}

float cwed_gizmo_dist_segment(cw_vec2 p, cw_vec2 a, cw_vec2 b) {
  cw_vec2 ab = cw_v2_sub(b, a), ap = cw_v2_sub(p, a);
  float l = cw_v2_dot(ab, ab), t = l > 0 ? cw_v2_dot(ap, ab) / l : 0;
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  return cw_v2_length(cw_v2_sub(p, cw_v2_add(a, cw_v2_scale(ab, t))));
}

/* ── 2D ──────────────────────────────────────────────────────────────── */

cw_vec2 cwed_gizmo_to_view2(const cwed_gizmo_ctx *c, cw_vec2 w) { return cw_xform2d_apply(&c->view2, w); }
cw_vec2 cwed_gizmo_from_view2(const cwed_gizmo_ctx *c, cw_vec2 px) {
  cw_xform2d inv = cw_xform2d_invert(&c->view2);
  return cw_xform2d_apply(&inv, px);
}
void cwed_gizmo_line2(cwed_gizmo_ctx *c, cw_vec2 a, cw_vec2 b, cw_color col, float w) {
  cwed_gizmo_overlay_line(c, cwed_gizmo_to_view2(c, a), cwed_gizmo_to_view2(c, b), col, w);
}

/* ── snapping, numbers ───────────────────────────────────────────────── */

float cwed_gizmo_snap(float v, float step) {
  if (step <= 0) return v;
  return roundf(v / step) * step;
}

const char *cwed_gizmo_num(double v) {
  if (v == 0) v = 0; /* no -0 */
  /* fixed notation when it reads back exactly (rulers, values in command lines) */
  for (int p = 0; p <= 7; p++) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.*f", p, v);
    if ((float)strtod(buf, NULL) == (float)v) return cw_frame_printf("%s", buf);
  }
  for (int p = 1; p <= 9; p++) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.*g", p, v);
    if ((float)strtod(buf, NULL) == (float)v) return cw_frame_printf("%s", buf);
  }
  return cw_frame_printf("%.9g", v);
}
const char *cwed_gizmo_vec2_text(cw_vec2 v) { return cw_frame_printf("vec2(%s, %s)", cwed_gizmo_num(v.x), cwed_gizmo_num(v.y)); }
const char *cwed_gizmo_vec3_text(t3_vec3 v) {
  return cw_frame_printf("vec3(%s, %s, %s)", cwed_gizmo_num(v.x), cwed_gizmo_num(v.y), cwed_gizmo_num(v.z));
}

/* ── running the registered gizmos ───────────────────────────────────── */

typedef struct { cwed_gizmo_ctx *c; const cw_ui_event *e; bool taken; } walk_ctx;

static bool type_matches(cw_node n, const cwed_gizmo_desc *g, bool is_3d) {
  return g->is_3d == is_3d && g->node_type && cw_node_is(n, g->node_type);
}

static bool draw_fn(cw_node n, void *u) {
  walk_ctx *w = u;
  for (int i = 0; i < cwed_gizmo_count(); i++) {
    const cwed_gizmo_desc *g = cwed_gizmo_at(i);
    if (!g->draw || !type_matches(n, g, w->c->is_3d)) continue;
    w->c->selected = cwed_selection_has(n);
    g->draw(n, w->c);
  }
  return true;
}

void cwed_gizmos_draw_all(cwed_gizmo_ctx *c) {
  if (!c->doc) return;
  walk_ctx w = { c, NULL, false };
  cw_node_walk(cwed_doc_root(c->doc), draw_fn, &w);
  c->selected = false;
}

bool cwed_gizmos_input_all(cwed_gizmo_ctx *c, const cw_ui_event *e) {
  if (!c->doc) return false;
  /* a gizmo holding the pointer gets everything until the release */
  for (int k = 0; k < cwed_selection_count(); k++) {
    cw_node n = cwed_selection_get(k);
    for (int i = 0; i < cwed_gizmo_count(); i++) {
      const cwed_gizmo_desc *g = cwed_gizmo_at(i);
      if (!g->input || !type_matches(n, g, c->is_3d)) continue;
      if (c->capture && c->capture != (const void *)g) continue;
      c->selected = true;
      if (g->input(n, c, e)) return true;
    }
  }
  return false;
}
