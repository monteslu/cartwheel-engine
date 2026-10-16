/* profile_demo: a frame with something in every engine phase, to look at in
 * the profiler (cartwheel/profile.h, docs/guides/profiling.md).
 *
 * The scene (main.cwscene, run by the scene runner): 32 RigidBody3D crates
 * dropped in a pile (the physics step and its jobs), an AnimationPlayer
 * spinning a mesh, a shadowed sun and the scene's render passes. The game's
 * own code adds a zone of its own, "game.flock", which spreads work over the
 * job pool (its chunks show on the workers as "game.flock/job"), and a looping
 * tone keeps the audio mixer busy.
 *
 * Capture a trace natively:
 *   CW_FRAMES=200 CW_PROFILE_TRACE=trace.json CW_PROFILE_TRACE_START=60 build/native/samples/profile_demo
 * or from a cart through romdev: node tools/cwtrace.mjs build/cart/carts/profile_demo.wasc -o trace.json
 *
 * Debug fields: demo.tick, demo.crates (the crates' positions folded into a
 * hash every tick: the simulation, which profiling must not change),
 * demo.flock (the job's result), phys.hash.
 * Debug command: zone_bench [n] times n zones with the profiler off and on
 * ({"off_ns":..,"on_ns":..} per zone). */
#include <stdlib.h>

#include "cartwheel/audio.h"
#include "cartwheel/debug.h"
#include "cartwheel/jobs.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/platform.h"
#include "cartwheel/profile.h"
#include "cartwheel/scene_file.h"

#define FLOCK 4096

static struct {
  int32_t tick;
  uint32_t crates, phys;
  float flock;
  float pos[FLOCK][2];
  float part[CW_JOB_CHUNKS * 2];
} G;

/* a stand-in for game AI: a little arithmetic per agent, spread over the job pool */
static void flock_range(int begin, int end, int worker, void *u) {
  (void)worker;
  (void)u;
  float s = 0;
  for (int i = begin; i < end; i++) {
    float x = G.pos[i][0], y = G.pos[i][1];
    for (int k = 0; k < 8; k++) {
      float nx = x * 0.99f - y * 0.01f, ny = y * 0.99f + x * 0.01f;
      x = nx, y = ny;
    }
    G.pos[i][0] = x, G.pos[i][1] = y;
    s += x + y;
  }
  G.part[begin / 32] = s; /* one slot per chunk (min batch 32): reduced in chunk order below */
}

static void zone_bench(int argc, const char **argv, cw_reply *r) {
  int n = argc > 1 ? atoi(argv[1]) : 100000;
  if (n < 1) n = 1;
  bool was = cw_profile_enabled();
  double ns[2];
  for (int on = 0; on < 2; on++) {
    cw_profile_set_enabled(on != 0);
    uint64_t t0 = cw_profile_now_ns();
    for (int i = 0; i < n; i++) {
      CW_PROFILE_SCOPE("bench.zone");
    }
    ns[on] = (double)(cw_profile_now_ns() - t0) / n;
  }
  cw_profile_set_enabled(was);
  cw_reply_printf(r, "{\"zones\":%d,\"off_ns\":%.2f,\"on_ns\":%.2f}", n, ns[0], ns[1]);
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("demo.tick", &G.tick);
  cw_debug_u32("demo.crates", &G.crates);
  cw_debug_f32("demo.flock", &G.flock);
  cw_debug_u32("phys.hash", &G.phys);
  cw_debug_command("zone_bench", "zone_bench [n]: ns per profiler zone, off and on", zone_bench);
  for (int i = 0; i < FLOCK; i++) G.pos[i][0] = (float)(i % 64), G.pos[i][1] = (float)(i / 64);
  G.crates = CW_HASH32_INIT;
  /* a quiet looping tone: the mixer runs every frame (natively on SDL's audio
   * thread, in a cart on the job pool) */
  if (cw_audio_init(NULL)) {
    enum { RATE = 48000 };
    static float pcm[RATE];
    for (int i = 0; i < RATE; i++) pcm[i] = 0.05f * cw_sin(6.2831853f * 220.0f * (float)i / RATE);
    cw_sound s = cw_sound_from_pcm(pcm, RATE, 1, RATE, NULL);
    cw_audio_play(s, &(cw_play_desc){ .loop = true });
  }
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  cw_tree *t = cw_scene_app_tree();
  G.tick = (int32_t)cw_tree_tick(t);
  {
    CW_PROFILE_SCOPE("game.flock");
    cw_parallel_for(FLOCK, 32, flock_range, NULL);
    float s = 0;
    for (int i = 0; i < FLOCK / 32; i++) s += G.part[i];
    G.flock = s;
  }
  cw_node *list;
  int n = cw_tree_group(t, "crates", &list);
  for (int i = 0; i < n; i++) {
    t3_vec3 p = cw_node3d_global_position(list[i]);
    G.crates = cw_hash32(&p, sizeof p, G.crates);
  }
  G.phys = cw_world3d_stats(cw_physics3d_world(t)).hash;
}

static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  cw_audio_update();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "profile demo", .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed,
                              .update = update };
  return cw_scene_app("main.cwscene", &base);
}
