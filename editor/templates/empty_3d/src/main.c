/* Empty 3D (a cartwheel project template): the main scene (main.cwscene)
 * runs; this file is where game logic goes. The cube spins so a running game
 * is visibly running; ui_accept (project.cwproj `input.*`) counts presses.
 *
 * Debug fields: game.ticks, game.presses, cube.rotation, tree.nodes. */
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/project.h"
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
  cw_debug_f32("cube.rotation", &G.rotation);
  cw_debug_i32("tree.nodes", &G.nodes);
}

static void fixed_update(void *user, double dt) {
  (void)user;
  cw_node cube = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Main/Cube");
  G.rotation += (float)dt;
  cw_node_set(cube, "rotation", cw_val_vec3(0, G.rotation, 0));
  if (cw_action_just_pressed(0, accept)) G.presses++;
  G.nodes = cw_tree_node_count(cw_scene_app_tree());
  G.ticks++;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  cw_scene_register_builtin_types();
  static cw_app_desc app = { .width = 960, .height = 540, .debug = true, .init = init, .fixed_update = fixed_update };
  cw_project_apply_app(&app); /* render.* and the name from project.cwproj */
  const char *main_scene = cw_project_get("main_scene");
  return cw_scene_app(main_scene ? main_scene : "main.cwscene", &app);
}
