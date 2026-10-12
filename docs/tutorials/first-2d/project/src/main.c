/* Gem Garden: the first 2D tutorial's game (docs/tutorials/first-2d).
 *
 * A top-down collect-and-avoid game. The level, the gems and the thorns are
 * scenes (main.cwscene, gem.cwscene, thorn.cwscene) made of built-in node
 * types; this file is the game logic. The player (the CharacterBody2D at
 * Main/Player) walks with the project's input map, a gem (group "gems") is
 * collected when the player enters it, a thorn (group "thorns") sends the
 * player back to the start, and collecting every gem wins.
 *
 * Debug fields: game.ticks, player.pos, gems, hits, won, tree.nodes. */
#include <stdio.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/project.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene_file.h"

/* #region state */
static struct {
  int32_t ticks, gems, total, hits, won, nodes;
  float pos[2];
} G;
static cw_action left, right, up, down;
static cw_vec2 start; /* where a thorn sends the player back to */
/* #endregion */

static cw_node main_node(void) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), "Main"); }
static cw_node player(void) { return cw_node_get(main_node(), "Player"); }

/* #region hud */
static void update_hud(void) {
  char text[32];
  snprintf(text, sizeof text, "GEMS %d/%d", (int)G.gems, (int)G.total);
  cw_node_set(cw_node_get(main_node(), "HUD/Gems"), "text", cw_val_string(text));
  if (G.won) cw_node_set(cw_node_get(main_node(), "HUD/Message"), "text", cw_val_string("ALL GEMS! YOU WIN"));
}
/* #endregion */

/* #region signals */
static void on_gem(cw_node gem, const cw_value *args, int argc, void *user) {
  (void)user;
  if (argc < 1 || args[0].node.id != player().id) return;
  G.gems++;
  cw_node_free(gem); /* deferred: physics signals arrive as a batch */
  if (G.gems == G.total) G.won = 1;
  update_hud();
}

static void on_thorn(cw_node thorn, const cw_value *args, int argc, void *user) {
  (void)thorn, (void)user;
  if (argc < 1 || args[0].node.id != player().id || G.won) return;
  G.hits++;
  cw_node2d_set_position(player(), start); /* back to the start */
}
/* #endregion */

static void init(void *user) {
  (void)user;
  /* #region init_input */
  cw_project_load_input();
  left = cw_action_find("move_left");
  right = cw_action_find("move_right");
  up = cw_action_find("move_up");
  down = cw_action_find("move_down");
  /* #endregion */
  /* #region init_signals */
  cw_tree *tree = cw_scene_app_tree();
  cw_node *nodes;
  int n = cw_tree_group(tree, "gems", &nodes);
  for (int i = 0; i < n; i++) cw_connect(nodes[i], "body_entered", nodes[i], NULL, on_gem, NULL, 0);
  G.total = n;
  n = cw_tree_group(tree, "thorns", &nodes);
  for (int i = 0; i < n; i++) cw_connect(nodes[i], "body_entered", nodes[i], NULL, on_thorn, NULL, 0);
  start = cw_node2d_position(player());
  update_hud();
  /* #endregion */
  /* #region init_debug */
  cw_debug_i32("game.ticks", &G.ticks);
  cw_debug_f32_array("player.pos", G.pos, 2);
  cw_debug_i32("gems", &G.gems);
  cw_debug_i32("hits", &G.hits);
  cw_debug_i32("won", &G.won);
  cw_debug_i32("tree.nodes", &G.nodes);
  /* #endregion */
}

static void fixed_update(void *user, double dt) {
  (void)user;
  /* #region move */
  const float speed = 90; /* pixels per second */
  cw_vec2 dir = cw_input_vector(0, left, right, up, down, CW_DEADZONE_RADIAL, 0.2f);
  if (G.won) dir = cw_v2(0, 0); /* the game is over: stand still */
  cw_node p = player();
  cw_node_set(p, "velocity", cw_val_vec2(dir.x * speed, dir.y * speed));
  cw_character2d_move_and_slide(p, (float)dt);
  /* #endregion */

  /* #region patrol */
  /* Thorn2 walks up and down the bottom corridor: a triangle wave over
   * 160 ticks, computed from the tick count so every host agrees. */
  cw_node thorn = cw_node_get(main_node(), "Thorn2");
  int phase = G.ticks % 160;
  float t = (phase < 80 ? phase : 160 - phase) / 80.0f;
  cw_node2d_set_position(thorn, cw_v2(100, 128 + t * 36));
  /* #endregion */

  /* #region debug */
  cw_vec2 pos = cw_node2d_position(p);
  G.pos[0] = pos.x;
  G.pos[1] = pos.y;
  G.nodes = cw_tree_node_count(cw_scene_app_tree());
  G.ticks++;
  /* #endregion */
}

/* #region main */
cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  cw_scene_register_builtin_types();
  static cw_app_desc app = { .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed_update };
  cw_project_apply_app(&app); /* render.* and the name from project.cwproj */
  const char *main_scene = cw_project_get("main_scene");
  return cw_scene_app(main_scene ? main_scene : "main.cwscene", &app);
}
/* #endregion */
