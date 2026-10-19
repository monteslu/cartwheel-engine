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
/* World positions are doubles (Box2D v3.2 large worlds: about 1000 km of world, the solver stays in floats), so a body
 * far from the origin keeps its millimetres. Vectors, velocities, local frames and sizes stay cw_vec2 / float. */
typedef struct cw_pos2 {
  double x, y;
} cw_pos2;
typedef struct cw_wxform2 {
  cw_pos2 p;
  cw_rot2 q;
} cw_wxform2;
static inline cw_pos2 cw_pos2_from(cw_vec2 v) { return (cw_pos2){ v.x, v.y }; }
/* The position as floats: exact near the origin, rounded far from it. */
static inline cw_vec2 cw_pos2_to_vec(cw_pos2 p) { return (cw_vec2){ (float)p.x, (float)p.y }; }
static inline cw_pos2 cw_pos2_add(cw_pos2 p, cw_vec2 d) { return (cw_pos2){ p.x + d.x, p.y + d.y }; }
/* a - b as a float vector (exact when a and b are close, however far they are from the origin). */
static inline cw_vec2 cw_pos2_sub(cw_pos2 a, cw_pos2 b) { return (cw_vec2){ (float)(a.x - b.x), (float)(a.y - b.y) }; }
static inline cw_wxform2 cw_wxform2_from(cw_xform2 t) { return (cw_wxform2){ cw_pos2_from(t.p), t.q }; }
static inline cw_xform2 cw_wxform2_to_xform(cw_wxform2 t) { return (cw_xform2){ cw_pos2_to_vec(t.p), t.q }; }
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

/* What a world is sized for (b2World_GetMaxCapacity reports what it grew to; feed that back to skip the growth). */
typedef struct cw_phys_capacity2d {
  int static_shapes, dynamic_shapes, static_bodies, dynamic_bodies, contacts, arena_bytes;
} cw_phys_capacity2d;

