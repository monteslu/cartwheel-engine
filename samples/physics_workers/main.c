/* physics_workers: the default worker count of a physics step against every pool thread.
 *
 * A world def's workers = 0 resolves to the job pool, at most
 * CW_PHYSICS_DEFAULT_MAX_WORKERS (12) threads (cartwheel/physics_tasks.h). This
 * app runs on the default pool (the host's hardware threads) and steps Box3D's
 * large pyramid (4095 boxes, sleeping off) and Box2D's (5050 boxes) ROUNDS
 * times each way, alternating: once with workers = 0 (the default) and once
 * with workers = cw_jobs_thread_count() (every pool thread, the default before
 * the cap). Each run times BENCH_STEPS steps after one warm-up step.
 *
 * Fields: pool_threads; b3_workers / b2_workers, what each world got per mode
 * ([0] default, [1] every thread); b3_ms / b2_ms, the median run's ms per step
 * per mode (a busy machine makes single runs noisy); b3_run_ms / b2_run_ms, every run (default, all, default,
 * all, ...); cap_ok: the default is min(pool, 12), every thread is the pool,
 * and both modes give the same bits (Box3D and Box2D are deterministic across
 * worker counts). `done` turns 1 at the end. */
#include <time.h>

#include "box2d/box2d.h"
#include "box3d/box3d.h"
#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/physics_tasks.h"
#include "cartwheel/platform.h"

void CreateLargePyramid(b3WorldId worldId);

#define ROUNDS 8
#define RUNS (2 * ROUNDS)
#define BENCH_STEPS 60

static int32_t pool_threads, done, cap_ok, phase, run;
static int32_t b3_workers[2], b2_workers[2];
static float b3_ms[2], b2_ms[2], b3_run_ms[RUNS], b2_run_ms[RUNS];
static uint32_t b3_hash[RUNS], b2_hash[RUNS];

static b3WorldId w3;
static b2WorldId w2;
static void *tasks;
static int bench_step;
static double bench_t0;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint32_t hash_bodies3(b3WorldId w) {
  b3BodyEvents ev = b3World_GetBodyEvents(w);
  uint32_t h = B3_HASH_INIT;
  for (int i = 0; i < ev.moveCount; i++) h = b3Hash(h, (const uint8_t *)&ev.moveEvents[i].transform, sizeof(b3WorldTransform));
  return h;
}

static uint32_t hash_bodies2(b2WorldId w) {
  b2BodyEvents ev = b2World_GetBodyEvents(w);
  uint32_t h = B2_HASH_INIT;
  for (int i = 0; i < ev.moveCount; i++) h = b2Hash(h, (const uint8_t *)&ev.moveEvents[i].transform, sizeof(b2Transform));
  return h;
}

/* Box2D's large pyramid (deps/box2d/shared/benchmarks.c): 100 boxes at the base, 5050 in all, sleeping off. */
static void create_large_pyramid2(b2WorldId w) {
  b2World_EnableSleeping(w, false);
  const int base_count = 100;
  b2BodyDef bd = b2DefaultBodyDef();
  bd.position = (b2Pos){ 0.0f, -1.0f };
  b2BodyId ground = b2CreateBody(w, &bd);
  b2ShapeDef sd = b2DefaultShapeDef();
  b2Polygon ground_box = b2MakeBox(100.0f, 1.0f);
  b2CreatePolygonShape(ground, &sd, &ground_box);
  bd = b2DefaultBodyDef();
  bd.type = b2_dynamicBody;
  sd = b2DefaultShapeDef();
  sd.density = 1.0f;
  const float a = 0.5f, shift = 1.0f * a;
  b2Polygon box = b2MakeSquare(a);
  for (int i = 0; i < base_count; ++i) {
    float y = (2.0f * i + 1.0f) * shift;
    for (int j = i; j < base_count; ++j) {
      float x = (i + 1.0f) * shift + 2.0f * (j - i) * shift - a * base_count;
      bd.position = (b2Pos){ x, y };
      b2CreatePolygonShape(b2CreateBody(w, &bd), &sd, &box);
    }
  }
}

/* workers for this run: 0 (the default) on even runs, every pool thread on odd ones */
static int run_workers(void) { return run % 2 ? cw_jobs_thread_count() : 0; }

/* the median of one mode's runs (runs m, m + 2, ...) */
static float median(const float *run_ms, int m) {
  float v[ROUNDS];
  for (int i = 0; i < ROUNDS; i++) v[i] = run_ms[m + 2 * i];
  for (int i = 1; i < ROUNDS; i++)
    for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
      float t = v[j];
      v[j] = v[j - 1], v[j - 1] = t;
    }
  return 0.5f * (v[(ROUNDS - 1) / 2] + v[ROUNDS / 2]);
}

