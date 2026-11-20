/* Skeleton3D and BoneAttachment3D (cartwheel/skeleton.h): a node view of a
 * skinned model's bones, and a Node3D that rides one. The bones are the
 * three.c objects the animation system already writes; this file adds no
 * second pose and no copy step between animation and skinning. */
#include <stdio.h>
#include <stdlib.h>

#include "anim_internal.h"
#include "cartwheel/debug.h"
#include "cartwheel/skeleton.h"

#define UNBOUND_EVERY 32   /* after UNBOUND_EAGER misses, look for bones every this many attempts */
#define UNBOUND_EAGER 300

typedef struct skel3d {
  const char *model;       /* property: node path, "" = automatic */
  bool show_rest_only;
  /* runtime */
  cw_node model_node;
  t3_object *root;         /* the model node's object the pose is bound to (borrowed; checked each tick) */
  cw_pose pose;            /* names, parents, rest; pose.bone[] retained while bound */
  bool bound, just_bound, warned;
  uint32_t gen;
  int misses;
} skel3d;

typedef struct attach3d {
  const char *bone_name, *skeleton;
  int32_t bone_idx;
  /* runtime */
  cw_node skel;
  uint32_t gen;
  const char *resolved_name;
  bool warned;
} attach3d;

typedef struct skw {
  CW_ARRAY(cw_node) skels;
  CW_ARRAY(cw_node) attachments;
} skw;

static const cw_node_type *skel_t, *attach_t;

/* ── binding ───────────────────────────────────────────────────────── */

static bool has_bone(const t3_object *o) {
  if (!o) return false;
  if (o->type == T3_BONE) return true;
  for (int i = 0; i < o->child_count; i++)
    if (has_bone(o->children[i])) return true;
  return false;
}

static void sk_unbind(skel3d *d) {
  if (d->bound) {
    for (int i = 0; i < d->pose.count; i++) t3_release(d->pose.bone[i]);
    cw_pose_free(&d->pose);
  }
  d->bound = false;
  d->root = NULL;
  d->model_node = (cw_node){ 0 };
}

/* the node whose object holds the bones, or a null handle */
static cw_node sk_find_model(cw_node n, const skel3d *d) {
  if (d->model && d->model[0]) {
    cw_node m = cw_node_get(n, d->model);
    return cw_node_valid(m) && cw_node_is(m, "Node3D") && has_bone(cw_node3d_object(m)) ? m : (cw_node){ 0 };
  }
  if (has_bone(cw_node3d_object(n))) return n;
  for (cw_node p = cw_node_parent(n); !CW_HANDLE_IS_NULL(p); p = cw_node_parent(p))
    if (cw_node_is(p, "Node3D")) return has_bone(cw_node3d_object(p)) ? p : (cw_node){ 0 };
  return (cw_node){ 0 };
}

/* still the bones of the model we bound: the first bone hangs under the root, and the root is the model's object */
static bool sk_still_valid(const skel3d *d) {
  if (!d->bound) return false;
  if (!cw_node_valid(d->model_node) || cw_node3d_object(d->model_node) != d->root) return false;
  for (const t3_object *o = d->pose.bone[0]; o; o = o->parent)
    if (o == d->root) return true;
  return false;
}

static bool sk_bind(cw_node n, skel3d *d) {
  if (sk_still_valid(d)) return true;
  if (d->bound) sk_unbind(d);
  if (d->misses >= UNBOUND_EAGER && (d->misses % UNBOUND_EVERY)) { d->misses++; return false; }
  cw_node m = sk_find_model(n, d);
  if (!cw_node_valid(m) || !cw_pose_bind(&d->pose, cw_node3d_object(m))) {
    if (++d->misses == UNBOUND_EAGER && !d->warned) {
      d->warned = true;
      cw_warn("Skeleton3D '%s': no bones found (it goes under, or above, the Node3D that holds a skinned glTF model; "
              "or set its `model` path)", cw_node_name(n));
    }
    return false;
  }
  d->misses = 0;
  d->model_node = m;
  d->root = cw_node3d_object(m);
  for (int i = 0; i < d->pose.count; i++) t3_retain(d->pose.bone[i]);
  d->bound = true;
  d->just_bound = true;
  d->gen++;
  cw_value c = cw_val_int(d->pose.count);
  cw_emit(n, "skeleton_bound", &c, 1);
  return true;
}

