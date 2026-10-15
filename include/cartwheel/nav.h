/* cartwheel navigation: navmeshes, path queries, obstacles and crowds.
 *
 *   build:    cw_nav_geom g = {0}; cw_nav_geom_add_world3d(&g, world, NULL, CW_NAV_AREA_WALK);
 *             cw_nav_config c = cw_nav_config_default();
 *             cw_navmesh nm = cw_navmesh_build(&c, &g);
 *   query:    n = cw_nav_find_path(nm, from, to, NULL, pts, 64, NULL);
 *   crowd:    cw_crowd cr = cw_crowd_new(nm, 500);
 *             a = cw_crowd_add(cr, p, NULL); cw_crowd_set_target(cr, a, goal);
 *             cw_crowd_step(cr, dt);  (every fixed tick)
 *
 * The navmesh is Recast and Detour's (zlib; patches/recastnavigation):
 * geometry is voxelized per tile, tiles become convex polygons, and Detour
 * answers queries over them. The crowd is cartwheel's own C, with ORCA
 * avoidance ported from RVO2 (Apache-2.0, see NOTICE).
 *
 * Determinism: a navmesh built from the same geometry and config has the same
 * bytes (cw_navmesh_hash) and the same polygon refs on every host and at any
 * worker count. Tiles build in parallel on the job pool and join the navmesh
 * in (z, x) order; the Recast patches make sorting, trigonometry and memory
 * identical across C libraries. Queries, obstacle updates and crowd steps
 * give the same results everywhere too: the crowd's phases read the state
 * of the step's start and write only their own agent, and budgets count
 * requests and search iterations, never time.
 *
 * Threads: build, update, tick and step from one thread at a time (the main
 * thread, usually). The query calls (cw_nav_find_path, nearest, raycast,
 * random point, poly height) are safe from job-pool threads while nothing
 * changes the navmesh: each pool thread has its own Detour query object.
 *
 * Memory: CW_MEM_NAV. */
#ifndef CW_NAV_H
#define CW_NAV_H

#include "core.h"
#include "math.h"
#include "physics3d.h"
#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

CW_HANDLE_TYPE(cw_navmesh);
CW_HANDLE_TYPE(cw_crowd);
CW_HANDLE_TYPE(cw_crowd_agent);

/* ── areas and filters ─────────────────────────────────────────────────
 * Every triangle of the input carries an area id: 0 is not walkable, 1..63
 * are walkable kinds of ground (CW_NAV_AREA_WALK is the default). Polygons
 * keep their area; filters exclude areas and weigh their cost. */
#define CW_NAV_AREA_NONE 0
#define CW_NAV_AREA_WALK 63
#define CW_NAV_AREAS 64

typedef struct cw_nav_filter {
  uint64_t exclude_areas;          /* bit n: never enter area n */
  float area_cost[CW_NAV_AREAS];   /* multiplies distance through the area; must be >= 1 */
} cw_nav_filter;
/* Every area allowed, every cost 1 (what NULL means for a filter argument). */
cw_nav_filter cw_nav_filter_default(void);

/* ── input geometry ────────────────────────────────────────────────────
 * A triangle soup in world space with an area per triangle. Fill it from
 * any mix of sources; the build reads it and does not keep it. */
typedef struct cw_nav_geom {
  CW_ARRAY(float) verts;   /* x, y, z per vertex */
  CW_ARRAY(int32_t) tris;  /* 3 vertex indices per triangle */
  CW_ARRAY(uint8_t) areas; /* one per triangle */
} cw_nav_geom;

/* Appends triangles: verts (xyz per vertex), idx (3 per triangle; NULL =
 * consecutive triples), transformed by xf (NULL = identity), with one area. */
void cw_nav_geom_add_tris(cw_nav_geom *g, const float *verts, int vert_count, const int32_t *idx, int tri_count,
                          const t3_mat4 *xf, uint8_t area);
