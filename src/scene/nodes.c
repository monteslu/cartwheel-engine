/* Node, Node3D, Node2D: the base types. */
#include "scene_internal.h"
#include "cartwheel/ui.h"
#include "cartwheel/render3d.h"

/* ── Node ───────────────────────────────────────────────────────────── */

static const char *const pause_names[] = { "INHERIT", "PAUSABLE", "WHEN_PAUSED", "ALWAYS", "DISABLED", NULL };

static void node_get_name(cw_node n, void *d, cw_value *o) { (void)d; *o = cw_val_string(cw_node_name(n)); }
static void node_set_name(cw_node n, void *d, const cw_value *v) { (void)d; cw_node_set_name(n, v->s); }
static void node_get_pause(cw_node n, void *d, cw_value *o) {
  (void)d;
  o->type = CW_T_ENUM;
  o->i = cw__node_rec(n)->pause;
}
static void node_set_pause(cw_node n, void *d, const cw_value *v) { (void)d; cw_node_set_pause_mode(n, (cw_pause_mode)v->i); }
static void node_get_uid(cw_node n, void *d, cw_value *o) { (void)d; *o = cw_val_int((int64_t)cw_node_uid(n)); }

static const cw_prop node_props[] = {
  CW_PROP_ACCESSOR("name", CW_T_STRING, CW_PROP_EDITOR, node_get_name, node_set_name, "Unique among siblings."),
  { .name = "pause_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = -1, .get = node_get_pause,
    .set = node_set_pause, .enum_names = pause_names, .doc = "Whether the node processes while the tree is paused." },
  CW_PROP_ACCESSOR("uid", CW_T_INT, CW_PROP_READONLY | CW_PROP_EDITOR, node_get_uid, NULL, "Stable id (scene files, saves)."),
  CW_PROP_END,
};

static cw_node_type node_type = {
  .name = "Node",
  .props = node_props,
  .doc = "Base of every node: a name in the tree, groups, signals, processing.",
};

/* ── Node3D ─────────────────────────────────────────────────────────────
 * The node's own position/quaternion/scale are the truth; the three.c
 * object is written from them before rendering (interpolated between the
 * last two fixed ticks when `interpolate` is on). Simulation never reads
 * three.c's matrices: global transforms are composed from node data with
 * three.c's arithmetic-only matrix code, which is identical on every host. */

static const cw_node_type *node3d_t;

static void quat_from_euler_xyz(t3_vec3 e, t3_quat *q) {
  /* three.js Quaternion.setFromEuler, order XYZ, with cw_sincos (deterministic) */
  float s1, c1, s2, c2, s3, c3;
  cw_sincos(e.x * 0.5f, &s1, &c1);
  cw_sincos(e.y * 0.5f, &s2, &c2);
  cw_sincos(e.z * 0.5f, &s3, &c3);
  q->x = s1 * c2 * c3 + c1 * s2 * s3;
  q->y = c1 * s2 * c3 - s1 * c2 * s3;
  q->z = c1 * c2 * s3 + s1 * s2 * c3;
  q->w = c1 * c2 * c3 - s1 * s2 * s3;
}

static t3_vec3 euler_xyz_from_quat(t3_quat q) {
  /* three.js Euler.setFromRotationMatrix (XYZ) through the rotation matrix */
  float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
  float xx = q.x * x2, xy = q.x * y2, xz = q.x * z2, yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
  float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
  float m11 = 1 - (yy + zz), m12 = xy - wz, m13 = xz + wy;
  float m22 = 1 - (xx + zz), m23 = yz - wx, m32 = yz + wx, m33 = 1 - (xx + yy);
  float m21 = xy + wz;
  t3_vec3 e;
  e.y = cw_asin(cw_clampf(m13, -1, 1));
  if (fabsf(m13) < 0.9999999f) {
    e.x = cw_atan2(-m23, m33);
    e.z = cw_atan2(-m12, m11);
  } else {
    e.x = cw_atan2(m32, m22);
    e.z = 0;
  }
  (void)m21;
  return e;
}