static skel3d *SK(cw_node n) { return cw_node_data_t(n, skel_t); }
/* the node's data when it is a bound Skeleton3D and `bone` is in range (bone < 0: any) */
static skel3d *sk_bone(cw_node n, int bone) {
  skel3d *d = SK(n);
  if (!d) return NULL;
  if (!d->bound && (!cw_node_tree(n) || !sk_bind(n, d))) return NULL; /* a model set since the last tick */
  if (bone >= d->pose.count) return NULL;
  return d;
}

/* ── matrices ──────────────────────────────────────────────────────── */

/* model-space matrix of a bone from the objects' fields (or the rest pose) */
static void bone_model(const skel3d *d, int bone, bool rest, t3_mat4 *out) {
  const cw_pose *p = &d->pose;
  int chain[128], c = 0;
  for (int b = bone; b >= 0 && c < 128; b = p->parent[b]) chain[c++] = b;
  int top = chain[c - 1];
  /* the objects between the model root and the top bone */
  t3_object *above[64];
  int na = 0;
  for (t3_object *a = p->bone[top]->parent; a && a != d->root && na < 64; a = a->parent) above[na++] = a;
  t3_mat4_identity(out);
  t3_mat4 l;
  for (int j = na - 1; j >= 0; j--) {
    t3_mat4_compose(&l, above[j]->position, above[j]->quaternion, above[j]->scale);
    t3_mat4_multiply(out, out, &l);
  }
  for (int j = c - 1; j >= 0; j--) {
    int b = chain[j];
    if (rest) t3_mat4_compose(&l, p->rest_pos[b], p->rest_rot[b], p->rest_scl[b]);
    else t3_mat4_compose(&l, p->bone[b]->position, p->bone[b]->quaternion, p->bone[b]->scale);
    t3_mat4_multiply(out, out, &l);
  }
}

/* ── public: Skeleton3D ────────────────────────────────────────────── */