/* An axis-aligned box's 12 triangles (a quick blocker or floor). */
void cw_nav_geom_add_box(cw_nav_geom *g, t3_vec3 center, t3_vec3 half, uint8_t area);
/* Every mesh in a three.c object tree (skinned meshes excluded), placed by
 * xf (NULL: the root's matrix_world), each child by its own local matrix. */
void cw_nav_geom_add_object(cw_nav_geom *g, const t3_object *root, const t3_mat4 *xf, uint8_t area);
/* The static bodies' shapes of a physics world (filter: their layers; NULL
 * = every non-sensor shape), in body creation order: hulls and boxes by their
 * faces, meshes and height fields by their triangles, spheres and capsules
 * as 8-sided prisms. Returns the triangles added. */
int cw_nav_geom_add_world3d(cw_nav_geom *g, cw_world3d w, const cw_phys_filter *filter, uint8_t area);
/* Bounds of every vertex; false when empty. */
bool cw_nav_geom_bounds(const cw_nav_geom *g, t3_vec3 *bmin, t3_vec3 *bmax);
void cw_nav_geom_clear(cw_nav_geom *g); /* empties it, keeps capacity */
void cw_nav_geom_free(cw_nav_geom *g); /* frees its arrays; it is empty and reusable afterwards */

/* ── build ─────────────────────────────────────────────────────────────── */

typedef enum {
  CW_NAV_PARTITION_WATERSHED, /* best polygons; the default */
  CW_NAV_PARTITION_MONOTONE,  /* fastest; long thin polygons */
  CW_NAV_PARTITION_LAYERS,    /* fast, no holes; good for tiles */
} cw_nav_partition;

typedef struct cw_nav_config {
  float cell_size;         /* 0.3: voxel width (world units) */
  float cell_height;       /* 0.2: voxel height */
  float agent_height;      /* 2.0: ceiling an agent needs */
  float agent_radius;      /* 0.6: the navmesh keeps this far from walls */
  float agent_max_climb;   /* 0.9: steps it walks up */
  float agent_max_slope;   /* 45: degrees */
  int tile_size;           /* 48: voxels per tile side; 0 = one tile over the whole bounds */
  float region_min_size;   /* 8: smaller islands (in cells, sqrt of area) are dropped */
  float region_merge_size; /* 20: smaller regions merge into neighbours */
  float edge_max_len;      /* 12: longest polygon edge (world units; 0 = no limit) */
  float edge_max_error;    /* 1.3: contour simplification (cells) */
  int verts_per_poly;      /* 6: 3..6 */
  float detail_sample_dist;      /* 6: height detail sampling (cells; 0 = no detail mesh) */
  float detail_sample_max_error; /* 1: (cells) */
  cw_nav_partition partition;    /* WATERSHED */
  /* Dynamic navmeshes build through Detour's tile cache: obstacles can then
   * be added and removed at run time (cw_nav_obstacle_*), each change
   * rebuilding only the tiles it touches. They have no height detail mesh. */
  bool dynamic;            /* false */
  int max_obstacles;       /* 256 (dynamic only) */
  int max_layers;          /* 8: stacked floors per tile (dynamic only) */
  int max_search_nodes;    /* 2048: A* node pool per query (bounds a search's memory and work) */
  t3_vec3 bmin, bmax;      /* the area to build; all zero = the geometry's bounds */
} cw_nav_config;
cw_nav_config cw_nav_config_default(void); /* the defaults noted in cw_nav_config above */

/* Builds the navmesh: tiles in parallel on the job pool, added in (z, x)
 * order. Null (logged) on bad config, empty geometry or out of memory. */
cw_navmesh cw_navmesh_build(const cw_nav_config *config, const cw_nav_geom *geom);
/* Rebuilds the tiles that overlap [bmin, bmax] from new geometry (the whole
 * scene's, or at least everything near the box), in parallel; tiles outside
 * the original bounds are not added. Static navmeshes only. */
