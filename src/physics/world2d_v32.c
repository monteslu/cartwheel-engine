/* What Box2D v3.2 added to the 2D wrapper: world tuning, body / shape / chain / joint extras, snapshots, recording and
 * replay. world2d.c has the worlds, bodies, shapes, joints and queries these extend. */
#include <stdio.h>
#include <string.h>

#include "physics2d_internal.h"

/* ── world tuning ────────────────────────────────────────────────────── */

static cw_aabb2 aabb2(b2AABB b) { return (cw_aabb2){ cwv(b.lowerBound), cwv(b.upperBound) }; }

cw_aabb2 cw_world2d_bounds(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  return w ? aabb2(b2World_GetBounds(w->id)) : (cw_aabb2){ { 0, 0 }, { 0, 0 } };
}
uint64_t cw_world2d_state_hash(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  return w ? b2World_GetStateHash(w->id) : 0;
}
void cw_world2d_set_max_linear_speed(cw_world2d wh, float speed) {
  cw__world2 *w = cw__w2(wh);
  if (w) b2World_SetMaxLinearSpeed(w->id, speed);
}
float cw_world2d_max_linear_speed(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  return w ? b2World_GetMaxLinearSpeed(w->id) : 0;
}
void cw_world2d_set_contact_recycle_distance(cw_world2d wh, float meters) {
  cw__world2 *w = cw__w2(wh);
  if (w) b2World_SetContactRecycleDistance(w->id, meters);
}
float cw_world2d_contact_recycle_distance(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  return w ? b2World_GetContactRecycleDistance(w->id) : 0;
}
void cw_world2d_set_contact_tuning(cw_world2d wh, float hertz, float damping_ratio, float push_speed) {
  cw__world2 *w = cw__w2(wh);
  if (w) b2World_SetContactTuning(w->id, hertz, damping_ratio, push_speed);
}
void cw_world2d_contact_tuning(cw_world2d wh, float *hertz, float *damping_ratio, float *push_speed) {
  float h = 0, d = 0, p = 0;
  cw__world2 *w = cw__w2(wh);
  if (w) b2World_GetContactTuning(w->id, &h, &d, &p);
  if (hertz) *hertz = h;
  if (damping_ratio) *damping_ratio = d;
  if (push_speed) *push_speed = p;
}
void cw_world2d_set_restitution_iterations(cw_world2d wh, int iterations) {
  cw__world2 *w = cw__w2(wh);
  if (w) b2World_SetRestitutionIterations(w->id, iterations);
}
int cw_world2d_restitution_iterations(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  return w ? b2World_GetRestitutionIterations(w->id) : 0;
}
void cw_world2d_set_restitution_propagation(cw_world2d wh, bool on) {
  cw__world2 *w = cw__w2(wh);
  if (w) b2World_EnableRestitutionPropagation(w->id, on);
}
bool cw_world2d_restitution_propagation(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  return w && b2World_IsRestitutionPropagationEnabled(w->id);
}
void cw_world2d_set_hit_speed(cw_world2d wh, float speed) {
  cw__world2 *w = cw__w2(wh);
  if (w) b2World_SetHitEventThreshold(w->id, speed);
}
void cw_world2d_set_workers(cw_world2d wh, int workers) {
  cw__world2 *w = cw__w2(wh);
  if (!w) return;
  int n = cw_jobs_thread_count();
  int k = workers > 0 && workers < n ? workers : n;
  if (k > B2_MAX_WORKERS) k = B2_MAX_WORKERS;
  b2World_SetWorkerCount(w->id, k);
}
int cw_world2d_workers(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  return w ? b2World_GetWorkerCount(w->id) : 0;
}
cw_phys_capacity2d cw_world2d_capacity(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  if (!w) return (cw_phys_capacity2d){ 0 };
  b2Capacity c = b2World_GetMaxCapacity(w->id);
  return (cw_phys_capacity2d){ c.staticShapeCount, c.dynamicShapeCount, c.staticBodyCount, c.dynamicBodyCount,
                               c.contactCount, c.arenaByteCount };
}
void cw_world2d_set_narrow_solver(cw_world2d wh, bool on) {
  cw__world2 *w = cw__w2(wh);
  if (w) b2World_EnableSSE2Fallback(w->id, on);
}
bool cw_physics2d_avx2_available(void) { return b2IsAVX2Available(); }
size_t cw_physics2d_memory_bytes(void) { return b2GetByteCount(); }

