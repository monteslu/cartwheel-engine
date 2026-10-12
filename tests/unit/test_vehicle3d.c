/* cw_vehicle3d unit suite: a box car on four raycast wheels settles on its
 * springs, drives, brakes, turns the way its wheels point, slides past the
 * friction circle and grips inside it, swings its tail on the handbrake,
 * stands on a wall along its own down, presses a car it stands on, digs in
 * when shoved sideways (ploughing) and interpolates its wheels; the same
 * drive run with one step worker and with all of them gives the same bits. Every numeric result folds into
 * unit.checksum, which the gate compares across hosts. */
#include <string.h>

#include "cartwheel/vehicle3d.h"
#include "test.h"

#define DT (1.0f / 60)
#define NEAR(a, b, eps) (fabsf((a) - (b)) <= (eps))

typedef struct rig {
  cw_world3d w;
  cw_body3d ground, chassis;
  cw_vehicle3d car;
} rig;

static cw_world3d world(t3_vec3 gravity, int workers) {
  cw_world3d_def wd = cw_world3d_default_def();
  wd.gravity = gravity;
  wd.workers = workers;
  wd.sleep = false;
  return cw_world3d_create(&wd);
}

static void floor_box(cw_world3d w, t3_vec3 at, t3_vec3 half) {
  cw_body3d_def bd = cw_body3d_default_def();
  bd.type = CW_BODY_STATIC;
  bd.position = at;
  cw_body3d b = cw_body3d_create(w, &bd);
  cw_shape3d_def sd = cw_shape3d_default_def();
  cw_shape3d_box(b, &sd, half, cw_xform3_identity());
}

/* a 1200 kg box (1.8 x 0.7 x 4.4 m) on wheels 0.35 m, front axle 0, rear 1 (handbrake) */
static cw_body3d make_car(cw_world3d w, cw_vehicle3d *car, t3_vec3 at, t3_quat rot) {
  cw_body3d_def bd = cw_body3d_default_def();
  bd.position = at;
  bd.rotation = rot;
  bd.sleep = false;
  cw_body3d b = cw_body3d_create(w, &bd);
  cw_shape3d_def sd = cw_shape3d_default_def();
  sd.density = 1200.0f / (1.8f * 0.7f * 4.4f);
  sd.material.friction = 0.4f;
  cw_shape3d_box(b, &sd, (t3_vec3){ 0.9f, 0.35f, 2.2f }, cw_xform3_identity());
  cw_vehicle3d_def vd = cw_vehicle3d_default_def();
  for (int i = 0; i < 4; i++) {
    int k = cw_vehicle3d_def_add_wheel(&vd, (t3_vec3){ i & 1 ? -0.8f : 0.8f, -0.2f, i < 2 ? 1.4f : -1.4f }, i < 2 ? 0 : 1);
    vd.wheels[k].handbrake = i >= 2;
  }
  CHECK(cw_vehicle3d_init(car, b, &vd));
  return b;
}

static rig make_rig(int workers) {
  rig r = { 0 };
  r.w = world((t3_vec3){ 0, -10, 0 }, workers);
  floor_box(r.w, (t3_vec3){ 0, -0.5f, 0 }, (t3_vec3){ 300, 0.5f, 300 });
  r.chassis = make_car(r.w, &r.car, (t3_vec3){ 0, 1, 0 }, (t3_quat){ 0, 0, 0, 1 });
  return r;
}

static void tick(rig *r, int n) {
  for (int i = 0; i < n; i++) {
    cw_vehicle3d_update(&r->car, DT);
    cw_world3d_step(r->w, DT);
  }
}

static float yaw_rate(cw_body3d b) { return cw_body3d_angular_velocity(b).y; }

static void test_settle(void) {
  rig r = make_rig(0);
  tick(&r, 150);
  CHECK_EQ_INT(r.car.contacts, 4);
  float stat = 10 / ((2 * CW_PI * 1.6f) * (2 * CW_PI * 1.6f)); /* g / omega^2 */
  float load = 0;
  for (int i = 0; i < 4; i++) {
    CHECK(NEAR(r.car.wheel[i].compression, stat, 0.02f));
    CHECK(NEAR(r.car.wheel[i].normal.y, 1, 1e-4f));
    load += r.car.wheel[i].load;
    FOLD(r.car.wheel[i].compression);
  }
  CHECK(NEAR(load, 1200 * 10, 1200 * 10 * 0.05f));
  t3_vec3 v = cw_body3d_velocity(r.chassis);
  CHECK(fabsf(v.x) + fabsf(v.y) + fabsf(v.z) < 0.05f);
  CHECK(fabsf(r.car.speed) < 0.05f);
  cw_world3d_destroy(r.w);
}

