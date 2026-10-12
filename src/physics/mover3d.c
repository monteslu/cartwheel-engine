/* CharacterBody3D-style move-and-slide on Box3D's (experimental) mover API:
 * b3World_CollideMover gathers contact planes, b3SolvePlanes finds the
 * closest allowed delta, b3World_CastMover sweeps it. Everything here is
 * simulation code: only + - * / sqrt and cw_ math.
 *
 * Contacts are classified floor / wall / ceiling by their normal against
 * up. A contact on the rounded bottom whose normal is too steep (a ledge or
 * stair EDGE under the capsule) is probed with a short ray just inside the
 * edge: if the surface there is walkable, the contact counts as floor. That
 * is what lets the mover stand on and walk over edges up to step_height
 * (below the capsule radius the rounded bottom rides over them; above it
 * the explicit up / across / down step does). */
#include <string.h>

#include "physics3d_internal.h"

#define MAX_PLANES 32
enum { K_FLOOR, K_WALL, K_CEILING };

typedef struct plane_set {
  b3CollisionPlane planes[MAX_PLANES];
  b3ShapeId shapes[MAX_PLANES];
  b3Vec3 points[MAX_PLANES];  /* relative to the query origin (the feet) */
  b3Vec3 surface[MAX_PLANES]; /* the walkable surface normal for floors */
  uint8_t kind[MAX_PLANES];
  int count;
} plane_set;

typedef struct contact_info {
  plane_set ps;
  bool on_floor, on_wall, on_ceiling;
  b3Vec3 floor_normal, wall_normal, floor_point;
  b3ShapeId floor_shape;
} contact_info;

cw_mover3d_def cw_mover3d_default_def(void) {
  return (cw_mover3d_def){ .radius = 0.3f, .height = 1.8f, .up = { 0, 1, 0 }, .max_floor_angle = CW_PI / 4,
                           .step_height = 0.3f, .snap_length = 0.25f, .skin = 0.02f, .max_iterations = 5,
                           .mask = CW_PHYS_ALL_LAYERS, .ride_platforms = true };
}

void cw_mover3d_init(cw_mover3d *m, cw_world3d w, const cw_mover3d_def *def, t3_vec3 feet) {
  memset(m, 0, sizeof *m);
  m->world = w;
  m->def = def ? *def : cw_mover3d_default_def();
  b3Vec3 up = v3(m->def.up);
  float l = b3Length(up);
  m->def.up = l > 0 ? t3v(b3MulSV(1 / l, up)) : (t3_vec3){ 0, 1, 0 };
  if (m->def.height < 2 * m->def.radius) m->def.height = 2 * m->def.radius;
  if (m->def.max_iterations <= 0) m->def.max_iterations = 5;
  if (m->def.skin <= 0) m->def.skin = 0.02f;
  m->floor_cos = cw_cos(m->def.max_floor_angle);
  m->position = m->prev_position = feet;
}

void cw_mover3d_teleport(cw_mover3d *m, t3_vec3 feet) {
  m->position = m->prev_position = feet;
  m->on_floor = m->on_wall = m->on_ceiling = false;
  m->floor_body = (cw_body3d){ 0 };
  m->platform_velocity = (t3_vec3){ 0, 0, 0 };
}

t3_vec3 cw_mover3d_interpolated(const cw_mover3d *m, float alpha) {
  t3_vec3 a = m->prev_position, b = m->position;
  return (t3_vec3){ a.x + (b.x - a.x) * alpha, a.y + (b.y - a.y) * alpha, a.z + (b.z - a.z) * alpha };
}

/* ── queries ─────────────────────────────────────────────────────────── */

typedef struct mover {
  cw_mover3d *m;
  b3WorldId world;
  b3Vec3 up;
  b3QueryFilter filter;
  plane_set *ps;
} mover;

static b3Vec3 horizontal(b3Vec3 v, b3Vec3 up) { return b3Sub(v, b3MulSV(b3Dot(v, up), up)); }

static b3Capsule capsule(const mover *mv, float extra) {
  const cw_mover3d_def *d = &mv->m->def;
  return (b3Capsule){ b3MulSV(d->radius, mv->up), b3MulSV(d->height - d->radius, mv->up), d->radius + extra };
}

