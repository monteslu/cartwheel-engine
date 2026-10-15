/* Box3D wrapper: worlds, bodies, shapes, joints, events, sync, debug draw. */
#include <string.h>

#include "internal.h"
#include "physics3d_internal.h"

/* ── conversions ─────────────────────────────────────────────────────── */

cw_xform3 cw_xform3_lerp(cw_xform3 a, cw_xform3 b, float t) {
  cw_xform3 r;
  r.p = (t3_vec3){ a.p.x + (b.p.x - a.p.x) * t, a.p.y + (b.p.y - a.p.y) * t, a.p.z + (b.p.z - a.p.z) * t };
  float d = a.q.x * b.q.x + a.q.y * b.q.y + a.q.z * b.q.z + a.q.w * b.q.w;
  float s = d < 0 ? -1.0f : 1.0f; /* shortest arc */
  t3_quat q = { a.q.x + (s * b.q.x - a.q.x) * t, a.q.y + (s * b.q.y - a.q.y) * t, a.q.z + (s * b.q.z - a.q.z) * t,
                a.q.w + (s * b.q.w - a.q.w) * t };
  float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (l > 0) q = (t3_quat){ q.x / l, q.y / l, q.z / l, q.w / l };
  r.q = q;
  return r;
}

/* rotation taking unit vector a onto unit vector b (sqrt only) */
b3Quat cw__quat_between(b3Vec3 a, b3Vec3 b) {
  float d = b3Dot(a, b);
  if (d < -0.999999f) {
    b3Vec3 axis = b3Cross((b3Vec3){ 1, 0, 0 }, a);
    if (b3Dot(axis, axis) < 1e-6f) axis = b3Cross((b3Vec3){ 0, 1, 0 }, a);
    axis = b3Normalize(axis);
    return (b3Quat){ axis, 0 };
  }
  b3Vec3 c = b3Cross(a, b);
  return b3NormalizeQuat((b3Quat){ c, 1 + d });
}

/* ── registry ────────────────────────────────────────────────────────── */

cw__world3 *cw__w3(cw_world3d w) {
  if (!w.id) return NULL;
  b3WorldId id = b3LoadWorldId((uint32_t)w.id);
  if (!b3World_IsValid(id)) return NULL;
  return b3World_GetUserData(id);
}

static cw__world3 *world_of_body(b3BodyId id) { return b3World_GetUserData(b3Body_GetWorld(id)); }

cw__body3 *cw__b3(cw_body3d b) {
  if (!b.id) return NULL;
  b3BodyId id = b3LoadBodyId(b.id);
  if (!b3Body_IsValid(id)) return NULL;
  return b3Body_GetUserData(id);
}

static cw__joint3 *jrec(cw_joint3d j) {
  if (!j.id) return NULL;
  b3JointId id = b3LoadJointId(j.id);
  if (!b3Joint_IsValid(id)) return NULL;
  return b3Joint_GetUserData(id);
}

#define BODY_OR(b, ret) \
  b3BodyId bid = b3LoadBodyId((b).id); \
  if (!(b).id || !b3Body_IsValid(bid)) { \
    cw_warn("physics3d: %s on a stale or null body", __func__); \
    return ret; \
  }

/* ── worlds ──────────────────────────────────────────────────────────── */

cw_world3d_def cw_world3d_default_def(void) {
  return (cw_world3d_def){ .gravity = { 0, -10, 0 }, .substeps = 4, .hit_speed = 1, .restitution_threshold = 1,
                           .sleep = true, .continuous = true };
}

static void *create_debug_shape(const b3DebugShape *s, void *ctx);
static void destroy_debug_shape(void *shape, void *ctx);

cw_world3d cw_world3d_create(const cw_world3d_def *def) {
  cw_world3d_def d = def ? *def : cw_world3d_default_def();
  b3WorldDef wd = b3DefaultWorldDef();
  wd.gravity = v3(d.gravity);
  wd.hitEventThreshold = d.hit_speed;
  wd.restitutionThreshold = d.restitution_threshold;
  wd.enableSleep = d.sleep;
  wd.enableContinuous = d.continuous;
  wd.createDebugShape = create_debug_shape;
  wd.destroyDebugShape = destroy_debug_shape;
  cw_box3d_tasks(&wd, d.workers);
  cw__world3 *w = cw_calloc(1, sizeof *w, CW_MEM_PHYSICS);
  wd.userData = w;
  w->id = b3CreateWorld(&wd);
  w->tasks = wd.userTaskContext;
  if (B3_IS_NULL(w->id)) {
    cw_error("physics3d: b3CreateWorld failed (at most %d worlds at once)", b3GetMaxWorldCount());
    cw_box3d_tasks_release(w->tasks);
    cw_free(w);
    return (cw_world3d){ 0 };
  }
  w->substeps = d.substeps > 0 ? d.substeps : 4;
  w->hash = CW_HASH32_INIT;
  cw_pool_init(&w->bodies, sizeof(cw__body3), CW_MEM_PHYSICS);
  cw_pool_init(&w->joints, sizeof(cw__joint3), CW_MEM_PHYSICS);
  cw__layers_init(&w->layers, d.layers);
  return (cw_world3d){ b3StoreWorldId(w->id) };
}

void cw_world3d_destroy(cw_world3d wh) {
  cw__world3 *w = cw__w3(wh);
  if (!w) return;
  b3DestroyWorld(w->id); /* destroys bodies, shapes, joints; debug shapes via the callback */
  cw_box3d_tasks_release(w->tasks); /* only after the world: its steps used it */
  cw_pool_free_all(&w->bodies);
  cw_pool_free_all(&w->joints);
  cw_array_free(&w->events);
  cw_array_free(&w->breaking);
  cw__layers_free(&w->layers);
  cw_free(w);
}

bool cw_world3d_valid(cw_world3d w) { return cw__w3(w) != NULL; }
b3WorldId cw_world3d_raw(cw_world3d w) { return w.id ? b3LoadWorldId((uint32_t)w.id) : b3_nullWorldId; }

void cw_world3d_set_gravity(cw_world3d wh, t3_vec3 g) {
  cw__world3 *w = cw__w3(wh);
  if (w) b3World_SetGravity(w->id, v3(g));
}
t3_vec3 cw_world3d_gravity(cw_world3d wh) {
  cw__world3 *w = cw__w3(wh);
  return w ? t3v(b3World_GetGravity(w->id)) : (t3_vec3){ 0, 0, 0 };
}

int cw_world3d_layer(cw_world3d wh, const char *name) {
  cw__world3 *w = cw__w3(wh);
  int i = w ? cw__layers_find(&w->layers, name, strlen(name)) : -1;
  if (i < 0) cw_error("physics3d: no layer named '%s' (name it in cw_world3d_def.layers)", name);
  return i;
}
uint32_t cw_world3d_layer_bits(cw_world3d wh, const char *names) {
  cw__world3 *w = cw__w3(wh);
  return w ? cw__layers_bits(&w->layers, names) : 0;
}

/* events: one helper per kind, all appending to w->events */
static cw_body3d body_of_shape(b3ShapeId s) {
  return b3Shape_IsValid(s) ? (cw_body3d){ b3StoreBodyId(b3Shape_GetBody(s)) } : (cw_body3d){ 0 };
}
static cw_phys_event3d *push_event(cw__world3 *w, cw_phys_event_type type, b3ShapeId a, b3ShapeId b) {
  cw_array_reserve(&w->events, w->events.count + 1);
  cw_phys_event3d *e = &w->events.data[w->events.count++];
  memset(e, 0, sizeof *e);
  e->type = type;
  e->shape_a = (cw_shape3d){ b3StoreShapeId(a) };
  e->shape_b = (cw_shape3d){ b3StoreShapeId(b) };
  e->body_a = body_of_shape(a);
  e->body_b = body_of_shape(b);
  return e;
}

static void mouse_drive(cw__world3 *w, float dt) {
  for (int i = 0; i < w->joints.slot_count; i++) {
    cw__joint3 *j = cw_pool_at(&w->joints, i);
    if (!j || j->type != CW_JOINT_MOUSE || !b3Body_IsValid(j->mouse_body)) continue;
    b3WorldTransform t = { pos3(j->target), b3Quat_identity };
    b3Body_SetTargetTransform(j->mouse_body, t, dt, true);
  }
}

