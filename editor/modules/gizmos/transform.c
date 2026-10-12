/* The transform tool: move / rotate / scale handles for the selection, in
 * 3D (Node3D) and 2D (Node2D), local or global axes, with snapping. Drags
 * edit through node.set (one node) or node.set_each (several) with merge
 * key "gizmo", so a drag is one undo step. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gizmos.h"

#define MAXSEL 64
#define HANDLE_PX 90.0f
#define HIT_PX 8.0f

enum { H_NONE = -1, H_X, H_Y, H_Z, H_XY, H_YZ, H_XZ, H_VIEW, H_COUNT };

static const cw_color AXIS_COL[3] = { { 0.93f, 0.29f, 0.33f, 1 }, { 0.45f, 0.82f, 0.30f, 1 }, { 0.30f, 0.55f, 0.98f, 1 } };
static const cw_color HOT = { 1, 0.85f, 0.25f, 1 };

typedef struct {
  cw_node node;
  /* 3D */
  t3_vec3 pos, scale;
  t3_quat quat;
  t3_mat4 parent, parent_inv, world;
  /* 2D */
  cw_vec2 pos2, scale2;
  float rot2;
  cw_xform2d parent2, parent2_inv;
} start_rec;

static struct {
  bool active;
  bool is_3d;
  int handle;
  cwed_tool tool;
  int count;
  start_rec s[MAXSEL];
  t3_vec3 pivot, axes[3];
  float size;          /* handle length (world units) at the press */
  float t0;            /* axis parameter at the press */
  t3_vec3 hit0;        /* plane hit at the press */
  cw_vec2 p0;          /* pointer at the press (px) */
  /* 2D */
  cw_vec2 pivot2, axes2[2];
  cw_vec2 w0;          /* world point at the press */
} D;

bool cwed_transform_dragging(void) { return D.active; }

/* ── the selection to transform (no node whose ancestor is also selected) ─ */

static bool ancestor_selected(cw_node n) {
  for (cw_node p = cw_node_parent(n); cw_node_valid(p); p = cw_node_parent(p))
    if (cwed_selection_has(p)) return true;
  return false;
}
static int targets(bool is_3d, cw_node *out) {
  int k = 0;
  for (int i = 0; i < cwed_selection_count() && k < MAXSEL; i++) {
    cw_node n = cwed_selection_get(i);
    if (!cw_node_is(n, is_3d ? "Node3D" : "Node2D") || ancestor_selected(n)) continue;
    out[k++] = n;
  }
  return k;
}

/* ── 3D geometry of the handles ──────────────────────────────────────── */

static t3_quat world_quat(const t3_mat4 *m) {
  t3_vec3 p, s;
  t3_quat q;
  t3_mat4_decompose(m, &p, &q, &s);
  return q;
}

bool cwed_transform_pivot3(cwed_gizmo_ctx *c, t3_vec3 *out) {
  (void)c;
  cw_node t[MAXSEL];
  if (!targets(true, t)) return false;
  t3_mat4 w;
  cwed_node3d_world(t[0], &w);
  *out = t3_mat4_get_position(&w);
  return true;
}

static void frame3(cwed_gizmo_ctx *c, t3_vec3 *pivot, t3_vec3 axes[3], float *size) {
  cw_node t[MAXSEL];
  int k = targets(true, t);
  t3_mat4 w;
  cwed_node3d_world(k ? t[0] : (cw_node){ 0 }, &w);
  *pivot = t3_mat4_get_position(&w);
  if (c->local && k) {
    t3_quat q = world_quat(&w);
    axes[0] = t3_vec3_apply_quat(t3_v3(1, 0, 0), q);
    axes[1] = t3_vec3_apply_quat(t3_v3(0, 1, 0), q);
    axes[2] = t3_vec3_apply_quat(t3_v3(0, 0, 1), q);
  } else {
    axes[0] = t3_v3(1, 0, 0), axes[1] = t3_v3(0, 1, 0), axes[2] = t3_v3(0, 0, 1);
  }
  *size = HANDLE_PX * c->px * cwed_gizmo_world_per_px(c, *pivot);
}

