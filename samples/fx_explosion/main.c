/* fx_explosion: the explosion kit. One Explosion3D ("Boom") at the middle of a
 * walled yard fires on the first tick; its children are the parts: Fire and
 * Flames (flipbook cards blended along baked motion vectors), Smoke (sorted,
 * lit by nothing: flipbook, soft where it meets geometry), Sparks and
 * Debris (one BOX mesh per piece), DebrisTrail (a sub-emitter: smoke born at
 * the debris in flight), Haze (heat distortion) and two shockwave rings. The
 * node also borrows a pooled light for the flash, stamps a scorch mark that
 * cools, and records a camera shake the camera node below reads.
 *
 * Environment (the tests' controls and variants):
 *   FXE_NEVER=1     the explosion never fires (the baseline)
 *   FXE_SHORT=1     a short life: the flash, the marks and the shake all end
 *                   inside 4 seconds (the "nothing leaks" test compares with
 *                   FXE_NEVER afterwards)
 *   FXE_NOFLASH=1   no flash light        FXE_NODISTORT=1  no haze or rings
 *   FXE_SHAKE=1     the camera applies the shake

 * With no environment (a cart) the blast fires again every 5 seconds.
 *   FXE_PRESET=low|medium|high|ultra   the quality preset
 *
(clustered lighting: idle pooled flash lights cost nothing; forward lighting pays for every light of the pool in every lit pixel)
 * Debug state: fxe.tick, fxe.lit (flash lights with energy now), fxe.pool
 * (flash lights made), fxe.scorch (scorch marks in use), fxe.shake (x 1000,
 * at the camera), fxe.alive (every particle system's alive count),
 * fxe.playing. */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/explosions.h"
#include "cartwheel/particles.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/scene_file.h"
#include <string.h>

static uint32_t tick;
static int32_t lit, pool, scorch, shake_x1000, alive, playing;
static t3_vec3 cam_home;
static bool apply_shake, no_refire;

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

static void init(void *u) {
  (void)u;
  cw_debug_u32("fxe.tick", &tick);
  cw_debug_i32("fxe.lit", &lit);
  cw_debug_i32("fxe.pool", &pool);
  cw_debug_i32("fxe.scorch", &scorch);
  cw_debug_i32("fxe.shake", &shake_x1000);
  cw_debug_i32("fxe.alive", &alive);
  cw_debug_i32("fxe.playing", &playing);
  const char *pr = getenv("FXE_PRESET");
  if (pr)
    for (int q = 0; q < CW_QUALITY_COUNT; q++)
      if ((pr[0] | 32) == (cw_quality_name(q)[0] | 32)) cw_quality_set((cw_quality)q, true);
  cw_node boom = node("World/Boom");
  if (getenv("FXE_NEVER")) cw_node_set(boom, "autostart", cw_val_bool(false));
  if (getenv("FXE_SHORT")) {
    cw_node_set(boom, "duration", cw_val_float(3));
    cw_node_set(boom, "scorch_time", cw_val_float(1));
    cw_node_set(boom, "scorch_life", cw_val_float(2));
  }
  if (getenv("FXE_NOFLASH")) cw_node_set(boom, "flash_energy", cw_val_float(0));
  if (getenv("FXE_NODISTORT")) {
    cw_node_set(node("World/Boom/Haze"), "distort_strength", cw_val_float(0));
    cw_node_set(node("World/Boom/Wave"), "strength", cw_val_float(0));
    cw_node_set(node("World/Boom/WaveAir"), "strength", cw_val_float(0));
  }
  apply_shake = getenv("FXE_SHAKE") != NULL;
  no_refire = getenv("FXE_NEVER") || getenv("FXE_SHORT");
  cam_home = cw_node3d_global_position(node("World/Camera"));
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  /* a showcase run (no environment: a cart, the livestream) fires the blast again every 5 seconds */
  if (!no_refire && tick % 300 == 0) cw_explosion_fire(node("World/Boom"));
  cw_node cam = node("World/Camera");
  float sh = cw_explosion_shake(cw_scene_app_tree(), cam_home);
  shake_x1000 = (int32_t)(sh * 1000.0f + 0.5f);
  if (apply_shake) {
    t3_vec3 o = cw_shake_offset(sh, tick, 0.18f);
    cw_node3d_set_position(cam, (t3_vec3){ cam_home.x + o.x, cam_home.y + o.y, cam_home.z + o.z });
  }
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  static const char *const parts[] = { "Fire", "Flames", "Smoke", "Sparks", "Debris", "DebrisTrail", "Haze" };
  int n = 0;
  for (int i = 0; i < 7; i++) {
    char p[48];
    snprintf(p, sizeof p, "World/Boom/%s", parts[i]);
    n += cw_particles_alive(node(p));
  }
  alive = n;
  cw_explosion_stats st;
  cw_explosion_get_stats(cw_scene_app_tree(), &st);
  lit = st.lights_lit;
  pool = st.flash_pool;
  scorch = st.scorches_active;
  playing = cw_explosion_playing(node("World/Boom"));
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_explosion", .width = 1280, .height = 720, .render_scale = 1, .debug = true,
                              .init = init, .fixed_update = fixed, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
