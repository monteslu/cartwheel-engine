/* prompts: how a game shows which button does what (cartwheel/prompt.h).
 *
 * The top half is the glyph sheet: every glyph the engine draws, at one size,
 * on a dark strip and on a light one, then the face-button diamond at sizes
 * from 10 to 48 canvas pixels on both (the native-scale crops the minimum
 * size was read from). The bottom half is live: player 1's prompts on the
 * device they used last, a shared prompt for every seat, the same action on
 * each device, a lane layout by button position, and hint lines whose markup
 * comes from a translation catalog (English, or Arabic right to left after
 * the debug request `l10n locale ar`).
 *
 * The debug request `rebind` listens for the next pad button and moves jump's
 * pad binding to it: every prompt follows on that frame. Native env:
 * PROMPTS_TYPO=1 writes {jmup} in one hint (the prompts-text gate's control).
 *
 * The debug request `ladder` swaps the page for the stick ladder: every stick glyph (a pair, four ways,
 * one way, each stick, a press, the touch stick) and the keycaps at 16, 20, 24, 32 and 48 canvas pixels, on dark
 * and light, then in lines of HUD text at 16 and 22: the native-scale crops the stick glyph was drawn from.
 *
 * Debug fields: sample.dev_hist (player 1's devices, in the order they
 * changed), sample.dev_n, sample.text_w (widths of the hint lines),
 * sample.rebind_code (the glyph jump drew on the frame it was rebound),
 * sample.missing_touch (glyphs drawn for an action with no touch binding), and
 * the engine's prompt.* fields. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/l10n.h"
#include "cartwheel/platform.h"
#include "cartwheel/prompt.h"
#include "cartwheel/render2d.h"
#include "cartwheel/ui.h"

/* #region catalogs */
static const char EN[] =
    "@locale = en\n"
    "hint.jump = {jump}: hop a gap\n"
    "hint.move = {move_left|move_right|move_up|move_down} move    {dash} dash\n"
    "hint.menu = {pause} pause    {menu_back} back\n";
static const char AR[] =
    "@locale = ar\n"
    "@direction = rtl\n"
    "@font = fonts/NotoSansArabic.ttf\n"
    "hint.jump = {jump} : اقفز فوق الفجوة\n"
    "hint.move = {move_left|move_right|move_up|move_down} تحرك    {dash} اندفع\n"
    "hint.menu = {pause} إيقاف مؤقت    {menu_back} رجوع\n";
/* #endregion */

static struct {
  cw_font sans;
  cw_action jump, dash, left, right, up, down, pause, back, fire, zoom, aim, look_l, look_r;
  int32_t dev_hist[8], dev_n;
  float text_w[3];
  uint32_t rebind_code, missing_touch;
  bool listening, typo, ladder;
  uint64_t rebound_frame;
} S;

static void cmd_rebind(int argc, const char **argv, cw_reply *r) {
  (void)argc, (void)argv;
  cw_input_capture_begin(0, 0, true);
  S.listening = true;
  cw_reply_printf(r, "{\"listening\":true}");
}

static void cmd_ladder(int argc, const char **argv, cw_reply *r) {
  (void)argc, (void)argv;
  S.ladder = !S.ladder;
  cw_reply_printf(r, "{\"ladder\":%s}", S.ladder ? "true" : "false");
}

