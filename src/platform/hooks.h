/* Platform hooks other engine modules register (no SDL here). */
#ifndef CW_PLATFORM_HOOKS_H
#define CW_PLATFORM_HOOKS_H

#include "cartwheel/platform.h"

/* The input module's place in cw__frame: frame_begin right after the event
 * poll, tick_begin before every fixed_update, ticks_end after the last tick
 * of the frame (before update and render). Registered by the input module
 * on first use, so the platform does not link it otherwise. */
typedef struct cw__input_hooks {
  void (*frame_begin)(const cw_raw_input *raw);
  void (*tick_begin)(void);
  void (*ticks_end)(void);
} cw__input_hooks;
void cw__platform_hook_input(const cw__input_hooks *h);

/* The UI module's place in cw__frame: frame_begin(dt) right after the input
 * module's frame_begin (GUI input is routed before any fixed tick reads
 * actions), and every app event (text, keys) as it is emitted. Registered by
 * the UI module on first use. */
typedef struct cw__ui_hooks {
  void (*frame_begin)(double dt);
  void (*event)(const cw_event *e);
} cw__ui_hooks;
void cw__platform_hook_ui(const cw__ui_hooks *h);

/* The engine's touch overlay (src/input/touch.c): drawn right after the
 * app's render callback, so it lands over the game's HUD and menus. Registered
 * by the input module when the first thumbstick or round button is added. */
void cw__platform_hook_touch_overlay(void (*draw)(void));

/* The user's preferred locale ("ja-JP", "en"), written into out; "" when
 * the platform does not say (carts: the wasmcart ABI carries no locale, so
 * always ""). The l10n module reads it through here, never SDL itself. */
void cw__platform_system_locale(char *out, size_t cap);

bool cw__platform_pad_has_rumble(int pad);                    /* sdl_common.c: cw_input_has_rumble's hardware half */
void cw__platform_pad_rumble(int pad, float low, float high, int ms); /* ms 0 stops */
extern uint32_t cw__rumble_req[5];                                  /* input.c: the debug field cw.rumble */

#endif
