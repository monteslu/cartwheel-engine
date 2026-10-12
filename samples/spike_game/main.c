/* spike_game: "spawn, move, collide, score" in a 3D arena.
 *
 * The player is a character mover (capsule) driven by the left stick, the
 * d-pad or the arrow keys. Coins spawn at seeded random spots every
 * COIN_INTERVAL seconds; walking into one collects it (score +1). Crates are
 * dynamic Box3D bodies the mover pushes. After ROUND_SECONDS the round ends:
 * movement stops and a game-over banner shows.
 *
 * Scene nodes (Camera3D, MeshInstance3D, lights, WorldEnvironment) do the
 * drawing; a cw_world3d does the physics; game state is plain C and is
 * published as named debug fields for romdev. */
#include <stdio.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/physics.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene.h"

#define ARENA_HALF 9.0f
#define MAX_COINS 8
#define NUM_CRATES 4
#define ROUND_SECONDS 60.0f
#define COIN_INTERVAL 1.5f
#define PLAYER_SPEED 5.0f
#define COIN_PICK_RADIUS 0.9f

typedef struct coin {
  cw_node node;
  int32_t active;
  float x, z;
} coin;

static struct game {
  cw_tree *tree;
  cw_world3d world;
  cw_mover3d mover;
  cw_node player, camera;
  cw_body3d crates[NUM_CRATES];
  cw_node crate_nodes[NUM_CRATES];
  coin coins[MAX_COINS];
  cw_rng rng;
  cw_action left, right, up, down;
  cw_font font;
  float spawn_timer, spin;

  /* debug fields (published by name) */
  int32_t score, game_over, coins_active, coins_spawned, ticks;
  float time_left;
  float player_pos[3];
  float crate0_pos[3];
  float input_x, input_y;
  float coin_xz[MAX_COINS * 2]; /* active coins' x,z; inactive = (99, 99) */
} G;

static cw_node add_mesh(cw_node parent, const char *name, const char *primitive, t3_vec3 size, cw_value color,
                        t3_vec3 pos) {
  cw_node n = cw_node_new("MeshInstance3D", name);
  const cw_prop *pp = cw_node_find_prop(n, "primitive", NULL);
  cw_value prim = { CW_T_ENUM };
  for (int k = 0; pp && pp->enum_names && pp->enum_names[k]; k++)
    if (!strcmp(pp->enum_names[k], primitive)) prim.i = k;
  if (!cw_node_set(n, "primitive", prim)) cw_warn("%s: primitive %s refused", name, primitive);
  cw_node_set(n, "size", cw_val_vec3(size.x, size.y, size.z));
  cw_node_set(n, "color", color);
  cw_node_set(n, "cast_shadow", cw_val_bool(true));
  cw_node_set(n, "receive_shadow", cw_val_bool(true));
  cw_node3d_set_position(n, pos);
  cw_node_add_child(parent, n);
  return n;
}

static void static_box(t3_vec3 center, t3_vec3 half) {
  cw_body3d_def bd = cw_body3d_default_def();
  bd.type = CW_BODY_STATIC;
  bd.position = center;
  cw_body3d b = cw_body3d_create(G.world, &bd);
  cw_shape3d_def sd = cw_shape3d_default_def();
  cw_shape3d_box(b, &sd, half, cw_xform3_identity());
}

static void spawn_coin(void) {
  for (int i = 0; i < MAX_COINS; i++) {
    coin *c = &G.coins[i];
    if (c->active) continue;
    c->x = cw_rng_range(&G.rng, -ARENA_HALF + 1.0f, ARENA_HALF - 1.0f);
    c->z = cw_rng_range(&G.rng, -ARENA_HALF + 1.0f, ARENA_HALF - 1.0f);
    c->active = 1;
    cw_node3d_set_position(c->node, (t3_vec3){ c->x, 0.6f, c->z });
    cw_node_set(c->node, "visible", cw_val_bool(true));
    G.coins_spawned++;
    return;
  }
}

/* debug command "coins": the active coins as JSON [[x, z], ...] */
static void cmd_coins(int argc, const char **argv, cw_reply *r) {
  char buf[512];
  int n = snprintf(buf, sizeof buf, "{\"score\": %d, \"coins\": [", G.score);
  bool first = true;
  for (int i = 0; i < MAX_COINS; i++) {
    if (!G.coins[i].active) continue;
    n += snprintf(buf + n, sizeof buf - n, "%s[%.3f, %.3f]", first ? "" : ", ", G.coins[i].x, G.coins[i].z);
    first = false;
  }
  snprintf(buf + n, sizeof buf - n, "]}");
  cw_reply_raw(r, buf);
}

