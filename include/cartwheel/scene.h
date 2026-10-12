/* cartwheel scene model: nodes, the scene tree, groups, signals, reflection.
 *
 * A node is a named element of a tree. Its TYPE (registered once) says what
 * data it carries, which properties it exposes by name (reflection), which
 * signals it can emit and which lifecycle callbacks it has. Node types
 * inherit: Node3D extends Node, MeshInstance3D extends Node3D. Users hold
 * nodes by generational handle (cw_node); a freed node's handle reads as
 * null, never as another node.
 *
 * Processing order is tree order (depth first, children in order), so a run
 * is deterministic. Adding or removing nodes while the tree is being
 * processed is queued and applied at the end of the current phase.
 *
 * Every property is reachable by name (cw_node_set / cw_node_get) with typed
 * values (cw_value). That one path serves the editor's inspector, the text
 * scene format, animation tracks, romdev's debug channel and script bindings.
 */
#ifndef CW_SCENE_H
#define CW_SCENE_H

#include "core.h"
#include "math.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

CW_HANDLE_TYPE(cw_node);
typedef struct cw_tree cw_tree;
typedef struct cw_node_type cw_node_type;

/* ── values (reflection) ───────────────────────────────────────────── */

typedef enum {
  CW_T_NONE, CW_T_BOOL, CW_T_INT, CW_T_FLOAT, CW_T_VEC2, CW_T_VEC3, CW_T_VEC4, CW_T_QUAT, CW_T_COLOR,
  CW_T_STRING, CW_T_NODE, CW_T_RES, CW_T_ENUM, CW_T_COUNT
} cw_type;

typedef struct cw_value {
  cw_type type;
  union {
    bool b;
    int64_t i;
    double f;
    float v[4]; /* vec2/3/4, quat (xyzw), color (rgba, linear) */
    const char *s; /* interned */
    cw_node node;
    uint64_t res;  /* resource uid (resource.h) */
  };
} cw_value;

/* Value constructors: build a cw_value of that type (no allocation). */
static inline cw_value cw_val_bool(bool b) { cw_value v = { CW_T_BOOL }; v.b = b; return v; }
/* 64-bit integer value */
static inline cw_value cw_val_int(int64_t i) { cw_value v = { CW_T_INT }; v.i = i; return v; }
/* double value; converts to int on set only when integral */
static inline cw_value cw_val_float(double f) { cw_value v = { CW_T_FLOAT }; v.f = f; return v; }
/* vec2 value (z, w zero) */
static inline cw_value cw_val_vec2(float x, float y) { cw_value v = { CW_T_VEC2 }; v.v[0] = x; v.v[1] = y; return v; }
/* vec3 value (w zero); converts to a color with alpha 1 */
static inline cw_value cw_val_vec3(float x, float y, float z) { cw_value v = { CW_T_VEC3 }; v.v[0] = x; v.v[1] = y; v.v[2] = z; return v; }
/* linear rgba color value */
static inline cw_value cw_val_color(float r, float g, float b, float a) { cw_value v = { CW_T_COLOR }; v.v[0] = r; v.v[1] = g; v.v[2] = b; v.v[3] = a; return v; }
/* string value; s is interned (copied into the intern table, caller keeps its buffer) */
static inline cw_value cw_val_string(const char *s) { cw_value v = { CW_T_STRING }; v.s = cw_intern(s); return v; }
/* An enum value by name; cw_node_set resolves it against the property's names. */
static inline cw_value cw_val_enum(const char *name) { return cw_val_string(name); }
const char *cw_type_name(cw_type t); /* static lower-case name ("vec3"); "?" for an out-of-range type */
/* Text form used by the scene format and the debug channel:
 * 3.5, true, vec3(1, 2, 3), color(1, 0.5, 0, 1), "text", node("Path/To"), res(0x1234).
 * Enum values format as their index; scene files and the debug channel write
 * the NAME (they know the property). To set an enum by name from code, pass a
 * string: cw_node_set(n, "primitive", cw_val_enum("SPHERE")). */
int cw_value_format(const cw_value *v, char *buf, int cap);
/* Parse the text form above into *out (zeroed first). Strings intern; node("path") resolves now, against the
 * current tree's root (null when not found or no tree). Scene files do not use this for node values: their
 * node("path") is relative to the scene root and resolved once the instance is built (scene_file.h).
 * expect only picks float over int for a bare number. false on unparseable text. */
