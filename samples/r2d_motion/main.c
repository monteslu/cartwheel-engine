/* r2d_motion: what moves and what scales in the 2D renderer.
 *
 *   wind      a meadow of grass blades and reeds submitted with
 *             cw_r2d_set_sway, bending in the pass's wind (cw_r2d_wind):
 *             the vertex stage moves their top edges, the CPU does nothing
 *   trails    comets on looping paths drawn with cw_r2d_trail: tapering,
 *             fading ribbons, additive and at intensity 3 under the post
 *             chain so their heads bloom; a textured trail with uv_repeat
 *   scaling   one fine-detailed image drawn at 1/2, 1/4, 1/6 and 1/8 size,
 *             once from a mipmapped atlas page (cw_texture_desc.mipmaps) and
 *             once without: the mipmapped row stays smooth where the other
 *             shimmers
 *
 * CW_R2D_MOTION picks a variant for the tests' controls (native only):
 * "nowind", "notrail", "nomips". A pure function of the tick. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/core.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"

enum { L_SKY, L_WORLD, L_HUD };

typedef struct {
  cw_texture blade, detail, detail_mip, streak;
  cw_font font;
  int32_t ticks;
  const char *mode;
} app_t;
static app_t app;

static bool mode(const char *m) { return app.mode && !strcmp(app.mode, m); }

static void make_art(app_t *a) {
  /* a grass blade: a tapered green wedge, lighter at the tip */
  enum { BW = 16, BH = 96 };
  static uint8_t b[BW * BH * 4];
  for (int y = 0; y < BH; y++)
    for (int x = 0; x < BW; x++) {
      float v = (float)y / BH, half = 0.5f * (0.15f + 0.85f * v), u = fabsf(((float)x + 0.5f) / BW - 0.5f);
      bool in = u < half * 0.9f;
      uint8_t *p = b + 4 * (y * BW + x);
      p[0] = (uint8_t)(in ? 40 + 60 * (1 - v) : 0), p[1] = (uint8_t)(in ? 110 + 90 * (1 - v) : 0), p[2] = (uint8_t)(in ? 40 : 0);
      p[3] = in ? 255 : 0;
    }
  a->blade = cw_texture_from_pixels(BW, BH, b, NULL);
  /* fine detail: a one-pixel checker under concentric rings (aliases badly when shrunk without mips) */
  enum { D = 512 };
  uint8_t *d = cw_alloc(D * D * 4, CW_MEM_GAME);
  for (int y = 0; y < D; y++)
    for (int x = 0; x < D; x++) {
      float r = sqrtf((float)((x - D / 2) * (x - D / 2) + (y - D / 2) * (y - D / 2)));
      int ring = ((int)(r / 3.0f)) & 1, chk = (x ^ y) & 1;
      uint8_t v = (uint8_t)(ring ? (chk ? 240 : 30) : (chk ? 30 : 200));
      uint8_t *p = d + 4 * (y * D + x);
      p[0] = v, p[1] = (uint8_t)(v * 0.85f), p[2] = (uint8_t)(255 - v / 2), p[3] = 255;
    }
  a->detail = cw_texture_from_pixels(D, D, d, NULL);
  a->detail_mip = cw_texture_from_pixels(D, D, d, &(cw_texture_desc){ .mipmaps = !mode("nomips") });
  cw_free(d);
  /* a streak for the textured trail: bright core, soft edges, repeating dashes */
  enum { SW = 64, SH = 16 };
  static uint8_t s[SW * SH * 4];
  for (int y = 0; y < SH; y++)
    for (int x = 0; x < SW; x++) {
      float v = 1 - fabsf(((float)y + 0.5f) / SH * 2 - 1), dash = x < SW * 3 / 4 ? 1.0f : 0.15f;
      uint8_t *p = s + 4 * (y * SW + x);
      p[0] = p[1] = p[2] = 255;
      p[3] = (uint8_t)(255 * v * v * dash);
    }
  a->streak = cw_texture_from_pixels(SW, SH, s, &(cw_texture_desc){ .repeat = true });
}

static void init(void *user) {
  app_t *a = user;
  a->mode = getenv("CW_R2D_MOTION");
  make_art(a);
  a->font = cw_font_load("fonts/FiraSans-Regular.ttf", NULL);
  cw_debug_i32("ticks", &a->ticks);
  cw_r2d_debug_register("r2d");
}

static void fixed_update(void *user, double dt) {
  (void)dt;
  ((app_t *)user)->ticks++;
}

