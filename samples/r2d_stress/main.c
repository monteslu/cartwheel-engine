/* r2d_stress: the 2D renderer under load, one case at a time, for the perf
 * gate (tools/r2d-perf.mjs) and for looking at.
 *
 * Cases (the `case` table below): plain sprites from 10k to 200k, normal
 * mapped sprites on a lit layer under 1 to 500 lights with and without
 * shadows from 120 occluder boxes, a 4-layer tilemap over the whole screen,
 * additive particles, and a screen of text. Natively CW_STRESS=<name> runs
 * one case and CW_STRESS_SIZE=WxH sets the window (1920x1080 by default);
 * without CW_STRESS the app walks every case for CASE_FRAMES frames each, so
 * a cart (no environment) and the gate see them all.
 *
 * Per case the debug fields say what the renderer did (stress.draws: draw
 * calls, stress.gpu_calls: WebGPU calls render2d made, 0 on GL) and how long
 * it took (stress.cpu_ms: submission plus cw_r2d_end on the CPU,
 * stress.frame_ms: to GPU completion through a cw_gpu_finish, averaged after
 * a warm-up). The picture is a pure function of the tick, so every host
 * draws the same frame. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/jobs.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"

#define CASE_FRAMES 12
#define WARM 4
#define MAXN 200000

typedef enum { K_SPRITES, K_LIT, K_TILES, K_PARTICLES, K_TEXT, K_ROCKS, K_GPUPART, K_INST, K_INST_KEPT, K_SIDE } kind_t;
typedef struct stress_case {
  const char *name;
  kind_t kind;
  int n;       /* sprites, particles, text lines */
  int lights;  /* lit cases */
  bool shadows;
  bool post;        /* HDR canvas + post chain */
  float softness;   /* lit cases */
  float volumetric; /* lit cases */
  bool nobloom;     /* post without bloom */
  bool gi;          /* global illumination of the lit layer */
} stress_case;

static const stress_case CASES[] = {
  { "sprites10k", K_SPRITES, 10000, 0, false },
  { "sprites50k", K_SPRITES, 50000, 0, false },
  { "sprites100k", K_SPRITES, 100000, 0, false },
  { "sprites200k", K_SPRITES, 200000, 0, false },
  { "lit1", K_LIT, 20000, 1, false },
  { "lit16", K_LIT, 20000, 16, false },
  { "lit64", K_LIT, 20000, 64, false },
  { "lit500", K_LIT, 20000, 500, false },
  { "shadow1", K_LIT, 20000, 1, true },
  { "shadow16", K_LIT, 20000, 16, true },
  { "shadow64", K_LIT, 20000, 64, true },
  { "shadow500", K_LIT, 20000, 500, true },
  { "tiles", K_TILES, 0, 0, false },
  { "particles50k", K_PARTICLES, 50000, 0, false },
  { "text", K_TEXT, 400, 0, false },
  /* the lit cases without the lights: what the light pass itself costs */
  { "rocks20k", K_ROCKS, 20000, 0, false },
  /* HDR canvas + the post chain over the 10k sprites (6 bloom levels) */
  { "post", K_SPRITES, 10000, 0, false, true },
  /* soft shadows (softness 0.6) and 16 volumetric lights */
  { "soft64", K_LIT, 20000, 64, true, false, 0.6f },
  { "volumetric16", K_LIT, 20000, 16, true, false, 0.3f, 0.4f },
  /* the chain without bloom: what the HDR canvas and the final pass cost alone */
  { "postnobloom", K_SPRITES, 10000, 0, false, true, 0, 0, true },
  /* global illumination over the lit case with 16 soft-shadowed lights */
  { "gi16", K_LIT, 20000, 16, true, false, 0.5f, 0, false, true },
  /* GPU particles (cw_r2d_particles): 100k and a million, additive, no CPU per particle */
  { "gpupart100k", K_GPUPART, 100000, 0, false },
  { "gpupart1m", K_GPUPART, 1000000, 0, false },
  /* sprite instances (cw_r2d_instances): the sprites200k scene as per-frame instances, and 200k kept (uploaded once) */
  { "inst200k", K_INST, 200000, 0, false },
  { "instkept200k", K_INST_KEPT, 200000, 0, false },
  /* a side-on game screen in HDR: a sky, four full-screen fogged parallax layers, a lit full-screen back wall, ~800
   * normal-mapped tiles, 7 soft-shadowed lights, the post chain and global illumination (the lit HDR floor) */
  { "sidescroll", K_SIDE, 800, 7, true, true, 0.45f, 0.05f, false, true },
};
#define NCASES ((int)CW_ARRAY_LEN(CASES))
#define NOCC 120

