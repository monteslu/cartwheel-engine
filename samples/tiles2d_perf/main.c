/* tiles2d_perf: timings of the 2D tile systems on a 256 x 256 map (tests/tiles2d-perf.mjs reads them).
 *
 * The map is made from a seed: blocks of solid ground four cells across with slopes on some of their tops, so
 * outlines have corners, diagonals and partly shared edges. On tick 5 it measures
 *   - TileCollision2D building all 64 chunks (best of 5), and one chunk rebuilt after a cell edit;
 * and publishes what was built (chains, segments), which must be the same on every host.
 *
 * Debug fields: perf.col_build_ms, perf.col_chunk_ms, perf.col_edit_ms (timings, not compared), perf.col_chunks,
 * perf.col_chains, perf.col_segments, perf.done. */
#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene_file.h"

#define N 256

static float col_build_ms, col_chunk_ms, col_edit_ms;
static int32_t col_chunks, col_chains, col_segments, done, tick;

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

static bool block(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N && hash2(x >> 2, y >> 2) % 5 < 2; }

static void bench(void) {
  cw_tree *t = cw_scene_app_tree();
  cw_node map = cw_node_new("TileMap2D", "Map");
  cw_node_set(map, "tile_shapes", cw_val_string("1: full; 5: slope 0 16; 6: slope 16 0"));
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
    double t0 = now_ms();
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
    double t0 = now_ms();
    cw_tile_collision2d_update(col);
    float ms = (float)(now_ms() - t0);
    if (ms < col_edit_ms) col_edit_ms = ms;
  }
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