static void init(void *user) {
  (void)user;
  S.sans = cw_ui_font_sans();
  cw_l10n_add_catalog(EN, sizeof EN - 1, "en");
  cw_l10n_add_catalog(AR, sizeof AR - 1, "ar");
  cw_l10n_set_locale("en");
  S.typo = getenv("PROMPTS_TYPO") != NULL;

  /* #region actions */
  S.jump = cw_action_add("jump", 0.5f);
  cw_action_bind_text(S.jump, "key:space, pad:south, touch:0");
  S.dash = cw_action_add("dash", 0.5f);
  cw_action_bind_text(S.dash, "key:lshift, pad:west, touch:2");
  S.left = cw_action_add("move_left", 0.3f);
  cw_action_bind_text(S.left, "key:a, key:left, pad:left, axis:lx-, touch:1.left");
  S.right = cw_action_add("move_right", 0.3f);
  cw_action_bind_text(S.right, "key:d, key:right, pad:right, axis:lx+, touch:1.right");
  S.up = cw_action_add("move_up", 0.3f);
  cw_action_bind_text(S.up, "key:w, key:up, pad:up, axis:ly-, touch:1.up");
  S.down = cw_action_add("move_down", 0.3f);
  cw_action_bind_text(S.down, "key:s, key:down, pad:down, axis:ly+, touch:1.down");
  S.pause = cw_action_add("pause", 0.5f);
  cw_action_bind_text(S.pause, "key:return, pad:start");
  S.back = cw_action_add("menu_back", 0.5f);
  cw_action_bind_text(S.back, "key:escape, pad:back");
  S.fire = cw_action_add("fire", 0.5f);
  cw_action_bind_text(S.fire, "mouse:left, axis:rt+@0.5, key:j");
  S.zoom = cw_action_add("zoom", 0.5f);
  cw_action_bind_text(S.zoom, "wheel:up, pad:rshoulder");
  S.aim = cw_action_add("aim", 0.5f); /* nothing on touch: a touch player is shown nothing, and the log says so once */
  cw_action_bind_text(S.aim, "mouse:right, axis:lt+@0.5");
  S.look_l = cw_action_add("look_left", 0.3f);
  cw_action_bind_text(S.look_l, "axis:rx-");
  S.look_r = cw_action_add("look_right", 0.3f);
  cw_action_bind_text(S.look_r, "axis:rx+");
  /* #endregion */

  /* the touch controls the touch prompts show (the overlay is off: the sheet is the picture) */
  cw_touch_add_round_button(&(cw_touch_button_desc){ .x = 110, .y = 110, .label = "Jump" });
  cw_touch_add_thumbstick(NULL);
  cw_touch_add_round_button(&(cw_touch_button_desc){ .x = 220, .y = 70, .label = "Dash", .color = cw_hex(0x80c0ffff) });
  cw_touch_set_overlay(CW_TOUCH_OVERLAY_OFF);

  cw_debug_command("ladder", "ladder: the stick ladder page instead of the sheet (again: back)", cmd_ladder);
  cw_debug_command("rebind", "rebind: the next pad button becomes jump's pad binding", cmd_rebind);
  cw_debug_field("sample.dev_hist", S.dev_hist, CW_DBG_I32, 8);
  cw_debug_i32("sample.dev_n", &S.dev_n);
  cw_debug_f32_array("sample.text_w", S.text_w, 3);
  cw_debug_u32("sample.rebind_code", &S.rebind_code);
  cw_debug_u32("sample.missing_touch", &S.missing_touch);
}

static void update(void *user, double dt) {
  (void)user, (void)dt;
  /* player 1's device, each change recorded (what the device gates check) */
  int32_t d = (int32_t)cw_prompt_device_of(0);
  if (S.dev_n == 0 || S.dev_hist[(S.dev_n - 1) % 8] != d) S.dev_hist[S.dev_n++ % 8] = d;
  /* a rebind: jump's pad binding becomes the pressed button, this frame */
  cw_binding b;
  if (S.listening && cw_input_capture_poll(&b)) {
    S.listening = false;
    cw_binding bs[CW_INPUT_MAX_BINDINGS];
    int n = cw_action_bindings(S.jump, bs, CW_INPUT_MAX_BINDINGS);
    for (int i = 0; i < n; i++)
      if (bs[i].kind == CW_BIND_PAD_BUTTON || bs[i].kind == CW_BIND_PAD_AXIS) {
        cw_action_set_binding(S.jump, i, b);
        break;
      }
    S.rebound_frame = cw_frame_index();
  }
}

static const cw_color INK = { 0.96f, 0.96f, 0.98f, 1 };

static void label(const char *t, float x, float y, float size, cw_color c) {
  cw_r2d_text(S.sans, t, (cw_vec2){ x, y }, &(cw_text_style){ .size = size, .color = c });
}

/* the glyph sheet: every glyph the engine has, on one strip */
static void sheet(float x0, float y, float size, cw_color bg, cw_color ink, float w) {
  static const char *const binds[] = {
    "pad:south", "pad:east", "pad:west", "pad:north", "pad:lshoulder", "pad:rshoulder", "axis:lt+", "axis:rt+",
    "axis:lx-", "axis:ry+", "pad:lstick", "pad:rstick", "pad:up", "pad:left", "pad:start", "pad:back", "pad:guide",
    "key:space", "key:w", "key:left", "key:return", "mouse:left", "mouse:right", "wheel:up", "touch:0", "touch:1.up",
  };
  cw_r2d_rect(cw_rect_make(x0, y, w, size * 3 + 40), bg);
  float x = x0 + 10, yy = y + 10;
  for (size_t i = 0; i < CW_ARRAY_LEN(binds); i++) {
    cw_binding b;
    if (!cw_binding_parse(binds[i], &b)) continue;
    float gw = cw_prompt_binding_size(b, size).x;
    if (x + gw > x0 + w - 10) x = x0 + 10, yy += size + 10;
    cw_prompt_binding(b, (cw_vec2){ x, yy }, size, ink);
    x += gw + size * 0.35f;
  }
}

