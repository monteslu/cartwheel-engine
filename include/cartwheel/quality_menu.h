/* cartwheel/quality_menu.h: a graphics settings page a game opens from its
 * own menus (the game shell, cartwheel/shell.h, opens it from Settings).
 *
 * One row per choice: the preset (Low, Medium, High, Ultra, Auto, or Custom
 * once a setting was changed on its own), then every setting of
 * cartwheel/quality.h (resolution with the pixel size it renders at,
 * ambient occlusion, shadows, anti-aliasing, effects detail), then the
 * game's own levers (cw_quality_lever_add), then Back.
 *
 *   // from the pause menu's "Graphics" entry (render-only: update, not the simulation)
 *   cw_quality_menu_open();
 *   // each frame, before the game's own menu reads input:
 *   if (cw_quality_menu_update()) { ... the page used this frame's presses ... }
 *   // in the frame's 2D pass, last:
 *   cw_quality_menu_draw(&(cw_quality_menu_style){ .font = my_font });
 *   // save when it closed after a change: cw_quality_menu_changed()
 *
 * Input (cw_quality_menu_update): up / down (arrows, W / S, d-pad, left
 * stick) choose a row, left / right (A / D, d-pad, stick) change it, accept
 * (Enter, Space, South) changes it forward or closes on Back, back (Escape,
 * Backspace, East) closes; a tap (mouse or touch) on a row changes it. A game
 * with its own input mapping drives it with cw_quality_menu_input instead.
 *
 * Every change applies at once (cw_quality_* settings); none reaches the
 * simulation. */
#ifndef CW_QUALITY_MENU_H
#define CW_QUALITY_MENU_H

#include "base.h"
#include "render2d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_quality_menu_style {
  cw_font font;      /* default: the built-in sans (cw_ui_font_sans) */
  cw_color accent;   /* the highlighted row (default a warm yellow) */
  cw_color backdrop; /* drawn over the whole canvas first (default dark, 0.92 alpha; a = 0 with
                      * set_backdrop draws none) */
  bool set_backdrop;
  float size;        /* text size in canvas units (default: the canvas height / 24) */
  const char *title; /* default "Graphics" */
} cw_quality_menu_style;

void cw_quality_menu_open(void);     /* show it, the preset row highlighted */
void cw_quality_menu_close(void);    /* hide it (Back and the back press do too) */
bool cw_quality_menu_is_open(void);  /* it is showing */
/* Drive it from the frame's raw input (once a frame, however often it is
 * called). True while it is open: the page owns this frame's presses, so the
 * game's own menu should not act on them. */
bool cw_quality_menu_update(void);
/* Drive it from the game's own mapping: dy -1 / +1 up / down, dx -1 / +1
 * left / right, accept, back. */
void cw_quality_menu_input(int dy, int dx, bool accept, bool back);
/* A tap at a canvas point (cw_r2d_window_to_canvas): true when it hit a row. */
bool cw_quality_menu_tap(cw_vec2 p);
/* Draw it (in the current 2D pass, over the whole canvas) when open. */
void cw_quality_menu_draw(const cw_quality_menu_style *style);
/* A setting changed since it was opened (save then: cw_quality_saved). */
bool cw_quality_menu_changed(void);

#ifdef __cplusplus
}
#endif
#endif
