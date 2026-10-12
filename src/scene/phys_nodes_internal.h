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

#endif
