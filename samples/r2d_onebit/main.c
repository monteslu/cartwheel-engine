/* r2d_onebit: a game's own HUD in two colours (cw_r2d_set_one_bit, cw_r2d_set_one_bit_paper, cw_prompt_set_two_colour).
 *
 * A paper canvas with a one-bit picture on its right (a checker of whole pixels), and over both a HUD in ink: a score
 * line with an outline and a drop shadow of its own (black and grey in the style: the paper takes their place), plain
 * text, text with pad prompts in it, prompt glyphs alone, and shapes (a circle, a ring, a rounded box, a slanted line,
 * a star) and a rotated, scaled sprite whose texture is an ink mask. With one-bit edges every pixel is ink or paper.
 *
 * ONEBIT (cw_test_env, read every frame):
 *   unset   one-bit on (from frame 2, when the switches have reached the cart on every host), the paper for text
 *           outlines and shadows, the prompts in ink and paper
 *   0       the setters called off every frame (cw_r2d_set_one_bit(false), paper {0}, cw_prompt_set_two_colour(false)):
 *           the frame must be the one drawn with none of them called (ONEBIT=none)
 *   none    none of them called (blended edges, the styles' own outline and shadow colours)
 *   ref     no one-bit: the styles' outline and shadow drawn in paper and the prompts in two colours, run with
 *           CW_TEST_R2D_HARD_OFF=1 (the blended reference of the same picture, for the readability check)
 * ONEBIT_INK, ONEBIT_PAPER: hex RRGGBB (default f0e8c8 on 182028, the shell's two-colour defaults).
 *
 * Debug fields: onebit.on (cw_r2d_one_bit after the HUD's setters), onebit.prompt (cw_prompt_two_colour),
 * onebit.lines (text lines drawn). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/platform.h"
#include "cartwheel/prompt.h"
#include "cartwheel/render2d.h"
#include "cartwheel/ui.h"

static struct {
  cw_font sans;
  cw_texture mask;
  cw_action jump, dash, left, right;
  int32_t on, prompt, lines;
} G;

static uint32_t hex_env(const char *name, uint32_t def) {
  const char *v = cw_test_env(name);
  return v && *v ? (uint32_t)strtoul(v, NULL, 16) & 0xffffffu : def;
}
static cw_color ink(void) { return cw_hex(hex_env("ONEBIT_INK", 0xf0e8c8) << 8 | 0xff); }
static cw_color paper(void) { return cw_hex(hex_env("ONEBIT_PAPER", 0x182028) << 8 | 0xff); }
/* the switches reach a cart at frame 2 on every host: nothing is set before then (a "none" run sets nothing at all) */
static const char *mode(void) {
  if (cw_frame_index() < 2) return "early";
  const char *v = cw_test_env("ONEBIT");
  return v ? v : "";
}

static void init(void *user) {
  (void)user;
  G.sans = cw_ui_font_sans();
  G.jump = cw_action_add("jump", 0.5f);
  cw_action_bind_text(G.jump, "key:space, pad:south");
  G.dash = cw_action_add("dash", 0.5f);
  cw_action_bind_text(G.dash, "key:lshift, pad:west");
  G.left = cw_action_add("move_left", 0.3f);
  cw_action_bind_text(G.left, "key:left, pad:left, axis:lx-");
  G.right = cw_action_add("move_right", 0.3f);
  cw_action_bind_text(G.right, "key:right, pad:right, axis:lx+");
  /* a 32 x 32 ink mask: an arrow head, white where it is, clear elsewhere (the sprite's colour is the ink) */
  static uint8_t px[32 * 32 * 4];
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) {
      bool in = x >= 4 && x < 28 && abs(y - 16) <= (x - 4) / 2 && !(x > 18 && abs(y - 16) < 3);
      uint8_t *p = px + (y * 32 + x) * 4;
      p[0] = p[1] = p[2] = 255;
      p[3] = in ? 255 : 0;
    }
  G.mask = cw_texture_from_pixels(32, 32, px, NULL);
  cw_debug_i32("onebit.on", &G.on);
  cw_debug_i32("onebit.prompt", &G.prompt);
  cw_debug_i32("onebit.lines", &G.lines);
}

static void update(void *user, double dt) { (void)user, (void)dt; }