static void node3d_init(cw_node n, void *data) {
  cw_node3d *d = data;
  d->scale = (t3_vec3){ 1, 1, 1 };
  d->quaternion = (t3_quat){ 0, 0, 0, 1 };
  d->visible = true;
  d->layers = 1;
  d->object = t3_group_new();
  d->object->user_data = (void *)(uintptr_t)n.id;
  d->prev_quaternion = d->quaternion;
  d->prev_position = d->position;
  t3_mat4_identity(&d->world);
}

static void node3d_destroy(cw_node n, void *data) {
  cw_node3d *d = data;
  if (d->object) {
    /* a parent being freed with us takes our object down with its own */
    if (d->object->parent && !cw__parent_freeing(n)) t3_object_remove(d->object->parent, d->object);
    t3_release(d->object);
    d->object = NULL;
  }
}

/* The three.c parent for this node's object: the nearest Node3D ancestor's
 * object, else the tree's 3D scene root. */
static t3_object *object_parent(cw_node n) {
  for (cw_node p = cw_node_parent(n); !CW_HANDLE_IS_NULL(p); p = cw_node_parent(p)) {
    cw_node3d *pd = cw_node_data_t(p, node3d_t);
    if (pd) return pd->object;
  }
  cw_tree *t = cw_node_tree(n);
  return t ? (t3_object *)cw_render3d_scene(t) : NULL;
}

static void node3d_enter(cw_node n, void *data) {
  cw_node3d *d = data;
  t3_object *parent = object_parent(n);
  if (parent && d->object && !d->object->parent) t3_object_add(parent, d->object);
}

static void node3d_exit(cw_node n, void *data) {
  cw_node3d *d = data;
  /* when a Node3D ancestor leaves with us, our object stays under its object:
   * detaching each of 100k children one by one from a shared parent is
   * quadratic, and re-entering the tree keeps the hierarchy as it was */
  if (cw__exiting_with_ancestor(n, node3d_t)) return;
  if (d->object && d->object->parent) t3_object_remove(d->object->parent, d->object);
}

/* a node on other layers than the default puts its whole three.c subtree on them (its meshes are
 * children of its object, and nodes under it follow unless they set their own) */
static void apply_layers(t3_object *o, uint32_t mask) {
  o->layers = mask;
  for (int i = 0; i < o->child_count; i++) apply_layers(o->children[i], mask);
}

static void write_object(cw_node3d *d, double alpha) {
  t3_object *o = d->object;
  if (!o) return;
  t3_vec3 p = d->position;
  t3_quat q = d->quaternion;
  if (d->interpolate && alpha < 1) {
    float a = (float)alpha;
    p = t3_vec3_lerp(d->prev_position, d->position, a);
    q = t3_quat_slerp(d->prev_quaternion, d->quaternion, a); /* rendering only */
  }
  o->position = p;
  o->scale = d->scale;
  t3_object_set_quaternion(o, q);
  o->visible = d->visible;
  uint32_t want = d->layers != 1 ? (uint32_t)d->layers : o->parent ? o->parent->layers : 1u; /* own layers, else the parent's */
  if (want != 1 || o->layers != 1) apply_layers(o, want);
}

static void node3d_pre_render(cw_node n, void *data, double alpha) {
  (void)n;
  write_object(data, alpha);
}

void cw_node3d_snapshot(cw_node n) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  if (!d) return;
  d->prev_position = d->position;
  d->prev_quaternion = d->quaternion;
}

