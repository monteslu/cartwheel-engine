/* Box2D wrapper internals shared by world2d.c, query2d.c and mover2d.c. */
#ifndef CW_PHYSICS2D_INTERNAL_H
#define CW_PHYSICS2D_INTERNAL_H

#include "internal.h"

typedef struct cw__body2 {
  uint64_t id;
  void *user;
  t3_object *obj;
  cw_xform2 *xf;
  cw_xform2 prev, cur;
  int32_t moved_span; /* the interpolation span (cw__world2.span) that last set cur from a move event */
  bool interpolate, settled;
} cw__body2;

typedef struct cw__joint2 {
  uint64_t id;
  void *user;
  cw_joint_type type;
  bool breakable;
  b2BodyId mouse_body;
  cw_vec2 target;
} cw__joint2;

typedef struct cw__world2 {
  b2WorldId id;
  void *tasks; /* cw_box2d_tasks context, released with the world */
  int substeps;
  int32_t steps;
  int32_t span;  /* interpolation span: one per step, or one per tick once cw_world2d_tick_begin is called */
  bool by_tick;
  uint32_t hash;
  cw_pool bodies, joints;
  CW_ARRAY(cw_phys_event2d) events;
  CW_ARRAY(b2JointId) breaking;
  cw__layers layers;
  b2BodyId ground;
} cw__world2;

cw__world2 *cw__w2(cw_world2d w);
cw__body2 *cw__b2(cw_body2d b);
void cw__joint2_destroy(cw__world2 *w, b2JointId jid);

static inline b2Vec2 v2(cw_vec2 v) { return (b2Vec2){ v.x, v.y }; }
static inline cw_vec2 cwv(b2Vec2 v) { return (cw_vec2){ v.x, v.y }; }
static inline b2Rot r2(cw_rot2 r) { return (b2Rot){ r.c, r.s }; }
static inline cw_rot2 cwr(b2Rot r) { return (cw_rot2){ r.c, r.s }; }
static inline cw_xform2 x2(b2Transform t) { return (cw_xform2){ cwv(t.p), cwr(t.q) }; }
static inline b2Transform bx2(cw_xform2 t) { return (b2Transform){ v2(t.p), r2(t.q) }; }

#endif
