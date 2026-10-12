/* cartwheel 2D physics: Box2D v3 (deps/box2d), mirrored name for name by
 * physics3d.h. Read physics.h first: layers, events, filters, stats, debug
 * lines and the handle/ownership rules are shared and documented there.
 *
 * Typical frame:
 *   init:          w = cw_world2d_create(&def); bodies, shapes, joints
 *   fixed_update:  movers / forces / kinematic targets, cw_world2d_step(w, dt),
 *                  then read cw_world2d_events(w, &n)
 *   render:        cw_world2d_sync(w, alpha) writes bound objects
 *
 * Vectors are cw_vec2 (same layout as b2Vec2). Rotations are a unit
 * (cos, sin) pair, Box2D's b2Rot: no trig in the simulation path; use
 * cw_rot2_angle / cw_rot2_from_angle (cartwheel/math.h, deterministic) to
 * convert. Units: meters, kilograms, seconds, radians. y is up. */
#ifndef CW_PHYSICS2D_H
#define CW_PHYSICS2D_H

#include "box2d/box2d.h"
#include "physics.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

CW_HANDLE_TYPE(cw_world2d);
CW_HANDLE_TYPE(cw_body2d);
CW_HANDLE_TYPE(cw_shape2d);
CW_HANDLE_TYPE(cw_joint2d);
CW_HANDLE_TYPE(cw_chain2d);

typedef struct cw_rot2 {
  float c, s;
} cw_rot2;
typedef struct cw_xform2 {
  cw_vec2 p;
  cw_rot2 q;
} cw_xform2;
/* Origin, rotation (1, 0). */
static inline cw_xform2 cw_xform2_identity(void) { return (cw_xform2){ { 0, 0 }, { 1, 0 } }; }
/* Position (x, y), no rotation. */
static inline cw_xform2 cw_xform2_at(float x, float y) { return (cw_xform2){ { x, y }, { 1, 0 } }; }
/* Unit rotation for an angle in radians (deterministic sincos). */
static inline cw_rot2 cw_rot2_from_angle(float radians) {
  cw_rot2 r;
  cw_sincos(radians, &r.s, &r.c);
  return r;
}
/* Angle in radians, -pi..pi (deterministic atan2). */
static inline float cw_rot2_angle(cw_rot2 r) { return cw_atan2(r.s, r.c); }
/* Linear position, normalized-lerp rotation. Deterministic, meant for rendering. */
cw_xform2 cw_xform2_lerp(cw_xform2 a, cw_xform2 b, float t);

/* ── worlds ──────────────────────────────────────────────────────────── */

typedef struct cw_world2d_def {
  cw_vec2 gravity;             /* (0, -10) */
  int substeps;                /* 4 */
  int workers;                 /* job-pool threads the step may use: 0 = all */
  float hit_speed;             /* 1 m/s */
  float restitution_threshold; /* 1 m/s */
  bool sleep, continuous;      /* true, true */
  const char *layers[CW_PHYS_LAYERS];
} cw_world2d_def;
/* The defaults noted in cw_world2d_def above. */
cw_world2d_def cw_world2d_default_def(void);

/* Box2D world stepped on the job pool; null (logged) past Box2D's 128-world limit. def NULL = defaults. */
cw_world2d cw_world2d_create(const cw_world2d_def *def);
/* Destroys everything in the world and releases its task context. */
void cw_world2d_destroy(cw_world2d w);
/* False for null, destroyed or stale handles. */
bool cw_world2d_valid(cw_world2d w);
/* One fixed step; see cw_world3d_step. */
void cw_world2d_step(cw_world2d w, float dt);

typedef struct cw_phys_event2d {
  cw_phys_event_type type;
  cw_shape2d shape_a, shape_b;
  cw_body2d body_a, body_b;
  cw_joint2d joint;
  cw_vec2 point, normal;
  float speed;
  uint64_t material_a, material_b;
  void *user;
} cw_phys_event2d;
/* The last step's events; see cw_world3d_events. Valid until the next step or destroy. */
const cw_phys_event2d *cw_world2d_events(cw_world2d w, int *count);

