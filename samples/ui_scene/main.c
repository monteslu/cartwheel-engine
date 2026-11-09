/* ui_scene: the gallery scene run by the engine's scene runner with no UI
 * code at all: Control nodes from a .cwscene are laid out, take input and
 * draw as the tree's canvas overlay pass (scene2d.h). The gate's control
 * run switches to the light theme (CW_UI_SCENE_LIGHT=1, from frame 2) and must differ. */
#include <stdlib.h>

#include "cartwheel/platform.h"
#include "cartwheel/debug.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/ui.h"

static void init(void *u) {
  (void)u;
  cw_ui_debug_register("ui");
}

/* the gate's control (CW_UI_SCENE_LIGHT=1: the light theme) from frame 2, when every host has it (cw_test_env) */
static void update(void *u, double dt) {
  (void)u, (void)dt;
  static bool done;
  if (done || cw_frame_index() < 1) return;
  done = true;
  if (cw_test_env("CW_UI_SCENE_LIGHT")) cw_ui_set_theme(cw_theme_default_light());
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return cw_scene_app("scenes/gallery.cwscene", &(cw_app_desc){ .title = "ui scene", .width = 1280, .height = 720,
                                                                 .debug = true, .init = init, .update = update });
}
