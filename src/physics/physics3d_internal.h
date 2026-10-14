/* Box3D wrapper internals shared by world3d.c, query3d.c and mover3d.c. */
#ifndef CW_PHYSICS3D_INTERNAL_H
#define CW_PHYSICS3D_INTERNAL_H

#include "internal.h"

typedef struct cw__body3 {
  uint64_t id;
  void *user;
  t3_object *obj;     /* sync target, or */
  cw_xform3 *xf;      /* plain transform target */
  cw_xform3 prev, cur;
  int32_t moved_span; /* the interpolation span (cw__world3.span) that last set cur from a move event */
  bool interpolate, settled;
} cw__body3;

typedef struct cw__joint3 {
  uint64_t id;
  void *user;
  cw_joint_type type;
  bool breakable;
  b3BodyId mouse_body; /* MOUSE: the kinematic body pulled to target */
  t3_vec3 target;
} cw__joint3;

typedef struct cw__world3 {
  b3WorldId id;
  void *tasks; /* cw_box3d_tasks context, released with the world */
  int substeps;
  int32_t steps;
  int32_t span;  /* interpolation span: one per step, or one per tick once cw_world3d_tick_begin is called */
  bool by_tick;
  uint32_t hash;
  cw_pool bodies, joints;
  CW_ARRAY(cw_phys_event3d) events;
  CW_ARRAY(b3JointId) breaking;
  cw__layers layers;
  b3BodyId ground; /* static body for joints to "the world" */
} cw__world3;

cw__world3 *cw__w3(cw_world3d w);
cw__body3 *cw__b3(cw_body3d b);
void cw__joint3_destroy(cw__world3 *w, b3JointId jid);
b3Quat cw__quat_between(b3Vec3 a, b3Vec3 b);

static inline b3Vec3 v3(t3_vec3 v) { return (b3Vec3){ v.x, v.y, v.z }; }
static inline t3_vec3 t3v(b3Vec3 v) { return (t3_vec3){ v.x, v.y, v.z }; }
static inline b3Pos pos3(t3_vec3 v) { return (b3Pos){ v.x, v.y, v.z }; }
static inline t3_vec3 t3p(b3Pos p) { return (t3_vec3){ (float)p.x, (float)p.y, (float)p.z }; }
static inline b3Quat q3(t3_quat q) { return (b3Quat){ { q.x, q.y, q.z }, q.w }; }
static inline t3_quat t3q(b3Quat q) { return (t3_quat){ q.v.x, q.v.y, q.v.z, q.s }; }
static inline cw_xform3 x3(b3WorldTransform t) { return (cw_xform3){ t3p(t.p), t3q(t.q) }; }

#endif
