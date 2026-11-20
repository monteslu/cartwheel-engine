/* 2D navigation nodes (nav2d.h): NavigationRegion2D, NavigationAgent2D and NavigationObstacle2D, and the bake
 * of a grid from a TileMap2D.
 *
 * One fixed-tick hook per tree (order -50, as the 3D nodes) does, in order:
 *   1. regions bake, or bake in the map cells edited since the last tick,
 *   2. obstacles that entered, moved or left change their region's grid,
 *   3. flow fields whose grid changed are built again,
 *   4. agents join their region's crowd, take new targets, and (when they do not drive their parent) follow
 *      their parent's position,
 *   5. every region's crowd steps,
 *   6. agents move their parent and emit their signals.
 * Regions, agents and obstacles are kept in the order they entered the tree. Positions are the pixels of the
 * region's TileMap2D (its local space). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../scene/scene2d_internal.h"
#include "cartwheel/nav2d.h"
#include "nav_internal.h"

/* ── a grid from a tile map ───────────────────────────────────────────── */

static bool map_blocked(cw_node map, uint32_t layers, int nl, int x, int y) {
  for (int l = 0; l < nl; l++)
    if ((!layers || (layers >> l) & 1) && cw_tilemap2d_cell_solid(map, l, x, y)) return true;
  return false;
}

static void bake_cells(cw_navgrid g, cw_node map, const cw_navgrid_bake *b, int x0, int y0, int x1, int y1) {
  cw__tilemap_info in;
  if (!cw__tilemap2d_info(map, &in)) return;
  int gw, gh;
  cw_navgrid_size(g, &gw, &gh);
  x0 = CW_MAX(x0, 0), y0 = CW_MAX(y0, 0), x1 = CW_MIN(x1, gw - 1), y1 = CW_MIN(y1, gh - 1);
  float cw = in.cell_size.x, ch = in.cell_size.y, r = b->agent_radius;
  int rx = r > 0 ? (int)(r / cw) + 1 : 0, ry = r > 0 ? (int)(r / ch) + 1 : 0;
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) {
      bool blocked = map_blocked(map, b->layers, in.layers, x, y);
      /* a wide agent: is a blocked cell's rectangle nearer than its radius to this cell's middle */
      for (int dy = -ry; dy <= ry && !blocked; dy++)
        for (int dx = -rx; dx <= rx && !blocked; dx++) {
          if ((!dx && !dy) || !map_blocked(map, b->layers, in.layers, x + dx, y + dy)) continue;
          float gx = ((float)abs(dx) - 0.5f) * cw, gy = ((float)abs(dy) - 0.5f) * ch;
          gx = CW_MAX(gx, 0.0f), gy = CW_MAX(gy, 0.0f);
          blocked = gx * gx + gy * gy < r * r;
        }
      uint8_t cost = 1;
      if (!blocked && b->cost_data) {
        float best = 1;
        for (int l = 0; l < in.layers; l++) best = CW_MAX(best, cw_tilemap2d_cell_data(map, l, x, y, b->cost_data, 1.0f));
        cost = (uint8_t)CW_CLAMP((int)(best + 0.5f), 1, 255);
      }
      cw_navgrid_set_cost(g, x, y, blocked ? 0 : cost);
    }
}

cw_navgrid cw_navgrid_from_tilemap(cw_node map, const cw_navgrid_bake *bake) {
  static const cw_navgrid_bake defaults = { 0 };
  if (!bake) bake = &defaults;
  cw__tilemap_info in;
  if (!cw__tilemap2d_info(map, &in) || !in.w || !in.h) return (cw_navgrid){ 0 };
  cw_navgrid g = cw_navgrid_new(&(cw_navgrid_desc){ .width = in.w, .height = in.h, .cell = in.cell_size, .no_diagonals = bake->no_diagonals,
                                                .layout = (cw_tile_layout)in.layout });
  if (cw_navgrid_valid(g)) bake_cells(g, map, bake, 0, 0, in.w - 1, in.h - 1);
  return g;
}

void cw_navgrid_rebake(cw_navgrid g, cw_node map, const cw_navgrid_bake *bake, int x0, int y0, int x1, int y1) {
  static const cw_navgrid_bake defaults = { 0 };
  if (cw_navgrid_valid(g)) bake_cells(g, map, bake ? bake : &defaults, x0, y0, x1, y1);
}

/* ── per-tree state ───────────────────────────────────────────────────── */

typedef struct nav2w {
  CW_ARRAY(cw_node) regions, agents, obstacles;
} nav2w;

static void nav2w_free(void *p) {
  nav2w *w = p;
  cw_array_free(&w->regions);
  cw_array_free(&w->agents);
  cw_array_free(&w->obstacles);
  cw_free(w);
}

static void hook(cw_tree *t, double dt, void *u);

