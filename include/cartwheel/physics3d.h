/* cartwheel 3D physics: Box3D (deps/box3d), mirrored name for name by
 * physics2d.h. Read physics.h first: layers, events, filters, stats, debug
 * lines and the handle/ownership rules are shared and documented there.
 *
 * Typical frame:
 *   init:          w = cw_world3d_create(&def); bodies, shapes, joints
 *   fixed_update:  movers / forces / kinematic targets, cw_world3d_step(w, dt),
 *                  then read cw_world3d_events(w, &n)
 *   render:        cw_world3d_sync(w, alpha) writes bound three.c objects
 *
 * Vectors and rotations are three.c's t3_vec3 / t3_quat (same layout as
 * Box3D's b3Vec3 / b3Quat). Units: meters, kilograms, seconds, radians. */
#ifndef CW_PHYSICS3D_H
#define CW_PHYSICS3D_H

#include "box3d/box3d.h"
#include "physics.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

CW_HANDLE_TYPE(cw_world3d);
CW_HANDLE_TYPE(cw_body3d);
CW_HANDLE_TYPE(cw_shape3d);
CW_HANDLE_TYPE(cw_joint3d);
CW_HANDLE_TYPE(cw_mesh3d);
CW_HANDLE_TYPE(cw_heightfield3d);

typedef struct cw_xform3 {
  t3_vec3 p;
  t3_quat q;
} cw_xform3;
/* Origin, identity rotation. */
static inline cw_xform3 cw_xform3_identity(void) { return (cw_xform3){ { 0, 0, 0 }, { 0, 0, 0, 1 } }; }
/* Position (x, y, z), identity rotation. */
static inline cw_xform3 cw_xform3_at(float x, float y, float z) { return (cw_xform3){ { x, y, z }, { 0, 0, 0, 1 } }; }
/* Linear position, normalized-lerp rotation (shortest arc). Deterministic,
 * but meant for rendering. */
cw_xform3 cw_xform3_lerp(cw_xform3 a, cw_xform3 b, float t);

/* ── worlds ──────────────────────────────────────────────────────────── */

typedef struct cw_world3d_def {
  t3_vec3 gravity;             /* (0, -10, 0) */
  int substeps;                /* solver sub-steps per step: 4 */
  int workers;                 /* job-pool threads the step may use: 0 = all */
  float hit_speed;             /* hit events above this approach speed: 1 m/s */
  float restitution_threshold; /* bounce above this speed: 1 m/s */
  bool sleep, continuous;      /* true, true */
  const char *layers[CW_PHYS_LAYERS]; /* layer names (copied); NULL = unnamed */
} cw_world3d_def;
/* The defaults noted in cw_world3d_def above. */
cw_world3d_def cw_world3d_default_def(void);

/* Creates a Box3D world stepped on the job pool. Up to 128 worlds. */
cw_world3d cw_world3d_create(const cw_world3d_def *def);
/* Destroys every body, shape, joint and record of the world. */
void cw_world3d_destroy(cw_world3d w);
/* False for null, destroyed or stale handles. */
bool cw_world3d_valid(cw_world3d w);

/* One fixed step of dt seconds: drives mouse joints, steps Box3D, then
 * builds the event list, breaks overloaded joints, updates the
 * interpolation pairs of moved bodies and folds the step into the state
 * hash. Cost: the library step plus O(moved bodies + events). */
void cw_world3d_step(cw_world3d w, float dt);
/* For a world stepped more than once a fixed tick (shorter steps for fast
 * bodies): call it at the start of every tick, before the tick's first step.
 * Interpolation (cw_world3d_sync, cw_body3d_interpolated) then runs from each
 * body's pose at the start of the tick to its pose after the tick's last
 * step, the span the render callback's alpha measures. Without it the pair is
 * the last step's alone: alpha would move a body over a fraction of the
 * tick while everything interpolated per tick (a camera) moves over all of
 * it, and the body would shift against them as alpha changes. */
void cw_world3d_tick_begin(cw_world3d w);
/* The last step's events (physics.h for order and meaning). Valid until the
 * next step or until the world is destroyed. */