static void test_drive_brake(void) {
  rig r = make_rig(0);
  tick(&r, 60);
  cw_vehicle3d_set_axle(&r.car, 1, 0, 6000, 0);
  tick(&r, 120);
  cw_xform3 x = cw_body3d_transform(r.chassis);
  CHECK(r.car.speed > 8 && r.car.speed < 12); /* 6000 N / 1200 kg = 5 m/s^2 for 2 s, less drag */
  CHECK(x.p.z > 6 && fabsf(x.p.x) < 0.05f);
  CHECK(NEAR(r.car.wheel[0].spin_rate, r.car.speed / 0.35f, 0.5f)); /* the front rolls */
  CHECK(!r.car.wheel[2].sliding);
  float pitch_free = r.car.wheel[0].compression;
  FOLD(x.p);
  FOLD(r.car.speed);
  cw_vehicle3d_set_axle(&r.car, 1, 0, 0, 8000);
  cw_vehicle3d_set_axle(&r.car, 0, 0, 0, 8000);
  tick(&r, 10);
  CHECK(r.car.wheel[0].compression > pitch_free); /* the nose dives under braking */
  tick(&r, 110);
  CHECK(fabsf(r.car.speed) < 0.1f);
  /* backing up */
  cw_vehicle3d_set_axle(&r.car, 0, 0, 0, 0);
  cw_vehicle3d_set_axle(&r.car, 1, 0, -4000, 0);
  tick(&r, 60);
  CHECK(r.car.speed < -2);
  CHECK(r.car.wheel[3].spin_rate < 0);
  FOLD(r.car.speed);
  cw_world3d_destroy(r.w);
}

static void test_steer(void) {
  rig r = make_rig(0);
  tick(&r, 60);
  cw_vehicle3d_set_axle(&r.car, 1, 0, 5000, 0);
  tick(&r, 60);
  cw_vehicle3d_set_axle(&r.car, 0, 0.3f, 0, 0); /* + steers toward the left: +x when facing +z */
  tick(&r, 60);
  cw_xform3 x = cw_body3d_transform(r.chassis);
  CHECK(yaw_rate(r.chassis) > 0.5f);
  CHECK(x.p.x > 1);
  CHECK(NEAR(r.car.wheel[0].steer, 0.3f, 1e-6f));
  /* a gentle turn holds the road: nothing slides */
  for (int i = 0; i < 4; i++) CHECK(r.car.wheel[i].slip_side < 0.5f);
  FOLD(x.p);
  FOLD(x.q);
  cw_world3d_destroy(r.w);
}

static void test_friction_circle(void) {
  rig r = make_rig(0);
  tick(&r, 60);
  /* thrown sideways at 12 m/s: far past the grip, the tyres slide and bleed it off at about slide_grip * g */
  cw_body3d_set_velocity(r.chassis, (t3_vec3){ 12, 0, 0 });
  tick(&r, 1);
  CHECK(cw_body3d_velocity(r.chassis).x > 11);
  for (int i = 0; i < 4; i++) CHECK(r.car.wheel[i].sliding);
  tick(&r, 29);
  float lost = 12 - cw_body3d_velocity(r.chassis).x;
  CHECK(lost > 3 && lost < 6); /* 0.5 s at 0.85 g is 4.25 m/s */
  FOLD(lost);
  /* a small skid sits inside the circle: the contacts stop at once (1 g takes 0.167 m/s a tick),
   * the body rocks on its springs over the stopped tyres and settles */
  tick(&r, 120);
  cw_body3d_set_velocity(r.chassis, (t3_vec3){ 0.1f, 0, 0 });
  tick(&r, 1);
  for (int i = 0; i < 4; i++) CHECK(r.car.wheel[i].slip_side < 0.01f);
  tick(&r, 30);
  CHECK(fabsf(cw_body3d_velocity(r.chassis).x) < 0.01f);
  for (int i = 0; i < 4; i++) CHECK(!r.car.wheel[i].sliding);
  cw_world3d_destroy(r.w);
}