static void render(void *user, double alpha) {
  (void)user, (void)alpha;
  const char *m = mode();
  bool on = !*m || !strcmp(m, "1"), ref = !strcmp(m, "ref"), zero = !strcmp(m, "0");
  cw_color I = ink(), P = paper();
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = P });
  cw_vec2 sz = cw_r2d_canvas_size();
  /* the one-bit picture: a checker of 6-pixel cells on the right third */
  float cx0 = floorf(sz.x * 0.62f / 6) * 6;
  for (int y = 0; y * 6 < (int)sz.y; y++)
    for (int x = (int)(cx0 / 6) + (y & 1); x * 6 < (int)sz.x; x += 2) cw_r2d_rect(cw_rect_make((float)x * 6, (float)y * 6, 6, 6), I);

  /* the HUD's setters (what a two-colour game calls at the top of its HUD) */
  if (on) {
    cw_r2d_set_one_bit(true);
    cw_r2d_set_one_bit_paper(P);
    cw_prompt_set_two_colour(true, I, P);
  } else if (zero) {
    cw_r2d_set_one_bit(false);
    cw_r2d_set_one_bit_paper((cw_color){ 0 });
    cw_prompt_set_two_colour(false, (cw_color){ 0 }, (cw_color){ 0 });
  } else if (ref) cw_prompt_set_two_colour(true, I, P);
  G.on = cw_r2d_one_bit();
  G.prompt = cw_prompt_two_colour();

  /* text: an outline and a drop shadow of the style's own (paper in their place while one-bit), plain, prompts */
  float x = 16, y = 12, s = 18;
  cw_color outline = ref ? P : cw_rgba(0, 0, 0, 1), shadow = ref ? P : cw_rgba(0.4f, 0.4f, 0.45f, 0.8f);
  cw_text_metrics t = cw_r2d_text(G.sans, "SCORE 012340   LIVES 3   STAGE 4-2   TIME 271", cw_v2(x, y),
                                  &(cw_text_style){ .size = s, .color = I, .outline_width = s * 0.08f, .outline_color = outline,
                                                    .shadow_color = shadow, .shadow_offset = cw_v2(2, 2) });
  y += t.height + 8;
  t = cw_r2d_text(G.sans, "One-bit text over paper and the picture alike", cw_v2(x, y), &(cw_text_style){ .size = s * 0.9f, .color = I });
  y += t.height + 8;
  cw_vec2 b = cw_prompt_text(G.sans, "{jump} Jump   {dash} Dash   {move_left|move_right} Move", 0, cw_v2(x, y), s * 0.9f, I, CW_ALIGN_LEFT);
  y += b.y + 8;
  G.lines = 3;
  /* glyphs alone */
  float gs = 28, gx = x;
  gx += cw_prompt_action(G.jump, 0, cw_v2(gx, y), gs, I).x + 10;
  gx += cw_prompt_action(G.dash, 0, cw_v2(gx, y), gs, I).x + 10;
  cw_action pair[2] = { G.left, G.right };
  gx += cw_prompt_actions(pair, 2, 0, cw_v2(gx, y), gs, I).x + 10;
  y += gs + 14;

  /* shapes and a sprite, below the text (and over the picture on the right, ink on its ink cells, as a game draws) */
  float r = 22, sy = fmaxf(y + r, sz.y - 70);
  cw_r2d_circle(cw_v2(40, sy), r, 0, I);
  cw_r2d_circle_lines(cw_v2(100, sy), r, 3.5f, 0, I);
  cw_r2d_round_rect(cw_rect_make(140, sy - r, 70, 2 * r), 9, 0, I);
  cw_r2d_line(cw_v2(225, sy + r), cw_v2(290, sy - r), 3.2f, I);
  cw_vec2 star[10];
  for (int i = 0; i < 10; i++) {
    float a = (float)i * 3.14159265f / 5 - 1.5707963f, rr = i & 1 ? r * 0.45f : r;
    star[i] = cw_v2(330 + cosf(a) * rr, sy + sinf(a) * rr);
  }
  cw_r2d_polygon(star, 10, I);
  cw_r2d_sprite(&(cw_sprite){ .texture = G.mask, .pos = cw_v2(395, sy), .size = cw_v2(44, 44), .pivot = cw_v2(0.5f, 0.5f),
                              .rotation = 0.37f, .color = I });
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){
    .title = "cartwheel one-bit 2D",
    .width = 640,
    .height = 360,
    .init = init,
    .update = update,
    .render = render,
    .debug = true,
    .user = &G,
  };
}
