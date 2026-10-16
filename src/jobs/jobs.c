/* The engine's one thread pool (cartwheel/jobs.h).
 *
 * Claiming work takes no lock. A submitted job is published in a table of
 * active jobs; any thread claims a job's next chunk with one fetch_add on that
 * job's own counter, so threads contend only on a job they share. Idle
 * workers sleep on a condition variable and a push wakes one per chunk it
 * offers (never a broadcast for work). A waiter helps: its own job's chunks
 * first, then any published job's, then spins briefly, then sleeps until its
 * job completes or new work arrives. A cart's main thread never sleeps: it is
 * the browser's page thread, which cannot block, so it keeps spinning, with no
 * call out of the module.
 *
 * Job slots are reused, and a thread may hold a stale table entry. Entries
 * carry the job's generation; a claimer announces itself in the job's `refs`
 * and checks the generation, and a refill bumps the generation and waits for
 * `refs` to drain before it writes the job: a claim is either on the job its
 * entry names or none at all.
 *
 * Chunk boundaries depend only on (count, min_batch), as before. */
#include <pthread.h>
#include <sched.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "cartwheel/jobs.h"
#include "cartwheel/profile.h"

#if CW_CART
int cw__cart_hardware_threads(void); /* src/platform/cart.c */
#endif

#define MAX_THREADS 64
#define JOB_POOL 4096
#define ACTIVE_CAP 1024 /* published jobs at once */
#define SPIN_SCANS 256  /* empty scans before a thread sleeps */
#define CACHE_LINE 64

struct cw_job {
  alignas(CACHE_LINE) atomic_int next; /* next chunk index to claim */
  atomic_int done;                     /* chunks finished */
  atomic_uint gen;                     /* bumped at every refill: names this use of the slot */
  atomic_int refs;                     /* threads between reading a table entry and claiming */
  atomic_int in_use;
  atomic_int active; /* index in the table */
  cw_range_fn range;
  cw_single_fn single;
  void *ctx;
  int count, batch, chunks;
  const char *zone; /* the profiler's name for its chunks ("<submitter>/job"), NULL when not profiling */
  atomic_bool background; /* cw_job_background: the main thread never helps with it */
};

static cw_job pool[JOB_POOL];
static atomic_uint pool_cursor;

/* published jobs: (generation << 32) | (pool index + 1); 0 = empty. Only [0, active_hi) is used */
static atomic_ullong active[ACTIVE_CAP];
static atomic_int active_hi;

/* per-thread counters, a cache line each */
typedef struct {
  alignas(CACHE_LINE) atomic_ullong jobs;
  atomic_ullong chunks;
} thread_stats;
static thread_stats stats[MAX_THREADS];

static pthread_t threads[MAX_THREADS];
static int n_workers; /* threads besides the main one */
static atomic_int running;
static _Thread_local int worker_index;

/* sleeping: idle workers on work_cond, waiters on done_cond; `ticket` counts publications */
static pthread_mutex_t sleep_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cond = PTHREAD_COND_INITIALIZER, done_cond = PTHREAD_COND_INITIALIZER;
static atomic_uint ticket;
static atomic_int idle_sleepers, done_sleepers;

int cw_jobs_thread_count(void) { return n_workers + 1; }
int cw_jobs_worker_index(void) { return worker_index; }

static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#endif
}

static cw_job *alloc_job(void) {
  for (;;) {
    for (int tries = 0; tries < JOB_POOL; tries++) {
      cw_job *j = &pool[atomic_fetch_add_explicit(&pool_cursor, 1, memory_order_relaxed) % JOB_POOL];
      int expect = 0;
      if (atomic_load_explicit(&j->in_use, memory_order_relaxed) || !atomic_compare_exchange_strong(&j->in_use, &expect, 1)) continue;
      /* a new generation, then no claimer left that read the old one */
      atomic_fetch_add(&j->gen, 1);
      while (atomic_load(&j->refs)) cpu_relax();
      return j;
    }
    sched_yield(); /* every slot in flight: wait for one to finish */
  }
}