bool cw_navmesh_rebuild_area(cw_navmesh nm, const cw_nav_geom *geom, t3_vec3 bmin, t3_vec3 bmax);
void cw_navmesh_free(cw_navmesh nm); /* null or stale: no-op; frees its crowds too */
bool cw_navmesh_valid(cw_navmesh nm); /* false for a null, freed or failed navmesh */
/* The navmesh as bytes (cw_alloc'd; free with cw_free): the config and every
 * tile, in tile order. Equal navmeshes give equal bytes on every host. A
 * dynamic navmesh saves its tile cache and its obstacles. NULL when invalid. */
void *cw_navmesh_save(cw_navmesh nm, size_t *len);
/* A navmesh from cw_navmesh_save's bytes; null (logged) on bad data. */
cw_navmesh cw_navmesh_load(const void *data, size_t len);
/* A hash of every tile's bytes in tile order: the cross-host determinism check. */
uint64_t cw_navmesh_hash(cw_navmesh nm);
const cw_nav_config *cw_navmesh_config(cw_navmesh nm); /* NULL when invalid */

typedef struct cw_navmesh_stats {
  int tiles, polys, verts;   /* tiles holding polygons; their polygons and vertices */
  int tiles_x, tiles_z;      /* the tile grid */
  int obstacles;             /* live obstacles (dynamic) */
  int64_t bytes;             /* tile data */
  float build_ms;            /* the last build or update, wall time (never feeds state) */
} cw_navmesh_stats;
cw_navmesh_stats cw_navmesh_get_stats(cw_navmesh nm); /* all zero for an invalid navmesh */

/* ── queries ───────────────────────────────────────────────────────────
 * Points snap to the navmesh within the query extents (half sizes of a box
 * around the point: 2, 4, 2 by default). filter NULL = cw_nav_filter_default. */
void cw_navmesh_set_query_extents(cw_navmesh nm, t3_vec3 half_extents);

/* The nearest navmesh point to p; false when no polygon is within the extents. */
bool cw_nav_nearest(cw_navmesh nm, t3_vec3 p, const cw_nav_filter *filter, t3_vec3 *out);
/* The navmesh height under (or over) p: false off the navmesh. */
bool cw_nav_height(cw_navmesh nm, t3_vec3 p, float *y);
/* A path from a to b as straight-line waypoints (a first, b, or the reachable
 * point closest to it, last); returns the waypoint count (0: no path; at most
 * max). *partial (may be NULL): b is not reachable and the path ends near it. */
int cw_nav_find_path(cw_navmesh nm, t3_vec3 a, t3_vec3 b, const cw_nav_filter *filter, t3_vec3 *pts, int max,
                     bool *partial);
/* Walks the navmesh surface from a toward b: false when a is off the mesh.
 * hit: a wall stops the ray; t in [0, 1] is how far (1 = reached b), normal
 * the wall's (xz) and point where it stopped. */
typedef struct cw_nav_hit {
  bool hit;
  float t;
  t3_vec3 point, normal;
} cw_nav_hit;
bool cw_nav_raycast(cw_navmesh nm, t3_vec3 a, t3_vec3 b, const cw_nav_filter *filter, cw_nav_hit *out); /* see cw_nav_hit */
/* A uniformly random navmesh point (area weighted), from rng. */
bool cw_nav_random_point(cw_navmesh nm, const cw_nav_filter *filter, cw_rng *rng, t3_vec3 *out);
/* A random point within radius of center, reachable from it. */
bool cw_nav_random_point_near(cw_navmesh nm, t3_vec3 center, float radius, const cw_nav_filter *filter, cw_rng *rng,
                              t3_vec3 *out);

/* Many paths at once, in parallel on the job pool: request i writes up to
 * max_points waypoints at pts[i * max_points] and its count at counts[i]
 * (the same answer as cw_nav_find_path). */