void cw_world3d_step(cw_world3d wh, float dt) {
  cw__world3 *w = cw__w3(wh);
  if (!w) {
    cw_warn("physics3d: step on a stale or null world");
    return;
  }
  mouse_drive(w, dt);
  cw_box3d_tasks_step(w->id, w->tasks);
  b3World_Step(w->id, dt, w->substeps);
  w->steps++;
  if (!w->by_tick) w->span++;
  cw_array_clear(&w->events);
  cw_array_clear(&w->breaking);
  uint32_t h = cw__fold(w->hash, &w->steps, sizeof w->steps);

  /* moved bodies: interpolation pairs + hash (library order) */
  b3BodyEvents be = b3World_GetBodyEvents(w->id);
  for (int i = 0; i < be.moveCount; i++) {
    const b3BodyMoveEvent *m = &be.moveEvents[i];
    uint64_t id = b3StoreBodyId(m->bodyId);
    float tf[7] = { (float)m->transform.p.x, (float)m->transform.p.y, (float)m->transform.p.z, m->transform.q.v.x,
                    m->transform.q.v.y, m->transform.q.v.z, m->transform.q.s };
    h = cw__fold(h, &id, sizeof id);
    h = cw__fold(h, tf, sizeof tf);
    cw__body3 *r = m->userData;
    if (!r) continue;
    if (r->moved_span != w->span) r->prev = r->cur; /* the pose the span started from */
    r->cur = (cw_xform3){ { tf[0], tf[1], tf[2] }, { tf[3], tf[4], tf[5], tf[6] } };
    r->moved_span = w->span;
    r->settled = false;
  }

  b3ContactEvents ce = b3World_GetContactEvents(w->id);
  for (int i = 0; i < ce.beginCount; i++)
    push_event(w, CW_PHYS_CONTACT_BEGIN, ce.beginEvents[i].shapeIdA, ce.beginEvents[i].shapeIdB);
  for (int i = 0; i < ce.endCount; i++)
    push_event(w, CW_PHYS_CONTACT_END, ce.endEvents[i].shapeIdA, ce.endEvents[i].shapeIdB);
  for (int i = 0; i < ce.hitCount; i++) {
    const b3ContactHitEvent *hv = &ce.hitEvents[i];
    cw_phys_event3d *e = push_event(w, CW_PHYS_HIT, hv->shapeIdA, hv->shapeIdB);
    e->point = (t3_vec3){ (float)hv->point.x, (float)hv->point.y, (float)hv->point.z };
    e->normal = t3v(hv->normal);
    e->speed = hv->approachSpeed;
    e->material_a = hv->userMaterialIdA;
    e->material_b = hv->userMaterialIdB;
  }
  b3SensorEvents se = b3World_GetSensorEvents(w->id);
  for (int i = 0; i < se.beginCount; i++)
    push_event(w, CW_PHYS_SENSOR_ENTER, se.beginEvents[i].sensorShapeId, se.beginEvents[i].visitorShapeId);
  for (int i = 0; i < se.endCount; i++)
    push_event(w, CW_PHYS_SENSOR_EXIT, se.endEvents[i].sensorShapeId, se.endEvents[i].visitorShapeId);
  b3JointEvents je = b3World_GetJointEvents(w->id);
  for (int i = 0; i < je.count; i++) {
    cw__joint3 *j = je.jointEvents[i].userData;
    if (!j || !j->breakable || !b3Joint_IsValid(je.jointEvents[i].jointId)) continue;
    cw_phys_event3d *e = push_event(w, CW_PHYS_JOINT_BREAK, b3_nullShapeId, b3_nullShapeId);
    b3JointId jid = je.jointEvents[i].jointId;
    e->joint = (cw_joint3d){ b3StoreJointId(jid) };
    e->body_a = (cw_body3d){ b3StoreBodyId(b3Joint_GetBodyA(jid)) };
    e->body_b = (cw_body3d){ b3StoreBodyId(b3Joint_GetBodyB(jid)) };
    e->user = j->user;
    cw_array_push(&w->breaking, jid);
  }
  for (int i = 0; i < w->breaking.count; i++) cw__joint3_destroy(w, w->breaking.data[i]);

  /* the event list into the hash: kinds, shapes, hit data */
  for (int i = 0; i < w->events.count; i++) {
    const cw_phys_event3d *e = &w->events.data[i];
    uint64_t k[4] = { (uint64_t)e->type, e->shape_a.id, e->shape_b.id, e->joint.id };
    h = cw__fold(h, k, sizeof k);
    if (e->type == CW_PHYS_HIT) {
      float f[7] = { e->point.x, e->point.y, e->point.z, e->normal.x, e->normal.y, e->normal.z, e->speed };
      h = cw__fold(h, f, sizeof f);
    }
  }
  w->hash = h;
}

const cw_phys_event3d *cw_world3d_events(cw_world3d wh, int *count) {
  cw__world3 *w = cw__w3(wh);
  *count = w ? w->events.count : 0;
  return w ? w->events.data : NULL;
}

cw_phys_stats cw_world3d_stats(cw_world3d wh) {
  cw_phys_stats s = { 0 };
  cw__world3 *w = cw__w3(wh);
  if (!w) return s;
  b3Counters c = b3World_GetCounters(w->id);
  s.bodies = c.bodyCount;
  s.shapes = c.shapeCount;
  s.contacts = c.contactCount;
  s.joints = c.jointCount;
  s.awake_bodies = b3World_GetAwakeBodyCount(w->id);
  s.events = w->events.count;
  s.steps = w->steps;
  s.hash = w->hash;
  s.step_ms = b3World_GetProfile(w->id).step;
  return s;
}

void cw_world3d_tick_begin(cw_world3d wh) {
  cw__world3 *w = cw__w3(wh);
  if (!w) return;
  w->by_tick = true;
  w->span++;
}

/* ── sync ────────────────────────────────────────────────────────────── */

static cw_xform3 rec_at(const cw__world3 *w, const cw__body3 *r, float alpha) {
  if (r->interpolate && r->moved_span == w->span) return cw_xform3_lerp(r->prev, r->cur, alpha);
  return r->cur;
}

static void write_target(cw__body3 *r, cw_xform3 x) {
  if (r->obj) {
    t3_object_set_position(r->obj, x.p.x, x.p.y, x.p.z);
    t3_object_set_quaternion(r->obj, x.q);
  } else if (r->xf) {
    *r->xf = x;
  }
}

void cw_world3d_sync(cw_world3d wh, float alpha) {
  cw__world3 *w = cw__w3(wh);
  if (!w) return;
  for (int i = 0; i < w->bodies.slot_count; i++) {
    cw__body3 *r = cw_pool_at(&w->bodies, i);
    if (!r || (!r->obj && !r->xf)) continue;
    if (r->moved_span == w->span) write_target(r, rec_at(w, r, alpha));
    else if (!r->settled) {
      write_target(r, r->cur);
      r->settled = true;
    }
  }
}

/* ── bodies ──────────────────────────────────────────────────────────── */

cw_body3d_def cw_body3d_default_def(void) {
  return (cw_body3d_def){ .type = CW_BODY_DYNAMIC, .rotation = { 0, 0, 0, 1 }, .gravity_scale = 1, .awake = true,
                          .sleep = true, .interpolate = true };
}

cw_body3d cw_body3d_create(cw_world3d wh, const cw_body3d_def *def) {
  cw__world3 *w = cw__w3(wh);
  if (!w) {
    cw_warn("physics3d: body on a stale or null world");
    return (cw_body3d){ 0 };
  }
  cw_body3d_def d = def ? *def : cw_body3d_default_def();
  uint64_t rid;
  cw__body3 *r = cw_pool_alloc(&w->bodies, &rid);
  b3BodyDef bd = b3DefaultBodyDef();
  bd.type = (b3BodyType)d.type;
  bd.position = pos3(d.position);
  bd.rotation = q3(d.rotation);
  bd.linearVelocity = v3(d.linear_velocity);
  bd.angularVelocity = v3(d.angular_velocity);
  bd.linearDamping = d.linear_damping;
  bd.angularDamping = d.angular_damping;
  bd.gravityScale = d.gravity_scale;
  bd.motionLocks.angularX = bd.motionLocks.angularY = bd.motionLocks.angularZ = d.lock_rotation;
  bd.isBullet = d.bullet;
  bd.isAwake = d.awake;
  bd.enableSleep = d.sleep;
  bd.name = d.name;
  bd.userData = r;
  b3BodyId id = b3CreateBody(w->id, &bd);
  r->id = b3StoreBodyId(id);
  r->user = d.user;
  r->interpolate = d.interpolate;
  b3WorldTransform t = b3Body_GetTransform(id);
  r->cur = r->prev = x3(t);
  r->moved_span = -1;
  return (cw_body3d){ r->id };
}

