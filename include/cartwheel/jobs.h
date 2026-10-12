/* cartwheel job system: ONE thread pool for the whole engine.
 *
 * Box2D, Box3D, culling, animation, transforms, particles and asset decoding
 * all run on it; no library spawns threads of its own. Native uses pthreads,
 * carts use wasi-threads (shared memory + wasi.thread-spawn), the same code.
 *
 * Two job shapes:
 *   - range jobs: fn(begin, end, worker, ctx) over [0, count) in chunks of at
 *     least `min_batch` items (Box2D's task shape, and the engine's
 *     parallel-for);
 *   - single jobs: fn(ctx), run once (Box3D's task shape).
 *
 * Waiting never parks the waiting thread while work is queued: it runs other
 * pending chunks until its own job is done (Box3D's finishTask requires a
 * waiter that helps, or a step that blocks on its own sub-jobs deadlocks).
 * The main thread never blocks on a condition variable, which a browser's
 * main thread could not do.
 *
 * Chunk boundaries depend only on (count, min_batch): never on the worker
 * count (which differs per machine and between native and carts) nor on
 * timing, so a job that writes per-chunk results and reduces them in chunk
 * order gives the same answer on every host. */
#ifndef CW_JOBS_H
#define CW_JOBS_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_job cw_job;
typedef void (*cw_range_fn)(int begin, int end, int worker, void *ctx);
typedef void (*cw_single_fn)(void *ctx);

/* workers: threads besides the caller; <0 = pick: the machine's hardware threads - 1 (a cart asks its
 * host, wc_host_info_t.hardware_threads; 3 on a host too old to say). At most 63. CW_WORKERS (native) overrides it.
 * Called by the platform at startup; safe to call again (no-op). */
void cw_jobs_init(int workers);
void cw_jobs_shutdown(void); /* joins the workers; later jobs run inline on the caller. No-op when not running */
/* Threads that run jobs, the calling thread included: what Box2D/Box3D's
 * workerCount should be. */
int cw_jobs_thread_count(void);
/* Index of the calling thread in [0, cw_jobs_thread_count()); 0 = main. */
int cw_jobs_worker_index(void);

#define CW_JOB_CHUNKS 128 /* a range job's target chunk count */
/* Queues fn over [0, count) in chunks of max(min_batch, ceil(count / CW_JOB_CHUNKS)) items (the last may be
 * shorter) and returns at once; every job
 * must be passed to cw_job_wait, which frees it. With no workers it runs inline before returning. */
cw_job *cw_job_range(int count, int min_batch, cw_range_fn fn, void *ctx);
/* Runs fn(ctx) once on some thread; same lifetime rules as cw_job_range. */
cw_job *cw_job_single(cw_single_fn fn, void *ctx);
/* Blocks (helping) until the job is done, then frees it. */
void cw_job_wait(cw_job *job);
/* Range job + wait in one call: the usual parallel-for. */
void cw_parallel_for(int count, int min_batch, cw_range_fn fn, void *ctx);

typedef struct cw_jobs_stats {
  uint64_t jobs, chunks;
  uint64_t chunks_by_thread[64];
} cw_jobs_stats;
void cw_jobs_get_stats(cw_jobs_stats *out); /* cumulative jobs and chunks run, per thread index; never reset */

#ifdef __cplusplus
}
#endif
#endif