typedef struct cw_nav_path_req {
  t3_vec3 from, to;
  const cw_nav_filter *filter; /* NULL = default */
} cw_nav_path_req;
void cw_nav_find_paths(cw_navmesh nm, const cw_nav_path_req *reqs, int count, t3_vec3 *pts, int max_points,
                       int *counts); /* see cw_nav_path_req */

/* ── the per-tick query budget ─────────────────────────────────────────
 * Requests queue and are answered by cw_nav_tick: at most max_requests per
 * tick, oldest first, each searching at most max_iters A* iterations (a path
 * that runs out ends at the closest point found: partial). Which requests a
 * tick answers, and their answers, depend only on the request order and the
 * budget: never on time or thread count. Answers run in parallel. */
typedef enum {
  CW_NAV_PATH_UNKNOWN,  /* no such ticket (never issued, or already taken) */
  CW_NAV_PATH_PENDING,  /* queued */
  CW_NAV_PATH_DONE,     /* a complete path */
  CW_NAV_PATH_PARTIAL,  /* the best path the budget or the navmesh allowed */
  CW_NAV_PATH_FAILED,   /* an end is off the navmesh */
} cw_nav_path_status;
/* Queues a path; returns its ticket (never 0). */
uint32_t cw_nav_request_path(cw_navmesh nm, t3_vec3 from, t3_vec3 to, const cw_nav_filter *filter);
/* Answers up to max_requests queued requests (0 = all), each with at most max_iters iterations (0 = the node pool's limit). */
void cw_nav_tick(cw_navmesh nm, int max_requests, int max_iters);
/* A request's status. Once answered (DONE, PARTIAL, FAILED) it copies up to
 * max waypoints into pts and their count into *count (both may be NULL) and
 * forgets the ticket. */
cw_nav_path_status cw_nav_take_path(cw_navmesh nm, uint32_t ticket, t3_vec3 *pts, int max, int *count);
void cw_nav_cancel_path(cw_navmesh nm, uint32_t ticket); /* forgets a queued or answered request */
int cw_nav_pending_paths(cw_navmesh nm); /* requests not yet answered */

/* ── obstacles (dynamic navmeshes) ─────────────────────────────────────
 * Adding and removing queue a change; cw_nav_update applies the queue: the
 * touched tiles rebuild in parallel and are committed in ascending tile
 * order. Ids are never 0; 0 means the obstacle could not be added. */
uint32_t cw_nav_obstacle_add_cylinder(cw_navmesh nm, t3_vec3 base, float radius, float height);
uint32_t cw_nav_obstacle_add_box(cw_navmesh nm, t3_vec3 center, t3_vec3 half, float y_rotation); /* half sizes; radians about y */
bool cw_nav_obstacle_remove(cw_navmesh nm, uint32_t id); /* false for an unknown id or a full change queue */
/* Applies queued obstacle changes; returns the tiles rebuilt. */
int cw_nav_update(cw_navmesh nm);

/* ── crowds ────────────────────────────────────────────────────────────
 * Agents on a navmesh that steer along their paths and avoid each other
 * (ORCA: optimal reciprocal collision avoidance). cw_crowd_step advances
 * every agent one fixed tick in four phases on the job pool:
 *   1. paths: the oldest agents waiting for one get it (the plan budget),
 *   2. neighbours: a grid by counting sort, then each agent's nearest
 *      (distance, then index),
 *   3. steering and avoidance: each agent's velocity from its path corners
 *      and its neighbours' velocities at the step's start,
 *   4. movement: each agent moves along the navmesh surface.
 * Agent iteration is in index order and every phase writes only its own
 * agent, so a step's result is the same at any worker count. */

