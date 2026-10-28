/* Navigation nodes (nav.h): NavigationRegion3D, NavigationAgent3D and
 * NavigationObstacle3D.
 *
 * One fixed-tick hook per tree (order -50: after the physics world has
 * built this tick's bodies, before nodes' fixed_update) does, in order:
 *   1. regions that need it bake (or load) their navmesh,
 *   2. obstacles that entered, moved or left change their region's navmesh,
 *      and each changed region applies them (cw_nav_update),
 *   3. agents join their region's crowd, take new targets, and (when they
 *      do not drive their parent) follow their parent's position,
 *   4. every region's crowd steps,
 *   5. agents move their parent and emit their signals.
 * Regions, agents and obstacles are kept in the order they entered the
 * tree, so a run is the same on every host. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/physics_nodes.h"
#include "cartwheel/vfs.h"
#include "nav_internal.h"

/* ── per-tree state ───────────────────────────────────────────────────── */

typedef struct navw {
  CW_ARRAY(cw_node) regions, agents, obstacles;
} navw;

static void navw_free(void *p) {
  navw *w = p;
  cw_array_free(&w->regions);
  cw_array_free(&w->agents);
  cw_array_free(&w->obstacles);
  cw_free(w);
}

static void hook(cw_tree *t, double dt, void *u);

