/* Raycast vehicles on Box3D (cartwheel/vehicle3d.h).
 *
 * One tick: every wheel casts first (pass 1: contacts, compression, how
 * many wheels of each axle touch); each touching wheel applies its
 * suspension force and sets up its tyre (pass 2); then the tyres run as a
 * few sequential-impulse iterations in wheel order (pass 3). Box3D applies
 * an impulse to the body's velocity at once, so each row sees what the rows
 * before it did. Simulation code: only + - * / sqrt and cw_ math. */
#include <string.h>

#include "cartwheel/math.h"
#include "cartwheel/vehicle3d.h"
#include "physics3d_internal.h"

cw_wheel3d_def cw_wheel3d_default_def(t3_vec3 anchor, int axle) {
  return (cw_wheel3d_def){ .anchor = anchor, .axle = axle, .radius = 0.35f, .travel = 0.25f, .hertz = 1.6f,
                           .damping_ratio = 0.5f, .grip = 1, .slide_grip = 0.85f, .lateral_stiffness = 1,
                           .plough_speed = 6, .handbrake_locks = true, .handbrake_grip = 0.35f };
}

cw_vehicle3d_def cw_vehicle3d_default_def(void) {
  return (cw_vehicle3d_def){ .up = { 0, 1, 0 }, .forward = { 0, 0, 1 }, .mask = CW_PHYS_ALL_LAYERS,
                             .roll_influence = 1, .rolling_resistance = 0.015f, .drag = 0.0005f, .max_load = 4 };
}

int cw_vehicle3d_def_add_wheel(cw_vehicle3d_def *def, t3_vec3 anchor, int axle) {
  if (def->wheel_count >= CW_VEHICLE3D_WHEELS) return -1;
  def->wheels[def->wheel_count] = cw_wheel3d_default_def(anchor, axle);
  return def->wheel_count++;
}

/* ── small vector maths (b3Vec3) ───────────────────────────────────────── */

static b3Vec3 unit(b3Vec3 v, b3Vec3 fallback) {
  float l = sqrtf(b3Dot(v, v));
  return l > 1e-9f ? b3MulSV(1 / l, v) : fallback;
}
static b3Vec3 pos_sub(b3Pos a, b3Pos b) { return (b3Vec3){ (float)(a.x - b.x), (float)(a.y - b.y), (float)(a.z - b.z) }; }
static b3Pos pos_add(b3Pos a, b3Vec3 v) { return (b3Pos){ a.x + v.x, a.y + v.y, a.z + v.z }; }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static float absf(float x) { return x < 0 ? -x : x; }

/* what a body resists an impulse along d at r (from its centre of mass) with */
typedef struct mass_info {
  bool dynamic;
  float inv_mass;
  b3Matrix3 inv_i;
  b3Pos com;
} mass_info;
static mass_info mass_of(b3BodyId id) {
  mass_info m = { 0 };
  if (b3Body_GetType(id) != b3_dynamicBody) return m;
  m.dynamic = true;
  m.inv_mass = b3Body_GetInverseMass(id);
  m.inv_i = b3Body_GetWorldInverseRotationalInertia(id);
  m.com = b3Body_GetWorldCenterOfMass(id);
  return m;
}
static float inv_k(const mass_info *m, b3Pos p, b3Vec3 d) {
  if (!m->dynamic) return 0;
  b3Vec3 rd = b3Cross(pos_sub(p, m->com), d);
  return m->inv_mass + b3Dot(rd, b3MulMV(m->inv_i, rd));
}

/* ── setup ───────────────────────────────────────────────────────────── */

static void local_axes(const cw_vehicle3d_def *d, b3Vec3 *up, b3Vec3 *fwd, b3Vec3 *left) {
  *up = unit(v3(d->up), (b3Vec3){ 0, 1, 0 });
  b3Vec3 f = v3(d->forward);
  f = b3Sub(f, b3MulSV(b3Dot(f, *up), *up));
  *fwd = unit(f, absf(up->z) < 0.9f ? b3Normalize(b3Cross((b3Vec3){ 1, 0, 0 }, *up)) : (b3Vec3){ 1, 0, 0 });
  *left = b3Cross(*up, *fwd);
}

