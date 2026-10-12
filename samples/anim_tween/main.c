/* anim_tween: handle-based tweens, no animation resources.
 *   Box     a forever sequence: slide right (out-cubic), wait, turn a quarter
 *           (relative) while turning red (parallel), a callback, slide back
 *   Ball    falls with out-bounce, 3 loops from an explicit start, then a
 *           finished callback
 *   Sun     energy pulses in the UPDATE phase (per rendered frame)
 *   Pillar  a tween started at tick 120 and bound to the pillar, which is
 *           freed at tick 200: the tween dies with it
 * Debug fields: positions, callback counts, live tween count, and a hash of
 * the animated values per 60-tick window. */
#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/scene_file.h"

static int32_t box_cb, ball_done, ball_loops, tweens_live, pillar_alive;
static float box[3], ball_y, pillar_y, sun;
static uint32_t hash_win[8], hash = CW_HASH32_INIT;
static cw_node level;
static cw_tween ball_tw;

static void on_box(cw_tween tw, void *u) { (void)tw; (void)u; box_cb++; }
static void on_ball(cw_tween tw, void *u) { (void)tw; (void)u; ball_done++; }

static cw_node N(const char *p) { return cw_node_get(level, p); }

static void init(void *u) {
  (void)u;
  cw_debug_f32_array("box", box, 3);
  cw_debug_f32("ball_y", &ball_y);
  cw_debug_f32("pillar_y", &pillar_y);
  cw_debug_f32("sun", &sun);
  cw_debug_i32("box_cb", &box_cb);
  cw_debug_i32("ball_done", &ball_done);
  cw_debug_i32("ball_loops", &ball_loops);
  cw_debug_i32("tweens", &tweens_live);
  cw_debug_i32("pillar_alive", &pillar_alive);
  cw_debug_field("hash_win", hash_win, CW_DBG_U32, 8);
  level = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level");

  /* #region sequence */
  cw_tween tw = cw_tween_new(NULL);
  cw_tween_bind(tw, N("Box"));
  cw_tween_add_property(tw, N("Box"), "position", cw_val_vec3(-0.5f, 0.5f, 0), 0.8f, CW_EASE_OUT_CUBIC);
  cw_tween_add_interval(tw, 0.2f);
  cw_tween_add_property(tw, N("Box"), "rotation", cw_val_vec3(0, CW_PI / 2, 0), 0.5f, CW_EASE_IN_OUT_SINE);
  cw_tween_relative(tw);
  cw_tween_parallel(tw);
  cw_tween_add_property(tw, N("Box"), "color", cw_val_color(1, 0.0331f, 0.0331f, 1), 0.5f, CW_EASE_LINEAR);
  cw_tween_add_callback(tw, on_box, NULL);
  cw_tween_add_property(tw, N("Box"), "position", cw_val_vec3(-3, 0.5f, 0), 0.6f, CW_EASE_IN_OUT_QUAD);
  cw_tween_parallel(tw);
  cw_tween_add_property(tw, N("Box"), "color", cw_val_color(1, 1, 1, 1), 0.3f, CW_EASE_LINEAR);
  cw_tween_delay(tw, 0.3f);
  cw_tween_set_loops(tw, 0);
  /* #endregion */

  /* #region oneshot */
  ball_tw = cw_tween_property(N("Ball"), "position", cw_val_vec3(0, 0.5f, 0), 1.0f, CW_EASE_OUT_BOUNCE);
  cw_tween_from(ball_tw, cw_val_vec3(0, 3, 0));
  cw_tween_set_loops(ball_tw, 3);
  cw_tween_on_finished(ball_tw, on_ball, NULL);
  /* #endregion */

  cw_tween pulse = cw_tween_new(NULL);
  cw_tween_add_property(pulse, N("Sun"), "energy", cw_val_float(1.88496), 0.75f, CW_EASE_IN_OUT_SINE);
  cw_tween_add_property(pulse, N("Sun"), "energy", cw_val_float(6.28319), 0.75f, CW_EASE_IN_OUT_SINE);
  cw_tween_set_loops(pulse, 0);
  cw_tween_set_process(pulse, CW_TWEEN_UPDATE);
}

static void fold(const void *p, size_t n) { hash = cw_hash32(p, n, hash); }

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  uint64_t tick = cw_tree_tick(cw_scene_app_tree());
  if (tick == 120) cw_tween_property(N("Pillar"), "position", cw_val_vec3(3, 3, 0), 2, CW_EASE_LINEAR);
  if (tick == 200) cw_node_free(N("Pillar"));
  t3_vec3 b = cw_node3d_position(N("Box"));
  box[0] = b.x; box[1] = b.y; box[2] = b.z;
  ball_y = cw_node3d_position(N("Ball")).y;
  cw_node p = N("Pillar");
  pillar_alive = cw_node_valid(p);
  if (pillar_alive) pillar_y = cw_node3d_position(p).y;
  cw_value v;
  cw_node_get_prop(N("Sun"), "energy", &v);
  sun = (float)v.f;
  if (cw_tween_running(ball_tw)) ball_loops = cw_tween_loops_done(ball_tw);
  tweens_live = cw_tween_count(NULL);
  cw_value rot, col;
  cw_node_get_prop(N("Box"), "quaternion", &rot);
  cw_node_get_prop(N("Box"), "color", &col);
  fold(box, sizeof box);
  fold(&ball_y, 4);
  fold(&pillar_y, 4);
  fold(rot.v, 16);
  fold(col.v, 16);
  if (tick / 60 < 8) hash_win[tick / 60] = hash;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "anim_tween", .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
