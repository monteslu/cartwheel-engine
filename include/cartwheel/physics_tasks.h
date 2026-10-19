/* Box2D / Box3D on the engine job pool.
 *
 * cw_box2d_tasks / cw_box3d_tasks fill a world def's task callbacks so the
 * physics step runs on cartwheel's one thread pool (cartwheel/jobs.h). Pass
 * workers = 0 for every pool thread, or a smaller count to cap it (the
 * library splits its work by this count; results do not depend on it:
 * Box2D v3 and Box3D are deterministic across worker counts). */
#ifndef CW_PHYSICS_TASKS_H
#define CW_PHYSICS_TASKS_H

#include "box2d/box2d.h"
#include "box3d/box3d.h"

#include "jobs.h"

#ifdef __cplusplus
extern "C" {
#endif

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
