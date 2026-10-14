/* Box2D wrapper: worlds, bodies, shapes, chains, joints, events, sync,
 * debug draw. Mirrors world3d.c section by section. */
#include <string.h>

#include "physics2d_internal.h"

cw_xform2 cw_xform2_lerp(cw_xform2 a, cw_xform2 b, float t) {
  cw_xform2 r;
  r.p = (cw_vec2){ a.p.x + (b.p.x - a.p.x) * t, a.p.y + (b.p.y - a.p.y) * t };
  float c = a.q.c + (b.q.c - a.q.c) * t, s = a.q.s + (b.q.s - a.q.s) * t;
  float l = sqrtf(c * c + s * s);
  r.q = l > 0 ? (cw_rot2){ c / l, s / l } : b.q;
  return r;
}

/* ── registry ────────────────────────────────────────────────────────── */

cw__world2 *cw__w2(cw_world2d w) {
  if (!w.id) return NULL;
  b2WorldId id = b2LoadWorldId((uint32_t)w.id);
  if (!b2World_IsValid(id)) return NULL;
  return b2World_GetUserData(id);
}
static cw__world2 *world_of_body(b2BodyId id) { return b2World_GetUserData(b2Body_GetWorld(id)); }
cw__body2 *cw__b2(cw_body2d b) {
  if (!b.id) return NULL;
  b2BodyId id = b2LoadBodyId(b.id);
  if (!b2Body_IsValid(id)) return NULL;
  return b2Body_GetUserData(id);
}
static cw__joint2 *jrec(cw_joint2d j) {
  if (!j.id) return NULL;
  b2JointId id = b2LoadJointId(j.id);
  if (!b2Joint_IsValid(id)) return NULL;
  return b2Joint_GetUserData(id);
}

#define BODY_OR(b, ret) \
  b2BodyId bid = b2LoadBodyId((b).id); \
  if (!(b).id || !b2Body_IsValid(bid)) { \
    cw_warn("physics2d: %s on a stale or null body", __func__); \
    return ret; \
  }

/* ── worlds ──────────────────────────────────────────────────────────── */

cw_world2d_def cw_world2d_default_def(void) {
  return (cw_world2d_def){ .gravity = { 0, -10 }, .substeps = 4, .hit_speed = 1, .restitution_threshold = 1,
                           .sleep = true, .continuous = true };
}

cw_world2d cw_world2d_create(const cw_world2d_def *def) {
  cw_world2d_def d = def ? *def : cw_world2d_default_def();
  b2WorldDef wd = b2DefaultWorldDef();
  wd.gravity = v2(d.gravity);
  wd.hitEventThreshold = d.hit_speed;
  wd.restitutionThreshold = d.restitution_threshold;
  wd.enableSleep = d.sleep;
  wd.enableContinuous = d.continuous;
  cw_box2d_tasks(&wd, d.workers);
  cw__world2 *w = cw_calloc(1, sizeof *w, CW_MEM_PHYSICS);
  wd.userData = w;
  w->tasks = wd.userTaskContext;
  w->id = b2CreateWorld(&wd);
  if (B2_IS_NULL(w->id)) {
    cw_error("physics2d: b2CreateWorld failed (at most 128 worlds at once)");
    cw_box2d_tasks_release(w->tasks);
    cw_free(w);
    return (cw_world2d){ 0 };
  }
  w->substeps = d.substeps > 0 ? d.substeps : 4;
  w->hash = CW_HASH32_INIT;
  cw_pool_init(&w->bodies, sizeof(cw__body2), CW_MEM_PHYSICS);
  cw_pool_init(&w->joints, sizeof(cw__joint2), CW_MEM_PHYSICS);
  cw__layers_init(&w->layers, d.layers);
  return (cw_world2d){ b2StoreWorldId(w->id) };
}

void cw_world2d_destroy(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  if (!w) return;
  b2DestroyWorld(w->id);
  cw_box2d_tasks_release(w->tasks); /* only after the world: its steps used it */
  cw_pool_free_all(&w->bodies);
  cw_pool_free_all(&w->joints);
  cw_array_free(&w->events);
  cw_array_free(&w->breaking);
  cw__layers_free(&w->layers);
  cw_free(w);
}

bool cw_world2d_valid(cw_world2d w) { return cw__w2(w) != NULL; }
b2WorldId cw_world2d_raw(cw_world2d w) { return w.id ? b2LoadWorldId((uint32_t)w.id) : b2_nullWorldId; }
void cw_world2d_set_gravity(cw_world2d wh, cw_vec2 g) {
  cw__world2 *w = cw__w2(wh);
  if (w) b2World_SetGravity(w->id, v2(g));
}
cw_vec2 cw_world2d_gravity(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  return w ? cwv(b2World_GetGravity(w->id)) : (cw_vec2){ 0, 0 };
}
int cw_world2d_layer(cw_world2d wh, const char *name) {
  cw__world2 *w = cw__w2(wh);
  int i = w ? cw__layers_find(&w->layers, name, strlen(name)) : -1;
  if (i < 0) cw_error("physics2d: no layer named '%s' (name it in cw_world2d_def.layers)", name);
  return i;
}
uint32_t cw_world2d_layer_bits(cw_world2d wh, const char *names) {
  cw__world2 *w = cw__w2(wh);
  return w ? cw__layers_bits(&w->layers, names) : 0;
}

static cw_body2d body_of_shape(b2ShapeId s) {
  return b2Shape_IsValid(s) ? (cw_body2d){ b2StoreBodyId(b2Shape_GetBody(s)) } : (cw_body2d){ 0 };
}
static cw_phys_event2d *push_event(cw__world2 *w, cw_phys_event_type type, b2ShapeId a, b2ShapeId b) {
  cw_array_reserve(&w->events, w->events.count + 1);
  cw_phys_event2d *e = &w->events.data[w->events.count++];
  memset(e, 0, sizeof *e);
  e->type = type;
  e->shape_a = (cw_shape2d){ b2StoreShapeId(a) };
  e->shape_b = (cw_shape2d){ b2StoreShapeId(b) };
  e->body_a = body_of_shape(a);
  e->body_b = body_of_shape(b);
  return e;
}