/* the diamond at sizes from 10 up, then the other kinds at 12 to 20: where the rings stop being rings */
static void ladder(float x0, float y, cw_color bg, cw_color ink, float w) {
  static const float sizes[] = { 10, 12, 14, 16, 18, 20, 24, 32, 48 };
  cw_r2d_rect(cw_rect_make(x0, y, w, 150), bg);
  float x = x0 + 8;
  cw_binding b;
  cw_binding_parse("pad:east", &b);
  for (size_t i = 0; i < CW_ARRAY_LEN(sizes); i++) {
    cw_prompt_binding(b, (cw_vec2){ x, y + 8 + (48 - sizes[i]) * 0.5f }, sizes[i], ink);
    x += sizes[i] + 8;
  }
  static const char *const kinds[] = { "key:space", "axis:lx-", "pad:left", "pad:lshoulder", "pad:start", "mouse:left" };
  for (size_t k = 0; k < CW_ARRAY_LEN(kinds); k++) {
    if (k % 3 == 0) x = x0 + 8;
    cw_binding_parse(kinds[k], &b);
    float ry = y + 64 + (float)(k / 3) * 42;
    for (int i = 1; i <= 5; i++) {
      float s = sizes[i];
      x += cw_prompt_binding(b, (cw_vec2){ x, ry + (20 - s) * 0.5f }, s, ink).x + 4;
    }
    x += 12;
  }
}

/* one cell of the stick ladder at size s: row r, item i (0 or 1); returns its width */
static float ladder_cell(int r, int i, cw_vec2 at, float s, cw_color ink) {
  static const char *const one[][2] = { { "axis:lx-", "axis:ry+" }, { "pad:lstick", "pad:rstick" }, { "key:j", "key:escape" } };
  cw_action steer[2] = { S.left, S.right }, vert[2] = { S.up, S.down }, four[4] = { S.left, S.right, S.up, S.down },
            look[2] = { S.look_l, S.look_r };
  cw_binding b;
  switch (r) {
  case 0: cw_prompt_force_device(3, CW_PROMPT_PAD); return cw_prompt_actions(i ? vert : steer, 2, 3, at, s, ink).x;
  case 1: cw_prompt_force_device(3, CW_PROMPT_PAD); return cw_prompt_actions(i ? look : four, i ? 2 : 4, 3, at, s, ink).x;
  case 2: case 3: case 5:
    cw_binding_parse(one[r == 2 ? 0 : r == 3 ? 1 : 2][i], &b);
    return cw_prompt_binding(b, at, s, ink).x;
  case 4:
    cw_prompt_force_device(3, CW_PROMPT_TOUCH);
    return cw_prompt_actions(i ? four : steer, i ? 4 : 2, 3, at, s, ink).x;
  default: return 0;
  }
}

/* the stick ladder page (the debug request ladder) */
static void stick_ladder(void) {
  static const float sizes[] = { 16, 20, 24, 32, 48 };
  cw_vec2 cs = cw_r2d_canvas_size();
  label("stick ladder: 16 20 24 32 48", 16, 4, 14, INK);
  for (int half = 0; half < 2; half++) {
    float x0 = half * cs.x * 0.5f, y = 24;
    cw_r2d_rect(cw_rect_make(x0, y, cs.x * 0.5f, 6 * 62 + 8), half ? cw_hex(0xe9e5dcff) : cw_hex(0x15171dff));
    for (int r = 0; r < 6; r++, y += 62) {
      float x = x0 + 8;
      for (int i = 0; i < 2; i++, x += 10)
        for (size_t k = 0; k < CW_ARRAY_LEN(sizes); k++)
          x += ladder_cell(r, i, (cw_vec2){ x, y + 8 + (48 - sizes[k]) * 0.5f }, sizes[k], INK) + 6;
    }
  }
  /* in HUD text: the pad and the keyboard, at 16 and 22 */
  float y = 6 * 62 + 40;
  static const cw_prompt_device devs[2] = { CW_PROMPT_PAD, CW_PROMPT_KEYBOARD };
  for (int i = 0; i < 4; i++) {
    float s = i < 2 ? 16 : 22;
    cw_prompt_force_device(3, devs[i & 1]);
    cw_prompt_text(S.sans, "{move_left|move_right} steer   {fire} fire   {look_left|look_right} look   {pad:rstick} horn", 3,
                   (cw_vec2){ 16, y }, s, INK, CW_ALIGN_LEFT);
    y += s * 1.5f;
  }
  cw_prompt_force_device(3, CW_PROMPT_NONE);
}