/* Bound three.c objects get position x,y (z kept) and a rotation about z;
 * bound cw_xform2 get the transform. */
void cw_world2d_sync(cw_world2d w, float alpha);
/* See cw_world3d_stats. */
cw_phys_stats cw_world2d_stats(cw_world2d w);
/* m/s^2; ignored for an invalid world. */
void cw_world2d_set_gravity(cw_world2d w, cw_vec2 g);
/* m/s^2; zero for an invalid world. */
cw_vec2 cw_world2d_gravity(cw_world2d w);
/* Layer index by name, -1 (logged) when unknown. */
int cw_world2d_layer(cw_world2d w, const char *name);
/* Bits for a "world|enemy" style list; unknown names are logged and skipped. */
uint32_t cw_world2d_layer_bits(cw_world2d w, const char *names);
/* Appends wireframes (z = 0) as asked by flags; see cw_world3d_debug_draw. */
void cw_world2d_debug_draw(cw_world2d w, cw_phys_lines *out, uint32_t flags);
/* The Box2D world id (null id for a null handle). Do not destroy it or set its user data. */
b2WorldId cw_world2d_raw(cw_world2d w);

/* ── bodies ──────────────────────────────────────────────────────────── */

typedef struct cw_body2d_def {
  cw_body_type type;           /* CW_BODY_DYNAMIC */
  cw_vec2 position;
  cw_rot2 rotation;            /* (1, 0) */
  cw_vec2 linear_velocity;
  float angular_velocity;
  float linear_damping, angular_damping, gravity_scale; /* 0, 0, 1 */
  bool lock_rotation, bullet;
  bool awake, sleep, interpolate; /* true, true, true */
  const char *name;
  void *user;
} cw_body2d_def;
/* The defaults noted in cw_body2d_def above. */
cw_body2d_def cw_body2d_default_def(void);

/* Null (logged) on an invalid world. def NULL = defaults. */
cw_body2d cw_body2d_create(cw_world2d w, const cw_body2d_def *def);
/* Destroys the body with its shapes, chains and joints. Logged no-op on an invalid handle. */
void cw_body2d_destroy(cw_body2d b);
/* False for null, destroyed or stale handles. */
bool cw_body2d_valid(cw_body2d b);
/* Owning world; null (logged) for an invalid body. */
cw_world2d cw_body2d_world(cw_body2d b);
/* CW_BODY_STATIC (logged) for an invalid body. */
cw_body_type cw_body2d_type(cw_body2d b);
/* Switches static / kinematic / dynamic at run time. */
void cw_body2d_set_type(cw_body2d b, cw_body_type type);
/* Current transform (last step, not interpolated); identity for an invalid body. */
cw_xform2 cw_body2d_transform(cw_body2d b);
/* Teleport: no interpolation from the old place. Wakes the body. */
void cw_body2d_set_transform(cw_body2d b, cw_xform2 x);
/* Velocity that reaches target in dt; see cw_body3d_set_target. */
void cw_body2d_set_target(cw_body2d b, cw_xform2 target, float dt);
/* m/s; zero (logged) for an invalid body. */
cw_vec2 cw_body2d_velocity(cw_body2d b);
/* m/s. */
void cw_body2d_set_velocity(cw_body2d b, cw_vec2 v);
/* rad/s, counter-clockwise positive; 0 for an invalid body. */
float cw_body2d_angular_velocity(cw_body2d b);
/* rad/s, counter-clockwise positive. */
void cw_body2d_set_angular_velocity(cw_body2d b, float w);
/* N at a world point (NULL = center of mass), for the next step. Wakes the body. */
void cw_body2d_apply_force(cw_body2d b, cw_vec2 force, const cw_vec2 *point);
/* N*s at a world point (NULL = center of mass). Wakes the body. */
void cw_body2d_apply_impulse(cw_body2d b, cw_vec2 impulse, const cw_vec2 *point);
/* N*m for the next step. Wakes the body. */
void cw_body2d_apply_torque(cw_body2d b, float torque);
/* N*m*s. Wakes the body. */
void cw_body2d_apply_angular_impulse(cw_body2d b, float impulse);
/* kg; 0 for static/kinematic or invalid bodies. */
float cw_body2d_mass(cw_body2d b);
/* False for a sleeping or invalid body. */
bool cw_body2d_awake(cw_body2d b);
/* Wakes or puts to sleep the body. */
void cw_body2d_set_awake(cw_body2d b, bool awake);
/* Your pointer; ignored for an invalid body. */
void cw_body2d_set_user(cw_body2d b, void *user);
/* NULL for an invalid body. */
void *cw_body2d_user(cw_body2d b);
/* Sync target (replaces the previous one, NULL unbinds); must outlive the binding. */
void cw_body2d_bind_object(cw_body2d b, t3_object *obj);
/* Plain transform sync target (replaces an object binding); must outlive the binding. */
void cw_body2d_bind_xform(cw_body2d b, cw_xform2 *dst);
/* The transform sync would write for this alpha; identity for an invalid body. */
cw_xform2 cw_body2d_interpolated(cw_body2d b, float alpha);
/* The Box2D body id. Do not destroy it through Box2D or set its user data. */
b2BodyId cw_body2d_raw(cw_body2d b);