static bool plane_fn(b3ShapeId s, const b3PlaneResult *pl, int n, void *ctx) {
  mover *mv = ctx;
  plane_set *ps = mv->ps;
  for (int i = 0; i < n && ps->count < MAX_PLANES; i++) {
    b3Plane p = pl[i].plane;
    /* found with an inflated capsule: back to true penetration, plus the
     * solver's slop so the mover rests ON surfaces rather than 5 mm in */
    p.offset += B3_LINEAR_SLOP - mv->m->def.skin;
    ps->planes[ps->count] = (b3CollisionPlane){ .plane = p, .pushLimit = FLT_MAX, .push = 0, .clipVelocity = true };
    ps->shapes[ps->count] = s;
    ps->points[ps->count] = pl[i].point;
    ps->count++;
  }
  return true;
}

/* is the surface under a steep bottom contact walkable? */
static bool probe_floor(mover *mv, b3Vec3 pos, b3Vec3 point, b3Vec3 n, b3Vec3 *normal) {
  const cw_mover3d_def *d = &mv->m->def;
  b3Vec3 toward = horizontal(b3Neg(n), mv->up); /* across the edge, onto the face beyond it */
  float l = b3Length(toward);
  if (l > 1e-5f) toward = b3MulSV(0.02f / l, toward);
  b3Vec3 o = b3Add(b3Add(b3Add(pos, point), toward), b3MulSV(2 * d->skin, mv->up));
  b3RayResult r = b3World_CastRayClosest(mv->world, b3ToPos(o), b3MulSV(-4 * d->skin, mv->up), mv->filter);
  if (!r.hit || b3Dot(r.normal, mv->up) < mv->m->floor_cos) return false;
  *normal = r.normal;
  return true;
}

/* planes within `skin` of the capsule at pos, classified */
static void collide(mover *mv, b3Vec3 pos, plane_set *ps) {
  ps->count = 0;
  mv->ps = ps;
  b3Capsule c = capsule(mv, mv->m->def.skin);
  b3World_CollideMover(mv->world, b3ToPos(pos), &c, mv->filter, plane_fn, mv);
  float fc = mv->m->floor_cos;
  /* edges this low are stepped over by riding the rounded bottom */
  float edge_height = CW_MIN(mv->m->def.radius, mv->m->def.step_height + mv->m->def.skin);
  for (int i = 0; i < ps->count; i++) {
    b3Vec3 n = ps->planes[i].plane.normal;
    float d = b3Dot(n, mv->up);
    ps->surface[i] = n;
    if (d >= fc) ps->kind[i] = K_FLOOR;
    else if (d <= -fc) ps->kind[i] = K_CEILING;
    else if (d > 0 && b3Dot(ps->points[i], mv->up) < edge_height && probe_floor(mv, pos, ps->points[i], n, &ps->surface[i]))
      ps->kind[i] = K_FLOOR;
    else ps->kind[i] = K_WALL;
  }
}

/* fraction of d the capsule can sweep from pos */
static float cast(mover *mv, b3Vec3 pos, b3Vec3 d) {
  if (b3Dot(d, d) == 0) return 1;
  b3Capsule c = capsule(mv, 0);
  return b3World_CastMover(mv->world, b3ToPos(pos), &c, d, mv->filter, NULL, NULL);
}

/* a grounded mover meets steep surfaces as vertical walls: it must not
 * climb them by sliding */
static void walls_vertical(mover *mv, plane_set *ps) {
  for (int i = 0; i < ps->count; i++) {
    if (ps->kind[i] != K_WALL) continue;
    b3Vec3 n = ps->planes[i].plane.normal;
    if (b3Dot(n, mv->up) <= 0) continue;
    b3Vec3 h = horizontal(n, mv->up);
    float l = b3Length(h);
    if (l > 1e-3f) ps->planes[i].plane.normal = b3MulSV(1 / l, h);
  }
}