bool cw_vehicle3d_init(cw_vehicle3d *v, cw_body3d chassis, const cw_vehicle3d_def *def) {
  memset(v, 0, sizeof *v);
  if (!def || def->wheel_count < 1 || def->wheel_count > CW_VEHICLE3D_WHEELS) {
    cw_error("vehicle3d: a vehicle needs 1..%d wheels", CW_VEHICLE3D_WHEELS);
    return false;
  }
  if (!cw_body3d_valid(chassis)) {
    cw_error("vehicle3d: init on a stale or null chassis");
    return false;
  }
  v->def = *def;
  b3Vec3 up, fwd, left;
  local_axes(def, &up, &fwd, &left);
  v->def.up = t3v(up);
  v->def.forward = t3v(fwd);
  for (int i = 0; i < v->def.wheel_count; i++) {
    cw_wheel3d_def *w = &v->def.wheels[i];
    w->axle = w->axle < 0 ? 0 : w->axle >= CW_VEHICLE3D_AXLES ? CW_VEHICLE3D_AXLES - 1 : w->axle;
    if (w->radius <= 0) w->radius = 0.35f;
    if (w->travel < 0) w->travel = 0;
    w->lateral_stiffness = clampf(w->lateral_stiffness, 0, 1);
  }
  v->def.roll_influence = clampf(v->def.roll_influence, 0, 1);
  v->chassis = chassis;
  v->world = cw_body3d_world(chassis);
  v->active = true;
  return true;
}

void cw_vehicle3d_set_axle(cw_vehicle3d *v, int axle, float steer, float drive, float brake) {
  if (axle < 0 || axle >= CW_VEHICLE3D_AXLES) return;
  v->axle[axle] = (cw_vehicle3d_axle){ steer, drive, brake < 0 ? 0 : brake };
}

void cw_vehicle3d_set_handbrake(cw_vehicle3d *v, bool on) { v->handbrake = on; }

void cw_vehicle3d_reset(cw_vehicle3d *v) {
  for (int i = 0; i < CW_VEHICLE3D_WHEELS; i++) {
    cw_wheel3d *w = &v->wheel[i];
    float steer = w->steer;
    memset(w, 0, sizeof *w);
    w->steer = w->prev_steer = steer;
  }
  v->contacts = 0;
}

void cw_vehicle3d_teleport(cw_vehicle3d *v, cw_xform3 x) {
  if (!cw_body3d_valid(v->chassis)) {
    cw_warn("vehicle3d: teleport on a stale or null chassis");
    return;
  }
  cw_body3d_set_transform(v->chassis, x);
  cw_body3d_set_velocity(v->chassis, (t3_vec3){ 0, 0, 0 });
  cw_body3d_set_angular_velocity(v->chassis, (t3_vec3){ 0, 0, 0 });
  cw_vehicle3d_reset(v);
}

void cw_vehicle3d_set_active(cw_vehicle3d *v, bool on) {
  if (v->active && !on) cw_vehicle3d_reset(v);
  v->active = on;
}

bool cw_vehicle3d_grounded(const cw_vehicle3d *v, bool all) {
  return all ? v->contacts == v->def.wheel_count : v->contacts > 0;
}

/* ── the tick ────────────────────────────────────────────────────────── */

typedef struct wheel_cast {
  b3Pos p;           /* contact point */
  b3Vec3 n;          /* surface normal */
  b3BodyId ground;
  float raw;         /* compression before clamping (> travel: bottomed out) */
} wheel_cast;