static navw *navw_of(cw_tree *t) {
  if (!t) return NULL;
  navw *w = cw_tree_slot(t, "nav");
  if (w) return w;
  w = cw_calloc(1, sizeof *w, CW_MEM_NAV);
  cw_tree_set_slot(t, "nav", w, navw_free);
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

static const cw_node_type *region_t, *agent_t, *obstacle_t, *link_t;

/* ── NavigationRegion3D ───────────────────────────────────────────────── */

typedef enum { SRC_BOTH, SRC_MESHES, SRC_COLLIDERS } bake_source;
static const char *const source_names[] = { "BOTH", "MESHES", "COLLIDERS", NULL };

typedef struct region3d {
  float cell_size, cell_height, agent_radius, agent_height, agent_max_climb, agent_max_slope;
  int tile_size;
  bool dynamic;
  int source;
  int64_t collision_mask;
  const char *navmesh_path;
  bool bake_on_ready;
  int max_agents, paths_per_tick;
  bool debug_draw;
  /* runtime */
  cw_navmesh nm;
  cw_crowd crowd;
  bool needs_bake, obstacles_dirty;
  int32_t polys, tiles;
  float build_ms;
  t3_mesh *lines_obj;
  t3_geometry *lines_geo;
  int lines_cap;
  cw_phys_lines lines;
} region3d;

static void region_init(cw_node n, void *d) {
  (void)n;
  region3d *r = d;
  cw_nav_config c = cw_nav_config_default();
  r->cell_size = c.cell_size;
  r->cell_height = c.cell_height;
  r->agent_radius = c.agent_radius;
  r->agent_height = c.agent_height;
  r->agent_max_climb = c.agent_max_climb;
  r->agent_max_slope = c.agent_max_slope;
  r->tile_size = c.tile_size;
  r->collision_mask = CW_PHYS_ALL_LAYERS;
  r->bake_on_ready = true;
  r->max_agents = 512;
  r->paths_per_tick = 8;
}

static void unjoin_agents(cw_tree *t, cw_node region);

static void region_release(cw_node n, region3d *r) {
  if (cw_navmesh_valid(r->nm)) cw_navmesh_free(r->nm); /* frees the crowd */
  r->nm = (cw_navmesh){ 0 };
  r->crowd = (cw_crowd){ 0 };
  r->polys = r->tiles = 0;
  unjoin_agents(cw_node_tree(n), n);
}

static void region_enter(cw_node n, void *d) {
  region3d *r = d;
  navw *w = navw_of(cw_node_tree(n));
  if (w) cw_array_push(&w->regions, n);
  r->needs_bake = r->bake_on_ready || (r->navmesh_path && r->navmesh_path[0]);
}

static void region_exit(cw_node n, void *d) {
  region3d *r = d;
  navw *w = cw_tree_slot(cw_node_tree(n), "nav");
  region_release(n, r);
  if (w) list_remove(w->regions.data, &w->regions.count, n);
}

static void region_destroy(cw_node n, void *d) {
  region3d *r = d;
  if (cw_navmesh_valid(r->nm)) cw_navmesh_free(r->nm);
  r->nm = (cw_navmesh){ 0 };
  if (r->lines_obj) {
    t3_object *parent = cw_node3d_object(n);
    if (parent) t3_object_remove(parent, r->lines_obj);
  }
  if (r->lines_geo) t3_release(r->lines_geo);
  r->lines_obj = NULL;
  r->lines_geo = NULL;
  cw_phys_lines_free(&r->lines);
}

/* ── baking ───────────────────────────────────────────────────────────── */

typedef struct bake_ctx {
  cw_nav_geom *g;
  cw_node region;
} bake_ctx;

static void add_prim(cw_nav_geom *g, cw_node n) {
  cw_value v;
  if (!cw_node_get_prop(n, "primitive", &v)) return;
  int prim = (int)v.i; /* NONE, BOX, SPHERE, PLANE, CYLINDER */
  if (!cw_node_get_prop(n, "size", &v)) return;
  t3_vec3 size = { v.v[0], v.v[1], v.v[2] };
  const t3_mat4 *world = cw_node3d_global_transform(n);
  t3_mat4 s, m;
  t3_geometry *geo = NULL;
  switch (prim) {
  case 1: /* BOX */
    t3_mat4_compose(&s, (t3_vec3){ 0, 0, 0 }, (t3_quat){ 0, 0, 0, 1 }, size);
    t3_mat4_multiply(&m, world, &s);
    {
      static const float v8[24] = { -0.5f, -0.5f, -0.5f, 0.5f, -0.5f, -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f,
                                    -0.5f, -0.5f, 0.5f,  0.5f, -0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f };
      static const int32_t idx[36] = { 2, 6, 7, 2, 7, 3, 0, 1, 5, 0, 5, 4, 1, 3, 7, 1, 7, 5,
                                       0, 4, 6, 0, 6, 2, 4, 5, 7, 4, 7, 6, 0, 2, 3, 0, 3, 1 };
      cw_nav_geom_add_tris(g, v8, 8, idx, 12, &m, CW_NAV_AREA_WALK);
    }
    return;
  case 3: /* PLANE: in XZ, facing +y */
    t3_mat4_compose(&s, (t3_vec3){ 0, 0, 0 }, (t3_quat){ 0, 0, 0, 1 }, (t3_vec3){ size.x, 1, size.z });
    t3_mat4_multiply(&m, world, &s);
    {
      static const float v4[12] = { -0.5f, 0, -0.5f, 0.5f, 0, -0.5f, -0.5f, 0, 0.5f, 0.5f, 0, 0.5f };
      static const int32_t idx[6] = { 0, 2, 3, 0, 3, 1 };
      cw_nav_geom_add_tris(g, v4, 4, idx, 2, &m, CW_NAV_AREA_WALK);
    }
    return;
  case 2: /* SPHERE: unit diameter, scaled by size.x */
    geo = t3_sphere_geometry_new(0.5f, 12, 8);
    t3_mat4_compose(&s, (t3_vec3){ 0, 0, 0 }, (t3_quat){ 0, 0, 0, 1 }, (t3_vec3){ size.x, size.x, size.x });
    break;
  case 4: /* CYLINDER */
    geo = t3_cylinder_geometry_new(0.5f, 0.5f, 1, 12, 1, false);
    t3_mat4_compose(&s, (t3_vec3){ 0, 0, 0 }, (t3_quat){ 0, 0, 0, 1 }, (t3_vec3){ size.x, size.y, size.x });
    break;
  default: return;
  }
  t3_mat4_multiply(&m, world, &s);
  cw__nav_geom_add_geometry(g, geo, &m, CW_NAV_AREA_WALK);
  t3_release(geo);
}

static bool has_agent_child(cw_node n) {
  for (int i = 0; i < cw_node_child_count(n); i++)
    if (cw_node_get_type(cw_node_child(n, i)) == agent_t) return true;
  return false;
}

static void add_link_node(cw_nav_geom *g, cw_node n);

static bool bake_walk(cw_node n, void *u) {
  bake_ctx *b = u;
  if (cw_node_get_type(n) == link_t) {
    add_link_node(b->g, n);
    return false;
  }
  /* moving things are not level geometry */
  if (cw_node_get_type(n) == obstacle_t || has_agent_child(n) || cw_node_is(n, "RigidBody3D") ||
      cw_node_is(n, "CharacterBody3D") || cw_node_is(n, "KinematicBody3D"))
    return false;
  if (n.id != b->region.id && cw_node_get_type(n) == region_t) return false; /* another region's */
  if (cw_node_is(n, "MeshInstance3D")) {
    cw_value v;
    if (cw_node_get_prop(n, "mesh", &v) && v.res) {
      /* a mesh resource: its built objects, when loaded */
      t3_object *o = cw_node3d_object(n);
      if (o)
        for (int i = 0; i < o->child_count; i++) {
          const t3_object *c = o->children[i];
          t3_mat4 local, m;
          t3_mat4_compose(&local, c->position, c->quaternion, c->scale);
          t3_mat4_multiply(&m, cw_node3d_global_transform(n), &local);
          cw_nav_geom_add_object(b->g, c, &m, CW_NAV_AREA_WALK);
        }
    } else {
      add_prim(b->g, n);
    }
  }
  return true;
}

/* Only the links (a COLLIDERS bake). */
static bool links_walk(cw_node n, void *u) {
  bake_ctx *b = u;
  if (n.id != b->region.id && cw_node_get_type(n) == region_t) return false;
  if (cw_node_get_type(n) == link_t) {
    add_link_node(b->g, n);
    return false;
  }
  return true;
}

static bool in_region(uint64_t shape_id, void *ctx) {
  cw_node region = *(cw_node *)ctx;
  cw_node n = cw_physics3d_body_node(cw_shape3d_body((cw_shape3d){ shape_id }));
  if (CW_HANDLE_IS_NULL(n)) return false;
  for (cw_node p = n; !CW_HANDLE_IS_NULL(p); p = cw_node_parent(p)) {
    if (p.id == region.id) return true;
    if (p.id != n.id && cw_node_get_type(p) == region_t) return false; /* a nested region's */
  }
  return false;
}

static bool region_load(cw_node n, region3d *r) {
  size_t len = 0;
  void *data = cw_vfs_read(r->navmesh_path, &len);
  if (!data) {
    cw_error("NavigationRegion3D %s: cannot read navmesh_path \"%s\"", cw_node_name(n), r->navmesh_path);
    return false;
  }
  cw_navmesh nm = cw_navmesh_load(data, len);
  free(data); /* cw_vfs_read's buffer is malloc'd */
  if (!cw_navmesh_valid(nm)) return false;
  r->nm = nm;
  return true;
}

static bool region_bake_now(cw_node n, region3d *r) {
  region_release(n, r);
  r->needs_bake = false;
  if (r->navmesh_path && r->navmesh_path[0]) {
    if (!region_load(n, r)) return false;
  } else {
    cw_nav_geom g = { 0 };
    bake_ctx b = { &g, n };
    if (r->source != SRC_COLLIDERS) cw_node_walk(n, bake_walk, &b);
    else cw_node_walk(n, links_walk, &b);
    cw_tree *t = cw_node_tree(n);
    if (r->source != SRC_MESHES && t && cw_tree_slot(t, "physics3d")) {
      cw_phys_filter f = { .layers = (uint32_t)r->collision_mask, .fn = in_region, .ctx = &n };
      cw_nav_geom_add_world3d(&g, cw_physics3d_world(t), &f, CW_NAV_AREA_WALK);
    }
    cw_nav_config c = cw_nav_config_default();
    c.cell_size = r->cell_size;
    c.cell_height = r->cell_height;
    c.agent_radius = r->agent_radius;
    c.agent_height = r->agent_height;
    c.agent_max_climb = r->agent_max_climb;
    c.agent_max_slope = r->agent_max_slope;
    c.tile_size = r->tile_size;
    c.dynamic = r->dynamic;
    if (g.tris.count == 0) {
      cw_warn("NavigationRegion3D %s: nothing to bake (no meshes or static colliders under it)", cw_node_name(n));
      cw_nav_geom_free(&g);
      return false;
    }
    r->nm = cw_navmesh_build(&c, &g);
    cw_nav_geom_free(&g);
    if (!cw_navmesh_valid(r->nm)) return false;
  }
  r->crowd = cw_crowd_new(r->nm, r->max_agents > 0 ? r->max_agents : 1);
  cw_crowd_set_plan_budget(r->crowd, r->paths_per_tick, 0);
  cw_navmesh_stats s = cw_navmesh_get_stats(r->nm);
  r->polys = s.polys;
  r->tiles = s.tiles;
  r->build_ms = s.build_ms;
  r->obstacles_dirty = true; /* obstacles re-add themselves */
  navw *w = cw_tree_slot(cw_node_tree(n), "nav");
  if (w)
    for (int i = 0; i < w->obstacles.count; i++) {
      void cw__nav_obstacle_reset(cw_node o, cw_node region);
      cw__nav_obstacle_reset(w->obstacles.data[i], n);
    }
  cw_emit(n, "baked", NULL, 0);
  return true;
}

bool cw_nav_region_bake(cw_node n) {
  region3d *r = cw_node_data_t(n, region_t);
  if (!r) return false;
  return region_bake_now(n, r);
}

cw_navmesh cw_nav_region_navmesh(cw_node n) {
  region3d *r = cw_node_data_t(n, region_t);
  return r ? r->nm : (cw_navmesh){ 0 };
}

cw_crowd cw_nav_region_crowd(cw_node n) {
  region3d *r = cw_node_data_t(n, region_t);
  return r ? r->crowd : (cw_crowd){ 0 };
}

static bool m_bake(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)args, (void)argc, (void)user;
  bool ok = cw_nav_region_bake(self);
  if (ret) *ret = cw_val_bool(ok);
  return ok;
}