static void rot_get(cw_node n, void *data, cw_value *o) {
  (void)n;
  cw_node3d *d = data;
  *o = cw_val_vec3(d->rotation.x, d->rotation.y, d->rotation.z);
}
static void rot_set(cw_node n, void *data, const cw_value *v) {
  (void)n;
  cw_node3d *d = data;
  d->rotation = (t3_vec3){ v->v[0], v->v[1], v->v[2] };
  quat_from_euler_xyz(d->rotation, &d->quaternion);
}
static void quat_get(cw_node n, void *data, cw_value *o) {
  (void)n;
  cw_node3d *d = data;
  o->type = CW_T_QUAT;
  memcpy(o->v, &d->quaternion, 16);
}
static void quat_set(cw_node n, void *data, const cw_value *v) {
  (void)n;
  cw_node3d *d = data;
  memcpy(&d->quaternion, v->v, 16);
  d->rotation = euler_xyz_from_quat(d->quaternion);
}
static void gpos_get(cw_node n, void *data, cw_value *o) {
  (void)data;
  t3_vec3 p = cw_node3d_global_position(n);
  *o = cw_val_vec3(p.x, p.y, p.z);
}

static const cw_prop node3d_props[] = {
  CW_PROP_FIELD(cw_node3d, position, CW_T_VEC3, CW_PROP_DEFAULT, "Position relative to the parent Node3D."),
  CW_PROP_ACCESSOR("rotation", CW_T_VEC3, CW_PROP_DEFAULT, rot_get, rot_set, "Euler angles XYZ, radians."),
  CW_PROP_ACCESSOR("quaternion", CW_T_QUAT, CW_PROP_ANIMATABLE, quat_get, quat_set, "Rotation as a quaternion."),
  CW_PROP_FIELD(cw_node3d, scale, CW_T_VEC3, CW_PROP_DEFAULT, "Scale relative to the parent."),
  CW_PROP_FIELD(cw_node3d, visible, CW_T_BOOL, CW_PROP_DEFAULT, "Hidden nodes hide their children."),
  CW_PROP_FIELD(cw_node3d, interpolate, CW_T_BOOL, CW_PROP_DEFAULT, "Render between the last two fixed ticks."),
  CW_PROP_FIELD(cw_node3d, layers, CW_T_INT, CW_PROP_DEFAULT, "Render layers (bit n = layer n; 1 = the default layer; -1 = all). A camera draws the layers in its `cull_layers`; its `viewmodel_layers` are drawn after, on top, with `viewmodel_fov`. Applies to the node and everything below it."),
  CW_PROP_ACCESSOR("global_position", CW_T_VEC3, CW_PROP_READONLY | CW_PROP_EDITOR, gpos_get, NULL, "World position."),
  CW_PROP_END,
};

static cw_node_type node3d_type = {
  .name = "Node3D",
  .parent = "Node",
  .data_size = sizeof(cw_node3d),
  .props = node3d_props,
  .doc = "A node with a 3D transform. Drives a three.c object for rendering.",
  .init = node3d_init,
  .destroy = node3d_destroy,
  .enter_tree = node3d_enter,
  .exit_tree = node3d_exit,
  .pre_render = node3d_pre_render,
};

t3_object *cw_node3d_object(cw_node n) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  return d ? d->object : NULL;
}

void cw_node3d_set_object(cw_node n, t3_object *obj) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  if (!d || d->object == obj) return;
  t3_object *parent = d->object ? d->object->parent : NULL;
  t3_object *old = d->object;
  if (old) {
    /* children objects move to the new object */
    while (old->child_count) {
      t3_object *c = old->children[0];
      t3_retain(c);
      t3_object_remove(old, c);
      t3_object_add(obj, c);
      t3_release(c);
    }
    if (parent) t3_object_remove(parent, old);
    t3_release(old);
  }
  t3_retain(obj);
  obj->user_data = (void *)(uintptr_t)n.id;
  d->object = obj;
  if (!parent && cw_node_in_tree(n)) parent = object_parent(n);
  if (parent) t3_object_add(parent, obj);
  write_object(d, 1);
}

