/* fx_distort: the distortion buffer and the scene colour copy.
 *
 * A wall of coloured stripes behind:
 *   Haze   GPUParticles3D blend DISTORT: a column of heat haze (bends the
 *          stripes behind it, draws no colour)
 *   Pole   a red pole between the camera and the haze: stays sharp (the
 *          depth rule) and never smears into the haze
 *   Wave   Shockwave3D, a ring looping on the wall
 *   Lens   GPUParticles3D blend REFRACT: three lenses that show the scene
 *          copy bent radially
 * FXD_OFF=1 zeroes every strength: the same frame without the bending (the
 * tests diff the two). FXD_POLE_BACK=1 puts the pole behind the haze: the
 * control that shows the depth rule is what keeps it sharp.
 *
 * Debug state: fxd.tick, fxd.haze_alive, fxd.wave_radius, fxd.wave_active. */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/explosions.h"
#include "cartwheel/particles.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static uint32_t tick;
static int32_t haze_alive, wave_active;
static float wave_radius;

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

static void init(void *u) {
  (void)u;
  cw_debug_u32("fxd.tick", &tick);
  cw_debug_i32("fxd.haze_alive", &haze_alive);
  cw_debug_i32("fxd.wave_active", &wave_active);
  cw_debug_f32("fxd.wave_radius", &wave_radius);
  if (getenv("FXD_POLE_BACK")) /* the control for the depth rule: the pole behind the haze is bent like the wall */
    cw_node_set(node("World/Pole"), "position", cw_val_vec3(-1.43f, 1.4f, -1.2f));
  if (getenv("FXD_OFF")) {
    cw_node_set(node("World/Haze"), "distort_strength", cw_val_float(0));
    cw_node_set(node("World/Lens"), "distort_strength", cw_val_float(0));
    cw_node_set(node("World/Wave"), "strength", cw_val_float(0));
  }
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  haze_alive = cw_particles_alive(node("World/Haze"));
  wave_radius = cw_shockwave_radius(node("World/Wave"));
  wave_active = cw_shockwave_active(node("World/Wave"));
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_distort", .width = 960, .height = 540, .debug = true, .init = init,
                              .fixed_update = fixed, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
