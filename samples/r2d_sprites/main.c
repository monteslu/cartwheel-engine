/* r2d_sprites: the 2D batcher's sprite and shape surface in one frame.
 * Atlas sprites with rotation, flips, scale, pivot, alpha and tint; additive
 * sprites; nine-slice panels; lines, polylines, rects, circles, a concave
 * polygon; multiply blend; textured quads with arbitrary UVs (a JPEG, and a
 * repeat texture); a clip rect; a y-sorted layer; and a render target drawn
 * into in its own pass, then drawn as a rotating sprite.
 * Everything is a function of the tick count, so every host renders the same
 * frame N. */
#include <math.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"

typedef struct {
  cw_texture sheet, photo, checker, rt;
  int32_t ticks;
  int32_t tri_indices;
  uint32_t sheet_w, frame_batches;
} app_t;
static app_t app;

static void init(void *user) {
  app_t *a = user;
  if (!cw_r2d_init()) CW_FATAL("render2d init failed");
  a->sheet = cw_texture_load("images/sheet.png", &(cw_texture_desc){ .filter = CW_FILTER_NEAREST });
  a->photo = cw_texture_load("images/photo.jpg", NULL);
  /* a 2x2 checker for repeat wrap, made in code */
  uint8_t ck[16] = { 255, 255, 255, 255, 40, 40, 60, 255, 40, 40, 60, 255, 255, 255, 255, 255 };
  a->checker = cw_texture_from_pixels(2, 2, ck, &(cw_texture_desc){ .filter = CW_FILTER_NEAREST, .repeat = true });
  a->rt = cw_target_create(&(cw_target_desc){ .width = 96, .height = 96, .filter = CW_FILTER_NEAREST });
  int w = 0;
  cw_texture_size(a->sheet, &w, NULL);
  a->sheet_w = (uint32_t)w;
  /* layer 1: y-sorted critters */
  cw_r2d_layer_set(1, &(cw_r2d_layer_desc){ .y_sort = true });
  cw_vec2 star[10];
  for (int i = 0; i < 10; i++) {
    float r = i % 2 ? 12.0f : 30.0f, t = (float)i * CW_PI / 5;
    star[i] = (cw_vec2){ cosf(t) * r, sinf(t) * r };
  }
  uint32_t idx[30];
  a->tri_indices = cw_triangulate(star, 10, idx);
  cw_debug_i32("ticks", &a->ticks);
  cw_debug_i32("star.indices", &a->tri_indices);
  cw_debug_u32("sheet.w", &a->sheet_w);
  cw_r2d_debug_register("r2d");
}

static void fixed_update(void *user, double dt) {
  (void)dt;
  ((app_t *)user)->ticks++;
}

