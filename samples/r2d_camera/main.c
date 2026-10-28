/* r2d_camera: cameras and canvas layers on a pixel-perfect virtual canvas.
 * A 300x170 canvas scaled by the largest whole factor (6x in its 1920x1080 framebuffer, 2x in a 640x360
 * window, letterboxed); a parallax background layer; the world layer with a
 * camera that follows a moving target (smoothing + limits), zooms and
 * rotates; a minimap layer with its own viewport and camera; a screen-space
 * HUD. Camera math (follow, screen<->world, window<->canvas) is published as
 * debug state: it is deterministic and must match on every host. */
#include <math.h>
#include <stdio.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"

enum { L_SKY, L_WORLD, L_MINIMAP, L_HUD };

typedef struct {
  cw_texture sheet;
  cw_font pixel;
  cw_camera2d cam;
  int32_t ticks;
  int32_t bad_blocks, scale; /* pixel-perfect check of the window: every canvas pixel a uniform scale x scale block */
  float target[2], cam_pos[2], probe_world[2], probe_back[2], win_to_canvas[2];
} app_t;
static app_t app;

#define WORLD_W 900.0f
#define WORLD_H 500.0f

static cw_vec2 target_at(int ticks) {
  float t = (float)ticks / 60.0f;
  return (cw_vec2){ 450 + 380 * cw_sin(t * 0.7f), 250 + 200 * cw_sin(t * 1.3f) };
}

static void init(void *user) {
  app_t *a = user;
  a->sheet = cw_texture_load("images/sheet.png", &(cw_texture_desc){ .filter = CW_FILTER_NEAREST });
  a->pixel = cw_font_load("fonts/PressStart2P-Regular.ttf", &(cw_font_desc){ .mode = CW_FONT_BITMAP, .bake_size = 8, .threshold = true });
  a->cam = (cw_camera2d){ .position = target_at(0), .zoom = 1, .use_limits = true, .limits = { 0, 0, WORLD_W, WORLD_H } };
  /* #region layers */
  cw_r2d_layer_set(L_SKY, &(cw_r2d_layer_desc){ .has_camera = true, .parallax = { 0.3f, 0.3f }, .pixel_snap = true });
  cw_r2d_layer_set(L_WORLD, &(cw_r2d_layer_desc){ .has_camera = true, .y_sort = true });
  cw_r2d_layer_set(L_MINIMAP, &(cw_r2d_layer_desc){ .has_camera = true, .viewport = { 220, 6, 74, 42 },
                                                    .camera = { .position = { WORLD_W / 2, WORLD_H / 2 }, .zoom = 74.0f / WORLD_W } });
  cw_r2d_layer_set(L_HUD, NULL);
  /* #endregion */
  cw_debug_i32("ticks", &a->ticks);
  cw_debug_i32("pixel_perfect.bad_blocks", &a->bad_blocks);
  cw_debug_i32("pixel_perfect.scale", &a->scale);
  cw_debug_f32_array("target", a->target, 2);
  cw_debug_f32_array("cam", a->cam_pos, 2);
  cw_debug_f32_array("probe.world", a->probe_world, 2);
  cw_debug_f32_array("probe.back", a->probe_back, 2);
  cw_debug_f32_array("probe.win_to_canvas", a->win_to_canvas, 2);
  cw_r2d_debug_register("r2d");
}

