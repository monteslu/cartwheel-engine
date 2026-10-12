/* r2d_lights: the 2D light pass. A lit layer of normal-mapped bricks (the
 * texture and its normal map are generated here from one height field) and
 * flat-lit critters, three moving coloured point lights (two cast shadows
 * from occluder polygons, one does not), a dim ambient, and an unlit HUD
 * layer on top that the lights never touch. */
#include <math.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"

enum { L_WORLD, L_HUD };
#define BW 64

typedef struct {
  cw_texture bricks, sheet;
  cw_font pixel;
  int32_t ticks;
  uint8_t normal_ok;
} app_t;
static app_t app;

/* height of the brick pattern at (x, y): mortar grooves, bevelled edges */
static float brick_height(int x, int y) {
  int row = y / 16, bx = (x + (row & 1) * 16) % 32, by = y % 16;
  float ex = (float)CW_MIN(bx, 31 - bx), ey = (float)CW_MIN(by, 15 - by);
  float e = CW_MIN(ex, ey);
  return e < 1 ? 0.0f : e < 4 ? (e - 1) / 3.0f : 1.0f;
}

static void make_bricks(app_t *a) {
  static uint8_t col[BW * BW * 4], nrm[BW * BW * 4];
  for (int y = 0; y < BW; y++)
    for (int x = 0; x < BW; x++) {
      float h = brick_height(x, y);
      uint8_t *c = col + 4 * (y * BW + x);
      uint32_t n = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
      int v = (int)((n >> 7) % 24);
      c[0] = (uint8_t)(h > 0 ? 150 + v : 70), c[1] = (uint8_t)(h > 0 ? 80 + v / 2 : 66), c[2] = (uint8_t)(h > 0 ? 60 : 60), c[3] = 255;
      /* normal from the height gradient (wrapping), +y up in the image */
      float dx = brick_height((x + 1) % BW, y) - brick_height((x + BW - 1) % BW, y);
      float dy = brick_height(x, (y + 1) % BW) - brick_height(x, (y + BW - 1) % BW);
      float nx = -dx * 2.0f, ny = dy * 2.0f, nz = 1.0f, l = sqrtf(nx * nx + ny * ny + nz * nz);
      uint8_t *o = nrm + 4 * (y * BW + x);
      o[0] = (uint8_t)((nx / l * 0.5f + 0.5f) * 255), o[1] = (uint8_t)((ny / l * 0.5f + 0.5f) * 255);
      o[2] = (uint8_t)((nz / l * 0.5f + 0.5f) * 255), o[3] = 255;
    }
  a->bricks = cw_texture_from_pixels(BW, BW, col, NULL);
  a->normal_ok = cw_texture_set_normal_map(a->bricks, nrm);
}

static void init(void *user) {
  app_t *a = user;
  make_bricks(a);
  a->sheet = cw_texture_load("images/sheet.png", &(cw_texture_desc){ .filter = CW_FILTER_NEAREST });
  a->pixel = cw_font_load("fonts/PressStart2P-Regular.ttf", &(cw_font_desc){ .mode = CW_FONT_BITMAP, .bake_size = 8, .threshold = true });
  cw_r2d_layer_set(L_WORLD, &(cw_r2d_layer_desc){ .lit = true });
  cw_r2d_layer_set(L_HUD, NULL);
  cw_debug_i32("ticks", &a->ticks);
  cw_debug_field("normal_ok", &a->normal_ok, CW_DBG_U8, 1);
  cw_r2d_debug_register("r2d");
}

static void fixed_update(void *user, double dt) {
  (void)dt;
  ((app_t *)user)->ticks++;
}

static const cw_vec2 BOX[4] = { { 0, 0 }, { 60, 0 }, { 60, 40 }, { 0, 40 } };

static void box_at(cw_vec2 o, cw_vec2 out[4]) {
  for (int i = 0; i < 4; i++) out[i] = cw_v2_add(o, BOX[i]);
}

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  float t = (float)a->ticks / 60.0f;
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x000000ff), .ambient = cw_rgba(0.12f, 0.12f, 0.18f, 1) });
  cw_r2d_use_layer(L_WORLD);
  for (int y = 0; y < 6; y++)
    for (int x = 0; x < 10; x++)
      cw_r2d_sprite(&(cw_sprite){ .texture = a->bricks, .pos = { (float)x * BW, (float)y * BW } });
  /* occluders: boxes and a triangle, drawn and registered */
  cw_vec2 boxes[3] = { { 140, 120 }, { 420, 80 }, { 300, 250 } };
  for (int i = 0; i < 3; i++) {
    cw_vec2 p[4];
    box_at(boxes[i], p);
    cw_r2d_polygon(p, 4, cw_hex(0x505860ff));
    cw_r2d_occluder(p, 4, true);
  }
  cw_vec2 tri[3] = { { 520, 230 }, { 590, 300 }, { 500, 320 } };
  cw_r2d_polygon(tri, 3, cw_hex(0x605050ff));
  cw_r2d_occluder(tri, 3, true);
  /* flat-lit critters */
  for (int i = 0; i < 5; i++)
    cw_r2d_sprite(&(cw_sprite){ .texture = a->sheet, .src = { (float)(i % 4) * 32, 0, 32, 32 }, .pos = { 60.0f + (float)i * 110, 320 },
                                .pivot = { 0.5f, 0.5f }, .scale = { 1.5f, 1.5f } });
  /* lights */
  cw_light2d lights[3] = {
    { .pos = { 320 + 200 * cosf(t * 0.8f), 180 + 110 * sinf(t * 0.8f) }, .radius = 260, .color = cw_hex(0xffd090ff), .energy = 1.6f, .shadows = true },
    { .pos = { 120 + 60 * sinf(t), 60 }, .radius = 200, .color = cw_hex(0x60a0ffff), .energy = 1.2f, .shadows = true },
    { .pos = { 540, 120 + 60 * sinf(t * 1.3f) }, .radius = 150, .color = cw_hex(0xff4060ff), .energy = 1.4f, .falloff = 1 },
  };
  for (int i = 0; i < 3; i++) cw_r2d_light(&lights[i]);
  /* unlit HUD: light markers + a label */
  cw_r2d_use_layer(L_HUD);
  for (int i = 0; i < 3; i++) cw_r2d_circle_lines(lights[i].pos, 6, 2, 0, lights[i].color);
  cw_r2d_rect((cw_rect){ 0, 0, 640, 18 }, cw_hex(0x000000c0));
  cw_r2d_text(a->pixel, "2D LIGHTS: NORMAL MAPS + SHADOWS", (cw_vec2){ 6, 5 }, &(cw_text_style){ .size = 8 });
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "r2d lights", .width = 640, .height = 360, .init = init, .fixed_update = fixed_update,
                        .render = render, .debug = true, .user = &app };
}