static void wake(int chunks) {
  if (atomic_load(&idle_sleepers) == 0 && atomic_load(&done_sleepers) == 0) return;
  pthread_mutex_lock(&sleep_lock);
  int n = atomic_load(&idle_sleepers);
  if (n > chunks) n = chunks;
  for (int i = 0; i < n; i++) pthread_cond_signal(&work_cond);
  if (atomic_load(&done_sleepers)) pthread_cond_broadcast(&done_cond); /* waiters help too */
  pthread_mutex_unlock(&sleep_lock);
}

static unsigned long long entry_of(cw_job *j, unsigned gen) {
  return ((unsigned long long)gen << 32) | (unsigned long long)(j - pool + 1);
}

static void unpublish(cw_job *j, unsigned gen) {
  unsigned long long e = entry_of(j, gen);
  atomic_compare_exchange_strong(&active[atomic_load_explicit(&j->active, memory_order_relaxed)], &e, 0ull);
}

static void publish(cw_job *j) {
  unsigned long long e = entry_of(j, atomic_load_explicit(&j->gen, memory_order_relaxed));
  for (;;) {
    for (int i = 0; i < ACTIVE_CAP; i++) {
      unsigned long long z = 0;
      if (atomic_load_explicit(&active[i], memory_order_relaxed) || !atomic_compare_exchange_strong(&active[i], &z, e)) continue;
      atomic_store_explicit(&j->active, i, memory_order_relaxed);
      int hi = atomic_load(&active_hi);
      while (hi < i + 1 && !atomic_compare_exchange_weak(&active_hi, &hi, i + 1)) {}
      atomic_fetch_add(&ticket, 1);
      wake(j->chunks);
      return;
    }
    sched_yield(); /* more published jobs than the table holds: wait for the workers */
  }
}

/* The profiler's zone for job chunks. Chunks under one zone name that a
 * thread runs back to back (one job's chunks, or a physics step's tasks)
 * become ONE zone, which saves a clock read per chunk (a cart's clock is a
 * host call) and keeps traces small: the run's start is kept while the
 * thread goes straight on to another chunk under the same name, and the
 * zone is written when it turns to anything else (another name, idle, the
 * end of a wait). */
static _Thread_local struct { const char *zone; uint64_t t0, t1; } run_z;
static void zone_flush(void) {
  if (run_z.zone) cw__profile_push(run_z.zone, run_z.t0, run_z.t1);
  run_z.zone = NULL;
}

static void run_chunk(cw_job *j, int c) {
  const char *zone = j->zone;
  uint64_t t0 = 0;
  if (zone && CW_PROFILE_ON()) {
    if (run_z.zone == zone) {
      t0 = run_z.t0; /* the run goes on */
      run_z.zone = NULL;
    } else {
      zone_flush();
      t0 = cw_profile_now_ns();
    }
  } else {
    zone = NULL;
  }
  if (j->single) {
    j->single(j->ctx);
  } else {
    int b = c * j->batch, e = b + j->batch;
    if (e > j->count) e = j->count;
    j->range(b, e, worker_index, j->ctx);
  }
  if (zone) {
    zone_flush(); /* a run that nested inside this chunk (it waited on a job) */
    run_z.zone = zone, run_z.t0 = t0, run_z.t1 = cw_profile_now_ns();
  }
  atomic_fetch_add_explicit(&stats[worker_index].chunks, 1, memory_order_relaxed);
  if (atomic_fetch_add(&j->done, 1) + 1 == j->chunks && atomic_load(&done_sleepers)) {
    pthread_mutex_lock(&sleep_lock);
    pthread_cond_broadcast(&done_cond);
    pthread_mutex_unlock(&sleep_lock);
  }
}

/* The owner claims and runs one chunk of its job; false when none is left. */
static bool run_own(cw_job *j) {
  int c = atomic_fetch_add_explicit(&j->next, 1, memory_order_acquire);
  if (c >= j->chunks) return false;
  run_chunk(j, c);
  return true;
}