void cw__joint3_destroy(cw__world3 *w, b3JointId jid) {
  cw__joint3 *j = b3Joint_GetUserData(jid);
  b3BodyId mouse = j ? j->mouse_body : b3_nullBodyId;
  b3DestroyJoint(jid, true);
  if (j) cw_pool_release(&w->joints, cw_pool_id_of(&w->joints, j));
  if (B3_IS_NON_NULL(mouse) && b3Body_IsValid(mouse)) b3DestroyBody(mouse);
}

void cw_body3d_destroy(cw_body3d b) {
  BODY_OR(b, );
  cw__world3 *w = world_of_body(bid);
  /* joints first: their records (and mouse bodies) are ours */
  b3JointId js[64];
  for (;;) {
    int n = b3Body_GetJoints(bid, js, 64);
    if (!n) break;
    for (int i = 0; i < n; i++) cw__joint3_destroy(w, js[i]);
  }
  cw__body3 *r = b3Body_GetUserData(bid);
  b3DestroyBody(bid);
  if (r) cw_pool_release(&w->bodies, cw_pool_id_of(&w->bodies, r));
}

bool cw_body3d_valid(cw_body3d b) { return b.id && b3Body_IsValid(b3LoadBodyId(b.id)); }
b3BodyId cw_body3d_raw(cw_body3d b) { return b3LoadBodyId(b.id); }
cw_world3d cw_body3d_world(cw_body3d b) {
  BODY_OR(b, (cw_world3d){ 0 });
  return (cw_world3d){ b3StoreWorldId(b3Body_GetWorld(bid)) };
}
cw_body_type cw_body3d_type(cw_body3d b) {
  BODY_OR(b, CW_BODY_STATIC);
  return (cw_body_type)b3Body_GetType(bid);
}
void cw_body3d_set_type(cw_body3d b, cw_body_type t) {
  BODY_OR(b, );
  b3Body_SetType(bid, (b3BodyType)t);
}
cw_xform3 cw_body3d_transform(cw_body3d b) {
  BODY_OR(b, cw_xform3_identity());
  return x3(b3Body_GetTransform(bid));
}
void cw_body3d_set_transform(cw_body3d b, cw_xform3 x) {
  BODY_OR(b, );
  b3Body_SetTransform(bid, pos3(x.p), q3(x.q));
  b3Body_SetAwake(bid, true);
  cw__body3 *r = b3Body_GetUserData(bid);
  if (r) {
    r->prev = r->cur = x3(b3Body_GetTransform(bid));
    r->settled = false;
  }
}
void cw_body3d_set_target(cw_body3d b, cw_xform3 target, float dt) {
  BODY_OR(b, );
  b3WorldTransform t = { pos3(target.p), q3(target.q) };
  b3Body_SetTargetTransform(bid, t, dt, true);
}
t3_vec3 cw_body3d_velocity(cw_body3d b) {
  BODY_OR(b, (t3_vec3){ 0 });
  return t3v(b3Body_GetLinearVelocity(bid));
}
void cw_body3d_set_velocity(cw_body3d b, t3_vec3 v) {
  BODY_OR(b, );
  b3Body_SetLinearVelocity(bid, v3(v));
}
t3_vec3 cw_body3d_angular_velocity(cw_body3d b) {
  BODY_OR(b, (t3_vec3){ 0 });
  return t3v(b3Body_GetAngularVelocity(bid));
}
void cw_body3d_set_angular_velocity(cw_body3d b, t3_vec3 v) {
  BODY_OR(b, );
  b3Body_SetAngularVelocity(bid, v3(v));
}
void cw_body3d_apply_force(cw_body3d b, t3_vec3 f, const t3_vec3 *point) {
  BODY_OR(b, );
  if (point) b3Body_ApplyForce(bid, v3(f), pos3(*point), true);
  else b3Body_ApplyForceToCenter(bid, v3(f), true);
}
void cw_body3d_apply_impulse(cw_body3d b, t3_vec3 j, const t3_vec3 *point) {
  BODY_OR(b, );
  if (point) b3Body_ApplyLinearImpulse(bid, v3(j), pos3(*point), true);
  else b3Body_ApplyLinearImpulseToCenter(bid, v3(j), true);
}
void cw_body3d_apply_torque(cw_body3d b, t3_vec3 t) {
  BODY_OR(b, );
  b3Body_ApplyTorque(bid, v3(t), true);
}
void cw_body3d_apply_angular_impulse(cw_body3d b, t3_vec3 j) {
  BODY_OR(b, );
  b3Body_ApplyAngularImpulse(bid, v3(j), true);
}
float cw_body3d_mass(cw_body3d b) {
  BODY_OR(b, 0);
  return b3Body_GetMass(bid);
}
bool cw_body3d_awake(cw_body3d b) {
  BODY_OR(b, false);
  return b3Body_IsAwake(bid);
}
void cw_body3d_set_awake(cw_body3d b, bool awake) {
  BODY_OR(b, );
  b3Body_SetAwake(bid, awake);
}
void cw_body3d_set_user(cw_body3d b, void *user) {
  cw__body3 *r = cw__b3(b);
  if (r) r->user = user;
}
void *cw_body3d_user(cw_body3d b) {
  cw__body3 *r = cw__b3(b);
  return r ? r->user : NULL;
}
void cw_body3d_bind_object(cw_body3d b, t3_object *obj) {
  cw__body3 *r = cw__b3(b);
  if (!r) return;
  r->obj = obj;
  r->xf = NULL;
  r->settled = false;
}
void cw_body3d_bind_xform(cw_body3d b, cw_xform3 *dst) {
  cw__body3 *r = cw__b3(b);
  if (!r) return;
  r->xf = dst;
  r->obj = NULL;
  r->settled = false;
}
cw_xform3 cw_body3d_interpolated(cw_body3d b, float alpha) {
  cw__body3 *r = cw__b3(b);
  if (!r) return cw_xform3_identity();
  return rec_at(world_of_body(b3LoadBodyId(b.id)), r, alpha);
}

int cw_body3d_contacts(cw_body3d b, cw_contact3d *out, int cap) {
  if (cap <= 0) return 0;
  BODY_OR(b, 0);
  enum { MAX = 64 };
  b3ContactData cd[MAX];
  int n = b3Body_GetContactData(bid, cd, MAX), count = 0;
  for (int i = 0; i < n; i++) {
    bool mine_a = B3_ID_EQUALS(b3Shape_GetBody(cd[i].shapeIdA), bid);
    b3ShapeId mine = mine_a ? cd[i].shapeIdA : cd[i].shapeIdB, theirs = mine_a ? cd[i].shapeIdB : cd[i].shapeIdA;
    b3BodyId ob = b3Shape_GetBody(theirs);
    b3Pos com_a = b3Body_GetWorldCenterOfMass(b3Shape_GetBody(cd[i].shapeIdA));
    cw_contact3d c = { { b3StoreShapeId(mine) }, { b3StoreShapeId(theirs) }, { b3StoreBodyId(ob) } };
    float wsum = 0, impulse = 0, approach = 0;
    b3Vec3 at = { 0, 0, 0 }, mid = { 0, 0, 0 }, nsum = { 0, 0, 0 };
    int np = 0;
    for (int m = 0; m < cd[i].manifoldCount; m++) {
      const b3Manifold *mf = &cd[i].manifolds[m];
      for (int k = 0; k < mf->pointCount; k++) {
        const b3ManifoldPoint *mp = &mf->points[k];
        float j = mp->totalNormalImpulse;
        at = b3MulAdd(at, j, mp->anchorA);
        mid = b3Add(mid, mp->anchorA);
        nsum = b3MulAdd(nsum, j > 0 ? j : 1e-6f, mf->normal);
        wsum += j;
        if (-mp->normalVelocity > approach) approach = -mp->normalVelocity;
        np++;
      }
    }
    if (!np) continue;
    impulse = wsum;
    b3Vec3 rel = wsum > 0 ? b3MulSV(1 / wsum, at) : b3MulSV(1.0f / np, mid);
    b3Pos p = { com_a.x + rel.x, com_a.y + rel.y, com_a.z + rel.z };
    float nl = sqrtf(b3Dot(nsum, nsum));
    b3Vec3 nrm = nl > 0 ? b3MulSV(1 / nl, nsum) : (b3Vec3){ 0, 1, 0 };
    if (!mine_a) nrm = b3Neg(nrm);
    b3Vec3 dv = b3Sub(b3Body_GetWorldPointVelocity(ob, p), b3Body_GetWorldPointVelocity(bid, p));
    dv = b3Sub(dv, b3MulSV(b3Dot(dv, nrm), nrm));
    c.point = t3p(p);
    c.normal = t3v(nrm);
    c.impulse = impulse;
    c.approach = approach;
    c.slide = t3v(dv);
    /* insert sorted by (shape, other shape): the library's list order is not part of the contract */
    if (count == cap && !(c.shape.id < out[count - 1].shape.id ||
                          (c.shape.id == out[count - 1].shape.id && c.other_shape.id < out[count - 1].other_shape.id)))
      continue;
    int at_i = count < cap ? count : cap - 1;
    while (at_i > 0 && (c.shape.id < out[at_i - 1].shape.id ||
                        (c.shape.id == out[at_i - 1].shape.id && c.other_shape.id < out[at_i - 1].other_shape.id))) {
      out[at_i] = out[at_i - 1];
      at_i--;
    }
    out[at_i] = c;
    if (count < cap) count++;
  }
  return count;
}

