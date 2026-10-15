/* nav_demo: a crowd of 500 agents crossing a level, with the navigation nodes
 * (cartwheel/nav.h).
 *
 * The level (samples/common/nav_level.h: a floor, two walls with doorways,
 * pillars, a ramp up to a deck) is built as MeshInstance3D boxes under a
 * dynamic NavigationRegion3D, which bakes its navmesh on the first tick (tiles
 * on the job pool). Two teams of agents (NavigationAgent3D, each moving a
 * Node3D with a cylinder) start on opposite sides of the walls and walk to
 * random points anywhere on the level; on target_reached each picks another. Every
 * 20 s a gate (NavigationObstacle3D) closes one doorway for 10 s: the tiles
 * under it rebuild and the agents heading through it replan.
 *
 * CW_NAV_AGENTS sets the crowd size (default 500).
 *
 * Debug fields (equal on every host and at every worker count):
 *   nav.hash: every tick's crowd hash (positions, velocities, states), folded;
 *   nav.mesh_hash: the navmesh's tile bytes after the latest change;
 *   nav.arrivals, nav.agents, nav.polys, nav.gate_changes.
 * Timings (reported, not compared): nav.build_ms (the bake), nav.update_ms (the
 * last obstacle update), nav.step_ms (crowd step, mean of the last 60 ticks),
 * nav.step_ms_max. */
#include <stdlib.h>

#include "../common/nav_level.h"
#include "cartwheel/debug.h"
#include "cartwheel/jobs.h"
#include "cartwheel/nav.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

#define GATE_PERIOD 1200 /* ticks (60 Hz): closed for the second half */

static struct {
  cw_node region, gate;
  cw_rng rng;
  int agents;
  uint64_t hash;
  uint32_t hash_lo, hash_hi, mesh_hash;
  int32_t arrivals, live, polys, gate_changes, threads;
  float build_ms, update_ms, step_ms, step_ms_max;
  float step_ring[60];
  int ring_n;
} G;

/* A random point on the level (snapped to the navmesh when it becomes a target). */
static t3_vec3 random_point(void) {
  return (t3_vec3){ cw_rng_range(&G.rng, -29, 29), 0, cw_rng_range(&G.rng, -29, 29) };
}

/* target_reached: off to another random point */
/* #region reached */
static void on_reached(cw_node agent, const cw_value *args, int argc, void *user) {
  (void)args, (void)argc, (void)user;
  G.arrivals++;
  t3_vec3 t = random_point();
  cw_node_set(agent, "target_position", cw_val_vec3(t.x, t.y, t.z));
}
/* #endregion */

static void build_level(cw_node region) {
  nav_level_box b[64];
  int n = nav_level_boxes(b, 64);
  for (int i = 0; i < n; i++) {
    cw_node m = cw_node_new("MeshInstance3D", "Block");
    cw_node_add_child(region, m);
    cw_node3d_set_position(m, b[i].center);
    cw_node_set(m, "size", cw_val_vec3(b[i].half.x * 2, b[i].half.y * 2, b[i].half.z * 2));
    cw_node_set(m, "color", i == 0 ? cw_val_color(0.08f, 0.09f, 0.11f, 1) : cw_val_color(0.35f, 0.37f, 0.42f, 1));
  }
}

static void spawn_agents(cw_node parent) {
  for (int i = 0; i < G.agents; i++) {
    /* two teams, west and east of the walls (only their colour differs) */
    bool east = i & 1;
    t3_vec3 p = { cw_rng_range(&G.rng, 13, 29), 0, cw_rng_range(&G.rng, -29, 29) };
    if (!east) p.x = -p.x;
    cw_node body = cw_node_new("Node3D", "Agent");
    cw_node_add_child(parent, body);
    cw_node3d_set_position(body, p);
    cw_node mesh = cw_node_new("MeshInstance3D", "Body");
    cw_node_add_child(body, mesh);
    cw_node_set(mesh, "primitive", cw_val_enum("CYLINDER"));
    cw_node_set(mesh, "size", cw_val_vec3(0.8f, 1.6f, 0.8f));
    cw_node3d_set_position(mesh, (t3_vec3){ 0, 0.8f, 0 });
    cw_node_set(mesh, "color", east ? cw_val_color(0.95f, 0.55f, 0.15f, 1) : cw_val_color(0.2f, 0.6f, 1.0f, 1));
    /* #region agent */
    cw_node nav = cw_node_new("NavigationAgent3D", "Nav");
    cw_node_add_child(body, nav);
    cw_node_set(nav, "radius", cw_val_float(0.45));
    cw_node_set(nav, "max_speed", cw_val_float(cw_rng_range(&G.rng, 2.5f, 3.5f)));
    t3_vec3 t = random_point();
    cw_node_set(nav, "target_position", cw_val_vec3(t.x, t.y, t.z));
    cw_connect(nav, "target_reached", (cw_node){ 0 }, NULL, on_reached, NULL, 0);
    /* #endregion */
  }
}

