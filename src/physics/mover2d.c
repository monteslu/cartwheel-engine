/* CharacterBody2D-style move-and-slide on Box2D's mover API; mirrors
 * mover3d.c step for step (up is +y). Simulation code: + - * / sqrt only. */
#include <string.h>

#include "physics2d_internal.h"

#define MAX_PLANES 32
enum { K_FLOOR, K_WALL, K_CEILING };

typedef struct plane_set {
  b2CollisionPlane planes[MAX_PLANES];
  b2ShapeId shapes[MAX_PLANES];
  b2Vec2 points[MAX_PLANES];  /* world */
  b2Vec2 surface[MAX_PLANES]; /* the walkable surface normal for floors */
  uint8_t kind[MAX_PLANES];
  int count;
} plane_set;

typedef struct contact_info {
  plane_set ps;
  bool on_floor, on_wall, on_ceiling;
  b2Vec2 floor_normal, wall_normal, floor_point;
  b2ShapeId floor_shape;
} contact_info;

cw_mover2d_def cw_mover2d_default_def(void) {
  return (cw_mover2d_def){ .radius = 0.3f, .height = 1.8f, .max_floor_angle = CW_PI / 4, .step_height = 0.3f,
                           .snap_length = 0.25f, .skin = 0.02f, .max_iterations = 5, .mask = CW_PHYS_ALL_LAYERS,
                           .ride_platforms = true };
}

void cw_mover2d_init(cw_mover2d *m, cw_world2d w, const cw_mover2d_def *def, cw_vec2 feet) {
  memset(m, 0, sizeof *m);
  m->world = w;
  m->def = def ? *def : cw_mover2d_default_def();
  if (m->def.height < 2 * m->def.radius) m->def.height = 2 * m->def.radius;
  if (m->def.max_iterations <= 0) m->def.max_iterations = 5;
  if (m->def.skin <= 0) m->def.skin = 0.02f; /* as cw_mover3d_init: a zero skin sticks to walls */
  m->floor_cos = cw_cos(m->def.max_floor_angle);
  m->position = m->prev_position = feet;
}

void cw_mover2d_teleport(cw_mover2d *m, cw_vec2 feet) {
  m->position = m->prev_position = feet;
  m->on_floor = m->on_wall = m->on_ceiling = false;
  m->floor_body = (cw_body2d){ 0 };
  m->platform_velocity = (cw_vec2){ 0, 0 };
}

cw_vec2 cw_mover2d_interpolated(const cw_mover2d *m, float alpha) { return cw_v2_lerp(m->prev_position, m->position, alpha); }

typedef struct mover {
  cw_mover2d *m;
  b2WorldId world;
  b2QueryFilter filter;
  plane_set *ps;
} mover;

static b2Capsule capsule(const mover *mv, b2Vec2 pos, float extra) {
  const cw_mover2d_def *d = &mv->m->def;
  return (b2Capsule){ { pos.x, pos.y + d->radius }, { pos.x, pos.y + d->height - d->radius }, d->radius + extra };
}

static bool plane_fn(b2ShapeId s, const b2PlaneResult *pl, void *ctx) {
  mover *mv = ctx;
  plane_set *ps = mv->ps;
  if (!pl->hit || ps->count >= MAX_PLANES) return true;
  b2Plane p = pl->plane;
  p.offset += 0.005f * b2GetLengthUnitsPerMeter() - mv->m->def.skin; /* true penetration + the solver slop */
  ps->planes[ps->count] = (b2CollisionPlane){ .plane = p, .pushLimit = FLT_MAX, .push = 0, .clipVelocity = true };
  ps->shapes[ps->count] = s;
  /* Box2D leaves the plane point in the shape's body frame (only the normal
   * is rotated to world): move it to world here */
  ps->points[ps->count] = b2TransformPoint(b2Body_GetTransform(b2Shape_GetBody(s)), pl->point);
  ps->count++;
  return true;
}

static bool probe_floor(mover *mv, b2Vec2 point, b2Vec2 n, b2Vec2 *normal) {
  float skin = mv->m->def.skin;
  b2Vec2 o = { point.x + (n.x > 0 ? -0.02f : 0.02f), point.y + 2 * skin }; /* across the edge, onto the face */
  b2RayResult r = b2World_CastRayClosest(mv->world, o, (b2Vec2){ 0, -4 * skin }, mv->filter);
  if (!r.hit || r.normal.y < mv->m->floor_cos) return false;
  *normal = r.normal;
  return true;
}