static t3_vec3 at(t3_vec3 o, t3_vec3 a, float t) { return t3_vec3_add(o, t3_vec3_scale(a, t)); }

/* plane handle corners: the small square between two axes */
static void plane_quad(t3_vec3 o, t3_vec3 a, t3_vec3 b, float s, t3_vec3 q[4]) {
  q[0] = t3_vec3_add(o, t3_vec3_add(t3_vec3_scale(a, s * 0.25f), t3_vec3_scale(b, s * 0.25f)));
  q[1] = t3_vec3_add(o, t3_vec3_add(t3_vec3_scale(a, s * 0.45f), t3_vec3_scale(b, s * 0.25f)));
  q[2] = t3_vec3_add(o, t3_vec3_add(t3_vec3_scale(a, s * 0.45f), t3_vec3_scale(b, s * 0.45f)));
  q[3] = t3_vec3_add(o, t3_vec3_add(t3_vec3_scale(a, s * 0.25f), t3_vec3_scale(b, s * 0.45f)));
}
static const int PLANE_AXES[3][2] = { { 0, 1 }, { 1, 2 }, { 0, 2 } }; /* XY YZ XZ */

static bool point_in_quad(cw_vec2 p, const cw_vec2 q[4]) {
  int sign = 0;
  for (int i = 0; i < 4; i++) {
    float cr = cw_v2_cross(cw_v2_sub(q[(i + 1) % 4], q[i]), cw_v2_sub(p, q[i]));
    int s = cr > 0 ? 1 : cr < 0 ? -1 : 0;
    if (!s) continue;
    if (!sign) sign = s;
    else if (s != sign) return false;
  }
  return true;
}

static float ring_dist(cwed_gizmo_ctx *c, t3_vec3 o, t3_vec3 axis, float r) {
  t3_vec3 u = fabsf(axis.y) < 0.9f ? t3_vec3_normalize(t3_vec3_cross(axis, t3_v3(0, 1, 0)))
                                   : t3_vec3_normalize(t3_vec3_cross(axis, t3_v3(1, 0, 0)));
  t3_vec3 v = t3_vec3_cross(axis, u);
  float best = 1e9f;
  cw_vec2 prev = { 0 };
  bool have = false;
  for (int i = 0; i <= 48; i++) {
    float a = (float)i / 48.0f * 6.2831853f;
    t3_vec3 p = t3_vec3_add(o, t3_vec3_add(t3_vec3_scale(u, cosf(a) * r), t3_vec3_scale(v, sinf(a) * r)));
    cw_vec2 s;
    bool ok = cwed_gizmo_project(c, p, &s);
    if (ok && have) {
      float d = cwed_gizmo_dist_segment(c->pointer, prev, s);
      if (d < best) best = d;
    }
    have = ok;
    prev = s;
  }
  return best;
}

static int hover3(cwed_gizmo_ctx *c) {
  t3_vec3 o, ax[3];
  float s;
  frame3(c, &o, ax, &s);
  cw_vec2 po;
  if (!cwed_gizmo_project(c, o, &po)) return H_NONE;
  float hit = HIT_PX * c->px;
  if (c->tool == CWED_TOOL_ROTATE) {
    int best = H_NONE;
    float bd = hit;
    for (int i = 0; i < 3; i++) {
      float d = ring_dist(c, o, ax[i], s);
      if (d < bd) bd = d, best = i;
    }
    return best;
  }
  if (cw_v2_length(cw_v2_sub(c->pointer, po)) < hit * 1.2f) return H_VIEW;
  if (c->tool == CWED_TOOL_MOVE)
    for (int p = 0; p < 3; p++) {
      t3_vec3 q[4];
      cw_vec2 sq[4];
      plane_quad(o, ax[PLANE_AXES[p][0]], ax[PLANE_AXES[p][1]], s, q);
      bool ok = true;
      for (int i = 0; i < 4; i++) ok = ok && cwed_gizmo_project(c, q[i], &sq[i]);
      if (ok && point_in_quad(c->pointer, sq)) return H_XY + p;
    }
  int best = H_NONE;
  float bd = hit;
  for (int i = 0; i < 3; i++) {
    cw_vec2 pe;
    if (!cwed_gizmo_project(c, at(o, ax[i], s), &pe)) continue;
    float d = cwed_gizmo_dist_segment(c->pointer, po, pe);
    if (d < bd) bd = d, best = i;
  }
  return best;
}

