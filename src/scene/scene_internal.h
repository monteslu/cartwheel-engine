#ifndef CW_SCENE_INTERNAL_H
#define CW_SCENE_INTERNAL_H

#include "cartwheel/scene.h"

#define CW__MAX_TYPE_DEPTH 12

typedef struct node_rec {
  const cw_node_type *type;
  uint8_t *data;
  int data_off[CW__MAX_TYPE_DEPTH];
  const char *name; /* interned */
  uint64_t uid;
  cw_node parent;
  CW_ARRAY(cw_node) children;
  cw_map child_names;   /* interned child name -> child id (sibling names are unique) */
  cw_map name_suffix;   /* interned base name -> next numeric suffix to try */
  CW_ARRAY(const char *) groups;
  cw_tree *tree;
  cw_pause_mode pause;
  bool proc_fixed, proc_frame, ready_done, queued_free, freeing;
  void *user;
  CW_ARRAY(uint64_t) conns; /* connections where this node is emitter or target */
  uint64_t scene_source;    /* resource uid of the scene this node was instanced from (0 = none) */
  bool scene_root;          /* root of an instanced scene */
} node_rec;

enum { CW__OP_ADD, CW__OP_REMOVE, CW__OP_FREE, CW__OP_REPARENT, CW__OP_MOVE };
typedef struct { int op; cw_node a, b; int index; } cw__deferred_op;

typedef struct { void *value; void (*free_fn)(void *); } tree_slot;
typedef struct { int order; cw_tree_hook fn; void *user; } tree_hook;
typedef struct { CW_ARRAY(cw_node) nodes; uint64_t version; bool dirty; } group_cache;

typedef struct {
  uint64_t conn;
  cw_node emitter;
  int argc;
  cw_value args[8];
} cw__queued_signal;

/* the change log (node.c): structural changes and debug-channel edits */
enum { CW__CH_ADDED, CW__CH_REMOVED, CW__CH_RENAMED, CW__CH_MOVED, CW__CH_SET, CW__CH_CALLED };
typedef struct cw__change {
  uint64_t rev, uid, parent;
  int32_t index; /* ADDED / MOVED: index among the siblings */
  uint8_t kind;
  const char *name, *type, *prop; /* interned */
} cw__change;
#define CW__CHANGE_RING 8192
typedef struct cw__changes {
  cw__change ring[CW__CHANGE_RING];
  uint64_t rev; /* the next revision; a client that saw rev - 1 is current */
} cw__changes;

struct cw_tree {
  cw_node root;
  cw_map by_uid;
  cw_map slots;
  cw_map groups;
  CW_ARRAY(cw_node) order;
  bool order_dirty;
  uint64_t structure_version; /* bumped on any change of membership or order */
  bool processing;
  bool paused;
  int node_count;
  uint64_t tick;
  cw_rng rng;
  CW_ARRAY(cw__deferred_op) deferred_ops;
  CW_ARRAY(cw__queued_signal) deferred_signals;
  CW_ARRAY(tree_hook) hooks[3];
  cw__changes *changes; /* NULL until a debug client asks */
};

node_rec *cw__node_rec(cw_node n);
/* The node whose subtree is leaving the tree right now (exit callbacks of its
 * descendants can skip work their ancestor's exit makes redundant). */
extern cw_node cw__exit_root;
/* true when n has an ancestor of type t inside the subtree that is exiting */
bool cw__exiting_with_ancestor(cw_node n, const cw_node_type *t);
/* true while n's parent node is being freed along with n */
bool cw__parent_freeing(cw_node n);
void cw__tree_apply_deferred(cw_tree *t);
void cw__tree_group_dirty(cw_tree *t, const char *g);
void cw__tree_order(cw_tree *t);
bool cw__find_by_name(cw_node n, const char *interned, cw_node *out);
void cw__signals_node_freed(cw_node n);
void cw__signals_flush(cw_tree *t);
void cw__register_node3d_types(void);
void cw__register_node2d_types(void);
/* 2D node layer (src/scene/nodes2d.c, canvas.c): the CanvasLayer type starts
 * a new canvas (Node2D global transforms stop there) */
extern const cw_node_type *cw__canvas_layer_t;
/* physics node types (src/scene/phys2d_nodes.c, phys3d_nodes.c) */
void cw__register_physics2d_types(void);
void cw__register_physics3d_types(void);
/* MeshInstance3D's three.c mesh, a child of the node's object (built on
 * demand, CPU side: no GL needed), for collision shapes made from a mesh;
 * NULL when the node has none */
t3_mesh *cw__mesh3d_t3(cw_node n);
void cw__scene_register_debug_commands(void);
struct cw_packed_scene;
void cw__scene_cache_put(const char *path, struct cw_packed_scene *ps); /* scene_file.c */
cw__changes *cw__changes_enable(cw_tree *t);
void cw__change_note(cw_tree *t, int kind, cw_node n, int index, const char *prop);
void cw__phys_register_debug_commands(void);
void cw__register_anim_types(void); /* src/anim: AnimationPlayer, AnimationTree, [animation] sections */
/* reflect.c: read / write one property by descriptor (no lookup, no coercion) */
void cw__prop_read(cw_node n, const cw_node_type *owner, const cw_prop *p, cw_value *out);
void cw__prop_write(cw_node n, const cw_node_type *owner, const cw_prop *p, const cw_value *v);
/* physics nodes under tree snapshots (cartwheel/net.h): a character's mover
 * state + outputs as private state, and every body put back at its node's
 * pose after a restore */
#define CW__CHAR_STATE_NOHASH 8 /* the floor body handle at the end: process-local */
int cw__char2d_state_save(cw_node n, void *data, uint8_t *out, int cap);
void cw__char2d_state_load(cw_node n, void *data, const uint8_t *in, int len);
int cw__char3d_state_save(cw_node n, void *data, uint8_t *out, int cap);
void cw__char3d_state_load(cw_node n, void *data, const uint8_t *in, int len);
void cw__phys2d_state_restored(cw_tree *t);
/* The 2D physics layer's share of a tree snapshot (net/state.c): Box2D's own image of the world (v3.2 snapshots)
 * and the node layer's handles into it. save returns a cw_alloc'd buffer (NULL: no 2D world), hash the world's
 * Box2D state hash (host-stable); load restores it and reports whether it did (false: the old pose-push path). */
uint8_t *cw__phys2d_snapshot_save(cw_tree *t, int *len, uint64_t *hash);
bool cw__phys2d_snapshot_load(cw_tree *t, const uint8_t *img, int len);
void cw__phys3d_state_restored(cw_tree *t);

#endif
