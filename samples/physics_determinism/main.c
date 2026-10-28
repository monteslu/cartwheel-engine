/* physics_determinism: threads in carts and determinism native vs wasm, as one app that runs on every host.
 *
 * Phase 1, determinism: upstream's OWN determinism scenes (Box3D falling
 * ragdolls, Box2D falling hinges, from deps/<lib>/shared/determinism.c),
 * stepped on cartwheel's job pool with 1..5 workers. Upstream's tests pin the
 * expected sleep step and state hash for 64-bit platforms; the debug fields
 * report what this build got for every worker count, so a run on any host is
 * checked against upstream's constants and against every other host.
 *
 * Phase 2, threading: Box3D's large pyramid (4095 boxes, sleeping off) stepped
 * with 1, 2, 4 and 8 workers; ms per step and a state hash per worker count.
 * Hashes must agree across worker counts; the timings show whether threads
 * actually run in parallel on this host.
 *
 * Phase 3: the same for Box2D's large pyramid (5050 boxes, sleeping off, as
 * deps/box2d/shared/benchmarks.c builds it).
 *
 * Phase 4: a small world (8 boxes on a mesh floor, sized per step as the
 * engine's worlds are: cw_box3d_tasks_step) stepped with 1 worker and with
 * every pool thread. scale_ok: both pyramids step at least 1.25x faster on 8
 * workers than on 1; small_ok: the small world costs about the same on every
 * thread as on one (a pool fan-out would make it many times slower).
 *
 * Work is spread over frames (a few steps per frame) so a host never sees a
 * multi-second wc_render. `done` turns 1 when both phases finish. */
#include <string.h>
#include <time.h>

#include "box2d/box2d.h"
#include "box3d/box3d.h"
#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/physics_tasks.h"
#include "cartwheel/platform.h"

#include "determinism.h" /* Box3D's (shared/), Box2D's is included below by name */

typedef struct FallingHingeData {
  b2BodyId *bodyIds;
  int bodyCount;
  int stepCount;
  int sleepStep;
  uint32_t hash;
} FallingHingeData;
FallingHingeData CreateFallingHinges(b2WorldId worldId);
bool UpdateFallingHinges(b2WorldId worldId, FallingHingeData *data);
void DestroyFallingHinges(FallingHingeData *data);
void CreateLargePyramid(b3WorldId worldId);

#define DET_RUNS 5          /* worker counts 1..5, as upstream's tests */
static const int bench_workers[] = { 1, 2, 4, 8 };
#define BENCH_RUNS 4
#define BENCH_STEPS 60

/* upstream expectations (box3d/test/test_determinism.c; Box2D's are EXPECTED_HASH / EXPECTED_SLEEP_STEP in its
 * shared/determinism.h: the double-precision pair, the engine builds Box2D with large worlds) */
static uint32_t b3_expected_hash = 0x50313037, b2_expected_hash = 0x7FCA93B8;
static int32_t b3_expected_sleep = 269, b2_expected_sleep = 337;

static uint32_t b3_hash[DET_RUNS], b2_hash[DET_RUNS];
static int32_t b3_sleep[DET_RUNS], b2_sleep[DET_RUNS];
static float bench_ms[BENCH_RUNS], b2_bench_ms[BENCH_RUNS], small_ms[2];
static uint32_t small_hash[2];
static int32_t small_ok, scale_ok;
static b3MeshData *small_mesh;
#define SMALL_STEPS 240
static uint32_t bench_hash[BENCH_RUNS], b2_bench_hash[BENCH_RUNS];
static int32_t pool_threads, done, phase, run;

static b3WorldId w3;
static b2WorldId w2;
static void *w2_tasks, *w3_tasks;
static FallingRagdollData rag;
static FallingHingeData hinge;
static int bench_step;
static double bench_t0;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void init(void *user) {
  (void)user;
  pool_threads = cw_jobs_thread_count();
  cw_debug_i32("done", &done);
  cw_debug_i32("pool_threads", &pool_threads);
  cw_debug_u32("b3_expected_hash", &b3_expected_hash);
  cw_debug_i32("b3_expected_sleep", &b3_expected_sleep);
  cw_debug_u32("b2_expected_hash", &b2_expected_hash);
  cw_debug_i32("b2_expected_sleep", &b2_expected_sleep);
  cw_debug_field("b3_hash", b3_hash, CW_DBG_U32, DET_RUNS);
  cw_debug_field("b3_sleep", b3_sleep, CW_DBG_I32, DET_RUNS);
  cw_debug_field("b2_hash", b2_hash, CW_DBG_U32, DET_RUNS);
  cw_debug_field("b2_sleep", b2_sleep, CW_DBG_I32, DET_RUNS);
  cw_debug_f32_array("bench_ms", bench_ms, BENCH_RUNS);
  cw_debug_field("bench_hash", bench_hash, CW_DBG_U32, BENCH_RUNS);
  cw_debug_f32_array("b2_bench_ms", b2_bench_ms, BENCH_RUNS);
  cw_debug_field("b2_bench_hash", b2_bench_hash, CW_DBG_U32, BENCH_RUNS);
  cw_debug_f32_array("small_ms", small_ms, 2);
  cw_debug_field("small_hash", small_hash, CW_DBG_U32, 2);
  cw_debug_i32("small_ok", &small_ok);
  cw_debug_i32("scale_ok", &scale_ok);
}

