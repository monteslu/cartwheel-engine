/* The shell samples' two-colour switches (cartwheel/shell.h two_colour), for a game with a one-bit look:
 *   SHELL_TWO_COLOUR=1     the shell in two colours, SHELL_INK and SHELL_PAPER (hex RRGGBB, default f0e8c8 on 182028),
 *                          and the game's own picture in the same two (cw_test_env, read every tick from frame 2)
 *   SHELL_SWITCH_AT=<n>    from frame n the second pair, SHELL_INK2 and SHELL_PAPER2 (default 40ff90 on 001008), through
 *                          cw_shell_set_colours: a game whose player picked another palette in Settings
 *   SHELL_TWO_BG=1         (getenv, read in init) a draw_background: a one-bit pattern of ink and paper behind the menus
 * SHELL_TWO_COLOUR=0 sets the mode off and every colour to its default each tick (the frames must be the plain ones).
 * Gate controls: SHELL_TWO_SHELL=0 the picture in two colours but the shell as it is (no two-colour mode);
 * SHELL_NO_SETTER=1 the picture switches pair at SHELL_SWITCH_AT but the shell is not told (no cw_shell_set_colours).
 * Without them the samples draw as they always did. */
#ifndef SAMPLES_SHELL_TWO_COLOUR_H
#define SAMPLES_SHELL_TWO_COLOUR_H

#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/shell.h"

static uint32_t two_hex(const char *name, uint32_t def) {
  const char *v = cw_test_env(name);
  return v && *v ? (uint32_t)strtoul(v, NULL, 16) & 0xffffffu : def;
}
static bool two_off(void) { /* SHELL_TWO_COLOUR=0: the mode set off and the colours to their defaults every tick */
  const char *v = cw_test_env("SHELL_TWO_COLOUR");
  return v && *v == '0';
}
static bool two_on(void) {
  const char *v = cw_test_env("SHELL_TWO_COLOUR");
  return v && *v == '1';
}
static bool two_switched(void) {
  const char *v = cw_test_env("SHELL_SWITCH_AT");
  return v && *v && cw_frame_index() >= (uint64_t)strtoull(v, NULL, 10);
}
static cw_color two_ink(void) {
  return cw_hex((two_switched() ? two_hex("SHELL_INK2", 0x40ff90) : two_hex("SHELL_INK", 0xf0e8c8)) << 8 | 0xff);
}
static cw_color two_paper(void) {
  return cw_hex((two_switched() ? two_hex("SHELL_PAPER2", 0x001008) : two_hex("SHELL_PAPER", 0x182028)) << 8 | 0xff);
}
/* every tick, before cw_shell_update: the mode and its colours (set again each tick, as a game does when its look changes) */
static void two_colour_tick(void) {
  if (two_off()) {
    cw_shell_set_two_colour(false);
    cw_shell_set_colours((cw_color){ 0 }, (cw_color){ 0 }, (cw_color){ 0 }, (cw_color){ 0 });
  }
  if (!two_on()) return;
  const char *shell = cw_test_env("SHELL_TWO_SHELL"), *no_setter = cw_test_env("SHELL_NO_SETTER");
  if (shell && *shell == '0') return;
  cw_shell_set_two_colour(true);
  if (two_switched() && no_setter && *no_setter == '1') return;
  cw_shell_set_colours((cw_color){ 0 }, (cw_color){ 0 }, two_ink(), two_paper());
}
/* a one-bit picture behind the menus: a checker of 6-pixel cells, whole pixels, no blended edge */
static void two_background(cw_shell_screen screen, cw_vec2 sz, void *user) {
  (void)screen, (void)user;
  cw_color ink = two_on() ? two_ink() : CW_WHITE, paper = two_on() ? two_paper() : cw_hex(0x000000ff);
  cw_r2d_rect(cw_rect_make(0, 0, sz.x, sz.y), paper);
  for (int y = 0; y * 6 < (int)sz.y; y++)
    for (int x = (y & 1); x * 6 < (int)sz.x; x += 2) cw_r2d_rect(cw_rect_make((float)x * 6, (float)y * 6, 6, 6), ink);
}
typedef void (*two_background_fn)(cw_shell_screen screen, cw_vec2 canvas, void *user);
static two_background_fn two_background_wanted(void) {
  const char *v = getenv("SHELL_TWO_BG");
  return v && *v == '1' ? two_background : NULL;
}

#endif
