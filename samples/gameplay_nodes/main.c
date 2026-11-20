/* gameplay_nodes: the gameplay node types working together, from one scene file (assets/main.cwscene).
 *
 *   Track (Path3D, a closed circle) carries Cart (PathFollow3D, speed 6 m/s). The cart's Hull is a
 *   KinematicBody3D; behind it a SpringArm3D holds the Camera3D, and slides it in where the Wall is in the way.
 *   Beacon has a VisibleOnScreenNotifier3D (in and out of the camera's view as the cart goes round) and a
 *   looping AudioStreamPlayer3D heard from the camera. Zone (Area3D) holds the Buoy and, once a lap, the Hull:
 *   its list of bodies is read in entry order. Blink (Timer, 0.5 s) rings Chime (AudioStreamPlayer) through a
 *   Trigger, with no code. On the 2D layer a Dot patrols Rail (Path2D + PathFollow2D, turned round at each
 *   end_reached), leaves the screen and comes back (VisibleOnScreenNotifier2D) with an AudioStreamPlayer2D
 *   that pans with it.
 *
 * The C below only counts signals and publishes probes. The mixer runs offline and each tick's audio is folded
 * into audio.hash, so the gate holds every host to the native samples. CW_GAMEPLAY_OFF=1 (the gate's control)
 * stops the cart, the dot and the timer from tick 1. */
#include <string.h>

#include "cartwheel/audio_nodes.h"
#include "cartwheel/curve.h"
#include "cartwheel/debug.h"
#include "cartwheel/gameplay.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/scene_file.h"

#define TICK_FRAMES 800 /* 48000 / 60 */

static struct {
  int32_t tick, laps, seen, seen_in, seen_out, zone_n, zone_max, zone_order_ok, blinks, chimes, dot_turns, dot_in, dot_out, off;
  float cart[3], progress, arm_len, arm_min, dot_x, peak_l, peak_r;
  uint32_t audio_hash;
} P;
static float mix[TICK_FRAMES * 2];

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

/* #region signals */
static void on_blink(cw_node e, const cw_value *a, int argc, void *u) {
  (void)e, (void)a, (void)argc, (void)u;
  P.blinks++;
}
static void on_chime_finished(cw_node e, const cw_value *a, int argc, void *u) {
  (void)e, (void)a, (void)argc, (void)u;
  P.chimes++;
}
static void on_seen(cw_node e, const cw_value *a, int argc, void *u) {
  (void)e, (void)a, (void)argc;
  (*(int32_t *)u)++;
}
/* the dot reached an end of its rail: send it back */
static void on_dot_end(cw_node dot, const cw_value *a, int argc, void *u) {
  (void)a, (void)argc, (void)u;
  cw_value v;
  if (cw_node_get_prop(dot, "speed", &v)) cw_node_set(dot, "speed", cw_val_float(-v.f));
  P.dot_turns++;
}
/* #endregion */