void cw_node3d_set_position(cw_node n, t3_vec3 p) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  if (d) d->position = p;
}
void cw_node3d_set_rotation(cw_node n, t3_vec3 e) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  if (!d) return;
  d->rotation = e;
  quat_from_euler_xyz(e, &d->quaternion);
}
void cw_node3d_set_quaternion(cw_node n, t3_quat q) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  if (!d) return;
  d->quaternion = q;
  d->rotation = euler_xyz_from_quat(q);
}
void cw_node3d_set_scale(cw_node n, t3_vec3 s) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  if (d) d->scale = s;
}
t3_vec3 cw_node3d_position(cw_node n) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  return d ? d->position : (t3_vec3){ 0, 0, 0 };
}
t3_quat cw_node3d_quaternion(cw_node n) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  return d ? d->quaternion : (t3_quat){ 0, 0, 0, 1 };
}

const t3_mat4 *cw_node3d_global_transform(cw_node n) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  if (!d) return NULL;
  t3_mat4 local;
  t3_mat4_compose(&local, d->position, d->quaternion, d->scale);
  const t3_mat4 *parent = NULL;
  for (cw_node p = cw_node_parent(n); !CW_HANDLE_IS_NULL(p); p = cw_node_parent(p))
    if (cw_node_data_t(p, node3d_t)) { parent = cw_node3d_global_transform(p); break; }
  if (parent) t3_mat4_multiply(&d->world, parent, &local);
  else d->world = local;
  return &d->world;
}

t3_vec3 cw_node3d_global_position(cw_node n) {
  const t3_mat4 *m = cw_node3d_global_transform(n);
  return m ? (t3_vec3){ m->e[12], m->e[13], m->e[14] } : (t3_vec3){ 0, 0, 0 };
}

void cw_node3d_look_at(cw_node n, t3_vec3 target, t3_vec3 up) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  if (!d) return;
  t3_vec3 eye = cw_node3d_global_position(n);
  t3_mat4 m;
  /* three.js Object3D.lookAt for cameras/lights looks down -Z: eye->target */
  t3_mat4_look_at(&m, eye, target, up);
  t3_quat q = t3_quat_from_rotation_matrix(&m); /* the WORLD rotation wanted */
  /* local = inverse(parent world rotation) * world, so the result is right
   * under rotated ancestors too (three.js does the same) */
  for (cw_node p = cw_node_parent(n); !CW_HANDLE_IS_NULL(p); p = cw_node_parent(p))
    if (cw_node_data_t(p, node3d_t)) {
      t3_vec3 pp, ps;
      t3_quat pq;
      t3_mat4_decompose(cw_node3d_global_transform(p), &pp, &pq, &ps);
      q = t3_quat_multiply((t3_quat){ -pq.x, -pq.y, -pq.z, pq.w }, q);
      break;
    }
  cw_node3d_set_quaternion(n, q);
}

/* ── Node2D ──────────────────────────────────────────────────────────── */

static const cw_node_type *node2d_t;

static void node2d_init(cw_node n, void *data) {
  (void)n;
  cw_node2d *d = data;
  d->scale = cw_v2(1, 1);
  d->visible = true;
  d->z_relative = true;
  for (int i = 0; i < 4; i++) d->modulate[i] = d->self_modulate[i] = 1;
}

static void gpos2_get(cw_node n, void *data, cw_value *o) {
  (void)data;
  cw_vec2 p = cw_node2d_global_position(n);
  *o = cw_val_vec2(p.x, p.y);
}

