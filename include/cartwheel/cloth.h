/* cartwheel cloth and ropes: XPBD particles that live next to a Box3D world
 * (physics3d.h). Capes, flags, banners, curtains, hanging ropes, chains and
 * rope bridges that swing, catch the wind, drape over bodies and pull on
 * what they hang from.
 *
 * Model (extended position-based dynamics: Macklin, Mueller, Chentanez 2016,
 * "Small Steps" 2019; long-range attachments: Kim et al. 2012; the shape of
 * the solver follows Jolt Physics' soft bodies and Ten Minute Physics, both
 * MIT):
 *   particles    position, previous position, velocity and inverse mass
 *                (0 = pinned: moved only by you or by what it is attached to)
 *   stretch      a distance constraint per mesh edge (rope segment)
 *   bend         a distance constraint across each pair of triangles that
 *                share an edge (rope: across every second particle)
 *   tethers      long-range attachments: no particle drifts farther from its
 *                nearest pinned particle than its rest distance along the
 *                mesh (x tether_slack), which is what keeps a long cape or a
 *                rope from stretching under its own weight at few iterations
 *   skin         optional per-particle max distance from a target you move
 *                (a cape stays near the back it is skinned to)
 *   wind         per-particle drag and lift against the air's velocity, by
 *                the particle's share of the surface
 *   collision    against Box3D spheres, capsules, boxes and convex hulls
 *                (static, kinematic and dynamic bodies), with friction;
 *                optionally against itself (self_collision)
 *
 * One cw_cloth_step, after cw_world3d_step each fixed tick: the shapes near
 * the cloth are gathered once (sorted by id), then `substeps` times: predict
 * (gravity, wind), solve every constraint once, collide, update velocities.
 * Body transforms are blended from their pose before the world step to the
 * pose after it across the substeps, so a moving body sweeps through the
 * cloth instead of jumping.
 *
 * Determinism: particles are cut into blocks of consecutive indices at
 * creation. Each substep solves the constraints inside pairs of blocks
 * {0,1}, {2,3}, ... as one job each, then those across the other boundaries
 * {1,2}, {3,4}, ...: the jobs of one phase share no particle, so they run in
 * parallel on the job pool without atomics, each a Gauss-Seidel sweep in a
 * fixed order, and the result does not depend on the thread count or the
 * order jobs run in. A cloth under 1024 particles runs on the calling thread. Only + - * / sqrt and cartwheel/math.h feed the
 * state: the same def and inputs give the same bits natively and in a cart,
 * with any number of workers (cw_cloth_hash).
 *
 * Coupling with bodies: one-way by default (bodies push the cloth, the cloth
 * does not push back). A particle attached to a dynamic body
 * (cw_cloth_attach_body) always pulls on it: the attachment is solved with
 * the body's effective mass at that point, and the momentum it took is
 * applied to the body as one impulse after the step (it acts in the world's
 * next step). two_way also hands collision impulses to dynamic bodies.
 *
 * Ropes are cloths without triangles (cw_rope_create builds the chain);
 * every cw_cloth_* function works on them. Chains of rigid links are plain
 * Box3D bodies and joints (cw_chain3d_create).
 *
 * Threads: main thread, outside a world step, like physics3d.h. The step
 * fans out to the job pool itself.
 *
 * Rendering: cw_cloth_geometry_new and cw_cloth_update_geometry (or
 * cw_rope_geometry_new for a tube) give a three.c geometry whose positions
 * are interpolated by the render alpha and whose normals are rebuilt every
 * frame; draw it with any material (double-sided for cloth). The Cloth3D and
 * Rope3D node types (physics_nodes.h) do all of this. */
#ifndef CW_CLOTH_H
#define CW_CLOTH_H

#include "physics3d.h"
#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

CW_HANDLE_TYPE(cw_cloth);