static void finish(void) {
  int expect = pool_threads < CW_PHYSICS_DEFAULT_MAX_WORKERS ? pool_threads : CW_PHYSICS_DEFAULT_MAX_WORKERS;
  int b2_all = pool_threads < B2_MAX_WORKERS ? pool_threads : B2_MAX_WORKERS;
  cap_ok = b3_workers[0] == expect && b2_workers[0] == expect && b3_workers[1] == pool_threads && b2_workers[1] == b2_all;
  for (int r = 1; r < RUNS; r++) cap_ok = cap_ok && b3_hash[r] == b3_hash[0] && b2_hash[r] == b2_hash[0];
  for (int m = 0; m < 2; m++) b3_ms[m] = median(b3_run_ms, m), b2_ms[m] = median(b2_run_ms, m);
  done = 1;
}

/* A few steps per frame, so a host never sees a long wc_render. */
static void advance(void) {
  if (phase == 0) { /* Box3D */
    if (B3_IS_NULL(w3)) {
      b3WorldDef def = b3DefaultWorldDef();
      cw_box3d_tasks(&def, run_workers());
      tasks = def.userTaskContext;
      w3 = b3CreateWorld(&def);
      b3_workers[run % 2] = b3World_GetWorkerCount(w3);
      CreateLargePyramid(w3);
      b3World_Step(w3, 1.0f / 60.0f, 4); /* warm caches and pools; not timed */
      bench_step = 0;
      bench_t0 = now_ms();
    }
    for (int i = 0; i < 4 && bench_step < BENCH_STEPS; i++, bench_step++) {
      cw_box3d_tasks_step(w3, tasks);
      b3World_Step(w3, 1.0f / 60.0f, 4);
    }
    if (bench_step == BENCH_STEPS) {
      b3_run_ms[run] = (float)((now_ms() - bench_t0) / BENCH_STEPS);
      b3_hash[run] = hash_bodies3(w3);
      b3DestroyWorld(w3);
      cw_box3d_tasks_release(tasks);
      w3 = b3_nullWorldId;
      if (++run == RUNS) { run = 0; phase = 1; }
    }
  } else if (phase == 1) { /* Box2D */
    if (B2_IS_NULL(w2)) {
      b2WorldDef def = b2DefaultWorldDef();
      cw_box2d_tasks(&def, run_workers());
      tasks = def.userTaskContext;
      w2 = b2CreateWorld(&def);
      b2_workers[run % 2] = b2World_GetWorkerCount(w2);
      create_large_pyramid2(w2);
      b2World_Step(w2, 1.0f / 60.0f, 4);
      bench_step = 0;
      bench_t0 = now_ms();
    }
    for (int i = 0; i < 4 && bench_step < BENCH_STEPS; i++, bench_step++) {
      cw_box2d_tasks_step(w2, tasks);
      b2World_Step(w2, 1.0f / 60.0f, 4);
    }
    if (bench_step == BENCH_STEPS) {
      b2_run_ms[run] = (float)((now_ms() - bench_t0) / BENCH_STEPS);
      b2_hash[run] = hash_bodies2(w2);
      b2DestroyWorld(w2);
      cw_box2d_tasks_release(tasks);
      w2 = b2_nullWorldId;
      if (++run == RUNS) { phase = 2; finish(); }
    }
  }
}

static void init(void *user) {
  (void)user;
  pool_threads = cw_jobs_thread_count();
  cw_debug_i32("done", &done);
  cw_debug_i32("cap_ok", &cap_ok);
  cw_debug_i32("pool_threads", &pool_threads);
  cw_debug_field("b3_workers", b3_workers, CW_DBG_I32, 2);
  cw_debug_field("b2_workers", b2_workers, CW_DBG_I32, 2);
  cw_debug_f32_array("b3_ms", b3_ms, 2);
  cw_debug_f32_array("b2_ms", b2_ms, 2);
  cw_debug_f32_array("b3_run_ms", b3_run_ms, RUNS);
  cw_debug_f32_array("b2_run_ms", b2_run_ms, RUNS);
  cw_debug_field("b3_hash", b3_hash, CW_DBG_U32, RUNS);
  cw_debug_field("b2_hash", b2_hash, CW_DBG_U32, RUNS);
}

static void update(void *user, double dt) {
  (void)user;
  (void)dt;
  if (!done) advance();
}

static void render(void *user, double alpha) {
  (void)user;
  (void)alpha;
  /* grey while running, green when cap_ok, red otherwise */
  if (!done) cw_gpu_clear_window(0.3f, 0.3f, 0.3f, 1);
  else if (cap_ok) cw_gpu_clear_window(0.1f, 0.6f, 0.2f, 1);
  else cw_gpu_clear_window(0.7f, 0.1f, 0.1f, 1);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  w3 = b3_nullWorldId;
  w2 = b2_nullWorldId;
  /* .workers left at 0: the pool is the host's hardware threads, as in a game */
  return (cw_app_desc){ .title = "physics workers", .width = 320, .height = 180, .debug = true,
                        .init = init, .update = update, .render = render };
}