static void draw3(cwed_gizmo_ctx *c) {
  t3_vec3 o, ax[3];
  float s;
  frame3(c, &o, ax, &s);
  int hot = D.active ? D.handle : (c->pointer_inside ? hover3(c) : H_NONE);
  float w = 2.5f * c->px;
  if (c->tool == CWED_TOOL_ROTATE) {
    for (int i = 0; i < 3; i++) {
      t3_vec3 u = fabsf(ax[i].y) < 0.9f ? t3_vec3_normalize(t3_vec3_cross(ax[i], t3_v3(0, 1, 0)))
                                        : t3_vec3_normalize(t3_vec3_cross(ax[i], t3_v3(1, 0, 0)));
      t3_vec3 v = t3_vec3_cross(ax[i], u);
      t3_vec3 prev = t3_vec3_add(o, t3_vec3_scale(u, s));
      for (int k = 1; k <= 48; k++) {
        float a = (float)k / 48.0f * 6.2831853f;
        t3_vec3 p = t3_vec3_add(o, t3_vec3_add(t3_vec3_scale(u, cosf(a) * s), t3_vec3_scale(v, sinf(a) * s)));
        cwed_gizmo_overlay_line3(c, prev, p, hot == i ? HOT : AXIS_COL[i], w);
        prev = p;
      }
    }
    if (D.active) cwed_gizmo_overlay_line3(c, o, D.hit0, cw_rgba(1, 1, 1, 0.6f), 1.5f * c->px);
    return;
  }
  for (int i = 0; i < 3; i++) {
    t3_vec3 e = at(o, ax[i], s);
    cw_color col = hot == i ? HOT : AXIS_COL[i];
    cwed_gizmo_overlay_line3(c, o, e, col, w);
    cw_vec2 pe;
    if (cwed_gizmo_project(c, e, &pe)) {
      if (c->tool == CWED_TOOL_SCALE) cwed_gizmo_overlay_square(c, pe, 5 * c->px, col);
      else cwed_gizmo_overlay_disc(c, pe, 5.5f * c->px, col);
    }
  }
  if (c->tool == CWED_TOOL_MOVE)
    for (int p = 0; p < 3; p++) {
      t3_vec3 q[4];
      cw_vec2 sq[4];
      plane_quad(o, ax[PLANE_AXES[p][0]], ax[PLANE_AXES[p][1]], s, q);
      bool ok = true;
      for (int i = 0; i < 4; i++) ok = ok && cwed_gizmo_project(c, q[i], &sq[i]);
      if (!ok) continue;
      cw_color col = hot == H_XY + p ? HOT : AXIS_COL[3 - PLANE_AXES[p][0] - PLANE_AXES[p][1]];
      col.a = hot == H_XY + p ? 0.8f : 0.45f;
      if (c->overlay_poly_fn) c->overlay_poly_fn(c, sq, 4, col);
    }
  cw_vec2 po;
  if (cwed_gizmo_project(c, o, &po)) cwed_gizmo_overlay_circle(c, po, 6 * c->px, hot == H_VIEW ? HOT : cw_rgba(1, 1, 1, 0.9f), 1.5f * c->px);
}

/* ── edits ───────────────────────────────────────────────────────────── */

