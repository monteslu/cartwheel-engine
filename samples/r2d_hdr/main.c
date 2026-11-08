/* r2d_hdr: the modern 2D lighting and HDR features in one scene.
 *
 *   a night sky (unlit, its moon drawn at intensity 3 so it blooms)
 *   a lit cave wall: stone with a normal map and a material map (wet patches
 *     that glint, dry stone that does not)
 *   crystals drawn with emission 1 and intensity 4: they ignore the lights,
 *     go past 1 and bloom
 *   pillars and ledges as occluders: soft shadows from two torches, hard ones
 *     from a spot lamp, and moonlight (a directional light) falling through a
 *     gap in the ceiling as volumetric shafts
 *   global illumination: the crystals light the cave round them in their own
 *     colour, and light bounces off the rock
 *   the post chain: bloom, the soft tone curve, a cool grade, a vignette
 *   a pool (cw_r2d_water in a screen_read layer): a spring surface that
 *     drips ripple (cw_water2d), the cave refracted through it and mirrored
 *     on it, caustics, a lit surface line; heat haze over the torches and a
 *     shockwave ring in a distortion layer
 *   a HUD drawn after the chain (post.hud_layer), untouched by it
 *
 * CW_R2D_HDR picks a variant for the tests' controls (native only): "nopost"
 * (no chain), "nobloom", "noshadow", "noemission", "nomaterial", "hard"
 * (every light's softness 0), "novolume", "nogi", "nowater", "nodistort", "lut" (the teal and orange
 * lookup table), "lutid" (the identity table: the picture unchanged), "cookie" (the torches and the
 * spot lamp shine through a window's panes: light textures), "front" (a lattice drawn above the
 * distortion layer, which must stay straight) and "frontnodistort" (the same without distortion). The scene is a pure function of the
 * tick. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"

enum { L_SKY, L_WORLD, L_WATER, L_HEAT, L_FRONT, L_HUD };
#define SW 128

typedef struct {
  cw_texture stone, crystal, solid, panes;
  cw_font font;
  cw_water2d *pool;
  int32_t ticks;
  const char *mode;
  uint8_t maps_ok;
} app_t;
static app_t app;

static bool mode(const char *m) { return app.mode && !strcmp(app.mode, m); }

static uint32_t hash32(uint32_t h) {
  h *= 2654435761u;
  h ^= h >> 16;
  h *= 0x45d9f3bu;
  h ^= h >> 16;
  return h;
}

static float vnoise(float x, float y) {
  int xi = (int)floorf(x), yi = (int)floorf(y);
  float fx = x - (float)xi, fy = y - (float)yi;
  float v[4];
  for (int k = 0; k < 4; k++) {
    /* wrap at 8 cells so the stone tiles */
    uint32_t h = hash32((uint32_t)(((xi + (k & 1)) & 7) * 131 + ((yi + (k >> 1)) & 7) * 977));
    v[k] = (float)(h & 0xffff) / 65535.0f;
  }
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  return (v[0] * (1 - fx) + v[1] * fx) * (1 - fy) + (v[2] * (1 - fx) + v[3] * fx) * fy;
}

/* stone blocks: height from a block pattern plus noise; wet where a second noise is high */
static float stone_h(int x, int y) {
  x &= SW - 1;
  y &= SW - 1;
  int row = y / 32, bx = (x + (row & 1) * 32) % 64, by = y % 32;
  float e = (float)CW_MIN(CW_MIN(bx, 63 - bx), CW_MIN(by, 31 - by));
  float block = e < 1 ? 0 : e < 5 ? (e - 1) / 4.0f : 1.0f;
  return block * (0.75f + 0.25f * vnoise((float)x / 16.0f, (float)y / 16.0f));
}

