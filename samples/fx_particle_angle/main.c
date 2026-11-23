/* fx_particle_angle: the start angle of GPUParticles3D and GPUParticles2D
 * (`angle`, `angle_random`). Upright arrows (a red head over a green shaft):
 * eight camera-facing GPUParticles3D billboards round the middle of the screen
 * while the camera circles them and bobs up and down, and eight GPUParticles2D
 * sprites along the bottom. The scene sets angle_random 0, so every arrow
 * points at the top of the screen however the camera moves.
 *
 * Test switches (cw_test_env, from frame 2): PA_ANGLE (radians, both
 * systems), PA_RANDOM (angle_random, both), PA_SPIN (rad/s, both),
 * PA_MODE (the 3D draw_mode), PA_STILL=1 (the camera stays put).
 *
 * Debug state: pa.tick, pa.p3.alive, pa.p2.alive, pa.yaw, pa.pitch. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/math.h"
#include "cartwheel/particles.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static uint32_t tick;
static int32_t alive3, alive2;
static float yaw, pitch;

static cw_node node(const char *p) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), p); }

static void init(void *u) {
  (void)u;
  cw_debug_u32("pa.tick", &tick);
  cw_debug_i32("pa.p3.alive", &alive3);
  cw_debug_i32("pa.p2.alive", &alive2);
  cw_debug_f32("pa.yaw", &yaw);
  cw_debug_f32("pa.pitch", &pitch);
}

/* a switch, applied when it arrives (frame 2 on every host) or changes */
static void apply(const char *name, const char *prop, int both, const char **last) {
  const char *v = cw_test_env(name);
  if (!v || (*last && !strcmp(*last, v))) return;
  *last = v;
  cw_node p3 = node("World/Arrows"), p2 = node("World/HUD/Arrows2D");
  if (both) {
    cw_node_set(p3, prop, cw_val_float(atof(v)));
    cw_node_set(p2, prop, cw_val_float(atof(v)));
  } else {
    cw_node_set(p3, prop, cw_val_enum(v));
  }
}

static void apply_env(void) {
  static const char *last[4];
  apply("PA_ANGLE", "angle", 1, &last[0]);
  apply("PA_RANDOM", "angle_random", 1, &last[1]);
  apply("PA_SPIN", "spin", 1, &last[2]);
  apply("PA_MODE", "draw_mode", 0, &last[3]);
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  apply_env();
  tick++;
  const char *still = cw_test_env("PA_STILL");
  float t = still && *still == '1' ? 0.0f : (float)tick;
  yaw = t * 0.02f;
  pitch = 0.45f * sinf(t * 0.013f);
  const float dist = 18;
  t3_vec3 eye = { dist * sinf(yaw) * cosf(pitch), dist * sinf(pitch), dist * cosf(yaw) * cosf(pitch) };
  cw_node cam = node("World/Camera");
  cw_node3d_set_position(cam, eye);
  cw_node3d_look_at(cam, (t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0, 1, 0 });
  alive3 = cw_particles_alive(node("World/Arrows"));
  alive2 = cw_particles_alive(node("World/HUD/Arrows2D"));
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_particle_angle", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
