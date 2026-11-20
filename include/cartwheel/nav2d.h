/* cartwheel 2D navigation: grids of walkable cells, path queries, flow fields, obstacles and crowds.
 *
 *   grid:    cw_navgrid g = cw_navgrid_new(&(cw_navgrid_desc){ .width = 64, .height = 64, .cell = { 16, 16 } });
 *            cw_navgrid_set_cost(g, x, y, 0);                       (0 blocks a cell)
 *            or cw_navgrid_from_tilemap(map, &bake): the cells a TileMap2D's tile shapes leave free
 *   query:   n = cw_navgrid_find_path(g, from, to, NULL, pts, 64, &partial);
 *   field:   cw_flowfield f = cw_flowfield_new(g); cw_flowfield_build(f, &goal, 1);
 *            dir = cw_flowfield_direction(f, position);             (every agent heading to one goal reads it)
 *   crowd:   cw_crowd2d c = cw_crowd2d_new(g, 500);
 *            a = cw_crowd2d_add(c, p, NULL); cw_crowd2d_set_target(c, a, goal);
 *            cw_crowd2d_step(c, dt);  (every fixed tick)
 *
 * Positions are cw_vec2 in the grid's own units (pixels for a grid made from a tile map, y down like every 2D
 * node). The 3D side, navmeshes for meshes and colliders, is cartwheel/nav.h; the names here mirror it.
 *
 * Paths: A* over the cells, moving to the eight neighbours (a diagonal step needs both cells beside it free,
 * so nothing cuts a corner). On a grid where every walkable cell costs the same, the search is jump point
 * search (Harabor and Grastien, 2011; the no-corner-cutting rules as in PathFinding.js, MIT, see NOTICE): the
 * same shortest paths, visiting a fraction of the cells. Cells with their own cost (mud, a road) switch the
 * grid to plain A*. The cell path is then pulled straight where cells are in sight of one another (on a grid
 * with costs, only across cells of cost 1: a straight line through mud is no short cut).
 *
 * Flow fields: one search outward from the goals gives every cell its distance and the neighbour to step to.
 * Building costs one pass over the grid whatever the number of agents; reading is one lookup. Use it for a
 * crowd with one goal (a horde, a stream of creeps); use paths for agents with goals of their own.
 *
 * Determinism: costs are integers (10 a straight step, 14 a diagonal, times the cell's cost), ties are broken
 * by a fixed order (lower total, then nearer the goal, then lower cell index), and nothing reads a clock: the
 * same grid and request give the same path on every host and at any worker count. Crowd steps read the state
 * of the step's start and write only their own agent. Budgets count requests and iterations, never time.
 *
 * Threads: build, edit and step from one thread at a time. cw_navgrid_find_path, cw_navgrid_raycast,
 * cw_navgrid_nearest and cw_flowfield_direction are safe from job-pool threads while nothing changes the grid
 * (each pool thread has its own search memory); cw_navgrid_find_paths runs a batch on the pool.
 *
 * Memory: CW_MEM_NAV. A grid is 1 byte a cell, plus 13 bytes a cell for each pool thread that has searched it;
 * a flow field is 5 bytes a cell. */
#ifndef CW_NAV2D_H
#define CW_NAV2D_H

#include "core.h"
#include "math.h"
#include "physics.h"
#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

CW_HANDLE_TYPE(cw_navgrid);
CW_HANDLE_TYPE(cw_flowfield);
CW_HANDLE_TYPE(cw_crowd2d);
CW_HANDLE_TYPE(cw_crowd2d_agent);

/* ── grids ─────────────────────────────────────────────────────────────── */

typedef struct cw_navgrid_desc {
  int width, height;   /* cells; at most 4096 each */
  cw_vec2 cell;        /* a cell's size (zero: 1 x 1) */
  cw_vec2 origin;      /* the position of cell (0, 0)'s top-left corner */
  bool no_diagonals;   /* false: eight neighbours; true: four */
} cw_navgrid_desc;

