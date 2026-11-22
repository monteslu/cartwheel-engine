/* shell_pause: the game's own items in the shell's pause menu (cw_shell_desc.pause_items / pause_item).
 *
 * One player plays a square that South makes hop. Start pauses: the menu is Resume, then up to two items of the
 * game's ("Restart", "Change color"), then Settings and Quit to title. Choosing one of the game's items puts the
 * shell back in play, as Resume does, and the game answers in pause_item: Restart puts the square back, Change color
 * gives it the next colour and renames the item for the next pause (cw_shell_set_pause_items).
 *
 * SHELL_PAUSE_ITEMS=0, 1 or 2 (default 2): how many items the game sets (0 is the shell's own three items). Read at
 * the first tick past frame 2, as a game changing its items between pauses would (cw_shell_set_pause_items).
 * SHELL_PAUSE_NOHOOK=1: the hook ignores the call (a game whose item does nothing: the gates' control).
 *
 * Debug fields: game.calls (pause_item calls), game.index (the last index, -1 none), game.hook_screen (the shell's
 * screen when the hook ran: 3, play), game.hops (South presses gameplay saw), game.first_hop (South just pressed on
 * the first gameplay tick after a pause: the menu's press leaking into the game), game.ticks, game.color. */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/shell.h"
#include "cartwheel/ui.h"

static struct {
  int32_t calls, index, hook_screen, hops, first_hop, ticks, color, items;
  bool configured, after_pause, nohook;
  float x, y, vy;
  char color_label[32];
  cw_action hop, left, right;
} G;

static const char *const COLOR_NAMES[3] = { "red", "blue", "green" };
static const uint32_t COLORS[3] = { 0xe05050ff, 0x50a0e0ff, 0x60d070ff };

static void set_items(void) {
  snprintf(G.color_label, sizeof G.color_label, "Color: %s", COLOR_NAMES[(G.color + 1) % 3]);
  const char *items[2] = { G.items >= 1 ? "Restart" : NULL, G.items >= 2 ? G.color_label : NULL };
  cw_shell_set_pause_items(items);
}
static void restart(void) { G.x = 60, G.y = 250, G.vy = 0; }

static void start(int players, void *user) { (void)players, (void)user; restart(); }
static void pause_item(int index, void *user) {
  (void)user;
  if (G.nohook) return;
  G.calls++;
  G.index = index;
  G.hook_screen = (int32_t)cw_shell_screen_now();
  if (index == 0) restart();
  else {
    G.color = (G.color + 1) % 3;
    set_items(); /* the next pause offers the colour after this one */
  }
}
static void screen_changed(cw_shell_screen from, cw_shell_screen to, void *user) {
  (void)user;
  if (from == CW_SHELL_PAUSE && to == CW_SHELL_PLAY) G.after_pause = true;
}

static void init(void *user) {
  (void)user;
  cw_shell_init(&(cw_shell_desc){ .title = "Pause Demo", .start = start, .pause_item = pause_item, .screen_changed = screen_changed });
  G.hop = cw_action_add("hop", 0.5f);
  G.left = cw_action_add("left", 0.2f);
  G.right = cw_action_add("right", 0.2f);
  cw_action_bind_text(G.hop, "pad:south, key:space, key:return");
  cw_action_bind_text(G.left, "key:left, axis:lx-");
  cw_action_bind_text(G.right, "key:right, axis:lx+");
  G.index = -1;
  restart();
  cw_debug_i32("game.calls", &G.calls);
  cw_debug_i32("game.index", &G.index);
  cw_debug_i32("game.hook_screen", &G.hook_screen);
  cw_debug_i32("game.hops", &G.hops);
  cw_debug_i32("game.first_hop", &G.first_hop);
  cw_debug_i32("game.ticks", &G.ticks);
  cw_debug_i32("game.color", &G.color);
  cw_debug_i32("game.items", &G.items);
}

static void fixed_update(void *user, double dt) {
  (void)user;
  if (!G.configured && cw_frame_index() >= 2) { /* the gate's switches reach a cart at frame 2 */
    const char *n = getenv("SHELL_PAUSE_ITEMS"), *nh = getenv("SHELL_PAUSE_NOHOOK");
    G.items = n && *n >= '0' && *n <= '2' ? *n - '0' : 2;
    G.nohook = nh && *nh == '1';
    G.configured = true;
    set_items();
  }
  if (!cw_shell_update()) return; /* a menu owns this tick */
  bool hop = cw_action_just_pressed(0, G.hop);
  if (G.after_pause && hop) G.first_hop++;
  G.after_pause = false;
  if (hop && G.y >= 250) G.vy = -420, G.hops++;
  G.vy += 1200.0f * (float)dt;
  G.y += G.vy * (float)dt;
  if (G.y > 250) G.y = 250, G.vy = 0;
  G.x += cw_input_axis(0, G.left, G.right) * 200.0f * (float)dt;
  G.ticks++;
}

static void render(void *user, double alpha) {
  (void)user, (void)alpha;
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x1c1e26ff) });
  cw_r2d_rect(cw_rect_make(0, 300, 640, 60), cw_hex(0x30343eff));
  if (cw_shell_screen_now() != CW_SHELL_TITLE) cw_r2d_rect(cw_rect_make(G.x, G.y, 50, 50), cw_hex(COLORS[G.color]));
  if (cw_shell_hud_shown())
    cw_r2d_text(cw_ui_font_sans(), "Start: pause", cw_v2(16, 16), &(cw_text_style){ .size = 360.0f * 0.03f, .color = CW_WHITE });
  cw_r2d_end();
  cw_shell_draw();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){
    .title = "cartwheel shell pause",
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
