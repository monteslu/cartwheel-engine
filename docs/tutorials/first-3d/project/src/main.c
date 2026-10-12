/* Crate Push: the first 3D tutorial's game (docs/tutorials/first-3d).
 *
 * Push the crate onto the glowing pad. The scene (main.cwscene) is built-in
 * node types only; this file is the game logic. The player (the
 * CharacterBody3D at Main/Player) walks on the XZ plane with the project's
 * input map and shoves the crate (a RigidBody3D) with its push_mass; the
 * camera follows the player; the goal (an Area3D) reports the crate
 * entering it, which wins. A crate pushed off the floor goes back to its
 * start.
 *
 * Debug fields: game.ticks, player.pos, crate.pos, crate.resets, won,
 * tree.nodes. */
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/project.h"
#include "cartwheel/scene_file.h"

/* #region state */
static struct {
  int32_t ticks, resets, won, nodes;
  float player[3], crate[3];
} G;
static cw_action left, right, forward, back;
static t3_vec3 crate_start, camera_pos;
/* #endregion */

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

/* #region goal */
static void on_goal(cw_node goal, const cw_value *args, int argc, void *user) {
  (void)goal, (void)user;
  if (argc < 1 || args[0].node.id != node("Main/Crate").id || G.won) return;
  G.won = 1;
  cw_node_set(node("Main/HUD/Status"), "text", cw_val_string("CRATE HOME! YOU WIN"));
  cw_node_set(node("Main/Goal/Lamp"), "color", cw_val_color(0.3f, 1, 0.4f, 1));
  cw_node_set(node("Main/Goal/Pad"), "emissive", cw_val_color(0.1f, 0.9f, 0.2f, 1));
}
/* #endregion */

static void init(void *user) {
  (void)user;
  cw_project_load_input();
  left = cw_action_find("move_left");
  right = cw_action_find("move_right");
  forward = cw_action_find("move_forward");
  back = cw_action_find("move_back");
  /* #region init */
  cw_connect(node("Main/Goal"), "body_entered", node("Main/Goal"), NULL, on_goal, NULL, 0);
  crate_start = cw_node3d_position(node("Main/Crate"));
  camera_pos = cw_node3d_position(node("Main/Camera"));
  /* #endregion */
  /* #region init_debug */
  cw_debug_i32("game.ticks", &G.ticks);
  cw_debug_f32_array("player.pos", G.player, 3);
  cw_debug_f32_array("crate.pos", G.crate, 3);
  cw_debug_i32("crate.resets", &G.resets);
  cw_debug_i32("won", &G.won);
  cw_debug_i32("tree.nodes", &G.nodes);
  /* #endregion */
}

static void fixed_update(void *user, double dt) {
  (void)user;
  /* #region move */
  cw_node player = node("Main/Player");
  cw_value v;
  cw_node_get_prop(player, "velocity", &v);
  float vx = v.v[0], vy = v.v[1], vz = v.v[2];
  const float speed = 3.5f, accel = 30, gravity = 20;
  float tx = cw_input_axis(0, left, right) * speed, tz = cw_input_axis(0, forward, back) * speed;
  if (G.won) tx = tz = 0; /* the game is over: stand still */
  float k = accel * (float)dt;
  vx += CW_CLAMP(tx - vx, -k, k);
  vz += CW_CLAMP(tz - vz, -k, k);
  vy -= gravity * (float)dt;
  cw_node_set(player, "velocity", cw_val_vec3(vx, vy, vz));
  cw_character3d_move_and_slide(player, (float)dt);
  /* #endregion */

  /* #region camera */
  /* The camera eases toward a point above, behind and to the right of the player
   * (the scene gives it a matching yaw). */
  t3_vec3 p = cw_node3d_position(player);
  t3_vec3 want = { p.x + 2, p.y + 4.5f, p.z + 6.5f };
  float ease = 4 * (float)dt;
  camera_pos.x += (want.x - camera_pos.x) * ease;
  camera_pos.y += (want.y - camera_pos.y) * ease;
  camera_pos.z += (want.z - camera_pos.z) * ease;
  cw_node3d_set_position(node("Main/Camera"), camera_pos);
  /* #endregion */

  /* #region reset */
  cw_node crate = node("Main/Crate");
  t3_vec3 c = cw_node3d_position(crate);
  if (c.y < -5) { /* off the edge: setting the position teleports the body */
    cw_node3d_set_position(crate, crate_start);
    cw_node_set(crate, "linear_velocity", cw_val_vec3(0, 0, 0));
    cw_node_set(crate, "angular_velocity", cw_val_vec3(0, 0, 0));
    G.resets++;
  }
  /* #endregion */

  /* #region debug */
  G.player[0] = p.x, G.player[1] = p.y, G.player[2] = p.z;
  G.crate[0] = c.x, G.crate[1] = c.y, G.crate[2] = c.z;
  G.nodes = cw_tree_node_count(cw_scene_app_tree());
  G.ticks++;
  /* #endregion */
}

/* #region main */
cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  cw_scene_register_builtin_types();
  static cw_app_desc app = { .width = 960, .height = 540, .debug = true, .init = init, .fixed_update = fixed_update };
  cw_project_apply_app(&app); /* render.* and the name from project.cwproj */
  const char *main_scene = cw_project_get("main_scene");
  return cw_scene_app(main_scene ? main_scene : "main.cwscene", &app);
}
/* #endregion */