typedef struct cw_phys_event3d {
  cw_phys_event_type type;
  cw_shape3d shape_a, shape_b; /* sensor events: a = sensor, b = visitor */
  cw_body3d body_a, body_b;    /* null when the shape is gone */
  cw_joint3d joint;            /* joint break */
  t3_vec3 point, normal;       /* hit: world point, normal from a to b */
  float speed;                 /* hit: approach speed */
  uint64_t material_a, material_b; /* hit: user materials */
  void *user;                  /* joint break: the joint's user pointer */
} cw_phys_event3d;
/* Sets *count; NULL (count 0) for an invalid world. The array belongs to the world. */
const cw_phys_event3d *cw_world3d_events(cw_world3d w, int *count);

/* Writes every bound body's transform, interpolated between the last two
 * steps (the last tick's start and end with cw_world3d_tick_begin) by alpha
 * (the render callback's), into its three.c object or plain transform.
 * Bodies that did not move in the last step (tick) are written once and then
 * skipped. */
void cw_world3d_sync(cw_world3d w, float alpha);

/* Counters, step count and state hash; all zero for an invalid world. Cheap. */
cw_phys_stats cw_world3d_stats(cw_world3d w);
/* m/s^2; ignored for an invalid world. */
/* Floating origin (cartwheel/stream.h): move every body by -shift, in
 * creation order, keeping velocities, sleep states and the interpolation
 * (synced objects do not jump). Box3D has no origin shift of its own, so
 * this sets each body's transform: O(bodies), for a rare event. Joint
 * anchors on the world itself (cw_joint3d to no body, mouse targets) do not
 * move: move them yourself. Deterministic: called on a tick the simulation
 * picks, every host shifts the same bodies by the same amount. */
void cw_world3d_shift_origin(cw_world3d w, t3_vec3 shift);
void cw_world3d_set_gravity(cw_world3d w, t3_vec3 g);
/* m/s^2; zero for an invalid world. */
t3_vec3 cw_world3d_gravity(cw_world3d w);

/* Layer index by name, -1 (logged) when no layer has that name. */
int cw_world3d_layer(cw_world3d w, const char *name);
/* Bits for a '|' or ',' separated list of layer names ("world|enemy"). */
uint32_t cw_world3d_layer_bits(cw_world3d w, const char *names);

/* Appends the world's debug geometry to out (physics.h): shapes as
 * wireframes, joints, bounds, contacts as asked by flags. */
void cw_world3d_debug_draw(cw_world3d w, cw_phys_lines *out, uint32_t flags);

/* The Box3D world id (null id for a null handle). Do not destroy it or set its user data. */
b3WorldId cw_world3d_raw(cw_world3d w);

/* ── bodies ──────────────────────────────────────────────────────────── */

typedef struct cw_body3d_def {
  cw_body_type type;            /* CW_BODY_DYNAMIC */
  t3_vec3 position;
  t3_quat rotation;             /* identity */
  t3_vec3 linear_velocity, angular_velocity;
  float linear_damping, angular_damping; /* 0, 0 */
  float gravity_scale;          /* 1 */
  bool lock_rotation;           /* no rotation at all (motion locks) */
  bool bullet;                  /* continuous vs other dynamic bodies */
  bool awake, sleep;            /* true, true */
  bool interpolate;             /* keep a prev/current pair for sync: true */
  const char *name;             /* debugging, copied */
  void *user;
} cw_body3d_def;
/* The defaults noted in cw_body3d_def above. */
cw_body3d_def cw_body3d_default_def(void);

/* Null (logged) on an invalid world. def NULL = defaults. Lives until destroyed or the world is. */
cw_body3d cw_body3d_create(cw_world3d w, const cw_body3d_def *def);
/* Destroys the body with its shapes and joints. */
void cw_body3d_destroy(cw_body3d b);
/* False for null, destroyed or stale handles. */
bool cw_body3d_valid(cw_body3d b);
/* Owning world; null (logged) for an invalid body. */
cw_world3d cw_body3d_world(cw_body3d b);
/* CW_BODY_STATIC (logged) for an invalid body. */
cw_body_type cw_body3d_type(cw_body3d b);
/* Switches static / kinematic / dynamic at run time; logged no-op for an invalid body. */
void cw_body3d_set_type(cw_body3d b, cw_body_type type);

