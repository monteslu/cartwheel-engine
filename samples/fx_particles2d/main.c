/* fx_particles2d: GPUParticles2D (CPU simulation on the job pool, drawn
 * through render2d): additive fire from a RECT, alpha snow, a one-shot
 * explosive CIRCLE burst re-armed every 2 s, a comet trail from an emitter
 * that orbits (world-space particles left behind it).
 *
 * Debug state: p2.<name>.alive (exact), p2.tick, p2.draw_calls (render2d
 * batches last frame). */
#include <stdio.h>

#include "cartwheel/debug.h"
#include "cartwheel/particles.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/scene_file.h"

static const char *const names[] = { "Fire", "Snow", "Burst", "Orbit/Comet" };
static int32_t alive[4], draws;
static uint32_t tick;

static cw_node node(const char *p) {
  char path[64];
  snprintf(path, sizeof path, "Stage/%s", p);
  return cw_node_get(cw_tree_root(cw_scene_app_tree()), path);
}

static void init(void *u) {
  (void)u;
  static char keys[4][40];
  for (int i = 0; i < 4; i++) {
    snprintf(keys[i], 40, "p2.%s.alive", i == 3 ? "Comet" : names[i]);
    cw_debug_i32(keys[i], &alive[i]);
  }
  cw_debug_u32("p2.tick", &tick);
  cw_debug_i32("p2.draw_calls", &draws);
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  float s, c;
  cw_sincos((float)tick * 0.06f, &s, &c);
  cw_node2d_set_position(node("Orbit/Comet"), cw_v2(120 * c, 70 * s));
  if (tick % 120 == 0) cw_particles_restart(node("Burst"));
  for (int i = 0; i < 4; i++) alive[i] = cw_particles_alive(node(names[i]));
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  draws = (int32_t)cw_r2d_get_stats()->draw_calls;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_particles2d", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