static nav2w *nav2w_of(cw_tree *t) {
  if (!t) return NULL;
  nav2w *w = cw_tree_slot(t, "nav2d");
  if (w) return w;
  w = cw_calloc(1, sizeof *w, CW_MEM_NAV);
  cw_tree_set_slot(t, "nav2d", w, nav2w_free);
  cw_tree_add_hook(t, 0, -50, hook, NULL);
  return w;
}

static void list_remove(cw_node *data, int *count, cw_node n) {
  for (int i = 0; i < *count; i++)
    if (data[i].id == n.id) {
      memmove(data + i, data + i + 1, sizeof(cw_node) * (size_t)(*count - i - 1));
      (*count)--;
      return;
    }
}

static const cw_node_type *region_t, *agent_t, *obstacle_t;

/* ── NavigationRegion2D ───────────────────────────────────────────────── */

typedef struct region2d {
  const char *tilemap;
  int32_t layers;
  float agent_radius;
  const char *cost_data;
  bool diagonals, bake_on_ready;
  int max_agents, paths_per_tick;
  bool debug_draw;
  /* runtime */
  cw_node map;
  cw_navgrid grid;
  cw_crowd2d crowd;
  cw_flowfield flow;
  cw_vec2 flow_target;
  bool has_flow, needs_bake;
  uint32_t serial, epoch;
  float build_ms, step_ms;
  cw_phys_lines lines;
} region2d;

static void region_init(cw_node n, void *d) {
  (void)n;
  region2d *r = d;
  r->tilemap = cw_intern("..");
  r->diagonals = true;
  r->bake_on_ready = true;
  r->max_agents = 256;
  r->paths_per_tick = 8;
}

static void unjoin_agents(cw_tree *t, cw_node region);
static void reset_obstacles(cw_tree *t, cw_node region);

static void region_drop(cw_node n, region2d *r) {
  if (cw_navgrid_valid(r->grid)) {
    unjoin_agents(cw_node_tree(n), n);
    reset_obstacles(cw_node_tree(n), n);
    cw_navgrid_free(r->grid); /* its crowd and flow field go with it */
  }
  r->grid = (cw_navgrid){ 0 };
  r->crowd = (cw_crowd2d){ 0 };
  r->flow = (cw_flowfield){ 0 };
}

static cw_navgrid_bake bake_of(const region2d *r) {
  return (cw_navgrid_bake){ .layers = (uint32_t)r->layers, .agent_radius = r->agent_radius,
                            .cost_data = r->cost_data && r->cost_data[0] ? r->cost_data : NULL, .no_diagonals = !r->diagonals };
}

static bool region_bake_now(cw_node n, region2d *r) {
  r->needs_bake = false;
  double t0 = cw__nav_now_ms();
  region_drop(n, r);
  r->map = cw_node_get(n, r->tilemap ? r->tilemap : "");
  cw__tilemap_info in;
  if (!cw__tilemap2d_info(r->map, &in)) {
    cw_error("NavigationRegion2D '%s': tilemap '%s' is not a TileMap2D (a path from this node; make it the map's child and "
             "leave it \"..\")", cw_node_name(n), r->tilemap ? r->tilemap : "");
    r->map = (cw_node){ 0 };
    return false;
  }
  cw_navgrid_bake b = bake_of(r);
  r->grid = cw_navgrid_from_tilemap(r->map, &b);
  if (!cw_navgrid_valid(r->grid)) return false;
  r->crowd = cw_crowd2d_new(r->grid, r->max_agents);
  cw_crowd2d_set_plan_budget(r->crowd, r->paths_per_tick, 0);
  if (r->has_flow) {
    r->flow = cw_flowfield_new(r->grid);
    cw_flowfield_build(r->flow, &r->flow_target, 1);
  }
  r->serial = in.serial, r->epoch = in.epoch;
  r->build_ms = (float)(cw__nav_now_ms() - t0);
  cw_emit(n, "baked", NULL, 0);
  return true;
}

/* the map cells edited since the last tick, and those their agent_radius reaches */
static void region_follow_map(cw_node n, region2d *r) {
  cw__tilemap_info in;
  if (!cw_navgrid_valid(r->grid) || !cw__tilemap2d_info(r->map, &in)) return;
  int gw, gh;
  cw_navgrid_size(r->grid, &gw, &gh);
  if (in.epoch != r->epoch || in.w != gw || in.h != gh) {
    region_bake_now(n, r);
    return;
  }
  if (in.serial == r->serial) return;
  cw__tile_edit ed[256];
  int ne = cw__tilemap2d_edits_since(r->map, r->serial, ed, 256);
  r->serial = in.serial;
  if (ne < 0) {
    region_bake_now(n, r);
    return;
  }
  double t0 = cw__nav_now_ms();
  cw_navgrid_bake b = bake_of(r);
  int rx = r->agent_radius > 0 ? (int)(r->agent_radius / in.cell_size.x) + 1 : 0;
  int ry = r->agent_radius > 0 ? (int)(r->agent_radius / in.cell_size.y) + 1 : 0;
  for (int i = 0; i < ne; i++) cw_navgrid_rebake(r->grid, r->map, &b, ed[i].x - rx, ed[i].y - ry, ed[i].x + rx, ed[i].y + ry);
  r->build_ms = (float)(cw__nav_now_ms() - t0);
}