static void make_art(app_t *a) {
  static uint8_t col[SW * SW * 4], nrm[SW * SW * 4], mat[SW * SW * 4];
  for (int y = 0; y < SW; y++)
    for (int x = 0; x < SW; x++) {
      float h = stone_h(x, y), n = vnoise((float)x / 8.0f + 3, (float)y / 8.0f + 7);
      float wet = CW_CLAMP((vnoise((float)x / 32.0f * 8.0f / 8.0f + 1.5f, (float)y / 32.0f + 2.5f) - 0.55f) * 6.0f, 0.0f, 1.0f);
      uint8_t *c = col + 4 * (y * SW + x), *o = nrm + 4 * (y * SW + x), *m = mat + 4 * (y * SW + x);
      float g = h > 0 ? 0.42f + 0.18f * n - 0.1f * wet : 0.2f;
      c[0] = (uint8_t)(g * 255 * 0.95f), c[1] = (uint8_t)(g * 255 * 0.92f), c[2] = (uint8_t)(g * 255), c[3] = 255;
      float dx = stone_h(x + 1, y) - stone_h(x - 1, y), dy = stone_h(x, y + 1) - stone_h(x, y - 1);
      float nx = -dx * 2.5f, ny = dy * 2.5f, l = sqrtf(nx * nx + ny * ny + 1);
      o[0] = (uint8_t)((nx / l * 0.5f + 0.5f) * 255), o[1] = (uint8_t)((ny / l * 0.5f + 0.5f) * 255);
      o[2] = (uint8_t)((1 / l * 0.5f + 0.5f) * 255), o[3] = 255;
      m[0] = 0, m[1] = (uint8_t)(wet * 230), m[2] = (uint8_t)(140 + wet * 100), m[3] = 255;
    }
  a->stone = cw_texture_from_pixels(SW, SW, col, NULL);
  a->maps_ok = cw_texture_set_normal_map(a->stone, nrm) && cw_texture_set_material_map(a->stone, mat);
  /* a crystal: a faceted shard, emissive through its material map */
  enum { CW = 32, CH = 64 };
  static uint8_t cc[CW * CH * 4], cm[CW * CH * 4];
  for (int y = 0; y < CH; y++)
    for (int x = 0; x < CW; x++) {
      float u = ((float)x + 0.5f) / CW * 2 - 1, v = ((float)y + 0.5f) / CH;
      float half = v < 0.3f ? v / 0.3f : 1 - (v - 0.3f) / 0.7f * 0.4f;
      bool in = fabsf(u) < half * 0.9f;
      float facet = u < 0 ? 0.75f : 1.0f;
      uint8_t *c = cc + 4 * (y * CW + x), *m = cm + 4 * (y * CW + x);
      c[0] = (uint8_t)(in ? 90 * facet : 0), c[1] = (uint8_t)(in ? 230 * facet : 0), c[2] = (uint8_t)(in ? 255 * facet : 0);
      c[3] = in ? 255 : 0;
      m[0] = in ? 255 : 0, m[1] = 0, m[2] = 0, m[3] = 255;
    }
  a->crystal = cw_texture_from_pixels(CW, CH, cc, NULL);
  /* a light texture: a window of four panes, the bars dark, the glass a little warm toward its middle */
  enum { PW = 64 };
  static uint8_t pw[PW * PW * 4];
  for (int y = 0; y < PW; y++)
    for (int x = 0; x < PW; x++) {
      bool bar = abs(x - 32) < 3 || abs(y - 32) < 3 || x < 4 || y < 4 || x > 59 || y > 59;
      float r = sqrtf((float)((x - 32) * (x - 32) + (y - 32) * (y - 32))) / 45.0f;
      uint8_t *p = pw + 4 * (y * PW + x);
      float v = bar ? 0.0f : 1.0f - 0.25f * r;
      p[0] = (uint8_t)(255 * v), p[1] = (uint8_t)(235 * v), p[2] = (uint8_t)(200 * v), p[3] = 255;
    }
  a->panes = cw_texture_from_pixels(PW, PW, pw, NULL);
  if (!mode("nomaterial")) cw_texture_set_material_map(a->crystal, cm);
}

