/* 3D Platformer (a cartwheel project template).
 *
 * The level, the player and the gems are scenes (main.cwscene, gem.cwscene)
 * made only of built-in node types, so the editor opens and edits all of
 * them. The game logic is this file: the player (the CharacterBody3D at
 * Level/Player) runs on the XZ plane and jumps with the actions of the
 * project's input map (project.cwproj `input.*`), the camera follows it, and
 * gems (group "gems") are collected when the player enters them; falling
 * off the level puts the player back at the start.
 *
 * Debug fields: game.ticks, player.pos, player.on_floor, player.jumps, gems,
 * player.respawns, tree.nodes. */
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/project.h"
#include "cartwheel/scene_file.h"

static struct {
  int32_t ticks, on_floor, jumps, gems, respawns, nodes;
  float pos[3];
} G;
static cw_action left, right, forward, back, jump;
static t3_vec3 start; /* where the player respawns */

static cw_node level(void) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level"); }

static void collect(cw_node gem, const cw_value *args, int argc, void *user) {
  (void)user;
  if (argc < 1 || args[0].node.id != cw_node_get(level(), "Player").id) return;
  G.gems++;
  cw_node_free(gem); /* deferred: physics signals arrive as a batch */
}

static void init(void *user) {
  (void)user;
  cw_project_load_input();
  left = cw_action_find("move_left");
  right = cw_action_find("move_right");
  forward = cw_action_find("move_forward");
  back = cw_action_find("move_back");
  jump = cw_action_find("jump");
  cw_node *gems;
  int n = cw_tree_group(cw_scene_app_tree(), "gems", &gems);
  for (int i = 0; i < n; i++) cw_connect(gems[i], "body_entered", gems[i], NULL, collect, NULL, 0);
  cw_debug_i32("game.ticks", &G.ticks);
  cw_debug_f32_array("player.pos", G.pos, 3);
  cw_debug_i32("player.on_floor", &G.on_floor);
  cw_debug_i32("player.jumps", &G.jumps);
  cw_debug_i32("gems", &G.gems);
  cw_debug_i32("player.respawns", &G.respawns);
  start = cw_node3d_position(cw_node_get(level(), "Player"));
  cw_debug_i32("tree.nodes", &G.nodes);
}

static void fixed_update(void *user, double dt) {
  (void)user;
  cw_node player = cw_node_get(level(), "Player");
  cw_value v;
  cw_node_get_prop(player, "velocity", &v);
  float vx = v.v[0], vy = v.v[1], vz = v.v[2];
  cw_node_get_prop(player, "on_floor", &v);
  bool on_floor = v.b;
  const float speed = 5, accel = 40, gravity = 20, jump_speed = 7.5f;
  float tx = cw_input_axis(0, left, right) * speed, tz = cw_input_axis(0, forward, back) * speed;
  float k = accel * (float)dt;
  vx += CW_CLAMP(tx - vx, -k, k);
  vz += CW_CLAMP(tz - vz, -k, k);
  if (on_floor && cw_action_just_pressed(0, jump)) {
    vy = jump_speed;
    G.jumps++;
  }
  vy -= gravity * (float)dt;
  cw_node_set(player, "velocity", cw_val_vec3(vx, vy, vz));
  cw_character3d_move_and_slide(player, (float)dt);

  t3_vec3 p = cw_node3d_position(player);
  if (p.y < -10) { /* fell off the level: back to the start */
    p = start;
    cw_node3d_set_position(player, p);
    cw_node_set(player, "velocity", cw_val_vec3(0, 0, 0));
    G.respawns++;
  }
  G.pos[0] = p.x, G.pos[1] = p.y, G.pos[2] = p.z;
  /* the camera keeps its offset from the player */
  cw_node3d_set_position(cw_node_get(level(), "Camera"), (t3_vec3){ p.x, p.y + 3.5f, p.z + 7 });
  cw_node_get_prop(player, "on_floor", &v);
  G.on_floor = v.b;
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
