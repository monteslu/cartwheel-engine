/* warm_states: the warm-up's shader phase must draw only the program states play draws. A scene with a cascaded sun, a
 * probe grid and many meshes over a few materials is revealed a slice at a time under the loading screen, several slices
 * a frame (most reveal no new program, so they are quick); every slice is a pipeline render of its own, and each takes
 * what the tree registers before it. When only a frame's first slice had that (the cascaded sun), the later ones drew the
 * sun as a plain shadowed light: states play never asks for, so a table surveyed in play (or in a deterministic run,
 * which has no slices) lacked them, and a player's first frames showed the red "no program" banner for good.
 * Control CW_TEST_WARM_NO_PRERENDER=1 (src/scene/runner.c): the later slices without their pre-render, as before.
 * Debug state: warm.tick. */
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static int32_t ticks;

static void init(void *user) {
  (void)user;
  cw_debug_i32("warm.tick", &ticks);
}

static void fixed(void *user, double dt) {
  (void)user, (void)dt;
  ticks++;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  cw_scene_register_builtin_types();
  static cw_app_desc base_app = { .title = "warm_states", .width = 480, .height = 270, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base_app);
}