static float turn_after(bool handbrake, bool *rear_locked, float *slip) {
  rig r = make_rig(0);
  tick(&r, 60);
  cw_vehicle3d_set_axle(&r.car, 1, 0, 7000, 0);
  tick(&r, 120);
  cw_vehicle3d_set_axle(&r.car, 1, 0, 0, 0);
  cw_vehicle3d_set_axle(&r.car, 0, 0.25f, 0, 0);
  cw_vehicle3d_set_handbrake(&r.car, handbrake);
  tick(&r, 30);
  float yaw = yaw_rate(r.chassis);
  *rear_locked = r.car.wheel[2].locked && r.car.wheel[3].locked && r.car.wheel[2].spin_rate == 0 && !r.car.wheel[0].locked;
  *slip = r.car.wheel[2].slip_side;
  FOLD(yaw);
  cw_world3d_destroy(r.w);
  return yaw;
}

static void test_handbrake(void) {
  bool locked, free_locked;
  float slip, free_slip;
  float swing = turn_after(true, &locked, &slip), grip = turn_after(false, &free_locked, &free_slip);
  CHECK(locked && !free_locked);
  CHECK(swing > grip * 1.3f); /* the tail comes round */
  CHECK(slip > 1 && slip > free_slip * 3);
}

static void test_wall(void) {
  /* no gravity, a wall at x = 0 and a car standing on it: its up is +x, its down is the wall */
  cw_world3d w = world((t3_vec3){ 0, 0, 0 }, 0);
  floor_box(w, (t3_vec3){ -0.5f, 0, 0 }, (t3_vec3){ 0.5f, 100, 100 });
  float h = 0.70710678f;
  cw_vehicle3d car;
  cw_body3d b = make_car(w, &car, (t3_vec3){ 0.8f, 0, 0 }, (t3_quat){ 0, 0, -h, h }); /* -90 about z: y -> +x */
  cw_body3d_set_velocity(b, (t3_vec3){ -0.5f, 0, 0 });
  /* the game's own stick force holds it to the wall (gravity's job on a floor) */
  t3_vec3 stick = { -1200 * 10, 0, 0 };
  for (int i = 0; i < 60; i++) {
    cw_body3d_apply_force(b, stick, NULL);
    cw_vehicle3d_update(&car, DT);
    cw_world3d_step(w, DT);
  }
  CHECK_EQ_INT(car.contacts, 4);
  CHECK(NEAR(car.wheel[1].normal.x, 1, 1e-4f));
  CHECK(NEAR(car.up_world.x, 1, 1e-3f));
  /* it drives along the wall: forward is still +z */
  cw_vehicle3d_set_axle(&car, 1, 0, 3000, 0);
  for (int i = 0; i < 60; i++) {
    cw_body3d_apply_force(b, stick, NULL);
    cw_vehicle3d_update(&car, DT);
    cw_world3d_step(w, DT);
  }
  cw_xform3 x = cw_body3d_transform(b);
  CHECK(car.speed > 1.5f && x.p.z > 0.5f);
  CHECK(x.p.x > 0.3f && x.p.x < 1.5f);
  FOLD(x.p);
  cw_world3d_destroy(w);
}

static void test_ride_on_car(void) {
  /* car b parked on car a's roof: b's wheels stand on a's body, and a's springs carry both */
  cw_world3d w = world((t3_vec3){ 0, -10, 0 }, 0);
  floor_box(w, (t3_vec3){ 0, -0.5f, 0 }, (t3_vec3){ 100, 0.5f, 100 });
  cw_vehicle3d a, b;
  make_car(w, &a, (t3_vec3){ 0, 1, 0 }, (t3_quat){ 0, 0, 0, 1 });
  cw_body3d bb = make_car(w, &b, (t3_vec3){ 0, 2.2f, 0 }, (t3_quat){ 0, 0, 0, 1 });
  for (int i = 0; i < 150; i++) {
    cw_vehicle3d_update(&a, DT);
    cw_vehicle3d_update(&b, DT);
    cw_world3d_step(w, DT);
  }
  CHECK_EQ_INT(b.contacts, 4);
  CHECK(b.wheel[0].ground.id == a.chassis.id);
  float la = 0, lb = 0;
  for (int i = 0; i < 4; i++) la += a.wheel[i].load, lb += b.wheel[i].load;
  CHECK(NEAR(lb, 12000, 600));
  CHECK(NEAR(la, 24000, 1200));
  CHECK(fabsf(cw_body3d_velocity(bb).y) < 0.05f);
  FOLD(la);
  FOLD(lb);
  cw_world3d_destroy(w);
}

