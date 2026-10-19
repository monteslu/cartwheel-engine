/* cartwheel physics: thin wrappers over Box2D v3 (2D) and Box3D (3D) with
 * PARALLEL APIs. This header holds what both share; physics2d.h and
 * physics3d.h hold the per-dimension functions, which mirror each other
 * name for name (cw_world2d_step / cw_world3d_step, cw_body2d_create /
 * cw_body3d_create, ...). Including this header includes both.
 *
 * What the wrapper adds over the raw libraries (and nothing else; every
 * handle has a *_raw() accessor returning the library id for the rest):
 *   - worlds stepped on the engine job pool (cartwheel/physics_tasks.h);
 *   - 32 named collision layers per world, mapped to filter bits;
 *   - one per-step EVENT LIST (contacts, sensors, hits, joint breaks) in the
 *     libraries' deterministic order, no allocation per event;
 *   - queries (ray / shape cast / overlap) filling caller arrays, sorted
 *     deterministically, plus a batched ray cast on the job pool;
 *   - character movers (move-and-slide on the libraries' mover APIs);
 *   - transform sync: bodies bound to a three.c object or a plain transform,
 *     interpolated between the last two fixed steps at render time;
 *   - debug draw into a flat line list; stats and a per-step state hash.
 *
 * Handles: worlds, bodies, shapes and joints are the libraries' own
 * generational ids packed into 64 bits (b2Store*Id / b3Store*Id), so a
 * stale handle reads as invalid, never as another object. The null handle
 * is 0.
 *
 * Ownership the libraries' user-data slots: cartwheel uses the WORLD's and
 * the BODY's and the JOINT's user data for its own records. Use
 * cw_body*_set_user / cw_joint*_set_user instead. Shape user data is yours
 * (cw_shape*_def.user). Bodies and joints you create through the raw
 * library API must leave their user data NULL (the wrapper then ignores
 * them in sync and events, but still hashes their motion).
 *
 * Determinism: everything that feeds the simulation (movers, queries,
 * events, the state hash) uses only + - * / sqrt and cartwheel/math.h. The
 * render-time interpolation is not simulation and may differ in the last
 * bit between hosts; never feed it back into game state.
 *
 * Threads: every function here is main-thread only, except that query
 * functions may run concurrently with each other (never with a step or a
 * world change). The batched ray cast does exactly that on the job pool. */
#ifndef CW_PHYSICS_H
#define CW_PHYSICS_H

#include "base.h"
#include "core.h"
#include "math.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── shared enums and plain types ─────────────────────────────────── */

typedef enum { CW_BODY_STATIC, CW_BODY_KINEMATIC, CW_BODY_DYNAMIC } cw_body_type; /* = b2/b3BodyType */

/* Surface material. friction 0..1 (mixed sqrt(a*b)), restitution 0..1
 * (mixed max), rolling_resistance 0..1 (circles/spheres/capsules), tangent
 * speed for conveyors (2D: along the surface; 3D: tangent_velocity in the
 * shape's local frame), user_material: your id for sound/particle lookup,
 * reported in hit events and query hits. */
typedef struct cw_phys_material {
  float friction, restitution, rolling_resistance;
  float tangent_speed;   /* 2D */
  float tangent_velocity[3]; /* 3D, shape-local */
  uint64_t user_material;
} cw_phys_material;
cw_phys_material cw_phys_material_default(void); /* friction 0.6, the rest 0 */

/* ── collision layers ─────────────────────────────────────────────────
 * A shape sits on ONE layer (0..31, category bit 1<<layer) and collides
 * with the layers in its mask (bit i = layer i). Two shapes collide when
 * each one's mask holds the other's layer, unless both share a nonzero
 * group: negative = never collide, positive = always.
 *
 * Bits above 31 are reserved for queries: every non-sensor shape accepts
 * queries, sensors only when a query asks for them (cw_phys_filter.sensors).
 * Do not set shape filters through the raw library APIs. */
#define CW_PHYS_LAYERS 32
#define CW_PHYS_ALL_LAYERS 0xffffffffu

/* ── events ───────────────────────────────────────────────────────────
 * After each step the world holds one event list, read with
 * cw_world*_events() and valid until the next step. Order (fixed, and
 * within each kind the library's own deterministic order):
 *   contact begin, contact end, hit, sensor enter, sensor exit, joint break.
 * Contact events need contact_events on BOTH shapes (the default), hit
 * events need hit_events on one shape and an approach speed above the
 * world's hit_speed, sensor events need sensor_events on the sensor and on
 * the visitor (the default). For END/EXIT events the shape may already be
 * destroyed: then body_a/body_b are null. */
typedef enum {
  CW_PHYS_CONTACT_BEGIN, /* shape_a, shape_b started touching */
  CW_PHYS_CONTACT_END,   /* stopped touching */
  CW_PHYS_HIT,           /* point, normal (a to b), speed, materials */
  CW_PHYS_SENSOR_ENTER,  /* shape_a = sensor, shape_b = visitor */
  CW_PHYS_SENSOR_EXIT,
  CW_PHYS_JOINT_BREAK,   /* joint (now destroyed), body_a/b, user */
  CW_PHYS_EVENT_TYPE_COUNT
} cw_phys_event_type;

