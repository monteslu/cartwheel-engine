/* shell_join: the game shell's join screen opened in the middle of a match.
 *
 * Two players start. A pad that has no seat pressing A takes a seat at once (cw_shell_join_press, one press).
 * West (X) on any seated pad, or a pad that has no seat pressing Start, opens the join screen over the running match: a third and fourth pad take a seat with A, a seated pad leaves with B,
 * Start confirms and play carries on with the new seating (every seat keeps its number). The game only answers players_changed. */
#include <stdio.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/shell.h"

static struct {
  int32_t players;      /* the match's player count (the highest seat held plus one) */
  int32_t seats;        /* bit per seat held now */
  int32_t changes;      /* how many times players_changed answered */
  int32_t ticks;        /* play ticks (they stop under the join screen) */
  int32_t opened;       /* how many times the join screen was opened */
  int32_t pressed;      /* how many pads joined with one press */
  float x[4];           /* each player's square, moved by its stick */
  cw_action left, right, open;
} G;

static void start(int players, void *user) {
  (void)user;
  G.players = players;
  G.seats = (1 << players) - 1;
  G.ticks = 0;
  for (int i = 0; i < 4; i++) G.x[i] = 80.0f + 60.0f * (float)i;
}
static void players_changed(int players, void *user) {
  (void)user;
  for (int i = G.players; i < players; i++) G.x[i] = 80.0f + 60.0f * (float)i; /* a new seat starts at its place */
  G.players = players;
  G.seats = 0;
  for (int i = 0; i < players; i++) G.seats |= cw_shell_seat_taken(i) ? 1 << i : 0;
  G.changes++;
}

static void init(void *user) {
  (void)user;
  cw_shell_init(&(cw_shell_desc){ .title = "Join Demo", .max_players = 4, .start = start, .players_changed = players_changed });
  G.left = cw_action_add("left", 0.2f);
  G.right = cw_action_add("right", 0.2f);
  G.open = cw_action_add("join", 0.5f);
  cw_action_bind_text(G.left, "key:left, axis:lx-");
  cw_action_bind_text(G.right, "key:right, axis:lx+");
  cw_action_bind_text(G.open, "pad:west, key:j");
  cw_debug_i32("game.players", &G.players);
  cw_debug_i32("game.seats", &G.seats);
  cw_debug_i32("game.changes", &G.changes);
  cw_debug_i32("game.ticks", &G.ticks);
  cw_debug_i32("game.opened", &G.opened);
  cw_debug_i32("game.pressed", &G.pressed);
}

static void fixed_update(void *user, double dt) {
  (void)user;
  if (!cw_shell_update()) return; /* a menu, or the join screen, owns this tick */
  if (cw_shell_join_press()) G.pressed++; /* a pad without a seat pressed A: seated already */
  bool ask = cw_shell_join_requested();   /* Start on a pad without a seat: offer the join screen */
  for (int p = 0; p < G.players; p++) {
    if (!((G.seats >> p) & 1)) continue; /* an empty seat */
    G.x[p] += cw_input_axis(p, G.left, G.right) * 200.0f * (float)dt;
    if (cw_action_just_pressed(p, G.open)) ask = true;
  }
  if (ask && cw_shell_join_open()) G.opened++;
  G.ticks++;
}

static void render(void *user, double alpha) {
  (void)user, (void)alpha;
  static const uint32_t col[4] = { 0xe05050ff, 0x50a0e0ff, 0x60d070ff, 0xe0c040ff };
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x1c1e26ff) });
  if (cw_shell_screen_now() == CW_SHELL_PLAY || cw_shell_screen_now() == CW_SHELL_JOIN)
    for (int p = 0; p < G.players; p++)
      if ((G.seats >> p) & 1) cw_r2d_rect(cw_rect_make(G.x[p], 60.0f + 70.0f * (float)p, 50, 50), cw_hex(col[p]));
  cw_r2d_end();
  cw_shell_draw(); /* the menus, in their own pass */
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){
    .title = "cartwheel shell join",
    .width = 640,
    .height = 360,
    .init = init,
    .fixed_update = fixed_update,
    .render = render,
    .save = cw_shell_save_region(),
    .save_size = cw_shell_save_region_size(),
    .debug = true,
    .user = &G,
  };
}
