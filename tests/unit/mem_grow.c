/* mem_grow: the cart's heap grows while every job thread allocates
 * (tests/mem-grow.mjs).
 *
 * V8 (Node, Chromium) keeps a copy of a shared wasm memory's size per thread
 * and brings another thread's copy up to date only when that thread stops for
 * an interrupt check; memory.fill / memory.copy (memset, memcpy) and
 * memory.size use the copy. A thread that waits a moment for malloc's lock
 * while another thread grows the heap, then carves its block out of the new
 * pages, can trap in the memset that follows although the block is in bounds
 * (src/core/mem.c). After the debug command `mem_grow_test <sync> <storms>`
 * each frame runs one storm: a parallel job on 23 worker threads whose chunks
 * compute a while, then allocate a 16 KiB block (cw_calloc, calloc, malloc +
 * memset, realloc in turn) and write all of it, over and over, so the heap
 * grows under light lock contention the whole time. <sync> 0 turns the
 * engine's refresh off: the control, where threads trap.
 *
 * Debug fields: mem.storms (storms finished), mem.blocks, mem.pages (memory
 * size in 64 KiB pages), mem.refreshes (stale views the engine refreshed),
 * mem.bad (blocks an allocation handed a thread that the thread could not
 * see: 0 with the refresh on). */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/jobs.h"
#include "cartwheel/platform.h"

extern int cw__jobs_stall_ms;
#if CW_CART
extern int cw__mem_sync_off;
#endif
long long cw__mem_refresh_count(void);

#define CHUNKS 256
#define PER_CHUNK 24
#define BLOCK (16 * 1024)
#define WORK 5000
#define MAX_STORMS 6 /* 6 * 256 * 24 * 16 KiB = 576 MiB (the cart's memory stops at 1 GiB) */

static int32_t storms, storms_left, blocks, pages, refreshes, bad;
static atomic_int bad_count;
static void *held[MAX_STORMS][CHUNKS][PER_CHUNK];
static int storm_ix;
static volatile unsigned work_sink;

static void seen_check(const void *p, size_t n) {
#if CW_CART
  if ((unsigned long long)(uintptr_t)p + n > (unsigned long long)__builtin_wasm_memory_size(0) * 65536ull)
    atomic_fetch_add_explicit(&bad_count, 1, memory_order_relaxed);
#else
  (void)p, (void)n;
#endif
}

static void chunk(int begin, int end, int worker, void *ctx) {
  (void)worker, (void)ctx;
  for (int c = begin; c < end; c++)
    for (int i = 0; i < PER_CHUNK; i++) {
      unsigned acc = 0;
      for (int k = 0; k < WORK; k++) acc += (unsigned)k * 2654435761u;
      work_sink += acc;
      void *p;
      switch ((c + i) & 3) {
      case 0: p = cw_calloc(1, BLOCK, CW_MEM_GENERAL); break;
      case 1: p = calloc(1, BLOCK); break;
      case 2:
        p = malloc(BLOCK);
        seen_check(p, BLOCK);
        memset(p, 0x5a, BLOCK);
        break;
      default: /* realloc moves a small block into a page-sized one: its copy lands in new memory */
        p = malloc(64);
        memset(p, 0x33, 64);
        p = realloc(p, BLOCK);
        seen_check(p, BLOCK);
        memset((char *)p + 64, 0x33, BLOCK - 64);
        break;
      }
      seen_check(p, BLOCK);
      held[storm_ix][c][i] = p;
    }
}

static void cmd_test(int argc, const char **argv, cw_reply *r) {
  if (argc != 3) {
    cw_reply_error(r, "usage: mem_grow_test <sync 0|1> <storms>");
    return;
  }
#if CW_CART
  cw__mem_sync_off = atoi(argv[1]) ? 0 : 1;
  cw__jobs_stall_ms = 5000; /* a worker that trapped ends the run in seconds */
#endif
  storms_left = atoi(argv[2]);
  if (storms_left > MAX_STORMS) storms_left = MAX_STORMS;
  cw_reply_raw(r, "{\"ok\":true}");
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("mem.storms", &storms);
  cw_debug_i32("mem.blocks", &blocks);
  cw_debug_i32("mem.pages", &pages);
  cw_debug_i32("mem.refreshes", &refreshes);
  cw_debug_i32("mem.bad", &bad);
  cw_debug_command("mem_grow_test", "mem_grow_test <sync 0|1> <storms>: allocate on every job thread while the heap grows",
                   cmd_test);
}

static void render(void *u, double alpha) {
  (void)u, (void)alpha;
  if (storms_left > 0 && storm_ix < MAX_STORMS) {
    cw_parallel_for(CHUNKS, 1, chunk, NULL);
    /* the page thread writes every block the workers allocated */
    for (int c = 0; c < CHUNKS; c++)
      for (int i = 0; i < PER_CHUNK; i++) {
        seen_check(held[storm_ix][c][i], BLOCK);
        memset(held[storm_ix][c][i], 0, BLOCK);
      }
    blocks += CHUNKS * PER_CHUNK;
    storm_ix++;
    storms_left--;
    storms++;
  }
#if CW_CART
  pages = (int32_t)__builtin_wasm_memory_size(0);
#endif
  refreshes = (int32_t)cw__mem_refresh_count();
  bad = atomic_load(&bad_count);
  cw_gpu_clear_window(0.1f, 0.2f, 0.3f, 1);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  return (cw_app_desc){ .title = "mem_grow", .width = 320, .height = 180, .debug = true, .workers = 23, .init = init, .render = render };
}
