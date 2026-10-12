/* Fixed-step timing: the simulation ticks at fixed_hz whatever the
 * render rate, so after the same simulated time every host and every frame
 * rate has run the same number of ticks with the same state. Run by the gate
 * at 30, 60 and 144 Hz render rates for 60 simulated seconds. */
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/gl.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"

static int32_t ticks;
static float pos, vel = 3;
static uint32_t rng_hash = CW_HASH32_INIT;
static cw_rng rng;

static void init(void *u) {
  (void)u;
  cw_rng_seed(&rng, 99);
  cw_debug_i32("ticks", &ticks);
  cw_debug_f32("pos", &pos);
  cw_debug_u32("rng_hash", &rng_hash);
}
static void fixed(void *u, double dt) {
  (void)u;
  ticks++;
  vel += (float)dt * (cw_rng_float(&rng) - 0.5f);
  pos += vel * (float)dt + 0.01f * cw_sin((float)ticks * 0.1f);
  uint32_t r = cw_rng_u32(&rng);
  rng_hash = cw_hash32(&r, 4, rng_hash);
}
static void render(void *u, double a) {
  (void)u;
  (void)a;
  glClearColor(0.2f, 0.2f, 0.2f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
}
cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "timing", .width = 64, .height = 64, .debug = true, .fixed_hz = 60, .max_substeps = 8,
                        .init = init, .fixed_update = fixed, .render = render };
}
