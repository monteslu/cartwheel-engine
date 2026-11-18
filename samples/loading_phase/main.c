/* loading_phase: a level loaded in play under the loading screen (cw_loading_begin). The debug request `load <n>` starts
 * a phase whose step adds one piece of the new level a frame (n pieces, each a few milliseconds of work), reporting
 * cw_loading_set; the scene runner's shader phase follows. While it loads no tick runs and the bar never steps back.
 * Debug state: lp.tick (ticks run), lp.steps (step calls), lp.pieces (pieces built), lp.loading (cw_loading_active),
 * lp.ticks_loading (ticks run while loading: always 0), lp.back (times the bar stepped back: always 0), lp.done
 * (phases finished: the frame path saw cw_loading_active go false after a begin), lp.frames (frames drawn while loading).
 * LP_LOAD_AT=<tick> LP_LOAD_N=<n> start the load at that tick by itself (tests/loading-phase.mjs); LP_SYNC=1 builds the
 * level in that tick instead, all at once with no loading phase (the frozen frame the phase replaces: the gate's control). */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene_file.h"

static int32_t ticks, steps, pieces, want, loading, ticks_loading, back, done, frames, last_bar = -1, first_bar = -1;
static cw_node level;

static bool load_step(void *user) {
  (void)user;
  steps++;
  if (pieces >= want) return true;
  /* one piece: a row of shapes (enough work that a real level would take seconds in one frame) */
  static const char *const PRIM[3] = { "BOX", "SPHERE", "CYLINDER" };
  for (int i = 0; i < 6; i++) {
    char name[32];
    snprintf(name, sizeof name, "P%d_%d", pieces, i);
    cw_node m = cw_node_new("MeshInstance3D", name);
    cw_node_set(m, "primitive", cw_val_enum(PRIM[(pieces + i) % 3]));
    cw_node3d_set_position(m, (t3_vec3){ -12.5f + 5.0f * (float)i, 0.5f, -12.5f + 2.0f * (float)pieces });
    cw_node_add_child(level, m);
  }
  pieces++;
  cw_loading_set(pieces, want, CW_LOAD_SCENE);
  return pieces >= want;
}

static void start_load(int n, bool sync) {
  want = n < 1 ? 1 : n;
  pieces = steps = frames = 0;
  first_bar = -1;
  last_bar = -1;
  if (cw_node_valid(level)) cw_node_free(level);
  level = cw_node_new("Node3D", "Level");
  cw_node_add_child(cw_tree_root(cw_scene_app_tree()), level);
  if (sync)
    while (!load_step(NULL)) {}
  else cw_loading_begin(load_step, NULL);
}

static void cmd_load(int argc, const char **argv, cw_reply *r) {
  if (argc > 1 && atoi(argv[1]) == 0) { /* load 0: the shader phase alone (no step), over the scene as it is */
    first_bar = last_bar = -1, frames = 0, want = 1;
    cw_loading_begin(NULL, NULL);
    cw_reply_printf(r, "{\"load\":0}");
    return;
  }
  start_load(argc > 1 ? atoi(argv[1]) : 8, false);
  cw_reply_printf(r, "{\"load\":%d}", (int)want);
}

static void each_frame(cw_raw_input *in, uint64_t frame, void *user);
static void init(void *user) {
  (void)user;
  cw_debug_i32("lp.tick", &ticks);
  cw_debug_i32("lp.steps", &steps);
  cw_debug_i32("lp.pieces", &pieces);
  cw_debug_i32("lp.loading", &loading);
  cw_debug_i32("lp.ticks_loading", &ticks_loading);
  cw_debug_i32("lp.back", &back);
  cw_debug_i32("lp.done", &done);
  cw_debug_i32("lp.frames", &frames);
  cw_debug_i32("lp.first_bar", &first_bar); /* the bar (thousandths) on the first frame a phase showed */
  cw_set_raw_input_filter(each_frame, NULL);
  cw_debug_command("load", "load [n]: load a level of n pieces in play under the loading screen", cmd_load);
}

static void fixed(void *user, double dt) {
  (void)user, (void)dt;
  ticks++;
  if (cw_loading_active()) ticks_loading++;
  const char *at = getenv("LP_LOAD_AT");
  if (at && ticks == atoi(at)) start_load(getenv("LP_LOAD_N") ? atoi(getenv("LP_LOAD_N")) : 8, getenv("LP_SYNC") != NULL);
}

/* every frame (the raw input filter runs on loading frames too, where no tick or render of the app does): the bar's
 * fraction (in thousandths) must not go down while a phase loads */
static void each_frame(cw_raw_input *in, uint64_t frame, void *user) {
  (void)in, (void)frame, (void)user;
  bool was = loading;
  loading = cw_loading_active();
  if (loading) {
    int d, t;
    cw_loading_get(&d, &t, NULL);
    int bar = t > 0 ? (int)(1000LL * d / t) : 0;
    if (last_bar >= 0 && bar < last_bar) back++;
    if (first_bar < 0) first_bar = bar;
    frames++;
    last_bar = bar;
  } else if (was && want > 0) done++;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  cw_scene_register_builtin_types();
  static cw_app_desc base_app = { .title = "loading_phase", .width = 480, .height = 270, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base_app);
}
