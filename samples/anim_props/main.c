/* anim_props: property-track animation from a .cwscene file. Three players:
 * "show" (LOOP) spins a box, cycles its colour (cubic), dims the sun and
 * steps the lamp colour, with a method track calling the C callback "count"
 * and event keys; "bounce" (PING_PONG) bounces a ball (cubic y, hermite
 * squash); "Blender" cross-fades between "low" and "high" (1 s blend) at
 * ticks 90 and 240. Everything runs in fixed_update; debug fields carry the
 * counters, sampled values and per-window hashes of everything written. */
#include <stdlib.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/scene_file.h"

#define WINDOW 60
static int32_t counts[3], events, loops, changed, roundtrip;
static uint32_t hash_win[8];
static float spin_y, ball_y, fader[3], sun_energy;
static cw_node show, bounce, blender;

static void count_fn(cw_node target, const cw_value *args, int argc, void *user) {
  (void)target;
  (void)user;
  if (argc == 2 && args[1].i >= 1 && args[1].i <= 3) counts[args[1].i - 1]++;
}
static void on_event(cw_node e, const cw_value *a, int n, void *u) { (void)e; (void)a; (void)n; (void)u; events++; }
static void on_loop(cw_node e, const cw_value *a, int n, void *u) { (void)e; (void)a; (void)n; (void)u; loops++; }
static void on_changed(cw_node e, const cw_value *a, int n, void *u) { (void)e; (void)a; (void)n; (void)u; changed++; }

static void init(void *u) {
  (void)u;
  cw_debug_field("count", counts, CW_DBG_I32, 3);
  cw_debug_i32("events", &events);
  cw_debug_i32("loops", &loops);
  cw_debug_i32("changed", &changed);
  cw_debug_i32("roundtrip", &roundtrip);
  cw_debug_f32("spin_y", &spin_y);
  cw_debug_f32("ball_y", &ball_y);
  cw_debug_f32_array("fader", fader, 3);
  cw_debug_f32("sun", &sun_energy);
  cw_debug_field("hash_win", hash_win, CW_DBG_U32, 8);
  cw_anim_register_method("count", count_fn, NULL);
  cw_node level = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level");
  show = cw_node_get(level, "Show");
  bounce = cw_node_get(level, "Bounce");
  blender = cw_node_get(level, "Blender");
  cw_connect(show, "animation_event", (cw_node){ 0 }, NULL, on_event, NULL, 0);
  cw_connect(bounce, "animation_looped", (cw_node){ 0 }, NULL, on_loop, NULL, 0);
  cw_connect(blender, "animation_changed", (cw_node){ 0 }, NULL, on_changed, NULL, 0);
  /* the file is canonical: saving a fresh instance reproduces it */
  size_t len;
  char *text = cw_asset_load("main.cwscene", &len);
  cw_node fresh = cw_scene_instance_path("main.cwscene");
  char *saved = cw_scene_save(fresh);
  cw_node_free(fresh);
  roundtrip = text && saved && !strcmp(text, saved);
  if (!roundtrip && saved) cw_error("main.cwscene does not round-trip; saved:\n%s", saved);
  free(text);
  cw_free(saved);
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  cw_tree *t = cw_scene_app_tree();
  uint64_t tick = cw_tree_tick(t);
  if (tick == 90) cw_anim_player_play(blender, "high", 1.0f);
  if (tick == 240) cw_anim_player_play(blender, "low", 1.0f);
  cw_node level = cw_node_get(cw_tree_root(t), "Level");
  cw_value v;
  cw_node_get_prop(cw_node_get(level, "Spinner"), "rotation", &v);
  spin_y = v.v[1];
  ball_y = cw_node3d_position(cw_node_get(level, "Bouncer")).y;
  t3_vec3 f = cw_node3d_position(cw_node_get(level, "Fader"));
  fader[0] = f.x;
  fader[1] = f.y;
  fader[2] = f.z;
  cw_node_get_prop(cw_node_get(level, "Sun"), "energy", &v);
  sun_energy = (float)v.f;
  uint32_t h = cw_anim_player_hash(show) ^ cw_anim_player_hash(bounce) * 3u ^ cw_anim_player_hash(blender) * 7u;
  int w = (int)(tick / WINDOW);
  if (w < 8) hash_win[w] = h;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "anim_props", .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