/* planes within skin of the capsule at pos, classified (see mover3d.c) */
static void collide(mover *mv, b2Vec2 pos, plane_set *ps) {
  ps->count = 0;
  mv->ps = ps;
  b2Capsule c = capsule(mv, pos, mv->m->def.skin);
  b2World_CollideMover(mv->world, b2ToPos((b2Vec2){ 0, 0 }), &c, mv->filter, plane_fn, mv);
  float fc = mv->m->floor_cos;
  float edge_height = CW_MIN(mv->m->def.radius, mv->m->def.step_height + mv->m->def.skin);
  for (int i = 0; i < ps->count; i++) {
    b2Vec2 n = ps->planes[i].plane.normal;
    ps->surface[i] = n;
    if (n.y >= fc) ps->kind[i] = K_FLOOR;
    else if (n.y <= -fc) ps->kind[i] = K_CEILING;
    else if (n.y > 0 && ps->points[i].y - pos.y < edge_height && probe_floor(mv, ps->points[i], n, &ps->surface[i]))
      ps->kind[i] = K_FLOOR;
    else ps->kind[i] = K_WALL;
  }
}

static float cast(mover *mv, b2Vec2 pos, b2Vec2 d) {
  if (d.x == 0 && d.y == 0) return 1;
  b2Capsule c = capsule(mv, pos, 0);
  return b2World_CastMover(mv->world, b2ToPos((b2Vec2){ 0, 0 }), &c, d, mv->filter);
}

static void wall_planes_vertical(mover *mv, plane_set *ps) {
  (void)mv;
  for (int i = 0; i < ps->count; i++) {
    b2Vec2 n = ps->planes[i].plane.normal;
    if (ps->kind[i] != K_WALL || n.y <= 0) continue;
    if (n.x > 1e-3f || n.x < -1e-3f) ps->planes[i].plane.normal = (b2Vec2){ n.x > 0 ? 1.0f : -1.0f, 0 };
  }
}

static b2Vec2 slide(mover *mv, b2Vec2 pos, b2Vec2 delta, bool grounded) {
  b2Vec2 target = b2Add(pos, delta);
  plane_set ps;
  for (int it = 0; it < mv->m->def.max_iterations; it++) {
    collide(mv, pos, &ps);
    if (grounded) wall_planes_vertical(mv, &ps);
    b2PlaneSolverResult r = b2SolvePlanes(b2Sub(target, pos), ps.planes, ps.count);
    b2Vec2 d = r.delta;
    if (b2Dot(d, d) < 1e-12f) break;
    float f = cast(mv, pos, d);
    b2Vec2 moved = b2MulSV(f, d);
    pos = b2Add(pos, moved);
    if (b2Dot(moved, moved) < 1e-8f) break;
  }
  return pos;
}

static void classify(mover *mv, b2Vec2 pos, contact_info *ci) {
  collide(mv, pos, &ci->ps);
  ci->on_floor = ci->on_wall = ci->on_ceiling = false;
  ci->floor_shape = b2_nullShapeId;
  float best = -2;
  for (int i = 0; i < ci->ps.count; i++) {
    b2Vec2 n = ci->ps.surface[i];
    if (ci->ps.kind[i] == K_FLOOR) {
      ci->on_floor = true;
      if (n.y > best) {
        best = n.y;
        ci->floor_normal = n;
        ci->floor_point = ci->ps.points[i];
        ci->floor_shape = ci->ps.shapes[i];
      }
    } else if (ci->ps.kind[i] == K_CEILING) {
      ci->on_ceiling = true;
    } else {
      if (!ci->on_wall) ci->wall_normal = ci->ps.planes[i].plane.normal;
      ci->on_wall = true;
    }
  }
}

static float absf(float x) { return x < 0 ? -x : x; }