void cw_world2d_step(cw_world2d wh, float dt) {
  cw__world2 *w = cw__w2(wh);
  if (!w) {
    cw_warn("physics2d: step on a stale or null world");
    return;
  }
  for (int i = 0; i < w->joints.slot_count; i++) {
    cw__joint2 *j = cw_pool_at(&w->joints, i);
    if (j && j->type == CW_JOINT_MOUSE && b2Body_IsValid(j->mouse_body))
      b2Body_SetTargetTransform(j->mouse_body, (b2Transform){ v2(j->target), b2Rot_identity }, dt, true);
  }
  cw_box2d_tasks_step(w->id, w->tasks);
  b2World_Step(w->id, dt, w->substeps);
  w->steps++;
  if (!w->by_tick) w->span++;
  cw_array_clear(&w->events);
  cw_array_clear(&w->breaking);
  uint32_t h = cw__fold(w->hash, &w->steps, sizeof w->steps);

  b2BodyEvents be = b2World_GetBodyEvents(w->id);
  for (int i = 0; i < be.moveCount; i++) {
    const b2BodyMoveEvent *m = &be.moveEvents[i];
    uint64_t id = b2StoreBodyId(m->bodyId);
    float tf[4] = { m->transform.p.x, m->transform.p.y, m->transform.q.c, m->transform.q.s };
    h = cw__fold(h, &id, sizeof id);
    h = cw__fold(h, tf, sizeof tf);
    cw__body2 *r = m->userData;
    if (!r) continue;
    if (r->moved_span != w->span) r->prev = r->cur; /* the pose the span started from */
    r->cur = x2(m->transform);
    r->moved_span = w->span;
    r->settled = false;
  }

  b2ContactEvents ce = b2World_GetContactEvents(w->id);
  for (int i = 0; i < ce.beginCount; i++)
    push_event(w, CW_PHYS_CONTACT_BEGIN, ce.beginEvents[i].shapeIdA, ce.beginEvents[i].shapeIdB);
  for (int i = 0; i < ce.endCount; i++)
    push_event(w, CW_PHYS_CONTACT_END, ce.endEvents[i].shapeIdA, ce.endEvents[i].shapeIdB);
  for (int i = 0; i < ce.hitCount; i++) {
    const b2ContactHitEvent *hv = &ce.hitEvents[i];
    cw_phys_event2d *e = push_event(w, CW_PHYS_HIT, hv->shapeIdA, hv->shapeIdB);
    e->point = cwv(hv->point);
    e->normal = cwv(hv->normal);
    e->speed = hv->approachSpeed;
    /* Box2D's hit event carries no materials: read them from the shapes */
    if (b2Shape_IsValid(hv->shapeIdA)) e->material_a = b2Shape_GetSurfaceMaterial(hv->shapeIdA).userMaterialId;
    if (b2Shape_IsValid(hv->shapeIdB)) e->material_b = b2Shape_GetSurfaceMaterial(hv->shapeIdB).userMaterialId;
  }
  b2SensorEvents se = b2World_GetSensorEvents(w->id);
  for (int i = 0; i < se.beginCount; i++)
    push_event(w, CW_PHYS_SENSOR_ENTER, se.beginEvents[i].sensorShapeId, se.beginEvents[i].visitorShapeId);
  for (int i = 0; i < se.endCount; i++)
    push_event(w, CW_PHYS_SENSOR_EXIT, se.endEvents[i].sensorShapeId, se.endEvents[i].visitorShapeId);
  b2JointEvents je = b2World_GetJointEvents(w->id);
  for (int i = 0; i < je.count; i++) {
    cw__joint2 *j = je.jointEvents[i].userData;
    b2JointId jid = je.jointEvents[i].jointId;
    if (!j || !j->breakable || !b2Joint_IsValid(jid)) continue;
    cw_phys_event2d *e = push_event(w, CW_PHYS_JOINT_BREAK, b2_nullShapeId, b2_nullShapeId);
    e->joint = (cw_joint2d){ b2StoreJointId(jid) };
    e->body_a = (cw_body2d){ b2StoreBodyId(b2Joint_GetBodyA(jid)) };
    e->body_b = (cw_body2d){ b2StoreBodyId(b2Joint_GetBodyB(jid)) };
    e->user = j->user;
    cw_array_push(&w->breaking, jid);
  }
  for (int i = 0; i < w->breaking.count; i++) cw__joint2_destroy(w, w->breaking.data[i]);

  for (int i = 0; i < w->events.count; i++) {
    const cw_phys_event2d *e = &w->events.data[i];
    uint64_t k[4] = { (uint64_t)e->type, e->shape_a.id, e->shape_b.id, e->joint.id };
    h = cw__fold(h, k, sizeof k);
    if (e->type == CW_PHYS_HIT) {
      float f[5] = { e->point.x, e->point.y, e->normal.x, e->normal.y, e->speed };
      h = cw__fold(h, f, sizeof f);
    }
  }
  w->hash = h;
}

const cw_phys_event2d *cw_world2d_events(cw_world2d wh, int *count) {
  cw__world2 *w = cw__w2(wh);
  *count = w ? w->events.count : 0;
  return w ? w->events.data : NULL;
}

cw_phys_stats cw_world2d_stats(cw_world2d wh) {
  cw_phys_stats s = { 0 };
  cw__world2 *w = cw__w2(wh);
  if (!w) return s;
  b2Counters c = b2World_GetCounters(w->id);
  s.bodies = c.bodyCount;
  s.shapes = c.shapeCount;
  s.contacts = c.contactCount;
  s.joints = c.jointCount;
  s.awake_bodies = b2World_GetAwakeBodyCount(w->id);
  s.events = w->events.count;
  s.steps = w->steps;
  s.hash = w->hash;
  s.step_ms = b2World_GetProfile(w->id).step;
  return s;
}

void cw_world2d_tick_begin(cw_world2d wh) {
  cw__world2 *w = cw__w2(wh);
  if (!w) return;
  w->by_tick = true;
  w->span++;
}

/* ── sync ────────────────────────────────────────────────────────────── */