/* ── debug draw ───────────────────────────────────────────────────────── */

static void region_pre_render(cw_node n, void *d, double alpha) {
  (void)alpha;
  region3d *r = d;
  t3_object *parent = cw_node3d_object(n);
  if (!r->debug_draw || !cw_navmesh_valid(r->nm) || !parent) {
    if (r->lines_obj) T3_OBJ(r->lines_obj)->visible = false;
    return;
  }
  cw_phys_lines_clear(&r->lines);
  cw_nav_debug_draw(r->nm, &r->lines, CW_NAV_DRAW_DEFAULT);
  cw_crowd_debug_draw(r->crowd, &r->lines, CW_NAV_DRAW_DEFAULT);
  int count = cw_phys_lines_count(&r->lines);
  if (!r->lines_obj) {
    r->lines_geo = t3_geometry_new();
    t3_material *m = t3_line_basic_material_new(0xffffff);
    m->vertex_colors = true;
    m->depth_test = false; /* an overlay */
    m->transparent = true;
    r->lines_obj = t3_line_segments_new(r->lines_geo, m);
    T3_OBJ(r->lines_obj)->frustum_culled = false;
    T3_OBJ(r->lines_obj)->render_order = 1000;
    t3_object_add(parent, r->lines_obj);
    t3_release(r->lines_obj); /* the region's object holds it */
    t3_release(m);
  }
  T3_OBJ(r->lines_obj)->visible = true;
  if (count > r->lines_cap || !r->lines_geo->attributes[T3_ATTR_POSITION]) {
    r->lines_cap = count > 64 ? count + count / 2 : 64;
    float *zero = cw_calloc((size_t)r->lines_cap * 6, sizeof(float), CW_MEM_NAV);
    t3_attribute *pos = t3_attribute_new(T3_FLOAT32, zero, r->lines_cap * 2, 3);
    t3_attribute *col = t3_attribute_new(T3_FLOAT32, zero, r->lines_cap * 2, 3);
    pos->usage = col->usage = T3_DYNAMIC_DRAW;
    t3_geometry_set_attribute(r->lines_geo, T3_ATTR_POSITION, pos);
    t3_geometry_set_attribute(r->lines_geo, T3_ATTR_COLOR, col);
    t3_release(pos);
    t3_release(col);
    cw_free(zero);
  }
  t3_attribute *pos = r->lines_geo->attributes[T3_ATTR_POSITION], *col = r->lines_geo->attributes[T3_ATTR_COLOR];
  memcpy(pos->array, r->lines.pos.data, (size_t)count * 6 * sizeof(float));
  float *c = col->array;
  for (int i = 0; i < count; i++) {
    uint32_t h = r->lines.color.data[i];
    float cr = (float)((h >> 16) & 255) / 255.0f, cg = (float)((h >> 8) & 255) / 255.0f, cb = (float)(h & 255) / 255.0f;
    c[i * 6 + 0] = c[i * 6 + 3] = cr;
    c[i * 6 + 1] = c[i * 6 + 4] = cg;
    c[i * 6 + 2] = c[i * 6 + 5] = cb;
  }
  pos->version++;
  col->version++;
  r->lines_geo->draw_start = 0;
  r->lines_geo->draw_count = count * 2;
  r->lines_geo->version++;
  /* the lines are in world space: undo the region's own transform */
  t3_mat4 inv;
  t3_mat4_invert(&inv, cw_node3d_global_transform(n));
  t3_object *lo = T3_OBJ(r->lines_obj);
  t3_mat4_decompose(&inv, &lo->position, &lo->quaternion, &lo->scale);
}