static void test_wheel_transforms(void) {
  rig r = make_rig(0);
  cw_vehicle3d_set_axle(&r.car, 1, 0, 6000, 0);
  cw_vehicle3d_set_axle(&r.car, 0, 0.2f, 0, 0);
  tick(&r, 90);
  const cw_wheel3d *w = &r.car.wheel[0];
  cw_xform3 a = cw_vehicle3d_wheel_local(&r.car, 0, 0), b = cw_vehicle3d_wheel_local(&r.car, 0, 1);
  /* the centre sits down the suspension by travel - compression */
  CHECK(NEAR(a.p.y, -0.2f - (0.25f - w->prev_compression), 1e-5f));
  CHECK(NEAR(b.p.y, -0.2f - (0.25f - w->compression), 1e-5f));
  CHECK(NEAR(b.p.x, 0.8f, 1e-6f) && NEAR(b.p.z, 1.4f, 1e-6f));
  /* steer about up, then spin about the axle: the wheel's axle (local x) stays level with steer applied */
  t3_quat q = b.q;
  float ax_y = 2 * (q.x * q.y + q.w * q.z); /* the rotated x axis's y */
  CHECK(fabsf(ax_y) < 1e-4f);
  /* the world transform sits on the chassis */
  cw_xform3 c = cw_body3d_interpolated(r.chassis, 1), ww = cw_vehicle3d_wheel_world(&r.car, 0, 1);
  float dx = ww.p.x - c.p.x, dy = ww.p.y - c.p.y, dz = ww.p.z - c.p.z;
  CHECK(NEAR(sqrtf(dx * dx + dy * dy + dz * dz), sqrtf(b.p.x * b.p.x + b.p.y * b.p.y + b.p.z * b.p.z), 1e-4f));
  /* half way between two spins takes the short way round */
  cw_vehicle3d copy = r.car;
  copy.wheel[0].prev_spin = CW_TAU - 0.1f;
  copy.wheel[0].spin = 0.1f;
  copy.wheel[0].prev_steer = copy.wheel[0].steer = 0;
  cw_xform3 m = cw_vehicle3d_wheel_local(&copy, 0, 0.5f);
  CHECK(NEAR(fabsf(m.q.w), 1, 1e-5f));
  cw_vehicle3d_reset(&r.car);
  CHECK(r.car.wheel[0].compression == 0 && r.car.wheel[0].spin == 0 && r.car.wheel[0].steer == 0.2f);
  FOLD(b);
  cw_world3d_destroy(r.w);
}

static void test_teleport_active(void) {
  rig r = make_rig(0);
  tick(&r, 90);
  CHECK(cw_vehicle3d_grounded(&r.car, true) && cw_vehicle3d_grounded(&r.car, false));
  /* off: a ragdoll, no forces: it falls through its springs onto its body */
  cw_vehicle3d_set_active(&r.car, false);
  tick(&r, 60);
  CHECK(!cw_vehicle3d_grounded(&r.car, false));
  CHECK(r.car.wheel[0].compression == 0 && r.car.wheel[0].load == 0);
  cw_xform3 x = cw_body3d_transform(r.chassis);
  CHECK(x.p.y < 0.4f); /* resting on the box: half height 0.35 */
  /* on again: back up on its wheels */
  cw_vehicle3d_set_active(&r.car, true);
  tick(&r, 120);
  CHECK(cw_vehicle3d_grounded(&r.car, true));
  CHECK(cw_body3d_transform(r.chassis).p.y > 0.6f);
  /* teleported upside down and moving: placed still, no history */
  cw_vehicle3d_set_axle(&r.car, 1, 0, 3000, 0);
  tick(&r, 30);
  cw_vehicle3d_teleport(&r.car, (cw_xform3){ { 5, 1, 5 }, { 0, 0, 0, 1 } });
  t3_vec3 v = cw_body3d_velocity(r.chassis);
  CHECK(v.x == 0 && v.y == 0 && v.z == 0);
  CHECK(r.car.contacts == 0 && r.car.wheel[2].spin_rate == 0 && r.car.wheel[2].slip_side == 0 && !r.car.wheel[2].sliding);
  cw_xform3 a = cw_vehicle3d_wheel_local(&r.car, 2, 0), b = cw_vehicle3d_wheel_local(&r.car, 2, 1);
  CHECK(a.p.y == b.p.y);
  FOLD(x.p);
  cw_world3d_destroy(r.w);
}

