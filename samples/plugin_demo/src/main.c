/* Plugin Demo (a cartwheel project with a plugin): the main scene uses the
 * Turntable node type, which no engine file defines: it comes from the
 * project's plugin plugins/turntable/ (runtime part compiled in by the
 * project build, editor part loaded by the editor). The game itself only
 * runs the scene and publishes what the gates read.
 *
 * Debug fields: game.ticks, game.presses, table.angle, table.turns,
 * table.found (1 when Main/Table is a Turntable), tree.nodes. Space / Enter /
 * pad south reverses the turntable (through its `reverse` method). */
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/project.h"
#include "cartwheel/scene_file.h"

static struct {
  int32_t ticks, presses, nodes, found, turns;
  float angle, speed;
} G;
static cw_action accept;

static void init(void *user) {
  (void)user;
  cw_project_load_input();
  accept = cw_action_find("ui_accept");
  cw_debug_i32("game.ticks", &G.ticks);
  cw_debug_i32("game.presses", &G.presses);
  cw_debug_f32("table.angle", &G.angle);
  cw_debug_f32("table.speed", &G.speed);
  cw_debug_i32("table.turns", &G.turns);
  cw_debug_i32("table.found", &G.found);
  cw_debug_i32("tree.nodes", &G.nodes);
}

static void fixed_update(void *user, double dt) {
  (void)user, (void)dt;
  cw_node table = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Main/Table");
  G.found = cw_node_valid(table) && cw_node_is(table, "Turntable");
  if (cw_action_just_pressed(0, accept)) {
    G.presses++;
    cw_value ret;
    if (G.found) cw_node_call(table, "reverse", NULL, 0, &ret);
  }
  cw_value v;
  if (G.found && cw_node_get_prop(table, "angle", &v)) G.angle = (float)v.f;
  if (G.found && cw_node_get_prop(table, "speed", &v)) G.speed = (float)v.f;
  if (G.found && cw_node_get_prop(table, "turns", &v)) G.turns = (int32_t)v.i;
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