/* Current world transform (last step, not interpolated); identity (logged) for an invalid body. */
cw_xform3 cw_body3d_transform(cw_body3d b);
/* Teleport: no interpolation from the old place. Wakes the body. */
void cw_body3d_set_transform(cw_body3d b, cw_xform3 x);
/* Kinematic bodies: velocity that reaches `target` in dt (call before the
 * step with the step's dt). Dynamic bodies are moved the same way. */
void cw_body3d_set_target(cw_body3d b, cw_xform3 target, float dt);

/* Linear velocity of the center of mass, m/s; zero (logged) for an invalid body. */
t3_vec3 cw_body3d_velocity(cw_body3d b);
/* m/s. Invalid bodies are logged and ignored. */
void cw_body3d_set_velocity(cw_body3d b, t3_vec3 v);
/* World-space rad/s; zero (logged) for an invalid body. */
t3_vec3 cw_body3d_angular_velocity(cw_body3d b);
/* World-space rad/s. */
void cw_body3d_set_angular_velocity(cw_body3d b, t3_vec3 w);
/* point: world point, NULL = center of mass. Wakes the body. */
void cw_body3d_apply_force(cw_body3d b, t3_vec3 force, const t3_vec3 *point);
/* Impulse in N*s at a world point (NULL = center of mass). Wakes the body. */
void cw_body3d_apply_impulse(cw_body3d b, t3_vec3 impulse, const t3_vec3 *point);
/* World-space torque in N*m for the next step. Wakes the body. */
void cw_body3d_apply_torque(cw_body3d b, t3_vec3 torque);
/* World-space angular impulse in N*m*s. Wakes the body. */
void cw_body3d_apply_angular_impulse(cw_body3d b, t3_vec3 impulse);
/* kg, from the shapes' densities; 0 for static/kinematic bodies or an invalid body. */
float cw_body3d_mass(cw_body3d b);
/* False for a sleeping or invalid body. */
bool cw_body3d_awake(cw_body3d b);
/* Wakes the body or puts it to sleep. */
void cw_body3d_set_awake(cw_body3d b, bool awake);
/* Your pointer (the library user data is the wrapper's). Ignored for an invalid body. */
void cw_body3d_set_user(cw_body3d b, void *user);
/* NULL for an invalid body. */
void *cw_body3d_user(cw_body3d b);

/* Transform sync targets (at most one; binding replaces the previous one,
 * NULL unbinds). The object / transform must outlive the binding. */
void cw_body3d_bind_object(cw_body3d b, t3_object *obj);
/* Plain transform target, written by cw_world3d_sync (replaces an object binding). */
void cw_body3d_bind_xform(cw_body3d b, cw_xform3 *dst);
/* The transform sync would write for this alpha. */
cw_xform3 cw_body3d_interpolated(cw_body3d b, float alpha);

/* The Box3D body id. Do not destroy it through Box3D or set its user data. */
b3BodyId cw_body3d_raw(cw_body3d b);

/* What a body touches after the last step, one entry per touching shape
 * pair (its manifolds summed): crash damage, landing thumps, scrapes. */
typedef struct cw_contact3d {
  cw_shape3d shape;            /* this body's shape */
  cw_shape3d other_shape;
  cw_body3d other;
  t3_vec3 point;               /* world: the points weighted by impulse (their middle when none pushed) */
  t3_vec3 normal;              /* from this body toward the other */
  float impulse;               /* N*s: Box3D's total normal impulse over the last step's sub-steps; it
                                  counts the solve and the relax pass, so it runs about twice the
                                  momentum the contact moved: a measure of how hard, not a momentum */
  float approach;              /* m/s: the fastest approach before the solve (> 0 closing) */
  t3_vec3 slide;               /* m/s: the other's velocity against this one at the point, across the normal */
} cw_contact3d;
/* Fills out with up to cap contacts sorted by (shape, other_shape) id, so
 * the order is the same on every host; returns how many. 0 for an invalid
 * body. Not during a step. Cost: O(the body's contacts). */