typedef struct cw_world2d_def {
  cw_vec2 gravity;             /* (0, -10) */
  int substeps;                /* 4 */
  int workers;                 /* job-pool threads the step may use: 0 = all */
  float hit_speed;             /* 1 m/s */
  float restitution_threshold; /* 1 m/s */
  int restitution_iterations;  /* 0 = Box2D's default */
  bool restitution_propagation; /* false = Box2D's default */
  float max_linear_speed;      /* m/s, 0 = Box2D's default */
  cw_phys_capacity2d capacity; /* expected counts, to size the world once (all zero: grow as needed) */
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
/* A world stepped more than once a tick; see cw_world3d_tick_begin. */
void cw_world2d_tick_begin(cw_world2d w);

typedef struct cw_phys_event2d {
  cw_phys_event_type type;
  cw_shape2d shape_a, shape_b;
  cw_body2d body_a, body_b;
  cw_joint2d joint;
  cw_pos2 point;
  cw_vec2 normal;
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

/* ── world tuning (Box2D v3.2) ────────────────────────────────────────── */

typedef struct cw_aabb2 {
  cw_vec2 lo, hi; /* floats: exact within a few km of the origin */
} cw_aabb2;
/* The union of every shape's bounds; zero for an invalid world. */
cw_aabb2 cw_world2d_bounds(cw_world2d w);
/* Box2D's hash of the world's simulation state (what rollback compares); 0 for an invalid world. */
uint64_t cw_world2d_state_hash(cw_world2d w);
/* Fastest a body may move, m/s. */
void cw_world2d_set_max_linear_speed(cw_world2d w, float speed);
float cw_world2d_max_linear_speed(cw_world2d w);
/* Contact points are reused while the shapes move less than this, m; 0 turns recycling off for new contacts. */
void cw_world2d_set_contact_recycle_distance(cw_world2d w, float meters);
float cw_world2d_contact_recycle_distance(cw_world2d w);
/* Contact softness: stiffness (Hz), damping ratio, and the fastest push-out speed (m/s). */
void cw_world2d_set_contact_tuning(cw_world2d w, float hertz, float damping_ratio, float push_speed);
void cw_world2d_contact_tuning(cw_world2d w, float *hertz, float *damping_ratio, float *push_speed);
/* Bounce solver passes, and whether a bounce carries through a stack (b2WorldDef::enableRestitutionPropagation). */
void cw_world2d_set_restitution_iterations(cw_world2d w, int iterations);
int cw_world2d_restitution_iterations(cw_world2d w);
void cw_world2d_set_restitution_propagation(cw_world2d w, bool on);
bool cw_world2d_restitution_propagation(cw_world2d w);
/* Hit events fire above this approach speed, m/s. */
void cw_world2d_set_hit_speed(cw_world2d w, float speed);
/* Threads the step uses (1..32, and the job pool's size at most). */
void cw_world2d_set_workers(cw_world2d w, int workers);
int cw_world2d_workers(cw_world2d w);
/* What the world has grown to; pass it as def.capacity next time. */
cw_phys_capacity2d cw_world2d_capacity(cw_world2d w);
/* Run the 4-wide solver even where the 8-wide AVX2 one is available (same bits; for testing). */
void cw_world2d_set_narrow_solver(cw_world2d w, bool on);
/* True when this CPU can run Box2D's 8-wide AVX2 contact solver. */
bool cw_physics2d_avx2_available(void);

/* ── snapshots (Box2D v3.2): the whole simulation state as bytes ─────────
 * Call between steps. A restore puts the world back to that instant, warm-start caches and sleeping islands
 * included, which is what rollback needs (the tree snapshot of cartwheel/net.h carries one per 2D world). Bodies,
 * shapes and joints made after the snapshot are gone after a restore; their handles go stale. Your user pointers
 * (cw_body2d_set_user...) and the wrapper's own records survive, event lists are cleared. Experimental in
 * Box2D: the image format changes between versions, so do not keep images across engine updates. */
/* Bytes a snapshot needs; 0 for an invalid world. */
int cw_world2d_snapshot_size(cw_world2d w);
/* Writes the snapshot to out (cap >= cw_world2d_snapshot_size); returns its size, or 0 (logged) when it does not fit. */
int cw_world2d_snapshot(cw_world2d w, uint8_t *out, int cap);
/* Restores a snapshot of this world. False (logged) when it is rejected; the world is then unchanged unless the
 * payload turns out corrupt, in which case destroy it. */
bool cw_world2d_restore(cw_world2d w, const uint8_t *image, int size);

/* ── recording and replay (Box2D v3.2) ───────────────────────────────────
 * A recording is the world at the start plus every call that changed it and the state hash of every step: attach it
 * to a bug report and the replay shows the same simulation. */
typedef struct cw_recording2d cw_recording2d;
/* Starts recording world w from now (call between steps; before the first step records the whole session).
 * NULL (logged) if it is already recording. Stop it with cw_world2d_record_stop. */
cw_recording2d *cw_world2d_record_start(cw_world2d w);
void cw_world2d_record_stop(cw_world2d w);
/* The recorded bytes so far; valid until the next recording call or cw_recording2d_destroy. */
const uint8_t *cw_recording2d_data(const cw_recording2d *r, int *size);
bool cw_recording2d_save(const cw_recording2d *r, const char *path);
/* Reads a recording file (an OS path); NULL if it cannot be read. */
cw_recording2d *cw_recording2d_load(const char *path);
/* A recording must not be destroyed while its world is still recording. */
void cw_recording2d_destroy(cw_recording2d *r);
/* Re-runs a recording from bytes and checks every recorded state hash; false on the first mismatch. */
bool cw_recording2d_validate(const void *data, int size);

typedef struct cw_replay2d cw_replay2d;
typedef struct cw_replay2d_info {
  int frames;       /* recorded steps */
  float dt;         /* the recorded step */
  int substeps;
  cw_aabb2 bounds;  /* the world bounds over the recording */
} cw_replay2d_info;
/* Opens a recording (the bytes are copied) in a world of its own, at its first step. NULL (logged) if malformed. */
cw_replay2d *cw_replay2d_open(const void *data, int size);
void cw_replay2d_close(cw_replay2d *p);
/* Advances one recorded step; false at the end. */
bool cw_replay2d_step(cw_replay2d *p);
/* Jumps to a recorded step (backwards re-runs from the nearest keyframe); clamped. */
void cw_replay2d_seek(cw_replay2d *p, int frame);
void cw_replay2d_restart(cw_replay2d *p);
int cw_replay2d_frame(const cw_replay2d *p);
bool cw_replay2d_at_end(const cw_replay2d *p);
cw_replay2d_info cw_replay2d_get_info(const cw_replay2d *p);
/* True when a recorded state hash failed to reproduce (this engine simulates it differently); the first such step is
 * cw_replay2d_diverge_frame, -1 otherwise. */
bool cw_replay2d_diverged(const cw_replay2d *p);
int cw_replay2d_diverge_frame(const cw_replay2d *p);
/* Memory for backwards seeking: keyframes at least min_interval steps apart within budget_bytes. */
void cw_replay2d_set_keyframes(cw_replay2d *p, size_t budget_bytes, int min_interval);
/* The replayed world as a wrapper handle is not offered (it is Box2D's own): its bodies, by creation order, and the
 * Box2D world id, for the raw API; and its wireframes, as cw_world2d_debug_draw. */
b2WorldId cw_replay2d_world(const cw_replay2d *p);
int cw_replay2d_body_count(const cw_replay2d *p);
b2BodyId cw_replay2d_body(const cw_replay2d *p, int index);
void cw_replay2d_debug_draw(cw_replay2d *p, cw_phys_lines *out, uint32_t flags);

/* ── bodies ──────────────────────────────────────────────────────────── */

typedef struct cw_body2d_def {
  cw_body_type type;           /* CW_BODY_DYNAMIC */
  cw_pos2 position;
  cw_rot2 rotation;            /* (1, 0) */
  cw_vec2 linear_velocity;
  float angular_velocity;
  float linear_damping, angular_damping, gravity_scale; /* 0, 0, 1 */
  bool lock_rotation, bullet;
  bool awake, sleep, interpolate; /* true, true, true */
  float safety_factor;         /* continuous collision, 0.01..0.5: Box2D's default (smaller is safer, can hitch) */
  bool allow_fast_rotation;    /* false */
  bool contact_recycling;      /* true: reuse contact points while the shapes barely move */
  bool enabled;                /* true; false = created disabled (no collision, no step) */
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
/* Current transform (last step, not interpolated) with a double position; identity for an invalid body. */
cw_wxform2 cw_body2d_world_transform(cw_body2d b);
/* The double position of the body origin; zero for an invalid body. */
cw_pos2 cw_body2d_position(cw_body2d b);
/* Teleport: no interpolation from the old place. Wakes the body. */
void cw_body2d_set_world_transform(cw_body2d b, cw_wxform2 x);
/* Velocity that reaches target in dt; see cw_body3d_set_target. */
void cw_body2d_set_world_target(cw_body2d b, cw_wxform2 target, float dt);
/* cw_body2d_world_transform rounded to floats (exact within a few km of the origin). */
cw_xform2 cw_body2d_transform(cw_body2d b);
/* cw_body2d_set_world_transform from a float transform. */
void cw_body2d_set_transform(cw_body2d b, cw_xform2 x);
/* cw_body2d_set_world_target from a float transform. */
void cw_body2d_set_target(cw_body2d b, cw_xform2 target, float dt);
/* Centre of mass: world (double) and body-local. */
cw_pos2 cw_body2d_world_center(cw_body2d b);
cw_vec2 cw_body2d_local_center(cw_body2d b);
/* The world box of the body's shapes (empty and centred on the origin when it has none). */
cw_aabb2 cw_body2d_bounds(cw_body2d b);
/* Distances from the shapes: min and max to the centre of mass, max to the body origin (conservative). */
float cw_body2d_min_extent(cw_body2d b);
float cw_body2d_max_extent(cw_body2d b);
float cw_body2d_max_extent_from_origin(cw_body2d b);
/* Recomputes the mass from the shapes (after you overrode it or changed shapes without updating). */
void cw_body2d_update_mass(cw_body2d b);
void cw_body2d_set_sleep_enabled(cw_body2d b, bool enabled);
bool cw_body2d_sleep_enabled(cw_body2d b);
/* Contacts made after the call use the new value. */
void cw_body2d_set_contact_recycling(cw_body2d b, bool enabled);
bool cw_body2d_contact_recycling(cw_body2d b);
void cw_body2d_set_safety_factor(cw_body2d b, float factor);
float cw_body2d_safety_factor(cw_body2d b);
/* m/s; zero (logged) for an invalid body. */
cw_vec2 cw_body2d_velocity(cw_body2d b);
/* m/s. */
void cw_body2d_set_velocity(cw_body2d b, cw_vec2 v);
/* rad/s, counter-clockwise positive; 0 for an invalid body. */
float cw_body2d_angular_velocity(cw_body2d b);
/* rad/s, counter-clockwise positive. */
void cw_body2d_set_angular_velocity(cw_body2d b, float w);
/* N at a world point (NULL = center of mass), for the next step. Wakes the body. */
void cw_body2d_apply_force(cw_body2d b, cw_vec2 force, const cw_pos2 *point);
/* N*s at a world point (NULL = center of mass). Wakes the body. */
void cw_body2d_apply_impulse(cw_body2d b, cw_vec2 impulse, const cw_pos2 *point);
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
  uint32_t debug_color;        /* 0xRRGGBB for this shape's wireframe; 0 = by body state */
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
/* Box with half extents, rounded by radius (the corners and the edges grow by radius), placed by local. */
cw_shape2d cw_shape2d_rounded_box(cw_body2d b, const cw_shape2d_def *def, cw_vec2 half, float radius, cw_xform2 local);
/* A loose one-sided chain segment p1..p2 with ghost end vertices (Box2D v3.2): the building block of a chain that
 * you grow or move segment by segment; collision on the right walking p1 to p2. Prefer cw_chain2d_create unless
 * you need to change segments at run time. Null (logged) when too short. */
cw_shape2d cw_shape2d_chain_segment(cw_body2d b, const cw_shape2d_def *def, cw_vec2 ghost1, cw_vec2 p1, cw_vec2 p2,
                                    cw_vec2 ghost2);
/* Turns the shape into (or updates) a loose chain segment; the shape must not belong to a cw_chain2d. Does not
 * change the mass. */
void cw_shape2d_set_chain_segment(cw_shape2d s, cw_vec2 ghost1, cw_vec2 p1, cw_vec2 p2, cw_vec2 ghost2);
/* The shape's world box; zero for an invalid shape. */
cw_aabb2 cw_shape2d_bounds(cw_shape2d s);
/* Does the world point lie inside the shape? */
bool cw_shape2d_test_point(cw_shape2d s, cw_pos2 point);
/* The closest point of the shape to target (world positions). */
cw_pos2 cw_shape2d_closest_point(cw_shape2d s, cw_pos2 target);
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
/* The material of every segment of a chain (Box2D v3.2 can also give each segment its own). */
void cw_chain2d_set_material(cw_chain2d c, const cw_phys_material *m);
/* One segment's material (no-op for an invalid chain or segment index). */
void cw_chain2d_set_segment_material(cw_chain2d c, int segment, const cw_phys_material *m);
/* One segment's material; all zero for an invalid chain or segment index. */
cw_phys_material cw_chain2d_material(cw_chain2d c, int segment);
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
  cw_pos2 target;
  /* MOVER: the commanded velocity (m/s) and the most force that may chase it, x and y separately (N). */
  cw_vec2 linear_velocity, max_velocity_force;
  /* POGO: a spring between frame_a's point (on body a) and frame_b's point (on body b), measured along +y of
   * frame_b (hertz, damping_ratio and length are its spring and rest length); it pushes on the bodies along
   * `normal` (the world normal of the ground it stands on), at most max_tension_force pulling and
   * max_compression_force pushing; pogo_impulse / pogo_velocity carry its state across a recreation (the pogo is
   * usually made again every frame). */
  cw_vec2 normal;
  float max_tension_force, max_compression_force, pogo_impulse, pogo_velocity;
  void *user;
} cw_joint2d_def;
/* Identity frames and per-type spring defaults; see cw_joint3d_default_def. */
cw_joint2d_def cw_joint2d_default_def(cw_joint_type type);
/* Fill frame_a/frame_b from a world anchor; frame x maps to world_axis (zero = +x). */
void cw_joint2d_def_anchor(cw_joint2d_def *def, cw_pos2 world_anchor, cw_vec2 world_axis);
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
/* MOVER: the commanded velocity (m/s) and the force limits (N); ignored for other joints. */
void cw_joint2d_set_mover(cw_joint2d j, cw_vec2 velocity, cw_vec2 max_force);
/* Distance and pogo: the spring's rest length. */
void cw_joint2d_set_rest_length(cw_joint2d j, float length);
float cw_joint2d_rest_length(cw_joint2d j);
/* Revolute, prismatic: the speed the joint is moving at (rad/s, m/s); pogo: its extension speed; others 0. */
float cw_joint2d_speed(cw_joint2d j);
/* The force (N) or torque (N*m) the motor is applying now (revolute, prismatic, wheel, distance); others 0. */
float cw_joint2d_motor_force(cw_joint2d j);
/* Distance: the spring's force now; pogo: its impulse; others 0. */
float cw_joint2d_spring_force(cw_joint2d j);
/* How far the joint is pulled apart: metres and radians. */
float cw_joint2d_linear_separation(cw_joint2d j);
float cw_joint2d_angular_separation(cw_joint2d j);
/* MOUSE only: the world point the grab point is pulled toward. */
void cw_joint2d_set_target(cw_joint2d j, cw_pos2 world_target);
/* Revolute: angle; prismatic: translation; distance: length; wheel: measured
 * spin speed (rad/s, b's angular velocity minus a's; as cw_joint3d_value); others 0. */
float cw_joint2d_value(cw_joint2d j);
/* The Box2D joint id. Leave its user data to the wrapper. */
b2JointId cw_joint2d_raw(cw_joint2d j);

/* ── queries (see physics3d.h; same contracts) ────────────────────────── */

typedef struct cw_ray_hit2d {
  cw_shape2d shape;
  cw_body2d body;
  cw_pos2 point;
  cw_vec2 normal;
  float fraction;
  uint64_t material;
  int triangle;                /* always -1 in 2D (kept for parity) */
} cw_ray_hit2d;

/* Closest hit along origin..origin+translation; false (hit zeroed) on a miss. */
bool cw_world2d_raycast(cw_world2d w, cw_pos2 origin, cw_vec2 translation, const cw_phys_filter *filter,
                        cw_ray_hit2d *hit);
/* The cap closest hits, sorted; returns how many. */
int cw_world2d_raycast_all(cw_world2d w, cw_pos2 origin, cw_vec2 translation, const cw_phys_filter *filter,
                           cw_ray_hit2d *hits, int cap);
/* Casts a ray at this one shape; false (hit zeroed) on a miss. hit->shape / body are filled. */
bool cw_shape2d_raycast(cw_shape2d s, cw_pos2 origin, cw_vec2 translation, cw_ray_hit2d *hit);
typedef struct cw_ray2d {
  cw_pos2 origin;
  cw_vec2 translation;
} cw_ray2d;
/* hits[i] answers rays[i] on the job pool; returns the hit count. Not during a step. */
int cw_world2d_raycast_batch(cw_world2d w, const cw_ray2d *rays, int count, const cw_phys_filter *filter,
                             cw_ray_hit2d *hits);

#define CW_QSHAPE2D_POINTS 8
typedef struct cw_qshape2d {
  cw_vec2 points[CW_QSHAPE2D_POINTS]; /* relative to origin */
  int count;
  float radius;
  cw_pos2 origin; /* zero from the constructors; cw_qshape2d_at moves the shape (the points keep their precision) */
} cw_qshape2d;
/* The shape with its points taken relative to a world position. */
static inline cw_qshape2d cw_qshape2d_at(cw_qshape2d s, cw_pos2 origin) {
  s.origin = origin;
  return s;
}
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
int cw_world2d_overlap_aabb(cw_world2d w, cw_pos2 lo, cw_pos2 hi, const cw_phys_filter *filter, cw_shape2d *out,
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
  cw_pos2 position, prev_position;
  cw_vec2 velocity;
  bool on_floor, on_wall, on_ceiling, stepped, snapped;
  cw_vec2 floor_normal, wall_normal;
  cw_body2d floor_body;
  cw_vec2 platform_velocity;
  float floor_cos;
} cw_mover2d;

/* Clears m, copies def (NULL = defaults; height >= 2 * radius) and places the feet. */
void cw_mover2d_init(cw_mover2d *m, cw_world2d w, const cw_mover2d_def *def, cw_pos2 feet);
/* Move-and-slide by velocity * dt; see cw_mover3d_move. Once per fixed step. */
void cw_mover2d_move(cw_mover2d *m, float dt);
/* Moves the feet with no interpolation and clears the contact state (velocity is kept). */
void cw_mover2d_teleport(cw_mover2d *m, cw_pos2 feet);
/* Feet position lerped between prev_position and position by alpha (rendering). */
cw_pos2 cw_mover2d_interpolated(const cw_mover2d *m, float alpha);

#ifdef __cplusplus
}
#endif
#endif