cw_world2d_profile cw_world2d_profile_get(cw_world2d wh) {
  cw_world2d_profile r = { 0 };
  cw__world2 *w = cw__w2(wh);
  if (!w) return r;
  b2Profile p = b2World_GetProfile(w->id);
  r = (cw_world2d_profile){ p.step, p.pairs, p.collide, p.solve, p.solverSetup, p.constraints, p.prepareConstraints,
                            p.integrateVelocities, p.warmStart, p.solveImpulses, p.integratePositions, p.relaxImpulses,
                            p.restitution, p.storeImpulses, p.splitIslands, p.transforms, p.sensorHits, p.jointEvents,
                            p.hitEvents, p.refit, p.bullets, p.sleepIslands, p.sensors };
  return r;
}
cw_world2d_counters cw_world2d_counters_get(cw_world2d wh) {
  cw_world2d_counters r = { 0 };
  cw__world2 *w = cw__w2(wh);
  if (!w) return r;
  b2Counters c = b2World_GetCounters(w->id);
  r.byte_count = c.byteCount;
  r.bodies = c.bodyCount;
  r.shapes = c.shapeCount;
  r.contacts = c.contactCount;
  r.awake_contacts = c.awakeContactCount;
  r.recycled_contacts = c.recycledContactCount;
  r.joints = c.jointCount;
  r.islands = c.islandCount;
  r.stack_used = c.stackUsed;
  r.static_tree_height = c.staticTreeHeight;
  r.tree_height = c.treeHeight;
  r.tasks = c.taskCount;
  memcpy(r.color_counts, c.colorCounts, sizeof r.color_counts);
  return r;
}

/* Box2D's own log lines and failed assertions go through the engine's log (fail fast: an assertion stops here). */
static void b2_log(const char *message) { cw_warn("Box2D: %s", message); }
static int b2_assert(const char *condition, const char *file, int line) {
  cw_error("Box2D assertion failed: %s (%s:%d)", condition, file, line);
  return 1;
}
void cw__box2d_hooks(void) {
  static bool done;
  if (done) return;
  done = true;
  b2SetLogCallback(b2_log);
  b2SetAssertCallback(b2_assert);
}

/* ── bodies ──────────────────────────────────────────────────────────── */

cw_pos2 cw_body2d_world_center(cw_body2d b) {
  BODY_OR(b, ((cw_pos2){ 0, 0 }));
  return cwp(b2Body_GetWorldCenter(bid));
}
cw_vec2 cw_body2d_local_center(cw_body2d b) {
  BODY_OR(b, ((cw_vec2){ 0, 0 }));
  return cwv(b2Body_GetLocalCenter(bid));
}
cw_aabb2 cw_body2d_bounds(cw_body2d b) {
  BODY_OR(b, ((cw_aabb2){ { 0, 0 }, { 0, 0 } }));
  return aabb2(b2Body_ComputeBounds(bid));
}
float cw_body2d_min_extent(cw_body2d b) {
  BODY_OR(b, 0);
  return b2Body_GetMinExtent(bid);
}
float cw_body2d_max_extent(cw_body2d b) {
  BODY_OR(b, 0);
  return b2Body_GetMaxExtent(bid);
}
float cw_body2d_max_extent_from_origin(cw_body2d b) {
  BODY_OR(b, 0);
  return b2Body_GetMaxExtentFromOrigin(bid);
}
void cw_body2d_update_mass(cw_body2d b) {
  BODY_OR(b, );
  b2Body_UpdateMassFromShapes(bid);
}
void cw_body2d_set_sleep_enabled(cw_body2d b, bool enabled) {
  BODY_OR(b, );
  b2Body_EnableSleeping(bid, enabled);
}
bool cw_body2d_sleep_enabled(cw_body2d b) {
  BODY_OR(b, false);
  return b2Body_IsSleepingEnabled(bid);
}
void cw_body2d_set_contact_recycling(cw_body2d b, bool enabled) {
  BODY_OR(b, );
  b2Body_EnableContactRecycling(bid, enabled);
}
bool cw_body2d_contact_recycling(cw_body2d b) {
  BODY_OR(b, false);
  return b2Body_IsContactRecyclingEnabled(bid);
}
void cw_body2d_set_safety_factor(cw_body2d b, float factor) {
  BODY_OR(b, );
  b2Body_SetSafetyFactor(bid, factor);
}
float cw_body2d_safety_factor(cw_body2d b) {
  BODY_OR(b, 0);
  return b2Body_GetSafetyFactor(bid);
}