/* A grid with every cell walkable at cost 1. Null (logged) for a size outside 1..4096. */
cw_navgrid cw_navgrid_new(const cw_navgrid_desc *desc);
void cw_navgrid_free(cw_navgrid g); /* frees its flow fields and crowds too; null or stale: no-op */
bool cw_navgrid_valid(cw_navgrid g); /* false for a null or freed grid */
void cw_navgrid_size(cw_navgrid g, int *width, int *height); /* 0 x 0 when invalid */
cw_vec2 cw_navgrid_cell_size(cw_navgrid g); /* zero when invalid */
/* A cell's cost: 0 = blocked, 1 = plain ground, up to 255 = that many times as long to cross. Out of range
 * reads as blocked and writes are ignored. Cost: O(1); a change raises the grid's version, and crowds plan
 * their paths again. */
void cw_navgrid_set_cost(cw_navgrid g, int x, int y, uint8_t cost);
uint8_t cw_navgrid_cost(cw_navgrid g, int x, int y); /* the cost set for the cell (obstacles aside); 0 outside */
/* Every cell at once: width * height costs, row by row. */
void cw_navgrid_set_costs(cw_navgrid g, const uint8_t *costs);
/* Is the cell walkable now (its cost is not 0 and no obstacle covers it). */
bool cw_navgrid_walkable(cw_navgrid g, int x, int y);
/* The cell a position is in (floor); it may lie outside the grid. */
void cw_navgrid_cell_at(cw_navgrid g, cw_vec2 p, int *x, int *y);
cw_vec2 cw_navgrid_cell_center(cw_navgrid g, int x, int y); /* the middle of a cell */
/* Raised by every change to costs or obstacles. */
uint32_t cw_navgrid_version(cw_navgrid g);
/* A hash of the size, costs and obstacle cover: the same grid on every host has the same hash. */
uint64_t cw_navgrid_hash(cw_navgrid g);

/* ── obstacles ─────────────────────────────────────────────────────────
 * Shapes that block the cells whose middle they cover, until removed: a crate dropped in a corridor, a door.
 * Ids are never 0 (0: the grid is invalid). Covers add up, so two obstacles over one cell must both go before
 * it opens. Cost: the cells of the shape's bounds. */
uint32_t cw_navgrid_obstacle_add_circle(cw_navgrid g, cw_vec2 center, float radius);
uint32_t cw_navgrid_obstacle_add_rect(cw_navgrid g, cw_vec2 center, cw_vec2 half, float rotation); /* half sizes; radians */
bool cw_navgrid_obstacle_remove(cw_navgrid g, uint32_t id); /* false for an unknown id */

/* ── a grid from a tile map ────────────────────────────────────────────
 * The grid has the map's cells: a cell is blocked when a tile on one of `layers` (bit k: map layer k; 0 = every
 * layer) has a solid tile shape there (TileMap2D tile_shapes: any solid shape blocks the whole cell; one-way
 * platforms do not block). `agent_radius` (the map's pixels) also blocks every cell whose middle is nearer than
 * that to a blocked cell, so an agent wider than a cell keeps off the walls (a radius of half a cell or less
 * blocks nothing more). `cost_data` names a tile_data
 * value that gives a cell its cost (1..255, a tile without one costs 1; NULL: every free cell costs 1).
 * Positions are the map node's local pixels. */
typedef struct cw_navgrid_bake {
  uint32_t layers;
  float agent_radius;
  const char *cost_data;
  bool no_diagonals;
} cw_navgrid_bake;
cw_navgrid cw_navgrid_from_tilemap(cw_node tilemap, const cw_navgrid_bake *bake); /* bake NULL = defaults; null when not a TileMap2D */
/* Bakes the cells x0..x1, y0..y1 (inclusive) again into a grid made by cw_navgrid_from_tilemap with the same
 * bake (after cell edits: the cells edited and those within agent_radius of them). */
void cw_navgrid_rebake(cw_navgrid g, cw_node tilemap, const cw_navgrid_bake *bake, int x0, int y0, int x1, int y1);

/* ── queries ───────────────────────────────────────────────────────────── */

typedef struct cw_navgrid_query {
  int max_iters;        /* cells (or jump points) expanded at most; 0 = no limit. A search that runs out returns
                         * the path to the cell nearest the goal it reached: partial */
  float agent_radius;   /* keeps a pulled-straight path this far from blocked cells' corners (0: a point) */
  bool keep_cells;      /* true: the path's corners stay on cell middles (no straightening) */
  float max_snap;       /* an end inside a blocked cell moves to the nearest walkable cell within this distance
                         * (0 = 4 cells) */
} cw_navgrid_query;