/* phase 4's world: a mesh floor and 8 boxes falling onto it */
static void create_small_world(b3WorldId w) {
  if (!small_mesh) small_mesh = b3CreateGridMesh(16, 16, 1.0f, 0, true);
  b3BodyDef bd = b3DefaultBodyDef();
  b3ShapeDef sd = b3DefaultShapeDef();
  bd.position = (b3Vec3){ -8, 0, -8 };
  b3CreateMeshShape(b3CreateBody(w, &bd), &sd, small_mesh, b3Vec3_one);
  b3BoxHull box = b3MakeBoxHull(0.5f, 0.5f, 0.5f);
  for (int i = 0; i < 8; i++) {
    bd = b3DefaultBodyDef();
    bd.type = b3_dynamicBody;
    bd.position = (b3Vec3){ (float)(i % 4) * 1.5f - 2, 1.0f + (float)(i / 4) * 1.2f, (float)(i % 3) * 0.7f };
    b3CreateHullShape(b3CreateBody(w, &bd), &sd, &box.base);
  }
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

/* Box2D's large pyramid benchmark (deps/box2d/shared/benchmarks.c CreateLargePyramid, whose name
 * Box3D's benchmarks.c also uses): 100 boxes at the base, 5050 in all, sleeping off. */
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

/* A few steps per frame; returns when the current run is finished. */
static void advance(void) {
  const int per_frame = 8;
  if (phase == 0) { /* Box3D ragdolls */
    if (run == 0 && B3_IS_NULL(w3) && rag.hash == 0) {}
    if (B3_IS_NULL(w3)) {
      b3WorldDef def = b3DefaultWorldDef();
      cw_box3d_tasks(&def, run + 1);
      w3_tasks = def.userTaskContext;
      w3 = b3CreateWorld(&def);
      rag = CreateFallingRagdolls(w3);
    }
    for (int i = 0; i < per_frame; i++) {
      b3World_Step(w3, 1.0f / 60.0f, 4);
      if (UpdateFallingRagdolls(w3, &rag) || rag.stepCount > 1000) {
        b3_hash[run] = rag.hash;
        b3_sleep[run] = rag.sleepStep;
        b3DestroyWorld(w3);
        cw_box3d_tasks_release(w3_tasks);
        DestroyFallingRagdolls(&rag);
        memset(&rag, 0, sizeof rag);
        w3 = b3_nullWorldId;
        if (++run == DET_RUNS) { run = 0; phase = 1; }
        return;
      }
    }
  } else if (phase == 1) { /* Box2D hinges */
    if (B2_IS_NULL(w2)) {
      b2WorldDef def = b2DefaultWorldDef();
      cw_box2d_tasks(&def, run + 1);
      w2_tasks = def.userTaskContext;
      w2 = b2CreateWorld(&def);
      hinge = CreateFallingHinges(w2);
    }
    for (int i = 0; i < per_frame * 4; i++) {
      b2World_Step(w2, 1.0f / 60.0f, 4);
      if (UpdateFallingHinges(w2, &hinge) || hinge.stepCount > 2000) {
        b2_hash[run] = hinge.hash;
        b2_sleep[run] = hinge.sleepStep;
        b2DestroyWorld(w2);
        cw_box2d_tasks_release(w2_tasks);
        DestroyFallingHinges(&hinge);
        memset(&hinge, 0, sizeof hinge);
        w2 = b2_nullWorldId;
        if (++run == DET_RUNS) { run = 0; phase = 2; }
        return;
      }
    }
  } else if (phase == 2) { /* threading: large pyramid */
    if (B3_IS_NULL(w3)) {
      b3WorldDef def = b3DefaultWorldDef();
      cw_box3d_tasks(&def, bench_workers[run]);
      w3_tasks = def.userTaskContext;
      w3 = b3CreateWorld(&def);
      CreateLargePyramid(w3);
      b3World_Step(w3, 1.0f / 60.0f, 4); /* warm caches and pools; not timed */
      bench_step = 0;
      bench_t0 = now_ms();
    }
    for (int i = 0; i < 4 && bench_step < BENCH_STEPS; i++, bench_step++) b3World_Step(w3, 1.0f / 60.0f, 4);
    if (bench_step == BENCH_STEPS) {
      bench_ms[run] = (float)((now_ms() - bench_t0) / BENCH_STEPS);
      bench_hash[run] = hash_bodies3(w3);
      b3DestroyWorld(w3);
      cw_box3d_tasks_release(w3_tasks);
      w3 = b3_nullWorldId;
      if (++run == BENCH_RUNS) { run = 0; phase = 3; }
    }
  } else if (phase == 3) { /* threading: Box2D's large pyramid */
    if (B2_IS_NULL(w2)) {
      b2WorldDef def = b2DefaultWorldDef();
      cw_box2d_tasks(&def, bench_workers[run]);
      w2_tasks = def.userTaskContext;
      w2 = b2CreateWorld(&def);
      create_large_pyramid2(w2);
      b2World_Step(w2, 1.0f / 60.0f, 4); /* warm caches and pools; not timed */
      bench_step = 0;
      bench_t0 = now_ms();
    }
    for (int i = 0; i < 4 && bench_step < BENCH_STEPS; i++, bench_step++) b2World_Step(w2, 1.0f / 60.0f, 4);
    if (bench_step == BENCH_STEPS) {
      b2_bench_ms[run] = (float)((now_ms() - bench_t0) / BENCH_STEPS);
      b2_bench_hash[run] = hash_bodies2(w2);
      b2DestroyWorld(w2);
      cw_box2d_tasks_release(w2_tasks);
      w2 = b2_nullWorldId;
      if (++run == BENCH_RUNS) { run = 0; phase = 4; }
    }
  } else if (phase == 4) { /* a small world: 1 worker, then every pool thread */
    if (B3_IS_NULL(w3)) {
      b3WorldDef def = b3DefaultWorldDef();
      def.enableSleep = false;
      cw_box3d_tasks(&def, run == 0 ? 1 : 0);
      w3_tasks = def.userTaskContext;
      w3 = b3CreateWorld(&def);
      create_small_world(w3);
      bench_step = 0;
      bench_t0 = now_ms();
    }
    for (int i = 0; i < 60 && bench_step < SMALL_STEPS; i++, bench_step++) {
      cw_box3d_tasks_step(w3, w3_tasks);
      b3World_Step(w3, 1.0f / 60.0f, 4);
    }
    if (bench_step == SMALL_STEPS) {
      small_ms[run] = (float)((now_ms() - bench_t0) / SMALL_STEPS);
      small_hash[run] = hash_bodies3(w3);
      b3DestroyWorld(w3);
      cw_box3d_tasks_release(w3_tasks);
      w3 = b3_nullWorldId;
      if (++run == 2) {
        phase = 5;
        scale_ok = bench_ms[3] < 0.8f * bench_ms[0] && b2_bench_ms[3] < 0.8f * b2_bench_ms[0];
        small_ok = small_ms[1] <= 1.5f * small_ms[0] + 0.02f;
        done = 1;
      }
    }
  }
}

static void update(void *user, double dt) {
  (void)user;
  (void)dt;
  if (!done) advance();
}

static void render(void *user, double alpha) {
  (void)user;
  (void)alpha;
  /* green when every hash matched upstream, red otherwise, grey while running */
  bool ok = done;
  for (int i = 0; i < DET_RUNS && ok; i++)
    ok = b3_hash[i] == b3_expected_hash && b3_sleep[i] == b3_expected_sleep && b2_hash[i] == b2_expected_hash &&
         b2_sleep[i] == b2_expected_sleep;
  if (!done) cw_gpu_clear_window(0.3f, 0.3f, 0.3f, 1);
  else if (ok) cw_gpu_clear_window(0.1f, 0.6f, 0.2f, 1);
  else cw_gpu_clear_window(0.7f, 0.1f, 0.1f, 1);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  w3 = b3_nullWorldId;
  w2 = b2_nullWorldId;
  return (cw_app_desc){ .title = "physics determinism", .width = 320, .height = 180, .debug = true, .workers = 8,
                        .init = init, .update = update, .render = render };
}