/* one property on every target: node.set for one, node.set_each for many */
static void commit(cwed_gizmo_ctx *c, const char *prop, const char *const *values, int n) {
  if (n == 1) {
    cwed_runf("node.set path=%s prop=%s value=%s merge=gizmo", cwed_quote(cwed_doc_node_path(c->doc, D.s[0].node)), prop,
              cwed_quote(values[0]));
    return;
  }
  size_t cap = 64;
  for (int i = 0; i < n; i++) cap += strlen(cwed_doc_node_path(c->doc, D.s[i].node)) + strlen(prop) + strlen(values[i]) + 4;
  char *edits = cw_frame_alloc(cap), *w = edits;
  for (int i = 0; i < n; i++) w += sprintf(w, "%s%s:%s=%s", i ? "|" : "", cwed_doc_node_path(c->doc, D.s[i].node), prop, values[i]);
  cwed_runf("node.set_each edits=%s merge=gizmo", cwed_quote(edits));
}

static t3_vec3 to_local(const start_rec *r, t3_vec3 world) { return t3_vec3_apply_mat4(world, &r->parent_inv); }

static t3_vec3 euler_xyz(t3_quat q) {
  t3_euler e = t3_euler_from_quat(t3_quat_normalize(q), T3_XYZ);
  return t3_v3(e.x, e.y, e.z);
}

