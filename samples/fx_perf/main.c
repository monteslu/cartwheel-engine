/* fx_perf: 100 000 GPU particles at 1280x720 (MODERN + bloom): 40k fountain
 * particles colliding with the depth buffer, 30k stretched sparks in curl
 * noise, 20k alpha smoke sorted back to front every frame (2^15 key bitonic
 * sort), 10k swarm around an attractor.
 *
 * Debug state: perf.alive / perf.capacity (exact, CPU emission function),
 * perf.draws, perf.sort_passes, and timings (reported, never compared):
 * perf.frame_ms (render end to render end, averaged after 60 warm-up
 * frames), perf.cpu_ms (update start to the end of the frame with a GPU sync),
 * perf.gpu_ms (the pipeline's timed passes), perf.gpu_particles (the particles
 * pass: simulation, sort and draw). */
#include <string.h>
#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/particles.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

#define WARMUP 60
static int32_t alive, capacity, draws, sort_passes, frames;
static float frame_ms, cpu_ms, gpu_ms, gpu_particles, frame_max_ms;
static double t_update, t_last, sum_frame, sum_cpu;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("perf.alive", &alive);
  cw_debug_i32("perf.capacity", &capacity);
  cw_debug_i32("perf.draws", &draws);
  cw_debug_i32("perf.sort_passes", &sort_passes);
  cw_debug_i32("perf.frames", &frames);
  cw_debug_f32("perf.frame_ms", &frame_ms);
  cw_debug_f32("perf.frame_max_ms", &frame_max_ms);
  cw_debug_f32("perf.cpu_ms", &cpu_ms);
  cw_debug_f32("perf.gpu_ms", &gpu_ms);
  cw_debug_f32("perf.gpu_particles", &gpu_particles);
}

static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  t_update = now_ms();
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  cw_gpu_finish(); /* wait for the GPU */
  double t = now_ms();
  cw_particles_stats st;
  cw_particles_get_stats(cw_scene_app_tree(), &st);
  alive = st.alive;
  capacity = st.capacity;
  draws = st.draw_calls;
  sort_passes = st.sort_passes;
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  gpu_ms = cw_pipeline_gpu_ms(p);
  const cw_pass_stat *ps;
  int n = cw_pipeline_pass_stats(p, &ps);
  for (int i = 0; i < n; i++)
    if (!strcmp(ps[i].name, "particles")) gpu_particles = ps[i].gpu_ms_avg;
  static int f;
  if (++f > WARMUP && t_last > 0) {
    frames++;
    double fm = t - t_last;
    sum_frame += fm;
    sum_cpu += t - t_update;
    if (fm > frame_max_ms) frame_max_ms = (float)fm;
    frame_ms = (float)(sum_frame / frames);
    cpu_ms = (float)(sum_cpu / frames);
  }
  t_last = t;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_perf", .width = 1280, .height = 720, .render_scale = 1, .debug = true, .init = init,
                              .update = update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