void cw_mover2d_move(cw_mover2d *m, float dt) {
  cw__world2 *w = cw__w2(m->world);
  if (!w) {
    cw_warn("physics2d: mover on a stale or null world");
    return;
  }
  const cw_mover2d_def *def = &m->def;
  mover mv = { .m = m, .world = w->id };
  mv.filter = b2DefaultQueryFilter();
  mv.filter.categoryBits = CW__QUERY_BIT;
  mv.filter.maskBits = def->mask;

  m->prev_position = m->position;
  b2Vec2 start = v2(m->position);
  b2Vec2 v = v2(m->velocity);
  bool was_floor = m->on_floor;
  bool rising = v.y > 1e-4f;

  b2Vec2 pv = b2Vec2_zero;
  b2BodyId floor_body = b2LoadBodyId(m->floor_body.id);
  if (was_floor && def->ride_platforms && m->floor_body.id && b2Body_IsValid(floor_body) &&
      b2Body_GetType(floor_body) != b2_staticBody)
    pv = b2Body_GetWorldPointVelocity(floor_body, start);

  b2Vec2 vm = v;
  if (was_floor && !rising) vm.y = 0;
  b2Vec2 delta = b2MulSV(dt, b2Add(vm, pv));

  b2Vec2 pos = slide(&mv, start, delta, was_floor);
  contact_info ci;
  classify(&mv, pos, &ci);

  m->stepped = false;
  float want = absf(delta.x), got = absf(pos.x - start.x);
  if (def->step_height > 0 && was_floor && !rising && ci.on_wall && want > 1e-5f && got < want * 0.95f) {
    b2Vec2 raise = { 0, def->step_height };
    float f1 = cast(&mv, start, raise);
    b2Vec2 q = b2Add(start, b2MulSV(f1, raise));
    q = slide(&mv, q, (b2Vec2){ delta.x, 0 }, true);
    b2Vec2 drop = { 0, -(def->step_height * f1 + def->skin) };
    float f3 = cast(&mv, q, drop);
    b2Vec2 q3 = b2Add(q, b2MulSV(f3, drop));
    contact_info c2;
    classify(&mv, q3, &c2);
    if (c2.on_floor && f3 < 1 && absf(q3.x - start.x) > got + 0.5f * (want - got)) {
      pos = q3;
      ci = c2;
      m->stepped = true;
    }
  }

  m->snapped = false;
  if (!ci.on_floor && was_floor && !rising && !m->stepped && def->snap_length > 0) {
    b2Vec2 drop = { 0, -def->snap_length };
    float f = cast(&mv, pos, drop);
    if (f < 1) {
      b2Vec2 q = b2Add(pos, b2MulSV(f, drop));
      contact_info c2;
      classify(&mv, q, &c2);
      if (c2.on_floor) {
        pos = q;
        ci = c2;
        m->snapped = true;
      }
    }
  }

  if (def->push_mass > 0) {
    float inv_mass = 1 / def->push_mass;
    for (int i = 0; i < ci.ps.count; i++) {
      b2BodyId body = b2Shape_GetBody(ci.ps.shapes[i]);
      if (b2Body_GetType(body) != b2_dynamicBody) continue;
      b2Vec2 point = ci.ps.points[i];
      b2Vec2 n = b2Neg(ci.ps.planes[i].plane.normal);
      b2Vec2 rb = b2Sub(point, b2ToVec2(b2Body_GetWorldCenter(body)));
      float rn = b2Cross(rb, n);
      float inv_i = b2Body_GetRotationalInertia(body);
      inv_i = inv_i > 0 ? 1 / inv_i : 0;
      float mb = b2Body_GetMass(body);
      float k = inv_mass + (mb > 0 ? 1 / mb : 0) + inv_i * rn * rn;
      float nm = k > 0 ? 1 / k : 0;
      b2Vec2 vb = b2Add(b2Body_GetLinearVelocity(body), b2CrossSV(b2Body_GetAngularVelocity(body), rb));
      float impulse = -nm * b2Dot(b2Sub(vb, v), n);
      if (impulse <= 0) continue;
      b2Vec2 P = b2MulSV(impulse, n);
      v = b2MulSub(v, inv_mass, P);
      b2Body_ApplyLinearImpulse(body, P, point, true);
    }
  }

  if (ci.on_floor && v.y < 0) v.y = 0;
  for (int pass = 0; pass < 3; pass++) {
    for (int i = 0; i < ci.ps.count; i++) {
      b2Vec2 n = ci.ps.planes[i].plane.normal;
      if (ci.on_floor && ci.ps.kind[i] == K_FLOOR) continue;
      if (ci.on_floor && ci.ps.kind[i] == K_WALL && n.y > 0 && absf(n.x) > 1e-3f) n = (b2Vec2){ n.x > 0 ? 1.0f : -1.0f, 0 };
      float vn = b2Dot(v, n);
      if (vn < 0) v = b2MulSub(v, vn, n);
    }
  }

  if (was_floor && !ci.on_floor) v = b2Add(v, pv);

  m->position = cwv(pos);
  m->velocity = cwv(v);
  m->on_floor = ci.on_floor;
  m->on_wall = ci.on_wall;
  m->on_ceiling = ci.on_ceiling;
  m->floor_normal = ci.on_floor ? cwv(ci.floor_normal) : (cw_vec2){ 0, 0 };
  m->wall_normal = ci.on_wall ? cwv(ci.wall_normal) : (cw_vec2){ 0, 0 };
  m->floor_body = ci.on_floor ? (cw_body2d){ b2StoreBodyId(b2Shape_GetBody(ci.floor_shape)) } : (cw_body2d){ 0 };
  m->platform_velocity = cwv(pv);
}