static void drag3(cwed_gizmo_ctx *c) {
  const char *vals[MAXSEL] = { 0 };
  int n = D.count;
  float snap_m = c->snap ? c->snap_move : 0;
  if (D.tool == CWED_TOOL_MOVE) {
    t3_vec3 off = { 0 };
    if (D.handle <= H_Z) {
      float t = cwed_gizmo_ray_axis(c, D.pivot, D.axes[D.handle]);
      off = t3_vec3_scale(D.axes[D.handle], cwed_gizmo_snap(t - D.t0, snap_m));
    } else {
      t3_vec3 normal, a, b;
      if (D.handle == H_VIEW) {
        normal = t3_vec3_normalize(t3_vec3_sub(c->cam_pos, D.pivot));
        a = t3_v3(c->view.e[0], c->view.e[4], c->view.e[8]); /* camera right / up in world */
        b = t3_v3(c->view.e[1], c->view.e[5], c->view.e[9]);
      } else {
        a = D.axes[PLANE_AXES[D.handle - H_XY][0]];
        b = D.axes[PLANE_AXES[D.handle - H_XY][1]];
        normal = t3_vec3_normalize(t3_vec3_cross(a, b));
      }
      t3_vec3 hit;
      if (!cwed_gizmo_ray_plane(c, D.pivot, normal, &hit)) return;
      t3_vec3 d = t3_vec3_sub(hit, D.hit0);
      off = t3_vec3_add(t3_vec3_scale(a, cwed_gizmo_snap(t3_vec3_dot(d, a), snap_m)),
                        t3_vec3_scale(b, cwed_gizmo_snap(t3_vec3_dot(d, b), snap_m)));
    }
    for (int i = 0; i < n; i++) {
      t3_vec3 wp = t3_vec3_add(t3_mat4_get_position(&D.s[i].world), off);
      t3_vec3 lp = to_local(&D.s[i], wp);
      /* a snapped move along a parent-aligned axis lands on exact values */
      if (snap_m > 0 && !c->local) {
        lp.x = fabsf(lp.x - D.s[i].pos.x) < 1e-6f ? D.s[i].pos.x : lp.x;
        lp.y = fabsf(lp.y - D.s[i].pos.y) < 1e-6f ? D.s[i].pos.y : lp.y;
        lp.z = fabsf(lp.z - D.s[i].pos.z) < 1e-6f ? D.s[i].pos.z : lp.z;
        t3_vec3 dl = t3_vec3_sub(lp, D.s[i].pos);
        float sx = cwed_gizmo_snap(dl.x, snap_m), sy = cwed_gizmo_snap(dl.y, snap_m), sz = cwed_gizmo_snap(dl.z, snap_m);
        if (fabsf(sx - dl.x) < 1e-4f && fabsf(sy - dl.y) < 1e-4f && fabsf(sz - dl.z) < 1e-4f) {
          /* the parent does not scale or rotate this axis: exact decimal sums */
          lp.x = (float)((double)D.s[i].pos.x + (double)sx);
          lp.y = (float)((double)D.s[i].pos.y + (double)sy);
          lp.z = (float)((double)D.s[i].pos.z + (double)sz);
        }
      }
      vals[i] = cwed_gizmo_vec3_text(lp);
    }
    commit(c, "position", vals, n);
  } else if (D.tool == CWED_TOOL_ROTATE) {
    if (D.handle < H_X || D.handle > H_Z) return;
    t3_vec3 axis = D.axes[D.handle], hit;
    if (!cwed_gizmo_ray_plane(c, D.pivot, axis, &hit)) return;
    t3_vec3 v0 = t3_vec3_sub(D.hit0, D.pivot), v1 = t3_vec3_sub(hit, D.pivot);
    float ang = atan2f(t3_vec3_dot(t3_vec3_cross(v0, v1), axis), t3_vec3_dot(v0, v1));
    if (c->snap && c->snap_angle > 0) ang = cwed_gizmo_snap(ang, c->snap_angle * 0.017453293f);
    t3_quat dq = t3_quat_from_axis_angle(axis, ang);
    const char *pos[MAXSEL] = { 0 };
    for (int i = 0; i < n; i++) {
      t3_quat wq = t3_quat_multiply(dq, world_quat(&D.s[i].world));
      t3_quat pq = world_quat(&D.s[i].parent);
      t3_quat lq = t3_quat_multiply(t3_quat_normalize((t3_quat){ -pq.x, -pq.y, -pq.z, pq.w }), wq);
      vals[i] = cwed_gizmo_vec3_text(euler_xyz(lq));
      t3_vec3 wp = t3_vec3_add(D.pivot, t3_vec3_apply_quat(t3_vec3_sub(t3_mat4_get_position(&D.s[i].world), D.pivot), dq));
      pos[i] = cwed_gizmo_vec3_text(to_local(&D.s[i], wp));
    }
    commit(c, "rotation", vals, n);
    if (n > 1) commit(c, "position", pos, n); /* around the pivot */
  } else if (D.tool == CWED_TOOL_SCALE) {
    float f;
    int axis = -1;
    if (D.handle == H_VIEW) {
      f = 1 + (D.p0.y - c->pointer.y) / (HANDLE_PX * c->px);
    } else if (D.handle <= H_Z) {
      float t = cwed_gizmo_ray_axis(c, D.pivot, D.axes[D.handle]);
      f = D.t0 != 0 ? t / D.t0 : 1;
      axis = D.handle;
    } else return;
    if (c->snap && c->snap_scale > 0) f = cwed_gizmo_snap(f, c->snap_scale);
    for (int i = 0; i < n; i++) {
      t3_vec3 s = D.s[i].scale;
      double sx = s.x, sy = s.y, sz = s.z;
      if (axis < 0 || axis == 0) sx *= f;
      if (axis < 0 || axis == 1) sy *= f;
      if (axis < 0 || axis == 2) sz *= f;
      vals[i] = cwed_gizmo_vec3_text(t3_v3((float)sx, (float)sy, (float)sz));
    }
    commit(c, "scale", vals, n);
  }
}