typedef struct cw_cloth_def {
  /* The mesh, in world space (copied). triangles: 3 indices each; ropes and
   * other line meshes pass edges instead (2 indices each, used as well as
   * the triangles' edges). */
  const t3_vec3 *positions;
  int vertex_count;
  const uint32_t *triangles;
  int triangle_count;
  const uint32_t *edges;
  int edge_count;
  const t3_vec2 *uvs;          /* per vertex, for rendering (NULL = none) */
  /* Per-vertex inverse mass scale: 0 pins the vertex, 1 = its share of
   * `mass` (NULL = all 1). */
  const float *inv_mass;
  float mass;                  /* kg of the whole cloth, spread by area (ropes: evenly): 1 */
  float stretch_compliance;    /* m/N of the edges: 0 = as stiff as the substeps allow */
  float bend_compliance;       /* m/N across neighbouring triangles: 0.01 (< 0 = no bending constraints) */
  float tether_slack;          /* tethers reach rest distance x this: 1.02 (0 = no tethers) */
  float damping;               /* share of velocity lost per second: 0.1 */
  float thickness;             /* m the cloth keeps from shapes (and itself): 0.02 */
  float friction;              /* against shapes, 0..1: 0.4 */
  int substeps;                /* per step, one iteration each: 8 */
  /* Skin targets: max_distance[v] (m) is how far vertex v may move from its
   * skin target (cw_cloth_set_skin_targets); NULL = no skin constraint, a
   * negative entry = free. */
  const float *max_distance;
  /* Collision with the world's shapes (spheres, capsules, boxes, hulls) on
   * these layers: all. 0 = none. */
  uint32_t collide_mask;
  bool two_way;                /* collisions push dynamic bodies back: false */
  bool self_collision;         /* particles keep `thickness` apart: false (costly) */
  float max_speed;             /* m/s, clamps runaway particles: 50 */
  int workers;                 /* 0 = the job pool; 1 = the calling thread only (same bits) */
  /* quality_scaled: a cosmetic cloth (it touches no game state: one-way, no
   * body attachments that matter) may follow the graphics settings' effects
   * detail: Low runs half the substeps and never self-collides. Off by
   * default, and ignored in a deterministic run (always High). */
  bool quality_scaled;
} cw_cloth_def;
/* The defaults noted in cw_cloth_def above (no mesh). */
cw_cloth_def cw_cloth_default_def(void);

/* Creates a cloth beside world w (its shapes collide; its gravity applies).
 * Null (logged) on an invalid world or mesh. Cost at creation: O(V + E log E)
 * (edges, colouring, tethers by Dijkstra from the pinned vertices). Lives
 * until destroyed or the world is. */
cw_cloth cw_cloth_create(cw_world3d w, const cw_cloth_def *def);
/* A rectangular grid of nu x nv vertices (at least 2 x 2) from `origin`
 * along `u` (the full width) and `v` (the full height), its quads split
 * along alternating diagonals; uvs (0,0)..(1,1). def's mesh fields are
 * ignored; its inv_mass, if given, has nu * nv entries (vertex i + j * nu). */
cw_cloth cw_cloth_create_grid(cw_world3d w, const cw_cloth_def *def, int nu, int nv, t3_vec3 origin, t3_vec3 u,
                              t3_vec3 v);
/* Frees the cloth (its geometry is yours: free it separately). */
void cw_cloth_destroy(cw_cloth c);
/* False for null, destroyed or stale handles. */
bool cw_cloth_valid(cw_cloth c);

/* One fixed step of dt seconds, after the world's step. Cost:
 * substeps x (V + constraints) plus the shapes near the cloth; fans out to
 * the job pool for large cloths. */
void cw_cloth_step(cw_cloth c, float dt);

/* ── pins, attachments, skin ──────────────────────────────────────────── */

/* Pins (inverse mass 0) or frees a vertex. A pinned vertex moves to its
 * target over the next step (blended across the substeps). */
void cw_cloth_pin(cw_cloth c, int vertex, bool pinned, t3_vec3 target);
/* Moves a pinned vertex's target (reached at the end of the next step). */
void cw_cloth_set_pin_target(cw_cloth c, int vertex, t3_vec3 target);
/* Ties vertex to the body-local point `local` of body b (pinned to it; a
 * null body frees the vertex). A dynamic body feels the pull. */
void cw_cloth_attach_body(cw_cloth c, int vertex, cw_body3d b, t3_vec3 local);
/* The skin targets of every vertex (vertex_count entries, copied), reached
 * at the end of the next step; vertices with max_distance >= 0 stay within
 * it of their target. Typically the rest mesh moved by a bone each tick. */