/* ── shapes and chains ───────────────────────────────────────────────── */

static b2ShapeDef shape_def_of(const cw_shape2d_def *def) {
  /* the same translation world2d.c does (kept in step by sharing the type, not the function: that one is static) */
  cw_shape2d_def d = def ? *def : cw_shape2d_default_def();
  b2ShapeDef sd = b2DefaultShapeDef();
  sd.material.friction = d.material.friction;
  sd.material.restitution = d.material.restitution;
  sd.material.rollingResistance = d.material.rolling_resistance;
  sd.material.tangentSpeed = d.material.tangent_speed;
  sd.material.userMaterialId = d.material.user_material;
  sd.material.customColor = d.debug_color;
  sd.density = d.density;
  sd.filter.categoryBits = cw__shape_category(d.layer);
  sd.filter.maskBits = cw__shape_mask(d.mask, d.sensor);
  sd.filter.groupIndex = d.group;
  sd.isSensor = d.sensor;
  sd.enableSensorEvents = d.sensor_events;
  sd.enableContactEvents = d.contact_events;
  sd.enableHitEvents = d.hit_events;
  sd.userData = d.user;
  return sd;
}

cw_shape2d cw_shape2d_rounded_box(cw_body2d b, const cw_shape2d_def *def, cw_vec2 half, float radius, cw_xform2 local) {
  BODY_OR(b, (cw_shape2d){ 0 });
  b2ShapeDef sd = shape_def_of(def);
  b2Polygon p = b2MakeOffsetRoundedBox(half.x, half.y, v2(local.p), r2(local.q), radius);
  return (cw_shape2d){ b2StoreShapeId(b2CreatePolygonShape(bid, &sd, &p)) };
}

static b2ChainSegment chain_segment(cw_vec2 g1, cw_vec2 p1, cw_vec2 p2, cw_vec2 g2) {
  return (b2ChainSegment){ v2(g1), { v2(p1), v2(p2) }, v2(g2), B2_NULL_INDEX };
}
cw_shape2d cw_shape2d_chain_segment(cw_body2d b, const cw_shape2d_def *def, cw_vec2 ghost1, cw_vec2 p1, cw_vec2 p2,
                                    cw_vec2 ghost2) {
  BODY_OR(b, (cw_shape2d){ 0 });
  b2ShapeDef sd = shape_def_of(def);
  sd.isSensor = false; /* chain segments are never sensors */
  sd.filter.maskBits = cw__shape_mask(def ? def->mask : CW_PHYS_ALL_LAYERS, false);
  b2ChainSegment seg = chain_segment(ghost1, p1, p2, ghost2);
  b2ShapeId id = b2CreateChainSegmentShape(bid, &sd, &seg);
  if (B2_IS_NULL(id)) {
    cw_error("physics2d: a chain segment needs p1 and p2 apart (and the ghost vertices clear of them)");
    return (cw_shape2d){ 0 };
  }
  return (cw_shape2d){ b2StoreShapeId(id) };
}
void cw_shape2d_set_chain_segment(cw_shape2d s, cw_vec2 ghost1, cw_vec2 p1, cw_vec2 p2, cw_vec2 ghost2) {
  SHAPE_OR(s, );
  b2ChainSegment seg = chain_segment(ghost1, p1, p2, ghost2);
  b2Shape_SetChainSegment(sid, &seg);
}
cw_aabb2 cw_shape2d_bounds(cw_shape2d s) {
  SHAPE_OR(s, ((cw_aabb2){ { 0, 0 }, { 0, 0 } }));
  return aabb2(b2Shape_GetBounds(sid));
}
bool cw_shape2d_test_point(cw_shape2d s, cw_pos2 point) {
  SHAPE_OR(s, false);
  return b2Shape_TestPoint(sid, bp(point));
}
cw_pos2 cw_shape2d_closest_point(cw_shape2d s, cw_pos2 target) {
  SHAPE_OR(s, target);
  return cwp(b2Shape_GetClosestPoint(sid, bp(target)));
}
bool cw_shape2d_raycast(cw_shape2d s, cw_pos2 origin, cw_vec2 translation, cw_ray_hit2d *hit) {
  memset(hit, 0, sizeof *hit);
  hit->triangle = -1;
  SHAPE_OR(s, false);
  b2WorldCastOutput o = b2Shape_CastRay(sid, bp(origin), v2(translation));
  if (!o.hit) return false;
  hit->shape = s;
  hit->body = (cw_body2d){ b2StoreBodyId(b2Shape_GetBody(sid)) };
  hit->point = cwp(o.point);
  hit->normal = cwv(o.normal);
  hit->fraction = o.fraction;
  hit->material = b2Shape_GetSurfaceMaterial(sid).userMaterialId;
  return true;
}