static uint32_t hash32(uint32_t h) {
  h *= 2654435761u;
  h ^= h >> 16;
  h *= 0x45d9f3bu;
  h ^= h >> 16;
  return h;
}

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  float t = (float)a->ticks / 60.0f;
  cw_vec2 cs = cw_r2d_canvas_size();
  float W = cs.x, H = cs.y, k = H / 720.0f;
  cw_r2d_post post = { .hud_layer = L_HUD, .bloom = true, .bloom_threshold = 1.0f, .bloom_intensity = 0.8f };
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x0b1020ff), .time = t, .post = &post,
                                    .wind = { .strength = mode("nowind") ? 0 : 14 * k, .speed = 0.6f, .wavelength = 260 * k, .gusts = 0.6f } });
  cw_r2d_use_layer(L_SKY);
  cw_r2d_rect((cw_rect){ 0, H * 0.62f, W, H * 0.38f }, cw_hex(0x1a2a18ff));

  /* the meadow: blades of three heights, swaying more the taller they are */
  cw_r2d_use_layer(L_WORLD);
  for (int i = 0; i < 260; i++) {
    uint32_t h = hash32((uint32_t)i);
    float x = (float)(h % 10000) / 10000.0f * W, hgt = (0.5f + (float)((h >> 14) % 100) / 100.0f) * 110 * k;
    cw_r2d_set_sway(0.4f + 0.6f * hgt / (160 * k));
    cw_r2d_sprite(&(cw_sprite){ .texture = a->blade, .pos = { x, H * 0.66f + (float)((h >> 8) % 40) * k }, .size = { 10 * k, hgt },
                                .pivot = { 0.5f, 1 }, .flip_y = true });
  }
  cw_r2d_set_sway(0);

  /* comets: glowing heads with fading trails (additive, intensity 3) */
  if (!mode("notrail")) {
    cw_r2d_set_blend(CW_BLEND_ADD);
    for (int c = 0; c < 3; c++) {
      cw_vec2 pts[48];
      for (int i = 0; i < 48; i++) {
        float tt = t * (0.6f + 0.2f * (float)c) - (float)i * 0.012f + (float)c * 2.1f;
        pts[i] = (cw_vec2){ W * (0.5f + 0.38f * cosf(tt * 1.3f)), H * (0.3f + 0.16f * sinf(tt * 2.1f + (float)c)) };
      }
      cw_color col = c == 0 ? cw_hex(0xffb050ff) : c == 1 ? cw_hex(0x60c0ffff) : cw_hex(0xff60c0ff);
      cw_r2d_set_intensity(3);
      cw_r2d_trail(&(cw_r2d_trail_desc){ .points = pts, .count = 48, .head_width = 10 * k, .tail_width = 1 * k, .head_color = col });
      cw_r2d_circle(pts[0], 7 * k, 0, col);
      cw_r2d_set_intensity(1);
    }
    /* a textured trail: dashes repeating every 64 units along a wave */
    cw_vec2 wave[40];
    for (int i = 0; i < 40; i++) wave[i] = (cw_vec2){ W * 0.1f + (float)i * W * 0.02f, H * 0.52f + 20 * k * sinf((float)i * 0.4f - t * 3) };
    cw_r2d_trail(&(cw_r2d_trail_desc){ .points = wave, .count = 40, .head_width = 12 * k, .tail_width = 12 * k,
                                       .head_color = cw_hex(0x80ffd0ff), .tail_color = cw_hex(0x80ffd040), .texture = a->streak,
                                       .uv_repeat = 64 * k });
    cw_r2d_set_blend(CW_BLEND_ALPHA);
  }

  /* scaling: the detail image at 1/2 .. 1/8, mipmapped (top row) and not (bottom row) */
  cw_r2d_use_layer(L_HUD);
  float sizes[4] = { 0.5f, 0.25f, 1.0f / 6.0f, 0.125f };
  float x = 20 * k;
  for (int i = 0; i < 4; i++) {
    float s = 512 * sizes[i] * k;
    cw_r2d_sprite(&(cw_sprite){ .texture = a->detail_mip, .pos = { x, H * 0.04f }, .size = { s, s } });
    cw_r2d_sprite(&(cw_sprite){ .texture = a->detail, .pos = { x + W * 0.5f, H * 0.04f }, .size = { s, s } });
    x += s + 12 * k;
  }
  cw_r2d_text(a->font, "wind sway, trails, mipmaps (left) vs none (right)", (cw_vec2){ 20 * k, H - 40 * k },
              &(cw_text_style){ .size = 22 * k, .own_backdrop = true });
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "r2d motion", .width = 1280, .height = 720, .init = init, .fixed_update = fixed_update,
                        .render = render, .debug = true, .user = &app };
}