/* Claim and run one chunk of any published job; false when none was claimable. */
static bool run_any(void) {
  int hi = atomic_load_explicit(&active_hi, memory_order_acquire);
  for (int i = 0; i < hi; i++) {
    unsigned long long e = atomic_load_explicit(&active[i], memory_order_acquire);
    if (!e) continue;
    cw_job *j = &pool[(e & 0xffffffffu) - 1];
    unsigned gen = (unsigned)(e >> 32);
    if (worker_index == 0 && atomic_load_explicit(&j->background, memory_order_relaxed)) continue; /* the main thread keeps to frame work */
    atomic_fetch_add(&j->refs, 1);
    if (atomic_load(&j->gen) != gen) { /* the slot was refilled: a stale entry */
      atomic_fetch_sub(&j->refs, 1);
      continue;
    }
    int c = atomic_fetch_add_explicit(&j->next, 1, memory_order_acquire), chunks = j->chunks;
    atomic_fetch_sub(&j->refs, 1);
    if (c >= chunks) {
      unpublish(j, gen); /* every chunk claimed */
      continue;
    }
    run_chunk(j, c); /* the job cannot be refilled before this chunk is done */
    return true;
  }
  return false;
}

static void *worker_main(void *arg) {
  worker_index = (int)(intptr_t)arg;
  int empty = 0;
  while (atomic_load_explicit(&running, memory_order_relaxed)) {
    unsigned seen = atomic_load(&ticket);
    if (run_any()) {
      empty = 0;
      continue;
    }
    if (empty == 0) zone_flush(); /* idle: the run of chunks is over */
    if (++empty < SPIN_SCANS) {
      cpu_relax();
      continue;
    }
    empty = 0;
    pthread_mutex_lock(&sleep_lock);
    atomic_fetch_add(&idle_sleepers, 1);
    if (atomic_load(&ticket) == seen && atomic_load(&running)) pthread_cond_wait(&work_cond, &sleep_lock);
    atomic_fetch_sub(&idle_sleepers, 1);
    pthread_mutex_unlock(&sleep_lock);
  }
  return NULL;
}

void cw_jobs_init(int workers) {
  if (atomic_load(&running)) return;
  if (workers < 0) {
#if CW_CART
    /* the host's hardware threads (wc_host_info_t.hardware_threads); a host
     * that predates the field says 0, and gets the 3 workers carts always had */
    int n = cw__cart_hardware_threads();
    workers = n > 1 ? n - 1 : n == 1 ? 0 : 3;
#else
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    long n = (long)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    workers = n > 1 ? (int)n - 1 : 0;
#endif
  }
  if (workers > MAX_THREADS - 1) workers = MAX_THREADS - 1;
  const char *env = getenv("CW_WORKERS");
  if (env) workers = CW_CLAMP(atoi(env), 0, MAX_THREADS - 1);
  atomic_store(&running, 1);
  worker_index = 0;
  n_workers = 0;
  for (int i = 0; i < workers; i++) {
    if (pthread_create(&threads[i], NULL, worker_main, (void *)(intptr_t)(i + 1)) != 0) {
      cw_warn("jobs: could only start %d of %d workers", i, workers);
      break;
    }
    n_workers++;
  }
}

void cw_jobs_shutdown(void) {
  if (!atomic_load(&running)) return;
  pthread_mutex_lock(&sleep_lock);
  atomic_store(&running, 0);
  pthread_cond_broadcast(&work_cond);
  pthread_mutex_unlock(&sleep_lock);
  for (int i = 0; i < n_workers; i++) pthread_join(threads[i], NULL);
  n_workers = 0;
}

static cw_job *submit(cw_job *j) {
  j->zone = CW_PROFILE_ON() ? cw__profile_job_name() : NULL;
  atomic_fetch_add_explicit(&stats[worker_index].jobs, 1, memory_order_relaxed);
  atomic_store_explicit(&j->done, 0, memory_order_relaxed);
  atomic_store_explicit(&j->next, 0, memory_order_release); /* the fields above go with it to a claimer */
  if (n_workers == 0) { /* no pool: run inline, the job is already done */
    while (run_own(j)) {}
    return j;
  }
  /* even a one-chunk job: Box2D enqueues one per solver worker and they spin
   * on each other, and Box3D waits on its tasks one by one, so they must run side by side */
  publish(j);
  return j;
}