static void init(void *user) {
  app_t *a = user;
  a->mode = getenv("CW_R2D_HDR");
  make_art(a);
  a->font = cw_font_load("fonts/FiraSans-Regular.ttf", NULL);
  cw_r2d_layer_set(L_SKY, NULL);
  cw_r2d_layer_set(L_WORLD, &(cw_r2d_layer_desc){ .lit = true, .occluder_depth = 14 });
  a->pool = cw_water2d_new(&(cw_water2d_desc){ .columns = 80, .width = 330 });
  cw_r2d_layer_set(L_WATER, &(cw_r2d_layer_desc){ .screen_read = !mode("nowater") });
  cw_r2d_layer_set(L_HEAT, &(cw_r2d_layer_desc){ .distortion = mode("nodistort") || mode("frontnodistort") ? 0 : 6 });
  cw_r2d_layer_set(L_FRONT, NULL);
  cw_r2d_layer_set(L_HUD, NULL);
  cw_debug_i32("ticks", &a->ticks);
  cw_debug_field("maps_ok", &a->maps_ok, CW_DBG_U8, 1);
  cw_r2d_debug_register("r2d");
}

static void fixed_update(void *user, double dt) {
  app_t *a = user;
  a->ticks++;
  /* a drip every 50 ticks, at a spot that walks across the pool */
  if (a->ticks % 50 == 0) cw_water2d_splash(a->pool, (float)((a->ticks / 50 * 97) % 300 + 15), -60);
  cw_water2d_step(a->pool, (float)dt);
}