static cw_xform2 rec_at(const cw__world2 *w, const cw__body2 *r, float alpha) {
  if (r->interpolate && r->moved_span == w->span) return cw_xform2_lerp(r->prev, r->cur, alpha);
  return r->cur;
}

static void write_target(cw__body2 *r, cw_xform2 x) {
  if (r->obj) {
    t3_object_set_position(r->obj, x.p.x, x.p.y, ((t3_object *)r->obj)->position.z);
    /* rotation about z from (cos, sin) by half-angle identities: no trig */
    float ch = sqrtf(CW_MAX(0.0f, (1 + x.q.c) * 0.5f)), sh = sqrtf(CW_MAX(0.0f, (1 - x.q.c) * 0.5f));
    if (x.q.s < 0) sh = -sh;
    t3_object_set_quaternion(r->obj, (t3_quat){ 0, 0, sh, ch });
  } else if (r->xf) {
    *r->xf = x;
  }
}

void cw_world2d_sync(cw_world2d wh, float alpha) {
  cw__world2 *w = cw__w2(wh);
  if (!w) return;
  for (int i = 0; i < w->bodies.slot_count; i++) {
    cw__body2 *r = cw_pool_at(&w->bodies, i);
    if (!r || (!r->obj && !r->xf)) continue;
    if (r->moved_span == w->span) write_target(r, rec_at(w, r, alpha));
    else if (!r->settled) {
      write_target(r, r->cur);
      r->settled = true;
    }
  }
}

/* ── bodies ──────────────────────────────────────────────────────────── */

cw_body2d_def cw_body2d_default_def(void) {
  return (cw_body2d_def){ .type = CW_BODY_DYNAMIC, .rotation = { 1, 0 }, .gravity_scale = 1, .awake = true,
                          .sleep = true, .interpolate = true };
}

cw_body2d cw_body2d_create(cw_world2d wh, const cw_body2d_def *def) {
  cw__world2 *w = cw__w2(wh);
  if (!w) {
    cw_warn("physics2d: body on a stale or null world");
    return (cw_body2d){ 0 };
  }
  cw_body2d_def d = def ? *def : cw_body2d_default_def();
  uint64_t rid;
  cw__body2 *r = cw_pool_alloc(&w->bodies, &rid);
  b2BodyDef bd = b2DefaultBodyDef();
  bd.type = (b2BodyType)d.type;
  bd.position = v2(d.position);
  bd.rotation = r2(d.rotation);
  bd.linearVelocity = v2(d.linear_velocity);
  bd.angularVelocity = d.angular_velocity;
  bd.linearDamping = d.linear_damping;
  bd.angularDamping = d.angular_damping;
  bd.gravityScale = d.gravity_scale;
  bd.motionLocks.angularZ = d.lock_rotation;
  bd.isBullet = d.bullet;
  bd.isAwake = d.awake;
  bd.enableSleep = d.sleep;
  bd.name = d.name;
  bd.userData = r;
  b2BodyId id = b2CreateBody(w->id, &bd);
  r->id = b2StoreBodyId(id);
  r->user = d.user;
  r->interpolate = d.interpolate;
  r->cur = r->prev = x2(b2Body_GetTransform(id));
  r->moved_span = -1;
  return (cw_body2d){ r->id };
}

void cw__joint2_destroy(cw__world2 *w, b2JointId jid) {
  cw__joint2 *j = b2Joint_GetUserData(jid);
  b2BodyId mouse = j ? j->mouse_body : b2_nullBodyId;
  b2DestroyJoint(jid, true);
  if (j) cw_pool_release(&w->joints, cw_pool_id_of(&w->joints, j));
  if (B2_IS_NON_NULL(mouse) && b2Body_IsValid(mouse)) b2DestroyBody(mouse);
}

void cw_body2d_destroy(cw_body2d b) {
  BODY_OR(b, );
  cw__world2 *w = world_of_body(bid);
  b2JointId js[64];
  for (;;) {
    int n = b2Body_GetJoints(bid, js, 64);
    if (!n) break;
    for (int i = 0; i < n; i++) cw__joint2_destroy(w, js[i]);
  }
  cw__body2 *r = b2Body_GetUserData(bid);
  b2DestroyBody(bid);
  if (r) cw_pool_release(&w->bodies, cw_pool_id_of(&w->bodies, r));
}