int cw_body3d_contacts(cw_body3d b, cw_contact3d *out, int cap);

/* ── shapes ──────────────────────────────────────────────────────────── */

typedef struct cw_shape3d_def {
  cw_phys_material material;
  float density;               /* kg/m^3: 1 */
  int layer;                   /* 0..31: 0 */
  uint32_t mask;               /* layers it collides with (queries never see sensors): all */
  int group;                   /* 0; <0 never collide within, >0 always */
  bool sensor;                 /* overlap events, no response */
  bool contact_events;         /* begin/end: true */
  bool sensor_events;          /* detected by / detecting sensors: true */
  bool hit_events;             /* false */
  void *user;
} cw_shape3d_def;
/* The defaults noted in cw_shape3d_def above. */
cw_shape3d_def cw_shape3d_default_def(void);

/* Box with half extents `half`, placed by `local` in the body frame. */
cw_shape3d cw_shape3d_box(cw_body3d b, const cw_shape3d_def *def, t3_vec3 half, cw_xform3 local);
/* Sphere at body-local center. Null (logged) for an invalid body. */
cw_shape3d cw_shape3d_sphere(cw_body3d b, const cw_shape3d_def *def, t3_vec3 center, float radius);
/* Capsule between body-local centers a and c. Null (logged) for an invalid body. */
cw_shape3d cw_shape3d_capsule(cw_body3d b, const cw_shape3d_def *def, t3_vec3 a, t3_vec3 c, float radius);
/* Convex hull of `count` body-local points (the hull is computed here and
 * copied into the world's hull database; the points are not kept). Null
 * (logged) when the points are degenerate. */
cw_shape3d cw_shape3d_hull(cw_body3d b, const cw_shape3d_def *def, const t3_vec3 *points, int count);
/* Triangle mesh: STATIC bodies only (Box3D collides meshes only there). The
 * mesh must outlive every shape made from it. */
cw_shape3d cw_shape3d_mesh(cw_body3d b, const cw_shape3d_def *def, cw_mesh3d mesh, t3_vec3 scale);
/* Updates the body's mass; no-op on an invalid handle. */
void cw_shape3d_destroy(cw_shape3d s);
/* False for null, destroyed or stale handles. */
bool cw_shape3d_valid(cw_shape3d s);
/* Owning body; null for an invalid shape. */
cw_body3d cw_shape3d_body(cw_shape3d s);
/* def.user; NULL for an invalid shape. */
void *cw_shape3d_user(cw_shape3d s);
/* False for an invalid shape. */
bool cw_shape3d_is_sensor(cw_shape3d s);
/* Replaces friction, restitution, rolling resistance, tangent velocity and user material. */
void cw_shape3d_set_material(cw_shape3d s, const cw_phys_material *m);
/* All zero for an invalid shape. */
cw_phys_material cw_shape3d_material(cw_shape3d s);
/* Moves the shape to layer (0..31) with a new mask; contacts are recomputed next step (costly). */
void cw_shape3d_set_layer(cw_shape3d s, int layer, uint32_t mask);
/* The Box3D shape id. Leave its filter and user data to the wrapper. */
b3ShapeId cw_shape3d_raw(cw_shape3d s);

/* Collision meshes (resources; one mesh serves many shapes). Builds a BVH
 * from `triangle_count` triangles (3 indices each). Cost: O(n log n). */
cw_mesh3d cw_mesh3d_create(const t3_vec3 *vertices, int vertex_count, const int32_t *indices, int triangle_count);
/* Frees the mesh; destroy every shape made from it first. No-op on an invalid handle. */
void cw_mesh3d_destroy(cw_mesh3d m);