int cw_skeleton3d_bone_count(cw_node n) {
  skel3d *d = sk_bone(n, -1);
  return d ? d->pose.count : 0;
}
cw_node cw_skeleton3d_model(cw_node n) {
  skel3d *d = sk_bone(n, -1);
  return d ? d->model_node : (cw_node){ 0 };
}
int cw_skeleton3d_find_bone(cw_node n, const char *name) {
  skel3d *d = sk_bone(n, -1);
  return d ? cw_pose_find(&d->pose, name) : -1;
}
const char *cw_skeleton3d_bone_name(cw_node n, int bone) {
  skel3d *d = sk_bone(n, bone);
  return d && bone >= 0 ? d->pose.name[bone] : NULL;
}
int cw_skeleton3d_bone_parent(cw_node n, int bone) {
  skel3d *d = sk_bone(n, bone);
  return d && bone >= 0 ? d->pose.parent[bone] : -1;
}
t3_object *cw_skeleton3d_bone_object(cw_node n, int bone) {
  skel3d *d = sk_bone(n, bone);
  return d && bone >= 0 ? d->pose.bone[bone] : NULL;
}
bool cw_skeleton3d_bone_pose(cw_node n, int bone, cw_bone_xform *out) {
  skel3d *d = sk_bone(n, bone);
  if (!d || bone < 0) return false;
  const t3_object *o = d->pose.bone[bone];
  if (out) *out = (cw_bone_xform){ o->position, o->quaternion, o->scale };
  return true;
}
void cw_skeleton3d_set_bone_pose(cw_node n, int bone, const t3_vec3 *position, const t3_quat *rotation, const t3_vec3 *scale) {
  skel3d *d = sk_bone(n, bone);
  if (!d || bone < 0) { cw_warn("cw_skeleton3d_set_bone_pose: bone %d is not a bone of this skeleton (%d bones)", bone, cw_skeleton3d_bone_count(n)); return; }
  t3_object *o = d->pose.bone[bone];
  if (position) o->position = *position;
  if (scale) o->scale = *scale;
  if (rotation) t3_object_set_quaternion(o, *rotation);
}
bool cw_skeleton3d_bone_rest(cw_node n, int bone, cw_bone_xform *out) {
  skel3d *d = sk_bone(n, bone);
  if (!d || bone < 0) return false;
  if (out) *out = (cw_bone_xform){ d->pose.rest_pos[bone], d->pose.rest_rot[bone], d->pose.rest_scl[bone] };
  return true;
}
void cw_skeleton3d_set_bone_rest(cw_node n, int bone, const cw_bone_xform *rest) {
  skel3d *d = sk_bone(n, bone);
  if (!d || bone < 0 || !rest) { cw_warn("cw_skeleton3d_set_bone_rest: bone %d is not a bone of this skeleton", bone); return; }
  d->pose.rest_pos[bone] = rest->position;
  d->pose.rest_rot[bone] = rest->rotation;
  d->pose.rest_scl[bone] = rest->scale;
}
static void rest_to_bone(skel3d *d, int b) {
  t3_object *o = d->pose.bone[b];
  o->position = d->pose.rest_pos[b];
  o->scale = d->pose.rest_scl[b];
  t3_object_set_quaternion(o, d->pose.rest_rot[b]);
}
void cw_skeleton3d_reset_bone_pose(cw_node n, int bone) {
  skel3d *d = sk_bone(n, bone);
  if (!d) return;
  if (bone >= 0) rest_to_bone(d, bone);
  else for (int b = 0; b < d->pose.count; b++) rest_to_bone(d, b);
}
bool cw_skeleton3d_bone_model_transform(cw_node n, int bone, t3_mat4 *out) {
  skel3d *d = sk_bone(n, bone);
  if (!d || bone < 0) return false;
  bone_model(d, bone, false, out);
  return true;
}
bool cw_skeleton3d_bone_rest_model_transform(cw_node n, int bone, t3_mat4 *out) {
  skel3d *d = sk_bone(n, bone);
  if (!d || bone < 0) return false;
  bone_model(d, bone, true, out);
  return true;
}
bool cw_skeleton3d_bone_global_transform(cw_node n, int bone, t3_mat4 *out) {
  skel3d *d = sk_bone(n, bone);
  if (!d || bone < 0) return false;
  t3_mat4 m;
  bone_model(d, bone, false, &m);
  const t3_mat4 *g = cw_node3d_global_transform(d->model_node);
  if (g) t3_mat4_multiply(out, g, &m);
  else *out = m;
  return true;
}
cw_pose *cw_skeleton3d_pose(cw_node n) {
  skel3d *d = sk_bone(n, -1);
  if (!d) return NULL;
  cw_pose_capture(&d->pose);
  return &d->pose;
}
uint32_t cw_skeleton3d_hash(cw_node n) {
  skel3d *d = sk_bone(n, -1);
  if (!d) return 0;
  uint32_t h = CW_HASH32_INIT;
  for (int i = 0; i < d->pose.count; i++) {
    const t3_object *o = d->pose.bone[i];
    h = cw_hash32(&o->position, sizeof o->position, h);
    h = cw_hash32(&o->quaternion, sizeof o->quaternion, h);
    h = cw_hash32(&o->scale, sizeof o->scale, h);
  }
  return h;
}
uint32_t cw_skeleton3d_generation(cw_node n) {
  skel3d *d = SK(n);
  return d ? d->gen : 0;
}

/* animation internals: where a holder's bones are (anim_internal.h) */
t3_object *cw__skeleton_bone_root(cw_node holder, cw_node *frame) {
  skel3d *d = SK(holder);
  if (d) {
    sk_bind(holder, d);
    if (d->bound) {
      if (frame) *frame = d->model_node;
      return d->root;
    }
    /* not bound yet: its own object, as any Node3D (nothing to bind there until the model arrives) */
  }
  if (frame) *frame = holder;
  return cw_node3d_object(holder);
}

/* ── BoneAttachment3D ──────────────────────────────────────────────── */

static cw_node attach_find_skel(cw_node n, const attach3d *a) {
  if (a->skeleton && a->skeleton[0]) {
    cw_node s = cw_node_get(n, a->skeleton);
    return cw_node_valid(s) && SK(s) ? s : (cw_node){ 0 };
  }
  for (cw_node p = cw_node_parent(n); !CW_HANDLE_IS_NULL(p); p = cw_node_parent(p))
    if (SK(p)) return p;
  return (cw_node){ 0 };
}

static void node_local(cw_node n, t3_mat4 *out) {
  const cw_node3d *d = cw_node_data(n, "Node3D");
  if (d) t3_mat4_compose(out, d->position, d->quaternion, d->scale);
  else t3_mat4_identity(out);
}