static void region_enter(cw_node n, void *d) {
  region2d *r = d;
  nav2w *w = nav2w_of(cw_node_tree(n));
  if (w) cw_array_push(&w->regions, n);
  if (r->bake_on_ready && !cw_navgrid_valid(r->grid)) r->needs_bake = true;
}

static void region_exit(cw_node n, void *d) {
  region_drop(n, d);
  nav2w *w = cw_tree_slot(cw_node_tree(n), "nav2d");
  if (w) list_remove(w->regions.data, &w->regions.count, n);
}

static void region_destroy(cw_node n, void *d) {
  (void)n;
  region2d *r = d;
  if (cw_navgrid_valid(r->grid)) cw_navgrid_free(r->grid);
  cw_phys_lines_free(&r->lines);
}

static void region_prop_changed(cw_node n, void *d, const cw_prop *p) {
  region2d *r = d;
  if (!strcmp(p->name, "debug_draw")) return;
  if (!strcmp(p->name, "paths_per_tick")) {
    if (cw_crowd2d_valid(r->crowd)) cw_crowd2d_set_plan_budget(r->crowd, r->paths_per_tick, 0);
    return;
  }
  if (cw_navgrid_valid(r->grid) && cw_node_tree(n)) r->needs_bake = true; /* what the grid is made from changed */
}

cw_navgrid cw_nav_region2d_grid(cw_node n) {
  region2d *r = cw_node_data_t(n, region_t);
  return r ? r->grid : (cw_navgrid){ 0 };
}
cw_crowd2d cw_nav_region2d_crowd(cw_node n) {
  region2d *r = cw_node_data_t(n, region_t);
  return r ? r->crowd : (cw_crowd2d){ 0 };
}
cw_flowfield cw_nav_region2d_flow(cw_node n) {
  region2d *r = cw_node_data_t(n, region_t);
  return r ? r->flow : (cw_flowfield){ 0 };
}
bool cw_nav_region2d_bake(cw_node n) {
  region2d *r = cw_node_data_t(n, region_t);
  return r ? region_bake_now(n, r) : false;
}
void cw_nav_region2d_set_flow_target(cw_node n, cw_vec2 goal) {
  region2d *r = cw_node_data_t(n, region_t);
  if (!r) return;
  r->flow_target = goal;
  r->has_flow = true;
  if (!cw_navgrid_valid(r->grid)) return;
  if (!cw_flowfield_valid(r->flow)) r->flow = cw_flowfield_new(r->grid);
  cw_flowfield_build(r->flow, &goal, 1);
}

static bool m_bake(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)args, (void)argc, (void)user;
  bool ok = cw_nav_region2d_bake(self);
  if (ret) *ret = cw_val_bool(ok);
  return true;
}

static void cells_get(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_int(cw_navgrid_get_stats(((region2d *)d)->grid).cells); }
static void walkable_get(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_int(cw_navgrid_get_stats(((region2d *)d)->grid).walkable); }
static void agents_get(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_int(cw_crowd2d_count(((region2d *)d)->crowd)); }
static void build_get(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_float(((region2d *)d)->build_ms); }
static void step_get(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_float(((region2d *)d)->step_ms); }

/* debug_draw: the blocked cells, the agents and their paths, the flow field, in the map's pixels (the region
 * draws in its own space: keep it at the map's origin) */
static void region_draw(cw_node n, void *d, const cw_canvas_item *it) {
  (void)n, (void)it;
  region2d *r = d;
  if (!r->debug_draw || !cw_navgrid_valid(r->grid)) return;
  cw_phys_lines_clear(&r->lines);
  cw_navgrid_debug_draw(r->grid, &r->lines, CW_NAV2D_DRAW_BLOCKED);
  if (cw_flowfield_valid(r->flow)) cw_flowfield_debug_draw(r->flow, &r->lines);
  cw_crowd2d_debug_draw(r->crowd, &r->lines, CW_NAV2D_DRAW_AGENTS | CW_NAV2D_DRAW_PATHS);
  for (int i = 0; i < cw_phys_lines_count(&r->lines); i++) {
    const float *p = &r->lines.pos.data[i * 6];
    uint32_t c = r->lines.color.data[i];
    cw_r2d_line(cw_v2(p[0], p[1]), cw_v2(p[3], p[4]), 1,
                (cw_color){ (float)((c >> 16) & 255) / 255.0f, (float)((c >> 8) & 255) / 255.0f, (float)(c & 255) / 255.0f, 0.9f });
  }
}

