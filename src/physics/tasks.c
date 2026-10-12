#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "cartwheel/physics_tasks.h"

/* Box2D: range tasks, enkiTS shaped. A task's workerIndex indexes per-worker
 * scratch inside the world (taskContexts[workerIndex]), so it must be in
 * [0, workerCount) and no two chunks running at once may share it. Pool
 * thread indices satisfy neither (the pool can be larger than the world's
 * workerCount), so each world gets a slot allocator: a chunk claims a free
 * slot for exactly as long as it runs. A chunk never waits on another chunk
 * of a range task, so holding a slot cannot deadlock. (Box2D's solver tasks
 * carry their own index in their context and ignore this one.) */

#define B2_TASK_RING 1024

typedef struct b2_task_wrap {
  b2TaskCallback *task;
  void *ctx;
  struct b2_world_tasks *world;
} b2_task_wrap;

typedef struct b2_world_tasks {
  atomic_int inline_run;    /* this step's tasks run on the calling thread (cw_box2d_tasks_step) */
  atomic_ullong free_slots; /* bit i set = slot i free */
  atomic_uint ring_next;
  b2_task_wrap ring[B2_TASK_RING];
} b2_world_tasks;

static int claim_slot(b2_world_tasks *w) {
  for (;;) {
    unsigned long long m = atomic_load(&w->free_slots);
    while (m) {
      int s = __builtin_ctzll(m);
      if (atomic_compare_exchange_weak(&w->free_slots, &m, m & ~(1ull << s))) return s;
    }
    sched_yield(); /* every slot busy: a chunk finishes in microseconds */
  }
}

static void b2_run(int begin, int end, int thread, void *ctx) {
  (void)thread;
  b2_task_wrap *t = ctx;
  int slot = claim_slot(t->world);
  t->task(begin, end, (uint32_t)slot, t->ctx);
  atomic_fetch_or(&t->world->free_slots, 1ull << slot);
}

static void *b2_enqueue(b2TaskCallback *task, int itemCount, int minRange, void *taskContext, void *userContext) {
  b2_world_tasks *w = userContext;
  if (atomic_load_explicit(&w->inline_run, memory_order_relaxed)) {
    /* run it here; NULL tells Box2D it is done. The solver's worker 0 task comes
     * first and does every stage alone; the others then find the step finished */
    task(0, itemCount, 0, taskContext);
    return NULL;
  }
  /* Box2D finishes every task within the step that enqueued it; the ring is far
   * larger than the tasks one step creates */
  b2_task_wrap *t = &w->ring[atomic_fetch_add(&w->ring_next, 1) % B2_TASK_RING];
  t->task = task;
  t->ctx = taskContext;
  t->world = w;
  return cw_job_range(itemCount, minRange, b2_run, t);
}
static void b2_finish(void *userTask, void *userContext) {
  (void)userContext;
  cw_job_wait(userTask);
}

void cw_box2d_tasks(b2WorldDef *def, int workers) {
  int n = cw_jobs_thread_count();
  int k = workers > 0 && workers < n ? workers : n;
  if (k > 64) k = 64;
  b2_world_tasks *w = calloc(1, sizeof *w);
  atomic_store(&w->free_slots, k == 64 ? ~0ull : (1ull << k) - 1);
  def->workerCount = k;
  def->enqueueTask = b2_enqueue;
  def->finishTask = b2_finish;
  def->userTaskContext = w;
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
