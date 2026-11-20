/* Path2D / Path3D and PathFollow2D / PathFollow3D (cartwheel/curve.h): a curve in a scene and nodes that ride
 * it by distance. */
#include <math.h>
#include <string.h>

#include "cartwheel/curve.h"
#include "scene_internal.h"

typedef struct path {
  const char *points; /* interned text */
  bool closed;
  float bake_interval;
  t3_vec3 up; /* Path3D */
  /* runtime */
  cw_curve *curve;
  int dims;
} path;

typedef struct follow {
  float progress;
  bool loop;
  float speed, h_offset, v_offset;
  int rotation_mode;     /* 3D */
  bool tilt_enabled, use_model_front; /* 3D */
  bool rotates;          /* 2D */
  /* runtime */
  bool at_end;           /* end_reached was sent for this arrival */
} follow;

enum { ROT_NONE, ROT_Y, ROT_XY, ROT_XYZ, ROT_ORIENTED };
static const char *const rot_names[] = { "NONE", "Y", "XY", "XYZ", "ORIENTED", NULL };

static cw_node_type path3d_type, path2d_type, follow3d_type, follow2d_type;
static const cw_node_type *node3d_t, *node2d_t;
static const char *s_curve_changed, *s_end_reached;

static path *path_of(cw_node n) {
  path *p = cw_node_data_t(n, &path3d_type);
  return p ? p : cw_node_data_t(n, &path2d_type);
}

cw_curve *cw_path_curve(cw_node n) {
  path *p = path_of(n);
  return p ? p->curve : NULL;
}

/* ── followers ──────────────────────────────────────────────────────── */

/* progress brought into [0, length]: wrapped when looping, clamped otherwise */
static float place(float progress, float length, bool loop) {
  if (!(length > 0)) return 0;
  if (loop) {
    float p = progress - floorf(progress / length) * length;
    return p < 0 || p >= length ? 0 : p;
  }
  return cw_clampf(progress, 0, length);
}

