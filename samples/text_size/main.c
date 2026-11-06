/* text_size: the player's text scale and the size floor (cartwheel/render2d.h "text size").
 *
 * A game screen of the usual kinds of text over a busy picture: a title drawn as art (fixed_size: drawn as asked,
 * left out of the check), a HUD block laid out from cw_font_measure, a hint line with button prompts, a wrapped
 * message and a smaller secondary line. Every size is a share of the canvas height: body text at
 * CW_TEXT_FLOOR_BODY or over, the secondary line at CW_TEXT_FLOOR_MIN or over, so the frame passes the size check
 * at every resolution and grows at every text scale with nothing overlapping.
 *
 * The debug requests `text scale <k>` and `text backdrop off|outline|panel` (the engine's) change the settings;
 * the sample's own `small 1` adds a line drawn 12 px tall at 1080 lines (1.1% of the canvas height), under the
 * floor: tests/text-floor.mjs runs it as the control the check must fail. Native env TEXT_SIZE_SMALL=1 does the
 * same from the start. Debug fields: sample.hud_h (the HUD block's measured height), sample.small. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/platform.h"
#include "cartwheel/prompt.h"
#include "cartwheel/render2d.h"
#include "cartwheel/ui.h"

static struct {
  cw_font font;
  cw_action jump, fire;
  int32_t small;
  float hud_h;
} S;

static void cmd_small(int argc, const char **argv, cw_reply *r) {
  if (argc > 1) S.small = atoi(argv[1]) != 0;
  cw_reply_printf(r, "{\"small\":%d}", S.small);
}

static void init(void *user) {
  (void)user;
  S.font = cw_ui_font_sans();
  S.jump = cw_action_add("jump", 0.5f);
  cw_action_bind_text(S.jump, "key:space, pad:south");
  S.fire = cw_action_add("fire", 0.5f);
  cw_action_bind_text(S.fire, "key:f, pad:west");
  const char *e = getenv("TEXT_SIZE_SMALL");
  S.small = e && atoi(e);
  cw_debug_i32("sample.small", &S.small);
  cw_debug_f32("sample.hud_h", &S.hud_h);
  cw_debug_command("small", "small [1|0]: a line under the size floor (the text-floor control)", cmd_small);
}

static void fixed_update(void *user, double dt) { (void)user, (void)dt; }

static void render(void *user, double alpha) {
  (void)user, (void)alpha;
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x203040ff) });
  cw_vec2 sz = cw_r2d_canvas_size();
  float h = sz.y, pad = h * 0.03f;
  /* a busy picture behind the text: the case the Text background setting is for */
  for (int i = 0; i < 24; i++) {
    float x = fmodf((float)i * 0.137f, 1.0f) * sz.x, y = fmodf((float)i * 0.291f, 1.0f) * sz.y;
    cw_r2d_circle(cw_v2(x, y), h * (0.05f + 0.03f * (float)(i % 4)), 0, i % 3 ? cw_hex(0xe8e0c8ff) : cw_hex(0x6a8a50ff));
  }
  /* the title is art: drawn as asked at every scale, not checked */
  cw_text_style title = { .size = h * 0.11f, .color = cw_hex(0xffd060ff), .outline_width = h * 0.008f, .fixed_size = true };
  cw_text_metrics tm = cw_font_measure(S.font, "TEXT SIZE", &title);
  cw_r2d_text(S.font, "TEXT SIZE", cw_v2((sz.x - tm.width) * 0.5f, h * 0.44f), &title);

  /* the HUD block: each line placed under the last by its measured height, so it grows with the scale */
  cw_text_style body = { .size = h * 0.026f, .color = CW_WHITE };
  cw_text_style small = { .size = h * 0.019f, .color = cw_rgba(1, 1, 1, 0.85f) };
  const char *lines[3] = { "Score 12400", "Lap 2 / 3", "Best 1:02.48" };
  float y = pad;
  for (int i = 0; i < 3; i++) y += cw_r2d_text(S.font, lines[i], cw_v2(pad, y), &body).height;
  y += cw_r2d_text(S.font, "secondary: 18 px at 1080 lines", cw_v2(pad, y), &small).height;
  S.hud_h = y - pad;

  /* a wrapped message in a box of fixed width: more lines when the text is larger */
  cw_text_style msg = { .size = h * 0.026f, .color = CW_WHITE, .wrap_width = sz.x * 0.36f, .own_backdrop = true };
  cw_text_metrics mm = cw_font_measure(S.font, "Checkpoint reached. The next gate opens when every buoy is cleared.", &msg);
  cw_rect box = cw_rect_make(sz.x - pad - sz.x * 0.36f - pad, pad, sz.x * 0.36f + 2 * pad, mm.height + 2 * pad);
  cw_r2d_round_rect(box, pad * 0.6f, 0, cw_rgba(0, 0, 0, 0.6f));
  cw_r2d_text(S.font, "Checkpoint reached. The next gate opens when every buoy is cleared.", cw_v2(box.x + pad, box.y + pad), &msg);

  /* the hint line: prompts scale with the text */
  float hs = h * 0.028f;
  cw_vec2 hz = cw_prompt_text_size(S.font, "{jump}: hop    {fire}: throw", 0, hs);
  cw_prompt_text(S.font, "{jump}: hop    {fire}: throw", 0, cw_v2(sz.x * 0.5f, h - pad - hz.y), hs, CW_WHITE, CW_ALIGN_CENTER);

  if (S.small) { /* the control: 12 px at 1080 lines */
    cw_text_style tiny = { .size = h * (12.0f / 1080.0f), .color = CW_WHITE };
    cw_r2d_text(S.font, "too small to read on a handheld", cw_v2(pad, h * 0.6f), &tiny);
  }
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){ .title = "cartwheel text size", .width = 1280, .height = 720, .init = init, .fixed_update = fixed_update,
                        .render = render, .debug = true };
}