static void game_init(void *user) {
  cw_scene_register_builtin_types();
  G.tree = cw_tree_new();
  cw_set_current_tree(G.tree);
  cw_rng_seed(&G.rng, 0x5EEDC0DEu);
  cw_node root = cw_tree_root(G.tree);

  /* environment, light, camera */
  cw_node env = cw_node_new("WorldEnvironment", "Env");
  cw_node_add_child(root, env);
  cw_node_set(env, "use_background", cw_val_bool(true));
  cw_node_set(env, "background", cw_val_color(0.17064f, 0.38006f, 0.78741f, 1));
  cw_node_set(env, "sky_color", cw_val_color(0.44799f, 0.60383f, 1, 1));
  cw_node_set(env, "ground_color", cw_val_color(0.07324f, 0.05088f, 0.0331f, 1));
  cw_node_set(env, "hemi_energy", cw_val_float(1.5708));
  cw_node_set(env, "ambient_color", cw_val_color(1, 1, 1, 1));
  cw_node_set(env, "ambient_energy", cw_val_float(0.31416));
  cw_node sun = cw_node_new("DirectionalLight3D", "Sun");
  cw_node_add_child(root, sun);
  cw_node3d_set_position(sun, (t3_vec3){ 6, 12, 4 });
  cw_node_set(sun, "energy", cw_val_float(4.08407));
  cw_node3d_look_at(sun, (t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0, 1, 0 });
  cw_node_set(sun, "shadow", cw_val_bool(true));
  G.camera = cw_node_new("Camera3D", "Camera");
  cw_node_add_child(root, G.camera);
  cw_node_set(G.camera, "current", cw_val_bool(true));

  /* physics world: floor, four walls */
  cw_world3d_def wd = cw_world3d_default_def();
  G.world = cw_world3d_create(&wd);
  static_box((t3_vec3){ 0, -0.25f, 0 }, (t3_vec3){ ARENA_HALF + 1, 0.25f, ARENA_HALF + 1 });
  static_box((t3_vec3){ ARENA_HALF + 0.5f, 0.75f, 0 }, (t3_vec3){ 0.5f, 0.75f, ARENA_HALF + 1 });
  static_box((t3_vec3){ -ARENA_HALF - 0.5f, 0.75f, 0 }, (t3_vec3){ 0.5f, 0.75f, ARENA_HALF + 1 });
  static_box((t3_vec3){ 0, 0.75f, ARENA_HALF + 0.5f }, (t3_vec3){ ARENA_HALF + 1, 0.75f, 0.5f });
  static_box((t3_vec3){ 0, 0.75f, -ARENA_HALF - 0.5f }, (t3_vec3){ ARENA_HALF + 1, 0.75f, 0.5f });

  cw_node floor = add_mesh(root, "Floor", "BOX", (t3_vec3){ 2 * ARENA_HALF + 2, 0.5f, 2 * ARENA_HALF + 2 },
                           cw_val_color(0.25f, 0.45f, 0.3f, 1), (t3_vec3){ 0, -0.25f, 0 });
  cw_value wall_col = cw_val_color(0.55f, 0.55f, 0.6f, 1);
  add_mesh(root, "WallE", "BOX", (t3_vec3){ 1, 1.5f, 2 * ARENA_HALF + 2 }, wall_col,
           (t3_vec3){ ARENA_HALF + 0.5f, 0.75f, 0 });
  add_mesh(root, "WallW", "BOX", (t3_vec3){ 1, 1.5f, 2 * ARENA_HALF + 2 }, wall_col,
           (t3_vec3){ -ARENA_HALF - 0.5f, 0.75f, 0 });
  add_mesh(root, "WallS", "BOX", (t3_vec3){ 2 * ARENA_HALF + 2, 1.5f, 1 }, wall_col,
           (t3_vec3){ 0, 0.75f, ARENA_HALF + 0.5f });
  add_mesh(root, "WallN", "BOX", (t3_vec3){ 2 * ARENA_HALF + 2, 1.5f, 1 }, wall_col,
           (t3_vec3){ 0, 0.75f, -ARENA_HALF - 0.5f });

  /* crates: dynamic boxes */
  static const t3_vec3 crate_at[NUM_CRATES] = { { 3, 0.5f, 0 }, { -4, 0.5f, 3 }, { 2, 0.5f, -5 }, { -3, 0.5f, -3 } };
  for (int i = 0; i < NUM_CRATES; i++) {
    cw_body3d_def bd = cw_body3d_default_def();
    bd.position = crate_at[i];
    bd.linear_damping = 1.0f;
    bd.angular_damping = 1.0f;
    G.crates[i] = cw_body3d_create(G.world, &bd);
    cw_shape3d_def sd = cw_shape3d_default_def();
    sd.density = 50.0f;
    cw_shape3d_box(G.crates[i], &sd, (t3_vec3){ 0.5f, 0.5f, 0.5f }, cw_xform3_identity());
    G.crate_nodes[i] = add_mesh(root, "Crate", "BOX", (t3_vec3){ 1, 1, 1 }, cw_val_color(0.6f, 0.4f, 0.2f, 1),
                                crate_at[i]);
  }

  /* player: a mover capsule plus a cylinder mesh with a "nose" */
  cw_mover3d_def md = cw_mover3d_default_def();
  md.radius = 0.4f;
  md.height = 1.6f;
  md.push_mass = 80.0f;
  cw_mover3d_init(&G.mover, G.world, &md, (t3_vec3){ 0, 0, 0 });
  G.player = add_mesh(root, "Player", "CYLINDER", (t3_vec3){ 0.8f, 1.6f, 0.8f },
                      cw_val_color(0.2f, 0.5f, 1.0f, 1), (t3_vec3){ 0, 0.8f, 0 });

  /* coins: a fixed pool of hidden meshes */
  for (int i = 0; i < MAX_COINS; i++) {
    G.coins[i].node = add_mesh(root, "Coin", "SPHERE", (t3_vec3){ 0.5f, 0.5f, 0.5f },
                               cw_val_color(1.0f, 0.8f, 0.1f, 1), (t3_vec3){ 0, -5, 0 });
    cw_node_set(G.coins[i].node, "emissive", cw_val_color(0.13287f, 0.07324f, 0, 1));
    cw_node_set(G.coins[i].node, "visible", cw_val_bool(false));
  }

  /* input: left stick, d-pad, arrows, WASD */
  G.left = cw_action_add("move_left", 0.2f);
  G.right = cw_action_add("move_right", 0.2f);
  G.up = cw_action_add("move_up", 0.2f);
  G.down = cw_action_add("move_down", 0.2f);
  cw_action_bind_text(G.left, "key:left, key:a, pad:left, axis:lx-");
  cw_action_bind_text(G.right, "key:right, key:d, pad:right, axis:lx+");
  cw_action_bind_text(G.up, "key:up, key:w, pad:up, axis:ly-");
  cw_action_bind_text(G.down, "key:down, key:s, pad:down, axis:ly+");

  G.font = cw_font_load("FiraSans-Regular.ttf", NULL);
  G.time_left = ROUND_SECONDS;
  spawn_coin();

  cw_debug_i32("score", &G.score);
  cw_debug_f32("time_left", &G.time_left);
  cw_debug_i32("game_over", &G.game_over);
  cw_debug_i32("coins_active", &G.coins_active);
  cw_debug_i32("coins_spawned", &G.coins_spawned);
  cw_debug_i32("ticks", &G.ticks);
  cw_debug_f32_array("player.pos", G.player_pos, 3);
  cw_debug_f32_array("crate0.pos", G.crate0_pos, 3);
  cw_debug_f32("input.x", &G.input_x);
  cw_debug_f32("input.y", &G.input_y);
  cw_debug_f32_array("coins.xz", G.coin_xz, MAX_COINS * 2);
  cw_debug_command("coins", "coins: active coin positions and the score", cmd_coins);
}