static void stats_get(cw_node n, void *d, cw_value *o, int which) {
  (void)n;
  region3d *r = d;
  switch (which) {
  case 0: *o = cw_val_int(r->polys); break;
  case 1: *o = cw_val_int(r->tiles); break;
  case 2: *o = cw_val_int(cw_crowd_count(r->crowd)); break;
  case 3: *o = cw_val_float(r->build_ms); break;
  default: *o = cw_val_float(cw_crowd_get_stats(r->crowd).step_ms); break;
  }
}
static void polys_get(cw_node n, void *d, cw_value *o) { stats_get(n, d, o, 0); }
static void tiles_get(cw_node n, void *d, cw_value *o) { stats_get(n, d, o, 1); }
static void agents_get(cw_node n, void *d, cw_value *o) { stats_get(n, d, o, 2); }
static void build_get(cw_node n, void *d, cw_value *o) { stats_get(n, d, o, 3); }
static void step_get(cw_node n, void *d, cw_value *o) { stats_get(n, d, o, 4); }

static const char *const region_signals[] = { "baked", NULL };
static const cw_prop region_props[] = {
  CW_PROP_FIELD_RANGE(region3d, cell_size, CW_T_FLOAT, 0.01, 10, "Voxel width, m."),
  CW_PROP_FIELD_RANGE(region3d, cell_height, CW_T_FLOAT, 0.01, 10, "Voxel height, m."),
  CW_PROP_FIELD_RANGE(region3d, agent_radius, CW_T_FLOAT, 0, 100, "The navmesh keeps this far from walls, m."),
  CW_PROP_FIELD_RANGE(region3d, agent_height, CW_T_FLOAT, 0, 100, "Headroom agents need, m."),
  CW_PROP_FIELD_RANGE(region3d, agent_max_climb, CW_T_FLOAT, 0, 100, "Steps agents walk up, m."),
  CW_PROP_FIELD_RANGE(region3d, agent_max_slope, CW_T_FLOAT, 0, 90, "Steepest walkable slope, degrees."),
  CW_PROP_FIELD_RANGE(region3d, tile_size, CW_T_INT, 0, 4096, "Voxels per tile side (0 = one tile)."),
  CW_PROP_FIELD(region3d, dynamic, CW_T_BOOL, CW_PROP_DEFAULT, "Build through the tile cache, so NavigationObstacle3D nodes can carve it."),
  { .name = "source", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(region3d, source), .size = sizeof(int),
    .enum_names = source_names, .doc = "What bakes: MeshInstance3D meshes, static colliders, or both." },
  CW_PROP_FIELD(region3d, collision_mask, CW_T_INT, CW_PROP_DEFAULT, "Collider layers that bake (bits)."),
  CW_PROP_FIELD(region3d, navmesh_path, CW_T_STRING, CW_PROP_DEFAULT, "Load this saved navmesh (cw_navmesh_save) instead of baking."),
  CW_PROP_FIELD(region3d, bake_on_ready, CW_T_BOOL, CW_PROP_DEFAULT, "Bake on the first tick in the tree."),
  CW_PROP_FIELD_RANGE(region3d, max_agents, CW_T_INT, 1, 65536, "Crowd capacity (set before baking)."),
  CW_PROP_FIELD_RANGE(region3d, paths_per_tick, CW_T_INT, 1, 4096, "Agent paths planned per tick (oldest requests first)."),
  CW_PROP_FIELD(region3d, debug_draw, CW_T_BOOL, CW_PROP_DEFAULT, "Draw the navmesh, agents and paths."),
  CW_PROP_ACCESSOR("polygon_count", CW_T_INT, CW_PROP_READONLY | CW_PROP_EDITOR, polys_get, NULL, "Navmesh polygons."),
  CW_PROP_ACCESSOR("tile_count", CW_T_INT, CW_PROP_READONLY | CW_PROP_EDITOR, tiles_get, NULL, "Navmesh tiles holding polygons."),
  CW_PROP_ACCESSOR("agent_count", CW_T_INT, CW_PROP_READONLY | CW_PROP_EDITOR, agents_get, NULL, "Agents in the crowd."),
  CW_PROP_ACCESSOR("build_ms", CW_T_FLOAT, CW_PROP_READONLY | CW_PROP_EDITOR, build_get, NULL, "The last bake or obstacle update, ms."),
  CW_PROP_ACCESSOR("crowd_step_ms", CW_T_FLOAT, CW_PROP_READONLY | CW_PROP_EDITOR, step_get, NULL, "The last crowd step, ms."),
  CW_PROP_END,
};
static cw_node_type region_type = { .name = "NavigationRegion3D", .parent = "Node3D", .data_size = sizeof(region3d),
                                    .props = region_props, .signals = region_signals,
                                    .doc = "A navmesh baked from the meshes and static colliders under it (or loaded), and its crowd.",
                                    .init = region_init, .destroy = region_destroy, .enter_tree = region_enter,
                                    .exit_tree = region_exit, .pre_render = region_pre_render };

/* ── NavigationAgent3D ────────────────────────────────────────────────── */

static const char *const status_names[] = { "IDLE", "WAITING", "MOVING", "ARRIVED", "NO_PATH", "VELOCITY", "ON_LINK", NULL };

typedef struct agent3d {
  t3_vec3 target_position;
  float radius, height, max_speed, max_accel, neighbor_dist, time_horizon, arrive_dist, slowdown_dist, avoidance_priority;
  int max_neighbors;
  bool avoidance, drive_parent, manual_links;
  const char *region;
  /* runtime */
  cw_node joined;        /* the region whose crowd holds it */
  cw_crowd_agent handle;
  bool has_target, target_pending, reached;
  bool velocity_pending;
  t3_vec3 velocity_request;
  int status;
  t3_vec3 velocity;
  uint32_t path_version;
  uint32_t link; /* the link it is crossing (0: none) */
  bool warned;
} agent3d;