/* Height fields (resources, like collision meshes): Box3D's grid of nx x nz
 * samples (x along columns, z along rows; sample (i, j) at local
 * (i * scale.x, q[j * nx + i] * scale.y, j * scale.z)), stored as the uint16
 * steps given, exactly. Each quad splits along the diagonal from its
 * (i, j + 1) corner to its (i + 1, j) corner. materials: (nx - 1) x (nz - 1)
 * cell bytes (0xFF = a hole, no collision), NULL = all 0. Fields that share
 * a scale.y line up exactly along shared edges (cartwheel/terrain.h tiles
 * them). Null (logged) for fewer than 2 x 2 samples. */
cw_heightfield3d cw_heightfield3d_create(const uint16_t *q, int nx, int nz, t3_vec3 scale, const uint8_t *materials);
/* Frees the field; destroy every shape made from it first. No-op on an invalid handle. */
void cw_heightfield3d_destroy(cw_heightfield3d h);
/* Box3D's data, for its pure queries (b3RayCastHeightField); NULL when invalid. */
const b3HeightFieldData *cw_heightfield3d_raw(cw_heightfield3d h);
/* A height-field shape at the body's origin: STATIC bodies only, as meshes.
 * The field must outlive every shape made from it. */
cw_shape3d cw_shape3d_heightfield(cw_body3d b, const cw_shape3d_def *def, cw_heightfield3d field);

/* ── joints ──────────────────────────────────────────────────────────── */

typedef struct cw_joint3d_def {
  cw_joint_type type;
  cw_body3d a, b;              /* MOUSE: b is the dragged body, a unused */
  /* Joint frames in each body's local space. Axes: REVOLUTE turns about z;
   * PRISMATIC slides along x of frame a; WHEEL spins about z of frame b and
   * travels along x of frame a; SPHERICAL cone about z of frame a, twist
   * about z of frame b. cw_joint3d_def_anchor fills both from world data. */
  cw_xform3 frame_a, frame_b;
  bool collide_connected;
  /* Break when the joint force (N) or torque (N*m) exceeds this; 0 = never.
   * A broken joint is destroyed after the step and reported as
   * CW_PHYS_JOINT_BREAK. */
  float break_force, break_torque;
  bool spring;                 /* revolute, prismatic, distance, spherical, wheel (suspension) */
  float hertz, damping_ratio;  /* spring (and weld / motor / mouse stiffness) */
  bool limit;                  /* revolute/spherical twist: angles; prismatic/wheel: translation */
  float lower, upper;          /* distance: min/max length */
  bool motor;
  float motor_speed;           /* rad/s or m/s */
  float max_motor;             /* torque or force */
  float length;                /* distance: rest length; 0 = current anchor distance */
  float cone_angle;            /* spherical: cone limit when limit is set */
  float max_force;             /* mouse / motor: max spring force, 0 = 1000 x weight */
  t3_vec3 target;              /* mouse: world target (and grab point at creation) */
  void *user;
} cw_joint3d_def;
/* Identity frames and per-type spring defaults (2 Hz / 0.5; motor and mouse 5 Hz / 0.7; weld rigid). */
cw_joint3d_def cw_joint3d_default_def(cw_joint_type type);
/* Fill frame_a/frame_b from a WORLD anchor point and axis using the bodies'
 * current transforms; the axis maps to the joint's main local axis (z for
 * revolute/spherical/parallel, x for prismatic/wheel/distance). */
void cw_joint3d_def_anchor(cw_joint3d_def *def, t3_vec3 world_anchor, t3_vec3 world_axis);