typedef struct cw_crowd_agent_params {
  float radius;         /* 0.5 */
  float height;         /* 2 */
  float max_speed;      /* 3.5 (units/s) */
  float max_accel;      /* 8 (units/s²) */
  float neighbor_dist;  /* 6: neighbours within this distance are avoided */
  int max_neighbors;    /* 10 (at most CW_CROWD_MAX_NEIGHBORS) */
  float time_horizon;   /* 2 s: how far ahead other agents are avoided */
  float arrive_dist;    /* 0.3: the target counts as reached within this distance */
  float slowdown_dist;  /* 1.5: speed falls off within this distance of the target */
  bool avoidance;       /* true: ORCA against other agents (others avoid this one either way) */
  float avoidance_priority; /* 0.5: between two agents each avoids in proportion to the OTHER's
                             * priority (equal priorities: half each; 0 makes the other avoid it all) */
} cw_crowd_agent_params;
#define CW_CROWD_MAX_NEIGHBORS 16
cw_crowd_agent_params cw_crowd_agent_params_default(void); /* the defaults noted in cw_crowd_agent_params above */

typedef enum {
  CW_CROWD_IDLE,       /* no target */
  CW_CROWD_WAITING,    /* target set, its path not planned yet */
  CW_CROWD_MOVING,
  CW_CROWD_ARRIVED,    /* within arrive_dist of the target */
  CW_CROWD_NO_PATH,    /* target unreachable (the agent stays put) */
  CW_CROWD_VELOCITY,   /* moving by cw_crowd_set_velocity */
} cw_crowd_agent_status;

typedef struct cw_crowd_agent_state {
  t3_vec3 position, velocity;
  t3_vec3 desired_velocity;  /* before avoidance */
  t3_vec3 target, next_corner;
  cw_crowd_agent_status status;
  bool partial;              /* the path ends short of the target */
  int path_polys;            /* corridor length */
  uint32_t path_version;     /* bumped whenever a new path is taken */
} cw_crowd_agent_state;

/* A crowd of up to max_agents agents on nm (null when nm is invalid). */
cw_crowd cw_crowd_new(cw_navmesh nm, int max_agents);
void cw_crowd_free(cw_crowd c); /* null or stale: no-op */
bool cw_crowd_valid(cw_crowd c); /* false for a null or freed crowd (freeing its navmesh frees it) */
/* Paths planned per step (oldest waiting agents first; 8) and search iterations each (0 = the node pool's limit). */
void cw_crowd_set_plan_budget(cw_crowd c, int paths_per_step, int max_iters);
void cw_crowd_set_filter(cw_crowd c, const cw_nav_filter *filter); /* NULL = default */
/* Adds an agent at p (snapped to the navmesh); params NULL = defaults. Null
 * when the crowd is full or p is off the navmesh. */
cw_crowd_agent cw_crowd_add(cw_crowd c, t3_vec3 p, const cw_crowd_agent_params *params);
void cw_crowd_remove(cw_crowd c, cw_crowd_agent a); /* stale handle: no-op; the slot is reused lowest first */
bool cw_crowd_agent_valid(cw_crowd c, cw_crowd_agent a); /* false once removed */
void cw_crowd_set_params(cw_crowd c, cw_crowd_agent a, const cw_crowd_agent_params *params); /* NULL = defaults */
/* Plans a path to target (snapped to the navmesh) at a coming step. */
void cw_crowd_set_target(cw_crowd c, cw_crowd_agent a, t3_vec3 target);
void cw_crowd_clear_target(cw_crowd c, cw_crowd_agent a); /* stops (IDLE) */
/* Moves at velocity v (xz; no path) while avoiding others, until a target is set. */
void cw_crowd_set_velocity(cw_crowd c, cw_crowd_agent a, t3_vec3 v);
void cw_crowd_teleport(cw_crowd c, cw_crowd_agent a, t3_vec3 p); /* snapped; the path is replanned */
/* Moves the agent to p along the navmesh surface, keeping its path: for an
 * agent whose body something else moves (a character controller following
 * its velocity). Unlike a teleport, walls stop it. */