bool cw_value_parse(const char *text, cw_type expect, cw_value *out);

/* ── properties ─────────────────────────────────────────────────────
 * A property is a named, typed field of a node type's data, or a
 * getter/setter pair. Descriptors are static tables, declared with the
 * CW_PROP_* macros next to the type's struct. */

typedef enum {
  CW_PROP_STORED = 1 << 0,   /* saved in scenes */
  CW_PROP_EDITOR = 1 << 1,   /* shown in the inspector */
  CW_PROP_READONLY = 1 << 2, /* set refused (computed values) */
  CW_PROP_ANIMATABLE = 1 << 3,
  CW_PROP_DEFAULT = CW_PROP_STORED | CW_PROP_EDITOR | CW_PROP_ANIMATABLE,
} cw_prop_flags;

typedef struct cw_prop {
  const char *name;
  cw_type type;
  uint32_t flags;
  /* field access: offset into the type's data (-1 when get/set are used) */
  int offset;
  int size;
  void (*get)(cw_node n, void *data, cw_value *out);
  void (*set)(cw_node n, void *data, const cw_value *in);
  /* hints for editors / validation */
  double min, max;
  const char *const *enum_names; /* CW_T_ENUM: NULL-terminated */
  const char *doc;
} cw_prop;

#define CW_PROP_FIELD(T, field, ty, fl, docstr) \
  { .name = #field, .type = ty, .flags = fl, .offset = (int)offsetof(T, field), .size = (int)sizeof(((T *)0)->field), .doc = docstr }
#define CW_PROP_FIELD_RANGE(T, field, ty, lo, hi, docstr) \
  { .name = #field, .type = ty, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(T, field), \
    .size = (int)sizeof(((T *)0)->field), .min = lo, .max = hi, .doc = docstr }
#define CW_PROP_ACCESSOR(nm, ty, fl, getter, setter, docstr) \
  { .name = nm, .type = ty, .flags = fl, .offset = -1, .get = getter, .set = setter, .doc = docstr }
#define CW_PROP_END { 0 }

/* ── node types ─────────────────────────────────────────────────────── */

struct cw_node_type {
  const char *name;           /* "MeshInstance3D" */
  const char *parent;         /* "Node3D"; NULL only for "Node" */
  size_t data_size;           /* this type's own data (parents' data is separate) */
  const cw_prop *props;       /* CW_PROP_END terminated, may be NULL */
  const char *const *signals; /* NULL-terminated signal names this type emits */
  const char *doc;
  /* Lifecycle. Every callback gets the node and THIS type's data. Parent
   * types' callbacks run first for init/enter/ready and last for exit/destroy. */
  void (*init)(cw_node n, void *data);       /* after allocation, before entering a tree */
  void (*destroy)(cw_node n, void *data);
  void (*enter_tree)(cw_node n, void *data);
  void (*ready)(cw_node n, void *data);      /* once, after its children are ready */
  void (*exit_tree)(cw_node n, void *data);
  void (*fixed_update)(cw_node n, void *data, double dt);
  void (*update)(cw_node n, void *data, double dt);
  void (*pre_render)(cw_node n, void *data, double alpha);
  /* Notifications. prop_changed: after cw_node_set wrote a property this
   * type declares (called on the declaring type only). children_changed:
   * after a child was added, removed or moved (every type in the chain that
   * has one, base first; the UI re-lays out containers on it). */
  void (*prop_changed)(cw_node n, void *data, const cw_prop *prop);
  void (*children_changed)(cw_node n, void *data);
  /* resolved at registration */
  const cw_node_type *base;
  int depth;     /* Node = 0 */
  int index;     /* registry index */
};

/* Register a type. The struct must outlive the program (static). Its parent
 * must already be registered. Engine types are registered by
 * cw_scene_register_builtin_types(). Registering a name again (hot reload:
 * new code registers its types again) takes the new behaviour (callbacks,
 * properties, signals, docs) and keeps the data layout; a different
 * data_size or parent is refused with an error. The macro records the
 * source file and line of the registration call (reflection metadata the
 * editor uses to jump from a node to the C code that drives it). */
void cw_node_type_register(cw_node_type *t);
/* what the macro calls; file/line may be NULL/0 */
void cw_node_type_register_at(cw_node_type *t, const char *file, int line);
#define cw_node_type_register(t) cw_node_type_register_at((t), __FILE__, __LINE__)
/* Where a type was registered: the source file as the compiler saw it (an
 * absolute path in CMake builds; NULL when unknown) and *line (may be NULL).
 * After a hot reload: the newest registration. */
const char *cw_node_type_source(const cw_node_type *t, int *line);
const cw_node_type *cw_node_type_find(const char *name); /* NULL when not registered; O(1) */
int cw_node_type_count(void); /* registered types, for iterating with cw_node_type_at */
const cw_node_type *cw_node_type_at(int i); /* registry order; NULL when out of range */
/* t is the named type or derives from it; walks the base chain */
bool cw_node_type_is(const cw_node_type *t, const char *ancestor_or_self);
/* Plugins (cartwheel/plugin.h): withdraw a registered type (a disabled
 * plugin's) so cw_node_type_find and cw_node_new no longer see it, the
 * way a build without the plugin does not; cw_node_type_at still lists it
 * (cw_node_type_withdrawn says so). Refused (false) while nodes of the type,
 * or of a type deriving from it, exist. Registering it again restores it. */
bool cw_node_type_withdraw(const char *name);
bool cw_node_type_withdrawn(const cw_node_type *t); /* true while t is withdrawn; false for NULL or unknown */
/* Live nodes whose type is t or derives from it (first: one of them, when
 * not NULL; null when none). O(nodes). */
int cw_node_type_live_count(const cw_node_type *t, cw_node *first);

/* ── nodes ──────────────────────────────────────────────────────────── */

typedef enum {
  CW_PAUSE_INHERIT, CW_PAUSE_PAUSABLE, CW_PAUSE_WHEN_PAUSED, CW_PAUSE_ALWAYS, CW_PAUSE_DISABLED
} cw_pause_mode;

/* Create a node of a registered type, not yet in any tree. name may be NULL
 * (the type name is used). */
cw_node cw_node_new(const char *type, const char *name);
/* Free a node and its whole subtree (deferred while the tree is processing). */
void cw_node_free(cw_node n);
bool cw_node_valid(cw_node n); /* false for a null, freed or never-allocated handle */

const cw_node_type *cw_node_get_type(cw_node n); /* NULL for an invalid node */
bool cw_node_is(cw_node n, const char *type); /* node is of the type or derives from it; false for an invalid node */
/* This node's data for one of its types (itself or an ancestor type); NULL
 * when the node is not of that type. */
void *cw_node_data(cw_node n, const char *type);
/* Same, by type pointer (no lookup): the fast path inside type code */
void *cw_node_data_t(cw_node n, const cw_node_type *type);

const char *cw_node_name(cw_node n); /* interned, lives for the program; NULL for an invalid node */
void cw_node_set_name(cw_node n, const char *name); /* siblings get unique names ("Enemy2") */
uint64_t cw_node_uid(cw_node n); /* stable id saved in scenes; 0 for an invalid node */
void cw_node_set_uid(cw_node n, uint64_t uid); /* re-keys the tree's uid index; uid 0 is ignored */
/* O(1) lookup among nodes in the tree; null handle when absent */
cw_node cw_node_by_uid(cw_tree *tree, uint64_t uid);

/* hierarchy */
void cw_node_add_child(cw_node parent, cw_node child); /* appends; child must have no parent */
/* inserts at index (out of range appends); child must have no parent */
void cw_node_add_child_at(cw_node parent, cw_node child, int index);
void cw_node_remove_child(cw_node parent, cw_node child); /* detaches; does not free */
/* detach from the current parent (if any) and append to new_parent; deferred while processing */
void cw_node_reparent(cw_node n, cw_node new_parent);
/* reorder among siblings; to_index out of range moves to the end */
void cw_node_move_child(cw_node parent, cw_node child, int to_index);
cw_node cw_node_parent(cw_node n); /* null handle for the root, a detached node or an invalid one */
int cw_node_child_count(cw_node n); /* 0 for an invalid node */
cw_node cw_node_child(cw_node n, int index); /* null handle when index is out of range */
int cw_node_index(cw_node n); /* among siblings */
/* "Arm/Hand", "../Sibling", "/root/Level/Player" (absolute from the tree root),
 * "%UniqueName" (scene-unique name) */
cw_node cw_node_get(cw_node from, const char *path);
/* path from `from` to n ("" when equal); absolute when from is null */
int cw_node_path(cw_node n, cw_node from, char *buf, int cap);
cw_tree *cw_node_tree(cw_node n); /* NULL when not in a tree */
bool cw_node_in_tree(cw_node n); /* same as cw_node_tree(n) != NULL */
/* depth-first visit (pre-order); return false from fn to skip a subtree */
void cw_node_walk(cw_node root, bool (*fn)(cw_node n, void *user), void *user);

/* processing */
void cw_node_set_process(cw_node n, bool fixed, bool frame); /* default: on when the type has the callback */
void cw_node_set_pause_mode(cw_node n, cw_pause_mode mode); /* default CW_PAUSE_INHERIT */
/* Whether n's fixed_update / update run now: its effective pause mode
 * (INHERIT walks up to the first explicit one; none = PAUSABLE) against the
 * tree's paused flag. false when n is not in a tree. Cost: O(depth). */
bool cw_node_can_process(cw_node n);
void cw_node_set_user(cw_node n, void *user);  /* game data (not reflected) */
void *cw_node_user(cw_node n); /* NULL until set */

/* groups */
/* groups are interned names; a node can be in many */
void cw_node_add_group(cw_node n, const char *group);
void cw_node_remove_group(cw_node n, const char *group); /* no-op when not a member */
bool cw_node_in_group(cw_node n, const char *group); /* O(groups on the node) */
/* nodes of a group in tree order, frame-arena array */
int cw_tree_group(cw_tree *t, const char *group, cw_node **out);

/* properties by name: "position", "material/color" style paths reach into
 * sub-objects only when a type provides them; returns false (and logs why)
 * for an unknown name or a type mismatch that cannot convert */
bool cw_node_set(cw_node n, const char *prop, cw_value v);
/* false (no log) for an unknown name or invalid node */
bool cw_node_get_prop(cw_node n, const char *prop, cw_value *out);
/* searches the type chain, most derived first; *owner (may be NULL) gets the declaring type */
const cw_prop *cw_node_find_prop(cw_node n, const char *prop, const cw_node_type **owner);
/* every property of the node's type chain, base type first */
int cw_node_props(cw_node n, const cw_prop **out_props, const cw_node_type **out_owners, int cap);

/* ── signals ─────────────────────────────────────────────────────────
 * A signal is a named event a node emits; connections call a C function.
 * Deferred connections are queued and run at the end of the current
 * phase in emission order. Arguments are cw_values. */
typedef void (*cw_signal_fn)(cw_node emitter, const cw_value *args, int argc, void *user);
typedef enum { CW_CONNECT_DEFAULT = 0, CW_CONNECT_DEFERRED = 1, CW_CONNECT_ONESHOT = 2 } cw_connect_flags;
/* target: the node the connection belongs to (it is dropped when either node
 * is freed); may be null. method: name saved in scene files (scripts and the
 * editor resolve it); may be NULL for code-only connections. */
uint64_t cw_connect(cw_node emitter, const char *signal, cw_node target, const char *method, cw_signal_fn fn,
                    void *user, uint32_t flags);
void cw_disconnect(uint64_t connection); /* by the id cw_connect returned; stale or 0 ids are ignored */
/* Call every connection of the signal now (deferred ones are queued, at most 8 args kept). No-op for an invalid
 * emitter. */
void cw_emit(cw_node emitter, const char *signal, const cw_value *args, int argc);
/* Resolves "method" names of connections loaded from scene files. */
typedef cw_signal_fn (*cw_method_resolver)(cw_node target, const char *method, void **user_out);
void cw_set_method_resolver(cw_method_resolver r); /* global; NULL clears it */
/* A node's connections in the order they were made: outgoing (emitter = n)
 * or incoming (target = n). Returns the total count (fills up to cap; out
 * may be NULL to count). method is NULL for code-only connections (scene
 * files save only named ones). Editors list and rebuild connections with
 * this; signal and method are interned. */
typedef struct cw_connection_info {
  uint64_t id;
  cw_node emitter, target;
  const char *signal, *method;
  uint32_t flags; /* cw_connect_flags */
} cw_connection_info;
/* see the block above cw_connection_info */
int cw_node_connections(cw_node n, bool incoming, cw_connection_info *out, int cap);

/* ── methods ─────────────────────────────────────────────────────────
 * A method is a named C function a node type exposes BY NAME, so the
 * editor and agents can invoke it (debug channel: call <node> <method>
 * [args...]). args is the signature as type names,
 * comma separated ("float,vec2"; NULL or "" = none; "..." = unchecked);
 * the debug channel parses and type-checks arguments against it. Return
 * false for a call that failed (bad arguments); write ret for a result. A
 * method registered on a type is found on every type deriving from it.
 * cw_node_call falls back to the method resolver (cw_set_method_resolver)
 * for names no type registered: the same names scene connections use. */
typedef bool (*cw_method_fn)(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user);
typedef struct cw_method {
  const char *type, *name; /* interned */
  const char *args;
  cw_method_fn fn;
  void *user;
  const char *doc;
} cw_method;
/* Register (or replace: hot reload) a method on a type and its descendants. */
void cw_node_type_add_method(const char *type, const char *name, const char *args, cw_method_fn fn, void *user,
                             const char *doc);
/* every method a node answers, its own type's first, then each base type's */
int cw_node_methods(cw_node n, const cw_method **out, int cap);
const cw_method *cw_node_find_method(cw_node n, const char *name); /* the most derived type's first; NULL = none */
/* false when nothing answers the name (or the method returned false) */
bool cw_node_call(cw_node n, const char *method, const cw_value *args, int argc, cw_value *ret);

/* ── the tree ────────────────────────────────────────────────────────── */

/* Create an empty tree with a root Node named "root"; registers the builtin types; becomes current if none is. */
cw_tree *cw_tree_new(void);
void cw_tree_free(cw_tree *t); /* frees every node, slot (via free_fn) and hook list; clears current if it was t */
/* Every cw_tree_* call takes a NULL tree (no current tree yet): it answers
 * like an empty tree (null root, 0, false, NULL rng) and changes nothing. */
cw_node cw_tree_root(cw_tree *t);
/* Run one phase over every processing node in tree order. */
void cw_tree_fixed_update(cw_tree *t, double dt);
void cw_tree_update(cw_tree *t, double dt); /* per rendered frame; dt in seconds */
/* alpha in [0, 1] between the last two fixed ticks; ignores pause */
void cw_tree_pre_render(cw_tree *t, double alpha);
void cw_tree_set_paused(cw_tree *t, bool paused); /* nodes run per their pause mode; fixed ticks stop counting */
bool cw_tree_paused(cw_tree *t); /* current paused flag */
int cw_tree_node_count(cw_tree *t); /* nodes currently in the tree, root included */
uint64_t cw_tree_tick(cw_tree *t); /* fixed ticks run while unpaused since creation */
/* deterministic per-tree random numbers (seeded from cw_host_seed or set) */
cw_rng *cw_tree_rng(cw_tree *t);
void cw_tree_seed(cw_tree *t, uint64_t seed); /* reseeds cw_tree_rng(t) */
/* Per-tree subsystem slots (physics worlds, render scene, audio listener):
 * modules store their per-tree state here by a key they own. */
void *cw_tree_slot(cw_tree *t, const char *key);
/* replaces the value (the old one is freed by its free_fn); the tree frees it on cw_tree_free */
void cw_tree_set_slot(cw_tree *t, const char *key, void *value, void (*free_fn)(void *));
/* Hooks a module runs once per phase (physics steps before nodes' fixed_update
 * sees results, etc.). Lower order runs first; nodes run at order 0. */
typedef void (*cw_tree_hook)(cw_tree *t, double dt, void *user);
/* Stable by order. Hooks cannot be removed; they live as long as the tree. */
void cw_tree_add_hook(cw_tree *t, int phase /* 0 fixed, 1 update, 2 pre_render */, int order, cw_tree_hook fn, void *user);

/* The one current tree (scene runner); modules default to it. */
cw_tree *cw_current_tree(void);
void cw_set_current_tree(cw_tree *t); /* NULL clears it; the tree is not owned */

/* Register Node, Node3D, Node2D and the other engine node types. Idempotent. */
void cw_scene_register_builtin_types(void);

/* ── Node3D (transform in 3D; owns an optional three.c object) ─────────── */

typedef struct cw_node3d {
  t3_vec3 position;
  t3_vec3 rotation; /* euler XYZ, radians (editor convenience; kept in sync with quaternion) */
  t3_quat quaternion;
  t3_vec3 scale;
  bool visible;
  /* engine-owned */
  t3_object *object;   /* the three.c object this node drives (a t3_group by default) */
  bool interpolate;    /* render between the last two fixed ticks (physics bodies set it) */
  t3_vec3 prev_position;
  t3_quat prev_quaternion;
  t3_mat4 world;       /* last computed world matrix */
} cw_node3d;

t3_object *cw_node3d_object(cw_node n); /* owned by the node; NULL when n is not a Node3D */
void cw_node3d_set_object(cw_node n, t3_object *obj); /* replaces the node's three.c object (retained) */
void cw_node3d_set_position(cw_node n, t3_vec3 p); /* local, parent space; no-op unless Node3D */
void cw_node3d_set_rotation(cw_node n, t3_vec3 euler_xyz); /* radians; also updates the quaternion */
void cw_node3d_set_quaternion(cw_node n, t3_quat q); /* also updates the euler rotation */
void cw_node3d_set_scale(cw_node n, t3_vec3 s); /* local scale */
t3_vec3 cw_node3d_position(cw_node n); /* local; zero when not a Node3D */
t3_quat cw_node3d_quaternion(cw_node n); /* local; identity when not a Node3D */
t3_vec3 cw_node3d_global_position(cw_node n); /* world translation; zero when not a Node3D */
/* Recomputed on each call through Node3D ancestors (O(depth)); points into the node's data, valid until it is
 * freed. NULL when not a Node3D. */
const t3_mat4 *cw_node3d_global_transform(cw_node n);
/* Point -Z at target (world space; up in world space too) by setting the
 * local quaternion that gives that world rotation under the current
 * ancestors (any rotation; a non-uniformly scaled ancestor skews it). */
void cw_node3d_look_at(cw_node n, t3_vec3 target, t3_vec3 up);
/* Record the current transform as the interpolation start (called by whoever
 * moves the node in fixed_update: physics sync, character code). */
void cw_node3d_snapshot(cw_node n);

/* ── Node2D ──────────────────────────────────────────────────────────── */

typedef struct cw_node2d {
  cw_vec2 position;
  float rotation; /* radians */
  cw_vec2 scale;
  float skew;
  int z_index;
  bool z_relative;
  bool visible;
  cw_vec2 prev_position;
  float prev_rotation;
  bool interpolate;
  float world[6]; /* affine: a b c d tx ty */
  float modulate[4];      /* rgba multiplied into this node and its children (inherited) */
  float self_modulate[4]; /* rgba multiplied into this node only */
} cw_node2d;

void cw_node2d_set_position(cw_node n, cw_vec2 p); /* local, parent space; no-op unless Node2D */
cw_vec2 cw_node2d_position(cw_node n); /* local; zero when not a Node2D */
cw_vec2 cw_node2d_global_position(cw_node n); /* translation of the global transform */
/* Composed from Node2D ancestors up to the nearest CanvasLayer (which
 * starts a new canvas). Plain Node ancestors pass the transform through. */
const float *cw_node2d_global_transform(cw_node n); /* 6 floats */
void cw_node2d_set_rotation(cw_node n, float radians); /* local */
float cw_node2d_rotation(cw_node n); /* local radians; 0 when not a Node2D */
/* Set the position and rotation so the node's GLOBAL transform lands there
 * (the parent's global transform is inverted; scale is kept). */
void cw_node2d_set_global(cw_node n, cw_vec2 position, float rotation);
float cw_node2d_global_rotation(cw_node n); /* radians, atan2 of the global transform */
void cw_node2d_snapshot(cw_node n); /* record position/rotation as the interpolation start (see cw_node3d_snapshot) */

#ifdef __cplusplus
}
#endif
#endif