/* a null = a static world body. Null (logged) for a NULL def or invalid bodies. */
cw_joint3d cw_joint3d_create(cw_world3d w, const cw_joint3d_def *def);
/* Also frees a MOUSE joint's internal body. No-op on an invalid handle; no break event. */
void cw_joint3d_destroy(cw_joint3d j);
/* False for null, destroyed (or broken) and stale handles. */
bool cw_joint3d_valid(cw_joint3d j);
/* CW_JOINT_TYPE_COUNT for an invalid joint. */
cw_joint_type cw_joint3d_type(cw_joint3d j);
/* Your pointer, reported again in the joint break event. */
void cw_joint3d_set_user(cw_joint3d j, void *user);
/* NULL for an invalid joint. */
void *cw_joint3d_user(cw_joint3d j);
/* Runtime changes for the types that have them (others: ignored). */
void cw_joint3d_set_motor(cw_joint3d j, bool on, float speed, float max);
/* Revolute angle, prismatic/wheel translation, distance length, spherical twist; others ignored. Wakes bodies. */
void cw_joint3d_set_limits(cw_joint3d j, bool on, float lower, float upper);
/* Revolute, prismatic, wheel, distance, spherical; weld / motor / parallel take hertz and damping only (on is
 * ignored); MOUSE: linear only. Wakes bodies. */
void cw_joint3d_set_spring(cw_joint3d j, bool on, float hertz, float damping_ratio);
/* MOUSE: the point the grab point is pulled toward. */
void cw_joint3d_set_target(cw_joint3d j, t3_vec3 world_target);
/* Revolute: angle; prismatic: translation; distance: length; wheel: spin
 * speed; spherical: twist angle; others 0. */
float cw_joint3d_value(cw_joint3d j);
/* The Box3D joint id. Leave its user data to the wrapper. */
b3JointId cw_joint3d_raw(cw_joint3d j);

/* ── queries ───────────────────────────────────────────────────────────
 * All take a filter (NULL = every non-sensor shape, physics.h) and write
 * into caller arrays: no allocation. Results are sorted (fraction, then
 * shape id; overlaps by shape id) so they are identical on every host. */

typedef struct cw_ray_hit3d {
  cw_shape3d shape;            /* null: no hit */
  cw_body3d body;
  t3_vec3 point, normal;
  float fraction;              /* hit = origin + fraction * translation */
  uint64_t material;           /* user material */
  int triangle;                /* mesh triangle, else -1 */
} cw_ray_hit3d;

/* Closest hit along origin..origin+translation. */
bool cw_world3d_raycast(cw_world3d w, t3_vec3 origin, t3_vec3 translation, const cw_phys_filter *filter,
                        cw_ray_hit3d *hit);
/* The `cap` closest hits (one per shape), sorted; returns how many. */
int cw_world3d_raycast_all(cw_world3d w, t3_vec3 origin, t3_vec3 translation, const cw_phys_filter *filter,
                           cw_ray_hit3d *hits, int cap);

/* Many closest-hit rays at once on the job pool (cw_parallel_for): hits[i]
 * answers rays[i] (shape null = miss). Returns the number of hits. Same
 * results as calling cw_world3d_raycast for each. Not during a step. */
typedef struct cw_ray3d {
  t3_vec3 origin, translation;
} cw_ray3d;
/* Sorted per ray as cw_world3d_raycast; 0 for an invalid world or count <= 0. */
int cw_world3d_raycast_batch(cw_world3d w, const cw_ray3d *rays, int count, const cw_phys_filter *filter,
                             cw_ray_hit3d *hits);

/* A convex query shape: up to CW_QSHAPE3D_POINTS world points plus a
 * radius (a sphere is one point, a capsule two, a box eight). */
#define CW_QSHAPE3D_POINTS 16
typedef struct cw_qshape3d {
  t3_vec3 points[CW_QSHAPE3D_POINTS];
  int count;
  float radius;
} cw_qshape3d;
/* One point plus radius. */
cw_qshape3d cw_qshape3d_sphere(t3_vec3 center, float radius);
/* Two points plus radius. */
cw_qshape3d cw_qshape3d_capsule(t3_vec3 a, t3_vec3 b, float radius);
/* The eight world corners of an oriented box (radius 0). */
cw_qshape3d cw_qshape3d_box(t3_vec3 center, t3_vec3 half, t3_quat rotation);
/* Copies up to CW_QSHAPE3D_POINTS points (more are dropped, logged), treated as their convex hull. */
cw_qshape3d cw_qshape3d_points(const t3_vec3 *points, int count, float radius);

