/* 2D Platformer (a cartwheel project template).
 *
 * The level, the player and the coins are scenes (main.cwscene,
 * coin.cwscene) made only of built-in node types, so the editor opens and
 * edits all of them. The game logic is this file: the player (the
 * CharacterBody2D at Level/Player) runs and jumps with the actions of the
 * project's input map (project.cwproj `input.*`), and coins (group "coins")
 * are collected when the player enters them; falling off the level puts the
 * player back at the start.
 *
 * Debug fields (the debug channel; romdev's wasm({op:'debugState'}), the
 * editor's play mode, tests): game.ticks, player.pos, player.on_floor,
 * player.jumps, coins, player.respawns, tree.nodes. */
#include <stdio.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/project.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene_file.h"

static struct {
  int32_t ticks, on_floor, jumps, coins, respawns, nodes;
  float pos[2];
} G;
static cw_action move_left, move_right, jump;
static cw_vec2 start; /* where the player respawns */

static cw_node level(void) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level"); }

static void collect(cw_node coin, const cw_value *args, int argc, void *user) {
  (void)user;
  if (argc < 1 || args[0].node.id != cw_node_get(level(), "Player").id) return;
  G.coins++;
  cw_node_free(coin); /* deferred: physics signals arrive as a batch */
  char text[32];
  snprintf(text, sizeof text, "COINS %d", (int)G.coins);
  cw_node_set(cw_node_get(level(), "HUD/Coins"), "text", cw_val_string(text));
}

static void init(void *user) {
  (void)user;
  cw_project_load_input();
  move_left = cw_action_find("move_left");
  move_right = cw_action_find("move_right");
  jump = cw_action_find("jump");
  cw_node *coins;
  int n = cw_tree_group(cw_scene_app_tree(), "coins", &coins);
  for (int i = 0; i < n; i++) cw_connect(coins[i], "body_entered", coins[i], NULL, collect, NULL, 0);
  cw_debug_i32("game.ticks", &G.ticks);
  cw_debug_f32_array("player.pos", G.pos, 2);
  cw_debug_i32("player.on_floor", &G.on_floor);
  cw_debug_i32("player.jumps", &G.jumps);
  cw_debug_i32("coins", &G.coins);
  cw_debug_i32("player.respawns", &G.respawns);
  start = cw_node2d_position(cw_node_get(level(), "Player"));
  cw_debug_i32("tree.nodes", &G.nodes);
}

static void fixed_update(void *user, double dt) {
  (void)user;
  cw_node player = cw_node_get(level(), "Player");
  cw_value v;
  cw_node_get_prop(player, "velocity", &v);
  cw_vec2 vel = cw_v2(v.v[0], v.v[1]);
  cw_node_get_prop(player, "on_floor", &v);
  bool on_floor = v.b;
  const float speed = 110, accel = 1000, gravity = 900, jump_speed = 330;
  float target = cw_input_axis(0, move_left, move_right) * speed, k = accel * (float)dt;
  vel.x += CW_CLAMP(target - vel.x, -k, k);
  if (on_floor && cw_action_just_pressed(0, jump)) {
    vel.y = -jump_speed;
    G.jumps++;
  }
  vel.y += gravity * (float)dt;
  cw_node_set(player, "velocity", cw_val_vec2(vel.x, vel.y));
  cw_character2d_move_and_slide(player, (float)dt);

  cw_vec2 p = cw_node2d_position(player);
  if (p.y > 400) { /* fell off the level: back to the start */
    p = start;
    cw_node2d_set_position(player, p);
    cw_node_set(player, "velocity", cw_val_vec2(0, 0));
    G.respawns++;
  }
  G.pos[0] = p.x;
  G.pos[1] = p.y;
  cw_node_get_prop(player, "on_floor", &v);
  G.on_floor = v.b;
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