static bool input3(cwed_gizmo_ctx *c, const cw_ui_event *e) {
  if (e->type == CW_UI_MOUSE_DOWN && e->button == 0 && !D.active) {
    if (c->tool == CWED_TOOL_SELECT) return false;
    int h = hover3(c);
    if (h == H_NONE) return false;
    cw_node t[MAXSEL];
    int k = targets(true, t);
    if (!k) return false;
    memset(&D, 0, sizeof D);
    D.active = true;
    D.is_3d = true;
    D.handle = h;
    D.tool = c->tool;
    D.count = k;
    frame3(c, &D.pivot, D.axes, &D.size);
    for (int i = 0; i < k; i++) {
      start_rec *r = &D.s[i];
      r->node = t[i];
      r->pos = cw_node3d_position(t[i]);
      r->quat = cw_node3d_quaternion(t[i]);
      cw_value sv;
      r->scale = cw_node_get_prop(t[i], "scale", &sv) ? t3_v3(sv.v[0], sv.v[1], sv.v[2]) : t3_v3(1, 1, 1);
      cwed_node3d_world(t[i], &r->world);
      cwed_node3d_world(cw_node_parent(t[i]), &r->parent);
      t3_mat4_invert(&r->parent_inv, &r->parent);
    }
    D.p0 = c->pointer;
    if (h <= H_Z) D.t0 = cwed_gizmo_ray_axis(c, D.pivot, D.axes[h]);
    if (D.tool == CWED_TOOL_SCALE && h <= H_Z && fabsf(D.t0) < 1e-4f) D.t0 = D.size;
    t3_vec3 normal = h == H_VIEW || h <= H_Z ? t3_vec3_normalize(t3_vec3_sub(c->cam_pos, D.pivot))
                                             : t3_vec3_normalize(t3_vec3_cross(D.axes[PLANE_AXES[h - H_XY][0]], D.axes[PLANE_AXES[h - H_XY][1]]));
    if (D.tool == CWED_TOOL_ROTATE && h <= H_Z) normal = D.axes[h];
    if (!cwed_gizmo_ray_plane(c, D.pivot, normal, &D.hit0)) D.hit0 = D.pivot;
    cwed_history_seal(); /* a new drag is a new undo step */
    return true;
  }
  if (!D.active || !D.is_3d) return false;
  if (e->type == CW_UI_MOUSE_MOVE) {
    drag3(c);
    return true;
  }
  if (e->type == CW_UI_MOUSE_UP) {
    if (!e->canceled) drag3(c);
    D.active = false;
    cwed_history_seal();
    return true;
  }
  return false;
}

/* ── 2D ─────────────────────────────────────────────────────────────── */

static void frame2(cwed_gizmo_ctx *c, cw_vec2 *pivot, cw_vec2 axes[2]) {
  cw_node t[MAXSEL];
  int k = targets(false, t);
  cw_xform2d w = k ? cwed_node2d_world(t[0]) : cw_xform2d_identity();
  *pivot = cw_v2(w.tx, w.ty);
  if (c->local && k) {
    axes[0] = cw_v2_normalize(cw_v2(w.a, w.b));
    axes[1] = cw_v2_normalize(cw_v2(w.c, w.d));
  } else {
    axes[0] = cw_v2(1, 0), axes[1] = cw_v2(0, 1);
  }
}

static int hover2(cwed_gizmo_ctx *c) {
  cw_vec2 o, ax[2];
  frame2(c, &o, ax);
  cw_vec2 po = cwed_gizmo_to_view2(c, o);
  float L = HANDLE_PX * c->px * 0.8f, hit = HIT_PX * c->px;
  if (c->tool == CWED_TOOL_ROTATE) {
    float d = fabsf(cw_v2_length(cw_v2_sub(c->pointer, po)) - L);
    return d < hit ? H_Z : H_NONE;
  }
  if (cw_v2_length(cw_v2_sub(c->pointer, po)) < hit * 1.3f) return H_VIEW;
  int best = H_NONE;
  float bd = hit;
  for (int i = 0; i < 2; i++) {
    cw_vec2 dir = cw_v2_normalize(cw_v2_sub(cwed_gizmo_to_view2(c, cw_v2_add(o, ax[i])), po));
    float d = cwed_gizmo_dist_segment(c->pointer, po, cw_v2_add(po, cw_v2_scale(dir, L)));
    if (d < bd) bd = d, best = i;
  }
  return best;
}