static cw_crowd_agent_params params_of(const agent3d *a) {
  cw_crowd_agent_params p = cw_crowd_agent_params_default();
  p.radius = a->radius;
  p.height = a->height;
  p.max_speed = a->max_speed;
  p.max_accel = a->max_accel;
  p.neighbor_dist = a->neighbor_dist;
  p.max_neighbors = a->max_neighbors;
  p.time_horizon = a->time_horizon;
  p.arrive_dist = a->arrive_dist;
  p.slowdown_dist = a->slowdown_dist;
  p.avoidance = a->avoidance;
  p.avoidance_priority = a->avoidance_priority;
  p.manual_links = a->manual_links;
  return p;
}

static void agent_init(cw_node n, void *d) {
  (void)n;
  agent3d *a = d;
  cw_crowd_agent_params p = cw_crowd_agent_params_default();
  a->radius = p.radius;
  a->height = p.height;
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

static cw_crowd crowd_of(cw_node region) {
  region3d *r = cw_node_data_t(region, region_t);
  return r ? r->crowd : (cw_crowd){ 0 };
}

static void agent_leave(agent3d *a) {
  if (!CW_HANDLE_IS_NULL(a->joined)) cw_crowd_remove(crowd_of(a->joined), a->handle);
  a->joined = (cw_node){ 0 };
  a->handle = (cw_crowd_agent){ 0 };
  a->link = 0;
}

static void agent_enter(cw_node n, void *d) {
  (void)d;
  navw *w = navw_of(cw_node_tree(n));
  if (w) cw_array_push(&w->agents, n);
}

static void agent_exit(cw_node n, void *d) {
  agent3d *a = d;
  agent_leave(a);
  navw *w = cw_tree_slot(cw_node_tree(n), "nav");
  if (w) list_remove(w->agents.data, &w->agents.count, n);
}

static void unjoin_agents(cw_tree *t, cw_node region) {
  navw *w = t ? cw_tree_slot(t, "nav") : NULL;
  if (!w) return;
  for (int i = 0; i < w->agents.count; i++) {
    agent3d *a = cw_node_data_t(w->agents.data[i], agent_t);
    if (a && a->joined.id == region.id) {
      a->joined = (cw_node){ 0 };
      a->handle = (cw_crowd_agent){ 0 };
      if (a->has_target) a->target_pending = true;
    }
  }
}

static void agent_prop_changed(cw_node n, void *d, const cw_prop *p) {
  agent3d *a = d;
  if (!strcmp(p->name, "target_position")) {
    a->has_target = true;
    a->target_pending = true;
    a->reached = false;
    return;
  }
  if (!strcmp(p->name, "region")) {
    agent_leave(a);
    return;
  }
  if (!CW_HANDLE_IS_NULL(a->joined)) {
    cw_crowd_agent_params pp = params_of(a);
    cw_crowd_set_params(crowd_of(a->joined), a->handle, &pp);
  }
  (void)n;
}

static cw_node region_for(cw_node agent_or_obstacle, const char *path, navw *w) {
  if (path && path[0]) {
    cw_node r = cw_node_get(agent_or_obstacle, path);
    return cw_node_get_type(r) == region_t ? r : (cw_node){ 0 };
  }
  return w->regions.count ? w->regions.data[0] : (cw_node){ 0 };
}

static cw_node parent3d(cw_node n) {
  cw_node p = cw_node_parent(n);
  return cw_node_is(p, "Node3D") ? p : (cw_node){ 0 };
}

/* World position p as the local position of n (under its Node3D ancestors). */
static t3_vec3 to_local(cw_node n, t3_vec3 p) {
  cw_node gp = cw_node_parent(n);
  while (!CW_HANDLE_IS_NULL(gp) && !cw_node_is(gp, "Node3D")) gp = cw_node_parent(gp);
  if (CW_HANDLE_IS_NULL(gp)) return p;
  t3_mat4 inv;
  t3_mat4_invert(&inv, cw_node3d_global_transform(gp));
  return t3_vec3_apply_mat4(p, &inv);
}

static bool m_set_velocity(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)ret, (void)user;
  agent3d *a = cw_node_data_t(self, agent_t);
  if (!a || argc < 1) return false;
  a->velocity_request = (t3_vec3){ args[0].v[0], args[0].v[1], args[0].v[2] };
  a->velocity_pending = true;
  a->has_target = a->target_pending = false;
  return true;
}

static bool m_stop(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)args, (void)argc, (void)ret, (void)user;
  agent3d *a = cw_node_data_t(self, agent_t);
  if (!a) return false;
  a->has_target = a->target_pending = a->velocity_pending = false;
  if (!CW_HANDLE_IS_NULL(a->joined)) cw_crowd_clear_target(crowd_of(a->joined), a->handle);
  return true;
}

static bool m_finish_link(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)args, (void)argc, (void)user;
  agent3d *a = cw_node_data_t(self, agent_t);
  bool ok = a && !CW_HANDLE_IS_NULL(a->joined) && cw_crowd_finish_link(crowd_of(a->joined), a->handle);
  if (ret) *ret = cw_val_bool(ok);
  return true;
}

cw_crowd_agent cw_nav_agent_handle(cw_node n) {
  agent3d *a = cw_node_data_t(n, agent_t);
  return a ? a->handle : (cw_crowd_agent){ 0 };
}

static void status_get(cw_node n, void *d, cw_value *o) {
  (void)n;
  agent3d *a = d;
  *o = (cw_value){ CW_T_ENUM };
  o->i = a->status;
}
static void finished_get(cw_node n, void *d, cw_value *o) {
  (void)n;
  agent3d *a = d;
  *o = cw_val_bool(a->status == CW_CROWD_ARRIVED || a->status == CW_CROWD_NO_PATH || a->status == CW_CROWD_IDLE);
}