typedef struct {
  cw_texture sheet, rock, tiles, wall, hill;
  cw_sprite_instance *inst;
  cw_r2d_instance_buffer kept;
  cw_tilemap map;
  cw_font font;
  cw_sprite *spr;
  int32_t ticks, case_index, single;
  int32_t frames[NCASES];
  double sum_cpu[NCASES], sum_frame[NCASES];
  float cpu_ms[NCASES], frame_ms[NCASES];
  uint32_t draws[NCASES], gpu_calls[NCASES], lights[NCASES];
  int w, h;
  const stress_case *cur;
} app_t;
static app_t app;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static uint32_t hash32(uint32_t h) {
  h *= 2654435761u;
  h ^= h >> 16;
  h *= 0x45d9f3bu;
  h ^= h >> 16;
  return h;
}

/* a 64x64 rock with a normal map from its height field */
static void make_rock(app_t *a) {
  enum { S = 64 };
  static uint8_t col[S * S * 4], nrm[S * S * 4];
  static float hgt[S * S];
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      float dx = (x - 31.5f) / 31.5f, dy = (y - 31.5f) / 31.5f, r = sqrtf(dx * dx + dy * dy);
      float bump = 0.15f * sinf((float)x * 0.7f) * cosf((float)y * 0.55f);
      hgt[y * S + x] = r < 1 ? sqrtf(1 - r * r) + bump : 0;
    }
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      float h = hgt[y * S + x];
      uint8_t *c = col + 4 * (y * S + x), *o = nrm + 4 * (y * S + x);
      int v = (int)(hash32((uint32_t)(y * S + x)) % 30);
      bool in = h > 0;
      c[0] = (uint8_t)(in ? 120 + v : 0), c[1] = (uint8_t)(in ? 110 + v : 0), c[2] = (uint8_t)(in ? 100 + v : 0), c[3] = in ? 255 : 0;
      float gx = hgt[y * S + CW_MIN(x + 1, S - 1)] - hgt[y * S + CW_MAX(x - 1, 0)];
      float gy = hgt[CW_MIN(y + 1, S - 1) * S + x] - hgt[CW_MAX(y - 1, 0) * S + x];
      float nx = -gx * 3, ny = gy * 3, nz = 1, l = sqrtf(nx * nx + ny * ny + nz * nz);
      o[0] = (uint8_t)((nx / l * 0.5f + 0.5f) * 255), o[1] = (uint8_t)((ny / l * 0.5f + 0.5f) * 255);
      o[2] = (uint8_t)((nz / l * 0.5f + 0.5f) * 255), o[3] = 255;
    }
  a->rock = cw_texture_from_pixels(S, S, col, NULL);
  cw_texture_set_normal_map(a->rock, nrm);
}