static const cw_prop node2d_props[] = {
  CW_PROP_FIELD(cw_node2d, position, CW_T_VEC2, CW_PROP_DEFAULT, "Position relative to the parent Node2D, pixels."),
  CW_PROP_FIELD(cw_node2d, rotation, CW_T_FLOAT, CW_PROP_DEFAULT, "Radians, clockwise on screen (y down)."),
  CW_PROP_FIELD(cw_node2d, scale, CW_T_VEC2, CW_PROP_DEFAULT, "Scale relative to the parent."),
  CW_PROP_FIELD(cw_node2d, skew, CW_T_FLOAT, CW_PROP_DEFAULT, "Skew in radians."),
  CW_PROP_FIELD(cw_node2d, z_index, CW_T_INT, CW_PROP_DEFAULT, "Draw order within the layer."),
  CW_PROP_FIELD(cw_node2d, z_relative, CW_T_BOOL, CW_PROP_DEFAULT, "z_index adds to the parent's."),
  CW_PROP_FIELD(cw_node2d, visible, CW_T_BOOL, CW_PROP_DEFAULT, "Hidden nodes hide their children."),
  CW_PROP_FIELD(cw_node2d, interpolate, CW_T_BOOL, CW_PROP_DEFAULT, "Render between the last two fixed ticks."),
  CW_PROP_FIELD(cw_node2d, modulate, CW_T_COLOR, CW_PROP_DEFAULT, "Tint multiplied into this node and its children."),
  CW_PROP_FIELD(cw_node2d, self_modulate, CW_T_COLOR, CW_PROP_DEFAULT, "Tint multiplied into this node only."),
  CW_PROP_ACCESSOR("global_position", CW_T_VEC2, CW_PROP_READONLY | CW_PROP_EDITOR, gpos2_get, NULL, "Canvas position."),
  CW_PROP_END,
};

static cw_node_type node2d_type = {
  .name = "Node2D",
  .parent = "Node",
  .data_size = sizeof(cw_node2d),
  .props = node2d_props,
  .doc = "A node with a 2D transform (pixels, y down).",
  .init = node2d_init,
};

void cw_node2d_set_position(cw_node n, cw_vec2 p) {
  cw_node2d *d = cw_node_data_t(n, node2d_t);
  if (d) d->position = p;
}
cw_vec2 cw_node2d_position(cw_node n) {
  cw_node2d *d = cw_node_data_t(n, node2d_t);
  return d ? d->position : cw_v2(0, 0);
}
void cw_node2d_snapshot(cw_node n) {
  cw_node2d *d = cw_node_data_t(n, node2d_t);
  if (!d) return;
  d->prev_position = d->position;
  d->prev_rotation = d->rotation;
}

const float *cw_node2d_global_transform(cw_node n) {
  cw_node2d *d = cw_node_data_t(n, node2d_t);
  if (!d) return NULL;
  float s, c;
  cw_sincos(d->rotation, &s, &c);
  float l[6] = { c * d->scale.x, s * d->scale.x, -s * d->scale.y, c * d->scale.y, d->position.x, d->position.y };
  if (d->skew != 0) { /* x axis skew */
    float ts = cw_sin(d->skew) / cw_cos(d->skew);
    l[2] += l[0] * ts;
    l[3] += l[1] * ts;
  }
  const float *p = NULL;
  for (cw_node q = cw_node_parent(n); !CW_HANDLE_IS_NULL(q); q = cw_node_parent(q)) {
    if (cw_node_data_t(q, node2d_t)) { p = cw_node2d_global_transform(q); break; }
    if (cw__canvas_layer_t && cw_node_data_t(q, cw__canvas_layer_t)) break; /* a CanvasLayer starts a new canvas */
  }
  if (!p) {
    memcpy(d->world, l, sizeof l);
  } else {
    d->world[0] = p[0] * l[0] + p[2] * l[1];
    d->world[1] = p[1] * l[0] + p[3] * l[1];
    d->world[2] = p[0] * l[2] + p[2] * l[3];
    d->world[3] = p[1] * l[2] + p[3] * l[3];
    d->world[4] = p[0] * l[4] + p[2] * l[5] + p[4];
    d->world[5] = p[1] * l[4] + p[3] * l[5] + p[5];
  }
  return d->world;
}

