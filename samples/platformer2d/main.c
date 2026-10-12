/* platformer2d: a 2D platformer built ONLY from .cwscene files (main,
 * player, coin), run by the scene runner. The only C here is the game
 * logic a script would hold: the Player node type (a CharacterBody2D that
 * reads pad 0, runs, jumps and picks its AnimatedSprite2D animation) and
 * the coin's "collect" method (resolved by name from the coin scene's
 * [connection], so Area2D.body_entered -> collect).
 *
 * The level: a TileMap2D (two layers, animated water), a StaticBody2D whose
 * colliders are a CHAIN outline (ground, ramps, end walls), a RECT block
 * and a POLYGON platform; six Area2D coins; a RigidBody2D crate the player
 * pushes; a Camera2D with smoothing and limits following the player; a
 * torch Light2D with an occluder and a lamp on the player over a dim
 * ambient; a parallax background CanvasLayer; a HUD CanvasLayer label.
 *
 * Built twice: `platformer2d` reads the pad (gate input:
 * tests/platformer2d-input.json); `platformer2d_auto` plays the same script
 * from the tick count, so input-free hosts run it too.
 *
 * Debug fields: player.pos, player.vel, player.floor, player.jumps,
 * player.anim, coins, crate.pos, crate.contacts, feet.hit, bodies.hash
 * (every tick's player + crate node transforms folded), phys.hash (the
 * Box2D world's state hash), camera.pos, tree.nodes. */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene_file.h"

#ifdef PLATFORMER_AUTOPILOT
static const struct { int frame; uint32_t buttons; float lx, ly; } SCRIPT[] = {
#include "script.inc"
};
#endif

static struct {
  float pos[2], vel[2], crate[2], cam[2];
  int32_t floor, jumps, coins, contacts, feet, anim, nodes;
  uint32_t bodies_hash, phys_hash;
  int32_t tick;
} G;

/* ── the Player type (what a script would be) ──────────────────────── */

/* #region player_data */
typedef struct player {
  uint32_t prev_buttons; /* the autopilot's buttons last tick: its jump edge */
  float run; /* horizontal run speed: kept apart from the clipped velocity so pushing keeps pushing */
} player;
/* #endregion */

static cw_action jump_action; /* pad South: just_pressed keeps a tap shorter than a tick */

/* the stick, and whether jump went down this tick */
static bool read_input(float *lx, uint32_t *prev_buttons) {
#ifdef PLATFORMER_AUTOPILOT
  int frame = G.tick + 1; /* the script's 1-based engine frame (one tick per frame) */
  uint32_t buttons = 0;
  *lx = 0;
  for (size_t i = 0; i < CW_ARRAY_LEN(SCRIPT) && SCRIPT[i].frame <= frame; i++) {
    *lx = SCRIPT[i].lx;
    buttons = SCRIPT[i].buttons;
  }
  bool jump = (buttons & (1u << CW_PAD_SOUTH)) && !(*prev_buttons & (1u << CW_PAD_SOUTH));
  *prev_buttons = buttons;
  return jump;
#else
  (void)prev_buttons;
  *lx = cw_input_raw()->pads[0].axes[CW_AXIS_LX];
  return cw_action_just_pressed(0, jump_action);
#endif
}

static void player_fixed(cw_node n, void *data, double dt) {
  player *pl = data;
  float lx;
  bool jump = read_input(&lx, &pl->prev_buttons);

  cw_value v;
  cw_node_get_prop(n, "velocity", &v);
  cw_vec2 vel = cw_v2(v.v[0], v.v[1]);
  cw_node_get_prop(n, "on_floor", &v);
  bool on_floor = v.b;
  const float speed = 90, accel = 900, gravity = 900;
  float k = accel * (float)dt;
  pl->run += CW_CLAMP(lx * speed - pl->run, -k, k);
  vel.x = pl->run;
  if (jump && on_floor) {
    vel.y = -360;
    G.jumps++;
  }
  vel.y += gravity * (float)dt;
  cw_node_set(n, "velocity", cw_val_vec2(vel.x, vel.y));
  cw_character2d_move_and_slide(n, (float)dt);

  /* animation */
  cw_node_get_prop(n, "velocity", &v);
  vel = cw_v2(v.v[0], v.v[1]);
  cw_node_get_prop(n, "on_floor", &v);
  on_floor = v.b;
  cw_node sprite = cw_node_get(n, "Sprite");
  const char *anim = on_floor ? (vel.x > 5 || vel.x < -5 ? "run" : "idle") : vel.y < 0 ? "jump" : "fall";
  cw_animated_sprite2d_play(sprite, anim, false);
  if (lx < 0) cw_node_set(sprite, "flip_h", cw_val_bool(true));
  else if (lx > 0) cw_node_set(sprite, "flip_h", cw_val_bool(false));
  G.anim = anim[0] == 'i' ? 0 : anim[0] == 'r' ? 1 : anim[0] == 'j' ? 2 : 3;
}