static void draw2(cwed_gizmo_ctx *c) {
  cw_vec2 o, ax[2];
  frame2(c, &o, ax);
  cw_vec2 po = cwed_gizmo_to_view2(c, o);
  float L = HANDLE_PX * c->px * 0.8f, w = 2.5f * c->px;
  int hot = D.active ? D.handle : (c->pointer_inside ? hover2(c) : H_NONE);
  if (c->tool == CWED_TOOL_ROTATE) {
    cwed_gizmo_overlay_circle(c, po, L, hot == H_Z ? HOT : AXIS_COL[2], w);
    return;
  }
  for (int i = 0; i < 2; i++) {
    cw_vec2 dir = cw_v2_normalize(cw_v2_sub(cwed_gizmo_to_view2(c, cw_v2_add(o, ax[i])), po));
    cw_vec2 e = cw_v2_add(po, cw_v2_scale(dir, L));
    cw_color col = hot == i ? HOT : AXIS_COL[i];
    cwed_gizmo_overlay_line(c, po, e, col, w);
    if (c->tool == CWED_TOOL_SCALE) cwed_gizmo_overlay_square(c, e, 5 * c->px, col);
    else cwed_gizmo_overlay_disc(c, e, 5.5f * c->px, col);
  }
  cwed_gizmo_overlay_square(c, po, 6 * c->px, hot == H_VIEW ? HOT : cw_rgba(1, 1, 1, 0.85f));
}

static void drag2(cwed_gizmo_ctx *c) {
  const char *vals[MAXSEL] = { 0 };
  int n = D.count;
  cw_vec2 w = cwed_gizmo_from_view2(c, c->pointer), d = cw_v2_sub(w, D.w0);
  float sm = c->snap ? c->snap_move : 0;
  if (D.tool == CWED_TOOL_MOVE) {
    cw_vec2 off;
    if (D.handle == H_VIEW) off = cw_v2(cwed_gizmo_snap(cw_v2_dot(d, D.axes2[0]), sm), cwed_gizmo_snap(cw_v2_dot(d, D.axes2[1]), sm));
    else if (D.handle <= H_Y) off = cw_v2(D.handle == 0 ? cwed_gizmo_snap(cw_v2_dot(d, D.axes2[0]), sm) : 0,
                                          D.handle == 1 ? cwed_gizmo_snap(cw_v2_dot(d, D.axes2[1]), sm) : 0);
    else return;
    cw_vec2 world_off = cw_v2_add(cw_v2_scale(D.axes2[0], off.x), cw_v2_scale(D.axes2[1], off.y));
    if (c->snap) { /* guides: the pivot sticks to a guide line it comes near */
      cw_vec2 p = cw_v2_add(D.pivot2, world_off);
      float tol = 6 / (c->zoom2 > 0 ? c->zoom2 : 1);
      for (int g = 0; g < c->guide_x_count; g++)
        if (fabsf(p.x - c->guides_x[g]) < tol && D.handle != 1) world_off.x += c->guides_x[g] - p.x;
      for (int g = 0; g < c->guide_y_count; g++)
        if (fabsf(p.y - c->guides_y[g]) < tol && D.handle != 0) world_off.y += c->guides_y[g] - p.y;
    }
    for (int i = 0; i < n; i++) {
      /* parent space: the inverse parent transform's linear part */
      const cw_xform2d *pi = &D.s[i].parent2_inv;
      cw_vec2 lo = cw_v2(pi->a * world_off.x + pi->c * world_off.y, pi->b * world_off.x + pi->d * world_off.y);
      vals[i] = cwed_gizmo_vec2_text(cw_v2((float)((double)D.s[i].pos2.x + lo.x), (float)((double)D.s[i].pos2.y + lo.y)));
    }
    commit(c, "position", vals, n);
  } else if (D.tool == CWED_TOOL_ROTATE) {
    cw_vec2 a = cw_v2_sub(D.w0, D.pivot2), b = cw_v2_sub(w, D.pivot2);
    float ang = atan2f(cw_v2_cross(a, b), cw_v2_dot(a, b));
    if (c->snap && c->snap_angle > 0) ang = cwed_gizmo_snap(ang, c->snap_angle * 0.017453293f);
    for (int i = 0; i < n; i++) vals[i] = cwed_gizmo_num(D.s[i].rot2 + ang);
    commit(c, "rotation", vals, n);
  } else if (D.tool == CWED_TOOL_SCALE) {
    float L = HANDLE_PX * c->px * 0.8f;
    cw_vec2 dp = cw_v2_sub(c->pointer, D.p0);
    float f;
    int axis = -1;
    if (D.handle == H_VIEW) f = 1 - dp.y / L;
    else if (D.handle <= H_Y) {
      cw_vec2 dir = cw_v2_normalize(cw_v2_sub(cwed_gizmo_to_view2(c, cw_v2_add(D.pivot2, D.axes2[D.handle])), cwed_gizmo_to_view2(c, D.pivot2)));
      f = 1 + cw_v2_dot(dp, dir) / L;
      axis = D.handle;
    } else return;
    if (c->snap && c->snap_scale > 0) f = cwed_gizmo_snap(f, c->snap_scale);
    for (int i = 0; i < n; i++) {
      cw_vec2 s = D.s[i].scale2;
      vals[i] = cwed_gizmo_vec2_text(cw_v2(axis != 1 ? s.x * f : s.x, axis != 0 ? s.y * f : s.y));
    }
    commit(c, "scale", vals, n);
  }
}