cw_job *cw_job_range(int count, int min_batch, cw_range_fn fn, void *ctx) {
  cw_job *j = alloc_job();
  if (min_batch < 1) min_batch = 1;
  /* about CW_JOB_CHUNKS chunks (enough to balance up to 32 threads), never
   * below min_batch. Not derived from the worker count: the same chunks on
   * every host and machine, so per-chunk results reduce identically */
  int batch = (int)(((int64_t)count + CW_JOB_CHUNKS - 1) / CW_JOB_CHUNKS);
  if (batch < min_batch) batch = min_batch;
  j->range = fn;
  j->single = NULL;
  atomic_store_explicit(&j->background, false, memory_order_relaxed);
  j->ctx = ctx;
  j->count = count;
  j->batch = batch;
  j->chunks = count > 0 ? (count + batch - 1) / batch : 0;
  return submit(j);
}

cw_job *cw_job_single(cw_single_fn fn, void *ctx) {
  cw_job *j = alloc_job();
  j->range = NULL;
  atomic_store_explicit(&j->background, false, memory_order_relaxed);
  j->single = fn;
  j->ctx = ctx;
  j->count = 1;
  j->batch = 1;
  j->chunks = 1;
  return submit(j);
}

void cw_job_wait(cw_job *j) {
  if (!j) return;
  /* help: first with this job's own chunks, then with anything published */
  while (run_own(j)) {}
  int empty = 0;
  while (atomic_load(&j->done) < j->chunks) {
    unsigned seen = atomic_load(&ticket);
    if (run_any()) {
      empty = 0;
      continue;
    }
    if (empty == 0) zone_flush(); /* waiting: the run of chunks is over */
    if (++empty < SPIN_SCANS) {
      cpu_relax();
      continue;
    }
    empty = 0;
#if CW_CART
    if (worker_index == 0) continue; /* the page thread cannot block */
#endif
    pthread_mutex_lock(&sleep_lock);
    atomic_fetch_add(&done_sleepers, 1);
    if (atomic_load(&j->done) < j->chunks && atomic_load(&ticket) == seen) pthread_cond_wait(&done_cond, &sleep_lock);
    atomic_fetch_sub(&done_sleepers, 1);
    pthread_mutex_unlock(&sleep_lock);
  }
  zone_flush();
  if (n_workers) unpublish(j, atomic_load_explicit(&j->gen, memory_order_relaxed));
  atomic_store_explicit(&j->in_use, 0, memory_order_release);
}

void cw_parallel_for(int count, int min_batch, cw_range_fn fn, void *ctx) {
  if (count <= 0) return;
  if (count <= min_batch || count == 1) {
    /* one chunk (cw_job_range's split makes exactly this one): the caller
     * would claim it the moment it waits, so run it without publishing a job
     * or waking anyone */
    atomic_fetch_add_explicit(&stats[worker_index].jobs, 1, memory_order_relaxed);
    fn(0, count, worker_index, ctx);
    atomic_fetch_add_explicit(&stats[worker_index].chunks, 1, memory_order_relaxed);
    return;
  }
  cw_job_wait(cw_job_range(count, min_batch, fn, ctx));
}

void cw_jobs_get_stats(cw_jobs_stats *out) {
  memset(out, 0, sizeof *out);
  for (int i = 0; i < MAX_THREADS; i++) {
    out->jobs += atomic_load_explicit(&stats[i].jobs, memory_order_relaxed);
    out->chunks_by_thread[i] = atomic_load_explicit(&stats[i].chunks, memory_order_relaxed);
    out->chunks += out->chunks_by_thread[i];
  }
}

bool cw_job_done(const cw_job *j) { return !j || atomic_load(&((cw_job *)j)->done) >= j->chunks; }

cw_job *cw_job_background(cw_single_fn fn, void *ctx) {
  cw_job *j = alloc_job();
  j->range = NULL;
  j->single = fn;
  j->ctx = ctx;
  j->count = 1;
  j->batch = 1;
  j->chunks = 1;
  atomic_store_explicit(&j->background, true, memory_order_relaxed);
  return submit(j);
}