static const char *const agent_signals[] = { "path_changed", "target_reached", "velocity_computed", "link_entered",
                                             "link_exited", NULL };
static const cw_prop agent_props[] = {
  CW_PROP_FIELD(agent3d, target_position, CW_T_VEC3, CW_PROP_DEFAULT, "Where to go (world space); setting it plans a path."),
  CW_PROP_FIELD_RANGE(agent3d, radius, CW_T_FLOAT, 0.01, 100, "Avoidance radius, m."),
  CW_PROP_FIELD_RANGE(agent3d, height, CW_T_FLOAT, 0.01, 100, "Height, m (agents on other floors are not neighbours)."),
  CW_PROP_FIELD_RANGE(agent3d, max_speed, CW_T_FLOAT, 0, 1000, "m/s."),
  CW_PROP_FIELD_RANGE(agent3d, max_accel, CW_T_FLOAT, 0, 10000, "m/s² (0 = no limit)."),
  CW_PROP_FIELD_RANGE(agent3d, neighbor_dist, CW_T_FLOAT, 0, 1000, "Neighbours within this distance are avoided, m."),
  CW_PROP_FIELD_RANGE(agent3d, max_neighbors, CW_T_INT, 0, CW_CROWD_MAX_NEIGHBORS, "Neighbours avoided at most."),
  CW_PROP_FIELD_RANGE(agent3d, time_horizon, CW_T_FLOAT, 0.01, 100, "How far ahead other agents are avoided, s."),
  CW_PROP_FIELD_RANGE(agent3d, arrive_dist, CW_T_FLOAT, 0, 100, "The target counts as reached within this distance, m."),
  CW_PROP_FIELD_RANGE(agent3d, slowdown_dist, CW_T_FLOAT, 0, 100, "Slows down within this distance of the target, m."),
  CW_PROP_FIELD(agent3d, avoidance, CW_T_BOOL, CW_PROP_DEFAULT, "Avoid other agents (ORCA). Others avoid it either way."),
  CW_PROP_FIELD_RANGE(agent3d, avoidance_priority, CW_T_FLOAT, 0, 1, "Higher: others give way more."),
  CW_PROP_FIELD(agent3d, drive_parent, CW_T_BOOL, CW_PROP_DEFAULT,
                "Move the parent Node3D every tick. Off: the parent moves by velocity_computed and the agent follows it."),
  CW_PROP_FIELD(agent3d, manual_links, CW_T_BOOL, CW_PROP_DEFAULT,
                "Stop at a link's start (ON_LINK) until the finish_link method: the game moves the parent across."),
  CW_PROP_FIELD(agent3d, region, CW_T_STRING, CW_PROP_DEFAULT, "Path to its NavigationRegion3D (empty = the tree's first)."),
  { .name = "status", .type = CW_T_ENUM, .flags = CW_PROP_READONLY | CW_PROP_EDITOR, .offset = -1, .get = status_get,
    .enum_names = status_names, .doc = "IDLE, WAITING (for a path), MOVING, ARRIVED, NO_PATH, VELOCITY or ON_LINK." },
  { .name = "velocity", .type = CW_T_VEC3, .flags = CW_PROP_READONLY | CW_PROP_EDITOR, .offset = offsetof(agent3d, velocity),
    .size = sizeof(t3_vec3), .doc = "The velocity it moved with last tick." },
  CW_PROP_ACCESSOR("is_navigation_finished", CW_T_BOOL, CW_PROP_READONLY | CW_PROP_EDITOR, finished_get, NULL,
                   "Arrived, unreachable or idle."),
  CW_PROP_END,
};
static cw_node_type agent_type = { .name = "NavigationAgent3D", .parent = "Node", .data_size = sizeof(agent3d),
                                   .props = agent_props, .signals = agent_signals,
                                   .doc = "A crowd agent that walks its parent Node3D along navmesh paths, avoiding other agents.",
                                   .init = agent_init, .enter_tree = agent_enter, .exit_tree = agent_exit,
                                   .prop_changed = agent_prop_changed };

/* ── NavigationObstacle3D ─────────────────────────────────────────────── */

static const char *const oshape_names[] = { "CYLINDER", "BOX", NULL };

typedef struct obstacle3d {
  int shape;
  float radius, height;
  t3_vec3 size;
  const char *region;
  /* runtime */
  cw_node in_region;
  uint32_t id;
  t3_vec3 at;
  float yaw;
  bool warned;
} obstacle3d;

static void obstacle_init(cw_node n, void *d) {
  (void)n;
  obstacle3d *o = d;
  o->radius = 0.5f;
  o->height = 2;
  o->size = (t3_vec3){ 1, 2, 1 };
}

static void obstacle_take_out(obstacle3d *o) {
  region3d *r = cw_node_data_t(o->in_region, region_t);
  if (r && o->id && cw_navmesh_valid(r->nm)) {
    cw_nav_obstacle_remove(r->nm, o->id);
    r->obstacles_dirty = true;
  }
  o->id = 0;
  o->in_region = (cw_node){ 0 };
}

void cw__nav_obstacle_reset(cw_node n, cw_node region) {
  obstacle3d *o = cw_node_data_t(n, obstacle_t);
  if (o && o->in_region.id == region.id) {
    o->id = 0;
    o->in_region = (cw_node){ 0 };
  }
}

static void obstacle_enter(cw_node n, void *d) {
  (void)d;
  navw *w = navw_of(cw_node_tree(n));
  if (w) cw_array_push(&w->obstacles, n);
}

static void obstacle_exit(cw_node n, void *d) {
  obstacle3d *o = d;
  obstacle_take_out(o);
  navw *w = cw_tree_slot(cw_node_tree(n), "nav");
  if (w) list_remove(w->obstacles.data, &w->obstacles.count, n);
}

static void obstacle_prop_changed(cw_node n, void *d, const cw_prop *p) {
  (void)n, (void)p;
  obstacle_take_out(d); /* re-added with the new shape at the next tick */
}

