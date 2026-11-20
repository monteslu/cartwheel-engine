/* tiles2d_perf: timings of the 2D tile systems on a 256 x 256 map (tests/tiles2d-perf.mjs reads them).
 *
 * The map is made from a seed: blocks of solid ground four cells across with slopes on some of their tops, so
 * outlines have corners, diagonals and partly shared edges. On tick 5 it measures
 *   - TileCollision2D building all 64 chunks (best of 5), and one chunk rebuilt after a cell edit;
 *   - a navigation grid baked from the map, 1000 paths between random walkable cells (one batch on the job
 *     pool, best of 5), a flow field toward one goal (best of 5), and 600 steps of a crowd of 500 on paths;
 * and publishes what was built (chains, segments, hashes of the grid, the paths, the field and the crowd), which
 * must be the same on every host.
 *
 * Debug fields: perf.col_build_ms, perf.col_chunk_ms, perf.col_edit_ms, perf.nav_bake_ms, perf.paths_ms,
 * perf.paths_per_ms, perf.flow_ms, perf.crowd_step_ms (timings, not compared), perf.col_chunks, perf.col_chains,
 * perf.col_segments, perf.nav_walkable, perf.grid_hash, perf.paths_hash, perf.paths_found, perf.flow_hash,
 * perf.crowd_hash, perf.threads, perf.done. */
#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/jobs.h"
#include "cartwheel/nav2d.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene_file.h"

#define N 256

static float col_build_ms, col_chunk_ms, col_edit_ms;
static int32_t col_chunks, col_chains, col_segments, done, tick;
static float nav_bake_ms, paths_ms, paths_per_ms, flow_ms, crowd_step_ms;
static int32_t nav_walkable, paths_found, threads;
static uint32_t grid_hash, paths_hash, flow_hash, crowd_hash;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

static uint32_t hash2(int x, int y) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

static bool block(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N && hash2(x >> 2, y >> 2) % 4 == 0; }