/* ── shapes ──────────────────────────────────────────────────────────── */

typedef struct cw_shape2d_def {
  cw_phys_material material;
  float density;               /* kg/m^2: 1 */
  int layer;
  uint32_t mask;
  int group;
  bool sensor, contact_events, sensor_events, hit_events; /* false, true, true, false */
  void *user;
} cw_shape2d_def;
/* The defaults noted in cw_shape2d_def above. */
cw_shape2d_def cw_shape2d_default_def(void);

/* Box with half extents half, placed by local in the body frame. Null (logged) for an invalid body. */
cw_shape2d cw_shape2d_box(cw_body2d b, const cw_shape2d_def *def, cw_vec2 half, cw_xform2 local);
/* Circle at body-local center. Null (logged) for an invalid body. */
cw_shape2d cw_shape2d_circle(cw_body2d b, const cw_shape2d_def *def, cw_vec2 center, float radius);
/* Capsule between body-local centers a and c. */
cw_shape2d cw_shape2d_capsule(cw_body2d b, const cw_shape2d_def *def, cw_vec2 a, cw_vec2 c, float radius);
/* Convex hull of up to 8 body-local points, rounded by radius. Null
 * (logged) when degenerate. */
cw_shape2d cw_shape2d_polygon(cw_body2d b, const cw_shape2d_def *def, const cw_vec2 *points, int count, float radius);
/* Two-sided line segment a..c (body-local); adds no mass. Null (logged) for an invalid body. */
cw_shape2d cw_shape2d_segment(cw_body2d b, const cw_shape2d_def *def, cw_vec2 a, cw_vec2 c);
/* Updates the body's mass; no-op on an invalid handle. */
void cw_shape2d_destroy(cw_shape2d s);
/* False for null, destroyed or stale handles. */
bool cw_shape2d_valid(cw_shape2d s);
/* Owning body; null for an invalid shape. */
cw_body2d cw_shape2d_body(cw_shape2d s);
/* def.user; NULL for an invalid shape. */
void *cw_shape2d_user(cw_shape2d s);
/* False for an invalid shape. */
bool cw_shape2d_is_sensor(cw_shape2d s);
/* Replaces friction, restitution, rolling resistance, tangent speed and user material. */
void cw_shape2d_set_material(cw_shape2d s, const cw_phys_material *m);
/* All zero for an invalid shape. */
cw_phys_material cw_shape2d_material(cw_shape2d s);
/* Moves the shape to layer (0..31) with a new mask; about as costly as recreating the shape. */
void cw_shape2d_set_layer(cw_shape2d s, int layer, uint32_t mask);
/* The Box2D shape id. Leave its filter and user data to the wrapper. */
b2ShapeId cw_shape2d_raw(cw_shape2d s);

/* Chain: one-sided segments through `count` points (>= 4 for loop=false,
 * >= 3 for a loop), collision on the right side walking the points (use
 * counter-clockwise points for terrain seen from above: solid below). The
 * 2D counterpart of a static triangle mesh. Points are copied. Uses the
 * def's material, layer, mask and sensor_events (chains are never sensors). */