/* ── query filter ─────────────────────────────────────────────────────
 * layers: which layers a query hits (CW_PHYS_ALL_LAYERS = all).
 * sensors: also report sensor shapes. ignore_body: skip this body's shapes
 * (the caster itself), as a packed body handle (cw_body2d.id / cw_body3d.id).
 * fn: optional per-shape filter (shape handle id, ctx) -> keep; it runs on
 * the querying thread (a job-pool thread for batched rays). */
typedef struct cw_phys_filter {
  uint32_t layers;
  bool sensors;
  uint64_t ignore_body;
  bool (*fn)(uint64_t shape_id, void *ctx);
  void *ctx;
} cw_phys_filter;
/* Filter for every non-sensor shape on every layer (same as passing NULL). */
static inline cw_phys_filter cw_phys_filter_all(void) { return (cw_phys_filter){ .layers = CW_PHYS_ALL_LAYERS }; }
/* Filter for the non-sensor shapes on the given layer bits. */
static inline cw_phys_filter cw_phys_filter_layers(uint32_t layers) { return (cw_phys_filter){ .layers = layers }; }

/* ── stats (publish as debug fields: cw_debug_i32("phys.bodies", &s.bodies)) */
typedef struct cw_phys_stats {
  int32_t bodies, awake_bodies, shapes, contacts, joints;
  int32_t events;      /* in the last step's list */
  int32_t steps;       /* steps since creation */
  uint32_t hash;       /* running hash of every step's moved transforms and events */
  float step_ms;       /* the library's own step time (profiling only: host-dependent) */
} cw_phys_stats;

/* ── debug draw output ────────────────────────────────────────────────
 * A flat line-segment list (2D lines have z = 0) that any renderer can draw
 * as GL_LINES: pos holds 6 floats per segment (x0 y0 z0 x1 y1 z1), color one
 * 0xRRGGBB per segment. cw_world*_debug_draw APPENDS; clear between frames
 * with cw_phys_lines_clear (keeps capacity: no allocation once warm). */
typedef struct cw_phys_lines {
  CW_ARRAY(float) pos;
  CW_ARRAY(uint32_t) color;
} cw_phys_lines;
/* Empties the list, keeping its capacity. */
static inline void cw_phys_lines_clear(cw_phys_lines *l) { cw_array_clear(&l->pos); cw_array_clear(&l->color); }
/* Number of segments in the list. */
static inline int cw_phys_lines_count(const cw_phys_lines *l) { return l->color.count; }
/* Appends one segment a..b; color is 0xRRGGBB (higher bits dropped). Grows the arrays as needed. */
void cw_phys_lines_add(cw_phys_lines *l, const float a[3], const float b[3], uint32_t color);
/* Frees the list's arrays; it is empty and reusable afterwards. */
void cw_phys_lines_free(cw_phys_lines *l);

enum {
  CW_PHYS_DRAW_SHAPES = 1 << 0,
  CW_PHYS_DRAW_JOINTS = 1 << 1,
  CW_PHYS_DRAW_BOUNDS = 1 << 2,
  CW_PHYS_DRAW_CONTACTS = 1 << 3, /* points and normals */
  CW_PHYS_DRAW_MASS = 1 << 4,
  /* 2D only (Box2D v3.2; 3D ignores them): */
  CW_PHYS_DRAW_JOINT_EXTRAS = 1 << 5,    /* limits, springs and the like on joints */
  CW_PHYS_DRAW_ANCHOR_A = 1 << 6,        /* contact points on anchor A instead of B */
  CW_PHYS_DRAW_CHAIN_NORMALS = 1 << 7,   /* which side of a chain is solid */
  CW_PHYS_DRAW_CONTACT_FORCES = 1 << 8,
  CW_PHYS_DRAW_FRICTION_FORCES = 1 << 9,
  CW_PHYS_DRAW_ISLANDS = 1 << 10,        /* the boxes around groups of touching bodies that sleep together */
  CW_PHYS_DRAW_GRAPH_COLORS = 1 << 11,   /* contacts tinted by the solver's graph colour */
  CW_PHYS_DRAW_DEFAULT = CW_PHYS_DRAW_SHAPES | CW_PHYS_DRAW_JOINTS,
};

/* ── joints: one type enum for both dimensions ────────────────────────
 * Which exist where (the libraries' own set):
 *   both: DISTANCE, FILTER (no collision between two bodies), MOTOR,
 *         PRISMATIC, REVOLUTE, WELD, WHEEL, MOUSE (a motor joint from an
 *         internal kinematic body named "cw_mouse", moved to the target
 *         each step: drag with a pointer)
 *   3D only: SPHERICAL (ball and socket, cone + twist limits), PARALLEL
 *         (keeps z axes aligned: keep a body upright)
 *   2D only: MOVER (Box2D v3.2's dynamic character mover: a velocity command on a dynamic body, x and y
 *         separately, limited by a force; no rotation), POGO (a spring along a normal that holds a dynamic
 *         character off the ground, with a tension and a compression limit) */
typedef enum {
  CW_JOINT_DISTANCE, CW_JOINT_FILTER, CW_JOINT_MOTOR, CW_JOINT_PRISMATIC, CW_JOINT_REVOLUTE, CW_JOINT_WELD,
  CW_JOINT_WHEEL, CW_JOINT_MOUSE, CW_JOINT_SPHERICAL, CW_JOINT_PARALLEL, CW_JOINT_MOVER, CW_JOINT_POGO,
  CW_JOINT_TYPE_COUNT
} cw_joint_type;

#ifdef __cplusplus
}
#endif

#include "physics2d.h"
#include "physics3d.h"

#endif