void cw_chain2d_set_material(cw_chain2d c, const cw_phys_material *m) {
  b2ChainId id = b2LoadChainId(c.id);
  if (!c.id || !b2Chain_IsValid(id)) return;
  b2SurfaceMaterial sm = b2DefaultSurfaceMaterial();
  sm.friction = m->friction;
  sm.restitution = m->restitution;
  sm.rollingResistance = m->rolling_resistance;
  sm.tangentSpeed = m->tangent_speed;
  sm.userMaterialId = m->user_material;
  b2Chain_SetAllSurfaceMaterials(id, &sm);
}
void cw_chain2d_set_segment_material(cw_chain2d c, int segment, const cw_phys_material *m) {
  b2ChainId id = b2LoadChainId(c.id);
  if (!c.id || !b2Chain_IsValid(id) || segment < 0 || segment >= b2Chain_GetSegmentCount(id)) return;
  b2SurfaceMaterial sm = b2DefaultSurfaceMaterial();
  sm.friction = m->friction;
  sm.restitution = m->restitution;
  sm.rollingResistance = m->rolling_resistance;
  sm.tangentSpeed = m->tangent_speed;
  sm.userMaterialId = m->user_material;
  b2Chain_SetSurfaceMaterial(id, &sm, segment);
}
cw_phys_material cw_chain2d_material(cw_chain2d c, int segment) {
  cw_phys_material m = { 0 };
  b2ChainId id = b2LoadChainId(c.id);
  if (!c.id || !b2Chain_IsValid(id) || segment < 0) return m;
  if (segment >= b2Chain_GetSegmentCount(id)) return m;
  b2SurfaceMaterial sm = b2Chain_GetSurfaceMaterial(id, segment);
  m.friction = sm.friction;
  m.restitution = sm.restitution;
  m.rolling_resistance = sm.rollingResistance;
  m.tangent_speed = sm.tangentSpeed;
  m.user_material = sm.userMaterialId;
  return m;
}

/* ── joints ──────────────────────────────────────────────────────────── */

