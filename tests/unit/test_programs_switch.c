/* A scene whose content depends on the graphics preset it was built on, as a game's does (more lights, a denser
 * scatter on High): at tick 5 it adds two shadow-casting omni lights when the preset is High or above, none below.
 * A preset changed in play then draws High's content with another preset's renderer state (on Very Low the omni
 * lights cast no shadow and the materials take the plain lighting model): a program state that no run started on
 * a single preset asks for. tests/programs-switch.mjs builds it twice with two tables:
 *   unit_programs_switch         its table surveyed by tools/programs.mjs as it ships (each preset, then the
 *                                preset switched half way through a run, both ways)
 *   unit_programs_switch_start   the same scene, its table surveyed with --no-switch (each preset from the start)
 * Debug state: switch.omni (omni lights added), cw.three.missing (the pipeline's missing states). */
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/quality.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene_file.h"

static int tick, omni;

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  if (++tick != 5) return;
  if (cw_quality_get() < CW_QUALITY_HIGH) return;
  cw_node level = cw_tree_root(cw_scene_app_tree());
  for (int i = 0; i < 2; i++) {
    cw_node l = cw_node_new("OmniLight3D", i ? "LampB" : "LampA");
    cw_node_add_child(level, l);
    cw_node3d_set_position(l, (t3_vec3){ i ? 2.0f : -2.0f, 2.0f, 1.0f });
    cw_node_set(l, "shadow", cw_val_bool(true));
    cw_node_set(l, "energy", cw_val_float(3.0f));
    omni++;
  }
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("switch.omni", &omni);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "programs switch", .width = 320, .height = 180, .debug = true, .init = init,
                              .fixed_update = fixed_update };
  return cw_scene_app("main.cwscene", &base);
}
