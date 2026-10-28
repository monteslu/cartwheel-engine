/* jobs_stall: a cart whose worker thread dies in the middle of a job chunk
 * (tests/jobs-stall.mjs). Every frame it runs a parallel job of 64 small
 * chunks on the job threads. After the debug command `jobs_stall_test <ms>`
 * a chunk that a worker thread (not the page thread) picks up traps, the way
 * a worker does when its code hits a bad access, and the job check's timeout
 * becomes <ms> (0 turns the check off: the control, where the page thread
 * would spin forever). Debug fields: jobs.frames, jobs.sum. */
#include <stdatomic.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/jobs.h"
#include "cartwheel/platform.h"

extern int cw__jobs_stall_ms;
static atomic_int armed;
static int32_t frames, sum;
static atomic_int acc;

static void cmd_stall(int argc, const char **argv, cw_reply *r) {
  if (argc != 2) {
    cw_reply_error(r, "usage: jobs_stall_test <ms>");
    return;
  }
#if CW_CART
  cw__jobs_stall_ms = atoi(argv[1]);
#endif
  atomic_store(&armed, 1);
  cw_reply_raw(r, "{\"ok\":true}");
}

static void chunk(int begin, int end, int worker, void *ctx) {
  (void)ctx;
  if (worker != 0 && atomic_load(&armed)) __builtin_trap();
  volatile int v = 0;
  for (int i = begin; i < end; i++)
    for (int k = 0; k < 20000; k++) v += k ^ i;
  atomic_fetch_add(&acc, end - begin);
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("jobs.frames", &frames);
  cw_debug_i32("jobs.sum", &sum);
  cw_debug_command("jobs_stall_test", "jobs_stall_test <ms>: a worker thread traps in its next chunk; the job check gives up after <ms> (0: never)",
                   cmd_stall);
}

static void render(void *u, double alpha) {
  (void)u, (void)alpha;
  cw_parallel_for(64, 1, chunk, NULL);
  sum = atomic_load(&acc);
  frames++;
  cw_gpu_clear_window(0.1f, 0.2f, 0.3f, 1);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){ .title = "jobs_stall", .width = 320, .height = 180, .debug = true, .workers = 3, .init = init, .render = render };
}