static void bench(void) {
  cw_tree *t = cw_scene_app_tree();
  cw_node map = cw_node_new("TileMap2D", "Map");
  cw_node_set(map, "tile_shapes", cw_val_string("1: full; 5: slope 0 16; 6: slope 16 0"));
  double t0;
  cw_tilemap2d_set_cell(map, 0, N - 1, N - 1, -1, 0); /* its size, once */
  for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++) {
      if (block(x, y)) cw_tilemap2d_set_cell(map, 0, x, y, 1, 0);
      else if (block(x, y + 1) && hash2(x, y) % 4 == 0)
        cw_tilemap2d_set_cell(map, 0, x, y, block(x + 1, y) ? 5 : block(x - 1, y) ? 6 : -1, 0); /* a ramp against a block */
    }
  cw_node col = cw_node_new("TileCollision2D", "Solid");
  cw_node_add_child(map, col);
  cw_node_add_child(cw_tree_root(t), map);
  col_build_ms = 1e9f;
  for (int i = 0; i < 5; i++) {
    cw_node_set(col, "layers", cw_val_int(i & 1 ? -1 : 1)); /* the same layer, asked for another way: everything again */
    t0 = now_ms();
    cw_tile_collision2d_update(col);
    float ms = (float)(now_ms() - t0);
    if (ms < col_build_ms) col_build_ms = ms;
  }
  cw_tile_collision2d_stats s = cw_tile_collision2d_stats_get(col);
  col_chunks = s.chunks, col_chains = s.chains, col_segments = s.segments;
  col_chunk_ms = col_build_ms / 64.0f;
  col_edit_ms = 1e9f;
  for (int i = 0; i < 5; i++) {
    cw_tilemap2d_set_cell(map, 0, 100, 100, i & 1 ? -1 : 1, 0); /* the middle of a chunk: that chunk alone */
    t0 = now_ms();
    cw_tile_collision2d_update(col);
    float ms = (float)(now_ms() - t0);
    if (ms < col_edit_ms) col_edit_ms = ms;
  }
  /* #region nav */
  /* navigation over the same map: a grid of its free cells */
  t0 = now_ms();
  cw_navgrid g = cw_navgrid_from_tilemap(map, NULL);
  nav_bake_ms = (float)(now_ms() - t0);
  nav_walkable = cw_navgrid_get_stats(g).walkable;
  grid_hash = (uint32_t)cw_navgrid_hash(g);
  /* 1000 paths between walkable cells drawn from a seed, as one batch on the job pool */
  enum { PATHS = 1000, MAX_PTS = 32 };
  static cw_navgrid_path_req reqs[PATHS];
  static cw_vec2 pts[PATHS * MAX_PTS];
  static int counts[PATHS];
  cw_rng rng;
  cw_rng_seed(&rng, 7);
  for (int i = 0; i < PATHS; i++) {
    cw_vec2 ends[2];
    for (int e = 0; e < 2; e++) {
      int x, y;
      do {
        uint32_t rx = cw_rng_u32(&rng), ry = cw_rng_u32(&rng);
        x = (int)(rx % N), y = (int)(ry % N);
      } while (!cw_navgrid_walkable(g, x, y));
      ends[e] = cw_navgrid_cell_center(g, x, y);
    }
    reqs[i] = (cw_navgrid_path_req){ ends[0], ends[1] };
  }
  paths_ms = 1e9f;
  for (int k = 0; k < 5; k++) {
    t0 = now_ms();
    cw_navgrid_find_paths(g, reqs, PATHS, NULL, pts, MAX_PTS, counts);
    float ms = (float)(now_ms() - t0);
    if (ms < paths_ms) paths_ms = ms;
  }
  /* #endregion */
  paths_per_ms = PATHS / paths_ms;
  paths_hash = CW_HASH32_INIT;
  for (int i = 0; i < PATHS; i++) {
    paths_found += counts[i] > 0;
    paths_hash = cw_hash32(&counts[i], sizeof counts[i], paths_hash);
    paths_hash = cw_hash32(&pts[i * MAX_PTS], sizeof(cw_vec2) * (size_t)counts[i], paths_hash);
  }
  /* a flow field toward the middle of the map */
  cw_flowfield f = cw_flowfield_new(g);
  cw_vec2 goal;
  cw_navgrid_nearest(g, cw_navgrid_cell_center(g, N / 2, N / 2), 64 * 16, &goal);
  flow_ms = 1e9f;
  for (int k = 0; k < 5; k++) {
    t0 = now_ms();
    cw_flowfield_build(f, &goal, 1);
    float ms = (float)(now_ms() - t0);
    if (ms < flow_ms) flow_ms = ms;
  }
  flow_hash = (uint32_t)cw_flowfield_hash(f);
  /* a crowd of 500 walking the first 500 paths' ends: 600 steps */
  cw_crowd2d c = cw_crowd2d_new(g, 500);
  for (int i = 0; i < 500; i++) {
    cw_crowd2d_agent a = cw_crowd2d_add(c, reqs[i].from, NULL);
    cw_crowd2d_set_target(c, a, reqs[i].to);
  }
  cw_crowd2d_set_plan_budget(c, 32, 0);
  t0 = now_ms();
  for (int s = 0; s < 600; s++) cw_crowd2d_step(c, 1.0f / 60);
  crowd_step_ms = (float)(now_ms() - t0) / 600.0f;
  crowd_hash = (uint32_t)cw_crowd2d_hash(c);
  threads = cw_jobs_thread_count();
  cw_navgrid_free(g);
  cw_node_free(map);
  done = 1;
}

static void init(void *u) {
  (void)u;
  cw_debug_f32("perf.col_build_ms", &col_build_ms);
  cw_debug_f32("perf.col_chunk_ms", &col_chunk_ms);
  cw_debug_f32("perf.col_edit_ms", &col_edit_ms);
  cw_debug_i32("perf.col_chunks", &col_chunks);
  cw_debug_i32("perf.col_chains", &col_chains);
  cw_debug_i32("perf.col_segments", &col_segments);
  cw_debug_f32("perf.nav_bake_ms", &nav_bake_ms);
  cw_debug_f32("perf.paths_ms", &paths_ms);
  cw_debug_f32("perf.paths_per_ms", &paths_per_ms);
  cw_debug_f32("perf.flow_ms", &flow_ms);
  cw_debug_f32("perf.crowd_step_ms", &crowd_step_ms);
  cw_debug_i32("perf.nav_walkable", &nav_walkable);
  cw_debug_i32("perf.paths_found", &paths_found);
  cw_debug_i32("perf.threads", &threads);
  cw_debug_u32("perf.grid_hash", &grid_hash);
  cw_debug_u32("perf.paths_hash", &paths_hash);
  cw_debug_u32("perf.flow_hash", &flow_hash);
  cw_debug_u32("perf.crowd_hash", &crowd_hash);
  cw_debug_i32("perf.done", &done);
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  if (++tick == 5) bench();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "tiles2d_perf", .width = 320, .height = 180, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app(NULL, &base);
}