/* one touching wheel's tyre in the solver */
typedef struct tyre_row {
  bool on, moving, lock, clamped, clamped_l;
  mass_info gm;           /* the ground's */
  b3Pos p, pl;            /* contact; where sideways grip acts */
  b3Vec3 f, s;            /* along and across the wheel on the contact plane */
  float ml, ms;           /* effective masses along f at p and along s at pl */
  float limit, drive, brake;
  float vs0;              /* the skid before the tyres */
  float jl, js, jb;       /* accumulated: along, across, the brake's share of along */
} tyre_row;
#define TYRE_ITERATIONS 4

static b3Vec3 rel_vel(b3BodyId body, b3BodyId ground, bool moving, b3Pos p) {
  b3Vec3 v = b3Body_GetWorldPointVelocity(body, p);
  return moving ? b3Sub(v, b3Body_GetWorldPointVelocity(ground, p)) : v;
}

static void rest_wheels(cw_vehicle3d *v) {
  for (int i = 0; i < v->def.wheel_count; i++) {
    cw_wheel3d *w = &v->wheel[i];
    w->prev_compression = w->compression;
    w->prev_spin = w->spin;
    w->prev_steer = w->steer;
  }
}

void cw_vehicle3d_update(cw_vehicle3d *v, float dt) {
  if (!cw_body3d_valid(v->chassis) || dt <= 0) {
    if (dt > 0) cw_warn("vehicle3d: update on a stale or null chassis");
    return;
  }
  b3BodyId bid = cw_body3d_raw(v->chassis);
  const cw_vehicle3d_def *d = &v->def;
  int n = d->wheel_count;

  if (!v->active) { /* a ragdoll: no forces, wheels hanging */
    rest_wheels(v);
    for (int i = 0; i < n; i++) {
      cw_wheel3d *w = &v->wheel[i];
      w->contact = false;
      w->compression = w->load = w->slip_side = w->slip_long = 0;
      w->ground = (cw_body3d){ 0 };
    }
    v->contacts = 0;
    return;
  }
  bool driving = false;
  for (int a = 0; a < CW_VEHICLE3D_AXLES; a++) driving |= v->axle[a].drive != 0;
  if (!b3Body_IsAwake(bid) && !driving) { /* parked and asleep: nothing moves, nothing to wake it for */
    rest_wheels(v);
    for (int i = 0; i < n; i++) v->wheel[i].steer = v->axle[d->wheels[i].axle].steer;
    return;
  }

  b3WorldTransform X = b3Body_GetTransform(bid);
  b3Vec3 up = b3RotateVector(X.q, v3(d->up)), fwd = b3RotateVector(X.q, v3(d->forward));
  b3Vec3 left = b3Cross(up, fwd), down = b3Neg(up);
  v->up_world = t3v(up);
  v->forward_world = t3v(fwd);
  mass_info me = mass_of(bid);
  float mass = me.inv_mass > 0 ? 1 / me.inv_mass : 0;
  float sprung = d->sprung_mass > 0 ? d->sprung_mass : mass;
  float share = sprung / n;
  t3_vec3 gv = cw_world3d_gravity(v->world);
  float g = sqrtf(gv.x * gv.x + gv.y * gv.y + gv.z * gv.z);
  float g_ref = g > 0.5f ? g : 9.81f; /* what "the weight" means for max_load in a world without gravity */
  float max_load = d->max_load > 0 ? d->max_load * share * g_ref : 1e30f;

  /* pass 1: cast every wheel */
  wheel_cast cast[CW_VEHICLE3D_WHEELS];
  int touching[CW_VEHICLE3D_AXLES] = { 0 }, on_axle[CW_VEHICLE3D_AXLES] = { 0 };
  cw_phys_filter filter = { .layers = d->mask, .ignore_body = v->chassis.id };
  v->contacts = 0;
  for (int i = 0; i < n; i++) {
    const cw_wheel3d_def *wd = &d->wheels[i];
    cw_wheel3d *w = &v->wheel[i];
    w->prev_compression = w->compression;
    w->prev_spin = w->spin;
    w->prev_steer = w->steer;
    w->steer = v->axle[wd->axle].steer;
    w->locked = v->handbrake && wd->handbrake;
    on_axle[wd->axle]++;
    b3Pos origin = pos_add(X.p, b3RotateVector(X.q, v3(wd->anchor)));
    float len = wd->travel + wd->radius;
    cw_ray_hit3d hit;
    w->contact = cw_world3d_raycast(v->world, t3p(origin), t3v(b3MulSV(len, down)), &filter, &hit);
    if (!w->contact) {
      w->ground = (cw_body3d){ 0 };
      w->ground_shape = (cw_shape3d){ 0 };
      w->ground_material = 0;
      w->compression = 0;
      w->load = 0;
      w->slip_side = w->slip_long = 0;
      w->sliding = false;
      continue;
    }
    cast[i].p = pos3(hit.point);
    cast[i].n = v3(hit.normal);
    cast[i].ground = cw_body3d_raw(hit.body);
    cast[i].raw = len - hit.fraction * len;
    w->point = hit.point;
    w->normal = hit.normal;
    w->ground = hit.body;
    w->ground_shape = hit.shape;
    w->ground_material = hit.material;
    w->compression = clampf(cast[i].raw, 0, wd->travel);
    touching[wd->axle]++;
    v->contacts++;
  }

  /* pass 2: suspension. A force (Box3D integrates it in the sub-steps with
   * gravity, so a car at rest reads still), plus a bump-stop impulse that
   * takes out any speed into a bottomed-out wheel. */
  tyre_row row[CW_VEHICLE3D_WHEELS];
  for (int i = 0; i < n; i++) {
    const cw_wheel3d_def *wd = &d->wheels[i];
    cw_wheel3d *w = &v->wheel[i];
    tyre_row *t = &row[i];
    t->on = false;
    if (!w->contact) continue;
    wheel_cast *c = &cast[i];
    t->gm = mass_of(c->ground);
    t->moving = b3Body_GetType(c->ground) != b3_staticBody;
    b3Vec3 vrel = rel_vel(bid, c->ground, t->moving, c->p);
    float omega = 2 * CW_PI * wd->hertz;
    float k = share * omega * omega, damp = 2 * share * wd->damping_ratio * omega;
    float rate = b3Dot(vrel, down); /* > 0: compressing */
    float force = k * w->compression + damp * rate;
    if (force < 0) force = 0;
    float bump = 0;
    if (c->raw >= wd->travel && rate > 0) {
      float kk = inv_k(&me, c->p, down) + inv_k(&t->gm, c->p, down);
      if (kk > 0) bump = rate / kk;
    }
    w->load = force + bump / dt;
    b3Body_ApplyForce(bid, b3MulSV(force, up), c->p, true);
    if (t->gm.dynamic) b3Body_ApplyForce(c->ground, b3MulSV(-force, up), c->p, true);
    if (bump > 0) {
      b3Body_ApplyLinearImpulse(bid, b3MulSV(bump, up), c->p, true);
      if (t->gm.dynamic) b3Body_ApplyLinearImpulse(c->ground, b3MulSV(-bump, up), c->p, true);
    }

    /* the tyre's frame: the wheel's heading laid on the contact plane */
    float sn, cs;
    cw_sincos(w->steer, &sn, &cs);
    b3Vec3 heading = b3Add(b3MulSV(cs, fwd), b3MulSV(sn, left));
    b3Vec3 f = b3Sub(heading, b3MulSV(b3Dot(heading, c->n), c->n));
    float fl = sqrtf(b3Dot(f, f));
    if (fl < 1e-4f) continue; /* a wheel pointing into the surface: no grip to speak of */
    t->on = true;
    t->f = b3MulSV(1 / fl, f);
    t->s = b3Cross(c->n, t->f);
    t->p = c->p;
    t->pl = c->p; /* sideways grip acts between the contact and the centre of mass's height */
    if (d->roll_influence < 1) t->pl = pos_add(c->p, b3MulSV((1 - d->roll_influence) * b3Dot(pos_sub(me.com, c->p), up), up));
    float kl = inv_k(&me, t->p, t->f) + inv_k(&t->gm, t->p, t->f), ks = inv_k(&me, t->pl, t->s) + inv_k(&t->gm, t->pl, t->s);
    t->ml = kl > 0 ? 1 / kl : 0;
    t->ms = ks > 0 ? 1 / ks : 0;
    t->lock = w->locked && wd->handbrake_locks;
    float grip = wd->grip * (w->locked ? wd->handbrake_grip : 1) * (w->sliding ? wd->slide_grip : 1);
    float load = w->load < max_load ? w->load : max_load;
    t->limit = grip * load * dt;
    const cw_vehicle3d_axle *ax = &v->axle[wd->axle];
    t->drive = t->lock || !touching[wd->axle] ? 0 : ax->drive / touching[wd->axle] * dt;
    t->brake = t->lock ? t->limit : ax->brake / on_axle[wd->axle] * dt;
    b3Vec3 v0 = rel_vel(bid, c->ground, t->moving, t->pl);
    t->vs0 = b3Dot(v0, t->s);
    if (wd->plough_grip > 0 && wd->plough_speed > 0) { /* sliding sideways through loose ground: it digs in */
      float dig = clampf((absf(t->vs0) - wd->plough_speed) / wd->plough_speed, 0, 1);
      t->limit += wd->plough_grip * dig * load * dt;
    }
    t->jl = t->js = t->jb = 0;
    t->clamped = false;
  }

  /* pass 3: the tyres, as a small sequential impulse solver: each row holds
   * its accumulated impulses inside its friction circle, so the wheels share
   * the work instead of each cancelling the whole skid */
  for (int it = 0; it < TYRE_ITERATIONS; it++) {
    for (int i = 0; i < n; i++) {
      tyre_row *t = &row[i];
      if (!t->on) continue;
      const cw_wheel3d_def *wd = &d->wheels[i];
      wheel_cast *c = &cast[i];
      /* along the wheel: the drive's push and a brake (or a locked wheel) holding the roll at zero */
      float vl = b3Dot(rel_vel(bid, c->ground, t->moving, t->p), t->f);
      float jb = t->brake > 0 ? clampf(t->jb - vl * t->ml, -t->brake, t->brake) : 0;
      float jl = clampf(t->drive + jb, -t->limit, t->limit);
      t->clamped_l = absf(t->drive + jb) > t->limit;
      float dl = jl - t->jl;
      t->jb = jb;
      t->jl = jl;
      if (dl != 0) {
        b3Body_ApplyLinearImpulse(bid, b3MulSV(dl, t->f), t->p, true);
        if (t->gm.dynamic) b3Body_ApplyLinearImpulse(c->ground, b3MulSV(-dl, t->f), t->p, true);
      }
      /* across: take the skid to (1 - lateral_stiffness) of where it began, inside what is left of the circle */
      float vs = b3Dot(rel_vel(bid, c->ground, t->moving, t->pl), t->s);
      float room = t->limit * t->limit - jl * jl;
      room = room > 0 ? sqrtf(room) : 0;
      float want = t->js - (vs - (1 - wd->lateral_stiffness) * t->vs0) * t->ms;
      float js = clampf(want, -room, room);
      t->clamped = absf(want) > room;
      float ds = js - t->js;
      t->js = js;
      if (ds != 0) {
        b3Body_ApplyLinearImpulse(bid, b3MulSV(ds, t->s), t->pl, true);
        if (t->gm.dynamic) b3Body_ApplyLinearImpulse(c->ground, b3MulSV(-ds, t->s), t->pl, true);
      }
    }
  }

  /* outputs, and the wheels' roll */
  for (int i = 0; i < n; i++) {
    const cw_wheel3d_def *wd = &d->wheels[i];
    cw_wheel3d *w = &v->wheel[i];
    const cw_vehicle3d_axle *ax = &v->axle[wd->axle];
    tyre_row *t = &row[i];
    if (!w->contact) {
      /* in the air: a driven wheel spins up, a braked or locked one stops */
      bool lock = w->locked && wd->handbrake_locks;
      if (lock || ax->brake > 0) w->spin_rate *= lock ? 0 : 0.8f;
      else if (ax->drive != 0 && share > 0) w->spin_rate += ax->drive / on_axle[wd->axle] / (share * wd->radius) * dt;
      w->spin_rate *= 1 - clampf(0.5f * dt, 0, 1);
    } else if (!t->on) {
      w->slip_side = w->slip_long = 0;
      w->sliding = false;
    } else {
      w->sliding = t->clamped;
      wheel_cast *c = &cast[i];
      float vl = b3Dot(rel_vel(bid, c->ground, t->moving, t->p), t->f);
      w->slip_side = absf(b3Dot(rel_vel(bid, c->ground, t->moving, t->pl), t->s));
      float want_l = t->drive + t->jb;
      w->slip_long = t->lock ? absf(vl) : (t->ml > 0 && t->clamped_l ? absf(want_l - t->jl) / t->ml : 0);
      /* it rolls with the ground, plus whatever the drive spins past the grip */
      if (t->lock) w->spin_rate = 0;
      else w->spin_rate = (vl + (want_l > t->jl ? w->slip_long : -w->slip_long)) / wd->radius;
    }
    w->spin += w->spin_rate * dt;
    w->spin -= floorf(w->spin / CW_TAU) * CW_TAU;
  }

  /* rolling resistance on the ground, air always: never past a stop */
  b3Vec3 lv = b3Body_GetLinearVelocity(bid);
  float sp = sqrtf(b3Dot(lv, lv));
  if (sp > 1e-4f && mass > 0) {
    float dec = (v->contacts ? d->rolling_resistance * g_ref : 0) + d->drag * sp * sp;
    float dv = dec * dt < sp ? dec * dt : sp;
    if (dv > 0) b3Body_ApplyLinearImpulseToCenter(bid, b3MulSV(-dv * mass / sp, lv), true);
  }
  lv = b3Body_GetLinearVelocity(bid);
  v->speed = b3Dot(lv, fwd);
}

