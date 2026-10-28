/* r2d_bench: 100,000 animated sprites (moving, rotating, frame-animated,
 * tinted) at 1280x720, with three submission variants INTERLEAVED in one
 * run so a shared, noisy machine affects them alike:
 *   A  cw_r2d_sprites(all)        bulk: vertices filled on the job pool
 *   B  cw_r2d_sprite() x 100k     one call per sprite
 *   C  bulk into a y-sorted layer (the radix sort runs every frame)
 * The variant switches every 10 frames after a warm-up; per variant the
 * sample averages the CPU time of submission and of cw_r2d_end (sort,
 * upload, draws), and the frame time to GPU completion (a 1-pixel readback
 * after the pass). Timings are bench.* debug fields; the frame and the
 * renderer's counters are deterministic and gated. */
#include <math.h>
#include <time.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/jobs.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"

#define N 100000
#define WARMUP 30
#define SLICE 10

typedef struct {
  cw_texture sheet;
  cw_sprite *spr;
  int32_t ticks, variant;
  /* per variant: frames, sums of submit / end / frame ms */
  int32_t frames[3];
  double sum_submit[3], sum_end[3], sum_frame[3], sum_update[3];
  float avg_submit[3], avg_end[3], avg_frame[3], avg_update[3];
  uint32_t draws_per_frame;
} app_t;
static app_t app;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void init(void *user) {
  app_t *a = user;
  a->sheet = cw_texture_load("images/sheet.png", &(cw_texture_desc){ .filter = CW_FILTER_NEAREST });
  a->spr = cw_calloc(N, sizeof(cw_sprite), CW_MEM_GAME);
  cw_r2d_layer_set(1, &(cw_r2d_layer_desc){ .y_sort = true });
  cw_debug_i32("ticks", &a->ticks);
  cw_debug_i32("bench.variant", &a->variant);
  cw_debug_field("bench.frames", a->frames, CW_DBG_I32, 3);
  cw_debug_f32_array("bench.update_ms", a->avg_update, 3);
  cw_debug_f32_array("bench.submit_ms", a->avg_submit, 3);
  cw_debug_f32_array("bench.end_ms", a->avg_end, 3);
  cw_debug_f32_array("bench.frame_ms", a->avg_frame, 3);
  cw_r2d_debug_register("r2d");
}

static void fixed_update(void *user, double dt) {
  (void)dt;
  ((app_t *)user)->ticks++;
}

/* #region animate */
/* sprite i at tick t: a pure function, so every host draws the same frame */
static void animate(int begin, int end, int worker, void *ctx) {
  (void)worker;
  app_t *a = ctx;
  float t = (float)a->ticks / 60.0f;
  for (int i = begin; i < end; i++) {
    uint32_t h = (uint32_t)i * 2654435761u;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    float fx = (float)(h & 0xffff) / 65535.0f, fy = (float)((h >> 16) & 0xffff) / 65535.0f;
    float sp = 0.2f + 0.8f * fx, ph = fy * 6.2831853f;
    float x = 640 + (560 * fx + 40) * cosf(ph + t * sp), y = 360 + (300 * fy + 20) * sinf(ph * 1.3f + t * sp);
    int fr = (int)((uint32_t)(a->ticks / 6 + i) & 3u);
    a->spr[i] = (cw_sprite){ .texture = a->sheet, .src = { (float)fr * 32, 0, 32, 32 }, .pos = { x, y },
                             .pivot = { 0.5f, 0.5f }, .scale = { 0.5f, 0.5f }, .rotation = t * (1 + fy) + ph,
                             .color = { 0.6f + 0.4f * fx, 0.6f + 0.4f * fy, 1, 1 }, .flip_x = (i & 1) != 0 };
  }
}
/* #endregion */

static void render(void *user, double alpha) {
  (void)alpha;
  app_t *a = user;
  int f = (int)cw_frame_index();
  a->variant = f < WARMUP ? 0 : ((f - WARMUP) / SLICE) % 3;
  double t0 = now_ms();
  /* #region dispatch */
  cw_parallel_for(N, 4096, animate, a);
  /* #endregion */
  double t1 = now_ms();
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_hex(0x101420ff) });
  cw_r2d_use_layer(a->variant == 2 ? 1 : 0);
  if (a->variant == 1)
    for (int i = 0; i < N; i++) cw_r2d_sprite(&a->spr[i]);
  else
    cw_r2d_sprites(a->spr, N);
  double t2 = now_ms();
  cw_r2d_end();
  double t3 = now_ms();
  cw_gpu_finish(); /* wait for the GPU */
  double t4 = now_ms();
  a->draws_per_frame = cw_r2d_get_stats()->draw_calls;
  if (f >= WARMUP) {
    int v = a->variant;
    a->frames[v]++;
    a->sum_update[v] += t1 - t0;
    a->sum_submit[v] += t2 - t1;
    a->sum_end[v] += t3 - t2;
    a->sum_frame[v] += t4 - t0;
    a->avg_update[v] = (float)(a->sum_update[v] / a->frames[v]);
    a->avg_submit[v] = (float)(a->sum_submit[v] / a->frames[v]);
    a->avg_end[v] = (float)(a->sum_end[v] / a->frames[v]);
    a->avg_frame[v] = (float)(a->sum_frame[v] / a->frames[v]);
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "r2d bench", .width = 1280, .height = 720, .render_scale = 1, .vsync_off = true, .init = init,
                        .fixed_update = fixed_update, .render = render, .debug = true, .user = &app };
}