static cw_rect frame_src(int f) { return (cw_rect){ (float)(f * 32), 0, 32, 32 }; }

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  float t = (float)a->ticks / 60.0f;
  int frame = (a->ticks / 8) % 4;

  /* pass 1: into the render target */
  cw_r2d_begin(&(cw_r2d_pass_desc){ .target = a->rt, .clear = true, .clear_color = cw_hex(0x203040ff) });
  cw_r2d_circle((cw_vec2){ 48, 48 }, 40, 0, cw_hex(0x305070ff));
  cw_r2d_rect_lines((cw_rect){ 2, 2, 92, 92 }, 3, cw_hex(0xffcc00ff));
  cw_r2d_sprite(&(cw_sprite){ .texture = a->sheet, .src = frame_src(frame), .pos = { 48, 48 }, .pivot = { 0.5f, 0.5f },
                              .scale = { 2, 2 } });
  cw_r2d_rect((cw_rect){ 4, 4, 20, 10 }, cw_hex(0xff3040ff)); /* marks the target's top-left */
  cw_r2d_end();

  /* pass 2: the window */
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x2a2d3aff) });
  /* row 1: critters with rotation / flips / scale / alpha / tint */
  for (int i = 0; i < 6; i++) {
    cw_sprite s = { .texture = a->sheet, .src = frame_src((frame + i) % 4), .pos = { 40.0f + (float)i * 56, 40 },
                    .pivot = { 0.5f, 0.5f } };
    if (i == 1) s.flip_x = true;
    if (i == 2) s.flip_y = true;
    if (i == 3) s.rotation = t * 2.0f;
    if (i == 4) s.scale = (cw_vec2){ 1.5f, 0.75f }, s.color = cw_rgba(1, 1, 1, 0.5f);
    if (i == 5) s.color = cw_hex(0x60a0ffff), s.rotation = -0.5f, s.pivot = (cw_vec2){ 0, 0 };
    cw_r2d_sprite(&s);
  }
  /* additive gems over a dark rect */
  cw_r2d_rect((cw_rect){ 350, 10, 120, 60 }, cw_hex(0x101018ff));
  cw_r2d_set_blend(CW_BLEND_ADD);
  for (int i = 0; i < 5; i++)
    cw_r2d_sprite(&(cw_sprite){ .texture = a->sheet, .src = { 32, 32, 16, 16 }, .pos = { 370.0f + (float)i * 18, 40 },
                                .pivot = { 0.5f, 0.5f }, .scale = { 2.5f, 2.5f } });
  cw_r2d_set_blend(CW_BLEND_ALPHA);
  /* nine-slice panels: wide, tall, smaller than its margins, hollow */
  cw_nine_slice ns = { .texture = a->sheet, .src = { 0, 32, 32, 32 }, .left = 6, .top = 6, .right = 6, .bottom = 6 };
  ns.dst = (cw_rect){ 480, 10, 150, 50 };
  cw_r2d_nine_slice(&ns);
  ns.dst = (cw_rect){ 480, 66, 40, 90 };
  ns.margin_scale = 2;
  cw_r2d_nine_slice(&ns);
  ns.dst = (cw_rect){ 530, 66, 8, 8 };
  ns.margin_scale = 1;
  cw_r2d_nine_slice(&ns);
  ns.dst = (cw_rect){ 550, 66, 80, 60 };
  ns.hollow = true;
  ns.color = cw_hex(0xffffffc0);
  cw_r2d_nine_slice(&ns);

  /* row 2: shapes */
  cw_r2d_rect((cw_rect){ 10, 90, 50, 40 }, cw_hex(0xd04040ff));
  cw_r2d_rect_lines((cw_rect){ 70, 90, 50, 40 }, 4, cw_hex(0x40d040ff));
  cw_r2d_line((cw_vec2){ 130, 90 }, (cw_vec2){ 180, 130 }, 3, cw_hex(0xe0e0e0ff));
  cw_vec2 zig[6] = { { 190, 130 }, { 205, 92 }, { 220, 130 }, { 235, 92 }, { 260, 125 }, { 290, 95 } };
  cw_r2d_polyline(zig, 6, 5, false, cw_hex(0xffb030ff));
  cw_r2d_circle((cw_vec2){ 320, 110 }, 20, 0, cw_hex(0x3080ffff));
  cw_r2d_circle_lines((cw_vec2){ 370, 110 }, 18, 3, 0, cw_hex(0xff60c0ff));
  cw_vec2 star[10];
  for (int i = 0; i < 10; i++) {
    float r = i % 2 ? 9.0f : 22.0f, an = (float)i * CW_PI / 5 + t * 0.5f;
    star[i] = (cw_vec2){ 425 + cosf(an) * r, 110 + sinf(an) * r };
  }
  cw_r2d_polygon(star, 10, cw_hex(0xf0e040ff));
  cw_r2d_polygon_lines(star, 10, 1.5f, cw_hex(0x806000ff));
  /* batched shapes: one submission each */
  cw_r2d_round_rect((cw_rect){ 456, 88, 40, 20 }, 7, 0, cw_hex(0x40c0a0ff));
  cw_r2d_round_rect_lines((cw_rect){ 456, 112, 40, 20 }, 7, 3, 0, cw_hex(0xc0a0ffff));
  cw_r2d_arc((cw_vec2){ 522, 110 }, 9, 17, -CW_PI * 0.5f, CW_PI * 0.75f + t, 0, cw_hex(0xff8040ff));
  cw_r2d_arc((cw_vec2){ 522, 110 }, 0, 6, 0, CW_PI * 1.5f, 0, cw_hex(0x80ff60ff));

  /* multiply: darkens what is under it */
  cw_r2d_set_blend(CW_BLEND_MULTIPLY);
  cw_r2d_rect((cw_rect){ 0, 100, 640, 12 }, cw_hex(0x8080ffff));
  cw_r2d_set_blend(CW_BLEND_ALPHA);

  /* row 3: textured quads with arbitrary UVs: a skewed JPEG, a repeat texture */
  cw_vec2 qp[4] = { { 20, 160 }, { 120, 150 }, { 140, 240 }, { 10, 230 } };
  cw_vec2 quv[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
  cw_r2d_quad(a->photo, qp, quv, CW_WHITE);
  cw_vec2 rp[4] = { { 160, 150 }, { 260, 150 }, { 260, 230 }, { 160, 230 } };
  cw_vec2 ruv[4] = { { 0, 0 }, { 5, 0 }, { 5, 4 }, { 0, 4 } };
  cw_r2d_quad(a->checker, rp, ruv, cw_hex(0xffd0a0ff));
  /* the render target, drawn rotating */
  cw_r2d_sprite(&(cw_sprite){ .texture = a->rt, .pos = { 330, 200 }, .pivot = { 0.5f, 0.5f }, .rotation = t * 0.7f });
  /* clip: sprites crossing the clip edge are cut */
  cw_r2d_rect_lines((cw_rect){ 409, 159, 82, 82 }, 1, cw_hex(0xffffffff));
  cw_r2d_push_clip((cw_rect){ 410, 160, 80, 80 });
  for (int i = 0; i < 4; i++)
    cw_r2d_sprite(&(cw_sprite){ .texture = a->sheet, .src = frame_src(i), .pos = { 400.0f + (float)i * 30 + fmodf(t * 20, 30), 175.0f + (float)i * 18 },
                                .scale = { 1.5f, 1.5f } });
  cw_r2d_pop_clip();

  /* layer 1, y-sorted: submitted bottom-first, drawn top-first */
  cw_r2d_use_layer(1);
  for (int i = 5; i >= 0; i--)
    cw_r2d_sprite(&(cw_sprite){ .texture = a->sheet, .src = frame_src(i % 4), .pos = { 520.0f + (float)(i % 2) * 14, 170.0f + (float)i * 12 },
                                .pivot = { 0.5f, 1.0f }, .scale = { 2, 2 }, .color = i == 3 ? cw_hex(0xff8080ff) : CW_WHITE });
  cw_r2d_use_layer(0);
  /* bottom strip: big sprites, flips + pivot sweep */
  for (int i = 0; i < 8; i++)
    cw_r2d_sprite(&(cw_sprite){ .texture = a->sheet, .src = { 64, 32, 32, 32 }, .pos = { 40.0f + (float)i * 75, 310 },
                                .pivot = { 0.5f, 0.5f }, .rotation = (float)i * CW_PI / 4, .flip_x = i % 2, .scale = { 1.5f, 1.5f } });
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "r2d sprites", .width = 640, .height = 360, .init = init, .fixed_update = fixed_update,
                        .render = render, .debug = true, .user = &app };
}