/* The shortest path from `from` to `to`: up to max waypoints in pts, the first `from`, the last `to` (each
 * moved to the nearest walkable cell when inside a blocked one). Returns the count: 0 when an end has no
 * walkable cell near it. *partial (may be NULL): the path ends short of `to` (unreachable, or out of
 * iterations). When the path has more corners than max, the last waypoint is the max-th corner and *partial
 * is set. q NULL = defaults. */
int cw_navgrid_find_path(cw_navgrid g, cw_vec2 from, cw_vec2 to, const cw_navgrid_query *q, cw_vec2 *pts, int max, bool *partial);
/* The length of a waypoint list. */
float cw_navgrid_path_length(const cw_vec2 *pts, int count);
/* Many paths at once, in parallel on the job pool: request i writes up to max_points waypoints at
 * pts[i * max_points] and its count at counts[i] (the same answer as cw_navgrid_find_path). */
typedef struct cw_navgrid_path_req {
  cw_vec2 from, to;
} cw_navgrid_path_req;
void cw_navgrid_find_paths(cw_navgrid g, const cw_navgrid_path_req *reqs, int count, const cw_navgrid_query *q, cw_vec2 *pts,
                           int max_points, int *counts); /* see cw_navgrid_path_req */
/* Walks from `from` toward `to` over walkable cells, `radius` wide: false and *hit (may be NULL) = the last
 * point before a blocked cell when something is in the way, true when `to` is in sight. */
bool cw_navgrid_raycast(cw_navgrid g, cw_vec2 from, cw_vec2 to, float radius, cw_vec2 *hit);
/* The middle of the walkable cell nearest p within max_distance (p itself when its cell is walkable). */
bool cw_navgrid_nearest(cw_navgrid g, cw_vec2 p, float max_distance, cw_vec2 *out);

typedef struct cw_navgrid_stats {
  int32_t cells, walkable;
  bool jump_points;      /* searches use jump point search (every walkable cell costs 1 and diagonals are on) */
  int32_t last_expanded; /* cells or jump points the calling thread's last search expanded */
} cw_navgrid_stats;
cw_navgrid_stats cw_navgrid_get_stats(cw_navgrid g); /* all zero for an invalid grid */

/* ── flow fields ───────────────────────────────────────────────────────
 * A field belongs to its grid and is freed with it. cw_flowfield_build searches outward from the goals over
 * the grid as it is now (one pass, a few ms for 256 x 256; the directions are filled on the job pool) and
 * remembers the grid's version: cw_flowfield_stale says when the grid has changed since. */
cw_flowfield cw_flowfield_new(cw_navgrid g);
void cw_flowfield_free(cw_flowfield f); /* null or stale: no-op */
bool cw_flowfield_valid(cw_flowfield f); /* false for a null or freed field (freeing its grid frees it) */
/* Goals inside blocked cells are skipped. */
void cw_flowfield_build(cw_flowfield f, const cw_vec2 *goals, int count);
bool cw_flowfield_stale(cw_flowfield f); /* the grid changed since the build (or it was never built) */
/* The way to walk from p: a unit vector toward the neighbour cell nearest a goal; zero in a goal's cell, in a
 * blocked cell and where no goal can be reached. */
cw_vec2 cw_flowfield_direction(cw_flowfield f, cw_vec2 p);
/* The walking distance from a cell to the nearest goal in the grid's units along cell steps; negative when
 * unreachable. */
float cw_flowfield_distance(cw_flowfield f, cw_vec2 p);
uint64_t cw_flowfield_hash(cw_flowfield f); /* of every cell's distance and direction */

/* ── crowds ────────────────────────────────────────────────────────────
 * Agents on a grid that follow paths (or a flow field) and avoid each other with the 3D crowd's ORCA solver.
 * cw_crowd2d_step advances every agent one fixed tick: the oldest agents waiting for a path get one (the plan
 * budget, searched in parallel), each agent finds its neighbours, steers along its path or field and avoids,
 * then moves, sliding along blocked cells. Agent work runs on the job pool in index order per agent, so a
 * step's result is the same at any worker count. */