static void init(void *user) {
  app_t *a = user;
  a->sheet = cw_texture_load("images/sheet.png", &(cw_texture_desc){ .filter = CW_FILTER_NEAREST });
  a->tiles = cw_texture_load("images/tiles.png", &(cw_texture_desc){ .filter = CW_FILTER_NEAREST });
  a->font = cw_font_load("fonts/FiraSans-Regular.ttf", NULL);
  make_rock(a);
  {
    /* the back wall (normal mapped stone) and a parallax silhouette (soft-edged hills, opaque below) */
    enum { S = 128 };
    static uint8_t col[S * S * 4], nrm[S * S * 4], hill[256 * 128 * 4];
    for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++) {
        uint32_t h = hash32((uint32_t)(y * S + x) + 4242u);
        bool joint = (y % 32) < 2 || ((x + ((y / 32) & 1) * 32) % 64) < 2;
        uint8_t *c = col + 4 * (y * S + x), *o = nrm + 4 * (y * S + x);
        int g = joint ? 40 : 70 + (int)(h % 18);
        c[0] = (uint8_t)g, c[1] = (uint8_t)(g - 4), c[2] = (uint8_t)(g + 6), c[3] = 255;
        o[0] = 128, o[1] = 128, o[2] = 255, o[3] = 255;
        if (joint) o[1] = (y % 32) < 1 ? 60 : 196;
      }
    a->wall = cw_texture_from_pixels(S, S, col, &(cw_texture_desc){ .repeat = true });
    cw_texture_set_normal_map(a->wall, nrm);
    for (int y = 0; y < 128; y++)
      for (int x = 0; x < 256; x++) {
        float top = 50 + 22 * sinf((float)x * 0.05f) + 10 * sinf((float)x * 0.13f + 1);
        float d = (float)y - top;
        uint8_t *c = hill + 4 * (y * 256 + x);
        float al = d < 0 ? 0 : d < 1.5f ? d / 1.5f : 1;
        c[0] = c[1] = c[2] = 255, c[3] = (uint8_t)(al * 255);
      }
    a->hill = cw_texture_from_pixels(256, 128, hill, &(cw_texture_desc){ .repeat = true });
  }
  a->spr = cw_calloc(MAXN, sizeof(cw_sprite), CW_MEM_GAME);
  a->inst = cw_calloc(MAXN, sizeof(cw_sprite_instance), CW_MEM_GAME);
  int tw = 0, th = 0;
  cw_texture_size(a->tiles, &tw, &th);
  int mw = 4096 / 16, mh = 2304 / 16;
  a->map = cw_tilemap_create(&(cw_tilemap_desc){ .width = mw, .height = mh, .layers = 4, .tileset = a->tiles, .tile_w = 16, .tile_h = 16 });
  int ntiles = CW_MAX(1, (tw / 16) * (th / 16));
  for (int l = 0; l < 4; l++)
    for (int y = 0; y < mh; y++)
      for (int x = 0; x < mw; x++) {
        uint32_t h = hash32((uint32_t)((l * mh + y) * mw + x));
        if (l == 0 || (h & 3) == 0) cw_tilemap_set(a->map, l, x, y, CW_TILE((int)(h >> 8) % ntiles));
      }
  cw_r2d_layer_set(0, NULL);
  cw_r2d_layer_set(1, &(cw_r2d_layer_desc){ .lit = true });
  /* sidescroll: 2..5 parallax (fogged), 6 lit wall and tiles, 7 effects (additive) */
  for (int l = 2; l <= 5; l++) cw_r2d_layer_set(l, &(cw_r2d_layer_desc){ .fog = cw_rgba(0.35f, 0.42f, 0.55f, 0.42f - 0.1f * (float)(l - 2)) });
  cw_r2d_layer_set(6, &(cw_r2d_layer_desc){ .lit = true, .occluder_depth = 12 });
  cw_debug_i32("ticks", &a->ticks);
  cw_debug_i32("stress.case", &a->case_index);
  cw_debug_field("stress.frames", a->frames, CW_DBG_I32, NCASES);
  cw_debug_f32_array("stress.cpu_ms", a->cpu_ms, NCASES);
  cw_debug_f32_array("stress.frame_ms", a->frame_ms, NCASES);
  cw_debug_field("stress.draws", a->draws, CW_DBG_U32, NCASES);
  cw_debug_field("stress.gpu_calls", a->gpu_calls, CW_DBG_U32, NCASES);
  cw_debug_field("stress.lights", a->lights, CW_DBG_U32, NCASES);
  cw_r2d_debug_register("r2d");
}

static void fixed_update(void *user, double dt) {
  (void)dt;
  ((app_t *)user)->ticks++;
}