static const cw_prop obstacle_props[] = {
  { .name = "shape", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(obstacle3d, shape), .size = sizeof(int),
    .enum_names = oshape_names, .doc = "CYLINDER (standing on the node's origin) or BOX (centered on it, turned with it about y)." },
  CW_PROP_FIELD_RANGE(obstacle3d, radius, CW_T_FLOAT, 0, 1000, "CYLINDER radius, m."),
  CW_PROP_FIELD_RANGE(obstacle3d, height, CW_T_FLOAT, 0, 1000, "CYLINDER height, m."),
  CW_PROP_FIELD(obstacle3d, size, CW_T_VEC3, CW_PROP_DEFAULT, "BOX full size, m."),
  CW_PROP_FIELD(obstacle3d, region, CW_T_STRING, CW_PROP_DEFAULT, "Path to its NavigationRegion3D (empty = the tree's first)."),
  CW_PROP_END,
};
static cw_node_type obstacle_type = { .name = "NavigationObstacle3D", .parent = "Node3D", .data_size = sizeof(obstacle3d),
                                      .props = obstacle_props,
                                      .doc = "A shape carved out of a dynamic NavigationRegion3D's navmesh while it is in the tree.",
                                      .init = obstacle_init, .enter_tree = obstacle_enter, .exit_tree = obstacle_exit,
                                      .prop_changed = obstacle_prop_changed };

/* ── NavigationLink3D ─────────────────────────────────────────────────── */

typedef struct link3d {
  t3_vec3 start_position, end_position;
  bool bidirectional;
  float cost, radius, duration, arc;
  int tag, area;
} link3d;

static void link_init(cw_node n, void *d) {
  (void)n;
  link3d *l = d;
  l->end_position = (t3_vec3){ 0, 0, 2 };
  l->bidirectional = true;
  l->tag = CW_NAV_LINK_JUMP;
}

static void add_link_node(cw_nav_geom *g, cw_node n) {
  link3d *d = cw_node_data_t(n, link_t);
  if (!d) return;
  const t3_mat4 *m = cw_node3d_global_transform(n);
  cw_nav_link l = { 0 };
  l.start = t3_vec3_apply_mat4(d->start_position, m);
  l.end = t3_vec3_apply_mat4(d->end_position, m);
  l.bidirectional = d->bidirectional;
  l.area = (uint8_t)(d->area > 0 && d->area < CW_NAV_AREAS ? d->area : 0);
  l.tag = (uint32_t)(d->tag > 0 ? d->tag : 0);
  l.cost = d->cost;
  l.radius = d->radius;
  l.duration = d->duration;
  l.arc = d->arc;
  cw_nav_geom_add_link(g, &l);
}

static const cw_prop link_props[] = {
  CW_PROP_FIELD(link3d, start_position, CW_T_VEC3, CW_PROP_DEFAULT, "Start, local to the node, m (on the walkable surface)."),
  CW_PROP_FIELD(link3d, end_position, CW_T_VEC3, CW_PROP_DEFAULT, "End, local to the node, m (on the walkable surface)."),
  CW_PROP_FIELD(link3d, bidirectional, CW_T_BOOL, CW_PROP_DEFAULT, "Crossable both ways (off: start to end only)."),
  CW_PROP_FIELD_RANGE(link3d, cost, CW_T_FLOAT, 0, 100000, "Crossing cost in metres of walking (0 = its length)."),
  CW_PROP_FIELD_RANGE(link3d, tag, CW_T_INT, 0, 2147483647, "The game's kind of link (1 jump, 2 drop, 3 pad, 4 ladder, 5 teleport, 16 and up its own)."),
  CW_PROP_FIELD_RANGE(link3d, area, CW_T_INT, 0, 63, "Area id filters weigh or exclude (0 = walk)."),
  CW_PROP_FIELD_RANGE(link3d, radius, CW_T_FLOAT, 0, 100, "How far across an end may be from the navmesh, m (0 = agent radius)."),
  CW_PROP_FIELD_RANGE(link3d, duration, CW_T_FLOAT, 0, 1000, "Crossing time for crowd agents, s (0 = length / speed)."),
  CW_PROP_FIELD_RANGE(link3d, arc, CW_T_FLOAT, 0, 1000, "Height of the crossing's arc at its middle, m."),
  CW_PROP_END,
};
static cw_node_type link_type = { .name = "NavigationLink3D", .parent = "Node3D", .data_size = sizeof(link3d),
                                  .props = link_props,
                                  .doc = "A link (jump, drop, pad, ladder) baked into the NavigationRegion3D above it: paths cross it and agents traverse it.",
                                  .init = link_init };

/* ── the tick ─────────────────────────────────────────────────────────── */

static void sync_obstacles(navw *w) {
  for (int i = 0; i < w->obstacles.count; i++) {
    cw_node n = w->obstacles.data[i];
    obstacle3d *o = cw_node_data_t(n, obstacle_t);
    cw_node rn = region_for(n, o->region, w);
    region3d *r = cw_node_data_t(rn, region_t);
    if (!r || !cw_navmesh_valid(r->nm)) continue;
    if (!r->dynamic) {
      if (!o->warned) cw_warn("NavigationObstacle3D %s: its region is not dynamic", cw_node_name(n));
      o->warned = true;
      continue;
    }
    const t3_mat4 *m = cw_node3d_global_transform(n);
    t3_vec3 at = { m->e[12], m->e[13], m->e[14] };
    float yaw = cw_atan2(-m->e[2], m->e[0]);
    if (o->id && o->in_region.id == rn.id) {
      float dx = at.x - o->at.x, dy = at.y - o->at.y, dz = at.z - o->at.z;
      if (dx * dx + dy * dy + dz * dz < 1e-4f && yaw == o->yaw) continue; /* still in place */
      obstacle_take_out(o);
    } else if (o->id) {
      obstacle_take_out(o);
    }
    o->id = o->shape == 0 ? cw_nav_obstacle_add_cylinder(r->nm, at, o->radius, o->height)
                          : cw_nav_obstacle_add_box(r->nm, at, (t3_vec3){ o->size.x * 0.5f, o->size.y * 0.5f, o->size.z * 0.5f }, yaw);
    o->in_region = rn;
    o->at = at;
    o->yaw = yaw;
    r->obstacles_dirty = true;
  }
  for (int i = 0; i < w->regions.count; i++) {
    region3d *r = cw_node_data_t(w->regions.data[i], region_t);
    if (r && r->obstacles_dirty && cw_navmesh_valid(r->nm) && r->dynamic) {
      cw_nav_update(r->nm);
      r->build_ms = cw_navmesh_get_stats(r->nm).build_ms;
    }
    if (r) r->obstacles_dirty = false;
  }
}