void cw_joint2d_set_mover(cw_joint2d j, cw_vec2 velocity, cw_vec2 max_force) {
  JOINT_OR(j, );
  if (b2Joint_GetType(jid) != b2_moverJoint) return;
  b2MoverJoint_SetLinearVelocity(jid, v2(velocity));
  b2MoverJoint_SetMaxVelocityForce(jid, v2(max_force));
  b2Joint_WakeBodies(jid);
}
void cw_joint2d_set_rest_length(cw_joint2d j, float length) {
  JOINT_OR(j, );
  switch (b2Joint_GetType(jid)) {
  case b2_distanceJoint: b2DistanceJoint_SetRestLength(jid, length); break;
  case b2_pogoJoint: b2PogoJoint_SetRestLength(jid, length); break;
  default: return;
  }
  b2Joint_WakeBodies(jid);
}
float cw_joint2d_rest_length(cw_joint2d j) {
  JOINT_OR(j, 0);
  switch (b2Joint_GetType(jid)) {
  case b2_distanceJoint: return b2DistanceJoint_GetRestLength(jid);
  case b2_pogoJoint: return b2PogoJoint_GetRestLength(jid);
  default: return 0;
  }
}
float cw_joint2d_speed(cw_joint2d j) {
  JOINT_OR(j, 0);
  switch (b2Joint_GetType(jid)) {
  case b2_revoluteJoint: return b2RevoluteJoint_GetCurrentSpeed(jid);
  case b2_prismaticJoint: return b2PrismaticJoint_GetCurrentSpeed(jid);
  case b2_pogoJoint: return b2PogoJoint_GetCurrentSpeed(jid);
  default: return 0;
  }
}
float cw_joint2d_motor_force(cw_joint2d j) {
  JOINT_OR(j, 0);
  switch (b2Joint_GetType(jid)) {
  case b2_revoluteJoint: return b2RevoluteJoint_GetCurrentMotorTorque(jid);
  case b2_prismaticJoint: return b2PrismaticJoint_GetCurrentMotorForce(jid);
  case b2_wheelJoint: return b2WheelJoint_GetCurrentMotorTorque(jid);
  case b2_distanceJoint: return b2DistanceJoint_GetCurrentMotorForce(jid);
  default: return 0;
  }
}
float cw_joint2d_spring_force(cw_joint2d j) {
  JOINT_OR(j, 0);
  switch (b2Joint_GetType(jid)) {
  case b2_distanceJoint: return b2DistanceJoint_GetSpringForce(jid);
  case b2_pogoJoint: return b2PogoJoint_GetImpulse(jid);
  default: return 0;
  }
}
float cw_joint2d_linear_separation(cw_joint2d j) {
  JOINT_OR(j, 0);
  return b2Joint_GetLinearSeparation(jid);
}
float cw_joint2d_angular_separation(cw_joint2d j) {
  JOINT_OR(j, 0);
  return b2Joint_GetAngularSeparation(jid);
}

/* ── snapshots ───────────────────────────────────────────────────────── */

/* Box2D's image is simulation state only: user data on bodies, shapes and joints is nulled by a restore, and the
 * wrapper's own records (a body's interpolation poses, a joint's break flag...) are not in it. The snapshot is
 * Box2D's image followed by those records, user pointers included, so it is an in-process snapshot (rollback, undo):
 * do not send it to another machine.
 *   header | Box2D image | body records | joint records | shape user pointers */
#define SNAP_MAGIC 0x32443243u /* "C2D2" */
#define SNAP_VERSION 1

typedef struct snap_hdr {
  uint32_t magic, version;
  int32_t b2_size, bodies, joints, shapes;
  int32_t steps, span;
  uint32_t hash;
  uint32_t by_tick;
  b2BodyId ground;
} snap_hdr;

typedef struct snap_shape {
  uint64_t id;
  void *user;
} snap_shape;

static int count_valid(const cw_pool *p, bool joints) {
  int n = 0;
  for (int i = 0; i < p->slot_count; i++) {
    const void *e = cw_pool_at(p, i);
    if (!e) continue;
    n += joints ? b2Joint_IsValid(b2LoadJointId(((const cw__joint2 *)e)->id))
                : b2Body_IsValid(b2LoadBodyId(((const cw__body2 *)e)->id));
  }
  return n;
}

static int count_shapes(const cw__world2 *w) {
  int n = 0;
  for (int i = 0; i < w->bodies.slot_count; i++) {
    const cw__body2 *r = cw_pool_at(&w->bodies, i);
    if (r && b2Body_IsValid(b2LoadBodyId(r->id))) n += b2Body_GetShapeCount(b2LoadBodyId(r->id));
  }
  return n;
}

int cw_world2d_snapshot_size(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  if (!w) return 0;
  int b2n = b2World_GetSnapshot(w->id, NULL, 0);
  if (b2n <= 0) return 0;
  return (int)(sizeof(snap_hdr)) + b2n + count_valid(&w->bodies, false) * (int)sizeof(cw__body2) +
         count_valid(&w->joints, true) * (int)sizeof(cw__joint2) + count_shapes(w) * (int)sizeof(snap_shape);
}