/* ── shapes ──────────────────────────────────────────────────────────── */

cw_shape3d_def cw_shape3d_default_def(void) {
  return (cw_shape3d_def){ .material = cw_phys_material_default(), .density = 1, .mask = CW_PHYS_ALL_LAYERS,
                           .contact_events = true, .sensor_events = true };
}

static b3SurfaceMaterial surface(const cw_phys_material *m) {
  b3SurfaceMaterial s = b3DefaultSurfaceMaterial();
  s.friction = m->friction;
  s.restitution = m->restitution;
  s.rollingResistance = m->rolling_resistance;
  s.tangentVelocity = (b3Vec3){ m->tangent_velocity[0], m->tangent_velocity[1], m->tangent_velocity[2] };
  s.userMaterialId = m->user_material;
  return s;
}

static b3ShapeDef shape_def(const cw_shape3d_def *def) {
  cw_shape3d_def d = def ? *def : cw_shape3d_default_def();
  b3ShapeDef sd = b3DefaultShapeDef();
  sd.baseMaterial = surface(&d.material);
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

#define SHAPE_BODY(b, ret) \
  b3BodyId bid = b3LoadBodyId((b).id); \
  if (!(b).id || !b3Body_IsValid(bid)) { \
    cw_warn("physics3d: %s on a stale or null body", __func__); \
    return ret; \
  }

cw_shape3d cw_shape3d_box(cw_body3d b, const cw_shape3d_def *def, t3_vec3 half, cw_xform3 local) {
  SHAPE_BODY(b, (cw_shape3d){ 0 });
  b3ShapeDef sd = shape_def(def);
  b3Transform t = { v3(local.p), q3(local.q) };
  b3BoxHull box = b3MakeTransformedBoxHull(half.x, half.y, half.z, t);
  return (cw_shape3d){ b3StoreShapeId(b3CreateHullShape(bid, &sd, &box.base)) };
}
cw_shape3d cw_shape3d_sphere(cw_body3d b, const cw_shape3d_def *def, t3_vec3 center, float radius) {
  SHAPE_BODY(b, (cw_shape3d){ 0 });
  b3ShapeDef sd = shape_def(def);
  b3Sphere s = { v3(center), radius };
  return (cw_shape3d){ b3StoreShapeId(b3CreateSphereShape(bid, &sd, &s)) };
}
cw_shape3d cw_shape3d_capsule(cw_body3d b, const cw_shape3d_def *def, t3_vec3 a, t3_vec3 c, float radius) {
  SHAPE_BODY(b, (cw_shape3d){ 0 });
  b3ShapeDef sd = shape_def(def);
  b3Capsule cap = { v3(a), v3(c), radius };
  return (cw_shape3d){ b3StoreShapeId(b3CreateCapsuleShape(bid, &sd, &cap)) };
}
cw_shape3d cw_shape3d_hull(cw_body3d b, const cw_shape3d_def *def, const t3_vec3 *points, int count) {
  SHAPE_BODY(b, (cw_shape3d){ 0 });
  CW_STATIC_ASSERT(sizeof(t3_vec3) == sizeof(b3Vec3), "t3_vec3 and b3Vec3 share a layout");
  /* Box3D keeps at most maxVertexCount vertices, and a hull must stay under
   * 255 half edges (a triangulated hull of V vertices has 6V - 12): dense
   * point sets (a mesh's vertices) are simplified to 40 */
  b3HullData *hull = b3CreateHull((const b3Vec3 *)points, count, count < 40 ? count : 40);
  if (!hull) {
    cw_error("physics3d: hull of %d points is degenerate (needs 4+ points spanning a volume)", count);
    return (cw_shape3d){ 0 };
  }
  b3ShapeDef sd = shape_def(def);
  b3ShapeId s = b3CreateHullShape(bid, &sd, hull); /* cloned into the world's hull database */
  b3DestroyHull(hull);
  return (cw_shape3d){ b3StoreShapeId(s) };
}

/* collision meshes: a global pool of b3MeshData pointers */
static cw_pool g_meshes;
static b3MeshData *mesh_get(cw_mesh3d m) {
  b3MeshData **p = g_meshes.elem_size ? cw_pool_get(&g_meshes, m.id) : NULL;
  return p ? *p : NULL;
}

cw_mesh3d cw_mesh3d_create(const t3_vec3 *vertices, int vertex_count, const int32_t *indices, int triangle_count) {
  if (!g_meshes.elem_size) cw_pool_init(&g_meshes, sizeof(b3MeshData *), CW_MEM_PHYSICS);
  b3MeshDef md = { 0 };
  md.vertices = (b3Vec3 *)vertices;
  md.indices = (int32_t *)indices;
  md.vertexCount = vertex_count;
  md.triangleCount = triangle_count;
  md.identifyEdges = true;
  b3MeshData *data = b3CreateMesh(&md, NULL, 0);
  if (!data) {
    cw_error("physics3d: mesh of %d triangles failed to build", triangle_count);
    return (cw_mesh3d){ 0 };
  }
  uint64_t id;
  b3MeshData **slot = cw_pool_alloc(&g_meshes, &id);
  *slot = data;
  return (cw_mesh3d){ id };
}
void cw_mesh3d_destroy(cw_mesh3d m) {
  b3MeshData *d = mesh_get(m);
  if (!d) return;
  b3DestroyMesh(d);
  cw_pool_release(&g_meshes, m.id);
}
cw_shape3d cw_shape3d_mesh(cw_body3d b, const cw_shape3d_def *def, cw_mesh3d mesh, t3_vec3 scale) {
  SHAPE_BODY(b, (cw_shape3d){ 0 });
  b3MeshData *d = mesh_get(mesh);
  if (!d) {
    cw_warn("physics3d: mesh shape from a stale or null mesh");
    return (cw_shape3d){ 0 };
  }
  if (b3Body_GetType(bid) != b3_staticBody) {
    cw_error("physics3d: mesh shapes collide only on static bodies; use hulls on dynamic ones");
    return (cw_shape3d){ 0 };
  }
  b3ShapeDef sd = shape_def(def);
  return (cw_shape3d){ b3StoreShapeId(b3CreateMeshShape(bid, &sd, d, v3(scale))) };
}

/* height fields: a global pool of b3HeightFieldData pointers */
static cw_pool g_fields;
static b3HeightFieldData *field_get(cw_heightfield3d h) {
  b3HeightFieldData **p = g_fields.elem_size ? cw_pool_get(&g_fields, h.id) : NULL;
  return p ? *p : NULL;
}

b3HeightFieldData *cw__heightfield3d_build(const uint16_t *q, int nx, int nz, t3_vec3 scale, const uint8_t *materials) {
  if (!q || nx < 2 || nz < 2 || !(scale.x > 0) || !(scale.y > 0) || !(scale.z > 0)) {
    cw_error("physics3d: height field needs 2 x 2 samples or more and a positive scale (got %d x %d)", nx, nz);
    return NULL;
  }
  /* Box3D quantizes floats between a global min and max into uint16 steps
   * of (max - min) / 65535; with the range 0..65535 a step is exactly 1, so
   * the steps given are stored as they are and scale.y makes them metres */
  float *h = cw_alloc(sizeof(float) * (size_t)nx * (size_t)nz, CW_MEM_PHYSICS);
  for (int i = 0; i < nx * nz; i++) h[i] = (float)q[i];
  b3HeightFieldDef d = { 0 };
  d.heights = h;
  d.materialIndices = (uint8_t *)materials;
  d.scale = v3(scale);
  d.countX = nx;
  d.countZ = nz;
  d.globalMinimumHeight = 0;
  d.globalMaximumHeight = 65535;
  b3HeightFieldData *data = b3CreateHeightField(&d);
  cw_free(h);
  if (!data) cw_error("physics3d: height field of %d x %d samples failed to build", nx, nz);
  return data;
}
cw_heightfield3d cw__heightfield3d_adopt(b3HeightFieldData *data) {
  if (!data) return (cw_heightfield3d){ 0 };
  if (!g_fields.elem_size) cw_pool_init(&g_fields, sizeof(b3HeightFieldData *), CW_MEM_PHYSICS);
  uint64_t id;
  b3HeightFieldData **slot = cw_pool_alloc(&g_fields, &id);
  *slot = data;
  return (cw_heightfield3d){ id };
}
cw_heightfield3d cw_heightfield3d_create(const uint16_t *q, int nx, int nz, t3_vec3 scale, const uint8_t *materials) {
  return cw__heightfield3d_adopt(cw__heightfield3d_build(q, nx, nz, scale, materials));
}
void cw_heightfield3d_destroy(cw_heightfield3d hf) {
  b3HeightFieldData *d = field_get(hf);
  if (!d) return;
  b3DestroyHeightField(d);
  cw_pool_release(&g_fields, hf.id);
}
const b3HeightFieldData *cw_heightfield3d_raw(cw_heightfield3d hf) { return field_get(hf); }
cw_shape3d cw_shape3d_heightfield(cw_body3d b, const cw_shape3d_def *def, cw_heightfield3d field) {
  SHAPE_BODY(b, (cw_shape3d){ 0 });
  b3HeightFieldData *d = field_get(field);
  if (!d) {
    cw_warn("physics3d: height-field shape from a stale or null field");
    return (cw_shape3d){ 0 };
  }
  if (b3Body_GetType(bid) != b3_staticBody) {
    cw_error("physics3d: height-field shapes collide only on static bodies");
    return (cw_shape3d){ 0 };
  }
  b3ShapeDef sd = shape_def(def);
  return (cw_shape3d){ b3StoreShapeId(b3CreateHeightFieldShape(bid, &sd, d)) };
}

#define SHAPE_OR(s, ret) \
  b3ShapeId sid = b3LoadShapeId((s).id); \
  if (!(s).id || !b3Shape_IsValid(sid)) return ret;

void cw_shape3d_destroy(cw_shape3d s) {
  SHAPE_OR(s, );
  b3DestroyShape(sid, true);
}
bool cw_shape3d_valid(cw_shape3d s) { return s.id && b3Shape_IsValid(b3LoadShapeId(s.id)); }
cw_body3d cw_shape3d_body(cw_shape3d s) {
  SHAPE_OR(s, (cw_body3d){ 0 });
  return (cw_body3d){ b3StoreBodyId(b3Shape_GetBody(sid)) };
}
void *cw_shape3d_user(cw_shape3d s) {
  SHAPE_OR(s, NULL);
  return b3Shape_GetUserData(sid);
}
bool cw_shape3d_is_sensor(cw_shape3d s) {
  SHAPE_OR(s, false);
  return b3Shape_IsSensor(sid);
}
void cw_shape3d_set_material(cw_shape3d s, const cw_phys_material *m) {
  SHAPE_OR(s, );
  b3Shape_SetSurfaceMaterial(sid, surface(m));
}
cw_phys_material cw_shape3d_material(cw_shape3d s) {
  cw_phys_material m = { 0 };
  SHAPE_OR(s, m);
  b3SurfaceMaterial sm = b3Shape_GetSurfaceMaterial(sid);
  m.friction = sm.friction;
  m.restitution = sm.restitution;
  m.rolling_resistance = sm.rollingResistance;
  m.tangent_velocity[0] = sm.tangentVelocity.x;
  m.tangent_velocity[1] = sm.tangentVelocity.y;
  m.tangent_velocity[2] = sm.tangentVelocity.z;
  m.user_material = sm.userMaterialId;
  return m;
}
void cw_shape3d_set_layer(cw_shape3d s, int layer, uint32_t mask) {
  SHAPE_OR(s, );
  b3Filter f = b3Shape_GetFilter(sid);
  f.categoryBits = cw__shape_category(layer);
  f.maskBits = cw__shape_mask(mask, b3Shape_IsSensor(sid));
  b3Shape_SetFilter(sid, f, true);
}
b3ShapeId cw_shape3d_raw(cw_shape3d s) { return b3LoadShapeId(s.id); }

/* ── joints ──────────────────────────────────────────────────────────── */

cw_joint3d_def cw_joint3d_default_def(cw_joint_type type) {
  cw_joint3d_def d = { .type = type, .frame_a = cw_xform3_identity(), .frame_b = cw_xform3_identity() };
  switch (type) {
  case CW_JOINT_WELD: break; /* hertz 0 = rigid */
  case CW_JOINT_MOTOR:
  case CW_JOINT_MOUSE: d.hertz = 5; d.damping_ratio = 0.7f; break;
  case CW_JOINT_PARALLEL: d.hertz = 2; d.damping_ratio = 1; d.max_motor = 1e9f; break;
  default: d.hertz = 2; d.damping_ratio = 0.5f; break;
  }
  return d;
}

static b3BodyId ground_body(cw__world3 *w) {
  if (B3_IS_NULL(w->ground) || !b3Body_IsValid(w->ground)) {
    b3BodyDef bd = b3DefaultBodyDef();
    bd.name = "cw_ground";
    w->ground = b3CreateBody(w->id, &bd);
  }
  return w->ground;
}

static b3Vec3 main_axis(cw_joint_type t) {
  switch (t) {
  case CW_JOINT_PRISMATIC:
  case CW_JOINT_WHEEL:
  case CW_JOINT_DISTANCE: return (b3Vec3){ 1, 0, 0 };
  default: return (b3Vec3){ 0, 0, 1 };
  }
}

static b3Transform local_frame(b3BodyId body, b3Vec3 anchor, b3Quat q) {
  if (B3_IS_NULL(body) || !b3Body_IsValid(body)) return (b3Transform){ anchor, q };
  b3WorldTransform t = b3Body_GetTransform(body);
  b3Vec3 d = b3Sub(anchor, (b3Vec3){ (float)t.p.x, (float)t.p.y, (float)t.p.z });
  return (b3Transform){ b3InvRotateVector(t.q, d), b3InvMulQuat(t.q, q) };
}

void cw_joint3d_def_anchor(cw_joint3d_def *def, t3_vec3 world_anchor, t3_vec3 world_axis) {
  b3Vec3 axis = v3(world_axis);
  float l = b3Length(axis);
  axis = l > 0 ? b3MulSV(1.0f / l, axis) : main_axis(def->type);
  b3Quat q = cw__quat_between(main_axis(def->type), axis);
  b3Transform fa = local_frame(b3LoadBodyId(def->a.id), v3(world_anchor), q);
  b3Transform fb = local_frame(b3LoadBodyId(def->b.id), v3(world_anchor), q);
  def->frame_a = (cw_xform3){ t3v(fa.p), t3q(fa.q) };
  def->frame_b = (cw_xform3){ t3v(fb.p), t3q(fb.q) };
}

cw_joint3d cw_joint3d_create(cw_world3d wh, const cw_joint3d_def *def) {
  cw__world3 *w = cw__w3(wh);
  if (!w) {
    cw_warn("physics3d: joint on a stale or null world");
    return (cw_joint3d){ 0 };
  }
  if (!def) {
    cw_error("physics3d: cw_joint3d_create with a NULL def");
    return (cw_joint3d){ 0 };
  }
  const cw_joint3d_def *d = def;
  b3BodyId a = d->a.id ? b3LoadBodyId(d->a.id) : ground_body(w);
  b3BodyId b = b3LoadBodyId(d->b.id);
  if (!b3Body_IsValid(b) || (d->type != CW_JOINT_MOUSE && !b3Body_IsValid(a))) {
    cw_error("physics3d: joint needs valid bodies (a may be null for the world)");
    return (cw_joint3d){ 0 };
  }
  uint64_t rid;
  cw__joint3 *r = cw_pool_alloc(&w->joints, &rid);
  r->type = d->type;
  r->user = d->user;
  r->breakable = d->break_force > 0 || d->break_torque > 0;
  b3JointDef base = { 0 };
  base.bodyIdA = a;
  base.bodyIdB = b;
  base.localFrameA = (b3Transform){ v3(d->frame_a.p), q3(d->frame_a.q) };
  base.localFrameB = (b3Transform){ v3(d->frame_b.p), q3(d->frame_b.q) };
  base.collideConnected = d->collide_connected;
  base.userData = r;
  base.forceThreshold = d->break_force > 0 ? d->break_force : FLT_MAX;
  base.torqueThreshold = d->break_torque > 0 ? d->break_torque : FLT_MAX;
  b3JointId id = b3_nullJointId;
#define BASE(jd) \
  do { \
    b3JointDef keep = (jd).base; \
    (jd).base = base; \
    (jd).base.constraintHertz = keep.constraintHertz; \
    (jd).base.constraintDampingRatio = keep.constraintDampingRatio; \
    (jd).base.drawScale = keep.drawScale; \
    (jd).base.internalValue = keep.internalValue; \
  } while (0)
  switch (d->type) {
  case CW_JOINT_DISTANCE: {
    b3DistanceJointDef jd = b3DefaultDistanceJointDef();
    BASE(jd);
    float len = d->length;
    if (len <= 0) {
      b3WorldTransform ta = b3Body_GetTransform(a), tb = b3Body_GetTransform(b);
      b3Vec3 pa = b3Add((b3Vec3){ (float)ta.p.x, (float)ta.p.y, (float)ta.p.z }, b3RotateVector(ta.q, base.localFrameA.p));
      b3Vec3 pb = b3Add((b3Vec3){ (float)tb.p.x, (float)tb.p.y, (float)tb.p.z }, b3RotateVector(tb.q, base.localFrameB.p));
      len = b3Length(b3Sub(pb, pa));
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
    id = b3CreateDistanceJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_FILTER: {
    b3FilterJointDef jd = b3DefaultFilterJointDef();
    BASE(jd);
    id = b3CreateFilterJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_MOTOR: {
    b3MotorJointDef jd = b3DefaultMotorJointDef();
    BASE(jd);
    jd.linearHertz = jd.angularHertz = d->hertz;
    jd.linearDampingRatio = jd.angularDampingRatio = d->damping_ratio;
    float mf = d->max_force > 0 ? d->max_force : 1e9f;
    jd.maxSpringForce = mf;
    jd.maxSpringTorque = d->max_motor > 0 ? d->max_motor : mf;
    id = b3CreateMotorJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_PRISMATIC: {
    b3PrismaticJointDef jd = b3DefaultPrismaticJointDef();
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
    id = b3CreatePrismaticJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_REVOLUTE: {
    b3RevoluteJointDef jd = b3DefaultRevoluteJointDef();
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
    id = b3CreateRevoluteJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_WELD: {
    b3WeldJointDef jd = b3DefaultWeldJointDef();
    BASE(jd);
    jd.linearHertz = jd.angularHertz = d->hertz;
    jd.linearDampingRatio = jd.angularDampingRatio = d->damping_ratio;
    id = b3CreateWeldJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_WHEEL: {
    b3WheelJointDef jd = b3DefaultWheelJointDef();
    BASE(jd);
    jd.enableSuspensionSpring = d->spring;
    jd.suspensionHertz = d->hertz;
    jd.suspensionDampingRatio = d->damping_ratio;
    jd.enableSuspensionLimit = d->limit;
    jd.lowerSuspensionLimit = d->lower;
    jd.upperSuspensionLimit = d->upper;
    jd.enableSpinMotor = d->motor;
    jd.spinSpeed = d->motor_speed;
    jd.maxSpinTorque = d->max_motor;
    id = b3CreateWheelJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_SPHERICAL: {
    b3SphericalJointDef jd = b3DefaultSphericalJointDef();
    BASE(jd);
    jd.enableSpring = d->spring;
    jd.hertz = d->hertz;
    jd.dampingRatio = d->damping_ratio;
    jd.enableConeLimit = d->limit && d->cone_angle > 0;
    jd.coneAngle = d->cone_angle;
    jd.enableTwistLimit = d->limit && d->lower < d->upper;
    jd.lowerTwistAngle = d->lower;
    jd.upperTwistAngle = d->upper;
    jd.enableMotor = d->motor;
    jd.maxMotorTorque = d->max_motor;
    jd.motorVelocity = (b3Vec3){ 0, 0, d->motor_speed };
    id = b3CreateSphericalJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_PARALLEL: {
    b3ParallelJointDef jd = b3DefaultParallelJointDef();
    BASE(jd);
    jd.hertz = d->hertz;
    jd.dampingRatio = d->damping_ratio;
    jd.maxTorque = d->max_motor;
    id = b3CreateParallelJoint(w->id, &jd);
    break;
  }
  case CW_JOINT_MOUSE: {
    /* Box3D's own recipe (samples): a kinematic body at the target, a motor
     * joint pulling the grab point to it, angular friction from the lever */
    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_kinematicBody;
    bd.position = pos3(d->target);
    bd.enableSleep = false;
    bd.name = "cw_mouse";
    r->mouse_body = b3CreateBody(w->id, &bd);
    r->target = d->target;
    b3MotorJointDef jd = b3DefaultMotorJointDef();
    BASE(jd);
    jd.base.bodyIdA = r->mouse_body;
    jd.base.localFrameA = b3Transform_identity;
    jd.base.localFrameB = (b3Transform){ b3Body_GetLocalPoint(b, pos3(d->target)), b3Quat_identity };
    jd.linearHertz = d->hertz;
    jd.linearDampingRatio = d->damping_ratio;
    jd.angularHertz = 0;
    float mass = b3Body_GetMass(b);
    float g = b3Length(b3World_GetGravity(w->id));
    float mg = mass * (g > 0 ? g : 10);
    jd.maxSpringForce = d->max_force > 0 ? d->max_force : 1000 * mg;
    jd.maxSpringTorque = 0;
    jd.maxVelocityTorque = 0.25f * mg * 0.5f;
    id = b3CreateMotorJoint(w->id, &jd);
    break;
  }
  default: break;
  }
#undef BASE
  if (B3_IS_NULL(id)) {
    if (B3_IS_NON_NULL(r->mouse_body)) b3DestroyBody(r->mouse_body);
    cw_pool_release(&w->joints, rid);
    cw_error("physics3d: joint type %d could not be created", (int)d->type);
    return (cw_joint3d){ 0 };
  }
  r->id = b3StoreJointId(id);
  return (cw_joint3d){ r->id };
}

#define JOINT_OR(j, ret) \
  b3JointId jid = b3LoadJointId((j).id); \
  if (!(j).id || !b3Joint_IsValid(jid)) return ret; \
  cw__joint3 *jr = b3Joint_GetUserData(jid); \
  (void)jr;

void cw_joint3d_destroy(cw_joint3d j) {
  JOINT_OR(j, );
  cw__joint3_destroy(world_of_body(b3Joint_GetBodyB(jid)), jid);
}
bool cw_joint3d_valid(cw_joint3d j) { return j.id && b3Joint_IsValid(b3LoadJointId(j.id)); }
b3JointId cw_joint3d_raw(cw_joint3d j) { return b3LoadJointId(j.id); }
cw_joint_type cw_joint3d_type(cw_joint3d j) {
  JOINT_OR(j, CW_JOINT_TYPE_COUNT);
  return jr ? jr->type : CW_JOINT_TYPE_COUNT;
}
void cw_joint3d_set_user(cw_joint3d j, void *user) {
  cw__joint3 *r = jrec(j);
  if (r) r->user = user;
}
void *cw_joint3d_user(cw_joint3d j) {
  cw__joint3 *r = jrec(j);
  return r ? r->user : NULL;
}
void cw_joint3d_set_motor(cw_joint3d j, bool on, float speed, float max) {
  JOINT_OR(j, );
  switch (b3Joint_GetType(jid)) {
  case b3_revoluteJoint: b3RevoluteJoint_EnableMotor(jid, on); b3RevoluteJoint_SetMotorSpeed(jid, speed); b3RevoluteJoint_SetMaxMotorTorque(jid, max); break;
  case b3_prismaticJoint: b3PrismaticJoint_EnableMotor(jid, on); b3PrismaticJoint_SetMotorSpeed(jid, speed); b3PrismaticJoint_SetMaxMotorForce(jid, max); break;
  case b3_wheelJoint: b3WheelJoint_EnableSpinMotor(jid, on); b3WheelJoint_SetSpinMotorSpeed(jid, speed); b3WheelJoint_SetMaxSpinTorque(jid, max); break;
  case b3_distanceJoint: b3DistanceJoint_EnableMotor(jid, on); b3DistanceJoint_SetMotorSpeed(jid, speed); b3DistanceJoint_SetMaxMotorForce(jid, max); break;
  case b3_sphericalJoint: b3SphericalJoint_EnableMotor(jid, on); b3SphericalJoint_SetMotorVelocity(jid, (b3Vec3){ 0, 0, speed }); b3SphericalJoint_SetMaxMotorTorque(jid, max); break;
  default: break;
  }
  b3Joint_WakeBodies(jid);
}
void cw_joint3d_set_limits(cw_joint3d j, bool on, float lower, float upper) {
  JOINT_OR(j, );
  switch (b3Joint_GetType(jid)) {
  case b3_revoluteJoint: b3RevoluteJoint_EnableLimit(jid, on); b3RevoluteJoint_SetLimits(jid, lower, upper); break;
  case b3_prismaticJoint: b3PrismaticJoint_EnableLimit(jid, on); b3PrismaticJoint_SetLimits(jid, lower, upper); break;
  case b3_wheelJoint: b3WheelJoint_EnableSuspensionLimit(jid, on); b3WheelJoint_SetSuspensionLimits(jid, lower, upper); break;
  case b3_distanceJoint: b3DistanceJoint_EnableLimit(jid, on); b3DistanceJoint_SetLengthRange(jid, lower, upper); break;
  case b3_sphericalJoint: b3SphericalJoint_EnableTwistLimit(jid, on); b3SphericalJoint_SetTwistLimits(jid, lower, upper); break;
  default: break;
  }
  b3Joint_WakeBodies(jid);
}
void cw_joint3d_set_spring(cw_joint3d j, bool on, float hertz, float damping) {
  JOINT_OR(j, );
  switch (b3Joint_GetType(jid)) {
  case b3_revoluteJoint: b3RevoluteJoint_EnableSpring(jid, on); b3RevoluteJoint_SetSpringHertz(jid, hertz); b3RevoluteJoint_SetSpringDampingRatio(jid, damping); break;
  case b3_prismaticJoint: b3PrismaticJoint_EnableSpring(jid, on); b3PrismaticJoint_SetSpringHertz(jid, hertz); b3PrismaticJoint_SetSpringDampingRatio(jid, damping); break;
  case b3_wheelJoint: b3WheelJoint_EnableSuspension(jid, on); b3WheelJoint_SetSuspensionHertz(jid, hertz); b3WheelJoint_SetSuspensionDampingRatio(jid, damping); break;
  case b3_distanceJoint: b3DistanceJoint_EnableSpring(jid, on); b3DistanceJoint_SetSpringHertz(jid, hertz); b3DistanceJoint_SetSpringDampingRatio(jid, damping); break;
  case b3_sphericalJoint: b3SphericalJoint_EnableSpring(jid, on); b3SphericalJoint_SetSpringHertz(jid, hertz); b3SphericalJoint_SetSpringDampingRatio(jid, damping); break;
  case b3_weldJoint: b3WeldJoint_SetLinearHertz(jid, hertz); b3WeldJoint_SetAngularHertz(jid, hertz); b3WeldJoint_SetLinearDampingRatio(jid, damping); b3WeldJoint_SetAngularDampingRatio(jid, damping); break;
  case b3_parallelJoint: b3ParallelJoint_SetSpringHertz(jid, hertz); b3ParallelJoint_SetSpringDampingRatio(jid, damping); break;
  case b3_motorJoint: b3MotorJoint_SetLinearHertz(jid, hertz); b3MotorJoint_SetLinearDampingRatio(jid, damping); if (!jr || jr->type != CW_JOINT_MOUSE) { b3MotorJoint_SetAngularHertz(jid, hertz); b3MotorJoint_SetAngularDampingRatio(jid, damping); } break;
  default: break;
  }
  b3Joint_WakeBodies(jid);
}
void cw_joint3d_set_target(cw_joint3d j, t3_vec3 target) {
  JOINT_OR(j, );
  if (jr && jr->type == CW_JOINT_MOUSE) jr->target = target;
}
float cw_joint3d_value(cw_joint3d j) {
  JOINT_OR(j, 0);
  switch (b3Joint_GetType(jid)) {
  case b3_revoluteJoint: return b3RevoluteJoint_GetAngle(jid);
  case b3_prismaticJoint: return b3PrismaticJoint_GetTranslation(jid);
  case b3_distanceJoint: return b3DistanceJoint_GetCurrentLength(jid);
  case b3_wheelJoint: return b3WheelJoint_GetSpinSpeed(jid);
  case b3_sphericalJoint: return b3SphericalJoint_GetTwistAngle(jid);
  default: return 0;
  }
}

/* ── debug draw ────────────────────────────────────────────────────────
 * Box3D draws shapes through user-made "debug shapes" (created once per
 * shape, destroyed when it changes); ours keep the geometry by value or by
 * the library's stable pointer and expand to wireframes on draw. */

typedef struct dbg_shape {
  b3ShapeType type;
  union {
    b3Sphere sphere;
    b3Capsule capsule;
    const b3HullData *hull;
    b3Mesh mesh;
    const b3HeightFieldData *field;
  };
} dbg_shape;

static void *create_debug_shape(const b3DebugShape *s, void *ctx) {
  (void)ctx;
  dbg_shape *d = cw_calloc(1, sizeof *d, CW_MEM_PHYSICS);
  d->type = s->type;
  switch (s->type) {
  case b3_sphereShape: d->sphere = *s->sphere; break;
  case b3_capsuleShape: d->capsule = *s->capsule; break;
  case b3_hullShape: d->hull = s->hull; break;
  case b3_meshShape: d->mesh = *s->mesh; break;
  case b3_heightShape: d->field = s->heightField; break;
  default: break;
  }
  return d;
}
static void destroy_debug_shape(void *shape, void *ctx) {
  (void)ctx;
  cw_free(shape);
}

static void tp(b3WorldTransform t, b3Vec3 p, float out[3]) {
  b3Vec3 r = b3RotateVector(t.q, p);
  out[0] = (float)t.p.x + r.x;
  out[1] = (float)t.p.y + r.y;
  out[2] = (float)t.p.z + r.z;
}

static void sphere_lines(cw_phys_lines *l, const float c[3], b3Quat q, float r, uint32_t color) {
  b3Vec3 x = b3RotateVector(q, (b3Vec3){ 1, 0, 0 }), y = b3RotateVector(q, (b3Vec3){ 0, 1, 0 }),
         z = b3RotateVector(q, (b3Vec3){ 0, 0, 1 });
  float ax[3] = { x.x, x.y, x.z }, ay[3] = { y.x, y.y, y.z }, az[3] = { z.x, z.y, z.z };
  cw__lines_circle(l, c, ax, ay, r, color, 16);
  cw__lines_circle(l, c, ay, az, r, color, 16);
  cw__lines_circle(l, c, az, ax, r, color, 16);
}

static void capsule_lines(cw_phys_lines *l, const float a[3], const float b[3], float r, uint32_t color) {
  b3Vec3 axis = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
  float len = b3Length(axis);
  b3Vec3 u = len > 0 ? b3MulSV(1 / len, axis) : (b3Vec3){ 0, 1, 0 };
  b3Quat q = cw__quat_between((b3Vec3){ 0, 1, 0 }, u);
  sphere_lines(l, a, q, r, color);
  sphere_lines(l, b, q, r, color);
  b3Vec3 sx = b3RotateVector(q, (b3Vec3){ 1, 0, 0 }), sz = b3RotateVector(q, (b3Vec3){ 0, 0, 1 });
  b3Vec3 side[4] = { sx, b3Neg(sx), sz, b3Neg(sz) };
  for (int i = 0; i < 4; i++) {
    float p0[3] = { a[0] + side[i].x * r, a[1] + side[i].y * r, a[2] + side[i].z * r };
    float p1[3] = { b[0] + side[i].x * r, b[1] + side[i].y * r, b[2] + side[i].z * r };
    cw_phys_lines_add(l, p0, p1, color);
  }
}

static bool draw_shape(void *user_shape, b3WorldTransform t, b3HexColor color, void *ctx) {
  cw_phys_lines *l = ctx;
  dbg_shape *d = user_shape;
  uint32_t c = (uint32_t)color;
  switch (d->type) {
  case b3_sphereShape: {
    float p[3];
    tp(t, d->sphere.center, p);
    sphere_lines(l, p, t.q, d->sphere.radius, c);
    break;
  }
  case b3_capsuleShape: {
    float a[3], b[3];
    tp(t, d->capsule.center1, a);
    tp(t, d->capsule.center2, b);
    capsule_lines(l, a, b, d->capsule.radius, c);
    break;
  }
  case b3_hullShape: {
    const b3Vec3 *pts = b3GetHullPoints(d->hull);
    const b3HullHalfEdge *e = b3GetHullEdges(d->hull);
    for (int i = 0; i < d->hull->edgeCount; i++) {
      if (e[i].twin < i) continue; /* each edge once */
      float a[3], b[3];
      tp(t, pts[e[i].origin], a);
      tp(t, pts[e[e[i].twin].origin], b);
      cw_phys_lines_add(l, a, b, c);
    }
    break;
  }
  case b3_meshShape: {
    const b3Vec3 *v = b3GetMeshVertices(d->mesh.data);
    const b3MeshTriangle *tri = b3GetMeshTriangles(d->mesh.data);
    b3Vec3 s = d->mesh.scale;
    for (int i = 0; i < d->mesh.data->triangleCount; i++) {
      int idx[3] = { tri[i].index1, tri[i].index2, tri[i].index3 };
      float p[3][3];
      for (int k = 0; k < 3; k++) tp(t, (b3Vec3){ v[idx[k]].x * s.x, v[idx[k]].y * s.y, v[idx[k]].z * s.z }, p[k]);
      cw_phys_lines_add(l, p[0], p[1], c);
      cw_phys_lines_add(l, p[1], p[2], c);
      cw_phys_lines_add(l, p[2], p[0], c);
    }
    break;
  }
  case b3_heightShape: {
    /* a height field: its grid lines, at most 64 each way (a terrain region has thousands) */
    const b3HeightFieldData *f = d->field;
    const uint16_t *q = b3GetHeightFieldCompressedHeights(f);
    int nx = f->columnCount, nz = f->rowCount;
    int sx = (nx + 63) / 64, sz = (nz + 63) / 64;
    b3Vec3 s = f->scale;
#define HF_P(i, j, out) tp(t, (b3Vec3){ (float)(i) * s.x, (f->minHeight + f->heightScale * q[(j) * nx + (i)]) * s.y, (float)(j) * s.z }, out)
    for (int j = 0; j < nz; j += sz)
      for (int i = 0; i + 1 < nx; i++) {
        float a[3], b[3];
        HF_P(i, j, a);
        HF_P(i + 1, j, b);
        cw_phys_lines_add(l, a, b, c);
      }
    for (int i = 0; i < nx; i += sx)
      for (int j = 0; j + 1 < nz; j++) {
        float a[3], b[3];
        HF_P(i, j, a);
        HF_P(i, j + 1, b);
        cw_phys_lines_add(l, a, b, c);
      }
#undef HF_P
    break;
  }
  default: break;
  }
  return true;
}

static void draw_segment(b3Pos a, b3Pos b, b3HexColor color, void *ctx) {
  float p[3] = { (float)a.x, (float)a.y, (float)a.z }, q[3] = { (float)b.x, (float)b.y, (float)b.z };
  cw_phys_lines_add(ctx, p, q, (uint32_t)color);
}
static void draw_transform(b3WorldTransform t, void *ctx) {
  float o[3], e[3];
  tp(t, b3Vec3_zero, o);
  static const uint32_t col[3] = { 0xff0000, 0x00ff00, 0x0000ff };
  for (int i = 0; i < 3; i++) {
    b3Vec3 ax = { i == 0 ? 0.25f : 0, i == 1 ? 0.25f : 0, i == 2 ? 0.25f : 0 };
    tp(t, ax, e);
    cw_phys_lines_add(ctx, o, e, col[i]);
  }
}
static void draw_point(b3Pos p, float size, b3HexColor color, void *ctx) {
  (void)size;
  const float h = 0.05f;
  for (int i = 0; i < 3; i++) {
    float a[3] = { (float)p.x, (float)p.y, (float)p.z }, b[3] = { (float)p.x, (float)p.y, (float)p.z };
    a[i] -= h;
    b[i] += h;
    cw_phys_lines_add(ctx, a, b, (uint32_t)color);
  }
}
static void draw_sphere(b3Pos p, float r, b3HexColor color, float alpha, void *ctx) {
  (void)alpha;
  float c[3] = { (float)p.x, (float)p.y, (float)p.z };
  sphere_lines(ctx, c, b3Quat_identity, r, (uint32_t)color);
}
static void draw_capsule(b3Pos p1, b3Pos p2, float r, b3HexColor color, float alpha, void *ctx) {
  (void)alpha;
  float a[3] = { (float)p1.x, (float)p1.y, (float)p1.z }, b[3] = { (float)p2.x, (float)p2.y, (float)p2.z };
  capsule_lines(ctx, a, b, r, (uint32_t)color);
}
static void box_lines(cw_phys_lines *l, b3Vec3 ext, b3WorldTransform t, uint32_t color) {
  float c[8][3];
  for (int i = 0; i < 8; i++)
    tp(t, (b3Vec3){ (i & 1) ? ext.x : -ext.x, (i & 2) ? ext.y : -ext.y, (i & 4) ? ext.z : -ext.z }, c[i]);
  for (int i = 0; i < 8; i++)
    for (int bit = 1; bit < 8; bit <<= 1)
      if (!(i & bit)) cw_phys_lines_add(l, c[i], c[i | bit], color);
}
static void draw_bounds(b3AABB box, b3HexColor color, void *ctx) {
  b3Vec3 ext = b3MulSV(0.5f, b3Sub(box.upperBound, box.lowerBound));
  b3Vec3 mid = b3MulSV(0.5f, b3Add(box.upperBound, box.lowerBound));
  box_lines(ctx, ext, (b3WorldTransform){ pos3(t3v(mid)), b3Quat_identity }, (uint32_t)color);
}
static void draw_box(b3Vec3 ext, b3WorldTransform t, b3HexColor color, void *ctx) { box_lines(ctx, ext, t, (uint32_t)color); }
static void draw_string(b3Pos p, const char *s, b3HexColor color, void *ctx) {
  (void)p;
  (void)s;
  (void)color;
  (void)ctx;
}

void cw_world3d_debug_draw(cw_world3d wh, cw_phys_lines *out, uint32_t flags) {
  cw__world3 *w = cw__w3(wh);
  if (!w) return;
  b3DebugDraw dd = b3DefaultDebugDraw();
  dd.DrawShapeFcn = draw_shape;
  dd.DrawSegmentFcn = draw_segment;
  dd.DrawTransformFcn = draw_transform;
  dd.DrawPointFcn = draw_point;
  dd.DrawSphereFcn = draw_sphere;
  dd.DrawCapsuleFcn = draw_capsule;
  dd.DrawBoundsFcn = draw_bounds;
  dd.DrawBoxFcn = draw_box;
  dd.DrawStringFcn = draw_string;
  dd.drawingBounds = (b3AABB){ { -1e9f, -1e9f, -1e9f }, { 1e9f, 1e9f, 1e9f } };
  dd.drawShapes = flags & CW_PHYS_DRAW_SHAPES;
  dd.drawJoints = flags & CW_PHYS_DRAW_JOINTS;
  dd.drawBounds = flags & CW_PHYS_DRAW_BOUNDS;
  dd.drawContacts = flags & CW_PHYS_DRAW_CONTACTS;
  dd.drawContactNormals = flags & CW_PHYS_DRAW_CONTACTS;
  dd.drawMass = flags & CW_PHYS_DRAW_MASS;
  dd.context = out;
  b3World_Draw(w->id, &dd, UINT64_MAX);
}