static t3_quat quat_from_basis(t3_vec3 x, t3_vec3 y, t3_vec3 z) {
  /* the columns of a rotation matrix to a quaternion (sqrt only) */
  float m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
  float tr = m00 + m11 + m22;
  t3_quat q;
  if (tr > 0) {
    float s = sqrtf(tr + 1) * 2;
    q.w = 0.25f * s, q.x = (m21 - m12) / s, q.y = (m02 - m20) / s, q.z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    float s = sqrtf(1 + m00 - m11 - m22) * 2;
    q.w = (m21 - m12) / s, q.x = 0.25f * s, q.y = (m01 + m10) / s, q.z = (m02 + m20) / s;
  } else if (m11 > m22) {
    float s = sqrtf(1 + m11 - m00 - m22) * 2;
    q.w = (m02 - m20) / s, q.x = (m01 + m10) / s, q.y = 0.25f * s, q.z = (m12 + m21) / s;
  } else {
    float s = sqrtf(1 + m22 - m00 - m11) * 2;
    q.w = (m10 - m01) / s, q.x = (m02 + m20) / s, q.y = (m12 + m21) / s, q.z = 0.25f * s;
  }
  return q;
}
static inline t3_vec3 v3cross(t3_vec3 a, t3_vec3 b) { return (t3_vec3){ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static inline t3_vec3 v3norm(t3_vec3 a, t3_vec3 fallback) {
  float l = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
  return l > 1e-12f ? (t3_vec3){ a.x / l, a.y / l, a.z / l } : fallback;
}

static void follow3d_apply(cw_node n, follow *f) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  path *p = cw_node_data_t(cw_node_parent(n), &path3d_type);
  if (!d || !p || !cw_curve_point_count(p->curve)) return;
  float length = cw_curve_length(p->curve);
  bool tilt = f->tilt_enabled && (f->rotation_mode == ROT_XYZ || f->rotation_mode == ROT_ORIENTED);
  cw_curve_frame fr = cw_curve_sample_frame(p->curve, place(f->progress, length, f->loop), f->rotation_mode == ROT_ORIENTED && tilt);
  t3_vec3 fwd = fr.forward, up = fr.up, right = fr.right;
  if (f->rotation_mode != ROT_ORIENTED) {
    /* built from the world's up, as a look-at does */
    t3_vec3 wup = { 0, 1, 0 };
    if (f->rotation_mode == ROT_Y || f->rotation_mode == ROT_NONE) fwd = v3norm((t3_vec3){ fwd.x, 0, fwd.z }, (t3_vec3){ 0, 0, -1 });
    right = v3norm(v3cross(fwd, wup), (t3_vec3){ 1, 0, 0 });
    up = v3cross(right, fwd);
    if (tilt && fr.tilt != 0) {
      float s, c;
      cw_sincos(fr.tilt, &s, &c);
      /* roll about fwd: up' = up cos + (fwd x up) sin, and fwd x up is right */
      t3_vec3 u2 = { up.x * c + right.x * s, up.y * c + right.y * s, up.z * c + right.z * s };
      up = u2;
      right = v3cross(fwd, up);
    }
  }
  t3_vec3 pos = { fr.position.x + right.x * f->h_offset + up.x * f->v_offset, fr.position.y + right.y * f->h_offset + up.y * f->v_offset,
                  fr.position.z + right.z * f->h_offset + up.z * f->v_offset };
  d->position = pos;
  if (f->rotation_mode != ROT_NONE) {
    /* the node's -Z looks along the curve (or its +Z with use_model_front, the node turned half round about up) */
    t3_vec3 z = f->use_model_front ? fwd : (t3_vec3){ -fwd.x, -fwd.y, -fwd.z };
    t3_vec3 x = f->use_model_front ? (t3_vec3){ -right.x, -right.y, -right.z } : right;
    cw_node3d_set_quaternion(n, quat_from_basis(x, up, z));
  }
}

static void follow2d_apply(cw_node n, follow *f) {
  cw_node2d *d = cw_node_data_t(n, node2d_t);
  path *p = cw_node_data_t(cw_node_parent(n), &path2d_type);
  if (!d || !p || !cw_curve_point_count(p->curve)) return;
  float length = cw_curve_length(p->curve);
  cw_vec2 pos, dir;
  cw_curve_sample_dir2d(p->curve, place(f->progress, length, f->loop), &pos, &dir);
  cw_vec2 across = { -dir.y, dir.x };
  d->position = cw_v2(pos.x + dir.x * f->h_offset + across.x * f->v_offset, pos.y + dir.y * f->h_offset + across.y * f->v_offset);
  if (f->rotates) d->rotation = cw_atan2(dir.y, dir.x);
}

static follow *follow_of(cw_node n, bool *is3d) {
  follow *f = cw_node_data_t(n, &follow3d_type);
  if (f) { *is3d = true; return f; }
  *is3d = false;
  return cw_node_data_t(n, &follow2d_type);
}
static void follow_apply(cw_node n) {
  bool is3d;
  follow *f = follow_of(n, &is3d);
  if (!f) return;
  if (is3d) follow3d_apply(n, f);
  else follow2d_apply(n, f);
}
void cw_path_follow_update(cw_node n) { follow_apply(n); }

static float follow_length(cw_node n) {
  path *p = path_of(cw_node_parent(n));
  return p && cw_curve_point_count(p->curve) ? cw_curve_length(p->curve) : 0;
}

static void follow_init(cw_node n, void *data) {
  (void)n;
  follow *f = data;
  f->loop = true;
  f->rotation_mode = ROT_XYZ;
  f->tilt_enabled = true;
  f->rotates = true;
}
static void follow_enter(cw_node n, void *data) {
  (void)data;
  follow_apply(n);
}
static void follow_changed(cw_node n, void *data, const cw_prop *p) {
  follow *f = data;
  if (!strcmp(p->name, "progress") || !strcmp(p->name, "loop")) f->at_end = false;
  follow_apply(n);
}
static void follow_fixed(cw_node n, void *data, double dt) {
  follow *f = data;
  if (f->speed == 0) return;
  float length = follow_length(n);
  if (!(length > 0)) return;
  bool is3d = cw_node_data_t(n, &follow3d_type) != NULL;
  /* drawn between ticks from where it was */
  if (is3d) {
    cw_node3d *d = cw_node_data_t(n, node3d_t);
    d->interpolate = true;
    cw_node3d_snapshot(n);
  } else {
    cw_node2d *d = cw_node_data_t(n, node2d_t);
    d->interpolate = true;
    cw_node2d_snapshot(n);
  }
  float before = f->progress;
  float next = before + f->speed * (float)dt;
  bool ended = false, wrapped = false;
  if (f->loop) {
    if (next >= length || next < 0) {
      next = place(next, length, true);
      wrapped = true;
    }
  } else if (next >= length || next <= 0) {
    next = cw_clampf(next, 0, length);
    ended = true;
  }
  f->progress = next;
  follow_apply(n);
  /* an open path that wraps jumps from one end to the other: do not draw the jump as motion */
  path *p = path_of(cw_node_parent(n));
  if (wrapped && p && !p->closed) {
    if (is3d) cw_node3d_snapshot(n);
    else cw_node2d_snapshot(n);
  }
  if (ended && !f->at_end) {
    f->at_end = true;
    cw_emit(n, s_end_reached, NULL, 0);
  } else if (!ended) f->at_end = false;
}

static void ratio_get(cw_node n, void *data, cw_value *o) {
  follow *f = data;
  float length = follow_length(n);
  *o = cw_val_float(length > 0 ? (double)(place(f->progress, length, f->loop) / length) : 0);
}
static void ratio_set(cw_node n, void *data, const cw_value *in) {
  follow *f = data;
  f->progress = (float)in->f * follow_length(n);
  f->at_end = false;
  follow_apply(n);
}

#define FOLLOW_COMMON \
  CW_PROP_FIELD(follow, progress, CW_T_FLOAT, CW_PROP_DEFAULT, "Distance along the curve."), \
  CW_PROP_ACCESSOR("progress_ratio", CW_T_FLOAT, CW_PROP_EDITOR | CW_PROP_ANIMATABLE, ratio_get, ratio_set, \
                   "progress as 0..1 of the curve's length (not saved: progress is)."), \
  CW_PROP_FIELD(follow, loop, CW_T_BOOL, CW_PROP_DEFAULT, "Wrap progress around the length instead of stopping at the ends."), \
  CW_PROP_FIELD(follow, speed, CW_T_FLOAT, CW_PROP_DEFAULT, "Units a second added to progress every fixed tick (0 = the game moves it).")

static const cw_prop follow3d_props[] = {
  FOLLOW_COMMON,
  CW_PROP_FIELD(follow, h_offset, CW_T_FLOAT, CW_PROP_DEFAULT, "Sideways offset from the curve (along the frame's right)."),
  CW_PROP_FIELD(follow, v_offset, CW_T_FLOAT, CW_PROP_DEFAULT, "Offset from the curve along the frame's up."),
  { .name = "rotation_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(follow, rotation_mode),
    .size = (int)sizeof(int), .enum_names = rot_names,
    .doc = "NONE keeps the node's rotation; Y faces along the curve, level; XY also pitches; XYZ also rolls by the curve's tilt; "
           "ORIENTED uses the curve's own carried frame (loops and corkscrews)." },
  CW_PROP_FIELD(follow, tilt_enabled, CW_T_BOOL, CW_PROP_DEFAULT, "Apply the curve's tilt (XYZ and ORIENTED)."),
  CW_PROP_FIELD(follow, use_model_front, CW_T_BOOL, CW_PROP_DEFAULT, "The node's +Z faces along the curve instead of its -Z."),
  CW_PROP_END,
};
static const cw_prop follow2d_props[] = {
  FOLLOW_COMMON,
  CW_PROP_FIELD(follow, h_offset, CW_T_FLOAT, CW_PROP_DEFAULT, "Offset along the curve's direction."),
  CW_PROP_FIELD(follow, v_offset, CW_T_FLOAT, CW_PROP_DEFAULT, "Offset across the curve."),
  CW_PROP_FIELD(follow, rotates, CW_T_BOOL, CW_PROP_DEFAULT, "Turn the node along the curve."),
  CW_PROP_END,
};
static const char *const follow_signals[] = { "end_reached", NULL };
static cw_node_type follow3d_type = { .name = "PathFollow3D", .parent = "Node3D", .data_size = sizeof(follow), .props = follow3d_props,
                                      .signals = follow_signals,
                                      .doc = "A child of a Path3D that sits on its curve at `progress` (cartwheel/curve.h).",
                                      .init = follow_init, .enter_tree = follow_enter, .fixed_update = follow_fixed,
                                      .prop_changed = follow_changed };
static cw_node_type follow2d_type = { .name = "PathFollow2D", .parent = "Node2D", .data_size = sizeof(follow), .props = follow2d_props,
                                      .signals = follow_signals,
                                      .doc = "A child of a Path2D that sits on its curve at `progress` (cartwheel/curve.h).",
                                      .init = follow_init, .enter_tree = follow_enter, .fixed_update = follow_fixed,
                                      .prop_changed = follow_changed };

/* ── paths ──────────────────────────────────────────────────────────── */

static void path_move_followers(cw_node n) {
  for (int i = 0; i < cw_node_child_count(n); i++) follow_apply(cw_node_child(n, i));
}
static void path_rebuild(cw_node n, path *p) {
  cw_curve_parse(p->curve, p->points, p->dims);
  cw_curve_set_closed(p->curve, p->closed);
  cw_curve_set_bake_interval(p->curve, p->bake_interval);
  if (p->dims == 3) cw_curve_set_up(p->curve, p->up);
  path_move_followers(n);
}
void cw_path_curve_changed(cw_node n) {
  path *p = path_of(n);
  if (!p) return;
  int need = cw_curve_format(p->curve, NULL, 0, p->dims) + 1;
  char *buf = cw_alloc((size_t)need, CW_MEM_SCENE);
  cw_curve_format(p->curve, buf, need, p->dims);
  p->points = cw_intern(buf);
  cw_free(buf);
  p->closed = cw_curve_closed(p->curve);
  p->bake_interval = cw_curve_bake_interval(p->curve);
  path_move_followers(n);
  cw_emit(n, s_curve_changed, NULL, 0);
}
static void path3d_init(cw_node n, void *data) {
  (void)n;
  path *p = data;
  p->points = cw_intern("");
  p->bake_interval = 0.2f;
  p->up = (t3_vec3){ 0, 1, 0 };
  p->dims = 3;
  p->curve = cw_curve_new();
}
static void path2d_init(cw_node n, void *data) {
  (void)n;
  path *p = data;
  p->points = cw_intern("");
  p->bake_interval = 5;
  p->dims = 2;
  p->curve = cw_curve_new();
  cw_curve_set_bake_interval(p->curve, 5);
}
static void path_destroy(cw_node n, void *data) {
  (void)n;
  path *p = data;
  cw_curve_free(p->curve);
  p->curve = NULL;
}
static void path_changed(cw_node n, void *data, const cw_prop *prop) {
  (void)prop;
  path_rebuild(n, data);
  cw_emit(n, s_curve_changed, NULL, 0);
}
static void length_get(cw_node n, void *data, cw_value *o) {
  (void)n;
  path *p = data;
  *o = cw_val_float(cw_curve_point_count(p->curve) ? (double)cw_curve_length(p->curve) : 0);
}

static const char *const path_signals[] = { "curve_changed", NULL };
static const cw_prop path3d_props[] = {
  CW_PROP_FIELD(path, points, CW_T_STRING, CW_PROP_STORED | CW_PROP_EDITOR,
                "The curve: points separated by commas, each \"x y z\", \"x y z  ix iy iz  ox oy oz\" (in and out handles, relative) or that plus a tilt in radians."),
  CW_PROP_FIELD(path, closed, CW_T_BOOL, CW_PROP_STORED | CW_PROP_EDITOR, "Join the last point back to the first."),
  CW_PROP_FIELD_RANGE(path, bake_interval, CW_T_FLOAT, 0.0001, 1e6, "Length of a baked piece, meters (smaller follows bends closer)."),
  CW_PROP_FIELD(path, up, CW_T_VEC3, CW_PROP_STORED | CW_PROP_EDITOR, "The reference up the curve's frames start from."),
  CW_PROP_ACCESSOR("length", CW_T_FLOAT, CW_PROP_READONLY | CW_PROP_EDITOR, length_get, NULL, "The curve's length."),
  CW_PROP_END,
};
static const cw_prop path2d_props[] = {
  CW_PROP_FIELD(path, points, CW_T_STRING, CW_PROP_STORED | CW_PROP_EDITOR,
                "The curve: points separated by commas, each \"x y\" or \"x y  ix iy  ox oy\" (in and out handles, relative)."),
  CW_PROP_FIELD(path, closed, CW_T_BOOL, CW_PROP_STORED | CW_PROP_EDITOR, "Join the last point back to the first."),
  CW_PROP_FIELD_RANGE(path, bake_interval, CW_T_FLOAT, 0.0001, 1e6, "Length of a baked piece, pixels (smaller follows bends closer)."),
  CW_PROP_ACCESSOR("length", CW_T_FLOAT, CW_PROP_READONLY | CW_PROP_EDITOR, length_get, NULL, "The curve's length."),
  CW_PROP_END,
};
static cw_node_type path3d_type = { .name = "Path3D", .parent = "Node3D", .data_size = sizeof(path), .props = path3d_props,
                                    .signals = path_signals, .doc = "A Bezier curve in 3D that PathFollow3D children ride (cartwheel/curve.h).",
                                    .init = path3d_init, .destroy = path_destroy, .prop_changed = path_changed };
static cw_node_type path2d_type = { .name = "Path2D", .parent = "Node2D", .data_size = sizeof(path), .props = path2d_props,
                                    .signals = path_signals, .doc = "A Bezier curve in 2D that PathFollow2D children ride (cartwheel/curve.h).",
                                    .init = path2d_init, .destroy = path_destroy, .prop_changed = path_changed };

void cw__register_path_types(void);
void cw__register_path_types(void) {
  s_curve_changed = cw_intern("curve_changed");
  s_end_reached = cw_intern("end_reached");
  node3d_t = cw_node_type_find("Node3D");
  node2d_t = cw_node_type_find("Node2D");
  cw_node_type_register(&path3d_type);
  cw_node_type_register(&path2d_type);
  cw_node_type_register(&follow3d_type);
  cw_node_type_register(&follow2d_type);
}