static void game_fixed_update(void *user, double dtd) {
  float dt = (float)dtd;
  G.ticks++;
  cw_vec2 mv = { 0, 0 };
  if (!G.game_over) {
    mv = cw_input_vector(0, G.left, G.right, G.up, G.down, CW_DEADZONE_RADIAL, 0.2f);
    G.time_left -= dt;
    if (G.time_left <= 0) {
      G.time_left = 0;
      G.game_over = 1;
      mv = (cw_vec2){ 0, 0 };
      cw_info("game over: score %d", G.score);
    }
    G.spawn_timer += dt;
    if (G.spawn_timer >= COIN_INTERVAL) {
      G.spawn_timer -= COIN_INTERVAL;
      spawn_coin();
    }
  }
  G.input_x = mv.x;
  G.input_y = mv.y;

  /* screen y down = toward the camera = +z */
  G.mover.velocity.x = mv.x * PLAYER_SPEED;
  G.mover.velocity.z = mv.y * PLAYER_SPEED;
  G.mover.velocity.y = G.mover.on_floor ? 0.0f : G.mover.velocity.y - 20.0f * dt;
  cw_mover3d_move(&G.mover, dt);
  cw_world3d_step(G.world, dt);
  cw_tree_fixed_update(G.tree, dt);

  t3_vec3 p = G.mover.position;
  G.player_pos[0] = p.x, G.player_pos[1] = p.y, G.player_pos[2] = p.z;
  cw_xform3 c0 = cw_body3d_transform(G.crates[0]);
  G.crate0_pos[0] = c0.p.x, G.crate0_pos[1] = c0.p.y, G.crate0_pos[2] = c0.p.z;

  /* coin pickup: horizontal distance from the player's axis */
  int active = 0;
  for (int i = 0; i < MAX_COINS; i++) {
    coin *c = &G.coins[i];
    if (!c->active) continue;
    float dx = c->x - p.x, dz = c->z - p.z;
    if (!G.game_over && dx * dx + dz * dz < COIN_PICK_RADIUS * COIN_PICK_RADIUS) {
      c->active = 0;
      G.score++;
      cw_node_set(c->node, "visible", cw_val_bool(false));
      cw_info("coin %d collected, score %d", i, G.score);
      continue;
    }
    active++;
  }
  G.coins_active = active;
  for (int i = 0; i < MAX_COINS; i++) {
    G.coin_xz[2 * i] = G.coins[i].active ? G.coins[i].x : 99.0f;
    G.coin_xz[2 * i + 1] = G.coins[i].active ? G.coins[i].z : 99.0f;
  }
  G.spin += dt * 3.0f;
}