static const char *const region_signals[] = { "baked", NULL };
static const cw_prop region_props[] = {
  CW_PROP_FIELD(region2d, tilemap, CW_T_STRING, CW_PROP_DEFAULT, "Path to the TileMap2D, from this node (\"..\": the parent)."),
  CW_PROP_FIELD(region2d, layers, CW_T_INT, CW_PROP_DEFAULT, "Bit k: map layer k's solid tiles block (0 = every layer)."),
  CW_PROP_FIELD_RANGE(region2d, agent_radius, CW_T_FLOAT, 0, 10000, "Cells whose middle is nearer than this to a blocked cell are blocked too, pixels."),
  CW_PROP_FIELD(region2d, cost_data, CW_T_STRING, CW_PROP_DEFAULT, "A tile_data name whose value is a cell's cost (1..255; empty: every free cell costs 1)."),
  CW_PROP_FIELD(region2d, diagonals, CW_T_BOOL, CW_PROP_DEFAULT, "Paths step diagonally where both cells beside the step are free."),
  CW_PROP_FIELD(region2d, bake_on_ready, CW_T_BOOL, CW_PROP_DEFAULT, "Bake on the first tick in the tree."),
  CW_PROP_FIELD_RANGE(region2d, max_agents, CW_T_INT, 1, 65536, "Crowd capacity (set before baking)."),
  CW_PROP_FIELD_RANGE(region2d, paths_per_tick, CW_T_INT, 1, 4096, "Agent paths planned per tick (oldest requests first)."),
  CW_PROP_FIELD(region2d, debug_draw, CW_T_BOOL, CW_PROP_DEFAULT, "Draw the blocked cells, agents, paths and flow field."),
  CW_PROP_ACCESSOR("cell_count", CW_T_INT, CW_PROP_READONLY | CW_PROP_EDITOR, cells_get, NULL, "Grid cells."),
  CW_PROP_ACCESSOR("walkable_count", CW_T_INT, CW_PROP_READONLY | CW_PROP_EDITOR, walkable_get, NULL, "Walkable grid cells."),
  CW_PROP_ACCESSOR("agent_count", CW_T_INT, CW_PROP_READONLY | CW_PROP_EDITOR, agents_get, NULL, "Agents in the crowd."),
  CW_PROP_ACCESSOR("build_ms", CW_T_FLOAT, CW_PROP_READONLY | CW_PROP_EDITOR, build_get, NULL, "The last bake or map update, ms."),
  CW_PROP_ACCESSOR("crowd_step_ms", CW_T_FLOAT, CW_PROP_READONLY | CW_PROP_EDITOR, step_get, NULL, "The last crowd step, ms."),
  CW_PROP_END,
};
static cw_node_type region_type = { .name = "NavigationRegion2D", .parent = "Node2D", .data_size = sizeof(region2d),
                                    .props = region_props, .signals = region_signals,
                                    .doc = "A navigation grid baked from a TileMap2D's solid tiles, and the crowd that walks it.",
                                    .init = region_init, .destroy = region_destroy, .enter_tree = region_enter,
                                    .exit_tree = region_exit, .prop_changed = region_prop_changed };

/* ── NavigationAgent2D ────────────────────────────────────────────────── */

static const char *const status_names[] = { "IDLE", "WAITING", "MOVING", "ARRIVED", "NO_PATH", "VELOCITY", NULL };

typedef struct agent2d {
  cw_vec2 target_position;
  float radius, max_speed, max_accel, neighbor_dist, time_horizon, arrive_dist, slowdown_dist, avoidance_priority;
  int max_neighbors;
  bool avoidance, drive_parent;
  const char *region;
  /* runtime */
  cw_node joined;
  cw_crowd2d_agent handle;
  bool has_target, target_pending, reached, velocity_pending, flow_pending, follows_flow;
  cw_vec2 velocity_request;
  int status;
  cw_vec2 velocity;
  uint32_t path_version;
  bool warned;
} agent2d;

static cw_crowd2d_agent_params params_of(const agent2d *a) {
  cw_crowd2d_agent_params p = cw_crowd2d_agent_params_default();
  p.radius = a->radius;
  p.max_speed = a->max_speed;
  p.max_accel = a->max_accel;
  p.neighbor_dist = a->neighbor_dist;
  p.max_neighbors = a->max_neighbors;
  p.time_horizon = a->time_horizon;
  p.arrive_dist = a->arrive_dist;
  p.slowdown_dist = a->slowdown_dist;
  p.avoidance = a->avoidance;
  p.avoidance_priority = a->avoidance_priority;
  return p;
}

