/* play_demo: the game the editor's play-in-editor gate plays
 * (tests/editor-play.mjs, M4.3). Its project (assets/project.cwproj) is
 * what the editor opens; the scenes use engine node types only (so the
 * editor can open and save them) and this file adds the game's own logic:
 * every frame the HUD Label shows the ball's height and the game's tick
 * count, and a ball that falls off the world respawns at its start. A live
 * edit of the ball (gravity_scale, freeze, position) therefore changes what
 * the GAME computes and shows, not just the edited value.
 *
 * Debug fields: game.ball_y (the ball's height, metres), game.ticks (fixed
 * ticks the game ran), game.respawns. */
#include <stdio.h>

#include "cartwheel/debug.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/ui.h"

static struct {
  float ball_y;
  int32_t ticks, respawns;
} G;

static cw_node level_node(const char *rel) {
  cw_node root = cw_tree_root(cw_scene_app_tree());
  /* the main scene's root is the first child of /root, whatever it is called */
  if (cw_node_child_count(root) == 0) return (cw_node){ 0 };
  return cw_node_get(cw_node_child(root, 0), rel);
}

static void init(void *u) {
  (void)u;
  cw_debug_f32("game.ball_y", &G.ball_y);
  cw_debug_i32("game.ticks", &G.ticks);
  cw_debug_i32("game.respawns", &G.respawns);
}

static void fixed_update(void *u, double dt) {
  (void)u, (void)dt;
  G.ticks++;
  cw_node ball = level_node("Ball");
  if (!cw_node_valid(ball)) return;
  t3_vec3 p = cw_node3d_position(ball);
  if (p.y < -10) { /* fell off the world: back to the start */
    cw_node3d_set_position(ball, (t3_vec3){ 0, 6, 0 });
    cw_node_set(ball, "linear_velocity", cw_val_vec3(0, 0, 0));
    G.respawns++;
  }
}

static void update(void *u, double dt) {
  (void)u, (void)dt;
  cw_node ball = level_node("Ball"), hud = level_node("Hud");
  if (!cw_node_valid(ball)) return;
  G.ball_y = cw_node3d_position(ball).y;
  if (cw_node_valid(hud)) cw_label_set_text(hud, cw_frame_printf("ball height %.2f m   tick %d", G.ball_y, (int)G.ticks));
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  static cw_app_desc base = { .title = "play_demo", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed_update, .update = update };
  return cw_scene_app("main.cwscene", &base);
}