/* Place the attachment on its bone. direct: also write its three.c object (a
 * skeleton that bound after the nodes' pre_render already ran this frame). */
static void attach_update(cw_node n, attach3d *a, bool direct) {
  cw_node s = attach_find_skel(n, a);
  skel3d *d = cw_node_valid(s) ? SK(s) : NULL;
  if (!d || !d->bound) { a->bone_idx = -1; a->gen = 0; return; }
  if (a->skel.id != s.id || a->gen != d->gen || a->resolved_name != a->bone_name) {
    a->skel = s;
    a->gen = d->gen;
    a->resolved_name = a->bone_name;
    a->bone_idx = cw_pose_find(&d->pose, a->bone_name);
    if (a->bone_idx < 0 && a->bone_name && a->bone_name[0] && !a->warned) {
      a->warned = true;
      cw_warn("BoneAttachment3D '%s': '%s' is not a bone of '%s' (try: skeleton <node> bones)", cw_node_name(n), a->bone_name,
              cw_node_name(s));
    }
  }
  if (a->bone_idx < 0) return;
  t3_mat4 m;
  bone_model(d, a->bone_idx, false, &m);
  /* the Node3D chain from the model down to our parent; when the model is not above us, through world space */
  cw_node chain[64];
  int c = 0;
  bool under = false;
  for (cw_node p = cw_node_parent(n); !CW_HANDLE_IS_NULL(p); p = cw_node_parent(p)) {
    if (p.id == d->model_node.id) { under = true; break; }
    if (cw_node_is(p, "Node3D") && c < 64) chain[c++] = p;
  }
  t3_mat4 local = m;
  if (under) {
    if (c) {
      t3_mat4 rel, l, inv;
      t3_mat4_identity(&rel);
      for (int j = c - 1; j >= 0; j--) {
        node_local(chain[j], &l);
        t3_mat4_multiply(&rel, &rel, &l);
      }
      t3_mat4_invert(&inv, &rel);
      t3_mat4_multiply(&local, &inv, &m);
    }
  } else {
    t3_mat4 world, inv;
    const t3_mat4 *g = cw_node3d_global_transform(d->model_node);
    if (g) t3_mat4_multiply(&world, g, &m);
    else world = m;
    cw_node par = cw_node_parent(n);
    while (!CW_HANDLE_IS_NULL(par) && !cw_node_is(par, "Node3D")) par = cw_node_parent(par);
    const t3_mat4 *pg = CW_HANDLE_IS_NULL(par) ? NULL : cw_node3d_global_transform(par);
    if (pg) {
      t3_mat4_invert(&inv, pg);
      t3_mat4_multiply(&local, &inv, &world);
    } else local = world;
  }
  t3_vec3 p, sc;
  t3_quat q;
  t3_mat4_decompose(&local, &p, &q, &sc);
  cw_node3d_set_position(n, p);
  cw_node3d_set_quaternion(n, q);
  cw_node3d_set_scale(n, sc);
  cw_node3d_snapshot(n); /* it sits on the bone: never eased from where it was */
  if (direct) {
    t3_object *o = cw_node3d_object(n);
    if (o) {
      o->position = p;
      o->scale = sc;
      t3_object_set_quaternion(o, q);
    }
  }
}

int cw_bone_attachment3d_bone(cw_node n) {
  attach3d *a = cw_node_data_t(n, attach_t);
  return a ? a->bone_idx : -1;
}
cw_node cw_bone_attachment3d_skeleton(cw_node n) {
  attach3d *a = cw_node_data_t(n, attach_t);
  return a ? attach_find_skel(n, a) : (cw_node){ 0 };
}

/* ── the per-tree phases ───────────────────────────────────────────── */

static void skw_free(void *p) {
  skw *W = p;
  cw_array_free(&W->skels);
  cw_array_free(&W->attachments);
  cw_free(W);
}

/* drop freed nodes; bind what can bind. Returns true when a skeleton bound in this call. */
static bool bind_all(skw *W) {
  bool any = false;
  for (int i = 0; i < W->skels.count; i++) {
    cw_node n = W->skels.data[i];
    skel3d *d = SK(n);
    if (!d || !cw_node_tree(n)) { cw_array_remove_swap(&W->skels, i); i--; continue; }
    d->just_bound = false;
    sk_bind(n, d);
    any |= d->just_bound;
  }
  return any;
}