static void agent_init(cw_node n, void *d) {
  (void)n;
  agent2d *a = d;
  cw_crowd2d_agent_params p = cw_crowd2d_agent_params_default();
  a->radius = p.radius;
  a->max_speed = p.max_speed;
  a->max_accel = p.max_accel;
  a->neighbor_dist = p.neighbor_dist;
  a->max_neighbors = p.max_neighbors;
  a->time_horizon = p.time_horizon;
  a->arrive_dist = p.arrive_dist;
  a->slowdown_dist = p.slowdown_dist;
  a->avoidance = p.avoidance;
  a->avoidance_priority = p.avoidance_priority;
  a->drive_parent = true;
}

static cw_crowd2d crowd_of(cw_node region) {
  region2d *r = cw_node_data_t(region, region_t);
  return r ? r->crowd : (cw_crowd2d){ 0 };
}

static void agent_leave(agent2d *a) {
  if (!CW_HANDLE_IS_NULL(a->joined)) cw_crowd2d_remove(crowd_of(a->joined), a->handle);
  a->joined = (cw_node){ 0 };
  a->handle = (cw_crowd2d_agent){ 0 };
}

static void agent_enter(cw_node n, void *d) {
  (void)d;
  nav2w *w = nav2w_of(cw_node_tree(n));
  if (w) cw_array_push(&w->agents, n);
}

static void agent_exit(cw_node n, void *d) {
  agent_leave(d);
  nav2w *w = cw_tree_slot(cw_node_tree(n), "nav2d");
  if (w) list_remove(w->agents.data, &w->agents.count, n);
}

static void unjoin_agents(cw_tree *t, cw_node region) {
  nav2w *w = t ? cw_tree_slot(t, "nav2d") : NULL;
  if (!w) return;
  for (int i = 0; i < w->agents.count; i++) {
    agent2d *a = cw_node_data_t(w->agents.data[i], agent_t);
    if (a && a->joined.id == region.id) {
      a->joined = (cw_node){ 0 };
      a->handle = (cw_crowd2d_agent){ 0 };
      if (a->follows_flow) a->flow_pending = true;
      else if (a->has_target) a->target_pending = true;
    }
  }
}

static void agent_prop_changed(cw_node n, void *d, const cw_prop *p) {
  (void)n;
  agent2d *a = d;
  if (!strcmp(p->name, "target_position")) {
    a->has_target = a->target_pending = true;
    a->reached = a->follows_flow = a->flow_pending = false;
    return;
  }
  if (!strcmp(p->name, "region")) {
    agent_leave(a);
    return;
  }
  if (!CW_HANDLE_IS_NULL(a->joined)) {
    cw_crowd2d_agent_params pp = params_of(a);
    cw_crowd2d_set_params(crowd_of(a->joined), a->handle, &pp);
  }
}

static cw_node region_for(cw_node from, const char *path, nav2w *w) {
  if (path && path[0]) {
    cw_node r = cw_node_get(from, path);
    return cw_node_get_type(r) == region_t ? r : (cw_node){ 0 };
  }
  return w->regions.count ? w->regions.data[0] : (cw_node){ 0 };
}

static cw_node parent2d(cw_node n) {
  cw_node p = cw_node_parent(n);
  return cw_node_is(p, "Node2D") ? p : (cw_node){ 0 };
}

/* a global position in the pixels of the region's map, and back */
static cw_vec2 to_map(cw_node region, cw_vec2 global) {
  region2d *r = cw_node_data_t(region, region_t);
  if (!r || !cw_node_valid(r->map)) return global;
  const float *m = cw_node2d_global_transform(r->map);
  float det = m[0] * m[3] - m[1] * m[2];
  if (det == 0) return global;
  float x = global.x - m[4], y = global.y - m[5];
  return cw_v2((m[3] * x - m[2] * y) / det, (m[0] * y - m[1] * x) / det);
}
static cw_vec2 from_map(cw_node region, cw_vec2 p) {
  region2d *r = cw_node_data_t(region, region_t);
  if (!r || !cw_node_valid(r->map)) return p;
  const float *m = cw_node2d_global_transform(r->map);
  return cw_v2(m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5]);
}

static bool m_set_velocity(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)ret, (void)user;
  agent2d *a = cw_node_data_t(self, agent_t);
  if (!a || argc < 1) return false;
  a->velocity_request = cw_v2(args[0].v[0], args[0].v[1]);
  a->velocity_pending = true;
  a->has_target = a->target_pending = a->follows_flow = a->flow_pending = false;
  return true;
}

static bool m_stop(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)args, (void)argc, (void)ret, (void)user;
  agent2d *a = cw_node_data_t(self, agent_t);
  if (!a) return false;
  a->has_target = a->target_pending = a->velocity_pending = a->follows_flow = a->flow_pending = false;
  if (!CW_HANDLE_IS_NULL(a->joined)) cw_crowd2d_clear_target(crowd_of(a->joined), a->handle);
  return true;
}

static bool m_follow_flow(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)args, (void)argc, (void)ret, (void)user;
  agent2d *a = cw_node_data_t(self, agent_t);
  if (!a) return false;
  a->follows_flow = a->flow_pending = true;
  a->has_target = a->target_pending = a->velocity_pending = a->reached = false;
  return true;
}