int cw_world2d_snapshot(cw_world2d wh, uint8_t *out, int cap) {
  cw__world2 *w = cw__w2(wh);
  if (!w) return 0;
  int need = cw_world2d_snapshot_size(wh);
  if (need <= 0 || need > cap || !out) {
    if (need > 0) cw_error("physics2d: snapshot needs %d bytes, got %d", need, cap);
    else cw_error("physics2d: a snapshot is taken between steps");
    return 0;
  }
  snap_hdr h = { SNAP_MAGIC, SNAP_VERSION };
  h.steps = w->steps;
  h.span = w->span;
  h.hash = w->hash;
  h.by_tick = w->by_tick;
  h.ground = w->ground;
  uint8_t *p = out + sizeof h;
  h.b2_size = b2World_GetSnapshot(w->id, p, cap - (int)sizeof h);
  p += h.b2_size;
  for (int i = 0; i < w->bodies.slot_count; i++) {
    const cw__body2 *r = cw_pool_at(&w->bodies, i);
    if (!r || !b2Body_IsValid(b2LoadBodyId(r->id))) continue;
    memcpy(p, r, sizeof *r);
    p += sizeof *r;
    h.bodies++;
  }
  for (int i = 0; i < w->joints.slot_count; i++) {
    const cw__joint2 *r = cw_pool_at(&w->joints, i);
    if (!r || !b2Joint_IsValid(b2LoadJointId(r->id))) continue;
    memcpy(p, r, sizeof *r);
    p += sizeof *r;
    h.joints++;
  }
  for (int i = 0; i < w->bodies.slot_count; i++) {
    const cw__body2 *r = cw_pool_at(&w->bodies, i);
    if (!r || !b2Body_IsValid(b2LoadBodyId(r->id))) continue;
    b2BodyId b = b2LoadBodyId(r->id);
    int n = b2Body_GetShapeCount(b);
    b2ShapeId stack[64], *shapes = n > 64 ? cw_alloc((size_t)n * sizeof *shapes, CW_MEM_PHYSICS) : stack;
    int got = b2Body_GetShapes(b, shapes, n);
    for (int k = 0; k < got; k++) {
      snap_shape s = { b2StoreShapeId(shapes[k]), b2Shape_GetUserData(shapes[k]) };
      memcpy(p, &s, sizeof s);
      p += sizeof s;
      h.shapes++;
    }
    if (shapes != stack) cw_free(shapes);
  }
  memcpy(out, &h, sizeof h);
  return (int)(p - out);
}

bool cw_world2d_restore(cw_world2d wh, const uint8_t *image, int size) {
  cw__world2 *w = cw__w2(wh);
  if (!w) return false;
  snap_hdr h;
  if (!image || size < (int)sizeof h) {
    cw_error("physics2d: restore of a snapshot that is too short");
    return false;
  }
  memcpy(&h, image, sizeof h);
  size_t want = sizeof h + (size_t)h.b2_size + (size_t)h.bodies * sizeof(cw__body2) +
                (size_t)h.joints * sizeof(cw__joint2) + (size_t)h.shapes * sizeof(snap_shape);
  if (h.magic != SNAP_MAGIC || h.version != SNAP_VERSION || h.b2_size <= 0 || h.bodies < 0 || h.joints < 0 ||
      h.shapes < 0 || want != (size_t)size) {
    cw_error("physics2d: not a cw_world2d_snapshot image (or one from another engine version)");
    return false;
  }
  const uint8_t *p = image + sizeof h;
  if (!b2World_RestoreFromSnapshot(w->id, p, h.b2_size)) {
    cw_error("physics2d: Box2D rejected the snapshot; a half-applied restore leaves the world unusable: destroy it");
    return false;
  }
  p += h.b2_size;
  /* the wrapper's records: the bodies and joints of the snapshot get their records (and Box2D user pointers) back,
   * anything made since is already gone from Box2D */
  cw_pool_free_all(&w->bodies);
  cw_pool_free_all(&w->joints);
  for (int i = 0; i < h.bodies; i++, p += sizeof(cw__body2)) {
    uint64_t rid;
    cw__body2 *r = cw_pool_alloc(&w->bodies, &rid);
    memcpy(r, p, sizeof *r);
    r->settled = false; /* the nodes write the restored poses */
    b2BodyId b = b2LoadBodyId(r->id);
    if (b2Body_IsValid(b)) b2Body_SetUserData(b, r);
    else cw_pool_release(&w->bodies, rid);
  }
  for (int i = 0; i < h.joints; i++, p += sizeof(cw__joint2)) {
    uint64_t rid;
    cw__joint2 *r = cw_pool_alloc(&w->joints, &rid);
    memcpy(r, p, sizeof *r);
    b2JointId j = b2LoadJointId(r->id);
    if (b2Joint_IsValid(j)) b2Joint_SetUserData(j, r);
    else cw_pool_release(&w->joints, rid);
  }
  for (int i = 0; i < h.shapes; i++, p += sizeof(snap_shape)) {
    snap_shape s;
    memcpy(&s, p, sizeof s);
    b2ShapeId sh = b2LoadShapeId(s.id);
    if (b2Shape_IsValid(sh)) b2Shape_SetUserData(sh, s.user);
  }
  w->steps = h.steps;
  w->span = h.span;
  w->hash = h.hash;
  w->by_tick = h.by_tick != 0;
  w->ground = h.ground;
  cw_array_clear(&w->events);
  cw_array_clear(&w->breaking);
  return true;
}