/* sprite i at tick t (sprites, lit and particle cases) */
static void animate(int begin, int end, int worker, void *ctx) {
  (void)worker;
  app_t *a = ctx;
  float t = (float)a->ticks / 60.0f, W = (float)a->w, H = (float)a->h;
  kind_t k = a->cur->kind;
  for (int i = begin; i < end; i++) {
    uint32_t h = hash32((uint32_t)i);
    float fx = (float)(h & 0xffff) / 65535.0f, fy = (float)((h >> 16) & 0xffff) / 65535.0f;
    if (k == K_PARTICLES) {
      /* a fountain: each particle on its own parabola, looping */
      float life = fmodf(t * (0.5f + fx) + fy * 4, 2.0f);
      float ang = -1.5708f + (fx - 0.5f) * 1.2f, sp = H * (0.45f + 0.4f * fy);
      float x = W * 0.5f + cosf(ang) * sp * life, y = H * 0.95f + sinf(ang) * sp * life + 0.5f * H * 0.9f * life * life;
      float fade = 1 - life * 0.5f;
      a->spr[i] = (cw_sprite){ .texture = a->sheet, .src = { 96, 0, 32, 32 }, .pos = { x, y }, .pivot = { 0.5f, 0.5f },
                               .scale = { 0.3f, 0.3f }, .color = { 1, 0.5f + 0.5f * fy, 0.2f, 0.6f * fade } };
      continue;
    }
    float sp = 0.2f + 0.8f * fx, ph = fy * 6.2831853f;
    float x = W * 0.5f + (W * 0.45f * fx + 20) * cosf(ph + t * sp), y = H * 0.5f + (H * 0.42f * fy + 10) * sinf(ph * 1.3f + t * sp);
    if (k == K_INST || k == K_INST_KEPT) {
      int fr = (int)((uint32_t)(a->ticks / 6 + i) & 3u);
      float sp2 = k == K_INST_KEPT ? 0 : sp;
      a->inst[i] = (cw_sprite_instance){ .x = W * 0.5f + (W * 0.45f * fx + 20) * cosf(ph + t * sp2), .y = H * 0.5f + (H * 0.42f * fy + 10) * sinf(ph * 1.3f + t * sp2),
                                         .rotation = t * (1 + fy) * (k == K_INST_KEPT ? 0 : 1) + ph, .scale = 0.75f,
                                         .rgba = { (uint8_t)((0.6f + 0.4f * fx) * 255 + 0.5f), (uint8_t)((0.6f + 0.4f * fy) * 255 + 0.5f), 255, 255 },
                                         .frame = (uint16_t)(k == K_INST_KEPT ? (i & 3) : fr), .flags = (uint16_t)((i & 1) ? CW_INSTANCE_FLIP_X : 0) };
      continue;
    }
    if (k == K_LIT || k == K_ROCKS) {
      a->spr[i] = (cw_sprite){ .texture = a->rock, .pos = { x, y }, .pivot = { 0.5f, 0.5f }, .scale = { 0.5f, 0.5f },
                               .rotation = t * (1 + fy) + ph };
    } else {
      int fr = (int)((uint32_t)(a->ticks / 6 + i) & 3u);
      a->spr[i] = (cw_sprite){ .texture = a->sheet, .src = { (float)fr * 32, 0, 32, 32 }, .pos = { x, y }, .pivot = { 0.5f, 0.5f },
                               .scale = { 0.75f, 0.75f }, .rotation = t * (1 + fy) + ph,
                               .color = { 0.6f + 0.4f * fx, 0.6f + 0.4f * fy, 1, 1 }, .flip_x = (i & 1) != 0 };
    }
  }
}