static bool input2(cwed_gizmo_ctx *c, const cw_ui_event *e) {
  if (e->type == CW_UI_MOUSE_DOWN && e->button == 0 && !D.active) {
    if (c->tool == CWED_TOOL_SELECT) return false;
    int h = hover2(c);
    if (h == H_NONE) return false;
    cw_node t[MAXSEL];
    int k = targets(false, t);
    if (!k) return false;
    memset(&D, 0, sizeof D);
    D.active = true;
    D.is_3d = false;
    D.handle = h;
    D.tool = c->tool;
    D.count = k;
    frame2(c, &D.pivot2, D.axes2);
    for (int i = 0; i < k; i++) {
      start_rec *r = &D.s[i];
      r->node = t[i];
      cw_value v;
      r->pos2 = cw_node_get_prop(t[i], "position", &v) ? cw_v2(v.v[0], v.v[1]) : cw_v2(0, 0);
      r->scale2 = cw_node_get_prop(t[i], "scale", &v) ? cw_v2(v.v[0], v.v[1]) : cw_v2(1, 1);
      r->rot2 = cw_node_get_prop(t[i], "rotation", &v) ? (float)v.f : 0;
      r->parent2 = cwed_node2d_world(cw_node_parent(t[i]));
      r->parent2_inv = cw_xform2d_invert(&r->parent2);
    }
    D.p0 = c->pointer;
    D.w0 = cwed_gizmo_from_view2(c, c->pointer);
    cwed_history_seal();
    return true;
  }
  if (!D.active || D.is_3d) return false;
  if (e->type == CW_UI_MOUSE_MOVE) {
    drag2(c);
    return true;
  }
  if (e->type == CW_UI_MOUSE_UP) {
    if (!e->canceled) drag2(c);
    D.active = false;
    cwed_history_seal();
    return true;
  }
  return false;
}

/* ── entry points ───────────────────────────────────────────────────── */

void cwed_transform_draw(cwed_gizmo_ctx *c) {
  cw_node t[MAXSEL];
  if (c->tool == CWED_TOOL_SELECT || !targets(c->is_3d, t)) return;
  if (c->is_3d) draw3(c);
  else draw2(c);
}

bool cwed_transform_input(cwed_gizmo_ctx *c, const cw_ui_event *e) {
  if (D.active && D.is_3d != c->is_3d) return false;
  return c->is_3d ? input3(c, e) : input2(c, e);
}

int cwed_transform_hover(cwed_gizmo_ctx *c) {
  cw_node t[MAXSEL];
  if (c->tool == CWED_TOOL_SELECT || !targets(c->is_3d, t)) return H_NONE;
  return c->is_3d ? hover3(c) : hover2(c);
}