static void fixed_update(void *user, double dt) {
  app_t *a = user;
  a->ticks++;
  cw_vec2 t = target_at(a->ticks);
  a->target[0] = t.x;
  a->target[1] = t.y;
  float tt = (float)a->ticks / 60.0f;
  a->cam.zoom = 1.25f + 0.5f * cw_sin(tt * 0.9f);
  a->cam.rotation = 0.25f * cw_sin(tt * 0.6f);
  cw_camera2d_follow(&a->cam, t, 4.0f, (float)dt);
  cw_camera2d_apply_limits(&a->cam, (cw_rect){ 0, 0, 300, 170 });
  a->cam_pos[0] = a->cam.position.x;
  a->cam_pos[1] = a->cam.position.y;
  cw_vec2 w = cw_camera2d_screen_to_world(&a->cam, (cw_rect){ 0, 0, 300, 170 }, (cw_vec2){ 40, 30 });
  cw_vec2 back = cw_camera2d_world_to_screen(&a->cam, (cw_rect){ 0, 0, 300, 170 }, w);
  a->probe_world[0] = w.x, a->probe_world[1] = w.y;
  a->probe_back[0] = back.x, a->probe_back[1] = back.y;
}

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  cw_r2d_layer_set_camera(L_SKY, &(cw_camera2d){ .position = a->cam.position });
  cw_r2d_layer_set_camera(L_WORLD, &a->cam);
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x6080b0ff), .virtual_width = 300,
                                    .virtual_height = 170, .scale = CW_SCALE_INTEGER, .letterbox = cw_hex(0x080808ff) });
  cw_vec2 wc = cw_r2d_window_to_canvas((cw_vec2){ 320, 180 });
  a->win_to_canvas[0] = wc.x, a->win_to_canvas[1] = wc.y;

  /* parallax mountains: tile a band every 160 units */
  cw_r2d_use_layer(L_SKY);
  for (int i = -2; i < 8; i++) {
    float x = (float)i * 160;
    cw_vec2 m[3] = { { x, 120 }, { x + 80, 10 + (float)(i & 1) * 30 }, { x + 160, 120 } };
    cw_r2d_polygon(m, 3, cw_hex(0x405878ff));
  }
  /* the world: ground, a grid, critters (y-sorted), the target */
  for (int L = 0; L < 2; L++) {
    cw_r2d_use_layer(L == 0 ? L_WORLD : L_MINIMAP);
    cw_r2d_set_z(-1);
    float lw = L ? 12 : 2;
    if (L) cw_r2d_rect((cw_rect){ 0, 0, WORLD_W, WORLD_H }, cw_hex(0x3c7a3cff));
    for (int i = 0; i < 30; i++) /* meadows */
      cw_r2d_circle((cw_vec2){ (float)((i * 211) % 900), (float)((i * 97) % 500) }, 40 + (float)(i % 3) * 20, 0, cw_hex(0x3c7a3cd0));
    for (int gx = 0; gx <= 9; gx++) cw_r2d_rect((cw_rect){ (float)gx * 100 - lw / 2, 0, lw, WORLD_H }, cw_hex(0x2a5a2aff));
    for (int gy = 0; gy <= 5; gy++) cw_r2d_rect((cw_rect){ 0, (float)gy * 100 - lw / 2, WORLD_W, lw }, cw_hex(0x2a5a2aff));
    cw_r2d_set_z(0);
    for (int i = 0; i < 120; i++) {
      uint32_t h = (uint32_t)i * 2654435761u;
      h ^= h >> 15;
      float x = 20 + (float)(h % 860), y = 30 + (float)((h >> 10) % 450);
      cw_r2d_sprite(&(cw_sprite){ .texture = a->sheet, .src = { (float)((i + a->ticks / 10) % 4) * 32, 0, 32, 32 },
                                  .pos = { x, y }, .pivot = { 0.5f, 1 }, .flip_x = i & 1 });
    }
    cw_r2d_circle((cw_vec2){ a->target[0], a->target[1] }, L ? 30 : 6, 0, cw_hex(0xff3030ff));
  }
  /* minimap frame (screen space) */
  cw_r2d_use_layer(L_HUD);
  cw_r2d_rect_lines((cw_rect){ 219, 5, 76, 44 }, 1, cw_hex(0xffffffff));
  cw_r2d_rect((cw_rect){ 0, 150, 300, 20 }, cw_hex(0x000000b0));
  char buf[64];
  snprintf(buf, sizeof buf, "CAM %4d %4d Z%.2f", (int)a->cam.position.x, (int)a->cam.position.y, a->cam.zoom);
  cw_r2d_text(a->pixel, buf, (cw_vec2){ 6, 156 }, &(cw_text_style){ .size = 8, .color = cw_hex(0xffffa0ff) });
  cw_r2d_end();
  /* read the canvas back and check the integer scaling: asked every 30
   * ticks, checked when the pixels arrive (a frame or more later on WebGPU) */
  static uint32_t ticket;
  static int rk, rw, rh;
  if (ticket) {
    uint8_t *px = cw_frame_alloc((size_t)rw * (size_t)rh * 4);
    int got = cw_gpu_read_result(ticket, px);
    if (got != 0) ticket = 0;
    if (got == 1) {
      int bad = 0;
      for (int by = 0; by < 170; by++)
        for (int bx = 0; bx < 300; bx++) {
          const uint8_t *p0 = px + ((size_t)(by * rk) * (size_t)rw + (size_t)(bx * rk)) * 4;
          for (int j = 0; j < rk; j++)
            for (int i = 0; i < rk; i++)
              if (memcmp(p0, px + ((size_t)(by * rk + j) * (size_t)rw + (size_t)(bx * rk + i)) * 4, 3)) bad++;
        }
      a->bad_blocks = bad;
      a->scale = rk;
    }
  }
  if (a->ticks % 30 == 0 && !ticket) {
    cw_rect r = cw_r2d_canvas_rect();
    rk = (int)(r.w / 300.0f);
    rw = 300 * rk, rh = 170 * rk;
    ticket = cw_gpu_read_window((int)r.x, (int)r.y, rw, rh);
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "r2d camera", .width = 640, .height = 360, .init = init, .fixed_update = fixed_update,
                        .render = render, .debug = true, .user = &app };
}
