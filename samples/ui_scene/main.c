/* ui_scene: the gallery scene run by the engine's scene runner with no UI
 * code at all: Control nodes from a .cwscene are laid out, take input and
 * draw as the tree's canvas overlay pass (scene2d.h). The gate's control
 * run switches to the light theme (CW_UI_SCENE_LIGHT=1) and must differ. */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/ui.h"

static void init(void *u) {
  (void)u;
  if (getenv("CW_UI_SCENE_LIGHT")) cw_ui_set_theme(cw_theme_default_light());
  cw_ui_debug_register("ui");
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return cw_scene_app("scenes/gallery.cwscene", &(cw_app_desc){ .title = "ui scene", .width = 1280, .height = 720,
                                                                 .debug = true, .init = init });
}