static void render(void *user, double alpha) {
  (void)user, (void)alpha;
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x23262fff) });
  if (S.ladder) {
    stick_ladder();
    cw_r2d_end();
    return;
  }
  cw_vec2 cs = cw_r2d_canvas_size();
  float W = cs.x;
  /* the live prompts first: their glyphs lead the prompt.glyphs debug field */
  float y = 330;
  const char *dev = cw_prompt_device_name(cw_prompt_device_of(0));
  label(cw_frame_printf("player 1 device: %s", dev), 16, y, 16, cw_rgba(0.7f, 0.75f, 0.85f, 1));
  /* #region hint */
  cw_prompt_text(S.sans, cw_tr("hint.jump"), 0, (cw_vec2){ 220, y - 4 }, 22, INK, CW_ALIGN_LEFT);
  /* #endregion */
  if (cw_frame_index() == S.rebound_frame && S.rebound_frame) cw_prompt_action_codes(&S.jump, 1, 0, &S.rebind_code, 1);
  y += 30;
  cw_prompt_text(S.sans, S.typo ? "{move_left|move_right|move_up|move_down} move    {jmup} dash" : cw_tr("hint.move"), 0,
                 (cw_vec2){ 220, y }, 22, INK, CW_ALIGN_LEFT);
  y += 30;
  cw_prompt_text(S.sans, cw_tr("hint.menu"), 0, (cw_vec2){ 220, y }, 22, INK, CW_ALIGN_LEFT);
  S.text_w[0] = cw_prompt_text_size(S.sans, cw_tr("hint.jump"), 0, 22).x;
  S.text_w[1] = cw_prompt_text_size(S.sans, cw_tr("hint.move"), 0, 22).x;
  S.text_w[2] = cw_prompt_text_size(S.sans, cw_tr("hint.menu"), 0, 22).x;
  /* every seat reads this one */
  y += 36;
  label("every seat:", 16, y + 3, 16, cw_rgba(0.7f, 0.75f, 0.85f, 1));
  /* #region shared */
  cw_prompt_text(S.sans, "{jump} to join", -1, (cw_vec2){ 220, y }, 22, INK, CW_ALIGN_LEFT);
  /* #endregion */
  /* #region lanes */
  /* lanes laid out by position: west, south, east, left to right */
  cw_prompt_text(S.sans, "lanes: {pad:west}  {pad:south}  {pad:east}", 0, (cw_vec2){ W - 16, y }, 22, INK, CW_ALIGN_RIGHT);
  /* #endregion */
  /* the same actions on every device (player 4 is borrowed and handed back) */
  y += 38;
  static const cw_prompt_device devs[4] = { CW_PROMPT_PAD, CW_PROMPT_KEYBOARD, CW_PROMPT_MOUSE, CW_PROMPT_TOUCH };
  float x = 16;
  for (int i = 0; i < 4; i++) {
    cw_prompt_force_device(3, devs[i]);
    label(cw_prompt_device_name(devs[i]), x, y + 6, 14, cw_rgba(0.7f, 0.75f, 0.85f, 1));
    x += 70;
    cw_action move[2] = { S.left, S.right };
    x += cw_prompt_action(S.jump, 3, (cw_vec2){ x, y }, 26, INK).x + 8;
    x += cw_prompt_actions(move, 2, 3, (cw_vec2){ x, y }, 26, INK).x + 8;
    x += cw_prompt_action(S.fire, 3, (cw_vec2){ x, y }, 26, INK).x + 8;
    cw_vec2 aim = cw_prompt_action(S.aim, 3, (cw_vec2){ x, y }, 26, INK);
    if (devs[i] == CW_PROMPT_TOUCH) S.missing_touch = aim.x > 0;
    x += aim.x + 24;
  }
  cw_prompt_force_device(3, CW_PROMPT_NONE);
  /* the sheet and the size ladder, dark and light */
  label("prompts", 16, 6, 18, INK);
  sheet(0, 30, 30, cw_hex(0x15171dff), INK, W * 0.5f);
  sheet(W * 0.5f, 30, 30, cw_hex(0xe9e5dcff), INK, W * 0.5f);
  ladder(0, 170, cw_hex(0x15171dff), INK, W * 0.5f);
  ladder(W * 0.5f, 170, cw_hex(0xe9e5dcff), INK, W * 0.5f);
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){ .title = "prompts", .width = 960, .height = 540, .init = init, .update = update, .render = render,
                        .debug = true };
}
