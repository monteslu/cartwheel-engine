/* ui_perf: 5000+ controls (a scrolling grid of 4950 buttons under a menu
 * bar and a status line). Every frame from frame 3 it forces a FULL
 * relayout (every control dirty) and times it, times an incremental layout
 * (one button's text changes), and times the UI's draw submission; the
 * means are perf.* debug fields (reported, never compared: hosts differ in
 * speed). Counts (controls, arranged, draw calls) are compared. */
#include <time.h>
#include <stdio.h>

#include "cartwheel/gpu.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/ui.h"

#define COLS 50
#define ROWS 99

static double wall(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static struct {
  cw_tree *tree;
  cw_node root, grid, status, probe;
  int32_t frames, samples, controls, full_arranged, incr_arranged;
  float build_ms, full_ms, incr_ms, draw_ms, first_layout_ms;
  double full_sum, incr_sum, draw_sum;
} P;

static void init(void *u) {
  (void)u;
  cw_scene_register_builtin_types();
  P.tree = cw_tree_new();
  cw_set_current_tree(P.tree);
  double t0 = wall();
  P.root = cw_node_new("Panel", "Root");
  cw_node_add_child(cw_tree_root(P.tree), P.root);
  cw_control_set_anchors_preset(P.root, CW_PRESET_FULL_RECT, 0);
  cw_node v = cw_node_new("VBoxContainer", "V");
  cw_node_add_child(P.root, v);
  cw_control_set_anchors_preset(v, CW_PRESET_FULL_RECT, 8);
  cw_node bar = cw_node_new("MenuBar", "Menu");
  cw_node_add_child(v, bar);
  cw_node m = cw_node_new("PopupMenu", "File");
  cw_node_add_child(bar, m);
  cw_node_set(m, "items", cw_val_string("Open|1\nQuit|2"));
  cw_node sc = cw_node_new("ScrollContainer", "Scroll");
  cw_node_add_child(v, sc);
  cw_control_set_size_flags(sc, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  P.grid = cw_node_new("GridContainer", "Grid");
  cw_node_add_child(sc, P.grid);
  cw_node_set(P.grid, "columns", cw_val_int(COLS));
  char name[16];
  for (int i = 0; i < COLS * ROWS; i++) {
    snprintf(name, sizeof name, "B%d", i);
    cw_node b = cw_node_new("Button", name);
    cw_button_set_text(b, name);
    cw_node_add_child(P.grid, b);
    if (i == 1234) P.probe = b;
  }
  P.status = cw_node_new("Label", "Status");
  cw_node_add_child(v, P.status);
  P.build_ms = (float)((wall() - t0) * 1000);
  t0 = wall();
  cw_ui_layout(P.tree);
  P.first_layout_ms = (float)((wall() - t0) * 1000);
  cw_debug_i32("perf.controls", &P.controls);
  cw_debug_i32("perf.samples", &P.samples);
  cw_debug_i32("perf.full_arranged", &P.full_arranged);
  cw_debug_i32("perf.incr_arranged", &P.incr_arranged);
  cw_debug_f32("perf.build_ms", &P.build_ms);
  cw_debug_f32("perf.first_layout_ms", &P.first_layout_ms);
  cw_debug_f32("perf.full_layout_ms", &P.full_ms);
  cw_debug_f32("perf.incr_layout_ms", &P.incr_ms);
  cw_debug_f32("perf.draw_submit_ms", &P.draw_ms);
  cw_ui_debug_register("ui");
}

static void update(void *u, double dt) {
  (void)u, (void)dt;
  P.frames++;
  P.controls = (int32_t)cw_ui_get_stats()->controls;
  if (P.frames < 3) return;
  /* full relayout: re-theming the root marks every control dirty */
  cw_control_set_theme(P.root, (cw_theme){ 0 });
  double t0 = wall();
  cw_ui_layout(P.tree);
  double full = wall() - t0;
  P.full_arranged = (int32_t)cw_ui_get_stats()->arranged;
  /* incremental: one button's text */
  cw_button_set_text(P.probe, (P.frames & 1) ? "B1234 (wide)" : "B1234");
  t0 = wall();
  cw_ui_layout(P.tree);
  double incr = wall() - t0;
  P.incr_arranged = (int32_t)cw_ui_get_stats()->arranged;
  P.samples++;
  P.full_sum += full;
  P.incr_sum += incr;
  P.full_ms = (float)(P.full_sum / P.samples * 1000);
  P.incr_ms = (float)(P.incr_sum / P.samples * 1000);
  char st[160];
  /* times stay out of the picture (hosts differ); they are perf.* fields */
  snprintf(st, sizeof st, "%d controls | full relayout of %d each frame | %u draw calls", P.controls, P.full_arranged,
           cw_ui_get_stats()->draw_calls);
  cw_label_set_text(P.status, st);
}

static void render(void *u, double a) {
  (void)u, (void)a;
  int w, h;
  cw_drawable_size(&w, &h);
  (void)w, (void)h;
  cw_gpu_clear_window(0, 0, 0, 1); /* both backends */
  double t0 = wall();
  cw_ui_render(P.tree);
  if (P.frames >= 3) {
    P.draw_sum += wall() - t0;
    P.draw_ms = (float)(P.draw_sum / (P.frames - 2) * 1000);
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){ .title = "ui perf", .width = 1280, .height = 720, .init = init, .update = update,
                        .render = render, .debug = true };
}
