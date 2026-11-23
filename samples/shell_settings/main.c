/* shell_settings: the game's own rows on the shell's Settings screen (cw_shell_desc.settings_items /
 * settings_changed).
 *
 * One player plays a square in a colour style. The Settings screen (from the title or the pause menu) lists the
 * shell's rows (Graphics, Music, Effects, Text size, Text background, Controls), then the game's three ("Visual
 * style", "Hit sounds", "Lane hints"), then Back. Left / right step a game row's choice, stopping at the ends; South
 * or a tap steps it forward and goes round, as on the shell's own rows. The shell saves each under its key and loads
 * it back before the game's first frame; settings_changed tells the game.
 *
 * SHELL_SETTINGS_ITEMS=0..3 (default 3): how many of the rows the game keeps, set at the first tick past frame 2
 * (cw_shell_set_settings_items, as a game changing its rows would; the gate's switches reach a cart at frame 2).
 * 0 is the shell's Settings alone. SHELL_SETTINGS_RENAME=1: the rows are renamed there (labels and choices change
 * at runtime, the values stay).
 *
 * Debug fields: game.calls (settings_changed calls), game.index (the last index, -1 none), game.value (its value),
 * game.style, game.hits, game.hints (the three values), game.items (rows kept).
 *
 * SHELL_SELECTION=0..3 (cw_test_env): the menus' selection style, COLOUR, MARKER, UNDERLINE or INVERSE.
 * SHELL_TWO_COLOUR=1 and its companions (samples/common/shell_two_colour.h): the shell in two colours, the game's picture too. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/shell.h"
#include "cartwheel/ui.h"
#include "../common/shell_two_colour.h"

static struct {
  int32_t calls, index, value, items;
  int style, hits, hints;
  bool configured;
  float x;
  cw_action left, right;
} G = { .style = 0, .hits = 1, .hints = 2 };

static const char *const STYLES[4] = { "All", "Fixed: Neon", "Fixed: Paper", "Fixed: Chalk" };
static const char *const ON_OFF[2] = { "Off", "On" };
static const char *const HINTS[3] = { "None", "Some", "Every lane" };
static const char *const STYLES_RENAMED[4] = { "Every style", "Neon only", "Paper only", "Chalk only" };
static const uint32_t STYLE_COLORS[4] = { 0xe0c050ff, 0x40f0e0ff, 0xf0ece0ff, 0xb0b8c0ff };

static cw_shell_setting ITEMS[3] = {
  { .label = "Visual style", .key = "style", .value = &G.style, .choices = STYLES, .count = 4 },
  { .label = "Hit sounds", .key = "hits", .value = &G.hits, .choices = ON_OFF, .count = 2 },
  { .label = "Lane hints", .key = "hints", .value = &G.hints, .choices = HINTS, .count = 3 },
};

static void start(int players, void *user) { (void)players, (void)user; G.x = 60; }
static void settings_changed(int index, int value, void *user) {
  (void)user;
  G.calls++;
  G.index = index;
  G.value = value;
}

static void init(void *user) {
  (void)user;
  /* the values above are the defaults; the shell puts the saved ones in before the first frame */
  cw_shell_init(&(cw_shell_desc){ .title = "Settings Demo", .start = start, .settings_items = ITEMS, .settings_item_count = 3,
                                  .settings_changed = settings_changed, .draw_background = two_background_wanted() });
  G.left = cw_action_add("left", 0.2f);
  G.right = cw_action_add("right", 0.2f);
  cw_action_bind_text(G.left, "key:left, axis:lx-");
  cw_action_bind_text(G.right, "key:right, axis:lx+");
  G.index = -1;
  G.items = 3;
  G.x = 60;
  cw_debug_i32("game.calls", &G.calls);
  cw_debug_i32("game.index", &G.index);
  cw_debug_i32("game.value", &G.value);
  cw_debug_i32("game.style", (int32_t *)&G.style);
  cw_debug_i32("game.hits", (int32_t *)&G.hits);
  cw_debug_i32("game.hints", (int32_t *)&G.hints);
  cw_debug_i32("game.items", &G.items);
}

static void fixed_update(void *user, double dt) {
  (void)user;
  const char *sel = cw_test_env("SHELL_SELECTION"); /* 0..3: the menus' selection style (cw_shell_set_selection) */
  if (sel && *sel) cw_shell_set_selection((cw_shell_selection)atoi(sel));
  two_colour_tick();
  if (!G.configured && cw_frame_index() >= 2) {
    const char *n = getenv("SHELL_SETTINGS_ITEMS"), *rn = getenv("SHELL_SETTINGS_RENAME");
    G.items = n && *n >= '0' && *n <= '3' ? *n - '0' : 3;
    if (rn && *rn == '1') ITEMS[0].label = "Look", ITEMS[0].choices = STYLES_RENAMED;
    G.configured = true;
    cw_shell_set_settings_items(ITEMS, G.items);
  }
  if (!cw_shell_update()) return; /* a menu owns this tick */
  G.x += cw_input_axis(0, G.left, G.right) * 200.0f * (float)dt;
}

static void render(void *user, double alpha) {
  (void)user, (void)alpha;
  bool two = two_on(); /* the game's picture in the shell's two colours */
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = two ? two_paper() : cw_hex(0x1c1e26ff) });
  cw_r2d_rect(cw_rect_make(0, 300, 640, 60), two ? two_ink() : cw_hex(0x30343eff));
  if (cw_shell_screen_now() != CW_SHELL_TITLE)
    cw_r2d_rect(cw_rect_make(two ? floorf(G.x) : G.x, 250, 50, 50), two ? two_ink() : cw_hex(STYLE_COLORS[G.style & 3]));
  if (cw_shell_hud_shown())
    cw_r2d_text(cw_ui_font_sans(), "Start: pause", cw_v2(16, 16), &(cw_text_style){ .size = 360.0f * 0.03f, .color = CW_WHITE });
  cw_r2d_end();
  cw_shell_draw();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){
    .title = "cartwheel shell settings",
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
