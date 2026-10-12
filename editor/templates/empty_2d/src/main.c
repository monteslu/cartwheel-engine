/* Empty 2D (a cartwheel project template): the main scene (main.cwscene)
 * runs; this file is where game logic goes. The marker spins so a running
 * game is visibly running; ui_accept (project.cwproj `input.*`) counts presses.
 *
 * Debug fields: game.ticks, game.presses, marker.rotation, tree.nodes. */
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/project.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene_file.h"

static struct {
  int32_t ticks, presses, nodes;
  float rotation;
} G;
static cw_action accept;

static void init(void *user) {
  (void)user;
  cw_project_load_input();
  accept = cw_action_find("ui_accept");
  cw_debug_i32("game.ticks", &G.ticks);
  cw_debug_i32("game.presses", &G.presses);
  cw_debug_f32("marker.rotation", &G.rotation);
  cw_debug_i32("tree.nodes", &G.nodes);
}

static void fixed_update(void *user, double dt) {
  (void)user;
  cw_node marker = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Main/Marker");
  G.rotation += (float)dt;
  cw_node_set(marker, "rotation", cw_val_float(G.rotation));
  if (cw_action_just_pressed(0, accept)) G.presses++;
  G.nodes = cw_tree_node_count(cw_scene_app_tree());
  G.ticks++;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  cw_scene_register_builtin_types();
  static cw_app_desc app = { .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed_update };
  cw_project_apply_app(&app); /* render.* and the name from project.cwproj */
  const char *main_scene = cw_project_get("main_scene");
  return cw_scene_app(main_scene ? main_scene : "main.cwscene", &app);
}
