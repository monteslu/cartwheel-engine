/* perf_nodes (gate): spawn 100k nodes into a tree, process a few
 * phases, free them all. Node3D (with its three.c object) and plain Node
 * timings are debug fields; counts are checked, times are reported. */
#include <stdlib.h>
#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene.h"

static float spawn3d_ms, process3d_ms, free3d_ms, spawn_ms, free_ms;
static int32_t peak_nodes, after_free, done;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void run_bench(const char *type, float *spawn, float *process, float *fr) {
  cw_tree *t = cw_tree_new();
  double t0 = now_ms();
  cw_node parent = cw_node_new(type, "Group");
  cw_node_add_child(cw_tree_root(t), parent);
  /* CW_PERF_COUNT overrides the size (the gate's control uses it to prove a
   * different run is detected) */
  int count = getenv("CW_PERF_COUNT") ? atoi(getenv("CW_PERF_COUNT")) : 100000;
  for (int i = 0; i < count; i++) {
    cw_node n = cw_node_new(type, "N");
    cw_node_add_child(parent, n);
  }
  *spawn = (float)(now_ms() - t0);
  peak_nodes = cw_tree_node_count(t);
  t0 = now_ms();
  for (int k = 0; k < 10; k++) {
    cw_tree_fixed_update(t, 1.0 / 60);
    cw_tree_update(t, 1.0 / 60);
    cw_tree_pre_render(t, 1);
  }
  if (process) *process = (float)(now_ms() - t0) / 10;
  t0 = now_ms();
  cw_node_remove_child(cw_tree_root(t), parent); /* leave the tree */
  double exit_ms = now_ms() - t0;
  t0 = now_ms();
  cw_node_free(parent);
  *fr = (float)(now_ms() - t0 + exit_ms);
  cw_info("%s: exit %.1f ms, free %.1f ms", type, exit_ms, now_ms() - t0);
  after_free = cw_tree_node_count(t);
  cw_tree_free(t);
}

static void init(void *u) {
  (void)u;
  cw_scene_register_builtin_types();
  cw_debug_f32("spawn3d_ms", &spawn3d_ms);
  cw_debug_f32("process3d_ms", &process3d_ms);
  cw_debug_f32("free3d_ms", &free3d_ms);
  cw_debug_f32("spawn_ms", &spawn_ms);
  cw_debug_f32("free_ms", &free_ms);
  cw_debug_i32("peak_nodes", &peak_nodes);
  cw_debug_i32("after_free", &after_free);
  cw_debug_i32("done", &done);
  run_bench("Node", &spawn_ms, NULL, &free_ms);
  run_bench("Node3D", &spawn3d_ms, &process3d_ms, &free3d_ms);
  done = 1;
  cw_info("100k Node: spawn %.1f ms free %.1f ms | 100k Node3D: spawn %.1f ms, phases %.2f ms/frame, free %.1f ms",
          spawn_ms, free_ms, spawn3d_ms, process3d_ms, free3d_ms);
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  glClearColor(0.1f, 0.3f, 0.5f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "perf_nodes", .width = 64, .height = 64, .debug = true, .init = init, .render = render };
}