cw_chain2d cw_chain2d_create(cw_body2d b, const cw_shape2d_def *def, const cw_vec2 *points, int count, bool loop);
/* Destroys the chain and its segments; no-op on an invalid handle. */
void cw_chain2d_destroy(cw_chain2d c);
/* The Box2D chain id. */
b2ChainId cw_chain2d_raw(cw_chain2d c);

/* ── joints (cw_joint_type, physics.h; SPHERICAL/PARALLEL are 3D only) ── */

typedef struct cw_joint2d_def {
  cw_joint_type type;
  cw_body2d a, b;
  /* Local frames. PRISMATIC / WHEEL travel along x of frame a; REVOLUTE
   * turns about the frames' origin. */
  cw_xform2 frame_a, frame_b;
  bool collide_connected;
  float break_force, break_torque;
  bool spring;
  float hertz, damping_ratio;
  bool limit;
  float lower, upper;
  bool motor;
  float motor_speed, max_motor;
  float length;
  float max_force;
  cw_vec2 target;
  void *user;
} cw_joint2d_def;
/* Identity frames and per-type spring defaults; see cw_joint3d_default_def. */
cw_joint2d_def cw_joint2d_default_def(cw_joint_type type);
/* Fill frame_a/frame_b from a world anchor; frame x maps to world_axis (zero = +x). */
void cw_joint2d_def_anchor(cw_joint2d_def *def, cw_vec2 world_anchor, cw_vec2 world_axis);
/* a null = a static world body. Null (logged) for a NULL def, SPHERICAL/PARALLEL or bad bodies. */
cw_joint2d cw_joint2d_create(cw_world2d w, const cw_joint2d_def *def);
/* Also frees a MOUSE joint's internal body. No-op on an invalid handle; no break event. */
void cw_joint2d_destroy(cw_joint2d j);
/* False for null, destroyed (or broken) and stale handles. */
bool cw_joint2d_valid(cw_joint2d j);
/* CW_JOINT_TYPE_COUNT for an invalid joint. */
cw_joint_type cw_joint2d_type(cw_joint2d j);
/* Your pointer, reported again in the joint break event. */
void cw_joint2d_set_user(cw_joint2d j, void *user);
/* NULL for an invalid joint. */
void *cw_joint2d_user(cw_joint2d j);
/* Revolute, prismatic, wheel, distance; others ignored. Wakes the bodies. */
void cw_joint2d_set_motor(cw_joint2d j, bool on, float speed, float max);
/* Revolute angle, prismatic/wheel translation, distance length; others ignored. */
void cw_joint2d_set_limits(cw_joint2d j, bool on, float lower, float upper);
/* Revolute, prismatic, wheel, distance; weld / motor take hertz and damping (on ignored). */
void cw_joint2d_set_spring(cw_joint2d j, bool on, float hertz, float damping_ratio);
/* MOUSE only: the world point the grab point is pulled toward. */
void cw_joint2d_set_target(cw_joint2d j, cw_vec2 world_target);
/* Revolute: angle; prismatic: translation; distance: length; wheel: measured
 * spin speed (rad/s, b's angular velocity minus a's; as cw_joint3d_value); others 0. */
float cw_joint2d_value(cw_joint2d j);
/* The Box2D joint id. Leave its user data to the wrapper. */
b2JointId cw_joint2d_raw(cw_joint2d j);

/* ── queries (see physics3d.h; same contracts) ────────────────────────── */

typedef struct cw_ray_hit2d {
  cw_shape2d shape;
  cw_body2d body;
  cw_vec2 point, normal;
  float fraction;
  uint64_t material;
  int triangle;                /* always -1 in 2D (kept for parity) */
} cw_ray_hit2d;

/* Closest hit along origin..origin+translation; false (hit zeroed) on a miss. */
bool cw_world2d_raycast(cw_world2d w, cw_vec2 origin, cw_vec2 translation, const cw_phys_filter *filter,
                        cw_ray_hit2d *hit);