cw_crowd2d_agent cw_nav_agent2d_handle(cw_node n) {
  agent2d *a = cw_node_data_t(n, agent_t);
  return a ? a->handle : (cw_crowd2d_agent){ 0 };
}

static void status_get(cw_node n, void *d, cw_value *o) {
  (void)n;
  *o = (cw_value){ CW_T_ENUM };
  o->i = ((agent2d *)d)->status;
}
static void finished_get(cw_node n, void *d, cw_value *o) {
  (void)n;
  int s = ((agent2d *)d)->status;
  *o = cw_val_bool(s == CW_CROWD2D_ARRIVED || s == CW_CROWD2D_NO_PATH || s == CW_CROWD2D_IDLE);
}

static const char *const agent_signals[] = { "path_changed", "target_reached", "velocity_computed", NULL };
static const cw_prop agent_props[] = {
  CW_PROP_FIELD(agent2d, target_position, CW_T_VEC2, CW_PROP_DEFAULT, "Where to go (the region's map pixels); setting it plans a path."),
  CW_PROP_FIELD_RANGE(agent2d, radius, CW_T_FLOAT, 0.01, 10000, "Avoidance radius, pixels."),
  CW_PROP_FIELD_RANGE(agent2d, max_speed, CW_T_FLOAT, 0, 100000, "px/s."),
  CW_PROP_FIELD_RANGE(agent2d, max_accel, CW_T_FLOAT, 0, 1000000, "px/s² (0 = no limit)."),
  CW_PROP_FIELD_RANGE(agent2d, neighbor_dist, CW_T_FLOAT, 0, 100000, "Neighbours within this distance are avoided, pixels."),
  CW_PROP_FIELD_RANGE(agent2d, max_neighbors, CW_T_INT, 0, CW_CROWD2D_MAX_NEIGHBORS, "Neighbours avoided at most."),
  CW_PROP_FIELD_RANGE(agent2d, time_horizon, CW_T_FLOAT, 0.01, 100, "How far ahead other agents are avoided, s."),
  CW_PROP_FIELD_RANGE(agent2d, arrive_dist, CW_T_FLOAT, 0, 10000, "The target counts as reached within this distance, pixels."),
  CW_PROP_FIELD_RANGE(agent2d, slowdown_dist, CW_T_FLOAT, 0, 10000, "Slows down within this distance of the target, pixels."),
  CW_PROP_FIELD(agent2d, avoidance, CW_T_BOOL, CW_PROP_DEFAULT, "Avoid other agents (ORCA). Others avoid it either way."),
  CW_PROP_FIELD_RANGE(agent2d, avoidance_priority, CW_T_FLOAT, 0, 1, "Higher: others give way more."),
  CW_PROP_FIELD(agent2d, drive_parent, CW_T_BOOL, CW_PROP_DEFAULT,
                "Move the parent Node2D every tick. Off: the parent moves by velocity_computed and the agent follows it."),
  CW_PROP_FIELD(agent2d, region, CW_T_STRING, CW_PROP_DEFAULT, "Path to its NavigationRegion2D (empty = the tree's first)."),
  { .name = "status", .type = CW_T_ENUM, .flags = CW_PROP_READONLY | CW_PROP_EDITOR, .offset = -1, .get = status_get,
    .enum_names = status_names, .doc = "IDLE, WAITING (for a path), MOVING, ARRIVED, NO_PATH or VELOCITY." },
  { .name = "velocity", .type = CW_T_VEC2, .flags = CW_PROP_READONLY | CW_PROP_EDITOR, .offset = offsetof(agent2d, velocity),
    .size = sizeof(cw_vec2), .doc = "The velocity it moved with last tick, px/s." },
  CW_PROP_ACCESSOR("is_navigation_finished", CW_T_BOOL, CW_PROP_READONLY | CW_PROP_EDITOR, finished_get, NULL,
                   "Arrived, unreachable or idle."),
  CW_PROP_END,
};
static cw_node_type agent_type = { .name = "NavigationAgent2D", .parent = "Node", .data_size = sizeof(agent2d),
                                   .props = agent_props, .signals = agent_signals,
                                   .doc = "A crowd agent that walks its parent Node2D along grid paths or a flow field, avoiding other agents.",
                                   .init = agent_init, .enter_tree = agent_enter, .exit_tree = agent_exit,
                                   .prop_changed = agent_prop_changed };

/* ── NavigationObstacle2D ─────────────────────────────────────────────── */

static const char *const oshape_names[] = { "CIRCLE", "RECT", NULL };

typedef struct obstacle2d {
  int shape;
  float radius;
  cw_vec2 size;
  const char *region;
  /* runtime */
  cw_node in_region;
  uint32_t id;
  cw_vec2 at;
  float rot;
} obstacle2d;