static void hook(cw_tree *t, double dt, void *u) {
  (void)u;
  navw *w = cw_tree_slot(t, "nav");
  if (!w) return;
  /* 1. bakes */
  for (int i = 0; i < w->regions.count; i++) {
    region3d *r = cw_node_data_t(w->regions.data[i], region_t);
    if (r && r->needs_bake) region_bake_now(w->regions.data[i], r);
  }
  /* 2. obstacles */
  sync_obstacles(w);
  /* 3. agents join, take targets, follow their parents */
  for (int i = 0; i < w->agents.count; i++) {
    cw_node n = w->agents.data[i];
    agent3d *a = cw_node_data_t(n, agent_t);
    cw_node body = parent3d(n);
    if (CW_HANDLE_IS_NULL(body)) continue;
    if (CW_HANDLE_IS_NULL(a->joined)) {
      cw_node rn = region_for(n, a->region, w);
      cw_crowd c = crowd_of(rn);
      if (!cw_crowd_valid(c)) continue;
      cw_crowd_agent_params p = params_of(a);
      a->handle = cw_crowd_add(c, cw_node3d_global_position(body), &p);
      if (CW_HANDLE_IS_NULL(a->handle)) {
        if (!a->warned) cw_warn("NavigationAgent3D %s: its parent is off the navmesh (or the crowd is full)", cw_node_name(n));
        a->warned = true;
        continue;
      }
      a->joined = rn;
      a->path_version = 0;
    }
    cw_crowd c = crowd_of(a->joined);
    if (!a->drive_parent) cw_crowd_set_position(c, a->handle, cw_node3d_global_position(body));
    if (a->target_pending) {
      cw_crowd_set_target(c, a->handle, a->target_position);
      a->target_pending = false;
    }
    if (a->velocity_pending) {
      cw_crowd_set_velocity(c, a->handle, a->velocity_request);
      a->velocity_pending = false;
    }
  }
  /* 4. crowds step */
  for (int i = 0; i < w->regions.count; i++) {
    region3d *r = cw_node_data_t(w->regions.data[i], region_t);
    if (r && cw_crowd_valid(r->crowd)) cw_crowd_step(r->crowd, (float)dt);
  }
  /* 5. parents move; signals */
  for (int i = 0; i < w->agents.count; i++) {
    cw_node n = w->agents.data[i];
    agent3d *a = cw_node_data_t(n, agent_t);
    if (CW_HANDLE_IS_NULL(a->joined)) continue;
    cw_crowd_agent_state s;
    if (!cw_crowd_get_state(crowd_of(a->joined), a->handle, &s)) continue;
    a->status = (int)s.status;
    a->velocity = s.velocity;
    cw_node body = parent3d(n);
    if (a->drive_parent && !CW_HANDLE_IS_NULL(body)) {
      cw_node3d_snapshot(body);
      cw_node3d_set_position(body, to_local(body, s.position));
    }
    if (s.path_version != a->path_version) {
      a->path_version = s.path_version;
      cw_emit(n, "path_changed", NULL, 0);
    }
    if (s.status == CW_CROWD_ARRIVED && !a->reached) {
      a->reached = true;
      cw_emit(n, "target_reached", NULL, 0);
    }
    if (s.link != a->link) {
      if (a->link) {
        cw_value v = cw_val_int(a->link);
        cw_emit(n, "link_exited", &v, 1);
      }
      a->link = s.link;
      if (s.link) {
        cw_nav_link l;
        cw_navmesh nm = cw_nav_region_navmesh(a->joined);
        uint32_t tag = cw_nav_link_get(nm, s.link, &l) ? l.tag : 0;
        cw_value v[3] = { cw_val_int(s.link), cw_val_int(tag), cw_val_vec3(s.link_end.x, s.link_end.y, s.link_end.z) };
        cw_emit(n, "link_entered", v, 3);
      }
    }
    if (s.status == CW_CROWD_MOVING || s.status == CW_CROWD_VELOCITY) {
      cw_value v = cw_val_vec3(s.velocity.x, s.velocity.y, s.velocity.z);
      cw_emit(n, "velocity_computed", &v, 1);
    }
  }
}

void cw__register_nav_types(void) {
  cw_node_type_register(&region_type);
  region_t = &region_type;
  cw_node_type_register(&agent_type);
  agent_t = &agent_type;
  cw_node_type_register(&obstacle_type);
  obstacle_t = &obstacle_type;
  cw_node_type_register(&link_type);
  link_t = &link_type;
  cw_node_type_add_method("NavigationRegion3D", "bake", NULL, m_bake, NULL, "Bake (or load) the navmesh now.");
  cw_node_type_add_method("NavigationAgent3D", "set_velocity", "vec3", m_set_velocity, NULL,
                          "Move at this velocity (avoiding others) instead of following a path.");
  cw_node_type_add_method("NavigationAgent3D", "stop", NULL, m_stop, NULL, "Drop the target and stand.");
  cw_node_type_add_method("NavigationAgent3D", "finish_link", NULL, m_finish_link, NULL,
                          "End a manual_links crossing: the agent stands on the link's end and walks on.");
}