static void rect_occluder(cw_rect r, cw_color c) {
  cw_vec2 p[4] = { { r.x, r.y }, { r.x + r.w, r.y }, { r.x + r.w, r.y + r.h }, { r.x, r.y + r.h } };
  cw_r2d_rect(r, c);
  cw_r2d_occluder(p, 4, true);
}

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  float t = (float)a->ticks / 60.0f;
  cw_vec2 cs = cw_r2d_canvas_size();
  float W = cs.x, H = cs.y, k = H / 720.0f;
  cw_r2d_post post = { .hud_layer = L_HUD, .bloom = !mode("nobloom"), .bloom_threshold = 1.0f, .bloom_intensity = 0.7f,
                       .tone_knee = 0.7f, .saturation = 1.1f, .contrast = 1.05f, .lift = { 0.01f, 0.015f, 0.03f, 0 },
                       .gain = { 0.97f, 1.0f, 1.06f, 0 }, .vignette = 0.35f };
  if (mode("lut")) post.lut = cw_r2d_lut_preset(CW_R2D_LUT_TEAL_ORANGE);
  if (mode("lutid")) post.lut = cw_r2d_lut_preset(CW_R2D_LUT_IDENTITY);
  cw_r2d_gi gi = { .intensity = 1.0f };
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x05070dff), .ambient = cw_rgba(0.10f, 0.11f, 0.16f, 1),
                                    .post = mode("nopost") ? NULL : &post, .gi = mode("nogi") ? NULL : &gi });
  /* sky: a gradient and the moon, unlit */
  cw_r2d_use_layer(L_SKY);
  for (int i = 0; i < 16; i++) {
    float f = (float)i / 16.0f;
    cw_r2d_rect((cw_rect){ 0, H * f * 0.5f, W, H * 0.5f / 16 + 1 }, cw_rgba(0.04f + 0.06f * f, 0.06f + 0.08f * f, 0.14f + 0.1f * f, 1));
  }
  cw_r2d_set_intensity(3.0f);
  cw_r2d_circle((cw_vec2){ W * 0.78f, H * 0.12f }, 26 * k, 0, cw_hex(0xf2f0e0ff));
  cw_r2d_set_intensity(1.0f);

  /* the cave: wall, floor, ceiling with a gap, pillars, ledges */
  cw_r2d_use_layer(L_WORLD);
  float ts = 128 * k;
  for (float y = H * 0.18f; y < H; y += ts)
    for (float x = 0; x < W; x += ts) cw_r2d_sprite(&(cw_sprite){ .texture = a->stone, .pos = { x, y }, .size = { ts, ts } });
  cw_color rock = cw_hex(0x2a262cff);
  rect_occluder((cw_rect){ 0, H * 0.18f, W * 0.55f, 40 * k }, rock);           /* ceiling, left of the gap */
  rect_occluder((cw_rect){ W * 0.66f, H * 0.18f, W * 0.34f, 40 * k }, rock);  /* ceiling, right of the gap */
  rect_occluder((cw_rect){ 0, H * 0.88f, W, H * 0.12f }, rock);               /* floor */
  rect_occluder((cw_rect){ W * 0.22f, H * 0.42f, 34 * k, H * 0.46f }, rock);   /* pillars */
  rect_occluder((cw_rect){ W * 0.72f, H * 0.36f, 40 * k, H * 0.52f }, rock);
  /* a ledge and two hanging stones: art that casts shadows by its alpha (no polygon) */
  cw_r2d_set_occluder(true);
  cw_r2d_rect((cw_rect){ W * 0.40f, H * 0.60f, W * 0.14f, 14 * k }, rock);
  cw_r2d_circle((cw_vec2){ W * 0.47f, H * 0.36f }, 18 * k, 0, rock);
  cw_r2d_circle((cw_vec2){ W * 0.60f, H * 0.30f }, 12 * k, 0, rock);
  cw_r2d_set_occluder(false);
  /* crystals: emission 1, intensity 4 */
  if (!mode("noemission")) {
    cw_r2d_set_emissive(1.0f);
    cw_r2d_set_intensity(4.0f);
  }
  for (int i = 0; i < 5; i++) {
    float x = W * (0.08f + 0.19f * (float)i), s = (0.8f + 0.25f * sinf((float)i * 2.3f)) * k;
    cw_r2d_sprite(&(cw_sprite){ .texture = a->crystal, .pos = { x, H * 0.88f }, .pivot = { 0.5f, 1 }, .scale = { s, s },
                                .rotation = 0.15f * sinf((float)i * 1.7f) });
  }
  cw_r2d_set_emissive(0);
  cw_r2d_set_intensity(1);
  float soft = mode("hard") ? 0.0f : 0.55f;
  bool sh = !mode("noshadow");
  /* torches: a slow waver (nothing three or more times a second: that is a strobe), soft shadows */
  for (int i = 0; i < 2; i++) {
    float fl = 1 + 0.08f * sinf(t * 13 + (float)i * 3) + 0.05f * sinf(t * 7.3f + (float)i);
    cw_vec2 p = { W * (0.12f + 0.5f * (float)i), H * 0.55f };
    cw_r2d_light(&(cw_light2d){ .pos = p, .radius = 420 * k, .color = cw_hex(0xffa050ff), .energy = 1.6f * fl, .falloff = 1.6f,
                                .shadows = sh, .softness = soft, .volumetric = mode("novolume") ? 0 : 0.06f,
                                .texture = mode("cookie") ? a->panes : (cw_texture){ 0 }, .angle = 0.3f * (float)i });
  }
  /* a spot lamp from the right wall, hard shadows */
  cw_r2d_light(&(cw_light2d){ .pos = { W * 0.96f, H * 0.3f }, .radius = 700 * k, .color = cw_hex(0x90c0ffff), .energy = 1.4f,
                              .angle = 2.6f + 0.15f * sinf(t * 0.7f), .cone = 0.32f, .cone_soft = 0.35f, .shadows = sh, .softness = 0.05f,
                              .texture = mode("cookie") ? a->panes : (cw_texture){ 0 } });
  /* moonlight down through the gap: directional, volumetric shafts */
  cw_r2d_light(&(cw_light2d){ .directional = true, .angle = 1.75f, .height = 0.6f, .color = cw_hex(0xa8c4ffff), .energy = 0.9f,
                              .shadows = sh, .softness = soft * 0.5f, .volumetric = mode("novolume") ? 0 : 0.35f });

  /* the pool: the cave above mirrored about the surface, darkened and tinted */
  float wx0 = W * 0.30f, wx1 = W * 0.56f, wy0 = H * 0.80f, wy1 = H * 0.88f;
  cw_r2d_use_layer(L_WATER);
  if (!mode("nowater"))
    cw_r2d_water(&(cw_r2d_water_desc){ .area = { wx0, wy0, wx1 - wx0, wy1 - wy0 }, .surface = a->pool, .time = t, .wave_height = 1.5f * k,
                                      .tint = cw_rgba(0.05f, 0.18f, 0.3f, 0.6f), .refraction = 3 * k, .reflection = 0.7f,
                                      .caustics = 0.6f, .surface_glow = 0.8f });
  /* heat over the torches, ripples on the pool, a shockwave: the distortion layer */
  cw_r2d_use_layer(L_HEAT);
  {
    cw_texture nz = cw_r2d_noise_texture();
    for (int i = 0; i < 2; i++) {
      float cx = W * (0.12f + 0.5f * (float)i), cy = H * 0.55f;
      cw_vec2 pos[4] = { { cx - 40 * k, cy - 140 * k }, { cx + 40 * k, cy - 140 * k }, { cx + 40 * k, cy }, { cx - 40 * k, cy } };
      float s = t * 0.35f;
      cw_vec2 uv[4] = { { 0, s }, { 0.3f, s }, { 0.3f, s + 0.6f }, { 0, s + 0.6f } };
      cw_r2d_quad(nz, pos, uv, cw_rgba(1, 1, 1, 0.8f));
    }
    cw_vec2 pos[4] = { { wx0, wy0 }, { wx1, wy0 }, { wx1, wy1 }, { wx0, wy1 } };
    float s = t * 0.05f;
    cw_vec2 uv[4] = { { s, 0 }, { s + 1.5f, 0 }, { s + 1.5f, 0.25f }, { s, 0.25f } };
    cw_r2d_quad(nz, pos, uv, cw_rgba(1, 1, 1, 0.6f));
    float ph = fmodf(t, 2.0f);
    cw_r2d_shockwave((cw_vec2){ W * 0.30f, H * 0.45f }, ph * 300 * k, 40 * k, 1 - ph / 2);
  }

  /* a lattice in front of the distortion layer: it draws over the bent picture, straight */
  if (mode("front") || mode("frontnodistort")) {
    cw_r2d_use_layer(L_FRONT);
    for (int i = 0; i < 9; i++) cw_r2d_rect((cw_rect){ W * 0.05f + (float)i * W * 0.1f, H * 0.3f, 3 * k, H * 0.5f }, cw_hex(0xe0d090ff));
    for (int j = 0; j < 4; j++) cw_r2d_rect((cw_rect){ W * 0.05f, H * (0.32f + 0.15f * (float)j), W * 0.8f, 3 * k }, cw_hex(0xe0d090ff));
  }
  /* HUD: after the chain */
  cw_r2d_use_layer(L_HUD);
  cw_r2d_rect((cw_rect){ 0, 0, W, 40 * k }, cw_hex(0x000000b0));
  cw_r2d_text(a->font, "HDR 2D: emission, materials, soft shadows, shafts, bloom", (cw_vec2){ 14 * k, 8 * k },
              &(cw_text_style){ .size = 22 * k, .own_backdrop = true });
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "r2d hdr", .width = 1280, .height = 720, .init = init, .fixed_update = fixed_update,
                        .render = render, .debug = true, .user = &app };
}