static void obstacle_init(cw_node n, void *d) {
  (void)n;
  obstacle2d *o = d;
  o->radius = 8;
  o->size = cw_v2(16, 16);
}

static void obstacle_take_out(obstacle2d *o) {
  region2d *r = cw_node_data_t(o->in_region, region_t);
  if (r && o->id && cw_navgrid_valid(r->grid)) cw_navgrid_obstacle_remove(r->grid, o->id);
  o->id = 0;
  o->in_region = (cw_node){ 0 };
}

static void reset_obstacles(cw_tree *t, cw_node region) {
  nav2w *w = t ? cw_tree_slot(t, "nav2d") : NULL;
  if (!w) return;
  for (int i = 0; i < w->obstacles.count; i++) {
    obstacle2d *o = cw_node_data_t(w->obstacles.data[i], obstacle_t);
    if (o && o->in_region.id == region.id) {
      o->id = 0;
      o->in_region = (cw_node){ 0 };
    }
  }
}

static void obstacle_enter(cw_node n, void *d) {
  (void)d;
  nav2w *w = nav2w_of(cw_node_tree(n));
  if (w) cw_array_push(&w->obstacles, n);
}

static void obstacle_exit(cw_node n, void *d) {
  obstacle_take_out(d);
  nav2w *w = cw_tree_slot(cw_node_tree(n), "nav2d");
  if (w) list_remove(w->obstacles.data, &w->obstacles.count, n);
}

static void obstacle_prop_changed(cw_node n, void *d, const cw_prop *p) {
  (void)n, (void)p;
  obstacle_take_out(d); /* added again with the new shape at the next tick */
}

static const cw_prop obstacle_props[] = {
  { .name = "shape", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(obstacle2d, shape), .size = sizeof(int),
    .enum_names = oshape_names, .doc = "CIRCLE or RECT." },
  CW_PROP_FIELD_RANGE(obstacle2d, radius, CW_T_FLOAT, 0, 100000, "CIRCLE radius, pixels."),
  CW_PROP_FIELD(obstacle2d, size, CW_T_VEC2, CW_PROP_DEFAULT, "RECT full size, pixels."),
  CW_PROP_FIELD(obstacle2d, region, CW_T_STRING, CW_PROP_DEFAULT, "Path to its NavigationRegion2D (empty = the tree's first)."),
  CW_PROP_END,
};
static cw_node_type obstacle_type = { .name = "NavigationObstacle2D", .parent = "Node2D", .data_size = sizeof(obstacle2d),
                                      .props = obstacle_props,
                                      .doc = "A circle or rectangle that blocks its region's grid cells while it is in the tree.",
                                      .init = obstacle_init, .enter_tree = obstacle_enter, .exit_tree = obstacle_exit,
                                      .prop_changed = obstacle_prop_changed };

/* ── the tick ─────────────────────────────────────────────────────────── */

static void sync_obstacles(nav2w *w) {
  for (int i = 0; i < w->obstacles.count; i++) {
    cw_node n = w->obstacles.data[i];
    obstacle2d *o = cw_node_data_t(n, obstacle_t);
    cw_node rn = region_for(n, o->region, w);
    region2d *r = cw_node_data_t(rn, region_t);
    if (!r || !cw_navgrid_valid(r->grid)) continue;
    cw_vec2 at = to_map(rn, cw_node2d_global_position(n));
    float rot = cw_node2d_global_rotation(n) - cw_node2d_global_rotation(r->map);
    if (o->id && o->in_region.id == rn.id) {
      if (at.x == o->at.x && at.y == o->at.y && rot == o->rot) continue; /* still in place */
      obstacle_take_out(o);
    } else if (o->id) {
      obstacle_take_out(o);
    }
    o->id = o->shape == 0 ? cw_navgrid_obstacle_add_circle(r->grid, at, o->radius)
                          : cw_navgrid_obstacle_add_rect(r->grid, at, cw_v2(o->size.x * 0.5f, o->size.y * 0.5f), rot);
    o->in_region = rn;
    o->at = at;
    o->rot = rot;
  }
}