bool cw_body2d_valid(cw_body2d b) { return b.id && b2Body_IsValid(b2LoadBodyId(b.id)); }
b2BodyId cw_body2d_raw(cw_body2d b) { return b2LoadBodyId(b.id); }
cw_world2d cw_body2d_world(cw_body2d b) {
  BODY_OR(b, (cw_world2d){ 0 });
  return (cw_world2d){ b2StoreWorldId(b2Body_GetWorld(bid)) };
}
cw_body_type cw_body2d_type(cw_body2d b) {
  BODY_OR(b, CW_BODY_STATIC);
  return (cw_body_type)b2Body_GetType(bid);
}
void cw_body2d_set_type(cw_body2d b, cw_body_type t) {
  BODY_OR(b, );
  b2Body_SetType(bid, (b2BodyType)t);
}
cw_xform2 cw_body2d_transform(cw_body2d b) {
  BODY_OR(b, cw_xform2_identity());
  return x2(b2Body_GetTransform(bid));
}
void cw_body2d_set_transform(cw_body2d b, cw_xform2 x) {
  BODY_OR(b, );
  b2Body_SetTransform(bid, v2(x.p), r2(x.q));
  b2Body_SetAwake(bid, true);
  cw__body2 *r = b2Body_GetUserData(bid);
  if (r) {
    r->prev = r->cur = x2(b2Body_GetTransform(bid));
    r->settled = false;
  }
}
void cw_body2d_set_target(cw_body2d b, cw_xform2 target, float dt) {
  BODY_OR(b, );
  b2Body_SetTargetTransform(bid, bx2(target), dt, true);
}
cw_vec2 cw_body2d_velocity(cw_body2d b) {
  BODY_OR(b, (cw_vec2){ 0 });
  return cwv(b2Body_GetLinearVelocity(bid));
}
void cw_body2d_set_velocity(cw_body2d b, cw_vec2 v) {
  BODY_OR(b, );
  b2Body_SetLinearVelocity(bid, v2(v));
}
float cw_body2d_angular_velocity(cw_body2d b) {
  BODY_OR(b, 0);
  return b2Body_GetAngularVelocity(bid);
}
void cw_body2d_set_angular_velocity(cw_body2d b, float v) {
  BODY_OR(b, );
  b2Body_SetAngularVelocity(bid, v);
}
void cw_body2d_apply_force(cw_body2d b, cw_vec2 f, const cw_vec2 *point) {
  BODY_OR(b, );
  if (point) b2Body_ApplyForce(bid, v2(f), v2(*point), true);
  else b2Body_ApplyForceToCenter(bid, v2(f), true);
}
void cw_body2d_apply_impulse(cw_body2d b, cw_vec2 j, const cw_vec2 *point) {
  BODY_OR(b, );
  if (point) b2Body_ApplyLinearImpulse(bid, v2(j), v2(*point), true);
  else b2Body_ApplyLinearImpulseToCenter(bid, v2(j), true);
}
void cw_body2d_apply_torque(cw_body2d b, float t) {
  BODY_OR(b, );
  b2Body_ApplyTorque(bid, t, true);
}
void cw_body2d_apply_angular_impulse(cw_body2d b, float j) {
  BODY_OR(b, );
  b2Body_ApplyAngularImpulse(bid, j, true);
}
float cw_body2d_mass(cw_body2d b) {
  BODY_OR(b, 0);
  return b2Body_GetMass(bid);
}
bool cw_body2d_awake(cw_body2d b) {
  BODY_OR(b, false);
  return b2Body_IsAwake(bid);
}
void cw_body2d_set_awake(cw_body2d b, bool awake) {
  BODY_OR(b, );
  b2Body_SetAwake(bid, awake);
}
void cw_body2d_set_user(cw_body2d b, void *user) {
  cw__body2 *r = cw__b2(b);
  if (r) r->user = user;
}
void *cw_body2d_user(cw_body2d b) {
  cw__body2 *r = cw__b2(b);
  return r ? r->user : NULL;
}
void cw_body2d_bind_object(cw_body2d b, t3_object *obj) {
  cw__body2 *r = cw__b2(b);
  if (!r) return;
  r->obj = obj;
  r->xf = NULL;
  r->settled = false;
}
void cw_body2d_bind_xform(cw_body2d b, cw_xform2 *dst) {
  cw__body2 *r = cw__b2(b);
  if (!r) return;
  r->xf = dst;
  r->obj = NULL;
  r->settled = false;
}
cw_xform2 cw_body2d_interpolated(cw_body2d b, float alpha) {
  cw__body2 *r = cw__b2(b);
  if (!r) return cw_xform2_identity();
  return rec_at(world_of_body(b2LoadBodyId(b.id)), r, alpha);
}

/* ── shapes and chains ───────────────────────────────────────────────── */

cw_shape2d_def cw_shape2d_default_def(void) {
  return (cw_shape2d_def){ .material = cw_phys_material_default(), .density = 1, .mask = CW_PHYS_ALL_LAYERS,
                           .contact_events = true, .sensor_events = true };
}

static b2SurfaceMaterial surface(const cw_phys_material *m) {
  b2SurfaceMaterial s = b2DefaultSurfaceMaterial();
  s.friction = m->friction;
  s.restitution = m->restitution;
  s.rollingResistance = m->rolling_resistance;
  s.tangentSpeed = m->tangent_speed;
  s.userMaterialId = m->user_material;
  return s;
}