typedef struct cw_crowd2d_agent_params {
  float radius;         /* 6 */
  float max_speed;      /* 60 (units/s) */
  float max_accel;      /* 480 (units/s²; 0 = no limit) */
  float neighbor_dist;  /* 48: neighbours within this distance are avoided */
  int max_neighbors;    /* 8 (at most CW_CROWD2D_MAX_NEIGHBORS) */
  float time_horizon;   /* 1 s: how far ahead other agents are avoided */
  float arrive_dist;    /* 3: the target counts as reached within this distance */
  float slowdown_dist;  /* 12: speed falls off within this distance of the target */
  bool avoidance;       /* true: ORCA against other agents (others avoid this one either way) */
  float avoidance_priority; /* 0.5: as cw_crowd_agent_params (nav.h) */
} cw_crowd2d_agent_params;
#define CW_CROWD2D_MAX_NEIGHBORS 16
cw_crowd2d_agent_params cw_crowd2d_agent_params_default(void); /* the defaults noted above */

typedef enum {
  CW_CROWD2D_IDLE,     /* no target */
  CW_CROWD2D_WAITING,  /* target set, its path not planned yet */
  CW_CROWD2D_MOVING,
  CW_CROWD2D_ARRIVED,  /* within arrive_dist of the target (or in a flow field's goal cell) */
  CW_CROWD2D_NO_PATH,  /* target unreachable (the agent stays put) */
  CW_CROWD2D_VELOCITY, /* moving by cw_crowd2d_set_velocity */
} cw_crowd2d_agent_status;

typedef struct cw_crowd2d_agent_state {
  cw_vec2 position, velocity;
  cw_vec2 desired_velocity; /* before avoidance */
  cw_vec2 target, next_corner;
  cw_crowd2d_agent_status status;
  bool partial;             /* the path ends short of the target */
  int path_points;          /* waypoints left */
  uint32_t path_version;    /* bumped whenever a new path is taken */
} cw_crowd2d_agent_state;

cw_crowd2d cw_crowd2d_new(cw_navgrid g, int max_agents); /* null when g is invalid */
void cw_crowd2d_free(cw_crowd2d c); /* null or stale: no-op */
bool cw_crowd2d_valid(cw_crowd2d c); /* false for a null or freed crowd (freeing its grid frees it) */
/* Paths planned per step (oldest waiting agents first; 8) and search iterations each (0 = no limit). */
void cw_crowd2d_set_plan_budget(cw_crowd2d c, int paths_per_step, int max_iters);
/* Adds an agent at p (moved to the nearest walkable cell when inside a blocked one); params NULL = defaults.
 * Null when the crowd is full or nothing walkable is near p. */
cw_crowd2d_agent cw_crowd2d_add(cw_crowd2d c, cw_vec2 p, const cw_crowd2d_agent_params *params);
void cw_crowd2d_remove(cw_crowd2d c, cw_crowd2d_agent a); /* stale handle: no-op; the slot is reused lowest first */
bool cw_crowd2d_agent_valid(cw_crowd2d c, cw_crowd2d_agent a); /* false once removed */
int cw_crowd2d_agent_slot(cw_crowd2d c, cw_crowd2d_agent a); /* 0..max_agents-1, stable while it lives; -1 when invalid */
void cw_crowd2d_set_params(cw_crowd2d c, cw_crowd2d_agent a, const cw_crowd2d_agent_params *params); /* NULL = defaults */
void cw_crowd2d_set_target(cw_crowd2d c, cw_crowd2d_agent a, cw_vec2 target); /* a path is planned at a coming step */
/* Follows a flow field instead of a path of its own (no search at all); the field must belong to the crowd's
 * grid. Arrives in a goal's cell. */
void cw_crowd2d_set_flow(cw_crowd2d c, cw_crowd2d_agent a, cw_flowfield f);
void cw_crowd2d_clear_target(cw_crowd2d c, cw_crowd2d_agent a); /* stops (IDLE) */
void cw_crowd2d_set_velocity(cw_crowd2d c, cw_crowd2d_agent a, cw_vec2 v); /* moves at v, avoiding others, until a target is set */
void cw_crowd2d_teleport(cw_crowd2d c, cw_crowd2d_agent a, cw_vec2 p); /* its path is planned again */
/* Puts the agent at p, keeping its path: for an agent whose body something else moves (a CharacterBody2D
 * following its velocity). */
