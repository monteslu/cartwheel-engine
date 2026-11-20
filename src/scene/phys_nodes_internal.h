/* Shared internals of the 2D and 3D physics nodes. */
#ifndef CW_PHYS_NODES_INTERNAL_H
#define CW_PHYS_NODES_INTERNAL_H

#include "scene_internal.h"
#include "cartwheel/physics_nodes.h"

/* a signal produced by a physics step, delivered after the step */
typedef struct cw__psig {
  cw_node emitter;
  const char *signal; /* interned */
  cw_node arg;
  float f;
  int argc; /* 0, 1 (node) or 2 (node, float) */
} cw__psig;
typedef CW_ARRAY(cw__psig) cw__psig_list;

/* Emit every queued signal in order with the tree marked as processing, so
 * frees/adds from handlers wait until all are delivered; then apply them. */
void cw__psig_deliver(cw_tree *t, cw__psig_list *l);
static inline void cw__psig_push(cw__psig_list *l, cw_node emitter, const char *sig, cw_node arg, float f, int argc) {
  cw__psig s = { emitter, sig, arg, f, argc };
  cw_array_push(l, s);
}

/* overlap pair counting (several shapes per pair): returns the new count */
int cw__pair_add(cw_map *m, uint64_t other, int delta);

/* The other nodes of an overlap map in the order they came in (oldest first): want 0 = not of type area_t
 * (bodies), 1 = of type area_t (areas). Returns how many there are; fills out up to cap (out may be NULL). */
int cw__pairs_list(const cw_map *pairs, const cw_node_type *area_t, int want, cw_node *out, int cap);
/* the same list as node names, comma separated, interned (the debug channel's view) */
const char *cw__pairs_names(const cw_map *pairs, const cw_node_type *area_t, int want);

/* layer name helpers against the project's names */
int cw__layer_index(const char *const names[CW_PHYS_LAYERS], const char *name);
uint32_t cw__layer_bits(const char *const names[CW_PHYS_LAYERS], const char *list);
void cw__layer_bits_text(const char *const names[CW_PHYS_LAYERS], uint32_t bits, char *out, int cap);

/* (node handles are 64-bit and user pointers are 32-bit in carts: nodes are
 * found through per-world maps keyed by shape / body / joint id, never by
 * packing a handle into a library user pointer) */

/* the signal names, interned once */
extern const char *cw__s_body_entered, *cw__s_body_exited, *cw__s_area_entered, *cw__s_area_exited, *cw__s_body_hit,
  *cw__s_broken;
void cw__phys_signals_init(void);

/* ── bodies whose colliders come from code instead of CollisionShape2D children (TileCollision2D) ─────────
 * The 2D physics layer owns the body; the builder adds and removes chains through these, so events, snapshots
 * and the node's layers and material work as for any body. */
#include "cartwheel/physics2d.h"
typedef struct cw__body_ctx {
  cw_node node;
  cw_body2d body;
  cw_shape2d_def def; /* the node's material, layer and mask */
  float ppm;
  void *world, *object; /* the physics layer's own */
} cw__body_ctx;
/* points in the body's frame, metres, y up (cw_chain2d_create's contract); returns the chain id (0 = failed) */
uint64_t cw__phys2d_ctx_chain(cw__body_ctx *x, const cw_shape2d_def *def, const cw_vec2 *points, int count, bool loop, int *segments);
void cw__phys2d_ctx_chain_destroy(cw__body_ctx *x, uint64_t chain);
/* build or update the node's colliders now (what the tick does before the step) */
void cw__phys2d_sync_shapes(cw_node n);

/* tile_collision2d.c */
extern const cw_node_type *cw__tilecol_t;
void cw__register_tile_collision_type(void);
uint32_t cw__tilecol_hash(cw_node n, uint32_t h); /* a change rebuilds every chunk */
void cw__tilecol_build(cw__body_ctx *x);          /* everything; the body's old chains are destroyed already */
void cw__tilecol_update(cw__body_ctx *x);         /* the chunks of the cells edited since the last build */
/* after a world image came back: do the chains the node remembers match the body's (body_chains of them)? */
bool cw__tilecol_restored(cw_node n, int body_chains);

#endif
