/* r2d_tilemap: a 3-layer tilemap (ground, decoration, transformed tiles)
 * of 96x48 tiles in 32x32 chunks, viewed through a zoomed, panning camera
 * so chunk culling matters; animated water and lava cycle in the vertex
 * shader from the pass time; every flip/rotation combination of an
 * asymmetric tile; a cell edited at tick 45 (one chunk rebuild). */
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"

#define MW 96
#define MH 48

typedef struct {
  cw_texture tiles;
  cw_tilemap map;
  int32_t ticks;
  uint32_t edited_cell;
  int32_t cell_xy[2];
  float cam[2];
} app_t;
static app_t app;

static uint32_t hash2(int x, int y) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

static void init(void *user) {
  app_t *a = user;
  a->tiles = cw_texture_load("images/tiles.png", &(cw_texture_desc){ .filter = CW_FILTER_NEAREST });
  a->map = cw_tilemap_create(&(cw_tilemap_desc){ .width = MW, .height = MH, .layers = 3, .tileset = a->tiles,
                                                 .tile_w = 16, .tile_h = 16 });
  static uint32_t ground[MW * MH], deco[MW * MH];
  for (int y = 0; y < MH; y++)
    for (int x = 0; x < MW; x++) {
      uint32_t h = hash2(x, y);
      int id = (h % 7 == 0) ? 1 : 0;              /* grass with dirt patches */
      if (y >= 20 && y < 26 && x > 4) id = 4;      /* a river (animated) */
      if (x >= 40 && x < 46 && y >= 4 && y < 12) id = 10; /* a lava pool (animated) */
      if (y == 30 || x == 60) id = 2;              /* brick walls */
      ground[y * MW + x] = CW_TILE(id);
      deco[y * MW + x] = id == 0 && h % 11 == 0 ? CW_TILE(8) : id == 4 && h % 9 == 0 ? CW_TILE(9) : 0;
    }
  cw_tilemap_set_layer(a->map, 0, ground);
  cw_tilemap_set_layer(a->map, 1, deco);
  /* all 8 transforms of the "F" tile in a row, twice (map layer 2) */
  static const uint32_t tf[8] = { 0, CW_TILE_FLIP_X, CW_TILE_FLIP_Y, CW_TILE_ROT180, CW_TILE_ROT90, CW_TILE_ROT270,
                                  CW_TILE_FLIP_D, CW_TILE_FLIP_D | CW_TILE_FLIP_X | CW_TILE_FLIP_Y };
  for (int i = 0; i < 8; i++) {
    cw_tilemap_set(a->map, 2, 6 + i, 14, CW_TILE(3) | tf[i]);
    cw_tilemap_set(a->map, 2, 26 + i, 16, CW_TILE(3) | tf[i]);
  }
  cw_tilemap_set_animation(a->map, 4, 4, 4.0f);
  cw_tilemap_set_animation(a->map, 10, 2, 2.0f);
  cw_r2d_layer_set(0, &(cw_r2d_layer_desc){ .has_camera = true, .camera = { .zoom = 2 }, .pixel_snap = true });
  cw_debug_i32("ticks", &a->ticks);
  cw_debug_u32("edited_cell", &a->edited_cell);
  cw_debug_field("cell_xy", a->cell_xy, CW_DBG_I32, 2);
  cw_debug_f32_array("cam", a->cam, 2);
  cw_r2d_debug_register("r2d");
}

static void fixed_update(void *user, double dt) {
  (void)dt;
  app_t *a = user;
  a->ticks++;
  if (a->ticks == 45) {
    for (int i = 0; i < 6; i++) cw_tilemap_set(a->map, 0, 30 + i, 14, CW_TILE(2));
    a->edited_cell = cw_tilemap_get(a->map, 0, 32, 14);
  }
}

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  float t = (float)a->ticks / 60.0f;
  cw_camera2d cam = { .zoom = 2, .position = { 160 + t * 120, 200 + t * 40 } };
  a->cam[0] = cam.position.x;
  a->cam[1] = cam.position.y;
  cw_r2d_layer_set_camera(0, &cam);
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x101018ff), .time = t });
  cw_r2d_use_layer(0);
  for (int l = 0; l < 3; l++) cw_r2d_tilemap(a->map, l, (cw_vec2){ 0, 0 }, CW_WHITE);
  /* the cell under the screen centre */
  cw_vec2 w = cw_r2d_layer_screen_to_world(0, (cw_vec2){ 320, 180 });
  cw_tilemap_world_to_cell(a->map, (cw_vec2){ 0, 0 }, w, &a->cell_xy[0], &a->cell_xy[1]);
  cw_r2d_rect_lines((cw_rect){ (float)a->cell_xy[0] * 16, (float)a->cell_xy[1] * 16, 16, 16 }, 1, cw_hex(0xff2020ff));
  /* screen-space HUD bar on layer 1 */
  cw_r2d_use_layer(1);
  cw_r2d_rect((cw_rect){ 0, 330, 640, 30 }, cw_hex(0x000000a0));
  cw_r2d_rect((cw_rect){ 10, 340, 620.0f * (float)(a->ticks % 120) / 120.0f, 10 }, cw_hex(0x40e080ff));
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "r2d tilemap", .width = 640, .height = 360, .init = init, .fixed_update = fixed_update,
                        .render = render, .debug = true, .user = &app };
}
