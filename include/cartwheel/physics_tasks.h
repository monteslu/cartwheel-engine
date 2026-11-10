/* Box2D / Box3D on the engine job pool.
 *
 * cw_box2d_tasks / cw_box3d_tasks fill a world def's task callbacks so the
 * physics step runs on cartwheel's one thread pool (cartwheel/jobs.h). Pass
 * workers = 0 for the default (the pool, at most CW_PHYSICS_DEFAULT_MAX_WORKERS
 * threads) or a count of your own (the pool's size at most, so a count of
 * cw_jobs_thread_count() or more uses every pool thread). The library splits
 * its work by this count; results do not depend on it: Box2D v3 and Box3D are
 * deterministic across worker counts.
 *
 * Why 12: a step past about 12 threads stops getting faster and, with the rest
 * of a frame's jobs on the pool, gets slower. A Box3D world of 6000 awake
 * bodies on a 24-thread machine stepped in 24.5 ms on 1 thread, 7.2 on 8, 5.8
 * on 12, 5.4 on 16 and 5.35 on 24, and a step of a blast under load took 6.2 to
 * 7.1 ms on 24 threads against 4.0 to 4.2 on 12. The cap is a fixed count, not
 * the physical cores: a cart sees only the host's hardware threads
 * (wc_host_info_t.hardware_threads counts logical CPUs, SMT siblings included)
 * and has no way to learn the cores behind them, so the engine caps every host
 * the same way. */
#ifndef CW_PHYSICS_TASKS_H
#define CW_PHYSICS_TASKS_H

#include "box2d/box2d.h"
#include "box3d/box3d.h"

#include "jobs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Upper bound of the default worker count (workers = 0); see the top of this header. */
#define CW_PHYSICS_DEFAULT_MAX_WORKERS 12
/* The worker count a world def's workers resolves to: workers > 0 gives min(workers, pool threads); 0 (or less)
 * gives min(pool threads, CW_PHYSICS_DEFAULT_MAX_WORKERS). Box2D further caps it at B2_MAX_WORKERS. */
int cw_physics_workers(int workers);
/* Sets def->workerCount and the task callbacks (one pool job per Box2D task); allocates a small per-world context in
 * def->userTaskContext (free it with cw_box2d_tasks_release, after b2DestroyWorld). Workers capped at B2_MAX_WORKERS
 * (32). */
void cw_box2d_tasks(b2WorldDef *def, int workers);
/* Free what cw_box2d_tasks allocated (def.userTaskContext), after b2DestroyWorld. */
void cw_box2d_tasks_release(void *userTaskContext);
/* Sets def->workerCount and the task callbacks (one pool job per Box3D task); allocates a small per-world context in
 * def->userTaskContext (free it with cw_box3d_tasks_release, after b3DestroyWorld). */
void cw_box3d_tasks(b3WorldDef *def, int workers);
/* Free what cw_box3d_tasks allocated (def.userTaskContext), after b3DestroyWorld. */
void cw_box3d_tasks_release(void *userTaskContext);

/* Fewer awake bodies than this and a step runs every task on the calling thread: a small world's step is a fraction
 * of a millisecond, and fanning it out to the pool (Box3D's per-stage tasks, the solver's spin-synced workers) costs
 * many times the work. The world keeps its workerCount, so the result is the same either way. */
#define CW_PHYSICS_POOL_MIN_AWAKE 128
/* Call before each b2World_Step / b3World_Step: decides, from the world's awake bodies, whether this step's tasks run
 * on the pool or on the calling thread. */
void cw_box2d_tasks_step(b2WorldId world, void *userTaskContext);
/* cw_box2d_tasks_step for a Box3D world (its cw_box3d_tasks context). */
void cw_box3d_tasks_step(b3WorldId world, void *userTaskContext);

#ifdef __cplusplus
}
#endif
#endif
