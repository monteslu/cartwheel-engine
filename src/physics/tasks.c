#include <stdatomic.h>
#include <stdlib.h>

#include "cartwheel/physics_tasks.h"

/* Box2D: one callback per task, as Box3D (since v3.2 the library splits its own
 * range work into per-worker tasks and indexes its workers itself). */
typedef struct b2_world_tasks {
  atomic_int inline_run; /* this step's tasks run on the calling thread (cw_box2d_tasks_step) */
} b2_world_tasks;

static void *b2_enqueue(b2TaskCallbackFcn *task, void *taskContext, void *userContext) {
  b2_world_tasks *w = userContext;
  if (w && atomic_load_explicit(&w->inline_run, memory_order_relaxed)) {
    /* run it here; NULL tells Box2D it is done. The solver's worker 0 task comes
     * first and does every stage alone; the others then find the step finished */
    task(taskContext);
    return NULL;
  }
  return cw_job_single((cw_single_fn)task, taskContext);
}
static void b2_finish(void *userTask, void *userContext) {
  (void)userContext;
  cw_job_wait(userTask);
}

void cw_box2d_tasks(b2WorldDef *def, int workers) {
  int n = cw_jobs_thread_count();
  int k = workers > 0 && workers < n ? workers : n;
  if (k > B2_MAX_WORKERS) k = B2_MAX_WORKERS;
  def->workerCount = k;
  def->enqueueTask = b2_enqueue;
  def->finishTask = b2_finish;
  def->userTaskContext = calloc(1, sizeof(b2_world_tasks));
}

void cw_box2d_tasks_release(void *userTaskContext) { free(userTaskContext); }

void cw_box2d_tasks_step(b2WorldId world, void *userTaskContext) {
  b2_world_tasks *w = userTaskContext;
  if (w) atomic_store_explicit(&w->inline_run, b2World_GetAwakeBodyCount(world) < CW_PHYSICS_POOL_MIN_AWAKE, memory_order_relaxed);
}

/* Box3D: one callback per task; Box3D splits the work and indexes its
 * workers itself. */
typedef struct b3_world_tasks {
  atomic_int inline_run; /* this step's tasks run on the calling thread (cw_box3d_tasks_step) */
} b3_world_tasks;

static void *b3_enqueue(b3TaskCallback *task, void *taskContext, void *userContext, const char *taskName) {
  (void)taskName;
  b3_world_tasks *w = userContext;
  if (w && atomic_load_explicit(&w->inline_run, memory_order_relaxed)) {
    /* run it here; NULL tells Box3D it is done. The solver's worker 0 task comes
     * first and orchestrates every stage alone; the others then find the step finished */
    task(taskContext);
    return NULL;
  }
  return cw_job_single((cw_single_fn)task, taskContext);
}
static void b3_finish(void *userTask, void *userContext) {
  (void)userContext;
  cw_job_wait(userTask);
}

void cw_box3d_tasks(b3WorldDef *def, int workers) {
  int n = cw_jobs_thread_count();
  def->workerCount = (uint32_t)(workers > 0 && workers < n ? workers : n);
  def->enqueueTask = b3_enqueue;
  def->finishTask = b3_finish;
  def->userTaskContext = calloc(1, sizeof(b3_world_tasks));
}

void cw_box3d_tasks_release(void *userTaskContext) { free(userTaskContext); }

void cw_box3d_tasks_step(b3WorldId world, void *userTaskContext) {
  b3_world_tasks *w = userTaskContext;
  if (w) atomic_store_explicit(&w->inline_run, b3World_GetAwakeBodyCount(world) < CW_PHYSICS_POOL_MIN_AWAKE, memory_order_relaxed);
}