static void init(void *u) {
  (void)u;
  cw_tree *t = cw_scene_app_tree();
  cw_node root = cw_tree_root(t);
  G.region = cw_node_get(root, "Level/Nav");
  G.gate = cw_node_get(root, "Level/Gate");
  cw_rng_seed(&G.rng, 2026);
  G.agents = getenv("CW_NAV_AGENTS") ? atoi(getenv("CW_NAV_AGENTS")) : 500;
  if (G.agents < 0) G.agents = 0;
  if (G.agents > 4096) G.agents = 4096;
  build_level(G.region);
  spawn_agents(cw_node_get(root, "Level/Agents"));
  cw_debug_i32("nav.threads", &G.threads);
  cw_debug_u32("nav.hash", &G.hash_lo);
  cw_debug_u32("nav.hash_hi", &G.hash_hi);
  cw_debug_u32("nav.mesh_hash", &G.mesh_hash);
  cw_debug_i32("nav.arrivals", &G.arrivals);
  cw_debug_i32("nav.agents", &G.live);
  cw_debug_i32("nav.polys", &G.polys);
  cw_debug_i32("nav.gate_changes", &G.gate_changes);
  cw_debug_f32("nav.build_ms", &G.build_ms);
  cw_debug_f32("nav.update_ms", &G.update_ms);
  cw_debug_f32("nav.step_ms", &G.step_ms);
  cw_debug_f32("nav.step_ms_max", &G.step_ms_max);
}

static void fixed(void *u, double dt) {
  (void)u, (void)dt;
  cw_tree *t = cw_scene_app_tree();
  uint64_t tick = cw_tree_tick(t);
  cw_navmesh nm = cw_nav_region_navmesh(G.region);
  cw_crowd cr = cw_nav_region_crowd(G.region);
  if (!cw_navmesh_valid(nm)) return;
  /* the gate: parked off the level, then in the left wall's south doorway */
  if (tick % GATE_PERIOD == GATE_PERIOD / 2 || tick % GATE_PERIOD == 0) {
    bool close = tick % GATE_PERIOD == GATE_PERIOD / 2;
    cw_node3d_set_position(G.gate, close ? (t3_vec3){ -10, 1, -6 } : (t3_vec3){ -10, 1, 40 });
    G.gate_changes++;
  }
  cw_navmesh_stats s = cw_navmesh_get_stats(nm);
  if (!G.build_ms) G.build_ms = s.build_ms;
  else if (s.build_ms != G.build_ms) G.update_ms = s.build_ms;
  G.polys = s.polys;
  G.mesh_hash = (uint32_t)cw_navmesh_hash(nm);
  uint64_t h = cw_crowd_hash(cr);
  G.hash = cw_hash_bytes(&h, sizeof h, G.hash);
  G.hash_lo = (uint32_t)G.hash;
  G.hash_hi = (uint32_t)(G.hash >> 32);
  G.live = cw_crowd_count(cr);
  G.threads = cw_jobs_thread_count();
  float ms = cw_crowd_get_stats(cr).step_ms;
  G.step_ring[G.ring_n++ % 60] = ms;
  int n = G.ring_n < 60 ? G.ring_n : 60;
  float sum = 0;
  for (int i = 0; i < n; i++) sum += G.step_ring[i];
  G.step_ms = sum / (float)n;
  if (ms > G.step_ms_max && G.ring_n > 30) G.step_ms_max = ms; /* after the first paths are planned */
  if (tick % 600 == 0)
    cw_info("nav_demo: tick %llu, %d agents, %d arrivals, step %.3f ms (max %.3f), build %.1f ms, update %.2f ms",
            (unsigned long long)tick, G.live, G.arrivals, (double)G.step_ms, (double)G.step_ms_max, (double)G.build_ms,
            (double)G.update_ms);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  static cw_app_desc base = { .title = "nav_demo", .width = 960, .height = 540, .debug = true, .init = init,
                              .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
