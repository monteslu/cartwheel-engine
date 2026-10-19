/* Solver spin-wait stress (native only; scripts/sanitize.sh runs it under
 * every sanitizer, and it is most useful under TSan).
 *
 * Box2D's and Box3D's constraint solvers run one orchestrator task and
 * workerCount - 1 spinner tasks on the job pool. The spinners poll the
 * stage sync bits until the orchestrator stores the next value. Polling with
 * acquire loads made a TSan build livelock: TSan serves an acquire load of a
 * published atomic under a reader lock on that atomic's sync state and does
 * the release store under the writer lock, which waits while new readers
 * keep coming. The spinners starved the store that would have released
 * them (patches/box2d/0002, patches/box3d/0003 poll relaxed instead).
 *
 * This steps a Box2D and a Box3D world on a pool far larger than the core
 * count, so many spinners poll at once, with a watchdog that fails the run
 * instead of letting it hang. Before the patches a TSan build of this hung
 * within a few seconds. Exit 0 = every step finished. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "cartwheel/jobs.h"
#include "cartwheel/physics_tasks.h"

#define WORKERS 31 /* 32 threads with the caller: oversubscribes any desktop */
#define STEPS2 240
#define STEPS3 120
#define WATCHDOG_S 300 /* TSan runs this in a few seconds; a hang never ends */

static void *watchdog(void *arg) {
  (void)arg;
  sleep(WATCHDOG_S);
  fprintf(stderr, "stress_solver_spin: HANG: physics steps did not finish in %d s\n", WATCHDOG_S);
  fflush(stderr);
  _exit(2);
}

static int step_box2d(void) {
  b2WorldDef def = b2DefaultWorldDef();
  cw_box2d_tasks(&def, 0);
  void *tasks = def.userTaskContext;
  b2WorldId w = b2CreateWorld(&def);

  b2BodyDef gd = b2DefaultBodyDef();
  b2BodyId ground = b2CreateBody(w, &gd);
  b2Polygon floor = b2MakeOffsetBox(60, 1, (b2Vec2){ 0, -1 }, b2MakeRot(0));
  b2ShapeDef sd = b2DefaultShapeDef();
  b2CreatePolygonShape(ground, &sd, &floor);

  /* a pyramid: hundreds of contacts, so every solver stage has many blocks
   * and the multi-block (spinning) path runs */
  b2Polygon box = b2MakeBox(0.5f, 0.5f);
  int rows = 24, bodies = 0;
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < rows - r; c++) {
      b2BodyDef bd = b2DefaultBodyDef();
      bd.type = b2_dynamicBody;
      bd.position = (b2Pos){ (float)c - (rows - r) * 0.5f, 0.5f + r * 1.0f };
      b2CreatePolygonShape(b2CreateBody(w, &bd), &sd, &box);
      bodies++;
    }
  }
  for (int i = 0; i < STEPS2; i++) b2World_Step(w, 1.0f / 60.0f, 4);
  b2Counters cn = b2World_GetCounters(w);
  b2DestroyWorld(w);
  cw_box2d_tasks_release(tasks);
  return bodies == rows * (rows + 1) / 2 && cn.contactCount > 0;
}

static int step_box3d(void) {
  b3WorldDef def = b3DefaultWorldDef();
  cw_box3d_tasks(&def, 0);
  b3WorldId w = b3CreateWorld(&def);

  b3BodyDef gd = b3DefaultBodyDef();
  b3BodyId ground = b3CreateBody(w, &gd);
  b3ShapeDef sd = b3DefaultShapeDef();
  b3BoxHull floor = b3MakeOffsetBoxHull(40, 1, 40, (b3Vec3){ 0, -1, 0 });
  b3CreateHullShape(ground, &sd, &floor.base);

  b3BoxHull cube = b3MakeBoxHull(0.5f, 0.5f, 0.5f);
  int n = 8, bodies = 0;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      for (int z = 0; z < n; z++) {
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position = (b3Vec3){ (float)x * 1.05f - n * 0.5f, 0.5f + y * 1.05f, (float)z * 1.05f - n * 0.5f };
        b3CreateHullShape(b3CreateBody(w, &bd), &sd, &cube.base);
        bodies++;
      }
    }
  }
  for (int i = 0; i < STEPS3; i++) b3World_Step(w, 1.0f / 60.0f, 4);
  b3DestroyWorld(w);
  cw_box3d_tasks_release(def.userTaskContext);
  return bodies == n * n * n;
}

int main(void) {
  pthread_t wd;
  pthread_create(&wd, NULL, watchdog, NULL);
  pthread_detach(wd);
  cw_jobs_init(WORKERS);
  int ok2 = step_box2d();
  int ok3 = step_box3d();
  printf("stress_solver_spin: %d threads, box2d %s, box3d %s\n", cw_jobs_thread_count(), ok2 ? "ok" : "BAD", ok3 ? "ok" : "BAD");
  cw_jobs_shutdown();
  return ok2 && ok3 ? 0 : 1;
}