static void test_powerslide(void) {
  /* a handbrake that only lowers the rear grip: the drive still pushes, the wheels still turn, the tail slides */
  rig r = make_rig(0);
  r.car.def.wheels[2].handbrake_locks = r.car.def.wheels[3].handbrake_locks = false;
  tick(&r, 60);
  cw_vehicle3d_set_axle(&r.car, 1, 0, 7000, 0);
  tick(&r, 100);
  cw_vehicle3d_set_axle(&r.car, 0, 0.3f, 0, 0);
  cw_vehicle3d_set_handbrake(&r.car, true);
  tick(&r, 25);
  CHECK(r.car.wheel[2].locked && r.car.wheel[2].spin_rate > 10);
  CHECK(r.car.wheel[2].slip_side > 1);
  CHECK(r.car.speed > 8);
  FOLD(r.car.speed);
  cw_world3d_destroy(r.w);
}

static float slide_after(float plough, float speed, float *roll) {
  rig r = make_rig(0);
  for (int i = 0; i < 4; i++) r.car.def.wheels[i].plough_grip = plough;
  tick(&r, 60);
  cw_body3d_set_velocity(r.chassis, (t3_vec3){ speed, 0, 0 });
  float up = 1;
  for (int t = 0; t < 20; t++) {
    tick(&r, 1);
    if (r.car.up_world.y < up) up = r.car.up_world.y;
  }
  float v = cw_body3d_velocity(r.chassis).x;
  *roll = up;
  cw_world3d_destroy(r.w);
  return v;
}

static void test_plough(void) {
  /* shoved sideways past plough_speed the tyres dig in: the slide stops sooner and the body leans further
   * over its outside wheels; below it nothing changes */
  float roll_plain, roll_dig, slow_plain, slow_dig;
  float plain = slide_after(0, 12, &roll_plain), dig = slide_after(2, 12, &roll_dig);
  CHECK(12 - dig > (12 - plain) * 1.3f);
  CHECK(roll_dig < roll_plain);
  float a = slide_after(0, 4, &slow_plain), b = slide_after(2, 4, &slow_dig);
  CHECK(a == b && slow_plain == slow_dig);
  FOLD(dig);
  FOLD(roll_dig);
}

/* a lap with throttle, steer, handbrake and brake; returns the chassis bits */
static void run_lap(int workers, float out[8]) {
  rig r = make_rig(workers);
  rig o = { 0 };
  /* a second car in the way: a crash and a ride-up in the run */
  cw_vehicle3d other;
  o.chassis = make_car(r.w, &other, (t3_vec3){ 2.5f, 1, 18 }, (t3_quat){ 0, 0.38268343f, 0, 0.92387953f });
  for (int t = 0; t < 420; t++) {
    float steer = t > 120 && t < 200 ? 0.35f : t > 260 && t < 300 ? -0.4f : 0;
    cw_vehicle3d_set_axle(&r.car, 0, steer, 0, t > 360 ? 9000 : 0);
    cw_vehicle3d_set_axle(&r.car, 1, 0, t < 330 ? 8000 : 0, t > 360 ? 9000 : 0);
    cw_vehicle3d_set_handbrake(&r.car, t > 160 && t < 185);
    cw_vehicle3d_update(&r.car, DT);
    cw_vehicle3d_update(&other, DT);
    cw_world3d_step(r.w, DT);
  }
  cw_xform3 x = cw_body3d_transform(r.chassis);
  t3_vec3 v = cw_body3d_velocity(o.chassis);
  float k[8] = { x.p.x, x.p.y, x.p.z, x.q.x, x.q.y, v.x, v.z, r.car.wheel[2].spin };
  memcpy(out, k, sizeof k);
  cw_world3d_destroy(r.w);
}

static void test_workers(void) {
  float one[8], all[8], again[8];
  run_lap(1, one);
  run_lap(0, all);
  run_lap(0, again);
  CHECK(memcmp(one, all, sizeof one) == 0);
  CHECK(memcmp(all, again, sizeof all) == 0);
  CHECK(one[2] > 10); /* it went somewhere */
  FOLD(all);
}

CW_TEST_SUITE("vehicle3d", { "settle", test_settle }, { "drive_brake", test_drive_brake }, { "steer", test_steer },
              { "friction_circle", test_friction_circle }, { "handbrake", test_handbrake }, { "wall", test_wall },
              { "ride_on_car", test_ride_on_car }, { "wheel_transforms", test_wheel_transforms },
              { "teleport_active", test_teleport_active },
              { "powerslide", test_powerslide }, { "plough", test_plough }, { "workers", test_workers })