static void game_render(void *user, double alpha) {
  float a = (float)alpha;
  t3_vec3 pp = cw_mover3d_interpolated(&G.mover, a);
  cw_node3d_set_position(G.player, (t3_vec3){ pp.x, pp.y + 0.8f, pp.z });
  for (int i = 0; i < NUM_CRATES; i++) {
    cw_xform3 x = cw_body3d_interpolated(G.crates[i], a);
    cw_node3d_set_position(G.crate_nodes[i], x.p);
    cw_node3d_set_quaternion(G.crate_nodes[i], x.q);
  }
  for (int i = 0; i < MAX_COINS; i++)
    if (G.coins[i].active) cw_node3d_set_rotation(G.coins[i].node, (t3_vec3){ 0, G.spin, 0 });
  /* camera follows from behind and above */
  cw_node3d_set_position(G.camera, (t3_vec3){ pp.x, pp.y + 9.0f, pp.z + 8.0f });
  cw_node3d_look_at(G.camera, (t3_vec3){ pp.x, pp.y, pp.z }, (t3_vec3){ 0, 1, 0 });

  cw_tree_pre_render(G.tree, alpha);
  int w, h;
  cw_drawable_size(&w, &h);
  cw_render3d_draw(G.tree, w, h);

  /* HUD */
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = false });
  char buf[64];
  snprintf(buf, sizeof buf, "Score %d", G.score);
  cw_r2d_rect(cw_rect_make(8, 8, 180, 64), cw_rgba(0, 0, 0, 0.5f));
  cw_r2d_text(G.font, buf, cw_v2(16, 12), &(cw_text_style){ .size = 26 });
  snprintf(buf, sizeof buf, "Time %d", (int)(G.time_left + 0.999f));
  cw_r2d_text(G.font, buf, cw_v2(16, 40), &(cw_text_style){ .size = 26 });
  if (G.game_over) {
    cw_vec2 cs = cw_r2d_canvas_size();
    cw_r2d_rect(cw_rect_make(0, cs.y * 0.35f, cs.x, cs.y * 0.3f), cw_rgba(0, 0, 0, 0.65f));
    snprintf(buf, sizeof buf, "GAME OVER - %d coins", G.score);
    cw_r2d_text(G.font, buf, cw_v2(0, cs.y * 0.42f),
                &(cw_text_style){ .size = 40, .wrap_width = cs.x, .align = CW_ALIGN_CENTER,
                                  .color = cw_rgba(1, 0.85f, 0.2f, 1) });
  }
  cw_r2d_end();
  t3_renderer *r = cw_render3d_renderer(G.tree);
  if (r) t3_renderer_reset_bindings(r);
}

cw_app_desc cw_main(int argc, char **argv) {
  return (cw_app_desc){
    .title = "spike_game",
    .width = 640,
    .height = 360,
    .deterministic = true,
    .debug = true,
    .init = game_init,
    .fixed_update = game_fixed_update,
    .render = game_render,
  };
}