static void hook(cw_tree *t, double dt, void *u) {
  (void)u;
  nav2w *w = cw_tree_slot(t, "nav2d");
  if (!w || cw_tree_paused(t)) return;
  /* 1. bakes and map edits */
  for (int i = 0; i < w->regions.count; i++) {
    region2d *r = cw_node_data_t(w->regions.data[i], region_t);
    if (!r) continue;
    if (r->needs_bake) region_bake_now(w->regions.data[i], r);
    else region_follow_map(w->regions.data[i], r);
  }
  /* 2. obstacles */
  sync_obstacles(w);
  /* 3. flow fields */
  for (int i = 0; i < w->regions.count; i++) {
    region2d *r = cw_node_data_t(w->regions.data[i], region_t);
    if (r && r->has_flow && cw_navgrid_valid(r->grid)) {
      if (!cw_flowfield_valid(r->flow)) r->flow = cw_flowfield_new(r->grid);
      if (cw_flowfield_stale(r->flow)) cw_flowfield_build(r->flow, &r->flow_target, 1);
    }
  }
  /* 4. agents join, take targets, follow their parents */
  for (int i = 0; i < w->agents.count; i++) {
    cw_node n = w->agents.data[i];
    agent2d *a = cw_node_data_t(n, agent_t);
    cw_node body = parent2d(n);
    if (CW_HANDLE_IS_NULL(body)) continue;
    if (CW_HANDLE_IS_NULL(a->joined)) {
      cw_node rn = region_for(n, a->region, w);
      cw_crowd2d c = crowd_of(rn);
      if (!cw_crowd2d_valid(c)) continue;
      cw_crowd2d_agent_params p = params_of(a);
      a->handle = cw_crowd2d_add(c, to_map(rn, cw_node2d_global_position(body)), &p);
      if (CW_HANDLE_IS_NULL(a->handle)) {
        if (!a->warned) cw_warn("NavigationAgent2D %s: its parent is not near a walkable cell (or the crowd is full)", cw_node_name(n));
        a->warned = true;
        continue;
      }
      a->joined = rn;
      a->path_version = 0;
    }
    cw_crowd2d c = crowd_of(a->joined);
    if (!a->drive_parent) cw_crowd2d_set_position(c, a->handle, to_map(a->joined, cw_node2d_global_position(body)));
    if (a->target_pending) {
      cw_crowd2d_set_target(c, a->handle, a->target_position);
      a->target_pending = false;
    }
    if (a->flow_pending) {
      region2d *r = cw_node_data_t(a->joined, region_t);
      if (r && cw_flowfield_valid(r->flow)) {
        cw_crowd2d_set_flow(c, a->handle, r->flow);
        a->flow_pending = false;
      }
    }
    if (a->velocity_pending) {
      cw_crowd2d_set_velocity(c, a->handle, a->velocity_request);
      a->velocity_pending = false;
    }
  }
  /* 5. crowds step */
  for (int i = 0; i < w->regions.count; i++) {
    region2d *r = cw_node_data_t(w->regions.data[i], region_t);
    if (r && cw_crowd2d_valid(r->crowd)) {
      double t0 = cw__nav_now_ms();
      cw_crowd2d_step(r->crowd, (float)dt);
      r->step_ms = (float)(cw__nav_now_ms() - t0);
    }
  }
  /* 6. parents move; signals */
  for (int i = 0; i < w->agents.count; i++) {
    cw_node n = w->agents.data[i];
    agent2d *a = cw_node_data_t(n, agent_t);
    if (CW_HANDLE_IS_NULL(a->joined)) continue;
    cw_crowd2d_agent_state s;
    if (!cw_crowd2d_get_state(crowd_of(a->joined), a->handle, &s)) continue;
    a->status = (int)s.status;
    a->velocity = s.velocity;
    cw_node body = parent2d(n);
    if (a->drive_parent && !CW_HANDLE_IS_NULL(body)) {
      cw_node2d_snapshot(body);
      cw_node2d_set_global(body, from_map(a->joined, s.position), cw_node2d_global_rotation(body));
    }
    if (s.path_version != a->path_version) {
      a->path_version = s.path_version;
      cw_emit(n, "path_changed", NULL, 0);
    }
    if (s.status == CW_CROWD2D_ARRIVED && !a->reached) {
      a->reached = true;
      cw_emit(n, "target_reached", NULL, 0);
    }
    if (s.status == CW_CROWD2D_MOVING || s.status == CW_CROWD2D_VELOCITY) {
      cw_value v = cw_val_vec2(s.velocity.x, s.velocity.y);
      cw_emit(n, "velocity_computed", &v, 1);
    }
  }
}

void cw__register_nav2d_types(void) {
  cw_node_type_register(&region_type);
  region_t = &region_type;
  cw_node_type_register(&agent_type);
  agent_t = &agent_type;
  cw_node_type_register(&obstacle_type);
  obstacle_t = &obstacle_type;
  cw_canvas_register_type("NavigationRegion2D", region_draw);
  cw_node_type_add_method("NavigationRegion2D", "bake", NULL, m_bake, NULL, "Bake the grid from the tile map now.");
  cw_node_type_add_method("NavigationAgent2D", "set_velocity", "vec2", m_set_velocity, NULL,
                          "Move at this velocity (avoiding others) instead of following a path.");
  cw_node_type_add_method("NavigationAgent2D", "stop", NULL, m_stop, NULL, "Drop the target and stand.");
  cw_node_type_add_method("NavigationAgent2D", "follow_flow", NULL, m_follow_flow, NULL,
                          "Walk the region's flow field toward its flow target instead of a path of its own.");
}