/* ── recording ───────────────────────────────────────────────────────── */

struct cw_recording2d {
  b2Recording *rec;
  cw__world2 *world; /* while it is being written */
};

cw_recording2d *cw_world2d_record_start(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  if (!w) return NULL;
  if (w->recording) {
    cw_error("physics2d: the world is already recording");
    return NULL;
  }
  cw_recording2d *r = cw_calloc(1, sizeof *r, CW_MEM_PHYSICS);
  r->rec = b2CreateRecording(0);
  r->world = w;
  w->recording = r;
  b2World_StartRecording(w->id, r->rec);
  return r;
}
void cw_world2d_record_stop(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  if (!w || !w->recording) return;
  b2World_StopRecording(w->id);
  w->recording->world = NULL;
  w->recording = NULL;
}
const uint8_t *cw_recording2d_data(const cw_recording2d *r, int *size) {
  if (size) *size = r ? b2Recording_GetSize(r->rec) : 0;
  return r ? b2Recording_GetData(r->rec) : NULL;
}
bool cw_recording2d_save(const cw_recording2d *r, const char *path) { return r && b2Recording_SaveToFile(r->rec, path); }
cw_recording2d *cw_recording2d_load(const char *path) {
  b2Recording *rec = b2CreateRecordingFromFile(path);
  if (!rec) return NULL;
  cw_recording2d *r = cw_calloc(1, sizeof *r, CW_MEM_PHYSICS);
  r->rec = rec;
  return r;
}
void cw_recording2d_destroy(cw_recording2d *r) {
  if (!r) return;
  if (r->world) { /* still being written: end it first, Box2D would write into freed memory */
    b2World_StopRecording(r->world->id);
    r->world->recording = NULL;
  }
  b2DestroyRecording(r->rec);
  cw_free(r);
}
bool cw_recording2d_validate(const void *data, int size) { return b2ValidateRecording(data, size, 0); }

/* ── replay ──────────────────────────────────────────────────────────── */

struct cw_replay2d {
  b2Replay *p;
};

