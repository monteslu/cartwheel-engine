/* r2d_text: SDF text (Fira Sans, OFL) at several sizes from one bake, with
 * kerning; a paragraph wrapped into boxes with left / centre / right
 * alignment, boxes sized from cw_font_measure; a bitmap pixel font (Press
 * Start 2P, OFL) at integer multiples; rotated SDF text through the local
 * transform. Measurements are published as debug fields, so every host must
 * lay text out identically, not just draw it alike. */
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"

static const char *PARA =
    "Cartwheel draws text from signed distance fields: one bake serves every size. Long lines wrap at spaces, "
    "and a word wider than the box breaks between letters: Pneumonoultramicroscopic.";

typedef struct {
  cw_font sans, pixel;
  int32_t ticks;
  float widths[6];
  int32_t lines[3];
  float kern_av, adv_a_plus_v, caret;
} app_t;
static app_t app;

static void init(void *user) {
  app_t *a = user;
  a->sans = cw_font_load("fonts/FiraSans-Regular.ttf", &(cw_font_desc){ .mode = CW_FONT_SDF, .bake_size = 48 });
  a->pixel = cw_font_load("fonts/PressStart2P-Regular.ttf",
                          &(cw_font_desc){ .mode = CW_FONT_BITMAP, .bake_size = 8, .threshold = true });
  const char *probe[6] = { "AVATAR", "Wa To", "Hello, world!", "iiii", "WWWW", "x" };
  for (int i = 0; i < 6; i++) a->widths[i] = cw_font_measure(a->sans, probe[i], &(cw_text_style){ .size = 32 }).width;
  for (int i = 0; i < 3; i++)
    a->lines[i] = cw_font_measure(a->sans, PARA, &(cw_text_style){ .size = 15, .wrap_width = 120.0f + 80.0f * (float)i }).lines;
  a->kern_av = cw_font_measure(a->sans, "AV", &(cw_text_style){ .size = 100 }).width;
  a->adv_a_plus_v = cw_font_advance(a->sans, 'A', 100) + cw_font_advance(a->sans, 'V', 100);
  a->caret = cw_font_caret_x(a->sans, "Hello, world!", 5, 32);
  cw_debug_i32("ticks", &a->ticks);
  cw_debug_f32_array("measure.widths", a->widths, 6);
  cw_debug_field("measure.lines", a->lines, CW_DBG_I32, 3);
  cw_debug_f32("measure.kern_av", &a->kern_av);
  cw_debug_f32("measure.adv_a_plus_v", &a->adv_a_plus_v);
  cw_debug_f32("measure.caret", &a->caret);
  cw_r2d_debug_register("r2d");
}

static void fixed_update(void *user, double dt) {
  (void)dt;
  ((app_t *)user)->ticks++;
}

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x1c1e26ff) });
  /* sizes */
  float y = 6;
  static const float sizes[] = { 11, 14, 18, 24, 36, 52 };
  for (int i = 0; i < 6; i++) {
    cw_text_metrics m =
        cw_r2d_text(a->sans, i < 5 ? "The quick brown fox: AV Wa To 0123" : "Kerning AVATAR",
                    (cw_vec2){ 8, y }, &(cw_text_style){ .size = sizes[i], .color = cw_hex(0xf0f0f0ff) });
    y += m.height;
  }
  /* wrapped paragraph, three alignments; boxes from measure */
  static const cw_text_align al[3] = { CW_ALIGN_LEFT, CW_ALIGN_CENTER, CW_ALIGN_RIGHT };
  for (int i = 0; i < 3; i++) {
    cw_text_style st = { .size = 13, .wrap_width = 150, .align = al[i], .color = cw_hex(0xffe0a0ff) };
    cw_text_metrics m = cw_font_measure(a->sans, PARA, &st);
    cw_rect box = { 8.0f + (float)i * 160, 200, 150, m.height };
    cw_r2d_rect(box, cw_hex(0x2c3040ff));
    cw_r2d_text(a->sans, PARA, (cw_vec2){ box.x, box.y }, &st);
  }
  /* pixel font at 1x, 2x, 3x */
  cw_r2d_text(a->pixel, "PIXEL 1X: Hello!", (cw_vec2){ 500, 8 }, &(cw_text_style){ .size = 8, .color = cw_hex(0x80ff80ff) });
  cw_r2d_text(a->pixel, "2X: Ready", (cw_vec2){ 490, 22 }, &(cw_text_style){ .size = 16, .color = cw_hex(0x80c0ffff) });
  cw_r2d_text(a->pixel, "3X", (cw_vec2){ 490, 44 }, &(cw_text_style){ .size = 24, .color = cw_hex(0xff8080ff) });
  /* outline and drop shadow: one layout, drawn under the fill (SDF: grown in
   * the shader; bitmap: stamped at whole-pixel offsets) */
  cw_r2d_text(a->sans, "Outline", (cw_vec2){ 492, 196 },
              &(cw_text_style){ .size = 26, .color = cw_hex(0xffe060ff), .outline_width = 2.5f, .outline_color = cw_hex(0x402000ff) });
  cw_r2d_text(a->sans, "Shadow", (cw_vec2){ 492, 230 },
              &(cw_text_style){ .size = 26, .color = cw_hex(0xf0f0f0ff), .shadow_color = cw_hex(0x000000b0),
                                .shadow_offset = { 2, 3 }, .shadow_spread = 1, .shadow_softness = 2 });
  cw_r2d_text(a->pixel, "1PX", (cw_vec2){ 560, 44 },
              &(cw_text_style){ .size = 16, .color = cw_hex(0xffffffff), .outline_width = 1, .outline_color = cw_hex(0x2040a0ff) });
  /* rotated text through the local transform */
  float rot = -0.3f + 0.2f * (float)(a->ticks % 120) / 120.0f;
  cw_xform2d m = cw_xform2d_trs((cw_vec2){ 540, 160 }, rot, (cw_vec2){ 1, 1 });
  cw_r2d_set_transform(&m);
  cw_r2d_text(a->sans, "rotated", (cw_vec2){ -40, -14 }, &(cw_text_style){ .size = 28, .color = cw_hex(0x60e0ffff) });
  cw_r2d_set_transform(NULL);
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "r2d text", .width = 640, .height = 360, .init = init, .fixed_update = fixed_update,
                        .render = render, .debug = true, .user = &app };
}