/* ── render transforms ───────────────────────────────────────────────── */

static t3_quat quat_axis(b3Vec3 axis, float angle) {
  float s, c;
  cw_sincos(angle * 0.5f, &s, &c);
  return (t3_quat){ axis.x * s, axis.y * s, axis.z * s, c };
}
static t3_quat quat_mul(t3_quat a, t3_quat b) {
  return (t3_quat){ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

cw_xform3 cw_vehicle3d_wheel_local(const cw_vehicle3d *v, int i, float alpha) {
  if (i < 0 || i >= v->def.wheel_count) return cw_xform3_identity();
  const cw_wheel3d_def *wd = &v->def.wheels[i];
  const cw_wheel3d *w = &v->wheel[i];
  b3Vec3 up = v3(v->def.up), fwd = v3(v->def.forward), left = b3Cross(up, fwd);
  float comp = w->prev_compression + (w->compression - w->prev_compression) * alpha;
  float steer = w->prev_steer + (w->steer - w->prev_steer) * alpha;
  float ds = w->spin - w->prev_spin;
  if (ds > CW_PI) ds -= CW_TAU;
  if (ds < -CW_PI) ds += CW_TAU;
  float spin = w->prev_spin + ds * alpha;
  b3Vec3 p = b3Sub(v3(wd->anchor), b3MulSV(wd->travel - comp, up));
  return (cw_xform3){ t3v(p), quat_mul(quat_axis(up, steer), quat_axis(left, spin)) };
}

cw_xform3 cw_vehicle3d_wheel_world(const cw_vehicle3d *v, int i, float alpha) {
  cw_xform3 l = cw_vehicle3d_wheel_local(v, i, alpha);
  cw_xform3 c = cw_body3d_interpolated(v->chassis, alpha);
  b3Vec3 p = b3RotateVector(q3(c.q), v3(l.p));
  return (cw_xform3){ { c.p.x + p.x, c.p.y + p.y, c.p.z + p.z }, quat_mul(c.q, l.q) };
}