cw_replay2d *cw_replay2d_open(const void *data, int size) {
  b2Replay *p = b2CreateReplay(data, size, 0);
  if (!p) {
    cw_error("physics2d: not a Box2D recording (or one this Box2D cannot replay)");
    return NULL;
  }
  cw_replay2d *r = cw_calloc(1, sizeof *r, CW_MEM_PHYSICS);
  r->p = p;
  return r;
}
void cw_replay2d_close(cw_replay2d *r) {
  if (!r) return;
  b2DestroyReplay(r->p);
  cw_free(r);
}
bool cw_replay2d_step(cw_replay2d *r) { return r && b2Replay_StepFrame(r->p); }
void cw_replay2d_seek(cw_replay2d *r, int frame) {
  if (r) b2Replay_SeekFrame(r->p, frame);
}
void cw_replay2d_restart(cw_replay2d *r) {
  if (r) b2Replay_Restart(r->p);
}
int cw_replay2d_frame(const cw_replay2d *r) { return r ? b2Replay_GetFrame(r->p) : 0; }
bool cw_replay2d_at_end(const cw_replay2d *r) { return !r || b2Replay_IsAtEnd(r->p); }
cw_replay2d_info cw_replay2d_get_info(const cw_replay2d *r) {
  cw_replay2d_info i = { 0 };
  if (!r) return i;
  b2ReplayInfo b = b2Replay_GetInfo(r->p);
  i.frames = b.frameCount;
  i.dt = b.timeStep;
  i.substeps = b.subStepCount;
  i.bounds = aabb2(b.bounds);
  return i;
}
bool cw_replay2d_diverged(const cw_replay2d *r) { return r && b2Replay_HasDiverged(r->p); }
int cw_replay2d_diverge_frame(const cw_replay2d *r) { return r ? b2Replay_GetDivergeFrame(r->p) : -1; }
void cw_replay2d_set_keyframes(cw_replay2d *r, size_t budget_bytes, int min_interval) {
  if (r) b2Replay_SetKeyframePolicy(r->p, budget_bytes, min_interval);
}
b2WorldId cw_replay2d_world(const cw_replay2d *r) { return r ? b2Replay_GetWorldId(r->p) : b2_nullWorldId; }
int cw_replay2d_body_count(const cw_replay2d *r) { return r ? b2Replay_GetBodyCount(r->p) : 0; }
b2BodyId cw_replay2d_body(const cw_replay2d *r, int index) { return r ? b2Replay_GetBodyId(r->p, index) : b2_nullBodyId; }
int cw_replay2d_query_count(const cw_replay2d *r) { return r ? b2Replay_GetFrameQueryCount(r->p) : 0; }
cw_replay2d_query cw_replay2d_get_query(const cw_replay2d *r, int index) {
  cw_replay2d_query q = { 0 };
  if (!r || index < 0 || index >= b2Replay_GetFrameQueryCount(r->p)) return q;
  b2ReplayQueryInfo i = b2Replay_GetFrameQuery(r->p, index);
  q.type = (cw_replay2d_query_type)i.type;
  q.origin = cwp(i.origin);
  q.aabb = aabb2(i.aabb);
  q.translation = cwv(i.translation);
  q.hit_count = i.hitCount;
  return q;
}
cw_replay2d_hit cw_replay2d_get_hit(const cw_replay2d *r, int query, int hit) {
  cw_replay2d_hit h = { b2_nullShapeId };
  if (!r || query < 0 || query >= b2Replay_GetFrameQueryCount(r->p) || hit < 0 ||
      hit >= b2Replay_GetFrameQuery(r->p, query).hitCount)
    return h;
  b2ReplayQueryHit b = b2Replay_GetFrameQueryHit(r->p, query, hit);
  h.shape = b.shapeId;
  h.point = cwp(b.point);
  h.normal = cwv(b.normal);
  h.fraction = b.fraction;
  return h;
}
void cw_replay2d_debug_draw_queries(cw_replay2d *r, cw_phys_lines *out, int query) {
  if (!r) return;
  b2DebugDraw dd = cw__debug_draw2(out, CW_PHYS_DRAW_SHAPES);
  b2Replay_DrawFrameQueries(r->p, &dd, query);
}
size_t cw_replay2d_keyframe_bytes(const cw_replay2d *r) { return r ? b2Replay_GetKeyframeBytes(r->p) : 0; }
size_t cw_replay2d_keyframe_budget(const cw_replay2d *r) { return r ? b2Replay_GetKeyframeBudget(r->p) : 0; }
int cw_replay2d_keyframe_interval(const cw_replay2d *r) { return r ? b2Replay_GetKeyframeInterval(r->p) : 0; }
void cw_replay2d_debug_draw(cw_replay2d *r, cw_phys_lines *out, uint32_t flags) {
  if (r) cw__world2_draw(b2Replay_GetWorldId(r->p), out, flags);
}