void cw_node2d_set_rotation(cw_node n, float r) {
  cw_node2d *d = cw_node_data_t(n, node2d_t);
  if (d) d->rotation = r;
}
float cw_node2d_rotation(cw_node n) {
  cw_node2d *d = cw_node_data_t(n, node2d_t);
  return d ? d->rotation : 0;
}

/* the parent's global transform (identity when there is none) */
static bool parent2d_xform(cw_node n, float out[6]) {
  for (cw_node q = cw_node_parent(n); !CW_HANDLE_IS_NULL(q); q = cw_node_parent(q)) {
    if (cw_node_data_t(q, node2d_t)) { memcpy(out, cw_node2d_global_transform(q), sizeof(float) * 6); return true; }
    if (cw__canvas_layer_t && cw_node_data_t(q, cw__canvas_layer_t)) break;
  }
  const float id[6] = { 1, 0, 0, 1, 0, 0 };
  memcpy(out, id, sizeof id);
  return false;
}

float cw_node2d_global_rotation(cw_node n) {
  const float *m = cw_node2d_global_transform(n);
  return m ? cw_atan2(m[1], m[0]) : 0;
}

void cw_node2d_set_global(cw_node n, cw_vec2 pos, float rot) {
  cw_node2d *d = cw_node_data_t(n, node2d_t);
  if (!d) return;
  float p[6];
  if (!parent2d_xform(n, p)) {
    d->position = pos;
    d->rotation = rot;
    return;
  }
  float det = p[0] * p[3] - p[2] * p[1];
  if (det == 0) return;
  float id = 1.0f / det, x = pos.x - p[4], y = pos.y - p[5];
  d->position = cw_v2((p[3] * x - p[2] * y) * id, (-p[1] * x + p[0] * y) * id);
  d->rotation = rot - cw_atan2(p[1], p[0]);
}

cw_vec2 cw_node2d_global_position(cw_node n) {
  const float *m = cw_node2d_global_transform(n);
  return m ? cw_v2(m[4], m[5]) : cw_v2(0, 0);
}

/* ── registration ────────────────────────────────────────────────────── */

void cw_scene_register_builtin_types(void) {
  static bool done;
  if (done) return;
  done = true;
  cw_node_type_register(&node_type);
  cw_node_type_register(&node3d_type);
  node3d_t = &node3d_type;
  cw_node_type_register(&node2d_type);
  node2d_t = &node2d_type;
  cw__register_node3d_types();
  cw__register_node2d_types();
  cw_ui_register_types();
  cw__register_physics2d_types();
  cw__register_physics3d_types();
  cw__scene_register_debug_commands();
  cw__register_anim_types();
  void cw__register_fx_types(void); /* src/fx: particles, trails, decals */
  cw__register_fx_types();
  void cw__register_visibility_types(void); /* src/visibility: culling, LOD, impostors */
  cw__register_visibility_types();
  void cw__register_terrain_types(void); /* src/terrain: Terrain3D, TerrainScatter3D */
  cw__register_terrain_types();
  void cw__register_cloth_types(void); /* src/scene/cloth_nodes.c: Cloth3D, Rope3D (cartwheel/cloth.h) */
  cw__register_cloth_types();
  void cw__register_water_types(void); /* src/water: Water3D, Buoyancy3D (cartwheel/water.h) */
  cw__register_water_types();
  void cw_triggers_register(void); /* src/scene/triggers.c: data-driven rules (cartwheel/triggers.h) */
  cw_triggers_register();
  void cw__register_nav_types(void); /* src/nav: regions, agents, obstacles (cartwheel/nav.h) */
  cw__register_nav_types();
  void cw__register_audio_types(void); /* src/audio: ReverbZone3D */
  cw__register_audio_types();
  void cw__register_gi_types(void); /* src/gi: reflection probes (cartwheel/gi.h) */
  cw__register_gi_types();
  void cw__register_csg_types(void); /* src/scene/csg.c: CSG block-out shapes (cartwheel/csg.h) */
  cw__register_csg_types();
}