/* The cap closest hits, sorted; returns how many. */
int cw_world2d_raycast_all(cw_world2d w, cw_vec2 origin, cw_vec2 translation, const cw_phys_filter *filter,
                           cw_ray_hit2d *hits, int cap);
typedef struct cw_ray2d {
  cw_vec2 origin, translation;
} cw_ray2d;
/* hits[i] answers rays[i] on the job pool; returns the hit count. Not during a step. */
int cw_world2d_raycast_batch(cw_world2d w, const cw_ray2d *rays, int count, const cw_phys_filter *filter,
                             cw_ray_hit2d *hits);

#define CW_QSHAPE2D_POINTS 8
typedef struct cw_qshape2d {
  cw_vec2 points[CW_QSHAPE2D_POINTS];
  int count;
  float radius;
} cw_qshape2d;
/* One point plus radius. */
cw_qshape2d cw_qshape2d_circle(cw_vec2 center, float radius);
/* Two points plus radius. */
cw_qshape2d cw_qshape2d_capsule(cw_vec2 a, cw_vec2 b, float radius);
/* The four world corners of a rotated box (radius 0). */
cw_qshape2d cw_qshape2d_box(cw_vec2 center, cw_vec2 half, cw_rot2 rotation);
/* Copies up to CW_QSHAPE2D_POINTS points (more are dropped, logged), as their convex hull. */
cw_qshape2d cw_qshape2d_points(const cw_vec2 *points, int count, float radius);

/* Sweep: the cap closest hits, sorted; shapes overlapping at the start are skipped. */
int cw_world2d_shapecast(cw_world2d w, const cw_qshape2d *shape, cw_vec2 translation, const cw_phys_filter *filter,
                         cw_ray_hit2d *hits, int cap);
/* Shapes overlapping the query shape, up to cap, sorted by id; returns how many. */
int cw_world2d_overlap(cw_world2d w, const cw_qshape2d *shape, const cw_phys_filter *filter, cw_shape2d *out, int cap);
/* Shapes whose fattened bounding boxes overlap lo..hi, up to cap, sorted by id. */
int cw_world2d_overlap_aabb(cw_world2d w, cw_vec2 lo, cw_vec2 hi, const cw_phys_filter *filter, cw_shape2d *out,
                            int cap);

/* ── character mover (Box2D's mover API; see cw_mover3d for the contract).
 * The capsule stands along +y; position is the feet. Box2D's mover cast
 * cannot skip shapes by callback, so sensors are kept out by the query
 * bits physics.h describes (never put sensors in a raw filter's way). */
typedef struct cw_mover2d_def {
  float radius, height;        /* 0.3, 1.8 */
  float max_floor_angle;       /* 45 degrees */
  float step_height, snap_length, skin; /* 0.3, 0.25, 0.02 (skin <= 0 becomes 0.02) */
  int max_iterations;          /* 5 */
  uint32_t mask;
  float push_mass;
  bool ride_platforms;
} cw_mover2d_def;
/* The defaults noted in cw_mover2d_def above. */
cw_mover2d_def cw_mover2d_default_def(void);

typedef struct cw_mover2d {
  cw_world2d world;
  cw_mover2d_def def;
  cw_vec2 position, prev_position, velocity;
  bool on_floor, on_wall, on_ceiling, stepped, snapped;
  cw_vec2 floor_normal, wall_normal;
  cw_body2d floor_body;
  cw_vec2 platform_velocity;
  float floor_cos;
} cw_mover2d;

/* Clears m, copies def (NULL = defaults; height >= 2 * radius) and places the feet. */
void cw_mover2d_init(cw_mover2d *m, cw_world2d w, const cw_mover2d_def *def, cw_vec2 feet);
/* Move-and-slide by velocity * dt; see cw_mover3d_move. Once per fixed step. */
void cw_mover2d_move(cw_mover2d *m, float dt);
/* Moves the feet with no interpolation and clears the contact state (velocity is kept). */
void cw_mover2d_teleport(cw_mover2d *m, cw_vec2 feet);
/* Feet position lerped between prev_position and position by alpha (rendering). */
cw_vec2 cw_mover2d_interpolated(const cw_mover2d *m, float alpha);

#ifdef __cplusplus
}
#endif
#endif