static void attach_all(skw *W, bool direct) {
  for (int i = 0; i < W->attachments.count; i++) {
    cw_node n = W->attachments.data[i];
    attach3d *a = cw_node_data_t(n, attach_t);
    if (!a || !cw_node_tree(n)) { cw_array_remove_swap(&W->attachments, i); i--; continue; }
    attach_update(n, a, direct);
  }
}

/* fixed, before the nodes: a model that arrived since the last tick binds before any animation writes it */
static void fixed_begin(cw_tree *t, double dt, void *u) {
  (void)dt, (void)u;
  bind_all(cw_tree_slot(t, "skeleton3d"));
}
/* fixed, after animation (0), tweens (100), physics sync (101) and the modifiers (150) */
static void fixed_end(cw_tree *t, double dt, void *u) {
  (void)dt, (void)u;
  skw *W = cw_tree_slot(t, "skeleton3d");
  for (int i = 0; i < W->skels.count; i++) {
    skel3d *d = SK(W->skels.data[i]);
    if (d && d->bound && d->show_rest_only && cw_node_can_process(W->skels.data[i]))
      for (int b = 0; b < d->pose.count; b++) rest_to_bone(d, b);
  }
  attach_all(W, false);
}
/* pre_render, before the nodes write their objects: the bones as they will be drawn */
static void render_begin(cw_tree *t, double alpha, void *u) {
  (void)alpha, (void)u;
  skw *W = cw_tree_slot(t, "skeleton3d");
  bind_all(W);
  attach_all(W, false);
}
/* pre_render, after the nodes: a model a MeshInstance3D created this frame */
static void render_end(cw_tree *t, double alpha, void *u) {
  (void)alpha, (void)u;
  skw *W = cw_tree_slot(t, "skeleton3d");
  if (bind_all(W)) attach_all(W, true);
}

static skw *world_of(cw_tree *t) {
  if (!t) return NULL;
  skw *W = cw_tree_slot(t, "skeleton3d");
  if (W) return W;
  W = cw_calloc(1, sizeof *W, CW_MEM_ANIM);
  cw_tree_set_slot(t, "skeleton3d", W, skw_free);
  cw_tree_add_hook(t, 0, -50, fixed_begin, NULL);
  cw_tree_add_hook(t, 0, 160, fixed_end, NULL);
  cw_tree_add_hook(t, 2, -10, render_begin, NULL);
  cw_tree_add_hook(t, 2, 10, render_end, NULL);
  return W;
}

static void list_add(void *arr, cw_node n) {
  CW_ARRAY(cw_node) *a = arr;
  for (int i = 0; i < a->count; i++)
    if (a->data[i].id == n.id) return;
  cw_array_push(a, n);
}

/* ── node types ────────────────────────────────────────────────────── */

static void sk_enter(cw_node n, void *data) {
  skw *W = world_of(cw_node_tree(n));
  if (W) list_add(&W->skels, n);
  sk_bind(n, data);
}
static void sk_exit(cw_node n, void *data) {
  (void)n;
  skel3d *d = data;
  sk_unbind(d);
  d->misses = 0;
}
static void sk_destroy(cw_node n, void *data) {
  (void)n;
  sk_unbind(data);
}
static void sk_prop_changed(cw_node n, void *data, const cw_prop *prop) {
  (void)n;
  skel3d *d = data;
  if (!strcmp(prop->name, "model")) {
    sk_unbind(d);
    d->misses = 0;
    d->warned = false;
  }
}
static void sk_count_get(cw_node n, void *data, cw_value *o) {
  (void)n;
  skel3d *d = data;
  *o = cw_val_int(d->bound ? d->pose.count : 0);
}
static void sk_bound_get(cw_node n, void *data, cw_value *o) {
  (void)n;
  *o = cw_val_bool(((skel3d *)data)->bound);
}
static const cw_prop sk_props[] = {
  CW_PROP_FIELD(skel3d, model, CW_T_STRING, CW_PROP_DEFAULT, "Node3D (path from this node) holding the skinned model; empty: this node when it holds the bones, else its parent."),
  CW_PROP_FIELD(skel3d, show_rest_only, CW_T_BOOL, CW_PROP_DEFAULT, "Hold every bone at its rest transform each tick, after animation and modifiers."),
  CW_PROP_ACCESSOR("bone_count", CW_T_INT, CW_PROP_READONLY, sk_count_get, NULL, "Bones in the skeleton (0 until the model's bones exist)."),
  CW_PROP_ACCESSOR("bound", CW_T_BOOL, CW_PROP_READONLY, sk_bound_get, NULL, "The model's bones were found."),
  CW_PROP_END,
};
static const char *const sk_signals[] = { "skeleton_bound", NULL };
static cw_node_type skel_type = {
  .name = "Skeleton3D", .parent = "Node3D", .data_size = sizeof(skel3d), .props = sk_props, .signals = sk_signals,
  .doc = "The bones of a skinned glTF model as a node: names, parents, rest and pose transforms to read and set. Skeleton modifiers and BoneAttachment3D nodes go under it.",
  .enter_tree = sk_enter, .exit_tree = sk_exit, .destroy = sk_destroy, .prop_changed = sk_prop_changed,
};