void cw_crowd_set_position(cw_crowd c, cw_crowd_agent a, t3_vec3 p);
bool cw_crowd_get_state(cw_crowd c, cw_crowd_agent a, cw_crowd_agent_state *out); /* false for an invalid agent */
t3_vec3 cw_crowd_position(cw_crowd c, cw_crowd_agent a); /* zero when invalid */
/* One fixed tick (dt in seconds; the same dt every tick for replays). */
void cw_crowd_step(cw_crowd c, float dt);
int cw_crowd_count(cw_crowd c); /* live agents */
/* Live agents in index order (at most cap); returns the live count. */
int cw_crowd_agents(cw_crowd c, cw_crowd_agent *out, int cap);
/* A hash of every live agent's position, velocity and status, in index order. */
uint64_t cw_crowd_hash(cw_crowd c);

typedef struct cw_crowd_stats {
  int agents, moving, waiting;
  int paths_planned;      /* during the last step */
  int neighbor_pairs;     /* neighbours found in the last step */
  float step_ms;          /* the last step, wall time (never feeds state) */
  float phase_ms[4];      /* paths, neighbours, steering, movement */
} cw_crowd_stats;
cw_crowd_stats cw_crowd_get_stats(cw_crowd c); /* all zero for an invalid crowd */

/* ── debug draw ────────────────────────────────────────────────────────
 * Appends line segments (physics.h's list; clear it between frames). */
enum {
  CW_NAV_DRAW_POLYS = 1 << 0,     /* polygon edges, raised slightly over the surface */
  CW_NAV_DRAW_BOUNDARY = 1 << 1,  /* walls (edges with no neighbour), bright */
  CW_NAV_DRAW_TILES = 1 << 2,     /* tile bounds */
  CW_NAV_DRAW_OBSTACLES = 1 << 3,
  CW_NAV_DRAW_AGENTS = 1 << 4,    /* crowd: circles, velocities */
  CW_NAV_DRAW_PATHS = 1 << 5,     /* crowd: each agent's corners to its target */
  CW_NAV_DRAW_DEFAULT = CW_NAV_DRAW_POLYS | CW_NAV_DRAW_BOUNDARY | CW_NAV_DRAW_OBSTACLES | CW_NAV_DRAW_AGENTS |
                        CW_NAV_DRAW_PATHS,
};
void cw_nav_debug_draw(cw_navmesh nm, cw_phys_lines *out, uint32_t flags); /* polygons, walls, tiles, obstacles */
void cw_crowd_debug_draw(cw_crowd c, cw_phys_lines *out, uint32_t flags); /* agents and paths */
/* A waypoint list as a polyline. */
void cw_nav_debug_draw_path(cw_phys_lines *out, const t3_vec3 *pts, int count, uint32_t color);

/* ── scene nodes ───────────────────────────────────────────────────────
 * NavigationRegion3D (Node3D): owns a navmesh and its crowd. It bakes from
 * its subtree (MeshInstance3D meshes, StaticBody3D shapes when the tree has
 * a physics world) on ready or on cw_nav_region_bake, or loads one
 * (`navmesh_path`). `debug_draw` shows the navmesh and its agents.
 * NavigationAgent3D (Node): a crowd agent that moves its parent Node3D.
 * `target_position` sets where it goes; signals path_changed, target_reached
 * and velocity_computed (vec3). It joins the region named by `region` (a
 * node path), else the first region in the tree.
 * NavigationObstacle3D (Node3D): a cylinder or box carved out of a dynamic
 * region's navmesh while it is in the tree; moving it moves the hole. */
cw_navmesh cw_nav_region_navmesh(cw_node region); /* null until baked or loaded */
cw_crowd cw_nav_region_crowd(cw_node region); /* null until baked or loaded */
/* Bakes now (replacing the navmesh; its agents are re-added). */
bool cw_nav_region_bake(cw_node region);
cw_crowd_agent cw_nav_agent_handle(cw_node agent); /* null while not in a crowd */

#ifdef __cplusplus
}
#endif
#endif
