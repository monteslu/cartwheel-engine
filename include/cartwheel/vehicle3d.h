/* cartwheel 3D vehicles: a wheeled body on Box3D (physics3d.h) with
 * raycast suspension and a friction-circle tyre model. Cars, karts, trucks,
 * buggies, an arcade car-ball that drives up walls.
 *
 * The chassis is an ordinary dynamic cw_body3d you make (its shapes give it
 * mass, inertia and its crashes); the wheels are not bodies. Each tick,
 * before cw_world3d_step, cw_vehicle3d_update casts one ray per wheel from
 * its anchor along the chassis's LOCAL down (so a car on a wall or a ceiling
 * still stands on its wheels there), and where a wheel touches it applies
 * impulses to the chassis (and the reaction to the ground, when the ground
 * is a dynamic body: a car riding up on another pushes it down):
 *
 *   suspension  a spring and damper along the suspension axis, from the
 *               wheel's hertz and damping ratio and its share of the sprung
 *               mass, never pulling; a bump stop at full compression
 *   tyre        drive, brake and sideways grip at the contact share one
 *               friction circle of radius grip * load: inside it the tyre
 *               holds (lateral_stiffness of the sideways skid cancelled per
 *               tick: 1 = rails), past it the tyre slides, and a sliding
 *               tyre keeps slide_grip of its grip until it slows back
 *               inside the circle, so a car carves at low speed and drifts
 *               when pushed
 *   ploughing   (plough_grip, off by default) a wheel sliding sideways
 *               faster than plough_speed digs into loose ground and grips
 *               harder, at the contact: what trips and rolls a car shoved
 *               sideways on dirt (roll_influence 1)
 *   handbrake   drops the grip of the wheels marked handbrake to
 *               handbrake_grip and (handbrake_locks) locks them: the rear
 *               swings round
 *   resistance  rolling resistance while a wheel touches, air drag always
 *
 * Steering, drive and brake are set per axle (a wheel names its axle):
 * cw_vehicle3d_set_axle. Everything the game might read is a plain field of
 * cw_vehicle3d / cw_wheel3d (contact, normal, compression, load, slip, spin).
 * The wheels' render transforms interpolate like bodies do.
 *
 * Frame of the chassis (local): up (0, 1, 0), forward (0, 0, 1) by default;
 * left is up x forward. A positive steer angle turns a wheel's front toward
 * the left (counter-clockwise seen from above); a positive spin rolls it
 * forward.
 *
 * Typical tick:
 *   init:          chassis = cw_body3d_create(...) + shapes; cw_vehicle3d_init(&car, chassis, &def)
 *   fixed_update:  cw_vehicle3d_set_axle(&car, 0, steer, 0, brake);
 *                  cw_vehicle3d_set_axle(&car, 1, 0, drive, brake);
 *                  cw_vehicle3d_update(&car, dt); cw_world3d_step(w, dt);
 *   render:        cw_world3d_sync(w, alpha); wheel i at cw_vehicle3d_wheel_world(&car, i, alpha)
 *
 * Determinism: only + - * / sqrt and cartwheel/math.h, sorted world queries,
 * wheels in index order: the same inputs give the same bits on every host
 * and with any number of step workers (cw_world3d_def.workers, 0 = all).
 * Threads: main thread, outside the step, like the rest of physics3d.h.
 * A cw_vehicle3d is a plain struct owned by the caller (no allocation); it
 * holds the chassis handle but does not own the body. */
#ifndef CW_VEHICLE3D_H
#define CW_VEHICLE3D_H

#include "physics3d.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_VEHICLE3D_WHEELS 8
#define CW_VEHICLE3D_AXLES 4

typedef struct cw_wheel3d_def {
  t3_vec3 anchor;          /* chassis-local wheel centre at FULL compression (the top of its travel) */
  int axle;                /* 0..CW_VEHICLE3D_AXLES-1: 0 */
  float radius;            /* m: 0.35 */
  float travel;            /* m of suspension, full compression to full droop: 0.25 */
  float hertz;             /* suspension frequency with its share of the mass: 1.6 */
  float damping_ratio;     /* 0.5 */
  float grip;              /* friction coefficient of the tyre (the ground's material is not used): 1 */
  float slide_grip;        /* share of grip left while the tyre slides: 0.85 */
  float lateral_stiffness; /* share of the sideways skid cancelled per tick inside the circle, 0..1: 1 */
  float plough_grip;       /* loose ground: extra friction a wheel builds sliding sideways, as it digs a
                              furrow (soil tripping, what rolls a car shoved sideways on dirt): 0 = off */
  float plough_speed;      /* m/s of sideways slide where ploughing starts; full grip at twice it: 6 */
  bool handbrake;          /* the handbrake acts on this wheel */
  bool handbrake_locks;    /* it locks the wheel (stops its roll: a skid); false = it only lowers the
                              grip (a powerslide with the drive still on): true */
  float handbrake_grip;    /* share of grip under the handbrake: 0.35 */
} cw_wheel3d_def;
/* The defaults noted in cw_wheel3d_def above, at anchor. */
cw_wheel3d_def cw_wheel3d_default_def(t3_vec3 anchor, int axle);