static void at_init(cw_node n, void *data) {
  (void)n;
  ((attach3d *)data)->bone_idx = -1;
}
static void at_enter(cw_node n, void *data) {
  skw *W = world_of(cw_node_tree(n));
  if (W) list_add(&W->attachments, n);
  attach3d *a = data;
  a->gen = 0;
  a->skel = (cw_node){ 0 };
  attach_update(n, a, false);
}
static void at_prop_changed(cw_node n, void *data, const cw_prop *prop) {
  (void)prop;
  attach3d *a = data;
  a->gen = 0;
  a->warned = false;
  if (cw_node_tree(n)) attach_update(n, a, false);
}
static const cw_prop at_props[] = {
  CW_PROP_FIELD(attach3d, bone_name, CW_T_STRING, CW_PROP_DEFAULT, "The bone this node follows."),
  CW_PROP_FIELD(attach3d, skeleton, CW_T_STRING, CW_PROP_DEFAULT, "Skeleton3D (path from this node); empty: the nearest Skeleton3D above."),
  { .name = "bone_idx", .type = CW_T_INT, .flags = CW_PROP_READONLY, .offset = offsetof(attach3d, bone_idx), .size = sizeof(int32_t),
    .doc = "Index of the bone followed; -1 while it is not found." },
  CW_PROP_END,
};
static cw_node_type attach_type = {
  .name = "BoneAttachment3D", .parent = "Node3D", .data_size = sizeof(attach3d), .props = at_props,
  .doc = "A Node3D that takes a bone's transform each tick and each frame: its children ride the bone (a sword in a hand, a hat on a head).",
  .init = at_init, .enter_tree = at_enter, .prop_changed = at_prop_changed,
};

/* ── debug channel ─────────────────────────────────────────────────── */

static void reply_xform(cw_reply *r, const char *key, const cw_bone_xform *x) {
  cw_reply_printf(r, "\"%s\":{\"position\":[%.9g,%.9g,%.9g],\"quaternion\":[%.9g,%.9g,%.9g,%.9g],\"scale\":[%.9g,%.9g,%.9g]}", key,
                  x->position.x, x->position.y, x->position.z, x->rotation.x, x->rotation.y, x->rotation.z, x->rotation.w,
                  x->scale.x, x->scale.y, x->scale.z);
}

