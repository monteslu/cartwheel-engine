/* r2d_normal_turn: a turned or flipped sprite is lit by its turned normals.
 * One sun (a directional light, the same at every pixel) over a lit layer.
 * Each row is one turn (rotation and flips); its four squares:
 *   A  a sprite of a flat surface tilted one way (every texel's normal map
 *      value is n0), turned by the row's rotation and flips
 *   I  the same as a sprite instance (cw_r2d_instances)
 *   B  an unturned sprite whose normal map holds n0 turned on the CPU: what
 *      A and I must look like
 *   C  an unturned sprite with n0 itself: what A looked like when the
 *      renderer lit a turned sprite as if unturned (the old fault)
 * tests/r2d-normal-turn.mjs compares the centres of the squares. Debug state:
 * nt.ticks, nt.rows, nt.normal_ok. Canvas 640 x 360, rows 40 px apart from
 * y = 20 (centres at y = 36 + 40 k), columns centred at x = 60, 140, 220, 300. */
#include <math.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"

#define TS 32
#define ROWS 8

typedef struct turn { float rot; bool fx, fy; } turn;
static const turn TURNS[ROWS] = {
  { 0.5235988f, false, false }, /* 30 degrees */
  { 1.5707963f, false, false }, /* 90 */
  { 2.4f, false, false },
  { 3.5f, false, false },
  { 0, true, false },           /* flip x */
  { 0, false, true },           /* flip y */
  { 0, true, true },
  { 1.0f, true, false },        /* flip x, then turned */
};

typedef struct {
  cw_texture base, turned[ROWS];
  int32_t ticks, rows;
  uint8_t normal_ok;
} app_t;
static app_t app;

/* n0: a surface tilted toward +x and up the image (+y up in the image, as normal maps are) */
static const float N0[3] = { 0.55f, 0.35f, 0.758f };

static cw_texture flat_tex(const float n[3], bool *ok) {
  static uint8_t col[TS * TS * 4], nrm[TS * TS * 4];
  float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  for (int i = 0; i < TS * TS; i++) {
    col[4 * i] = 170, col[4 * i + 1] = 170, col[4 * i + 2] = 170, col[4 * i + 3] = 255;
    for (int c = 0; c < 3; c++) nrm[4 * i + c] = (uint8_t)lroundf((n[c] / l * 0.5f + 0.5f) * 255.0f);
    nrm[4 * i + 3] = 255;
  }
  cw_texture t = cw_texture_from_pixels(TS, TS, col, &(cw_texture_desc){ .filter = CW_FILTER_NEAREST });
  if (!cw_texture_set_normal_map(t, nrm)) *ok = false;
  return t;
}

static void init(void *user) {
  app_t *a = user;
  bool ok = true;
  a->base = flat_tex(N0, &ok);
  for (int k = 0; k < ROWS; k++) {
    /* the flips act on the image (x mirrored, then y), then the turn: `rotation` is clockwise on the
     * canvas (y down), so in the normal's y-up frame it is a turn by -rot */
    float x = TURNS[k].fx ? -N0[0] : N0[0], y = TURNS[k].fy ? -N0[1] : N0[1];
    float c = cosf(TURNS[k].rot), s = sinf(TURNS[k].rot);
    float n[3] = { c * x + s * y, -s * x + c * y, N0[2] };
    a->turned[k] = flat_tex(n, &ok);
  }
  a->normal_ok = ok;
  a->rows = ROWS;
  cw_r2d_layer_set(0, &(cw_r2d_layer_desc){ .lit = true });
  cw_debug_i32("nt.ticks", &a->ticks);
  cw_debug_i32("nt.rows", &a->rows);
  cw_debug_field("nt.normal_ok", &a->normal_ok, CW_DBG_U8, 1);
}

static void fixed_update(void *user, double dt) {
  (void)dt;
  ((app_t *)user)->ticks++;
}

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x000000ff), .ambient = cw_rgba(0.1f, 0.1f, 0.1f, 1) });
  cw_r2d_use_layer(0);
  cw_sprite_instance inst[ROWS];
  for (int k = 0; k < ROWS; k++) {
    const turn *t = &TURNS[k];
    float cy = 36.0f + 40.0f * (float)k;
    cw_r2d_sprite(&(cw_sprite){ .texture = a->base, .pos = { 60, cy }, .pivot = { 0.5f, 0.5f }, .rotation = t->rot, .flip_x = t->fx, .flip_y = t->fy });
    inst[k] = (cw_sprite_instance){ .x = 140, .y = cy, .rotation = t->rot, .flags = (t->fx ? CW_INSTANCE_FLIP_X : 0) | (t->fy ? CW_INSTANCE_FLIP_Y : 0) };
    cw_r2d_sprite(&(cw_sprite){ .texture = a->turned[k], .pos = { 220, cy }, .pivot = { 0.5f, 0.5f } });
    cw_r2d_sprite(&(cw_sprite){ .texture = a->base, .pos = { 300, cy }, .pivot = { 0.5f, 0.5f } });
  }
  cw_r2d_instances(&(cw_r2d_instances_desc){ .texture = a->base, .instances = inst, .count = ROWS });
  /* the sun: an angle and height where every row's turned surface and the unturned one take clearly different light */
  cw_r2d_light(&(cw_light2d){ .directional = true, .angle = 4.5f, .height = 2.04f, .color = cw_hex(0xffffffff), .energy = 1.2f });
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "r2d normal turn", .width = 640, .height = 360, .init = init, .fixed_update = fixed_update,
                        .render = render, .debug = true, .user = &app };
}