static void fixed(void *u, double dt) {
  (void)u, (void)dt;
  cw_node cart = node("Level/Track/Cart"), dot = node("Level/Rail/Dot"), arm = node("Level/Track/Cart/Hull/Pivot/Arm");
  if (P.tick == 1 && cw_test_env("CW_GAMEPLAY_OFF")) {
    cw_node_set(cart, "speed", cw_val_float(0));
    cw_node_set(dot, "speed", cw_val_float(0));
    cw_timer_stop(node("Level/Blink"));
    P.off = 1;
  }
  P.tick++;
  cw_value v;
  t3_vec3 p = cw_node3d_position(cart);
  P.cart[0] = p.x, P.cart[1] = p.y, P.cart[2] = p.z;
  float before = P.progress;
  if (cw_node_get_prop(cart, "progress", &v)) P.progress = (float)v.f;
  if (P.progress < before) P.laps++;
  P.arm_len = cw_spring_arm_length(arm);
  if (P.tick == 1 || P.arm_len < P.arm_min) P.arm_min = P.arm_len;
  P.seen = cw_notifier_is_on_screen(node("Level/Beacon/Seen"));
  /* #region zone */
  cw_node inside[4];
  P.zone_n = cw_area3d_overlapping_bodies(node("Level/Zone"), inside, 4);
  if (P.zone_n > P.zone_max) P.zone_max = P.zone_n;
  /* the buoy was there first, so it is first in the list whenever the hull is in too */
  if (P.zone_n == 2 && !strcmp(cw_node_name(inside[0]), "Buoy") && !strcmp(cw_node_name(inside[1]), "Hull")) P.zone_order_ok = 1;
  /* #endregion */
  P.dot_x = cw_node2d_position(dot).x;
  /* this tick's audio, offline: the same samples on every host */
  memset(mix, 0, sizeof mix);
  cw_audio_render(mix, TICK_FRAMES);
  cw_audio_analysis an = cw_audio_analyze(mix, TICK_FRAMES * 2);
  P.audio_hash = cw_hash32(&an.hash, sizeof an.hash, P.audio_hash);
  P.peak_l = P.peak_r = 0;
  for (int i = 0; i < TICK_FRAMES; i++) {
    float l = mix[2 * i] < 0 ? -mix[2 * i] : mix[2 * i], r = mix[2 * i + 1] < 0 ? -mix[2 * i + 1] : mix[2 * i + 1];
    if (l > P.peak_l) P.peak_l = l;
    if (r > P.peak_r) P.peak_r = r;
  }
}

static void init(void *u) {
  (void)u;
  cw_audio_init(&(cw_audio_desc){ .sample_rate = 48000, .offline = true });
  P.audio_hash = CW_HASH32_INIT;
  cw_connect(node("Level/Blink"), "timeout", (cw_node){ 0 }, NULL, on_blink, NULL, 0);
  cw_connect(node("Level/Chime"), "finished", (cw_node){ 0 }, NULL, on_chime_finished, NULL, 0);
  cw_connect(node("Level/Beacon/Seen"), "screen_entered", (cw_node){ 0 }, NULL, on_seen, &P.seen_in, 0);
  cw_connect(node("Level/Beacon/Seen"), "screen_exited", (cw_node){ 0 }, NULL, on_seen, &P.seen_out, 0);
  cw_connect(node("Level/Rail/Dot/Seen"), "screen_entered", (cw_node){ 0 }, NULL, on_seen, &P.dot_in, 0);
  cw_connect(node("Level/Rail/Dot/Seen"), "screen_exited", (cw_node){ 0 }, NULL, on_seen, &P.dot_out, 0);
  cw_connect(node("Level/Rail/Dot"), "end_reached", (cw_node){ 0 }, NULL, on_dot_end, NULL, 0);
  cw_debug_i32("tick", &P.tick);
  cw_debug_i32("off", &P.off);
  cw_debug_f32_array("cart.pos", P.cart, 3);
  cw_debug_f32("cart.progress", &P.progress);
  cw_debug_i32("cart.laps", &P.laps);
  cw_debug_f32("arm.len", &P.arm_len);
  cw_debug_f32("arm.min", &P.arm_min);
  cw_debug_i32("beacon.seen", &P.seen);
  cw_debug_i32("beacon.in", &P.seen_in);
  cw_debug_i32("beacon.out", &P.seen_out);
  cw_debug_i32("zone.n", &P.zone_n);
  cw_debug_i32("zone.max", &P.zone_max);
  cw_debug_i32("zone.order_ok", &P.zone_order_ok);
  cw_debug_i32("timer.blinks", &P.blinks);
  cw_debug_i32("chime.finished", &P.chimes);
  cw_debug_f32("dot.x", &P.dot_x);
  cw_debug_i32("dot.turns", &P.dot_turns);
  cw_debug_i32("dot.in", &P.dot_in);
  cw_debug_i32("dot.out", &P.dot_out);
  cw_debug_u32("audio.hash", &P.audio_hash);
  cw_debug_f32("audio.peak_l", &P.peak_l);
  cw_debug_f32("audio.peak_r", &P.peak_r);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "gameplay_nodes", .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