typedef struct cw_vehicle3d_def {
  int wheel_count;         /* 1..CW_VEHICLE3D_WHEELS */
  cw_wheel3d_def wheels[CW_VEHICLE3D_WHEELS];
  t3_vec3 up, forward;     /* chassis-local axes: (0, 1, 0), (0, 0, 1); made orthonormal */
  uint32_t mask;           /* layers the wheels stand on: all (the chassis itself is always skipped) */
  float sprung_mass;       /* kg shared by the wheels; 0 = the chassis's mass at update time */
  float roll_influence;    /* 0..1: where sideways grip acts, from the centre of mass's height (0: no
                              body roll from cornering) to the contact (1, physical): 1 */
  float rolling_resistance;/* deceleration while a wheel touches, as a share of g: 0.015 */
  float drag;              /* air: deceleration per (m/s)^2, 1/m: 0.0005 */
  float max_load;          /* the most a wheel's grip counts, x its static share of the weight: 4 */
} cw_vehicle3d_def;
/* The defaults noted above with no wheels. */
cw_vehicle3d_def cw_vehicle3d_default_def(void);
/* Adds a wheel (the defaults at anchor on axle); returns its index, -1 when full. */
int cw_vehicle3d_def_add_wheel(cw_vehicle3d_def *def, t3_vec3 anchor, int axle);

/* One wheel's state; every field is an output of cw_vehicle3d_update. */
typedef struct cw_wheel3d {
  bool contact;            /* touching something this tick */
  t3_vec3 point, normal;   /* contact point and surface normal (world) */
  cw_body3d ground;        /* what it stands on (null in the air) */
  cw_shape3d ground_shape;
  uint64_t ground_material;/* the ground shape's user material */
  float compression;       /* m, 0 (full droop / in the air) .. travel */
  float load;              /* N: the suspension's push this tick */
  float slip_side;         /* m/s of sideways skid left after the tyre's grip */
  float slip_long;         /* m/s of the contact against the wheel's roll (locked wheel: the road speed) */
  bool sliding;            /* past the friction circle (keeps slide_grip until back inside) */
  bool locked;             /* under the handbrake (locked when handbrake_locks) */
  float steer;             /* radians, from its axle */
  float spin, spin_rate;   /* radians (0..2pi) and rad/s about its axle; positive rolls forward */
  /* internal: the last tick's values for interpolation */
  float prev_compression, prev_spin, prev_steer;
} cw_wheel3d;

typedef struct cw_vehicle3d_axle {
  float steer;             /* radians, + toward the left */
  float drive;             /* N at the contacts, shared by the axle's touching wheels; < 0 backs up */
  float brake;             /* N of braking at most, shared by the axle's wheels */
} cw_vehicle3d_axle;

typedef struct cw_vehicle3d {
  cw_world3d world;
  cw_body3d chassis;
  cw_vehicle3d_def def;
  cw_wheel3d wheel[CW_VEHICLE3D_WHEELS];
  cw_vehicle3d_axle axle[CW_VEHICLE3D_AXLES];
  bool handbrake;
  bool active;             /* false: update applies nothing (cw_vehicle3d_set_active) */
  /* outputs of cw_vehicle3d_update */
  int contacts;            /* wheels touching */
  float speed;             /* m/s along the chassis forward (negative backing up) */
  t3_vec3 up_world, forward_world; /* the chassis axes in world space */
} cw_vehicle3d;

/* Clears v, copies def (axes made orthonormal, axles clamped) and binds the
 * chassis. The wheels start at full droop. False (logged) for an invalid
 * chassis or a def that is NULL or has no wheels. */
bool cw_vehicle3d_init(cw_vehicle3d *v, cw_body3d chassis, const cw_vehicle3d_def *def);
/* Steering angle, drive force and brake force for one axle (kept until changed). */
void cw_vehicle3d_set_axle(cw_vehicle3d *v, int axle, float steer, float drive, float brake);
/* Locks the handbrake wheels (kept until changed). */
void cw_vehicle3d_set_handbrake(cw_vehicle3d *v, bool on);
/* One fixed tick of dt seconds, before cw_world3d_step: casts the wheels,
 * applies suspension, tyre, rolling and air impulses, and updates every
 * output. A sleeping chassis with no drive and no ground motion stays
 * asleep. Cost: one ray cast per wheel. */
void cw_vehicle3d_update(cw_vehicle3d *v, float dt);
/* Forgets the suspension, slip and spin history: every wheel at full droop
 * and out of contact, nothing to interpolate from. Controls are kept. */
void cw_vehicle3d_reset(cw_vehicle3d *v);
/* Respawn / kickoff: moves the chassis to x with no velocity and no
 * interpolation from the old place (cw_body3d_set_transform), then
 * cw_vehicle3d_reset. */
void cw_vehicle3d_teleport(cw_vehicle3d *v, cw_xform3 x);
/* Off: update applies no force at all (the chassis tumbles as a plain body,
 * a ragdoll), the wheels read out of contact and hang at full droop. On
 * again: it picks up from the next update. On after init. */
void cw_vehicle3d_set_active(cw_vehicle3d *v, bool on);
/* Wheels touching after the last update: any (all = false) or every one (all = true). */
bool cw_vehicle3d_grounded(const cw_vehicle3d *v, bool all);

/* Wheel i's transform in the chassis's frame: its centre down the suspension
 * axis by the compression, turned by its steer about up and by its spin
 * about the axle, interpolated between the last two updates by alpha. */
cw_xform3 cw_vehicle3d_wheel_local(const cw_vehicle3d *v, int i, float alpha);
/* The same in world space, on the chassis's interpolated transform
 * (cw_body3d_interpolated): what a renderer draws. */
cw_xform3 cw_vehicle3d_wheel_world(const cw_vehicle3d *v, int i, float alpha);

#ifdef __cplusplus
}
#endif
#endif