void cw_cloth_set_skin_targets(cw_cloth c, const t3_vec3 *targets);
/* Moves every particle (positions and previous positions) by the rigid
 * transform `x` applied after `from` is undone: a teleport with no motion. */
void cw_cloth_teleport(cw_cloth c, cw_xform3 from, cw_xform3 to);

/* ── wind and forces ──────────────────────────────────────────────────── */

/* Air velocity (m/s), drag and lift coefficients (per m^2 of cloth at the
 * density of air: drag 1 and lift 0.3 look like fabric). turbulence (0..1)
 * varies the wind smoothly over the cloth and over time (deterministic). */
void cw_cloth_set_wind(cw_cloth c, t3_vec3 wind, float drag, float lift, float turbulence);
/* Gravity scale (1 = the world's). */
void cw_cloth_set_gravity_scale(cw_cloth c, float scale);
/* Adds a velocity change (m/s) to every free particle within radius of
 * center, falling off linearly: a blast, a sword swipe. */
void cw_cloth_impulse(cw_cloth c, t3_vec3 center, float radius, t3_vec3 delta_v);

/* ── reading ──────────────────────────────────────────────────────────── */

/* Current particle positions (simulation values, not interpolated); NULL
 * (count 0) for an invalid cloth. Valid until the next step. */
const t3_vec3 *cw_cloth_positions(cw_cloth c, int *count);
/* Current particle velocities, as cw_cloth_positions. */
const t3_vec3 *cw_cloth_velocities(cw_cloth c, int *count);
/* Positions blended between the last two steps by alpha (rendering only). */
void cw_cloth_interpolated(cw_cloth c, float alpha, t3_vec3 *out);
/* vertex_count; 0 for an invalid cloth. */
int cw_cloth_vertex_count(cw_cloth c);
/* A running hash of every step's particle positions and velocities (bits). */
uint32_t cw_cloth_hash(cw_cloth c);

typedef struct cw_cloth_stats {
  int32_t vertices, triangles, constraints, groups, tethers, shapes; /* shapes: gathered in the last step */
  int32_t steps;
  float max_stretch;           /* the most stretched edge: its length over its rest length, minus 1 (0.01 = 1%) */
  float mean_stretch;          /* the same averaged over every edge (compressed edges count as 0) */
  float kinetic_energy;        /* J, of the free particles */
  float step_us;               /* the last step's wall time, microseconds (not simulation state) */
} cw_cloth_stats;
/* Counts, stretch and energy now (O(V + E)); all zero for an invalid cloth. */
cw_cloth_stats cw_cloth_stats_get(cw_cloth c);
/* The force (N) with which the cloth pulled the dynamic body attached at
 * `vertex` over the last step (the impulse it applied over dt): a rope's
 * tension at a crate. 0 when the vertex is not attached to a dynamic body. */
float cw_cloth_attachment_force(cw_cloth c, int vertex);

/* ── rollback ─────────────────────────────────────────────────────────── */

/* Bytes cw_cloth_save writes: the particles, targets and running hash. */
size_t cw_cloth_state_size(cw_cloth c);
/* Copies the state out (out holds cw_cloth_state_size bytes); false when invalid. */
bool cw_cloth_save(cw_cloth c, void *out);
/* Restores a state saved from this cloth (same mesh); false when the size differs. */
bool cw_cloth_load(cw_cloth c, const void *state, size_t size);

/* ── rendering (three.c; not simulation) ──────────────────────────────── */

/* A geometry with the cloth's triangles, uvs, positions and normals, its
 * position and normal attributes T3_DYNAMIC_DRAW. NULL for a cloth without
 * triangles (use cw_rope_geometry_new). The caller owns it. */
t3_geometry *cw_cloth_geometry_new(cw_cloth c);
/* Writes the positions interpolated by alpha and rebuilds the normals
 * (area-weighted, in a fixed order), bumps the attributes' versions and the
 * bounding sphere. Works on cloth and rope geometries. */
void cw_cloth_update_geometry(cw_cloth c, t3_geometry *g, float alpha);

/* ── ropes ────────────────────────────────────────────────────────────── */