void cw_crowd2d_set_position(cw_crowd2d c, cw_crowd2d_agent a, cw_vec2 p);
bool cw_crowd2d_get_state(cw_crowd2d c, cw_crowd2d_agent a, cw_crowd2d_agent_state *out); /* false for an invalid agent */
cw_vec2 cw_crowd2d_position(cw_crowd2d c, cw_crowd2d_agent a); /* zero when invalid */
void cw_crowd2d_step(cw_crowd2d c, float dt); /* one fixed tick (the same dt every tick for replays) */
int cw_crowd2d_count(cw_crowd2d c); /* live agents */
uint64_t cw_crowd2d_hash(cw_crowd2d c); /* of every live agent's position, velocity and status, in index order */

/* ── debug draw ────────────────────────────────────────────────────────
 * Appends line segments (physics.h's list; x and y in the grid's units, z 0). */
enum {
  CW_NAV2D_DRAW_BLOCKED = 1 << 0, /* a cross in every blocked cell */
  CW_NAV2D_DRAW_AGENTS = 1 << 1,  /* crowd: an octagon and the velocity */
  CW_NAV2D_DRAW_PATHS = 1 << 2,   /* crowd: each agent's waypoints */
  CW_NAV2D_DRAW_FLOW = 1 << 3,    /* flow field: a tick along each cell's direction */
};
void cw_navgrid_debug_draw(cw_navgrid g, cw_phys_lines *out, uint32_t flags); /* blocked cells (red; orange under an obstacle) */
void cw_crowd2d_debug_draw(cw_crowd2d c, cw_phys_lines *out, uint32_t flags); /* agents and paths */
void cw_flowfield_debug_draw(cw_flowfield f, cw_phys_lines *out); /* a tick along each cell's direction */

/* ── scene nodes ───────────────────────────────────────────────────────
 * The 2D mirrors of nav.h's nodes, with the same property and signal names where the thing exists in 2D.
 * NavigationRegion2D (Node2D): owns a grid and its crowd, baked from a TileMap2D (`tilemap`, a path from the
 *   node, default "..": make it the map's child) on its first tick or on the `bake` method: `layers` (bit k:
 *   map layer k blocks; 0 = every layer), `agent_radius`, `cost_data`, `diagonals`, `bake_on_ready`,
 *   `max_agents`, `paths_per_tick`, `debug_draw`; read-only `cell_count`, `walkable_count`, `agent_count`,
 *   `build_ms`, `crowd_step_ms`. Signal `baked`. Cell edits of the map (cw_tilemap2d_set_cell, terrains) are
 *   baked in at the next tick: only the cells edited and those within agent_radius of them.
 * NavigationAgent2D (Node): a crowd agent that moves its parent Node2D. `target_position` (the region's map
 *   pixels) sets where it goes; `radius`, `max_speed`, `max_accel`, `neighbor_dist`, `max_neighbors`,
 *   `time_horizon`, `arrive_dist`, `slowdown_dist`, `avoidance`, `avoidance_priority`, `drive_parent` (off:
 *   the parent moves by velocity_computed, a CharacterBody2D's move_and_slide say, and the agent follows it),
 *   `region` (a path; empty = the tree's first NavigationRegion2D); read-only `status`, `velocity`,
 *   `is_navigation_finished`. Signals path_changed, target_reached, velocity_computed (vec2). Methods
 *   set_velocity (vec2), stop, and follow_flow (the region's flow field toward the `flow_target` it was last
 *   built for: see cw_nav_region2d_set_flow_target).
 * NavigationObstacle2D (Node2D): a CIRCLE (`radius`) or RECT (`size`) that blocks the region's cells under
 *   it while it is in the tree; moving it moves the hole. `region` as for agents.
 * One fixed-tick hook per tree (order -50, as in 3D) bakes, applies map edits and obstacles, takes targets,
 * steps every region's crowd and moves the agents' parents, in the order the nodes entered the tree. */
cw_navgrid cw_nav_region2d_grid(cw_node region);   /* null until baked */
cw_crowd2d cw_nav_region2d_crowd(cw_node region); /* null until baked */
bool cw_nav_region2d_bake(cw_node region);         /* bakes now (agents are re-added); false when it has no map */
/* The region's flow field toward a goal (the region's map pixels), built now and again whenever the grid
 * changes; agents told to follow_flow walk it. */
void cw_nav_region2d_set_flow_target(cw_node region, cw_vec2 goal);
cw_flowfield cw_nav_region2d_flow(cw_node region); /* null until a flow target is set */
cw_crowd2d_agent cw_nav_agent2d_handle(cw_node agent); /* null while not in a crowd */

#ifdef __cplusplus
}
#endif
#endif