static void draw_case(app_t *a) {
  const stress_case *c = a->cur;
  float t = (float)a->ticks / 60.0f, W = (float)a->w, H = (float)a->h;
  switch (c->kind) {
  case K_SPRITES:
    cw_r2d_sprites(a->spr, c->n);
    break;
  case K_PARTICLES:
    cw_r2d_set_blend(CW_BLEND_ADD);
    cw_r2d_sprites(a->spr, c->n);
    cw_r2d_set_blend(CW_BLEND_ALPHA);
    break;
  case K_LIT: {
    cw_r2d_use_layer(1);
    cw_r2d_rect((cw_rect){ 0, 0, W, H }, cw_hex(0x8a8070ff));
    cw_r2d_sprites(a->spr, c->n);
    for (int i = 0; i < NOCC; i++) {
      uint32_t h = hash32((uint32_t)(i + 77777));
      float x = (float)(h % 1000) / 1000.0f * W, y = (float)((h >> 10) % 1000) / 1000.0f * H, s = 12 + (float)((h >> 20) % 24);
      cw_vec2 p[4] = { { x, y }, { x + s, y }, { x + s, y + s }, { x, y + s } };
      cw_r2d_polygon(p, 4, cw_hex(0x302830ff));
      if (c->shadows) cw_r2d_occluder(p, 4, true);
    }
    for (int i = 0; i < c->lights; i++) {
      uint32_t h = hash32((uint32_t)(i + 999));
      float fx = (float)(h & 0xffff) / 65535.0f, fy = (float)((h >> 16) & 0xffff) / 65535.0f;
      float r = c->lights > 64 ? H * 0.12f : c->lights > 1 ? H * 0.3f : H * 0.8f;
      cw_light2d l = { .pos = { W * fx + 40 * cosf(t + fy * 6), H * fy + 40 * sinf(t * 1.3f + fx * 6) }, .radius = r,
                       .color = cw_rgba(0.5f + 0.5f * fx, 0.5f + 0.5f * fy, 1 - 0.5f * fx, 1), .energy = 1.2f,
                       .shadows = c->shadows, .softness = c->softness, .volumetric = c->volumetric };
      cw_r2d_light(&l);
    }
    cw_r2d_use_layer(0);
    break;
  }
  case K_ROCKS:
    cw_r2d_rect((cw_rect){ 0, 0, W, H }, cw_hex(0x8a8070ff));
    cw_r2d_sprites(a->spr, c->n);
    break;
  case K_GPUPART:
    cw_r2d_set_blend(CW_BLEND_ADD);
    cw_r2d_particles(&(cw_r2d_particles_desc){ .count = c->n, .seed = 7, .time = t, .pos = { W * 0.5f, H * 0.95f }, .extent = { W * 0.3f, 4 },
                                               .angle = -1.5708f, .spread = 0.5f, .speed_min = H * 0.4f, .speed_max = H * 0.9f,
                                               .gravity = { 0, H * 0.5f }, .drag = 0.3f, .life_min = 1.5f, .life_max = 3,
                                               .size_start = 4, .size_end = 1, .color_start = cw_rgba(1, 0.6f, 0.2f, 0.5f), .turbulence = 20 });
    cw_r2d_set_blend(CW_BLEND_ALPHA);
    break;
  case K_INST:
  case K_INST_KEPT: {
    static const cw_rect fr[4] = { { 0, 0, 32, 32 }, { 32, 0, 32, 32 }, { 64, 0, 32, 32 }, { 96, 0, 32, 32 } };
    cw_r2d_instances_desc d = { .texture = a->sheet, .frames = fr, .nframes = 4, .instances = a->inst, .count = c->n };
    if (c->kind == K_INST_KEPT) {
      if (CW_HANDLE_IS_NULL(a->kept)) {
        a->kept = cw_r2d_instance_buffer_create(c->n);
        cw_r2d_instance_buffer_write(a->kept, 0, a->inst, c->n);
      }
      d.buffer = a->kept;
    }
    cw_r2d_instances(&d);
    break;
  }
  case K_SIDE: {
    /* sky, four parallax layers of hills (each fully covering the screen below its ridge) */
    cw_r2d_use_layer(0);
    cw_r2d_rect((cw_rect){ 0, 0, W, H }, cw_rgba(0.35f, 0.45f, 0.65f, 1));
    for (int l = 0; l < 4; l++) {
      cw_r2d_use_layer(2 + l);
      float y0 = H * (0.1f + 0.12f * (float)l), off = fmodf(t * (20 + 30 * (float)l), 512);
      float g = 0.25f - 0.05f * (float)l;
      cw_r2d_sprite(&(cw_sprite){ .texture = a->hill, .src = { off, 0, W * 0.5f, 128 }, .pos = { 0, y0 }, .size = { W, H - y0 }, .color = cw_rgba(g, g + 0.05f, g + 0.1f, 1) });
    }
    /* the lit wall over the whole screen and ~800 tiles (64 px) round its edges and in platforms */
    cw_r2d_use_layer(6);
    float ox = fmodf(t * 40, 128);
    cw_r2d_sprite(&(cw_sprite){ .texture = a->wall, .src = { ox, 0, W, H }, .pos = { 0, 0 }, .size = { W, H } });
    int n = 0;
    for (int ty = 0; ty < (int)(H / 32) + 1 && n < c->n; ty++)
      for (int tx = 0; tx < (int)(W / 32) + 1 && n < c->n; tx++) {
        uint32_t h = hash32((uint32_t)(ty * 97 + tx));
        bool solid = ty < 3 || ty > (int)(H / 32) - 4 || tx < 2 || tx > (int)(W / 32) - 3 || ((ty % 7) == 3 && (h & 3) != 0);
        if (!solid) continue;
        cw_r2d_sprite(&(cw_sprite){ .texture = a->rock, .pos = { (float)tx * 32 - ox * 0.25f, (float)ty * 32 }, .size = { 34, 34 } });
        n++;
      }
    for (int i = 0; i < c->lights; i++) {
      uint32_t h = hash32((uint32_t)(i + 31));
      float fx = (float)(h & 0xffff) / 65535.0f, fy = (float)((h >> 16) & 0xffff) / 65535.0f;
      cw_r2d_light(&(cw_light2d){ .pos = { W * (0.1f + 0.8f * fx), H * (0.2f + 0.6f * fy) + 20 * sinf(t + (float)i) }, .radius = H * 0.35f,
                                  .color = cw_rgba(1, 0.7f + 0.3f * fy, 0.4f + 0.5f * fx, 1), .energy = 1.6f, .shadows = c->shadows,
                                  .softness = c->softness, .volumetric = c->volumetric });
    }
    cw_r2d_set_emissive(1);
    cw_r2d_set_intensity(3);
    for (int i = 0; i < 6; i++) cw_r2d_circle((cw_vec2){ W * (0.15f + 0.14f * (float)i), H * 0.5f }, 6, 0, cw_rgba(1, 0.8f, 0.4f, 1));
    cw_r2d_set_emissive(0);
    cw_r2d_set_intensity(1);
    cw_r2d_use_layer(0);
    break;
  }
  case K_TILES: {
    cw_vec2 o = { -fmodf(t * 37, 512), -fmodf(t * 21, 256) };
    for (int l = 0; l < 4; l++) cw_r2d_tilemap(a->map, l, o, CW_WHITE);
    break;
  }
  case K_TEXT: {
    char buf[96];
    float size = H / 40;
    for (int i = 0; i < c->n; i++) {
      snprintf(buf, sizeof buf, "line %d: the quick brown fox %d", i, (a->ticks + i) % 1000);
      float x = (float)(i % 4) * W / 4 + 8, y = (float)(i / 4) * size * 0.4f;
      cw_r2d_text(a->font, buf, (cw_vec2){ x, fmodf(y + t * 20, H) },
                  &(cw_text_style){ .size = size, .color = cw_rgba(1, 1, 1, 0.9f), .fixed_size = true, .own_backdrop = true });
    }
    break;
  }
  }
}

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  int f = (int)cw_frame_index();
  int ci = a->single >= 0 ? a->single : CW_MIN(f / CASE_FRAMES, NCASES - 1);
  int in_case = a->single >= 0 ? f : f % CASE_FRAMES;
  a->case_index = ci;
  a->cur = &CASES[ci];
  cw_window_size(&a->w, &a->h);
  if (a->cur->kind == K_SPRITES || a->cur->kind == K_LIT || a->cur->kind == K_ROCKS || a->cur->kind == K_PARTICLES ||
      (a->cur->kind == K_INST) || (a->cur->kind == K_INST_KEPT && CW_HANDLE_IS_NULL(a->kept)))
    cw_parallel_for(a->cur->n, 4096, animate, a);
  double t0 = now_ms();
  cw_r2d_post post = { .bloom = !a->cur->nobloom, .bloom_threshold = 0.8f, .bloom_intensity = 0.8f, .vignette = 0.3f };
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x101420ff), .ambient = cw_rgba(0.08f, 0.08f, 0.1f, 1),
                                    .post = a->cur->post ? &post : NULL, .gi = a->cur->gi ? &(cw_r2d_gi){ .intensity = 1 } : NULL });
  if (a->cur->post && a->cur->kind != K_SIDE) cw_r2d_set_intensity(2.5f);
  draw_case(a);
  cw_r2d_end();
  double t1 = now_ms();
  cw_gpu_finish();
  double t2 = now_ms();
  const cw_r2d_stats *s = cw_r2d_get_stats();
  a->draws[ci] = s->draw_calls;
  a->gpu_calls[ci] = s->gpu_calls;
  a->lights[ci] = s->lights;
  if (in_case >= WARM) {
    a->frames[ci]++;
    a->sum_cpu[ci] += t1 - t0;
    a->sum_frame[ci] += t2 - t0;
    a->cpu_ms[ci] = (float)(a->sum_cpu[ci] / a->frames[ci]);
    a->frame_ms[ci] = (float)(a->sum_frame[ci] / a->frames[ci]);
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  int w = 1920, h = 1080;
  app.single = -1;
#ifndef __wasm__
  const char *sz = getenv("CW_STRESS_SIZE");
  if (sz) sscanf(sz, "%dx%d", &w, &h);
  const char *pick = getenv("CW_STRESS");
  if (pick)
    for (int i = 0; i < NCASES; i++)
      if (!strcmp(CASES[i].name, pick)) app.single = i;
#endif
  return (cw_app_desc){ .title = "r2d stress", .width = w, .height = h, .render_scale = 1, .vsync_off = true, .init = init,
                        .fixed_update = fixed_update, .render = render, .debug = true, .user = &app };
}