static b3Vec3 slide(mover *mv, b3Vec3 pos, b3Vec3 delta, bool grounded) {
  b3Vec3 target = b3Add(pos, delta);
  plane_set ps;
  for (int it = 0; it < mv->m->def.max_iterations; it++) {
    collide(mv, pos, &ps);
    if (grounded) walls_vertical(mv, &ps);
    b3PlaneSolverResult r = b3SolvePlanes(b3Sub(target, pos), ps.planes, ps.count);
    b3Vec3 d = r.delta;
    if (b3Dot(d, d) < 1e-12f) break;
    float f = cast(mv, pos, d);
    b3Vec3 moved = b3MulSV(f, d);
    pos = b3Add(pos, moved);
    if (b3Dot(moved, moved) < 1e-8f) break;
  }
  return pos;
}

static void classify(mover *mv, b3Vec3 pos, contact_info *ci) {
  collide(mv, pos, &ci->ps);
  ci->on_floor = ci->on_wall = ci->on_ceiling = false;
  ci->floor_shape = b3_nullShapeId;
  float best = -2;
  for (int i = 0; i < ci->ps.count; i++) {
    switch (ci->ps.kind[i]) {
    case K_FLOOR: {
      float d = b3Dot(ci->ps.surface[i], mv->up);
      ci->on_floor = true;
      if (d > best) {
        best = d;
        ci->floor_normal = ci->ps.surface[i];
        ci->floor_point = b3Add(pos, ci->ps.points[i]);
        ci->floor_shape = ci->ps.shapes[i];
      }
      break;
    }
    case K_CEILING: ci->on_ceiling = true; break;
    default:
      if (!ci->on_wall) ci->wall_normal = ci->ps.planes[i].plane.normal;
      ci->on_wall = true;
      break;
    }
  }
}

static float hprogress(b3Vec3 a, b3Vec3 b, b3Vec3 up) { return b3Length(horizontal(b3Sub(b, a), up)); }