/* Sweep the shape along translation: the `cap` closest hits (cap 1 = the
 * closest), sorted; returns how many. Shapes already overlapping at the
 * start are not reported (use overlap). */
int cw_world3d_shapecast(cw_world3d w, const cw_qshape3d *shape, t3_vec3 translation, const cw_phys_filter *filter,
                         cw_ray_hit3d *hits, int cap);
/* Shapes overlapping the query shape (exact test), up to cap, sorted by
 * id; returns how many were written. */
int cw_world3d_overlap(cw_world3d w, const cw_qshape3d *shape, const cw_phys_filter *filter, cw_shape3d *out, int cap);
/* Shapes whose (fattened) bounding boxes overlap the box lo..hi. */
int cw_world3d_overlap_aabb(cw_world3d w, t3_vec3 lo, t3_vec3 hi, const cw_phys_filter *filter, cw_shape3d *out,
                            int cap);

/* ── character mover ───────────────────────────────────────────────────
 * A kinematic capsule outside the simulation (Box3D's mover API, which is
 * EXPERIMENTAL upstream: deps/box3d/docs/character.md), moved with
 * move-and-slide: you set velocity (input + gravity), cw_mover3d_move
 * moves as far as the world allows, slides along surfaces, walks up slopes
 * up to max_floor_angle, steps up ledges up to step_height, snaps down to
 * the floor within snap_length, rides moving platforms and clips the
 * velocity against what it hit. Sensors never block it.
 *
 * Plain struct, owned by the caller; fields below the def are outputs. */
typedef struct cw_mover3d_def {
  float radius, height;        /* capsule, feet to top: 0.3, 1.8 */
  t3_vec3 up;                  /* (0, 1, 0) */
  float max_floor_angle;       /* radians: 45 degrees */
  float step_height;           /* 0.3; 0 = no stepping */
  float snap_length;           /* 0.25; 0 = no snapping */
  float skin;                  /* contact detection distance: 0.02 (<= 0 becomes 0.02) */
  int max_iterations;          /* slide iterations per move: 5 */
  uint32_t mask;               /* layers it collides with (queries never see sensors): all */
  float push_mass;             /* >0: pushes dynamic bodies as this mass */
  bool ride_platforms;         /* inherit the floor body's velocity: true */
} cw_mover3d_def;
/* The defaults noted in cw_mover3d_def above. */
cw_mover3d_def cw_mover3d_default_def(void);

typedef struct cw_mover3d {
  cw_world3d world;
  cw_mover3d_def def;
  t3_vec3 position;            /* the feet: bottom of the capsule */
  t3_vec3 prev_position;       /* before the last move (interpolation) */
  t3_vec3 velocity;            /* set by you, clipped by move */
  bool on_floor, on_wall, on_ceiling;
  bool stepped, snapped;       /* the last move stepped up a ledge / snapped down */
  t3_vec3 floor_normal, wall_normal;
  cw_body3d floor_body;
  t3_vec3 platform_velocity;   /* the floor's velocity under the feet */
  float floor_cos;             /* cos(max_floor_angle), set by init */
} cw_mover3d;

/* Clears m, copies def (NULL = defaults; up normalized, height >= 2 * radius) and places the feet. */
void cw_mover3d_init(cw_mover3d *m, cw_world3d w, const cw_mover3d_def *def, t3_vec3 feet);
/* Move by velocity * dt (plus the platform's motion) and update the
 * outputs. Call once per fixed step, before or after the world step but
 * always the same. Cost: a few world queries (more when stepping). */
void cw_mover3d_move(cw_mover3d *m, float dt);
/* Moves the feet with no interpolation and clears the contact state (velocity is kept). */
void cw_mover3d_teleport(cw_mover3d *m, t3_vec3 feet);
/* Feet position lerped between prev_position and position by alpha (rendering). */
t3_vec3 cw_mover3d_interpolated(const cw_mover3d *m, float alpha);

#ifdef __cplusplus
}
#endif
#endif