typedef struct cw_rope_def {
  t3_vec3 start, end;          /* the first and last particle at creation */
  int segments;                /* particles - 1: 16 */
  float length;                /* rest length (m): 0 = |end - start| (a longer rope sags) */
  float mass;                  /* kg: 1 */
  float stretch_compliance;    /* m/N: 0 */
  float bend_compliance;       /* m/N: 1 (ropes bend freely; < 0 = no bending constraints) */
  float damping;               /* 0.1 */
  float thickness;             /* m: the rope's radius for collision: 0.03 */
  float friction;              /* 0.4 */
  int substeps;                /* 16 */
  bool pin_start, pin_end;     /* pinned where created: true, false */
  uint32_t collide_mask;       /* all */
  bool two_way;                /* false */
  int workers;                 /* 0 */
} cw_rope_def;
/* The defaults noted in cw_rope_def above. */
cw_rope_def cw_rope_default_def(void);
/* A chain of segments + 1 particles laid along start..end (sagging below
 * the line when length is longer, as a catenary-like curve). Edge i joins
 * particles i and i + 1. Null (logged) on an invalid world or < 1 segment. */
cw_cloth cw_rope_create(cw_world3d w, const cw_rope_def *def);
/* A tube of `sides` around the rope's particles with this radius (the
 * caller owns it); update it with cw_cloth_update_geometry. */
t3_geometry *cw_rope_geometry_new(cw_cloth c, float radius, int sides);

/* ── chains of rigid links ────────────────────────────────────────────── */

typedef struct cw_chain3d_def {
  t3_vec3 start, end;          /* the first link starts here, the last ends here */
  int links;                   /* 12 */
  float radius;                /* capsule radius: 0.05 */
  float density;               /* kg/m^3: 1000 */
  float damping;               /* the links' linear and angular damping: 0.1 */
  cw_body3d attach_start, attach_end; /* joined to these bodies (null: start = the world, end = free) */
  bool fix_start;              /* start joined to the world when attach_start is null: true */
  int layer;                   /* the links' collision layer: 0 */
  uint32_t mask;               /* all */
  int group;                   /* < 0: links never collide with each other: -1 */
} cw_chain3d_def;
/* The defaults noted in cw_chain3d_def above. */
cw_chain3d_def cw_chain3d_default_def(void);
/* Capsule links joined end to end by spherical joints: a chain of Box3D
 * bodies (heavy chains, a wrecking ball). Writes up to `links` bodies and
 * links + 1 joints (null where an end is free) and returns how many links
 * it made. The bodies and joints are ordinary world objects: destroy them
 * with the world or one by one. */
int cw_chain3d_create(cw_world3d w, const cw_chain3d_def *def, cw_body3d *links, cw_joint3d *joints);

/* ── nodes ──────────────────────────────────────────────────────────────
 * Cloth3D: a rectangle of columns x rows quads hanging from the node down
 * its -y and along its +x, pinned along an edge or at corners (`pin`). The
 * pins follow `attach_to` (a node path; empty = the node itself): a body
 * node (RigidBody3D ...) is attached to, so a dynamic body feels the pull;
 * any other Node3D moves the pins with its global transform each tick (a
 * cape on a character, a flag on a moving pole), or `bone` names a three.c
 * object under that node's model (a skeleton's joint) whose transform they
 * follow instead. skin_distance keeps every
 * particle near the rest shape riding that frame. Wind, pattern colours and
 * fabric sheen are properties.
 * Rope3D: a rope from the node to `end` (node space), XPBD particles or a
 * chain of rigid Box3D links (`mode`). Each end hangs from a body node
 * (start_attach / end_attach), follows the node (pin_start / pin_end) or
 * is free.
 * Both build on the first tick in a tree (after the bodies they name
 * exist), step right after the tree's 3D world, and draw through three.c in
 * the tree's scene (GL and WebGPU). Their simulation hash is the read-only
 * property `hash`. */

/* The node's cloth (null before its first tick or for another type). */
cw_cloth cw_cloth3d_get(cw_node n);
/* A Rope3D's rope (null before its first tick, in LINKS mode or for another type). */
cw_cloth cw_rope3d_get(cw_node n);

#ifdef __cplusplus
}
#endif
#endif