static b2ShapeDef shape_def(const cw_shape2d_def *def) {
  cw_shape2d_def d = def ? *def : cw_shape2d_default_def();
  b2ShapeDef sd = b2DefaultShapeDef();
  sd.material = surface(&d.material);
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

cw_shape2d cw_shape2d_box(cw_body2d b, const cw_shape2d_def *def, cw_vec2 half, cw_xform2 local) {
  BODY_OR(b, (cw_shape2d){ 0 });
  b2ShapeDef sd = shape_def(def);
  b2Polygon p = b2MakeOffsetBox(half.x, half.y, v2(local.p), r2(local.q));
  return (cw_shape2d){ b2StoreShapeId(b2CreatePolygonShape(bid, &sd, &p)) };
}
cw_shape2d cw_shape2d_circle(cw_body2d b, const cw_shape2d_def *def, cw_vec2 center, float radius) {
  BODY_OR(b, (cw_shape2d){ 0 });
  b2ShapeDef sd = shape_def(def);
  b2Circle c = { v2(center), radius };
  return (cw_shape2d){ b2StoreShapeId(b2CreateCircleShape(bid, &sd, &c)) };
}
cw_shape2d cw_shape2d_capsule(cw_body2d b, const cw_shape2d_def *def, cw_vec2 a, cw_vec2 c, float radius) {
  BODY_OR(b, (cw_shape2d){ 0 });
  b2ShapeDef sd = shape_def(def);
  b2Capsule cap = { v2(a), v2(c), radius };
  return (cw_shape2d){ b2StoreShapeId(b2CreateCapsuleShape(bid, &sd, &cap)) };
}
cw_shape2d cw_shape2d_polygon(cw_body2d b, const cw_shape2d_def *def, const cw_vec2 *points, int count, float radius) {
  BODY_OR(b, (cw_shape2d){ 0 });
  if (count > B2_MAX_POLYGON_VERTICES) {
    cw_error("physics2d: polygons take at most %d points (got %d); split it into several shapes",
             B2_MAX_POLYGON_VERTICES, count);
    return (cw_shape2d){ 0 };
  }
  b2Hull hull = b2ComputeHull((const b2Vec2 *)points, count);
  if (hull.count == 0) {
    cw_error("physics2d: polygon of %d points is degenerate", count);
    return (cw_shape2d){ 0 };
  }
  b2ShapeDef sd = shape_def(def);
  b2Polygon p = b2MakePolygon(&hull, radius);
  return (cw_shape2d){ b2StoreShapeId(b2CreatePolygonShape(bid, &sd, &p)) };
}
cw_shape2d cw_shape2d_segment(cw_body2d b, const cw_shape2d_def *def, cw_vec2 a, cw_vec2 c) {
  BODY_OR(b, (cw_shape2d){ 0 });
  b2ShapeDef sd = shape_def(def);
  b2Segment s = { v2(a), v2(c) };
  return (cw_shape2d){ b2StoreShapeId(b2CreateSegmentShape(bid, &sd, &s)) };
}

#define SHAPE_OR(s, ret) \
  b2ShapeId sid = b2LoadShapeId((s).id); \
  if (!(s).id || !b2Shape_IsValid(sid)) return ret;

void cw_shape2d_destroy(cw_shape2d s) {
  SHAPE_OR(s, );
  b2DestroyShape(sid, true);
}
bool cw_shape2d_valid(cw_shape2d s) { return s.id && b2Shape_IsValid(b2LoadShapeId(s.id)); }
cw_body2d cw_shape2d_body(cw_shape2d s) {
  SHAPE_OR(s, (cw_body2d){ 0 });
  return (cw_body2d){ b2StoreBodyId(b2Shape_GetBody(sid)) };
}
void *cw_shape2d_user(cw_shape2d s) {
  SHAPE_OR(s, NULL);
  return b2Shape_GetUserData(sid);
}
bool cw_shape2d_is_sensor(cw_shape2d s) {
  SHAPE_OR(s, false);
  return b2Shape_IsSensor(sid);
}
void cw_shape2d_set_material(cw_shape2d s, const cw_phys_material *m) {
  SHAPE_OR(s, );
  b2SurfaceMaterial sm = surface(m);
  b2Shape_SetSurfaceMaterial(sid, &sm);
}
cw_phys_material cw_shape2d_material(cw_shape2d s) {
  cw_phys_material m = { 0 };
  SHAPE_OR(s, m);
  b2SurfaceMaterial sm = b2Shape_GetSurfaceMaterial(sid);
  m.friction = sm.friction;
  m.restitution = sm.restitution;
  m.rolling_resistance = sm.rollingResistance;
  m.tangent_speed = sm.tangentSpeed;
  m.user_material = sm.userMaterialId;
  return m;
}
void cw_shape2d_set_layer(cw_shape2d s, int layer, uint32_t mask) {
  SHAPE_OR(s, );
  b2Filter f = b2Shape_GetFilter(sid);
  f.categoryBits = cw__shape_category(layer);
  f.maskBits = cw__shape_mask(mask, b2Shape_IsSensor(sid));
  b2Shape_SetFilter(sid, f);
}
b2ShapeId cw_shape2d_raw(cw_shape2d s) { return b2LoadShapeId(s.id); }

cw_chain2d cw_chain2d_create(cw_body2d b, const cw_shape2d_def *def, const cw_vec2 *points, int count, bool loop) {
  BODY_OR(b, (cw_chain2d){ 0 });
  if (count < (loop ? 3 : 4)) {
    cw_error("physics2d: a chain needs at least %d points (%s), got %d", loop ? 3 : 4, loop ? "loop" : "open", count);
    return (cw_chain2d){ 0 };
  }
  cw_shape2d_def d = def ? *def : cw_shape2d_default_def();
  b2SurfaceMaterial mat = surface(&d.material);
  b2ChainDef cd = b2DefaultChainDef();
  cd.points = (const b2Vec2 *)points;
  cd.count = count;
  cd.materials = &mat;
  cd.materialCount = 1;
  cd.filter.categoryBits = cw__shape_category(d.layer);
  cd.filter.maskBits = cw__shape_mask(d.mask, false);
  cd.filter.groupIndex = d.group;
  cd.isLoop = loop;
  cd.enableSensorEvents = d.sensor_events;
  cd.userData = d.user;
  return (cw_chain2d){ b2StoreChainId(b2CreateChain(bid, &cd)) };
}
void cw_chain2d_destroy(cw_chain2d c) {
  b2ChainId id = b2LoadChainId(c.id);
  if (c.id && b2Chain_IsValid(id)) b2DestroyChain(id);
}
b2ChainId cw_chain2d_raw(cw_chain2d c) { return b2LoadChainId(c.id); }

/* ── joints ──────────────────────────────────────────────────────────── */

cw_joint2d_def cw_joint2d_default_def(cw_joint_type type) {
  cw_joint2d_def d = { .type = type, .frame_a = cw_xform2_identity(), .frame_b = cw_xform2_identity() };
  switch (type) {
  case CW_JOINT_WELD: break;
  case CW_JOINT_MOTOR:
  case CW_JOINT_MOUSE: d.hertz = 5; d.damping_ratio = 0.7f; break;
  default: d.hertz = 2; d.damping_ratio = 0.5f; break;
  }
  return d;
}

static b2BodyId ground_body(cw__world2 *w) {
  if (B2_IS_NULL(w->ground) || !b2Body_IsValid(w->ground)) {
    b2BodyDef bd = b2DefaultBodyDef();
    bd.name = "cw_ground";
    w->ground = b2CreateBody(w->id, &bd);
  }
  return w->ground;
}

static b2Transform local_frame(b2BodyId body, b2Vec2 anchor, b2Rot q) {
  if (B2_IS_NULL(body) || !b2Body_IsValid(body)) return (b2Transform){ anchor, q };
  b2Transform t = b2Body_GetTransform(body);
  return (b2Transform){ b2InvTransformPoint(t, anchor), b2InvMulRot(t.q, q) };
}

void cw_joint2d_def_anchor(cw_joint2d_def *def, cw_vec2 world_anchor, cw_vec2 world_axis) {
  float l = sqrtf(world_axis.x * world_axis.x + world_axis.y * world_axis.y);
  b2Rot q = l > 0 ? (b2Rot){ world_axis.x / l, world_axis.y / l } : b2Rot_identity; /* x axis onto world_axis */
  def->frame_a = x2(local_frame(b2LoadBodyId(def->a.id), v2(world_anchor), q));
  def->frame_b = x2(local_frame(b2LoadBodyId(def->b.id), v2(world_anchor), q));
}

cw_joint2d cw_joint2d_create(cw_world2d wh, const cw_joint2d_def *d) {
  cw__world2 *w = cw__w2(wh);
  if (!w) {
    cw_warn("physics2d: joint on a stale or null world");
    return (cw_joint2d){ 0 };
  }
  if (!d) {
    cw_error("physics2d: cw_joint2d_create with a NULL def");
    return (cw_joint2d){ 0 };
  }
  if (d->type == CW_JOINT_SPHERICAL || d->type == CW_JOINT_PARALLEL) {
    cw_error("physics2d: spherical and parallel joints are 3D only (Box3D); use revolute in 2D");
    return (cw_joint2d){ 0 };
  }
  b2BodyId a = d->a.id ? b2LoadBodyId(d->a.id) : ground_body(w);
  b2BodyId b = b2LoadBodyId(d->b.id);
  if (!b2Body_IsValid(b) || (d->type != CW_JOINT_MOUSE && !b2Body_IsValid(a))) {
    cw_error("physics2d: joint needs valid bodies (a may be null for the world)");
    return (cw_joint2d){ 0 };
  }
  uint64_t rid;
  cw__joint2 *r = cw_pool_alloc(&w->joints, &rid);
  r->type = d->type;
  r->user = d->user;
  r->breakable = d->break_force > 0 || d->break_torque > 0;
  b2JointDef base = { 0 };
  base.bodyIdA = a;
  base.bodyIdB = b;
  base.localFrameA = bx2(d->frame_a);
  base.localFrameB = bx2(d->frame_b);
  base.collideConnected = d->collide_connected;
  base.userData = r;
  base.forceThreshold = d->break_force > 0 ? d->break_force : FLT_MAX;
  base.torqueThreshold = d->break_torque > 0 ? d->break_torque : FLT_MAX;
  b2JointId id = b2_nullJointId;
#define BASE(jd) \
  do { \
    b2JointDef keep = (jd).base; \
    (jd).base = base; \
    (jd).base.constraintHertz = keep.constraintHertz; \
    (jd).base.constraintDampingRatio = keep.constraintDampingRatio; \
    (jd).base.drawScale = keep.drawScale; \
  } while (0)
  switch (d->type) {
  case CW_JOINT_DISTANCE: {
    b2DistanceJointDef jd = b2DefaultDistanceJointDef();
    BASE(jd);
    float len = d->length;
    if (len <= 0) {
      b2Vec2 pa = b2TransformPoint(b2Body_GetTransform(a), base.localFrameA.p);
      b2Vec2 pb = b2TransformPoint(b2Body_GetTransform(b), base.localFrameB.p);
      len = b2Length(b2Sub(pb, pa));
    }
    jd.length = len;
    jd.enableSpring = d->spring;
    jd.hertz = d->hertz;
    jd.dampingRatio = d->damping_ratio;
    jd.enableLimit = d->limit;
    if (d->limit) {
      jd.minLength = d->lower;
      jd.maxLength = d->upper;
    }
    jd.enableMotor = d->motor;
    jd.motorSpeed = d->motor_speed;
    jd.maxMotorForce = d->max_motor;
    id = b2CreateDistanceJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_FILTER: {
    b2FilterJointDef jd = b2DefaultFilterJointDef();
    BASE(jd);
    id = b2CreateFilterJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_MOTOR: {
    b2MotorJointDef jd = b2DefaultMotorJointDef();
    BASE(jd);
    jd.linearHertz = jd.angularHertz = d->hertz;
    jd.linearDampingRatio = jd.angularDampingRatio = d->damping_ratio;
    float mf = d->max_force > 0 ? d->max_force : 1e9f;
    jd.maxSpringForce = mf;
    jd.maxSpringTorque = d->max_motor > 0 ? d->max_motor : mf;
    id = b2CreateMotorJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_PRISMATIC: {
    b2PrismaticJointDef jd = b2DefaultPrismaticJointDef();
    BASE(jd);
    jd.enableSpring = d->spring;
    jd.hertz = d->hertz;
    jd.dampingRatio = d->damping_ratio;
    jd.enableLimit = d->limit;
    jd.lowerTranslation = d->lower;
    jd.upperTranslation = d->upper;
    jd.enableMotor = d->motor;
    jd.motorSpeed = d->motor_speed;
    jd.maxMotorForce = d->max_motor;
    id = b2CreatePrismaticJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_REVOLUTE: {
    b2RevoluteJointDef jd = b2DefaultRevoluteJointDef();
    BASE(jd);
    jd.enableSpring = d->spring;
    jd.hertz = d->hertz;
    jd.dampingRatio = d->damping_ratio;
    jd.enableLimit = d->limit;
    jd.lowerAngle = d->lower;
    jd.upperAngle = d->upper;
    jd.enableMotor = d->motor;
    jd.motorSpeed = d->motor_speed;
    jd.maxMotorTorque = d->max_motor;
    id = b2CreateRevoluteJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_WELD: {
    b2WeldJointDef jd = b2DefaultWeldJointDef();
    BASE(jd);
    jd.linearHertz = jd.angularHertz = d->hertz;
    jd.linearDampingRatio = jd.angularDampingRatio = d->damping_ratio;
    id = b2CreateWeldJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_WHEEL: {
    b2WheelJointDef jd = b2DefaultWheelJointDef();
    BASE(jd);
    jd.enableSpring = d->spring;
    jd.hertz = d->hertz;
    jd.dampingRatio = d->damping_ratio;
    jd.enableLimit = d->limit;
    jd.lowerTranslation = d->lower;
    jd.upperTranslation = d->upper;
    jd.enableMotor = d->motor;
    jd.motorSpeed = d->motor_speed;
    jd.maxMotorTorque = d->max_motor;
    id = b2CreateWheelJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_MOUSE: {
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_kinematicBody;
    bd.position = v2(d->target);
    bd.enableSleep = false;
    bd.name = "cw_mouse";
    r->mouse_body = b2CreateBody(w->id, &bd);
    r->target = d->target;
    b2MotorJointDef jd = b2DefaultMotorJointDef();
    BASE(jd);
    jd.base.bodyIdA = r->mouse_body;
    jd.base.localFrameA = b2Transform_identity;
    jd.base.localFrameB = (b2Transform){ b2Body_GetLocalPoint(b, v2(d->target)), b2Rot_identity };
    jd.linearHertz = d->hertz;
    jd.linearDampingRatio = d->damping_ratio;
    jd.angularHertz = 0;
    b2MassData md = b2Body_GetMassData(b);
    float g = b2Length(b2World_GetGravity(w->id));
    float mg = md.mass * (g > 0 ? g : 10);
    jd.maxSpringForce = d->max_force > 0 ? d->max_force : 1000 * mg;
    jd.maxSpringTorque = 0;
    if (md.mass > 0) jd.maxVelocityTorque = 0.25f * sqrtf(md.rotationalInertia / md.mass) * mg; /* Box2D's sample */
    id = b2CreateMotorJoint(w->id, &jd);
    break;
  }
  default: break;
  }
#undef BASE
  if (B2_IS_NULL(id)) {
    if (B2_IS_NON_NULL(r->mouse_body)) b2DestroyBody(r->mouse_body);
    cw_pool_release(&w->joints, rid);
    cw_error("physics2d: joint type %d could not be created", (int)d->type);
    return (cw_joint2d){ 0 };
  }
  r->id = b2StoreJointId(id);
  return (cw_joint2d){ r->id };
}

#define JOINT_OR(j, ret) \
  b2JointId jid = b2LoadJointId((j).id); \
  if (!(j).id || !b2Joint_IsValid(jid)) return ret; \
  cw__joint2 *jr = b2Joint_GetUserData(jid); \
  (void)jr;

void cw_joint2d_destroy(cw_joint2d j) {
  JOINT_OR(j, );
  cw__joint2_destroy(world_of_body(b2Joint_GetBodyB(jid)), jid);
}
bool cw_joint2d_valid(cw_joint2d j) { return j.id && b2Joint_IsValid(b2LoadJointId(j.id)); }
b2JointId cw_joint2d_raw(cw_joint2d j) { return b2LoadJointId(j.id); }
cw_joint_type cw_joint2d_type(cw_joint2d j) {
  JOINT_OR(j, CW_JOINT_TYPE_COUNT);
  return jr ? jr->type : CW_JOINT_TYPE_COUNT;
}
void cw_joint2d_set_user(cw_joint2d j, void *user) {
  cw__joint2 *r = jrec(j);
  if (r) r->user = user;
}
void *cw_joint2d_user(cw_joint2d j) {
  cw__joint2 *r = jrec(j);
  return r ? r->user : NULL;
}
void cw_joint2d_set_motor(cw_joint2d j, bool on, float speed, float max) {
  JOINT_OR(j, );
  switch (b2Joint_GetType(jid)) {
  case b2_revoluteJoint: b2RevoluteJoint_EnableMotor(jid, on); b2RevoluteJoint_SetMotorSpeed(jid, speed); b2RevoluteJoint_SetMaxMotorTorque(jid, max); break;
  case b2_prismaticJoint: b2PrismaticJoint_EnableMotor(jid, on); b2PrismaticJoint_SetMotorSpeed(jid, speed); b2PrismaticJoint_SetMaxMotorForce(jid, max); break;
  case b2_wheelJoint: b2WheelJoint_EnableMotor(jid, on); b2WheelJoint_SetMotorSpeed(jid, speed); b2WheelJoint_SetMaxMotorTorque(jid, max); break;
  case b2_distanceJoint: b2DistanceJoint_EnableMotor(jid, on); b2DistanceJoint_SetMotorSpeed(jid, speed); b2DistanceJoint_SetMaxMotorForce(jid, max); break;
  default: break;
  }
  b2Joint_WakeBodies(jid);
}
void cw_joint2d_set_limits(cw_joint2d j, bool on, float lower, float upper) {
  JOINT_OR(j, );
  switch (b2Joint_GetType(jid)) {
  case b2_revoluteJoint: b2RevoluteJoint_EnableLimit(jid, on); b2RevoluteJoint_SetLimits(jid, lower, upper); break;
  case b2_prismaticJoint: b2PrismaticJoint_EnableLimit(jid, on); b2PrismaticJoint_SetLimits(jid, lower, upper); break;
  case b2_wheelJoint: b2WheelJoint_EnableLimit(jid, on); b2WheelJoint_SetLimits(jid, lower, upper); break;
  case b2_distanceJoint: b2DistanceJoint_EnableLimit(jid, on); b2DistanceJoint_SetLengthRange(jid, lower, upper); break;
  default: break;
  }
  b2Joint_WakeBodies(jid);
}
void cw_joint2d_set_spring(cw_joint2d j, bool on, float hertz, float damping) {
  JOINT_OR(j, );
  switch (b2Joint_GetType(jid)) {
  case b2_revoluteJoint: b2RevoluteJoint_EnableSpring(jid, on); b2RevoluteJoint_SetSpringHertz(jid, hertz); b2RevoluteJoint_SetSpringDampingRatio(jid, damping); break;
  case b2_prismaticJoint: b2PrismaticJoint_EnableSpring(jid, on); b2PrismaticJoint_SetSpringHertz(jid, hertz); b2PrismaticJoint_SetSpringDampingRatio(jid, damping); break;
  case b2_wheelJoint: b2WheelJoint_EnableSpring(jid, on); b2WheelJoint_SetSpringHertz(jid, hertz); b2WheelJoint_SetSpringDampingRatio(jid, damping); break;
  case b2_distanceJoint: b2DistanceJoint_EnableSpring(jid, on); b2DistanceJoint_SetSpringHertz(jid, hertz); b2DistanceJoint_SetSpringDampingRatio(jid, damping); break;
  case b2_weldJoint: b2WeldJoint_SetLinearHertz(jid, hertz); b2WeldJoint_SetAngularHertz(jid, hertz); b2WeldJoint_SetLinearDampingRatio(jid, damping); b2WeldJoint_SetAngularDampingRatio(jid, damping); break;
  case b2_motorJoint: b2MotorJoint_SetLinearHertz(jid, hertz); b2MotorJoint_SetLinearDampingRatio(jid, damping); if (!jr || jr->type != CW_JOINT_MOUSE) { b2MotorJoint_SetAngularHertz(jid, hertz); b2MotorJoint_SetAngularDampingRatio(jid, damping); } break;
  default: break;
  }
  b2Joint_WakeBodies(jid);
}
void cw_joint2d_set_target(cw_joint2d j, cw_vec2 target) {
  JOINT_OR(j, );
  if (jr && jr->type == CW_JOINT_MOUSE) jr->target = target;
}
float cw_joint2d_value(cw_joint2d j) {
  JOINT_OR(j, 0);
  switch (b2Joint_GetType(jid)) {
  case b2_revoluteJoint: return b2RevoluteJoint_GetAngle(jid);
  case b2_prismaticJoint: return b2PrismaticJoint_GetTranslation(jid);
  case b2_distanceJoint: return b2DistanceJoint_GetCurrentLength(jid);
  case b2_wheelJoint: /* measured spin (rad/s), like Box3D's b3WheelJoint_GetSpinSpeed */
    return b2Body_GetAngularVelocity(b2Joint_GetBodyB(jid)) - b2Body_GetAngularVelocity(b2Joint_GetBodyA(jid));
  default: return 0;
  }
}

/* ── debug draw ──────────────────────────────────────────────────────── */

static void line2(cw_phys_lines *l, b2Vec2 a, b2Vec2 b, uint32_t c) {
  float p[3] = { a.x, a.y, 0 }, q[3] = { b.x, b.y, 0 };
  cw_phys_lines_add(l, p, q, c);
}
static void circle2(cw_phys_lines *l, b2Vec2 c, float r, uint32_t color) {
  float cc[3] = { c.x, c.y, 0 }, ax[3] = { 1, 0, 0 }, ay[3] = { 0, 1, 0 };
  cw__lines_circle(l, cc, ax, ay, r, color, 20);
}
static void draw_polygon(const b2Vec2 *v, int n, b2HexColor color, void *ctx) {
  for (int i = 0; i < n; i++) line2(ctx, v[i], v[(i + 1) % n], (uint32_t)color);
}
static void draw_solid_polygon(b2Transform t, const b2Vec2 *v, int n, float radius, b2HexColor color, void *ctx) {
  (void)radius;
  for (int i = 0; i < n; i++) line2(ctx, b2TransformPoint(t, v[i]), b2TransformPoint(t, v[(i + 1) % n]), (uint32_t)color);
}
static void draw_circle(b2Vec2 c, float r, b2HexColor color, void *ctx) { circle2(ctx, c, r, (uint32_t)color); }
static void draw_solid_circle(b2Transform t, float r, b2HexColor color, void *ctx) {
  circle2(ctx, t.p, r, (uint32_t)color);
  line2(ctx, t.p, b2TransformPoint(t, (b2Vec2){ r, 0 }), (uint32_t)color); /* shows rotation */
}
static void draw_solid_capsule(b2Vec2 p1, b2Vec2 p2, float r, b2HexColor color, void *ctx) {
  circle2(ctx, p1, r, (uint32_t)color);
  circle2(ctx, p2, r, (uint32_t)color);
  b2Vec2 d = b2Sub(p2, p1);
  float l = b2Length(d);
  b2Vec2 n = l > 0 ? (b2Vec2){ -d.y / l * r, d.x / l * r } : (b2Vec2){ 0, r };
  line2(ctx, b2Add(p1, n), b2Add(p2, n), (uint32_t)color);
  line2(ctx, b2Sub(p1, n), b2Sub(p2, n), (uint32_t)color);
}
static void draw_line(b2Vec2 a, b2Vec2 b, b2HexColor color, void *ctx) { line2(ctx, a, b, (uint32_t)color); }
static void draw_transform(b2Transform t, void *ctx) {
  line2(ctx, t.p, b2TransformPoint(t, (b2Vec2){ 0.25f, 0 }), 0xff0000);
  line2(ctx, t.p, b2TransformPoint(t, (b2Vec2){ 0, 0.25f }), 0x00ff00);
}
static void draw_point(b2Vec2 p, float size, b2HexColor color, void *ctx) {
  (void)size;
  const float h = 0.05f;
  line2(ctx, (b2Vec2){ p.x - h, p.y }, (b2Vec2){ p.x + h, p.y }, (uint32_t)color);
  line2(ctx, (b2Vec2){ p.x, p.y - h }, (b2Vec2){ p.x, p.y + h }, (uint32_t)color);
}
static void draw_string(b2Vec2 p, const char *s, b2HexColor color, void *ctx) {
  (void)p;
  (void)s;
  (void)color;
  (void)ctx;
}

void cw_world2d_debug_draw(cw_world2d wh, cw_phys_lines *out, uint32_t flags) {
  cw__world2 *w = cw__w2(wh);
  if (!w) return;
  b2DebugDraw dd = b2DefaultDebugDraw();
  dd.DrawPolygonFcn = draw_polygon;
  dd.DrawSolidPolygonFcn = draw_solid_polygon;
  dd.DrawCircleFcn = draw_circle;
  dd.DrawSolidCircleFcn = draw_solid_circle;
  dd.DrawSolidCapsuleFcn = draw_solid_capsule;
  dd.DrawLineFcn = draw_line;
  dd.DrawTransformFcn = draw_transform;
  dd.DrawPointFcn = draw_point;
  dd.DrawStringFcn = draw_string;
  dd.drawingBounds = (b2AABB){ { -1e9f, -1e9f }, { 1e9f, 1e9f } };
  dd.drawShapes = flags & CW_PHYS_DRAW_SHAPES;
  dd.drawJoints = flags & CW_PHYS_DRAW_JOINTS;
  dd.drawBounds = flags & CW_PHYS_DRAW_BOUNDS;
  dd.drawContactPoints = flags & CW_PHYS_DRAW_CONTACTS;
  dd.drawContactNormals = flags & CW_PHYS_DRAW_CONTACTS;
  dd.drawMass = flags & CW_PHYS_DRAW_MASS;
  dd.context = out;
  b2World_Draw(w->id, &dd);
}