/* #region player_type */
static cw_node_type player_type = { .name = "Player", .parent = "CharacterBody2D", .data_size = sizeof(player),
                                    .doc = "platformer2d's player: pad 0 runs and jumps.", .fixed_update = player_fixed };
/* #endregion */

/* ── coins: Area2D.body_entered -> collect ──────────────────────────── */

/* #region collect */
static void collect(cw_node coin, const cw_value *args, int argc, void *user) {
  (void)user;
  if (argc < 1 || !cw_node_is(args[0].node, "Player")) return;
  G.coins++;
  cw_node_free(coin); /* deferred: physics signals are delivered as a batch */
  cw_node label = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level/HUD/Coins");
  char text[32];
  snprintf(text, sizeof text, "COINS %d", (int)G.coins);
  cw_node_set(label, "text", cw_val_string(text));
}

static cw_signal_fn resolve(cw_node target, const char *method, void **user) {
  (void)target;
  *user = NULL;
  return !strcmp(method, "collect") ? collect : NULL;
}
/* #endregion */

/* ── app ────────────────────────────────────────────────────────────── */

static void crate_contact(cw_node crate, const cw_value *args, int argc, void *user) {
  (void)crate;
  (void)args;
  (void)argc;
  (void)user;
  G.contacts++;
}

static void init(void *u) {
  (void)u;
  jump_action = cw_action_add("jump", 0.5f);
  cw_action_bind_text(jump_action, "pad:south");
  cw_debug_f32_array("player.pos", G.pos, 2);
  cw_debug_f32_array("player.vel", G.vel, 2);
  cw_debug_i32("player.floor", &G.floor);
  cw_debug_i32("player.jumps", &G.jumps);
  cw_debug_i32("player.anim", &G.anim);
  cw_debug_i32("coins", &G.coins);
  cw_debug_f32_array("crate.pos", G.crate, 2);
  cw_debug_i32("crate.contacts", &G.contacts);
  cw_debug_i32("feet.hit", &G.feet);
  cw_debug_u32("bodies.hash", &G.bodies_hash);
  cw_debug_u32("phys.hash", &G.phys_hash);
  cw_debug_f32_array("camera.pos", G.cam, 2);
  G.bodies_hash = CW_HASH32_INIT;
  cw_node crate = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level/Crate");
  /* #region connect */
  cw_connect(crate, "body_entered", crate, NULL, crate_contact, NULL, 0);
  /* #endregion */
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  cw_tree *t = cw_scene_app_tree();
  cw_node root = cw_tree_root(t);
  cw_node p = cw_node_get(root, "Level/Player"), crate = cw_node_get(root, "Level/Crate");
  cw_vec2 pp = cw_node2d_position(p), cp = cw_node2d_position(crate);
  G.pos[0] = pp.x;
  G.pos[1] = pp.y;
  G.crate[0] = cp.x;
  G.crate[1] = cp.y;
  cw_value v;
  cw_node_get_prop(p, "velocity", &v);
  G.vel[0] = v.v[0];
  G.vel[1] = v.v[1];
  cw_node_get_prop(p, "on_floor", &v);
  G.floor = v.b;
  cw_node_get_prop(cw_node_get(p, "Feet"), "colliding", &v);
  G.feet = v.b;
  float rot = cw_node2d_rotation(crate);
  G.bodies_hash = cw_hash32(G.pos, sizeof G.pos, G.bodies_hash);
  G.bodies_hash = cw_hash32(G.crate, sizeof G.crate, G.bodies_hash);
  G.bodies_hash = cw_hash32(&rot, sizeof rot, G.bodies_hash);
  G.phys_hash = cw_world2d_stats(cw_physics2d_world(t)).hash;
  cw_camera2d cam = cw_canvas_camera_value(t);
  G.cam[0] = cam.position.x;
  G.cam[1] = cam.position.y;
  if (getenv("PLAT_TRACE")) cw_info("t=%d pos=%.1f,%.1f vel=%.1f,%.1f floor=%d coins=%d crate=%.1f,%.1f", G.tick, G.pos[0], G.pos[1], G.vel[0], G.vel[1], G.floor, G.coins, G.crate[0], G.crate[1]);
  G.tick++;
}

/* #region main */
/* Every node type this game defines and nothing else (no scene, no state):
 * the editor calls it to open the game's scenes, cwhot after a reload. */
void cw_game_register_types(void);
void cw_game_register_types(void) { cw_node_type_register(&player_type); }

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  cw_scene_register_builtin_types();
  cw_game_register_types();
  cw_set_method_resolver(resolve);
  static cw_app_desc base = { .title = "platformer2d", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
/* #endregion */