static void cmd_skeleton(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) {
    cw_reply_error(r, "usage: skeleton <node> [bones | bone <name> | set <name> position|quaternion|scale <value> | rest [<name>] | hash]");
    return;
  }
  cw_node n = cw__anim_debug_node(argv[1], r, "Skeleton3D");
  if (!cw_node_valid(n)) return;
  skel3d *d = SK(n);
  const char *op = argc > 2 ? argv[2] : "";
  if (!d->bound) {
    if (op[0]) cw_reply_error(r, "'%s' is not bound: no bones under its model yet", argv[1]);
    else cw_reply_raw(r, "{\"bound\":false,\"bones\":0}");
    return;
  }
  if (!op[0] || !strcmp(op, "hash")) {
    char path[512];
    cw_node_path(d->model_node, cw_tree_root(cw_node_tree(n)), path, sizeof path);
    cw_reply_printf(r, "{\"bound\":true,\"bones\":%d,\"generation\":%u,\"hash\":%u,\"show_rest_only\":%s,\"model\":", d->pose.count,
                    d->gen, cw_skeleton3d_hash(n), d->show_rest_only ? "true" : "false");
    cw_reply_str(r, path);
    cw_reply_raw(r, "}");
    return;
  }
  if (!strcmp(op, "bones")) {
    cw_reply_raw(r, "{\"bones\":[");
    for (int i = 0; i < d->pose.count; i++) {
      cw_reply_raw(r, i ? ",{\"name\":" : "{\"name\":");
      cw_reply_str(r, d->pose.name[i] ? d->pose.name[i] : "");
      cw_reply_printf(r, ",\"parent\":%d}", d->pose.parent[i]);
    }
    cw_reply_raw(r, "]}");
    return;
  }
  if (!strcmp(op, "rest") && argc < 4) {
    cw_skeleton3d_reset_bone_pose(n, -1);
    cw_reply_printf(r, "{\"reset\":%d}", d->pose.count);
    return;
  }
  if (argc < 4) { cw_reply_error(r, "skeleton %s %s: name a bone (try: skeleton %s bones)", argv[1], op, argv[1]); return; }
  int b = cw_pose_find(&d->pose, argv[3]);
  if (b < 0) { cw_reply_error(r, "'%s' is not a bone of '%s' (try: skeleton %s bones)", argv[3], argv[1], argv[1]); return; }
  if (!strcmp(op, "rest")) {
    cw_skeleton3d_reset_bone_pose(n, b);
    cw_reply_printf(r, "{\"reset\":1,\"bone\":%d}", b);
    return;
  }
  if (!strcmp(op, "set")) {
    if (argc < 6) { cw_reply_error(r, "usage: skeleton <node> set <bone> position|quaternion|scale <value>, e.g. vec3(0, 1, 0) or quat(0, 0, 0, 1)"); return; }
    char buf[256];
    int k = 0;
    for (int i = 5; i < argc && k < (int)sizeof buf - 2; i++) k += snprintf(buf + k, sizeof buf - (size_t)k, "%s%s", i > 5 ? " " : "", argv[i]);
    bool isq = !strcmp(argv[4], "quaternion");
    cw_value v;
    if (!cw_value_parse(buf, isq ? CW_T_QUAT : CW_T_VEC3, &v)) { cw_reply_error(r, "cannot parse '%s' as a %s", buf, isq ? "quat(x, y, z, w)" : "vec3(x, y, z)"); return; }
    t3_vec3 v3 = { v.v[0], v.v[1], v.v[2] };
    t3_quat q = { v.v[0], v.v[1], v.v[2], v.v[3] };
    if (!strcmp(argv[4], "position")) cw_skeleton3d_set_bone_pose(n, b, &v3, NULL, NULL);
    else if (isq) cw_skeleton3d_set_bone_pose(n, b, NULL, &q, NULL);
    else if (!strcmp(argv[4], "scale")) cw_skeleton3d_set_bone_pose(n, b, NULL, NULL, &v3);
    else { cw_reply_error(r, "set position, quaternion or scale, not '%s'", argv[4]); return; }
  } else if (strcmp(op, "bone")) {
    cw_reply_error(r, "unknown skeleton request '%s' (bones, bone, set, rest, hash)", op);
    return;
  }
  cw_bone_xform pose = { 0 }, rest = { 0 };
  t3_mat4 g;
  t3_mat4_identity(&g);
  cw_skeleton3d_bone_pose(n, b, &pose);
  cw_skeleton3d_bone_rest(n, b, &rest);
  cw_skeleton3d_bone_global_transform(n, b, &g);
  cw_reply_printf(r, "{\"bone\":%d,\"name\":", b);
  cw_reply_str(r, d->pose.name[b]);
  cw_reply_printf(r, ",\"parent\":%d,", d->pose.parent[b]);
  reply_xform(r, "pose", &pose);
  cw_reply_raw(r, ",");
  reply_xform(r, "rest", &rest);
  cw_reply_printf(r, ",\"global_position\":[%.9g,%.9g,%.9g]}", g.e[12], g.e[13], g.e[14]);
}

void cw__register_skeleton_types(void) {
  cw_node_type_register(&skel_type);
  skel_t = &skel_type;
  cw_node_type_register(&attach_type);
  attach_t = &attach_type;
  cw_debug_command("skeleton", "skeleton <node> [bones | bone <name> | set <name> position|quaternion|scale <value> | rest [<name>] | hash]: a Skeleton3D's bones",
                   cmd_skeleton);
}