void cw_mover3d_move(cw_mover3d *m, float dt) {
  cw__world3 *w = cw__w3(m->world);
  if (!w) {
    cw_warn("physics3d: mover on a stale or null world");
    return;
  }
  const cw_mover3d_def *def = &m->def;
  mover mv = { .m = m, .world = w->id, .up = v3(def->up) };
  mv.filter = b3DefaultQueryFilter();
  mv.filter.categoryBits = CW__QUERY_BIT; /* sensors never answer movers */
  mv.filter.maskBits = def->mask;
  b3Vec3 up = mv.up;

  m->prev_position = m->position;
  b3Vec3 start = v3(m->position);
  b3Vec3 v = v3(m->velocity);
  bool was_floor = m->on_floor;
  bool rising = b3Dot(v, up) > 1e-4f;

  /* the floor's velocity under the feet, read now so a kinematic platform
   * driven before this call is followed in the same step */
  b3Vec3 pv = b3Vec3_zero;
  b3BodyId floor_body = b3LoadBodyId(m->floor_body.id);
  if (was_floor && def->ride_platforms && m->floor_body.id && b3Body_IsValid(floor_body) &&
      b3Body_GetType(floor_body) != b3_staticBody)
    pv = b3Body_GetWorldPointVelocity(floor_body, b3ToPos(start));

  /* grounded: gravity does not push into the floor (no creeping down
   * slopes); snapping keeps the feet on it instead */
  b3Vec3 vm = v;
  if (was_floor && !rising) vm = horizontal(v, up);
  b3Vec3 delta = b3MulSV(dt, b3Add(vm, pv));

  b3Vec3 pos = slide(&mv, start, delta, was_floor);
  contact_info ci;
  classify(&mv, pos, &ci);

  /* step up a ledge: up, across, down; keep it if it got further */
  m->stepped = false;
  b3Vec3 want_h = horizontal(delta, up);
  float want = b3Length(want_h), got = hprogress(start, pos, up);
  if (def->step_height > 0 && was_floor && !rising && ci.on_wall && want > 1e-5f && got < want * 0.95f) {
    b3Vec3 raise = b3MulSV(def->step_height, up);
    float f1 = cast(&mv, start, raise);
    b3Vec3 q = b3Add(start, b3MulSV(f1, raise));
    q = slide(&mv, q, want_h, true);
    b3Vec3 drop = b3MulSV(-(def->step_height * f1 + def->skin), up);
    float f3 = cast(&mv, q, drop);
    b3Vec3 q3 = b3Add(q, b3MulSV(f3, drop));
    contact_info c2;
    classify(&mv, q3, &c2);
    if (c2.on_floor && f3 < 1 && hprogress(start, q3, up) > got + 0.5f * (want - got)) { /* real progress, not jitter */
      pos = q3;
      ci = c2;
      m->stepped = true;
    }
  }

  /* snap down onto the floor (stairs and slopes going down) */
  m->snapped = false;
  if (!ci.on_floor && was_floor && !rising && !m->stepped && def->snap_length > 0) {
    b3Vec3 drop = b3MulSV(-def->snap_length, up);
    float f = cast(&mv, pos, drop);
    if (f < 1) {
      b3Vec3 q = b3Add(pos, b3MulSV(f, drop));
      contact_info c2;
      classify(&mv, q, &c2);
      if (c2.on_floor) {
        pos = q;
        ci = c2;
        m->snapped = true;
      }
    }
  }

  /* push dynamic bodies (Box3D's mover sample's impulse) */
  if (def->push_mass > 0) {
    float inv_mass = 1 / def->push_mass;
    for (int i = 0; i < ci.ps.count; i++) {
      b3BodyId body = b3Shape_GetBody(ci.ps.shapes[i]);
      if (b3Body_GetType(body) != b3_dynamicBody) continue;
      b3Pos point = b3ToPos(b3Add(pos, ci.ps.points[i]));
      b3Vec3 n = b3Neg(ci.ps.planes[i].plane.normal);
      b3Vec3 rb = b3SubPos(point, b3Body_GetWorldCenterOfMass(body));
      b3Matrix3 inv_i = b3Body_GetWorldInverseRotationalInertia(body);
      b3Vec3 rn = b3Cross(rb, n);
      float k = inv_mass + b3Body_GetInverseMass(body) + b3Dot(rn, b3MulMV(inv_i, rn));
      float nm = k > 0 ? 1 / k : 0;
      b3Vec3 vb = b3Add(b3Body_GetLinearVelocity(body), b3Cross(b3Body_GetAngularVelocity(body), rb));
      float impulse = -nm * b3Dot(b3Sub(vb, v), n);
      if (impulse <= 0) continue;
      b3Vec3 P = b3MulSV(impulse, n);
      v = b3MulSub(v, inv_mass, P);
      b3Body_ApplyLinearImpulse(body, P, point, true);
    }
  }

  /* clip the velocity against what was hit; walking, the floor is not in
   * the way and steep surfaces are vertical walls */
  if (ci.on_floor && b3Dot(v, up) < 0) v = horizontal(v, up);
  for (int pass = 0; pass < 3; pass++) {
    for (int i = 0; i < ci.ps.count; i++) {
      if (ci.on_floor && ci.ps.kind[i] == K_FLOOR) continue;
      b3Vec3 n = ci.ps.planes[i].plane.normal;
      if (ci.on_floor && ci.ps.kind[i] == K_WALL && b3Dot(n, up) > 0) {
        b3Vec3 h = horizontal(n, up);
        float l = b3Length(h);
        if (l > 1e-3f) n = b3MulSV(1 / l, h);
      }
      float vn = b3Dot(v, n);
      if (vn < 0) v = b3MulSub(v, vn, n);
    }
  }

  /* leaving a moving platform keeps its motion */
  if (was_floor && !ci.on_floor) v = b3Add(v, pv);

  m->position = t3v(pos);
  m->velocity = t3v(v);
  m->on_floor = ci.on_floor;
  m->on_wall = ci.on_wall;
  m->on_ceiling = ci.on_ceiling;
  m->floor_normal = ci.on_floor ? t3v(ci.floor_normal) : (t3_vec3){ 0, 0, 0 };
  m->wall_normal = ci.on_wall ? t3v(ci.wall_normal) : (t3_vec3){ 0, 0, 0 };
  m->floor_body = ci.on_floor ? (cw_body3d){ b3StoreBodyId(b3Shape_GetBody(ci.floor_shape)) } : (cw_body3d){ 0 };
  m->platform_velocity = t3v(pv);
}
